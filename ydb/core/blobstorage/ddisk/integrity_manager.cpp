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
    , MaxBlockStates(Max<size_t>(1, checksumCacheBytes / BlockStateApproxBytes))
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
}

void TIntegrityManager::FreeExtent(TDataChunkKey key, TExtentInfo& extent) {
    for (const auto& pair : extent.Pairs) {
        Y_ABORT_UNLESS(!pair.OperationPins);
    }
    for (const auto& [_, runtime] : extent.PairRuntime) {
        Y_ABORT_UNLESS(!runtime->Loading && !runtime->Flushing);
    }
    extent.Completion->Failed = true;
    extent.Completion->Changed.NotifyAll();
    DropBlockStates(extent);
    if (extent.State == EExtentState::Pending) {
        std::erase(PendingExtents, key);
    } else if (!extent.Formatting) {
        ReleaseSlot(extent.Ref.IntegrityChunkIdx, extent.Ref.ExtentSlot);
    }
    // A formatting coroutine retains the original slot and generation. It releases an
    // orphan only after its accepted write completes, even if the key has been reused.
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
    if (Stopped) { return; }
    size_t supply = size_t(PendingChunkAllocations) * ExtentsPerChunkCount;
    for (const auto& [chunkIdx, chunk] : IntegrityChunks) {
        supply += chunk.FreeSlots.size();
    }
    while (supply < PendingExtents.size()) {
        ++PendingChunkAllocations;
        supply += ExtentsPerChunkCount;
        Host.Launch([this] { return AllocateChunk(Host.Actor(), *this); });
    }
}

void TIntegrityManager::OnIntegrityChunkAllocated(TChunkIdx chunkIdx) {
    Y_ABORT_UNLESS(PendingChunkAllocations > 0);
    --PendingChunkAllocations;

    const auto [it, inserted] = IntegrityChunks.try_emplace(chunkIdx);
    Y_ABORT_UNLESS(inserted, "integrity chunk already in use, ChunkIdx# %" PRIu32, chunkIdx);

    TIntegrityChunkInfo& chunk = it->second;
    chunk.Generation = AllocateGeneration();
    chunk.FreeSlots.reserve(ExtentsPerChunkCount);
    for (ui32 slot = ExtentsPerChunkCount; slot > 0; --slot) {
        chunk.FreeSlots.push_back(slot - 1);
    }

    chunk.HeaderWritesRemaining = ChunkHeaderReplicaCount;
    for (ui32 replica = 0; replica < ChunkHeaderReplicaCount; ++replica) {
        Host.Launch([this, chunkIdx, replica] { return FormatHeader(Host.Actor(), *this, chunkIdx, replica); });
    }
    TryAssignExtents();
}

bool TIntegrityManager::CancelChunkAllocationIfExcess() {
    Y_ABORT_UNLESS(PendingChunkAllocations > 0);
    size_t supply = size_t(PendingChunkAllocations - 1) * ExtentsPerChunkCount;
    for (const auto& [chunkIdx, chunk] : IntegrityChunks) {
        supply += chunk.FreeSlots.size();
    }
    if (supply < PendingExtents.size()) {
        return false;
    }
    --PendingChunkAllocations;
    return true;
}

