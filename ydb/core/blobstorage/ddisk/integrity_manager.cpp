#include "integrity_manager.h"

#include <util/generic/overloaded.h>

#include <algorithm>
#include <cstring>
#include <util/generic/scope.h>

namespace NKikimr::NDDisk {

TIntegrityManager::TIntegrityManager(IHost& host, ui64 dataChunkSizeBytes, ui64 ddiskId, ui64 pdiskGuid,
        ui64 checksumCacheBytes)
    : Host(host)
    , DataChunkSize(dataChunkSizeBytes)
    , DataBlocksPerChunkCount(dataChunkSizeBytes / IntegrityUnitSize)
    , BlocksPerExtentCount((DataBlocksPerChunkCount + ChecksumsPerIntegrityBlock - 1) / ChecksumsPerIntegrityBlock)
    , ExtentOnDiskSizeBytes(size_t(BlocksPerExtentCount) * IntegrityUnitSize * IntegrityPairSlots)
    , ExtentsPerChunkCount((dataChunkSizeBytes - IntegrityChunkHeaderRegionSize) / ExtentOnDiskSizeBytes)
    , DDiskId(ddiskId)
    , PDiskGuid(pdiskGuid)
    , MaxBlockStates(checksumCacheBytes ? Max<size_t>(1, checksumCacheBytes / BlockStateApproxBytes) : 0)
{
    Y_ABORT_UNLESS(dataChunkSizeBytes % IntegrityUnitSize == 0);
    Y_ABORT_UNLESS(dataChunkSizeBytes > IntegrityChunkHeaderRegionSize);
    Y_ABORT_UNLESS(ExtentsPerChunkCount >= 1);
}

ui32 TIntegrityManager::ChunkHeaderReplicaOffset(ui32 replica) const {
    Y_ABORT_UNLESS(replica < ChunkHeaderReplicaCount);
    // Replicas are spread evenly across the header region so that a single localized corruption
    // cannot take out all of them.
    const ui32 headerRegionBlocks = IntegrityChunkHeaderRegionSize / IntegrityUnitSize;
    return replica * (headerRegionBlocks / ChunkHeaderReplicaCount) * IntegrityUnitSize;
}

ui32 TIntegrityManager::ExtentOffset(ui32 extentSlot) const {
    Y_ABORT_UNLESS(extentSlot < ExtentsPerChunkCount);
    return IntegrityChunkHeaderRegionSize + extentSlot * ExtentOnDiskSizeBytes;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Data chunk lifecycle
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TIntegrityManager::TExtent TIntegrityManager::StartExtent(TDataChunkKey key, TChunkIdx dataChunkIdx) {
    const auto [it, inserted] = Extents.try_emplace(key);
    Y_ABORT_UNLESS(inserted, "data chunk already tracked, TabletId# %" PRIu64 " VChunkIndex# %" PRIu64,
        key.TabletId, key.VChunkIndex);

    TExtentInfo& extent = it->second;
    extent.DataChunkIdx = dataChunkIdx;
    extent.Ref.VChunkGeneration = AllocateGeneration();
    extent.Pairs.resize(BlocksPerExtentCount);
    for (TPairMeta& pair : extent.Pairs) {
        pair.DigestKnown = true;
        pair.BitmapKnown = true;
        pair.Resident = true; // a freshly formatted pair is known to contain no used blocks
        pair.CurrentSlot = EPairSlot::B;
    }

    PendingExtents.push_back(key);
    auto completion = extent.Completion;
    TryAssignExtents();
    EnsureChunkCapacity();
    return TExtent(std::move(completion));
}

void TIntegrityManager::PrepareTabletChunksDeletion(ui64 tabletId) {
    bool found = false;
    for (auto& [key, extent] : Extents) {
        if (key.TabletId != tabletId) {
            continue;
        }
        Y_ABORT_UNLESS(!extent.DeletionPending);
        extent.DeletionPending = true;
        found = true;

        // A pending extent has no durable mapping and no physical slot yet. Stop it from being
        // assigned while the deletion record is in flight; CommitTabletChunksDeletion will erase
        // the extent itself.
        if (extent.State == EExtentState::Pending) {
            std::erase(PendingExtents, key);
        }
    }
    Y_ABORT_UNLESS(found, "tablet deletion has no integrity extents, TabletId# %" PRIu64, tabletId);
}

void TIntegrityManager::CommitTabletChunksDeletion(ui64 tabletId) {
    bool found = false;
    for (auto it = Extents.begin(); it != Extents.end(); ) {
        if (it->first.TabletId == tabletId) {
            Y_ABORT_UNLESS(it->second.DeletionPending);
            found = true;
            FreeExtent(it->first, it->second);
            Extents.erase(it++);
        } else {
            ++it;
        }
    }
    Y_ABORT_UNLESS(found, "tablet deletion was not prepared, TabletId# %" PRIu64, tabletId);
    DrainPairWork();
}

void TIntegrityManager::FreeExtent(TDataChunkKey key, TExtentInfo& extent) {
    for (const auto& pair : extent.Pairs) {
        Y_ABORT_UNLESS(!pair.OperationPins);
    }
    for (const auto& [_, runtime] : extent.PairRuntime) {
        Y_ABORT_UNLESS(!runtime->Loading && !runtime->Flushing);
    }
    extent.Completion->Failed = true;
    DropBlockStates(extent);
    if (extent.State == EExtentState::Pending) {
        std::erase(PendingExtents, key);
    } else if (!extent.Formatting) {
        ReleaseSlot(extent.Ref.IntegrityChunkIdx, extent.Ref.ExtentSlot);
    }
    // A submitted format retains the original slot until its result, including after key reuse.
    PairWork.emplace_back(extent.Completion);
}

void TIntegrityManager::ReleaseSlot(TChunkIdx chunkIdx, ui32 extentSlot) {
    const auto chunkIt = IntegrityChunks.find(chunkIdx);
    Y_ABORT_UNLESS(chunkIt != IntegrityChunks.end());
    chunkIt->second.FreeSlots.push_back(extentSlot);
    std::sort(chunkIt->second.FreeSlots.begin(), chunkIt->second.FreeSlots.end(), std::greater<ui32>());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Integrity chunk allocation and formatting
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void TIntegrityManager::EnsureChunkCapacity() {
    if (Stopped) {
        return;
    }
    size_t supply = PendingChunkAllocations.size() * ExtentsPerChunkCount;
    for (const auto& [chunkIdx, chunk] : IntegrityChunks) {
        supply += chunk.FreeSlots.size();
    }
    while (supply < PendingExtents.size()) {
        const ui64 token = NextAllocationToken++;
        PendingChunkAllocations.insert(token);
        supply += ExtentsPerChunkCount;
        PairWork.emplace_back(TAllocationSubmission{token});
    }
    DrainPairWork();
}

void TIntegrityManager::OnIntegrityChunkAllocated(TChunkIdx chunkIdx) {
    const auto [it, inserted] = IntegrityChunks.try_emplace(chunkIdx);
    Y_ABORT_UNLESS(inserted, "integrity chunk already in use, ChunkIdx# %" PRIu32, chunkIdx);

    TIntegrityChunkInfo& chunk = it->second;
    chunk.Generation = AllocateGeneration();
    chunk.FreeSlots.reserve(ExtentsPerChunkCount);
    for (ui32 slot = ExtentsPerChunkCount; slot > 0; --slot) {
        chunk.FreeSlots.push_back(slot - 1);
    }

    chunk.HeaderWritesRemaining = ChunkHeaderReplicaCount;
    // Publish all descriptors before the first host submission can complete inline.
    for (ui32 replica = 0; replica < ChunkHeaderReplicaCount; ++replica) {
        const ui64 id = NextPairWriteId++;
        FormatWrites.emplace(id, TFormatWrite{EWriteIoKind::ChunkHeader, chunkIdx,
            chunk.Generation, {}, {}, chunk.Completion});
        PairWork.emplace_back(TWriteSubmission{id, chunkIdx, ChunkHeaderReplicaOffset(replica),
            MakeHeaderImage(chunkIdx, chunk.Generation), EWriteIoKind::ChunkHeader});
    }
    TryAssignExtents();
    DrainPairWork();
}

bool TIntegrityManager::CancelChunkAllocationIfExcess() {
    size_t supply = PendingChunkAllocations.size() * ExtentsPerChunkCount;
    for (const auto& [chunkIdx, chunk] : IntegrityChunks) {
        supply += chunk.FreeSlots.size();
    }
    if (supply < PendingExtents.size()) {
        return false;
    }
    return true;
}

TRcBuf TIntegrityManager::MakeHeaderImage(TChunkIdx chunkIdx, ui64 generation) const {
    auto data = TRcBuf::UninitializedPageAligned(sizeof(TIntegrityChunkHeader));
    auto* header = reinterpret_cast<TIntegrityChunkHeader*>(data.GetDataMut());
    memset(header, 0, sizeof(*header));
    header->Magic = MagicIntegrityChunkHeader;
    header->FormatVersion = static_cast<ui32>(EIntegrityFormatVersion::BaseAwupf4KiB);
    header->HeaderSize = sizeof(TIntegrityChunkHeader);
    header->DDiskId = DDiskId;
    header->PDiskGuid = PDiskGuid;
    header->IntegrityChunkId = chunkIdx;
    header->IntegrityChunkGeneration = generation;
    header->HeaderChecksum = CalculateRawChecksum(header, sizeof(*header));
    return data;
}

std::vector<TChunkIdx> TIntegrityManager::TakeReleasableIntegrityChunks() {
    // Freed slots first satisfy pending extents (queueing their format writes); a chunk that is
    // still fully free afterwards has no demand left for its slots.
    TryAssignExtents();

    std::vector<TChunkIdx> released;
    for (auto it = IntegrityChunks.begin(); it != IntegrityChunks.end(); ) {
        const TIntegrityChunkInfo& chunk = it->second;
        // Formatting chunks have a header write in flight: skip them, they become releasable
        // once headers settle. A slot withheld by an orphaned format write keeps FreeSlots
        // below capacity, so a fully free chunk has no extent I/O in flight either.
        if (chunk.State == EChunkState::Ready && chunk.FreeSlots.size() == ExtentsPerChunkCount) {
            released.push_back(it->first);
            IntegrityChunks.erase(it++);
        } else {
            ++it;
        }
    }
    return released;
}

void TIntegrityManager::TryAssignExtents() {
    while (!Stopped && !PendingExtents.empty()) {
        // Find a chunk with a free slot (Formatting or Ready; smallest chunk index first for
        // determinism). Extents may be formatted in parallel with the chunk's own headers.
        TChunkIdx chunkIdx = 0;
        TIntegrityChunkInfo* chunk = nullptr;
        for (auto& [idx, info] : IntegrityChunks) {
            if (!info.FreeSlots.empty() && (!chunk || idx < chunkIdx)) {
                chunkIdx = idx;
                chunk = &info;
            }
        }
        if (!chunk) {
            break; // waiting for a chunk allocation; still drain previously registered work
        }

        const TDataChunkKey key = PendingExtents.front();
        PendingExtents.pop_front();

        const auto it = Extents.find(key);
        Y_ABORT_UNLESS(it != Extents.end() && it->second.State == EExtentState::Pending);
        TExtentInfo& extent = it->second;

        extent.Ref.IntegrityChunkIdx = chunkIdx;
        extent.Ref.ExtentSlot = chunk->FreeSlots.back();
        chunk->FreeSlots.pop_back();
        extent.State = EExtentState::Formatting;

        extent.Formatting = true;
        const auto ref = extent.Ref;
        auto completion = extent.Completion;
        const ui64 id = NextPairWriteId++;
        FormatWrites.emplace(id, TFormatWrite{EWriteIoKind::ExtentFormat, chunkIdx,
            chunk->Generation, key, ref, completion});
        PairWork.emplace_back(TWriteSubmission{id, chunkIdx, ExtentOffset(ref.ExtentSlot),
            MakeExtentImage(key, ref, chunk->Generation), EWriteIoKind::ExtentFormat});
        extent.Completion->Placed = true;
        PairWork.emplace_back(completion);
    }
    DrainPairWork();
}

TRcBuf TIntegrityManager::MakeExtentImage(TDataChunkKey key, TExtentRef ref, ui64 generation) const {
    auto data = TRcBuf::UninitializedPageAligned(ExtentOnDiskSizeBytes);
    auto* blocks = reinterpret_cast<TIntegrityBlock*>(data.GetDataMut());
    memset(blocks, 0, ExtentOnDiskSizeBytes);

    for (ui32 pair = 0; pair < BlocksPerExtentCount; ++pair) {
        for (ui32 slot = 0; slot < IntegrityPairSlots; ++slot) {
            TIntegrityBlock& block = blocks[pair * IntegrityPairSlots + slot];
            TIntegrityBlockHeader& header = block.Header;
            header.Magic = MagicIntegrityBlock;
            header.FormatVersion = static_cast<ui16>(EIntegrityFormatVersion::BaseAwupf4KiB);
            header.ChecksumBlockIdx = pair;
            header.OwnerId = key.TabletId;
            header.VChunkId = key.VChunkIndex;
            header.VChunkGeneration = ref.VChunkGeneration;
            header.IntegrityChunkId = ref.IntegrityChunkIdx;
            header.IntegrityExtentId = ref.ExtentSlot;
            header.IntegrityChunkGeneration = generation;
            // Slot A gets sequence 0, slot B gets 1, so B starts as the current slot of each pair.
            header.PairSequenceNumber = slot;
            header.BlockChecksum = CalculateRawChecksum(&block, sizeof(block));
        }
    }


    return data;
}

void TIntegrityManager::MaybeCompleteExtent(TDataChunkKey key, TExtentRef ref,
        const std::shared_ptr<TExtentState>& completion)
{
    const auto it = Extents.find(key);
    if (it == Extents.end() || it->second.Completion != completion
            || it->second.Ref.VChunkGeneration != ref.VChunkGeneration
            || it->second.Formatting || !it->second.Completion->Placed) {
        return;
    }
    const auto chunkIt = IntegrityChunks.find(ref.IntegrityChunkIdx);
    if (chunkIt == IntegrityChunks.end() || chunkIt->second.HeaderWritesRemaining) {
        return;
    }
    auto& extent = it->second;
    if (extent.State == EExtentState::Ready || completion->Failed) {
        return;
    }
    if (!Stopped && extent.FormatComplete && chunkIt->second.Completion->Ready
            && !extent.DeletionPending) {
        extent.State = EExtentState::Ready;
        completion->Ready = true;
    } else {
        completion->Failed = true;
    }
    std::vector<std::pair<ui32, std::shared_ptr<TPairRuntime>>> pairs;
    for (const auto& pair : extent.PairRuntime) {
        pairs.push_back(pair);
    }
    for (const auto& [idx, runtime] : pairs) {
        FlushPair(key, idx, runtime);
    }
    PairWork.emplace_back(completion);
}

void TIntegrityManager::CompleteFormatWrite(TFormatWrite write, bool ok) {
    const auto chunkIt = IntegrityChunks.find(write.ChunkIdx);
    if (chunkIt == IntegrityChunks.end() || chunkIt->second.Generation != write.ChunkGeneration) {
        // The original owner has retired; the same physical index may now have a new
        // generation. Its free-slot set must never be touched by this stale completion.
        return;
    }
    if (write.Kind == EWriteIoKind::ChunkHeader) {
        auto& chunk = chunkIt->second;
        if (!ok) {
            chunk.Completion->Failed = true;
        }
        Y_ABORT_UNLESS(chunk.HeaderWritesRemaining);
        if (!--chunk.HeaderWritesRemaining) {
            if (!Stopped && !chunk.Completion->Failed) {
                chunk.State = EChunkState::Ready;
                chunk.Completion->Ready = true;
            }
            std::vector<std::pair<TDataChunkKey, std::pair<TExtentRef, std::shared_ptr<TExtentState>>>> extents;
            for (const auto& [key, extent] : Extents) {
                if (extent.Ref.IntegrityChunkIdx == write.ChunkIdx && extent.State == EExtentState::Formatting) {
                    extents.emplace_back(key, std::make_pair(extent.Ref, extent.Completion));
                }
            }
            for (const auto& [key, pair] : extents) {
                MaybeCompleteExtent(key, pair.first, pair.second);
            }
            PairWork.emplace_back(chunk.Completion);
        }
    } else {
        const auto it = Extents.find(write.Key);
        if (it == Extents.end() || it->second.Completion != write.Completion
                || it->second.Ref.VChunkGeneration != write.Ref.VChunkGeneration) {
            ReleaseSlot(write.Ref.IntegrityChunkIdx, write.Ref.ExtentSlot);
            TryAssignExtents();
            return;
        }
        it->second.Formatting = false;
        it->second.FormatComplete = ok;
        MaybeCompleteExtent(write.Key, write.Ref, write.Completion);
    }
}

bool TIntegrityManager::LoadPairImage(TDataChunkKey key, TExtentInfo& extent, ui32 pairIdx,
        const TReadPayload& data, TString* errorReason, bool* lostWriteDetected)
{
    *lostWriteDetected = false;
    if (data.size() != IntegrityPairSlots * sizeof(TIntegrityBlock)) {
        *errorReason = TStringBuilder() << "short integrity pair read: expected "
            << IntegrityPairSlots * sizeof(TIntegrityBlock) << " bytes, got " << data.size();
        return false;
    }

    TIntegrityBlock slots[IntegrityPairSlots];
    data.CopyTo(slots, sizeof(slots));
    const i32 winner = SelectIntegrityBlockWinner(slots, MakeBlockIdentity(key, extent, pairIdx));
    if (winner < 0) {
        *errorReason = TStringBuilder() << "both integrity slots are invalid for pair " << pairIdx;
        return false;
    }

    const TIntegrityBlock& block = slots[winner];
    TPairMeta& pair = extent.Pairs.at(pairIdx);
    if (pair.DigestKnown && pair.Digest != block.Header.IntegrityBlockDigest) {
        *errorReason = TStringBuilder() << "integrity digest mismatch for pair " << pairIdx
            << ": expected " << pair.Digest << ", got " << block.Header.IntegrityBlockDigest;
        *lostWriteDetected = true;
        return false;
    }

    pair.Digest = block.Header.IntegrityBlockDigest;
    pair.DigestKnown = true;
    pair.BitmapKnown = true;
    pair.Resident = true;
    pair.CurrentSlot = winner == 0 ? EPairSlot::A : EPairSlot::B;

    TIntegrityBlockState& state = GetOrCreateBlockState(key, extent, pairIdx);
    state.PairSequenceNumber = block.Header.PairSequenceNumber;
    state.Known.Clear();
    state.Known.Reserve(ChecksumsPerIntegrityBlock);
    std::fill(state.Checksums.begin(), state.Checksums.end(), 0);

    const ui32 firstBlock = pairIdx * ChecksumsPerIntegrityBlock;
    const ui32 endBlock = Min(DataBlocksPerChunkCount, firstBlock + ChecksumsPerIntegrityBlock);
    for (ui32 blockIdx = firstBlock; blockIdx < endBlock; ++blockIdx) {
        const ui32 slot = blockIdx - firstBlock;
        const bool used = block.Header.UsedBlocksBitmap[slot / 8] & ui8(1u << (slot % 8));
        if (!used) {
            continue;
        }
        extent.UsedBlocks.Set(blockIdx);
        state.Known.Set(slot);
        state.Checksums[slot] = UnsealBlockChecksum(block.Checksums[slot], DDiskId,
            PDiskGuid, key.TabletId, key.VChunkIndex, blockIdx);
    }
    return true;
}

bool TIntegrityManager::IsExtentReady(TDataChunkKey key) const {
    const auto it = Extents.find(key);
    return it != Extents.end() && !it->second.DeletionPending
        && it->second.State == EExtentState::Ready;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Write path
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TIntegrityManager::TIntegrityBlockState& TIntegrityManager::GetOrCreateBlockState(TDataChunkKey key,
        TExtentInfo& extent, ui32 pairIdx) {
    const auto [it, inserted] = extent.BlockStates.try_emplace(pairIdx);
    if (inserted) {
        it->second = std::make_unique<TIntegrityBlockState>();
        TIntegrityBlockState& state = *it->second;
        state.Key = key;
        state.PairIdx = pairIdx;
        state.PairSequenceNumber = extent.Pairs.at(pairIdx).CurrentSlot == EPairSlot::B ? 1 : 0;
        state.Known.Reserve(ChecksumsPerIntegrityBlock);
        state.Checksums.resize(ChecksumsPerIntegrityBlock, 0);
        BlockStateLru.PushBack(&state);
        ++BlockStateCount;
        return state;
    }
    TIntegrityBlockState& state = *it->second;
    state.Unlink();
    BlockStateLru.PushBack(&state); // touch
    return state;
}

TIntegrityManager::TIntegrityBlockState* TIntegrityManager::FindBlockState(TExtentInfo& extent, ui32 pairIdx) {
    const auto it = extent.BlockStates.find(pairIdx);
    if (it == extent.BlockStates.end()) {
        return nullptr;
    }
    TIntegrityBlockState& state = *it->second;
    state.Unlink();
    BlockStateLru.PushBack(&state); // touch
    return &state;
}

void TIntegrityManager::EvictBlockStatesOverBudget() {
    while (BlockStateCount > MaxBlockStates) {
        TIntegrityBlockState* victim = nullptr;
        size_t examined = 0;
        while (examined++ < BlockStateCount) {
            TIntegrityBlockState* candidate = BlockStateLru.Front();
            TExtentInfo& extent = Extents.at(candidate->Key);
            const auto runtimeIt = extent.PairRuntime.find(candidate->PairIdx);
            if (extent.Pairs.at(candidate->PairIdx).OperationPins == 0
                    && (runtimeIt == extent.PairRuntime.end()
                    || (!runtimeIt->second->Loading && !runtimeIt->second->Flushing
                        && !runtimeIt->second->Dirty))) {
                victim = candidate;
                break;
            }
            candidate->Unlink();
            BlockStateLru.PushBack(candidate);
        }
        if (!victim) {
            // The budget is soft while all resident states are needed by in-flight operations.
            return;
        }
        const auto extentIt = Extents.find(victim->Key);
        Y_ABORT_UNLESS(extentIt != Extents.end());
        extentIt->second.Pairs.at(victim->PairIdx).Resident = false;
        const size_t numErased = extentIt->second.BlockStates.erase(victim->PairIdx);
        Y_ABORT_UNLESS(numErased == 1); // unlinks from the LRU via ~TIntrusiveListItem
        --BlockStateCount;
    }
}

void TIntegrityManager::DropBlockStates(TExtentInfo& extent) {
    BlockStateCount -= extent.BlockStates.size();
    extent.BlockStates.clear(); // each state unlinks from the LRU via ~TIntrusiveListItem
}

ui32 TIntegrityManager::FirstPair(ui32 offsetInBytes) const {
    return (offsetInBytes / IntegrityUnitSize) / ChecksumsPerIntegrityBlock;
}

ui32 TIntegrityManager::EndPair(ui32 offsetInBytes, ui32 size) const {
    const ui32 endBlock = (offsetInBytes + size + IntegrityUnitSize - 1) / IntegrityUnitSize;
    return (endBlock + ChecksumsPerIntegrityBlock - 1) / ChecksumsPerIntegrityBlock;
}

TIntegrityBlockIdentity TIntegrityManager::MakeBlockIdentity(TDataChunkKey key,
        const TExtentInfo& extent, ui32 pairIdx) const {
    return {
        .OwnerId = key.TabletId,
        .VChunkId = key.VChunkIndex,
        .VChunkGeneration = extent.Ref.VChunkGeneration,
        .IntegrityChunkId = extent.Ref.IntegrityChunkIdx,
        .IntegrityExtentId = extent.Ref.ExtentSlot,
        .IntegrityChunkGeneration = IntegrityChunks.at(extent.Ref.IntegrityChunkIdx).Generation,
        .ChecksumBlockIdx = pairIdx,
    };
}

std::shared_ptr<TIntegrityManager::TPairRuntime> TIntegrityManager::GetPairRuntime(
        TExtentInfo& extent, ui32 pairIdx)
{
    auto& runtime = extent.PairRuntime[pairIdx];
    if (!runtime) {
        runtime = std::make_shared<TPairRuntime>();
        runtime->VChunkGeneration = extent.Ref.VChunkGeneration;
    }
    return runtime;
}

TIntegrityManager::TExtentInfo* TIntegrityManager::FindCurrentPair(TDataChunkKey key,
        ui32 pairIdx, const std::shared_ptr<TPairRuntime>& runtime)
{
    const auto it = Extents.find(key);
    if (it == Extents.end() || it->second.Ref.VChunkGeneration != runtime->VChunkGeneration
            || pairIdx >= it->second.Pairs.size()) {
        return nullptr;
    }
    const auto runtimeIt = it->second.PairRuntime.find(pairIdx);
    return runtimeIt != it->second.PairRuntime.end() && runtimeIt->second == runtime
        ? &it->second : nullptr;
}

void TIntegrityManager::MaybeDropPairRuntime(TExtentInfo& extent, ui32 pairIdx) {
    const auto it = extent.PairRuntime.find(pairIdx);
    if (it == extent.PairRuntime.end()) {
        return;
    }
    const auto& runtime = *it->second;
    const auto& pair = extent.Pairs.at(pairIdx);
    if (!pair.Corrupted && !pair.OperationPins && !runtime.Loading && !runtime.Flushing) {
        extent.PairRuntime.erase(it);
    }
}

bool TIntegrityManager::PairHasAllUsedChecksums(const TExtentInfo& extent, ui32 pairIdx,
        const TIntegrityBlockState& state) const {
    const ui32 firstBlock = pairIdx * ChecksumsPerIntegrityBlock;
    const ui32 endBlock = Min(DataBlocksPerChunkCount, firstBlock + ChecksumsPerIntegrityBlock);
    for (ui32 block = firstBlock; block < endBlock; ++block) {
        if (extent.UsedBlocks.Get(block) && !state.Known.Get(block - firstBlock)) {
            return false;
        }
    }
    return true;
}

TRcBuf TIntegrityManager::MakePairImage(TDataChunkKey key, TExtentInfo& extent, ui32 pairIdx) {
    const auto& pair = extent.Pairs.at(pairIdx);
    TIntegrityBlockState& state = GetOrCreateBlockState(key, extent, pairIdx);
    Y_ABORT_UNLESS(PairHasAllUsedChecksums(extent, pairIdx, state),
        "used data block without a checksum in pair# %" PRIu32, pairIdx);

    auto data = TRcBuf::UninitializedPageAligned(sizeof(TIntegrityBlock));
    auto* block = reinterpret_cast<TIntegrityBlock*>(data.GetDataMut());
    memset(block, 0, sizeof(*block));

    TIntegrityBlockHeader& header = block->Header;
    header.Magic = MagicIntegrityBlock;
    header.FormatVersion = static_cast<ui16>(EIntegrityFormatVersion::BaseAwupf4KiB);
    header.ChecksumBlockIdx = pairIdx;
    header.OwnerId = key.TabletId;
    header.VChunkId = key.VChunkIndex;
    header.VChunkGeneration = extent.Ref.VChunkGeneration;
    header.IntegrityChunkId = extent.Ref.IntegrityChunkIdx;
    header.IntegrityExtentId = extent.Ref.ExtentSlot;
    header.IntegrityChunkGeneration = IntegrityChunks.at(extent.Ref.IntegrityChunkIdx).Generation;
    header.IntegrityBlockDigest = pair.Digest;
    header.PairSequenceNumber = state.PairSequenceNumber + 1;

    const ui32 firstBlock = pairIdx * ChecksumsPerIntegrityBlock;
    const ui32 endBlock = Min(DataBlocksPerChunkCount, firstBlock + ChecksumsPerIntegrityBlock);
    for (ui32 blockIdx = firstBlock; blockIdx < endBlock; ++blockIdx) {
        const ui32 slot = blockIdx - firstBlock;
        if (!extent.UsedBlocks.Get(blockIdx)) {
            continue;
        }
        header.UsedBlocksBitmap[slot / 8] |= ui8(1u << (slot % 8));
        Y_ABORT_UNLESS(state.Known.Get(slot));
        block->Checksums[slot] = SealBlockChecksum(state.Checksums[slot], DDiskId,
            PDiskGuid, key.TabletId, key.VChunkIndex, blockIdx);
    }
    header.BlockChecksum = CalculateRawChecksum(block, sizeof(*block));

    return data;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Read path
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TIntegrityManager::TReadPlan TIntegrityManager::MakeReadPlan(TDataChunkKey key, ui32 offsetInBytes, ui32 size) const {
    TReadPlan plan;

    const auto it = Extents.find(key);
    if (it == Extents.end()) {
        // StartRead rejects allocated chunks without an integrity extent, so this fallback
        // is unreachable through the actor read path.
        return plan;
    }
    const TExtentInfo& extent = it->second;

    if (offsetInBytes % IntegrityUnitSize == 0 && size % IntegrityUnitSize == 0 && size > 0
            && ui64(offsetInBytes) + size <= DataChunkSize) {
        for (ui32 pairIdx = FirstPair(offsetInBytes); pairIdx < EndPair(offsetInBytes, size); ++pairIdx) {
            if (!extent.Pairs.at(pairIdx).BitmapKnown) {
                // A restored pair has not been read yet; pass through until StartRead
                // restores its exact bitmap.
                return plan;
            }
        }
    }

    if (extent.UsedBlocks.Empty()) {
        // Nothing was ever written to this chunk.
        plan.Kind = TReadPlan::AllZero;
        return plan;
    }

    if (offsetInBytes % IntegrityUnitSize != 0 || size % IntegrityUnitSize != 0) {
        // Unaligned ranges cannot be safely zero-masked per block; fall back to passthrough.
        // (DDisk validates requests against a 4 KiB sector size, so this should not happen.)
        return plan;
    }

    Y_ABORT_UNLESS(size > 0 && ui64(offsetInBytes) + size <= DataChunkSize);

    const ui32 firstBlock = offsetInBytes / IntegrityUnitSize;
    const ui32 numBlocks = size / IntegrityUnitSize;

    ui32 usedCount = 0;
    plan.UsedBlocks.Reserve(numBlocks);
    for (ui32 i = 0; i < numBlocks; ++i) {
        if (extent.UsedBlocks.Get(firstBlock + i)) {
            plan.UsedBlocks.Set(i);
            ++usedCount;
        }
    }

    if (usedCount == 0) {
        plan.Kind = TReadPlan::AllZero;
        plan.UsedBlocks.Clear();
    } else if (usedCount == numBlocks) {
        plan.Kind = TReadPlan::Passthrough;
        plan.UsedBlocks.Clear();
    } else {
        plan.Kind = TReadPlan::Mixed;
    }
    return plan;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Mapping snapshot
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TIntegrityManager::TMappingSnapshot TIntegrityManager::SnapshotMapping() const {
    TMappingSnapshot snapshot;
    snapshot.GenerationCounter = GenerationCounter;
    for (const auto& [chunkIdx, chunk] : IntegrityChunks) {
        if (chunk.State == EChunkState::Ready) {
            snapshot.IntegrityChunks.push_back({chunkIdx, chunk.Generation});
        }
    }
    for (const auto& [key, extent] : Extents) {
        if (extent.State == EExtentState::Ready && !extent.DeletionPending) {
            snapshot.Extents.push_back({key, extent.DataChunkIdx, extent.Ref});
        }
    }
    return snapshot;
}

void TIntegrityManager::ApplyMappingSnapshot(const TMappingSnapshot& snapshot) {
    Y_ABORT_UNLESS(IntegrityChunks.empty() && Extents.empty() && PendingExtents.empty(),
        "mapping snapshot must be applied to a fresh manager");

    // Resume the generation counter past everything ever persisted. Generations handed out after
    // the snapshot watermark was taken can only appear in records logged after it, so the max
    // over the watermark and the restored records covers all durable state. A generation reused
    // from an uncommitted (crash-lost) record is benign: the identity fields plus the
    // format-before-use ordering already disambiguate such extents.
    GenerationCounter = Max(GenerationCounter, snapshot.GenerationCounter);

    for (const auto& entry : snapshot.IntegrityChunks) {
        const auto [it, inserted] = IntegrityChunks.try_emplace(entry.ChunkIdx);
        Y_ABORT_UNLESS(inserted);
        it->second.Generation = entry.Generation;
        it->second.State = EChunkState::Ready;
        it->second.Completion->Ready = true;
        GenerationCounter = Max(GenerationCounter, entry.Generation);
    }

    absl::flat_hash_map<TChunkIdx, TDynBitMap> usedSlots;
    for (const auto& entry : snapshot.Extents) {
        const auto chunkIt = IntegrityChunks.find(entry.Ref.IntegrityChunkIdx);
        Y_ABORT_UNLESS(chunkIt != IntegrityChunks.end() && entry.Ref.ExtentSlot < ExtentsPerChunkCount);

        const auto [it, inserted] = Extents.try_emplace(entry.Key);
        Y_ABORT_UNLESS(inserted);
        TExtentInfo& extent = it->second;
        extent.Ref = entry.Ref;
        extent.State = EExtentState::Ready;
        extent.Completion->Ready = extent.Completion->Placed = true;
        extent.DataChunkIdx = entry.DataChunkIdx;
        // Pinned pair state is reconstructed lazily by adjacent 8 KiB A/B reads.
        extent.Pairs.resize(BlocksPerExtentCount);

        GenerationCounter = Max(GenerationCounter, entry.Ref.VChunkGeneration);

        usedSlots[entry.Ref.IntegrityChunkIdx].Set(entry.Ref.ExtentSlot);
    }

    for (auto& [chunkIdx, chunk] : IntegrityChunks) {
        const auto usedIt = usedSlots.find(chunkIdx);
        chunk.FreeSlots.reserve(ExtentsPerChunkCount);
        for (ui32 slot = ExtentsPerChunkCount; slot > 0; --slot) {
            if (usedIt == usedSlots.end() || !usedIt->second.Get(slot - 1)) {
                chunk.FreeSlots.push_back(slot - 1);
            }
        }
    }

}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Introspection
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const TIntegrityManager::TExtentRef* TIntegrityManager::FindExtentRef(TDataChunkKey key) const {
    const auto it = Extents.find(key);
    if (it == Extents.end() || it->second.DeletionPending
            || it->second.State == EExtentState::Pending) {
        return nullptr;
    }
    return &it->second.Ref;
}

ui64 TIntegrityManager::GetIntegrityChunkGeneration(TChunkIdx chunkIdx) const {
    const auto it = IntegrityChunks.find(chunkIdx);
    return it != IntegrityChunks.end() ? it->second.Generation : 0;
}

std::vector<TChunkIdx> TIntegrityManager::GetIntegrityChunkIdxs() const {
    std::vector<TChunkIdx> chunks;
    chunks.reserve(IntegrityChunks.size());
    for (const auto& [chunkIdx, info] : IntegrityChunks) {
        Y_UNUSED(info);
        chunks.push_back(chunkIdx);
    }
    return chunks;
}

bool TIntegrityManager::IsIntegrityChunkFormatted(TChunkIdx chunkIdx) const {
    const auto it = IntegrityChunks.find(chunkIdx);
    return it != IntegrityChunks.end() && it->second.State == EChunkState::Ready;
}

ui64 TIntegrityManager::GetIntegrityBlockDigest(TDataChunkKey key, ui32 integrityBlockIdx) const {
    const auto it = Extents.find(key);
    Y_ABORT_UNLESS(it != Extents.end() && integrityBlockIdx < BlocksPerExtentCount);
    const TPairMeta& pair = it->second.Pairs.at(integrityBlockIdx);
    return pair.DigestKnown ? pair.Digest : 0;
}

bool TIntegrityManager::GetBlockChecksum(TDataChunkKey key, ui32 blockIdx, ui64* checksum) const {
    const auto it = Extents.find(key);
    Y_ABORT_UNLESS(it != Extents.end() && blockIdx < DataBlocksPerChunkCount);
    const auto stateIt = it->second.BlockStates.find(blockIdx / ChecksumsPerIntegrityBlock);
    if (stateIt == it->second.BlockStates.end()) {
        return false;
    }
    const TIntegrityBlockState& state = *stateIt->second;
    const ui32 slot = blockIdx % ChecksumsPerIntegrityBlock;
    if (!state.Known.Get(slot)) {
        return false;
    }
    *checksum = state.Checksums[slot];
    return true;
}

bool TIntegrityManager::HasInFlightOperationsForTablet(ui64 tabletId) const {
    for (const auto& [key, extent] : Extents) {
        if (key.TabletId != tabletId) {
            continue;
        }
        for (const auto& pair : extent.Pairs) {
            if (pair.OperationPins) {
                return true;
            }
        }
        for (const auto& [_, runtime] : extent.PairRuntime) {
            if (runtime->Loading || runtime->Flushing) {
                return true;
            }
        }
    }
    return false;
}

NActors::async<bool> TIntegrityManager::TExtent::WaitImpl(NActors::IActor& actor,
        std::shared_ptr<TExtentState> state, bool ready)
{
    Y_UNUSED(actor);
    while (!(ready ? state->Ready : state->Placed) && !state->Failed) {
        co_await state->Changed.Wait();
    }
    co_return ready ? state->Ready : state->Placed;
}

void TIntegrityManager::Stop() {
    Stopped = true;
    // Snapshot all observers before notification: a resumed coroutine can retire an extent.
    std::vector<std::shared_ptr<TOperationState>> reads;
    reads.reserve(PendingReads.size());
    for (const auto& [_, read] : PendingReads) {
        reads.push_back(read->Completion);
    }
    std::vector<std::shared_ptr<TExtentState>> milestones;
    milestones.reserve(IntegrityChunks.size() + Extents.size());
    for (auto& [_, chunk] : IntegrityChunks) {
        chunk.Completion->Failed = true;
        milestones.push_back(chunk.Completion);
    }
    for (auto& [_, extent] : Extents) {
        extent.Completion->Failed = true;
        milestones.push_back(extent.Completion);
    }
    std::vector<std::shared_ptr<TPendingWrite>> writes;
    for (const auto& [_, write] : PendingWrites) {
        writes.push_back(write);
    }
    for (const auto& write : writes) {
        PairWork.emplace_back(write);
    }
    for (const auto& completion : reads) {
        TOperationResult result;
        result.Status = EOperationStatus::Failed;
        PairWork.emplace_back(TCompletionNotification{completion, std::move(result)});
    }
    for (const auto& milestone : milestones) {
        PairWork.emplace_back(milestone);
    }
    DrainPairWork();
}

void TIntegrityManager::CompleteAllocation(ui64 token, TChunkIdx chunkIdx) {
    if (!PendingChunkAllocations.erase(token)) {
        return;
    }
    if (!chunkIdx) {
        return;
    }
    if (Stopped || CancelChunkAllocationIfExcess()) {
        Host.ReturnChunk(chunkIdx);
    } else {
        OnIntegrityChunkAllocated(chunkIdx);
    }
    DrainPairWork();
}

TIntegrityManager::TPairRead TIntegrityManager::ClaimPairRead(TDataChunkKey key,
        TExtentInfo& extent, ui32 pairIdx, const std::shared_ptr<TPairRuntime>& runtime)
{
    Y_ABORT_UNLESS(!runtime->Loading);
    runtime->Loading = true;
    const ui64 id = NextPairReadId++;
    PairLoads.emplace(id, TPairLoad{key, pairIdx, runtime});
    return {
        id,
        extent.Ref.IntegrityChunkIdx,
        static_cast<ui32>(ExtentOffset(extent.Ref.ExtentSlot)
            + pairIdx * IntegrityPairSlots * IntegrityUnitSize),
        IntegrityPairSlots * IntegrityUnitSize,
    };
}

void TIntegrityManager::CompleteOperation(const std::shared_ptr<TOperationState>& completion,
        TOperationResult result)
{
    if (!completion->Result) {
        completion->Result.emplace(std::move(result));
        completion->Changed.NotifyAll();
    } else if (completion->Settled) {
        // Stop publishes a logical failure while the shared loads are still in flight, so
        // the settle can be the only transition a waiter of a fully owned result sees.
        completion->Changed.NotifyAll();
    }
    if (!completion->Settled) {
        return;
    }
    auto callback = std::move(completion->CompletionCallback);
    if (callback) {
        callback();
    }
}

void TIntegrityManager::CompletePairReads(TConstArrayRef<TPairReadResult> results) {
    std::vector<TPairLoad> completed;
    completed.reserve(results.size());
    // Apply every image before waking either readers or writers. A callback can submit more
    // work, so no reference into PairLoads or Extents survives notification.
    for (const auto& result : results) {
        const auto it = PairLoads.find(result.Id);
        if (it == PairLoads.end()) {
            continue;
        }
        // Duplicate or retired completion.
        auto load = std::move(it->second);
        PairLoads.erase(it);
        auto& runtime = *load.Runtime;
        Y_ABORT_UNLESS(runtime.Loading);
        runtime.Loading = false;
        runtime.Failed = !result.Result.Ok;
        if (auto* extent = FindCurrentPair(load.Key, load.PairIdx, load.Runtime)) {
            if (result.Result.Ok && !LoadPairImage(load.Key, *extent, load.PairIdx, result.Result.Data,
                    &runtime.CorruptionReason, &runtime.LostWriteCorruption)) {
                extent->Pairs.at(load.PairIdx).Corrupted = true;
            }
        } else {
            runtime.Failed = true;
        }
        completed.push_back(std::move(load));
    }
    std::vector<std::shared_ptr<TPendingRead>> ready;
    for (const auto& load : completed) {
        auto readers = std::exchange(load.Runtime->Readers, {});
        for (const auto& weak : readers) {
            if (auto read = weak.lock()) {
                Y_ABORT_UNLESS(read->Remaining);
                if (!--read->Remaining) {
                    ready.push_back(std::move(read));
                }
            }
        }
        if (auto* extent = FindCurrentPair(load.Key, load.PairIdx, load.Runtime)) {
            MaybeDropPairRuntime(*extent, load.PairIdx);
        }
    }
    for (const auto& read : ready) {
        FinishPendingRead(read);
    }
    for (const auto& load : completed) {
        NotifyWriters(load.Runtime);
    }
    EvictBlockStatesOverBudget();
    DrainPairWork();
}

void TIntegrityManager::FinishPendingRead(const std::shared_ptr<TPendingRead>& read) {
    Y_ABORT_UNLESS(!read->Remaining);
    auto it = Extents.find(read->Key);
    TExtentInfo* extent = it != Extents.end() && it->second.Ref.VChunkGeneration == read->VChunkGeneration
        ? &it->second : nullptr;
    const ui32 first = FirstPair(read->Offset), end = EndPair(read->Offset, read->Size);
    TOperationResult result;
    if (!extent) {
        result.Status = EOperationStatus::Failed;
    }
    if (extent && !read->Completion->Result) {
        for (ui32 idx = first; idx < end; ++idx) {
            const auto runtimeIt = extent->PairRuntime.find(idx);
            if (extent->Pairs.at(idx).Corrupted) {
                Y_ABORT_UNLESS(runtimeIt != extent->PairRuntime.end());
                result.Status = EOperationStatus::Corrupted;
                result.ErrorReason = runtimeIt->second->CorruptionReason;
                result.LostWriteDetected = runtimeIt->second->LostWriteCorruption;
                break;
            }
            if (runtimeIt != extent->PairRuntime.end() && runtimeIt->second->Failed) {
                result.Status = EOperationStatus::Failed;
            }
        }
        if (Stopped) {
            result.Status = EOperationStatus::Failed;
        }
        if (result.Status == EOperationStatus::Ok) {
            CollectReadResult(*extent, read->Offset, read->Size, result, false);
        }
    }
    if (extent) {
        for (ui32 idx = first; idx < end; ++idx) {
            Y_ABORT_UNLESS(extent->Pairs.at(idx).OperationPins);
            --extent->Pairs.at(idx).OperationPins;
            MaybeDropPairRuntime(*extent, idx);
        }
    }
    PendingReads.erase(read->Id);
    EvictBlockStatesOverBudget();
    read->Completion->Settled = true;
    PairWork.emplace_back(TCompletionNotification{read->Completion, std::move(result)});
}

void TIntegrityManager::FlushPair(TDataChunkKey key, ui32 pairIdx,
        const std::shared_ptr<TPairRuntime>& runtime)
{
    if (runtime->Flushing || runtime->Failed || !runtime->Dirty || Stopped) {
        return;
    }
    auto* current = FindCurrentPair(key, pairIdx, runtime);
    if (!current) {
        runtime->Failed = true;
        NotifyWriters(runtime);
        return;
    }
    auto& extent = *current;
    if (extent.Completion->Failed) {
        runtime->Failed = true;
        NotifyWriters(runtime);
        return;
    }
    if (!extent.Completion->Ready) {
        return;
    }
    const auto slot = extent.Pairs.at(pairIdx).CurrentSlot == EPairSlot::A ? EPairSlot::B : EPairSlot::A;
    const auto ref = extent.Ref;
    auto image = MakePairImage(key, extent, pairIdx);
    const ui64 id = NextPairWriteId++;
    PairWrites.emplace(id, TPairWrite{key, pairIdx, runtime, runtime->MutationVersion, slot});
    runtime->Dirty = false;
    runtime->Flushing = true;
    const size_t offset = ExtentOffset(ref.ExtentSlot)
        + (pairIdx * IntegrityPairSlots + (slot == EPairSlot::A ? 0 : 1)) * IntegrityUnitSize;
    Y_ABORT_UNLESS(offset <= Max<ui32>());
    PairWork.emplace_back(TWriteSubmission{id, ref.IntegrityChunkIdx,
        static_cast<ui32>(offset),
        std::move(image), EWriteIoKind::Pair});
}

void TIntegrityManager::CompleteWrite(ui64 id, bool ok) {
    if (auto it = FormatWrites.find(id); it != FormatWrites.end()) {
        auto write = std::move(it->second);
        FormatWrites.erase(it);
        CompleteFormatWrite(std::move(write), ok);
    } else {
        PairWork.emplace_back(TPairWriteCompletion{id, ok});
    }
    DrainPairWork();
}

void TIntegrityManager::ProcessPairWriteCompletion(ui64 id, bool ok) {
    const auto it = PairWrites.find(id);
    if (it == PairWrites.end()) {
        return;
    }
    // Duplicate or retired completion.
    auto write = std::move(it->second);
    PairWrites.erase(it);
    const auto runtime = write.Runtime;
    runtime->Flushing = false;
    auto* extent = FindCurrentPair(write.Key, write.PairIdx, runtime);
    if (ok && extent) {
        extent->Pairs.at(write.PairIdx).CurrentSlot = write.Slot;
        auto* state = FindBlockState(*extent, write.PairIdx);
        Y_ABORT_UNLESS(state);
        ++state->PairSequenceNumber;
        runtime->DurableVersion = write.Version;
    } else {
        runtime->Failed = true;
    }
    // Follow-up submission may complete inline. It queues its own result, leaving this frame's
    // bookkeeping intact until we finish and the queue advances.
    if (extent) {
        FlushPair(write.Key, write.PairIdx, runtime);
    }
    if (auto* current = FindCurrentPair(write.Key, write.PairIdx, runtime)) {
        MaybeDropPairRuntime(*current, write.PairIdx);
    }
    NotifyWriters(runtime);
    EvictBlockStatesOverBudget();
}

void TIntegrityManager::DrainPairWork() {
    if (DrainingPairWork) {
        return;
    }
    DrainingPairWork = true;
    Y_DEFER { DrainingPairWork = false; };
    while (!PairWork.empty()) {
        auto work = std::move(PairWork.front());
        PairWork.pop_front();
        if (auto* pair = std::get_if<TPairWriteCompletion>(&work)) {
            ProcessPairWriteCompletion(pair->Id, pair->Ok);
        } else if (auto* write = std::get_if<std::shared_ptr<TPendingWrite>>(&work)) {
            AdvanceWrite(*write);
        } else if (auto* notification = std::get_if<TCompletionNotification>(&work)) {
            CompleteOperation(notification->Completion, std::move(notification->Result));
        } else if (auto* allocation = std::get_if<TAllocationSubmission>(&work)) {
            if (PendingChunkAllocations.contains(allocation->Token)) {
                if (Stopped) {
                    CompleteAllocation(allocation->Token, 0);
                }
                else {
                    Host.SubmitAllocation(allocation->Token);
                }
            }
        } else if (auto* submission = std::get_if<TWriteSubmission>(&work)) {
            if (Stopped) {
                CompleteWrite(submission->Id, false);
            }
            else {
                Host.SubmitWrite(submission->Id, submission->ChunkIdx, submission->Offset,
                std::move(submission->Data), submission->Kind);
            }
        } else {
            auto milestone = std::get<std::shared_ptr<TExtentState>>(work);
            milestone->Changed.NotifyAll();
            if (auto callback = milestone->ProgressCallback) {
                callback();
            }
        }
    }
}

TIntegrityManager::TReadPreparation TIntegrityManager::PrepareRead(
        TDataChunkKey key, ui32 offset, ui32 size, std::optional<TOperationResult>& readyResult)
{
    Y_ABORT_UNLESS(!readyResult);
    ValidateOperationRange(offset, size);
    TReadPreparation preparation;
    bool failed = false;
    const auto it = Extents.find(key);
    if (Stopped) {
        readyResult.emplace().Status = EOperationStatus::Failed;
        return preparation;
    }
    if (it == Extents.end() || it->second.DeletionPending) {
        auto& error = readyResult.emplace();
        error.Status = EOperationStatus::Corrupted;
        error.ErrorReason = "integrity extent is missing for an allocated data chunk";
        return preparation;
    }
    auto& extent = it->second;
    const ui32 first = FirstPair(offset), end = EndPair(offset, size);
    bool pending = false;
    for (ui32 idx = first; idx < end; ++idx) {
        const auto& pair = extent.Pairs.at(idx);
        const auto runtimeIt = extent.PairRuntime.find(idx);
        if (pair.Corrupted) {
            Y_ABORT_UNLESS(runtimeIt != extent.PairRuntime.end());
            auto& error = readyResult.emplace();
            error.Status = EOperationStatus::Corrupted;
            error.ErrorReason = runtimeIt->second->CorruptionReason;
            error.LostWriteDetected = runtimeIt->second->LostWriteCorruption;
            return preparation;
        }
        if (runtimeIt != extent.PairRuntime.end() && runtimeIt->second->Failed) {
            failed = true;
        }
        pending |= !pair.Resident;
    }
    if (failed) {
        readyResult.emplace().Status = EOperationStatus::Failed;
        return preparation;
    }
    if (!pending) {
        CollectReadResult(extent, offset, size, readyResult.emplace(), true);
        EvictBlockStatesOverBudget();
        return preparation;
    }
    // Pin the complete range before claiming any load. No submission or eager coroutine may
    // evict a cached sibling before the read captures its complete immutable snapshot.
    for (ui32 idx = first; idx < end; ++idx) {
        ++extent.Pairs.at(idx).OperationPins;
    }
    auto read = std::make_shared<TPendingRead>();
    read->Id = NextPendingReadId++;
    read->Key = key;
    read->VChunkGeneration = extent.Ref.VChunkGeneration;
    read->Offset = offset;
    read->Size = size;
    read->Completion = std::make_shared<TOperationState>();
    read->Completion->Settled = false;
    PendingReads.emplace(read->Id, read);
    preparation.Pending = TOperation(read->Completion);
    for (ui32 idx = first; idx < end; ++idx) {
        if (extent.Pairs.at(idx).Resident) {
            FindBlockState(extent, idx);
            continue;
        }
        auto runtime = GetPairRuntime(extent, idx);
        if (!runtime->Loading) {
            preparation.Reads.push_back(ClaimPairRead(key, extent, idx, runtime));
        }
        ++read->Remaining;
        runtime->Readers.push_back(read);
    }
    Y_ABORT_UNLESS(read->Remaining);
    return preparation;
}

TIntegrityManager::TOperation TIntegrityManager::StartRead(TDataChunkKey key, ui32 offset, ui32 size) {
    std::optional<TOperationResult> readyResult;
    auto preparation = PrepareRead(key, offset, size, readyResult);
    if (readyResult) {
        auto completion = std::make_shared<TOperationState>();
        completion->Result.emplace(std::move(*readyResult));
        return TOperation(std::move(completion));
    }
    if (!preparation.Reads.empty()) {
        Host.SubmitPairReads(std::move(preparation.Reads));
    }
    return std::move(preparation.Pending);
}

void TIntegrityManager::ValidateOperationRange(ui32 offset, ui32 size) const {
    Y_ABORT_UNLESS(size && offset % IntegrityUnitSize == 0 && size % IntegrityUnitSize == 0
        && ui64(offset) + size <= DataChunkSize);
}

void TIntegrityManager::CollectSingleReadResult(TExtentInfo& extent, ui32 block,
        TOperationResult& result, bool touch)
{
    const ui32 pair = block / ChecksumsPerIntegrityBlock;
    const TIntegrityBlockState* state = nullptr;
    if (touch) {
        state = FindBlockState(extent, pair);
    } else if (const auto it = extent.BlockStates.find(pair); it != extent.BlockStates.end()) {
        state = it->second.get();
    }
    const bool used = extent.UsedBlocks.Get(block);
    result.ReadPlan.Kind = !used && extent.Pairs.at(pair).BitmapKnown
        ? TReadPlan::AllZero : TReadPlan::Passthrough;
    const ui32 slot = block % ChecksumsPerIntegrityBlock;
    if (!used) {
        result.Checksums.SetSingle(GetZeroBlockChecksum());
    } else if (state && state->Known.Get(slot)) {
        result.Checksums.SetSingle(state->Checksums[slot]);
    } else {
        result.Status = EOperationStatus::Corrupted;
        result.ErrorReason = TStringBuilder() << "checksum is missing for used data block " << block;
    }
}

void TIntegrityManager::CollectReadResult(TExtentInfo& extent, ui32 offset, ui32 size,
        TOperationResult& result, bool touch)
{
    const ui32 firstBlock = offset / IntegrityUnitSize;
    const ui32 endBlock = (offset + size) / IntegrityUnitSize;
    const ui32 numBlocks = endBlock - firstBlock;
    if (numBlocks == 1) {
        CollectSingleReadResult(extent, firstBlock, result, touch);
        return;
    }
    std::vector<ui64> checksums;
    checksums.reserve(numBlocks);
    result.ReadPlan.UsedBlocks.Reserve(numBlocks);
    ui32 usedCount = 0;
    bool bitmapKnown = true;
    for (ui32 pair = FirstPair(offset); pair < EndPair(offset, size); ++pair) {
        bitmapKnown &= extent.Pairs.at(pair).BitmapKnown;
        const TIntegrityBlockState* state = nullptr;
        if (touch) {
            state = FindBlockState(extent, pair);
        } else if (const auto it = extent.BlockStates.find(pair); it != extent.BlockStates.end()) {
            state = it->second.get();
        }
        const ui32 begin = Max(firstBlock, pair * ChecksumsPerIntegrityBlock);
        const ui32 end = Min(endBlock, (pair + 1) * ChecksumsPerIntegrityBlock);
        for (ui32 block = begin; block < end; ++block) {
            const bool used = extent.UsedBlocks.Get(block);
            if (used) {
                ++usedCount;
                result.ReadPlan.UsedBlocks.Set(block - firstBlock);
            }
            // Keep collecting the plan after the first checksum error.
            if (result.Status != EOperationStatus::Ok) {
                continue;
            }
            const ui32 slot = block % ChecksumsPerIntegrityBlock;
            if (!used) {
                checksums.push_back(GetZeroBlockChecksum());
            } else if (state && state->Known.Get(slot)) {
                checksums.push_back(state->Checksums[slot]);
            } else {
                result.Status = EOperationStatus::Corrupted;
                result.ErrorReason = TStringBuilder() << "checksum is missing for used data block " << block;
                checksums.clear();
            }
        }
    }
    if (result.Status == EOperationStatus::Ok) {
        result.Checksums.SetMany(std::move(checksums));
    }
    if (!bitmapKnown || usedCount == numBlocks) {
        result.ReadPlan.Kind = TReadPlan::Passthrough;
        result.ReadPlan.UsedBlocks.Clear();
    } else if (!usedCount) {
        result.ReadPlan.Kind = TReadPlan::AllZero;
        result.ReadPlan.UsedBlocks.Clear();
    } else {
        result.ReadPlan.Kind = TReadPlan::Mixed;
    }
}

TIntegrityManager::TWritePreparation TIntegrityManager::PrepareWrite(TDataChunkKey key, ui32 offset, ui32 size) {
    ValidateOperationRange(offset, size);
    auto completion = std::make_shared<TOperationState>();
    auto preparation = std::make_shared<TOperationState>();
    TWritePreparation resultHandle{0, TOperation(preparation), TOperation(completion)};
    TOperationResult result;
    auto it = Extents.find(key);
    if (Stopped) {
        result.Status = EOperationStatus::Failed;
        CompleteOperation(preparation, result);
        CompleteOperation(completion, std::move(result));
        return resultHandle;
    }
    if (it == Extents.end() || it->second.DeletionPending) {
        result.Status = EOperationStatus::Corrupted;
        result.ErrorReason = "integrity extent is missing for an allocated data chunk";
        CompleteOperation(preparation, result);
        CompleteOperation(completion, std::move(result));
        return resultHandle;
    }
    const ui32 first = FirstPair(offset), end = EndPair(offset, size);
    for (ui32 idx = first; idx < end; ++idx) {
        if (it->second.Pairs.at(idx).Corrupted) {
            const auto& runtime = *it->second.PairRuntime.at(idx);
            result.Status = EOperationStatus::Corrupted;
            result.ErrorReason = runtime.CorruptionReason;
            result.LostWriteDetected = runtime.LostWriteCorruption;
            CompleteOperation(preparation, result);
            CompleteOperation(completion, std::move(result));
            return resultHandle;
        }
    }
    // Own the complete range before claiming loads or allowing synchronous completions.
    auto write = std::make_shared<TPendingWrite>();
    write->Id = NextPendingWriteId++;
    write->Key = key;
    write->Offset = offset;
    write->Size = size;
    write->Completion = completion;
    write->Preparation = preparation;
    completion->Settled = false;
    preparation->Settled = false;
    resultHandle.Id = write->Id;
    PendingWrites.emplace(write->Id, write);
    for (ui32 idx = first; idx < end; ++idx) {
        ++it->second.Pairs.at(idx).OperationPins;
    }
    std::vector<TPairRead> reads;
    for (ui32 idx = first; idx < end; ++idx) {
        auto& extent = Extents.at(key);
        auto runtime = GetPairRuntime(extent, idx);
        write->Runtimes.push_back(runtime);
        runtime->Writers.push_back(write);
        const auto& pair = extent.Pairs.at(idx);
        if (!Stopped && !pair.Resident && !runtime->Loading && !runtime->Failed) {
            reads.push_back(ClaimPairRead(key, extent, idx, runtime));
        } else if (pair.Resident) {
            FindBlockState(extent, idx);
        }
    }
    if (!reads.empty()) {
        Host.SubmitPairReads(std::move(reads));
    }
    PairWork.emplace_back(write);
    DrainPairWork();
    return resultHandle;
}

void TIntegrityManager::ConsumeWrite(ui64 id, std::vector<ui64> checksums) {
    const auto it = PendingWrites.find(id);
    if (it == PendingWrites.end()) {
        return;
    }
    const auto write = it->second;
    Y_ABORT_UNLESS(!write->Consumed && !write->Cancelled && !write->Applied);
    Y_ABORT_UNLESS(checksums.size() == write->Size / IntegrityUnitSize);
    write->Consumed = true;
    write->Checksums = std::move(checksums);
    PairWork.emplace_back(write);
    DrainPairWork();
}

void TIntegrityManager::CancelWrite(ui64 id) {
    const auto it = PendingWrites.find(id);
    if (it == PendingWrites.end() || it->second->Applied) {
        return;
    }
    it->second->Cancelled = true;
    PairWork.emplace_back(it->second);
    DrainPairWork();
}

TIntegrityManager::TOperation TIntegrityManager::StartWrite(TDataChunkKey key, ui32 offset, ui32 size,
        const std::vector<ui64>& checksums)
{
    Y_ABORT_UNLESS(checksums.size() == size / IntegrityUnitSize);
    auto preparation = PrepareWrite(key, offset, size);
    if (preparation.Id) {
        ConsumeWrite(preparation.Id, checksums);
    }
    return std::move(preparation.Durable);
}

void TIntegrityManager::FinishWrite(const std::shared_ptr<TPendingWrite>& write, TOperationResult result) {
    for (ui32 idx = FirstPair(write->Offset); idx < EndPair(write->Offset, write->Size); ++idx) {
        if (auto* extent = FindCurrentPair(write->Key, idx,
                write->Runtimes[idx - FirstPair(write->Offset)])) {
            Y_ABORT_UNLESS(extent->Pairs.at(idx).OperationPins);
            --extent->Pairs.at(idx).OperationPins;
            MaybeDropPairRuntime(*extent, idx);
        }
    }
    PendingWrites.erase(write->Id);
    EvictBlockStatesOverBudget();
    if (!write->PreparationNotified) {
        write->PreparationNotified = true;
        write->Preparation->Settled = true;
        PairWork.emplace_back(TCompletionNotification{write->Preparation, result});
    }
    write->Completion->Settled = true;
    PairWork.emplace_back(TCompletionNotification{write->Completion, std::move(result)});
}

void TIntegrityManager::NotifyWriters(const std::shared_ptr<TPairRuntime>& runtime) {
    // Writer advancement and completion notification run only through the queue. A callback may
    // delete and recreate this key or start another mutation with immediately completed I/O.
    const auto writers = runtime->Writers;
    for (const auto& weak : writers) {
        if (auto write = weak.lock()) {
            PairWork.emplace_back(std::move(write));
        }
    }
    std::erase_if(runtime->Writers, [](const auto& weak) {
        auto write = weak.lock();
        return !write || write->Completion->Settled;
    });
}

void TIntegrityManager::AdvanceWrite(const std::shared_ptr<TPendingWrite>& write) {
    if (write->Completion->Settled) {
        return;
    }
    const auto key = write->Key;
    const ui32 first = FirstPair(write->Offset), end = EndPair(write->Offset, write->Size);
    auto& runtimes = write->Runtimes;
    TOperationResult result;
    if (!write->Applied) {
        // Failure still joins all accepted sibling loads before releasing any pins.
        for (const auto& runtime : runtimes) {
            if (runtime->Loading) {
                return;
            }
        }
    }
    for (ui32 idx = first; idx < end; ++idx) {
        if (!FindCurrentPair(key, idx, runtimes[idx - first])) {
            result.Status = EOperationStatus::Failed;
            FinishWrite(write, std::move(result));
            return;
        }
    }
    if (!write->Applied) {
        for (ui32 idx = first; idx < end; ++idx) {
            const auto& runtime = runtimes[idx - first];
            if (auto* extent = FindCurrentPair(key, idx, runtime); extent && extent->Pairs.at(idx).Corrupted) {
                result.Status = EOperationStatus::Corrupted;
                result.ErrorReason = runtime->CorruptionReason;
                result.LostWriteDetected = runtime->LostWriteCorruption;
                FinishWrite(write, std::move(result));
                return;
            }
            if (runtime->Failed) {
                result.Status = EOperationStatus::Failed;
            }
        }
        if (Stopped || write->Cancelled || result.Status == EOperationStatus::Failed) {
            result.Status = EOperationStatus::Failed;
            FinishWrite(write, std::move(result));
            return;
        }
        if (!write->PreparationNotified) {
            write->PreparationNotified = true;
            write->Preparation->Settled = true;
            PairWork.emplace_back(TCompletionNotification{write->Preparation, {}});
        }
        if (!write->Consumed) {
            return;
        }
        // Apply the complete mutation and capture all versions in one actor turn.
        auto& extent = *FindCurrentPair(key, first, runtimes.front());
        for (ui32 block = write->Offset / IntegrityUnitSize;
                block < (write->Offset + write->Size) / IntegrityUnitSize; ++block) {
            extent.UsedBlocks.Set(block);
            const ui32 idx = block / ChecksumsPerIntegrityBlock, slot = block % ChecksumsPerIntegrityBlock;
            auto& state = GetOrCreateBlockState(key, extent, idx);
            auto& pair = extent.Pairs.at(idx);
            const ui64 checksum = write->Checksums[block - write->Offset / IntegrityUnitSize];
            if (state.Known.Get(slot)) {
                UpdateRoot(pair.Digest, extent.Ref.VChunkGeneration, block, state.Checksums[slot], checksum);
            } else {
                pair.Digest ^= Contribution(extent.Ref.VChunkGeneration, block, checksum);
                state.Known.Set(slot);
            }
            state.Checksums[slot] = checksum;
            pair.DigestKnown = true;
        }
        for (auto& runtime : runtimes) {
            write->Versions.push_back(++runtime->MutationVersion);
            runtime->Dirty = true;
        }
        write->Applied = true;
        std::vector<ui64>().swap(write->Checksums);
        for (ui32 idx = first; idx < end; ++idx) {
            auto runtime = runtimes[idx - first];
            FlushPair(key, idx, runtime);
        }
        EvictBlockStatesOverBudget();
    }
    for (size_t i = 0; i < runtimes.size(); ++i) {
        const auto& runtime = runtimes[i];
        if (runtime->DurableVersion < write->Versions[i]) {
            if (!runtime->Failed && (!Stopped || runtime->Flushing)) {
                return;
            }
            result.Status = EOperationStatus::Failed;
        }
    }
    FinishWrite(write, std::move(result));
}

} // namespace NKikimr::NDDisk