NActors::async<void> TIntegrityManager::FormatHeader(NActors::IActor& actor, TIntegrityManager& self,
        TChunkIdx chunkIdx, ui32 replica) {
    Y_UNUSED(actor);
    const auto generation = self.IntegrityChunks.at(chunkIdx).Generation;
    auto data = TRcBuf::UninitializedPageAligned(sizeof(TIntegrityChunkHeader));
    auto* header = reinterpret_cast<TIntegrityChunkHeader*>(data.GetDataMut());
    memset(header, 0, sizeof(*header));
    header->Magic = MagicIntegrityChunkHeader;
    header->FormatVersion = static_cast<ui32>(EIntegrityFormatVersion::BaseAwupf4KiB);
    header->HeaderSize = sizeof(TIntegrityChunkHeader);
    header->DDiskId = self.DDiskId;
    header->PDiskGuid = self.PDiskGuid;
    header->IntegrityChunkId = chunkIdx;
    header->IntegrityChunkGeneration = generation;
    header->HeaderChecksum = CalculateRawChecksum(header, sizeof(*header));


    const bool ok = co_await self.Host.Write(chunkIdx, self.ChunkHeaderReplicaOffset(replica),
        std::move(data), EWriteIoKind::ChunkHeader);
    auto& chunk = self.IntegrityChunks.at(chunkIdx);
    if (!ok) { chunk.Completion->Failed = true; }
    if (!--chunk.HeaderWritesRemaining) {
        if (!chunk.Completion->Failed) {
            chunk.State = EChunkState::Ready;
            chunk.Completion->Ready = true;
        }
        chunk.Completion->Changed.NotifyAll();
    }
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
            return; // waiting for a chunk allocation
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

        extent.Completion->Placed = true;
        extent.Completion->Changed.NotifyAll();
        extent.Formatting = true;
        const auto ref = extent.Ref;
        auto completion = extent.Completion;
        Host.Launch([this, key, ref, completion] {
            return FormatExtent(Host.Actor(), *this, key, ref, completion);
        });
    }
}

NActors::async<void> TIntegrityManager::FormatExtent(NActors::IActor& actor, TIntegrityManager& self,
        TDataChunkKey key, TExtentRef ref, std::shared_ptr<TExtentState> completion) {
    const auto generation = self.IntegrityChunks.at(ref.IntegrityChunkIdx).Generation;
    auto headers = self.IntegrityChunks.at(ref.IntegrityChunkIdx).Completion;
    auto data = TRcBuf::UninitializedPageAligned(self.ExtentOnDiskSizeBytes);
    auto* blocks = reinterpret_cast<TIntegrityBlock*>(data.GetDataMut());
    memset(blocks, 0, self.ExtentOnDiskSizeBytes);

    for (ui32 pair = 0; pair < self.BlocksPerExtentCount; ++pair) {
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


    const bool ok = co_await self.Host.Write(ref.IntegrityChunkIdx, self.ExtentOffset(ref.ExtentSlot),
        std::move(data), EWriteIoKind::ExtentFormat);
    auto it = self.Extents.find(key);
    if (it == self.Extents.end() || it->second.Ref.VChunkGeneration != ref.VChunkGeneration) {
        self.ReleaseSlot(ref.IntegrityChunkIdx, ref.ExtentSlot);
        self.TryAssignExtents();
        co_return;
    }
    it->second.Formatting = false;
    it->second.FormatComplete = ok;
    if (ok) {
        const TExtent headerExtent(headers);
        if (!headerExtent.GetReadyResult().has_value()) {
            co_await headerExtent.WaitReady(actor);
        }
    }
    // Re-look up after suspension: the logical extent may have been deleted and replaced.
    it = self.Extents.find(key);
    if (it != self.Extents.end() && it->second.Ref.VChunkGeneration == ref.VChunkGeneration
            && ok && headers->Ready) {
        it->second.State = EExtentState::Ready;
        completion->Ready = !it->second.DeletionPending;
    } else {
        completion->Failed = true;
    }
    completion->Changed.NotifyAll();
}

bool TIntegrityManager::LoadPairImage(TDataChunkKey key, TExtentInfo& extent, ui32 pairIdx,
        const TRope& data, TString* errorReason, bool* lostWriteDetected) {
    *lostWriteDetected = false;
    if (data.size() != IntegrityPairSlots * sizeof(TIntegrityBlock)) {
        *errorReason = TStringBuilder() << "short integrity pair read: expected "
            << IntegrityPairSlots * sizeof(TIntegrityBlock) << " bytes, got " << data.size();
        return false;
    }

    TIntegrityBlock slots[IntegrityPairSlots];
    data.Begin().ExtractPlainDataAndAdvance(slots, sizeof(slots));
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
        TExtentInfo& extent, ui32 pairIdx) {
    auto& runtime = extent.PairRuntime[pairIdx];
    if (!runtime) { runtime = std::make_shared<TPairRuntime>(); }
    return runtime;
}

void TIntegrityManager::MaybeDropPairRuntime(TExtentInfo& extent, ui32 pairIdx) {
    const auto it = extent.PairRuntime.find(pairIdx);
    if (it == extent.PairRuntime.end()) { return; }
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
        if (key.TabletId != tabletId) { continue; }
        for (const auto& pair : extent.Pairs) {
            if (pair.OperationPins) { return true; }
        }
        for (const auto& [_, runtime] : extent.PairRuntime) {
            if (runtime->Loading || runtime->Flushing) { return true; }
        }
    }
    return false;
}

NActors::async<bool> TIntegrityManager::TExtent::WaitImpl(NActors::IActor& actor,
        std::shared_ptr<TExtentState> state, bool ready) {
    Y_UNUSED(actor);
    while (!(ready ? state->Ready : state->Placed) && !state->Failed) {
        co_await state->Changed.Wait();
    }
    co_return ready ? state->Ready : state->Placed;
}

void TIntegrityManager::Stop() {
    Stopped = true;
    // Publish failure now, but retain every read's pins until its shared metadata loads settle.
    std::vector<std::shared_ptr<TOperationState>> reads;
    reads.reserve(PendingReads.size());
    for (const auto& [_, read] : PendingReads) { reads.push_back(read->Completion); }
    for (auto& [_, chunk] : IntegrityChunks) {
        chunk.Completion->Failed = true;
        chunk.Completion->Changed.NotifyAll();
    }
    for (auto& [_, extent] : Extents) {
        extent.Completion->Failed = true;
        extent.Completion->Changed.NotifyAll();
        for (auto& [_, runtime] : extent.PairRuntime) {
            runtime->Changed.NotifyAll();
        }
    }
    for (const auto& completion : reads) {
        TOperationResult result;
        result.Status = EOperationStatus::Failed;
        CompleteOperation(completion, std::move(result));
    }
}

NActors::async<void> TIntegrityManager::AllocateChunk(NActors::IActor& actor, TIntegrityManager& self) {
    Y_UNUSED(actor);
    const auto chunk = co_await self.Host.Allocate();
    if (!chunk || self.Stopped) {
        --self.PendingChunkAllocations;
        if (chunk) { self.Host.ReturnChunk(chunk); }
    } else if (self.CancelChunkAllocationIfExcess()) {
        self.Host.ReturnChunk(chunk);
    } else {
        self.OnIntegrityChunkAllocated(chunk);
    }
}

TIntegrityManager::TPairRead TIntegrityManager::ClaimPairRead(TDataChunkKey key,
        TExtentInfo& extent, ui32 pairIdx, const std::shared_ptr<TPairRuntime>& runtime) {
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
        TOperationResult result) {
    if (!completion->Result) {
        completion->Result.emplace(std::move(result));
        completion->Changed.NotifyAll();
    }
    if (!completion->Settled) { return; }
    auto callback = std::move(completion->CompletionCallback);
    if (callback) { callback(); }
}

void TIntegrityManager::CompletePairReads(TConstArrayRef<TPairReadResult> results) {
    std::vector<TPairLoad> completed;
    completed.reserve(results.size());
    // Apply every image before waking either readers or writers. A callback can submit more
    // work, so no reference into PairLoads or Extents survives notification.
    for (const auto& result : results) {
        const auto it = PairLoads.find(result.Id);
        Y_ABORT_UNLESS(it != PairLoads.end());
        auto load = std::move(it->second);
        PairLoads.erase(it);
        auto& extent = Extents.at(load.Key); // Loading/pins prohibit deletion until retirement.
        auto& runtime = *load.Runtime;
        Y_ABORT_UNLESS(runtime.Loading);
        runtime.Loading = false;
        runtime.Failed = !result.Result.Ok;
        if (result.Result.Ok && !LoadPairImage(load.Key, extent, load.PairIdx, result.Result.Data,
                &runtime.CorruptionReason, &runtime.LostWriteCorruption)) {
            extent.Pairs.at(load.PairIdx).Corrupted = true;
        }
        completed.push_back(std::move(load));
    }
    std::vector<std::shared_ptr<TPendingRead>> ready;
    for (const auto& load : completed) {
        auto readers = std::exchange(load.Runtime->Readers, {});
        for (const auto& weak : readers) {
            if (auto read = weak.lock()) {
                Y_ABORT_UNLESS(read->Remaining);
                if (!--read->Remaining) { ready.push_back(std::move(read)); }
            }
        }
        load.Runtime->Changed.NotifyAll();
        MaybeDropPairRuntime(Extents.at(load.Key), load.PairIdx);
    }
    for (const auto& read : ready) { FinishPendingRead(read); }
    EvictBlockStatesOverBudget();
}

void TIntegrityManager::FinishPendingRead(const std::shared_ptr<TPendingRead>& read) {
    Y_ABORT_UNLESS(!read->Remaining);
    auto& extent = Extents.at(read->Key);
    const ui32 first = FirstPair(read->Offset), end = EndPair(read->Offset, read->Size);
    TOperationResult result;
    if (!read->Completion->Result) {
        for (ui32 idx = first; idx < end; ++idx) {
            const auto it = extent.PairRuntime.find(idx);
            if (extent.Pairs.at(idx).Corrupted) {
                Y_ABORT_UNLESS(it != extent.PairRuntime.end());
                result.Status = EOperationStatus::Corrupted;
                result.ErrorReason = it->second->CorruptionReason;
                result.LostWriteDetected = it->second->LostWriteCorruption;
                break;
            }
            if (it != extent.PairRuntime.end() && it->second->Failed) {
                result.Status = EOperationStatus::Failed;
            }
        }
        if (Stopped) { result.Status = EOperationStatus::Failed; }
        if (result.Status == EOperationStatus::Ok) {
            CollectReadResult(extent, read->Offset, read->Size, result, false);
        }
    }
    for (ui32 idx = first; idx < end; ++idx) {
        Y_ABORT_UNLESS(extent.Pairs.at(idx).OperationPins);
        --extent.Pairs.at(idx).OperationPins;
        MaybeDropPairRuntime(extent, idx);
    }
    PendingReads.erase(read->Id);
    EvictBlockStatesOverBudget();
    read->Completion->Settled = true;
    CompleteOperation(read->Completion, std::move(result));
}

NActors::async<void> TIntegrityManager::FlushPair(NActors::IActor& actor, TIntegrityManager& self,
        TDataChunkKey key, ui32 pairIdx, std::shared_ptr<TPairRuntime> runtime) {
    auto completion = self.Extents.at(key).Completion;
    const TExtent extentCompletion(completion);
    const auto immediate = extentCompletion.GetReadyResult();
    const bool ready = immediate ? *immediate : co_await extentCompletion.WaitReady(actor);
    if (!ready) { runtime->Failed = true; }
    while (ready && runtime->Dirty && !self.Stopped) {
        auto& extent = self.Extents.at(key);
        const auto ref = extent.Ref;
        const auto slot = extent.Pairs.at(pairIdx).CurrentSlot == EPairSlot::A ? EPairSlot::B : EPairSlot::A;
        const auto version = runtime->MutationVersion;
        auto image = self.MakePairImage(key, extent, pairIdx);
        runtime->Dirty = false;
        const bool ok = co_await self.Host.Write(ref.IntegrityChunkIdx,
            self.ExtentOffset(ref.ExtentSlot)
                + (pairIdx * IntegrityPairSlots + (slot == EPairSlot::A ? 0 : 1)) * IntegrityUnitSize,
            std::move(image), EWriteIoKind::Pair);
        if (!ok) {
            runtime->Failed = true;
            break;
        }
        auto& current = self.Extents.at(key);
        current.Pairs.at(pairIdx).CurrentSlot = slot;
        ++self.FindBlockState(current, pairIdx)->PairSequenceNumber;
        runtime->DurableVersion = version;
        runtime->Changed.NotifyAll();
    }
    runtime->Flushing = false;
    runtime->Changed.NotifyAll();
    self.MaybeDropPairRuntime(self.Extents.at(key), pairIdx);
    self.EvictBlockStatesOverBudget();
}

TIntegrityManager::TReadPreparation TIntegrityManager::PrepareRead(
        TDataChunkKey key, ui32 offset, ui32 size, std::optional<TOperationResult>& readyResult) {
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
    for (ui32 idx = first; idx < end; ++idx) { ++extent.Pairs.at(idx).OperationPins; }
    auto read = std::make_shared<TPendingRead>();
    read->Id = NextPendingReadId++;
    read->Key = key;
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
    if (!preparation.Reads.empty()) { Host.SubmitPairReads(std::move(preparation.Reads)); }
    return std::move(preparation.Pending);
}

void TIntegrityManager::ValidateOperationRange(ui32 offset, ui32 size) const {
    Y_ABORT_UNLESS(size && offset % IntegrityUnitSize == 0 && size % IntegrityUnitSize == 0
        && ui64(offset) + size <= DataChunkSize);
}

void TIntegrityManager::CollectReadResult(TExtentInfo& extent, ui32 offset, ui32 size,
        TOperationResult& result, bool touch) {
    const ui32 firstBlock = offset / IntegrityUnitSize;
    const ui32 endBlock = (offset + size) / IntegrityUnitSize;
    const ui32 numBlocks = endBlock - firstBlock;
    result.Checksums.reserve(numBlocks);
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
            if (result.Status != EOperationStatus::Ok) { continue; }
            const ui32 slot = block % ChecksumsPerIntegrityBlock;
            if (!used) {
                result.Checksums.push_back(GetZeroBlockChecksum());
            } else if (state && state->Known.Get(slot)) {
                result.Checksums.push_back(state->Checksums[slot]);
            } else {
                result.Status = EOperationStatus::Corrupted;
                result.ErrorReason = TStringBuilder() << "checksum is missing for used data block " << block;
                result.Checksums.clear();
            }
        }
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

TIntegrityManager::TOperation TIntegrityManager::StartWrite(TDataChunkKey key, ui32 offset, ui32 size,
        const std::vector<ui64>& checksums) {
    Y_ABORT_UNLESS(checksums.size() == size / IntegrityUnitSize);
    return StartOperation(key, offset, size, checksums);
}

TIntegrityManager::TOperation TIntegrityManager::StartOperation(TDataChunkKey key, ui32 offset, ui32 size,
        std::vector<ui64> checksums) {
    ValidateOperationRange(offset, size);
    auto completion = std::make_shared<TOperationState>();
    Host.Launch([this, key, offset, size, checksums = std::move(checksums), completion] {
        return RunOperation(Host.Actor(), *this, key, offset, size, checksums, completion);
    });
    return TOperation(std::move(completion));
}

NActors::async<void> TIntegrityManager::RunOperation(NActors::IActor& actor, TIntegrityManager& self,
        TDataChunkKey key, ui32 offset, ui32 size, std::vector<ui64> checksums,
        std::shared_ptr<TOperationState> completion) {
    Y_UNUSED(actor);
    TOperationResult result;
    auto finish = [&] {
        completion->Result.emplace(std::move(result));
        completion->Changed.NotifyAll();
    };
    auto it = self.Extents.find(key);
    if (it == self.Extents.end() || it->second.DeletionPending) {
        result.Status = EOperationStatus::Corrupted;
        result.ErrorReason = "integrity extent is missing for an allocated data chunk";
        finish();
        co_return;
    }
    const ui32 first = self.FirstPair(offset), end = self.EndPair(offset, size);
    for (ui32 idx = first; idx < end; ++idx) {
        if (it->second.Pairs.at(idx).Corrupted) {
            const auto& runtime = *it->second.PairRuntime.at(idx);
            result.Status = EOperationStatus::Corrupted;
            result.ErrorReason = runtime.CorruptionReason;
            result.LostWriteDetected = runtime.LostWriteCorruption;
            finish();
            co_return;
        }
    }
    // Pin the complete range before any eager child can load or evict a pair.
    for (ui32 idx = first; idx < end; ++idx) { ++it->second.Pairs.at(idx).OperationPins; }
    Y_DEFER {
        auto& extent = self.Extents.at(key);
        for (ui32 idx = first; idx < end; ++idx) {
            --extent.Pairs.at(idx).OperationPins;
            self.MaybeDropPairRuntime(extent, idx);
        }
        self.EvictBlockStatesOverBudget();
    };
    std::vector<std::shared_ptr<TPairRuntime>> runtimes;
    runtimes.reserve(end - first);
    std::vector<TPairRead> reads;
    for (ui32 idx = first; idx < end; ++idx) {
        auto& extent = self.Extents.at(key);
        auto runtime = self.GetPairRuntime(extent, idx);
        runtimes.push_back(runtime);
        const auto& pair = extent.Pairs.at(idx);
        if (!self.Stopped && !pair.Resident && !pair.Corrupted && !runtime->Loading && !runtime->Failed) {
            reads.push_back(self.ClaimPairRead(key, extent, idx, runtime));
        } else if (pair.Resident) {
            self.FindBlockState(extent, idx);
        }
    }
    if (!reads.empty()) { self.Host.SubmitPairReads(std::move(reads)); }
    // Join every submitted load, including siblings of a failed/corrupted load.
    for (const auto& runtime : runtimes) {
        while (runtime->Loading) { co_await runtime->Changed.Wait(); }
    }
    for (ui32 idx = first; idx < end; ++idx) {
        const auto& runtime = runtimes[idx - first];
        if (self.Extents.at(key).Pairs.at(idx).Corrupted) {
            result.Status = EOperationStatus::Corrupted;
            result.ErrorReason = runtime->CorruptionReason;
            result.LostWriteDetected = runtime->LostWriteCorruption;
            finish();
            co_return;
        }
        if (runtime->Failed) { result.Status = EOperationStatus::Failed; }
    }
    if (self.Stopped || result.Status == EOperationStatus::Failed) {
        result.Status = EOperationStatus::Failed;
        finish();
        co_return;
    }
    // Apply the entire mutation and record all required versions without suspension.
    auto& extent = self.Extents.at(key);
    for (ui32 block = offset / IntegrityUnitSize; block < (offset + size) / IntegrityUnitSize; ++block) {
        extent.UsedBlocks.Set(block);
        const ui32 idx = block / ChecksumsPerIntegrityBlock, slot = block % ChecksumsPerIntegrityBlock;
        auto& state = self.GetOrCreateBlockState(key, extent, idx);
        auto& pair = extent.Pairs.at(idx);
        const ui64 checksum = checksums[block - offset / IntegrityUnitSize];
        if (state.Known.Get(slot)) {
            UpdateRoot(pair.Digest, extent.Ref.VChunkGeneration, block, state.Checksums[slot], checksum);
        } else {
            pair.Digest ^= Contribution(extent.Ref.VChunkGeneration, block, checksum);
            state.Known.Set(slot);
        }
        state.Checksums[slot] = checksum;
        pair.DigestKnown = true;
    }
    std::vector<ui64> versions;
    for (auto& runtime : runtimes) {
        versions.push_back(++runtime->MutationVersion);
        runtime->Dirty = true;
    }
    for (ui32 idx = first; idx < end; ++idx) {
        auto runtime = runtimes[idx - first];
        if (!runtime->Flushing) {
            runtime->Flushing = true;
            self.Host.Launch([&self, key, idx, runtime] {
                return FlushPair(self.Host.Actor(), self, key, idx, runtime);
            });
        }
    }
    self.EvictBlockStatesOverBudget();
    for (size_t i = 0; i < runtimes.size(); ++i) {
        auto& runtime = runtimes[i];
        while (runtime->DurableVersion < versions[i] && !runtime->Failed
                && (!self.Stopped || runtime->Flushing)) {
            co_await runtime->Changed.Wait();
        }
        if (runtime->DurableVersion < versions[i]) { result.Status = EOperationStatus::Failed; }
    }
    finish();
}

} // namespace NKikimr::NDDisk
