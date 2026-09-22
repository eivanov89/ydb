#include "ddisk_actor.h"
#include "direct_io_op.h"
#include <ydb/library/actors/async/async.h>
#include <ydb/library/actors/async/wait_for_event.h>

#include <algorithm>
#include <util/generic/overloaded.h>
#include <util/generic/scope.h>
#include <ydb/core/protos/blobstorage_ddisk_internal.pb.h>
#include <ydb/core/util/stlog.h>
#include <ydb/library/actors/core/interconnect.h>

#define YDB_LOG_THIS_FILE_COMPONENT BS_DDISK

namespace NKikimr::NDDisk {

    void TDDiskActor::LaunchIntegrity(std::function<NActors::async<void>()> factory) {
        co_await factory();
        if (!Stopping && !IsBroken()) { RunIntegrityReclamation(); }
    }

    NActors::async<TChunkIdx> TDDiskActor::AllocateIntegrityChunk() {
        if (Stopping || IsBroken()) { co_return 0; }
        co_return co_await NActors::WithAsyncContinuation<TChunkIdx>([this](auto continuation) {
            IntegrityAllocations.push_back(std::move(continuation));
            ChunkManager.Enqueue(TChunkForIntegrity{});
            HandleChunkReserved();
        });
    }

    NActors::async<TIntegrityManager::TIoResult> TDDiskActor::ReadIntegrity(
            TChunkIdx chunk, ui32 offset, ui32 size) {
        if (Stopping || IsBroken()) { co_return TIntegrityManager::TIoResult{}; }
        Counters.Checksums.IntegrityPairReads->Inc();
        const ui64 cookie = NActors::AllocateWaitCookie();
        std::unique_ptr<TDirectIoOpBase> op = AllocateOp<TIntegrityIoOp>();
        op->SetCompletionCookie(cookie);
        op->PrepareRead(size, DiskFormat->Offset(chunk, 0, offset), chunk, offset);
        DirectUringOp(op);
        auto event = co_await NActors::ActorWaitForEvent<TEvPrivate::TEvIntegrityIoResult>(cookie);
        auto& msg = *event->Get();
        const bool ok = msg.Status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
        if (!ok && !Stopping) { EnterBroken(msg.ErrorMessage); }
        co_return TIntegrityManager::TIoResult{ok && !IsBroken(), std::move(msg.Data)};
    }

    NActors::async<bool> TDDiskActor::WriteIntegrity(TChunkIdx chunk, ui32 offset, TRcBuf data,
            TIntegrityManager::EWriteIoKind kind) {
        if (Stopping || IsBroken()) { co_return false; }
        if (kind == TIntegrityManager::EWriteIoKind::Pair) { Counters.Checksums.IntegrityPairWrites->Inc(); }
        const ui64 cookie = NActors::AllocateWaitCookie();
        std::unique_ptr<TDirectIoOpBase> op = AllocateOp<TIntegrityIoOp>();
        op->SetCompletionCookie(cookie);
        op->PrepareWrite(TRope(std::move(data)), DiskFormat->Offset(chunk, 0, offset), chunk, offset);
        DirectUringOp(op);
        auto event = co_await NActors::ActorWaitForEvent<TEvPrivate::TEvIntegrityIoResult>(cookie);
        auto& msg = *event->Get();
        const bool ok = msg.Status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
        if (!ok && !Stopping) { EnterBroken(msg.ErrorMessage); }
        co_return ok && !IsBroken();
    }

    void TDDiskActor::CountIntegrityResult(const TIntegrityManager::TOperationResult& result) {
        if (result.Status == TIntegrityManager::EOperationStatus::Corrupted) {
            Counters.Checksums.IntegrityCorruption->Inc();
            if (result.LostWriteDetected) { Counters.Checksums.IntegrityLostWriteDetected->Inc(); }
        }
    }

    NActors::async<bool> TDDiskActor::WaitForChunkCommit(ui64 tabletId, ui64 vChunkIndex) {
        auto& chunk = ChunkRefs[tabletId][vChunkIndex];
        ++chunk.AllocationWaiters;
        Y_DEFER { --chunk.AllocationWaiters; };
        while (DataChunkAllocationsInFlight.contains({tabletId, vChunkIndex}) && !Stopping && !IsBroken()) {
            co_await chunk.CommitReady.Wait();
        }
        co_return !IsBroken() && !DataChunkAllocationsInFlight.contains({tabletId, vChunkIndex});
    }

    NActors::async<TDDiskActor::TEvPrivate::TEvDDiskIoResult::TPtr> TDDiskActor::AwaitDataIo(
            std::unique_ptr<TDirectIoOpBase> op, ui64 tabletId, ui64 vChunkIndex) {
        auto& chunk = ChunkRefs.at(tabletId).at(vChunkIndex);
        ++chunk.InFlightDataIo;
        Y_DEFER { --chunk.InFlightDataIo; };
        const ui64 cookie = NActors::AllocateWaitCookie();
        op->SetCompletionCookie(cookie);
        DirectUringOp(op);
        co_return co_await NActors::ActorWaitForEvent<TEvPrivate::TEvDDiskIoResult>(cookie);
    }

    NActors::async<TDDiskActor::TEvPrivate::TEvInternalSyncWriteResult::TPtr> TDDiskActor::AwaitSyncIo(
            std::unique_ptr<TDirectIoOpBase> op, ui64 tabletId, ui64 vChunkIndex) {
        auto& chunk = ChunkRefs.at(tabletId).at(vChunkIndex);
        ++chunk.InFlightDataIo;
        Y_DEFER { --chunk.InFlightDataIo; };
        const ui64 cookie = NActors::AllocateWaitCookie();
        op->SetCompletionCookie(cookie);
        DirectUringOp(op);
        co_return co_await NActors::ActorWaitForEvent<TEvPrivate::TEvInternalSyncWriteResult>(cookie);
    }

    void TDDiskActor::IssueChunkAllocation(ui64 tabletId, ui64 vChunkIndex) {
        if (Stopping || Y_UNLIKELY(IsBroken())) {
            return;
        }
        ChunkRefs.at(tabletId).at(vChunkIndex).AllocationPending = true;
        ChunkManager.Enqueue(TChunkForData{tabletId, vChunkIndex});
        HandleChunkReserved();
    }

    NActors::async<void> TDDiskActor::WaitForChunk(ui64 tabletId, ui64 vChunkIndex, bool allocate) {
        // THashMap keeps references stable; deletion is forbidden while a waiter owns this
        // entry. The guard also releases the pin when forced teardown destroys the frame.
        TChunkRef& chunkRef = ChunkRefs[tabletId][vChunkIndex];
        ++chunkRef.AllocationWaiters;
        Y_DEFER { --chunkRef.AllocationWaiters; };

        if (allocate && !chunkRef.ChunkIdx && !chunkRef.AllocationPending) {
            IssueChunkAllocation(tabletId, vChunkIndex);
        }
        // Allocation may complete synchronously from the reserve. Notifications are not
        // sticky, so always check the state before subscribing.
        while (chunkRef.AllocationPending && !Stopping && !IsBroken()) {
            co_await chunkRef.AllocationReady.Wait();
        }
        // Check while the entry is still pinned, before callers re-resolve their request state.
        Y_DEBUG_ABORT_UNLESS(!allocate || chunkRef.ChunkIdx || Stopping || IsBroken());
    }

    void TDDiskActor::Handle(TEvPrivate::TEvIssuePersistentBufferChunkAllocation::TPtr ev) {
        if (!CanHandleQuery(ev)) {
            return;
        }
        if (!IssuePersistentBufferChunkAllocationInflight) {
            IssuePersistentBufferChunkAllocationInflight = true;
            ChunkManager.Enqueue(TChunkForPersistentBuffer{});
            HandleChunkReserved();
        }
    }

    void TDDiskActor::Handle(TEvPrivate::TEvDeallocatePersistentBufferChunk::TPtr ev) {
        auto chunkIdx = ev->Get()->ChunkIdx;
        auto it = std::find(PersistentBufferChunks.begin(), PersistentBufferChunks.end(), chunkIdx);
        Y_DEBUG_ABORT_UNLESS(it != PersistentBufferChunks.end());
        PersistentBufferChunks.erase(it);
        const ui64 lsn = IssuePDiskLogRecord(TLogSignature::SignaturePersistentBufferChunkMap, 0,
            CreatePersistentBufferChunkMapSnapshot(), &PersistentBufferChunkMapSnapshotLsn, {chunkIdx});
        if (co_await WaitForLog(lsn)) {
            Send(PersistentBufferActorId, new TEvPrivate::TEvDeallocatePersistentBufferChunkResult(chunkIdx));
            --*Counters.Chunks.ChunksOwned;
        }
    }

    void TDDiskActor::ReserveChunks(size_t count) {
        YDB_LOG_DEBUG("TDDiskActor::ReserveChunks requesting chunk reserve",
            {"marker", "BSDD28"},
            {"DDiskId", DDiskId},
            {"chunkReserveSize", ChunkManager.GetReservedChunkCount()},
            {"minChunksReserved", MinChunksReserved},
            {"formattingChunks", FormattingChunks.size()},
            {"requestCount", count});
        ChunkManager.BeginReservation();
        const ui64 cookie = NActors::AllocateWaitCookie();
        auto request = std::make_unique<NPDisk::TEvChunkReserve>(PDiskParams->Owner,
            PDiskParams->OwnerRound, count);
        request->IsDDisk = true;
        Send(BaseInfo.PDiskActorID, request.release(), IEventHandle::FlagTrackDelivery, cookie);
        auto event = co_await NActors::ActorWaitForEvent<IEventHandle>(cookie);
        ChunkManager.FinishReservation();
        if (event->GetTypeRewrite() == TEvents::TEvUndelivered::EventType) {
            BeginStopping("PDisk reserve request was not delivered");
            TryCompleteStop();
            co_return;
        }
        Y_ABORT_UNLESS(event->GetTypeRewrite() == NPDisk::TEvChunkReserveResult::EventType);
        const auto& msg = *event->Get<NPDisk::TEvChunkReserveResult>();
        YDB_LOG_DEBUG("TDDiskActor::ReserveChunks received reserve result",
            {"marker", "BSDD04"},
            {"DDiskId", DDiskId},
            {"msg", msg});
        if (Stopping) {
            if (msg.Status == NKikimrProto::OK) {
                for (TChunkIdx chunk : msg.ChunkIds) {
                    ChunkManager.ReturnChunk(chunk);
                }
                if (OwnDrainFinishing) {
                    ReleaseUncommittedChunks();
                }
            }
            TryCompleteStop();
            co_return;
        }
        if (!CheckPDiskReply(msg.Status, msg.ErrorReason, "ReserveChunks")) {
            co_return;
        }
        for (TChunkIdx chunk : msg.ChunkIds) {
            if (Config.EnableChecksums || IsBroken()) {
                ChunkManager.ReturnChunk(chunk);
            } else {
                Y_ABORT_UNLESS(FormattingChunks.try_emplace(chunk, 0).second);
                FormatChunk(chunk);
            }
        }
        HandleChunkReserved();
    }

    void TDDiskActor::ReleaseUncommittedChunks() {
        if (IsPersistentBufferActor || !PDiskParams || !LogReplayComplete) {
            return;
        }
        Y_ABORT_UNLESS(Stopping && !GetDirectIoInflight());

        TVector<TChunkIdx> chunks = ChunkManager.ExtractReservations();
        for (const auto& [chunkIdx, _] : FormattingChunks) {
            chunks.push_back(chunkIdx);
        }
        FormattingChunks.clear();
        chunks.insert(chunks.end(), PendingChunkRelease.begin(), PendingChunkRelease.end());
        PendingChunkRelease.clear();
        for (const auto& [_, allocation] : DataChunkAllocationsInFlight) {
            // A submitted commit can still succeed after this actor stops.
            if (!allocation.LogIssued) {
                chunks.push_back(allocation.ChunkIdx);
            }
        }
        if (IntegrityManager) {
            for (const TChunkIdx chunkIdx : IntegrityManager->GetIntegrityChunkIdxs()) {
                if (!IsIntegrityChunkCommitted(chunkIdx)) {
                    chunks.push_back(chunkIdx);
                }
            }
        }
        std::sort(chunks.begin(), chunks.end());
        chunks.erase(std::unique(chunks.begin(), chunks.end()), chunks.end());
        // PDisk validates the entire batch: repeating an already forgotten ID
        // would reject fresh reservations in the same request as well.
        std::erase_if(chunks, [this](TChunkIdx chunkIdx) {
            return !ShutdownChunkReleasesIssued.insert(chunkIdx).second;
        });
        if (!chunks.empty()) {
            YDB_LOG_NOTICE("DDisk releasing uncommitted reservations", {"DDiskId", DDiskId}, {"chunks", chunks});
            auto request = std::make_unique<NPDisk::TEvChunkForget>(
                PDiskParams->Owner, PDiskParams->OwnerRound, std::move(chunks));
            request->IsDDisk = true;
            Send(BaseInfo.PDiskActorID, request.release());
        }
    }

    void TDDiskActor::FormatChunk(TChunkIdx chunkIdx) {
        static constexpr ui32 FormatSliceSize = 16u << 20;
        Y_ABORT_UNLESS(!Config.EnableChecksums && DiskFormat->ChunkSize <= Max<ui32>());
        for (ui32 offset = 0; offset < DiskFormat->ChunkSize;) {
            if (Stopping || IsBroken()) {
                break;
            }
            const ui32 size = Min(FormatSliceSize, static_cast<ui32>(DiskFormat->ChunkSize) - offset);
            auto zero = TRcBuf::UninitializedPageAligned(size);
            memset(zero.GetDataMut(), 0, size);
            std::unique_ptr<TDirectIoOpBase> op = std::make_unique<TChunkFormatIoOp>(*this);
            op->Reinit();
            static_cast<TChunkFormatIoOp*>(op.get())->SetFormatRange(chunkIdx, offset, size);
            op->PrepareWrite(TRope(std::move(zero)), DiskFormat->Offset(chunkIdx, 0, offset), chunkIdx, offset);
            DirectUringOp(op);
            // The completion machinery owns both the op and its buffer through retirement.
            auto event = co_await NActors::ActorWaitForEvent<TEvPrivate::TEvChunkFormatIoResult>(chunkIdx);
            const auto& msg = *event->Get();
            if (Stopping || IsBroken()) {
                break;
            }
            Y_ABORT_UNLESS(msg.ChunkIdx == chunkIdx && msg.OffsetInBytes == offset && msg.Size == size);
            if (msg.Status != NKikimrBlobStorage::NDDisk::TReplyStatus::OK) {
                EnterBroken(TStringBuilder() << "failed to zero-format newly reserved chunk " << chunkIdx
                    << " at offset " << offset << ": " << msg.ErrorMessage);
                break;
            }
            offset += size;
            FormattingChunks.at(chunkIdx) = offset;
            if (offset == DiskFormat->ChunkSize) {
                FormattingChunks.erase(chunkIdx);
                ChunkManager.ReturnChunk(chunkIdx);
                HandleChunkReserved();
                co_return;
            }
        }
        PendingChunkRelease.insert(chunkIdx);
        FormattingChunks.erase(chunkIdx);
        HandleChunkReserved();
    }

    void TDDiskActor::HandleChunkReserved() {
        if (Stopping) {
            return;
        }
        Y_ABORT_UNLESS(!IsPersistentBufferActor);
        while (auto allocation = ChunkManager.TakeAllocation()) {
            const auto& [chunkAllocate, chunkIdx] = *allocation;
            AllocateChunk(chunkAllocate, chunkIdx);
            // Chunk-map increments (data and integrity alike) need a snapshot starting point to
            // replay from.
            if (!std::holds_alternative<TChunkForPersistentBuffer>(chunkAllocate)
                    && ChunkMapSnapshotLsn == Max<ui64>()) {
                IssuePDiskLogRecord(TLogSignature::SignatureDDiskChunkMap, 0, CreateChunkMapSnapshot(),
                    &ChunkMapSnapshotLsn);
            }
        }
        const size_t count = IsBroken()
            ? ChunkManager.GetRefillCount(ChunkManager.CountPendingPersistentBufferAllocations())
            : ChunkManager.GetRefillCount(MinChunksReserved, FormattingChunks.size());
        if (count) {
            ReserveChunks(count);
        }
    }

    void TDDiskActor::AllocateChunk(TChunkManager::TAllocation allocation, TChunkIdx chunkIdx) {
        if (const auto* data = std::get_if<TChunkForData>(&allocation)) {
            co_await AllocateDataChunk(data->TabletId, data->VChunkIndex, chunkIdx);
        } else if (std::holds_alternative<TChunkForIntegrity>(allocation)) {
            Y_ABORT_UNLESS(!IntegrityAllocations.empty());
            auto continuation = std::move(IntegrityAllocations.front());
            IntegrityAllocations.pop_front();
            if (continuation) { continuation.Resume(chunkIdx); }
            else { ChunkManager.ReturnChunk(chunkIdx); }
        } else {
            Y_DEBUG_ABORT_UNLESS(std::find(PersistentBufferChunks.begin(),
                PersistentBufferChunks.end(), chunkIdx) == PersistentBufferChunks.end());
            PersistentBufferChunks.emplace_back(chunkIdx);
            const ui64 lsn = IssuePDiskLogRecord(TLogSignature::SignaturePersistentBufferChunkMap,
                chunkIdx, CreatePersistentBufferChunkMapSnapshot(), &PersistentBufferChunkMapSnapshotLsn);
            if (co_await WaitForLog(lsn)) {
                IssuePersistentBufferChunkAllocationInflight = false;
                Send(PersistentBufferActorId, new TEvPrivate::TEvHandlePersistentBufferEventForChunk(chunkIdx));
                ++*Counters.Chunks.ChunksOwned;
            }
        }
    }

    NActors::async<void> TDDiskActor::AllocateDataChunk(ui64 tabletId, ui64 vChunkIndex, TChunkIdx chunkIdx) {
        const bool inserted = DataChunkAllocationsInFlight.try_emplace(
            std::make_pair(tabletId, vChunkIndex), TDataChunkAllocationInFlight{.ChunkIdx = chunkIdx}).second;
        Y_ABORT_UNLESS(inserted);
        // Allocation pins this node through placement and commit; teardown only unlinks the pin.
        auto& chunk = ChunkRefs.at(tabletId).at(vChunkIndex);
        ++chunk.AllocationWaiters;
        Y_DEFER { --chunk.AllocationWaiters; };
        if (Config.EnableChecksums) {
            auto extent = IntegrityManager->StartExtent({tabletId, vChunkIndex}, chunkIdx);
            if (co_await extent.WaitPlaced(*this)) {
                chunk.ChunkIdx = chunkIdx;
                chunk.AllocationPending = false;
                chunk.AllocationReady.NotifyAll();
            }
            co_await extent.WaitReady(*this);
        } else {
            chunk.ChunkIdx = chunkIdx;
            chunk.AllocationPending = false;
            chunk.AllocationReady.NotifyAll();
        }
        if (!Stopping && !IsBroken() && (co_await CommitDataChunk(tabletId, vChunkIndex))) {
            CompleteDataChunkAllocation(tabletId, vChunkIndex);
        }
    }

    bool TDDiskActor::IsIntegrityChunkCommitted(TChunkIdx chunkIdx) const {
        return std::any_of(CommittedIntegrityChunks.begin(), CommittedIntegrityChunks.end(),
            [chunkIdx](const auto& entry) { return entry.ChunkIdx == chunkIdx; });
    }

    void TDDiskActor::RunIntegrityReclamation() {
        co_await ReclaimUnusedIntegrityChunks();
    }

    NActors::async<bool> TDDiskActor::ReclaimUnusedIntegrityChunks() {
        if (Stopping) {
            co_return false;
        }
        if (!Config.EnableChecksums) {
            co_return true;
        }
        Y_ABORT_UNLESS(Config.EnableChecksums && IntegrityManager);
        if (Y_UNLIKELY(IsBroken())) {
            co_return false;
        }

        const auto releasableChunks = IntegrityManager->TakeReleasableIntegrityChunks();

        TVector<TChunkIdx> chunksToDelete;
        for (const TChunkIdx chunkIdx : releasableChunks) {
            if (IsIntegrityChunkCommitted(chunkIdx)) {
                const size_t erased = std::erase_if(CommittedIntegrityChunks, [chunkIdx](const auto& entry) {
                    return entry.ChunkIdx == chunkIdx;
                });
                Y_ABORT_UNLESS(erased == 1);
                chunksToDelete.push_back(chunkIdx);
            } else {
                ChunkManager.ReturnChunk(chunkIdx);
            }
        }

        if (chunksToDelete.empty()) {
            co_return true;
        }

        *Counters.Chunks.ChunksOwned -= chunksToDelete.size();
        const ui64 lsn = IssuePDiskLogRecord(TLogSignature::SignatureDDiskChunkMap, TChunkIdx(0), CreateChunkMapSnapshot(),
            &ChunkMapSnapshotLsn, std::move(chunksToDelete));
        co_return co_await WaitForLog(lsn);
    }

    NActors::async<bool> TDDiskActor::CommitDataChunk(ui64 tabletId, ui64 vChunkIndex) {
        if (Stopping || Y_UNLIKELY(IsBroken())) {
            co_return false;
        }

        const auto it = DataChunkAllocationsInFlight.find({tabletId, vChunkIndex});
        Y_ABORT_UNLESS(it != DataChunkAllocationsInFlight.end());
        auto& allocation = it->second;
        if (allocation.LogIssued) {
            co_return false;
        }

        allocation.LogIssued = true;
        const TChunkIdx chunkIdx = allocation.ChunkIdx;
        ChunkMapIncrementsInFlight.emplace(tabletId, vChunkIndex, chunkIdx);

        TVector<TChunkIdx> commitChunks;
        const TIntegrityManager::TMappingSnapshot::TIntegrityChunkEntry* integrityChunk = nullptr;
        TIntegrityManager::TMappingSnapshot::TIntegrityChunkEntry integrityEntry;
        const TIntegrityManager::TExtentRef* ref = nullptr;
        if (Config.EnableChecksums) {
            Y_ABORT_UNLESS(IntegrityManager);
            Y_ABORT_UNLESS(IntegrityManager->IsExtentReady({tabletId, vChunkIndex}));
            ref = IntegrityManager->FindExtentRef({tabletId, vChunkIndex});
            Y_ABORT_UNLESS(ref);
            if (!IsIntegrityChunkCommitted(ref->IntegrityChunkIdx)) {
                integrityEntry = {
                    .ChunkIdx = ref->IntegrityChunkIdx,
                    .Generation = IntegrityManager->GetIntegrityChunkGeneration(ref->IntegrityChunkIdx),
                };
                CommittedIntegrityChunks.push_back(integrityEntry);
                integrityChunk = &CommittedIntegrityChunks.back();
                commitChunks.push_back(ref->IntegrityChunkIdx);
            }
        }
        commitChunks.push_back(chunkIdx);
        allocation.NewlyCommittedChunks = commitChunks.size();

        const ui64 lsn = IssuePDiskLogRecord(TLogSignature::SignatureDDiskChunkMap, std::move(commitChunks),
            CreateChunkMapIncrement(tabletId, vChunkIndex, chunkIdx, ref, integrityChunk),
            nullptr);
        co_return co_await WaitForLog(lsn);
    }

    void TDDiskActor::CompleteDataChunkAllocation(ui64 tabletId, ui64 vChunkIndex) {
        if (Y_UNLIKELY(IsBroken())) {
            return;
        }

        const auto it = DataChunkAllocationsInFlight.find({tabletId, vChunkIndex});
        Y_ABORT_UNLESS(it != DataChunkAllocationsInFlight.end());
        auto allocation = std::move(it->second);
        DataChunkAllocationsInFlight.erase(it);

        TChunkRef& chunkRef = ChunkRefs[tabletId][vChunkIndex];
        Y_ABORT_UNLESS(chunkRef.ChunkIdx == allocation.ChunkIdx);

        const size_t numErased = ChunkMapIncrementsInFlight.erase({tabletId, vChunkIndex, allocation.ChunkIdx});
        Y_ABORT_UNLESS(numErased == 1);
        *Counters.Chunks.ChunksOwned += allocation.NewlyCommittedChunks;

        chunkRef.CommitReady.NotifyAll();
    }

    NActors::async<bool> TDDiskActor::AcquireIntegrityExtent(ui64 tabletId, ui64 vChunkIndex) {
        auto& chunk = ChunkRefs.at(tabletId).at(vChunkIndex);
        const ui64 ticket = NextCookie++;
        auto pos = chunk.ExtentWaiters.insert(chunk.ExtentWaiters.end(), ticket);
        // Erasing a ticket is also safe during forced frame destruction.
        Y_DEFER { chunk.ExtentWaiters.erase(pos); };
        while (!Stopping && !IsBroken()
                && (chunk.IntegrityExtentWriteInFlight || chunk.ExtentWaiters.front() != ticket)) {
            co_await chunk.ExtentAvailable.Wait();
        }
        if (Stopping || IsBroken()) {
            co_return false;
        }
        chunk.IntegrityExtentWriteInFlight = true;
        co_return true;
    }

    void TDDiskActor::ReleaseIntegrityExtentWrite(ui64 tabletId, ui64 vChunkIndex) {
        auto& chunk = ChunkRefs.at(tabletId).at(vChunkIndex);
        Y_ABORT_UNLESS(chunk.IntegrityExtentWriteInFlight);
        chunk.IntegrityExtentWriteInFlight = false;
        chunk.ExtentAvailable.NotifyAll();
    }

    void TDDiskActor::Handle(NPDisk::TEvCutLog::TPtr ev) {
        auto& msg = *ev->Get();
        YDB_LOG_DEBUG("TDDiskActor::Handle(TEvCutLog)",
            {"marker", "BSDD06"},
            {"DDiskId", DDiskId},
            {"msg", msg});

        ++*Counters.RecoveryLog.CutLogMessages;

        // YardInit installs the CutLog recipient before chunk-map replay is complete. Until
        // ApplyMappingSnapshot runs, ChunkRefs may already contain restored data chunks while the
        // integrity manager is still empty, so a snapshot here would either abort or omit replayed
        // mappings. Coalesce early requests and process the strongest one after recovery.
        if (!LogReplayComplete) {
            DeferredCutLogFreeUpToLsn = Max(DeferredCutLogFreeUpToLsn.value_or(0), msg.FreeUpToLsn);
            return;
        }

        ProcessCutLog(msg.FreeUpToLsn);
    }

    void TDDiskActor::ProcessCutLog(ui64 freeUpToLsn) {
        Y_ABORT_UNLESS(LogReplayComplete);

        if (!IsBroken() && ChunkMapSnapshotLsn < freeUpToLsn) { // we have to rewrite snapshot
            IssuePDiskLogRecord(TLogSignature::SignatureDDiskChunkMap, 0, CreateChunkMapSnapshot(), &ChunkMapSnapshotLsn);
        }
        if (PersistentBufferChunkMapSnapshotLsn < freeUpToLsn) { // we have to rewrite snapshot
            IssuePDiskLogRecord(TLogSignature::SignaturePersistentBufferChunkMap, 0, CreatePersistentBufferChunkMapSnapshot(), &PersistentBufferChunkMapSnapshotLsn);
        }
    }

    NKikimrBlobStorage::NDDisk::NInternal::TPersistentBufferChunkMapLogRecord TDDiskActor::CreatePersistentBufferChunkMapSnapshot() {
        NKikimrBlobStorage::NDDisk::NInternal::TPersistentBufferChunkMapLogRecord record;
        for (const ui32 chunkIdx : PersistentBufferChunks) {
            record.AddChunkIdxs(chunkIdx);
        }
        record.SetUniqueId(PersistentBufferUniqueId);
        Y_ABORT_UNLESS(PersistentBufferUniqueId != 0);
        return record;
    }

    NKikimrBlobStorage::NDDisk::NInternal::TChunkMapLogRecord TDDiskActor::CreateChunkMapSnapshot() {
        NKikimrBlobStorage::NDDisk::NInternal::TChunkMapLogRecord record;
        record.SetChecksumsDisabled(!Config.EnableChecksums);
        auto *snapshot = record.MutableSnapshot();

        const auto fillExtentRef = [this](auto *item, ui64 tabletId, ui64 vChunkIndex) {
            Y_ABORT_UNLESS(Config.EnableChecksums && IntegrityManager);
            // Non-null for every chunk with a log record: the extent is Ready by the time its
            // increment is issued, and refs survive until the chunk is deleted.
            const auto *ref = IntegrityManager->FindExtentRef({tabletId, vChunkIndex});
            Y_ABORT_UNLESS(ref);
            auto *extentRef = item->MutableExtentRef();
            extentRef->SetIntegrityChunkIdx(ref->IntegrityChunkIdx);
            extentRef->SetExtentSlot(ref->ExtentSlot);
            extentRef->SetVChunkGeneration(ref->VChunkGeneration);
        };

        for (const auto& [tabletId, chunks] : ChunkRefs) {
            auto *tabletRecord = snapshot->AddTabletRecords();
            tabletRecord->SetTabletId(tabletId);

            for (const auto& [vChunkIndex, chunkRef] : chunks) {
                if (!chunkRef.ChunkIdx) {
                    continue;
                }
                if (DataChunkAllocationsInFlight.contains({tabletId, vChunkIndex})) {
                    // Not yet logged: issued increments are covered by ChunkMapIncrementsInFlight.
                    continue;
                }
                auto *item = tabletRecord->AddChunkRefs();
                item->SetVChunkIndex(vChunkIndex);
                item->SetChunkIdx(chunkRef.ChunkIdx);
                if (Config.EnableChecksums) {
                    fillExtentRef(item, tabletId, vChunkIndex);
                }
            }

            // check for increments in flight, they would have been committed by the time this entry gets read
            for (auto it = ChunkMapIncrementsInFlight.lower_bound({tabletId, 0, 0});
                    it != ChunkMapIncrementsInFlight.end() && std::get<0>(*it) == tabletId; ++it) {
                const auto& [tabletId, vChunkIndex, chunkIdx] = *it;
                auto *item = tabletRecord->AddChunkRefs();
                item->SetVChunkIndex(vChunkIndex);
                item->SetChunkIdx(chunkIdx);
                if (Config.EnableChecksums) {
                    fillExtentRef(item, tabletId, vChunkIndex);
                }
            }
        }

        if (Config.EnableChecksums) {
            for (const auto& entry : CommittedIntegrityChunks) {
                auto *chunk = snapshot->AddIntegrityChunks();
                chunk->SetChunkIdx(entry.ChunkIdx);
                chunk->SetGeneration(entry.Generation);
            }
            snapshot->SetGenerationCounter(IntegrityManager->GetGenerationCounter());
        }

        ++*Counters.RecoveryLog.NumChunkMapSnapshots;
        return record;
    }

    NKikimrBlobStorage::NDDisk::NInternal::TChunkMapLogRecord TDDiskActor::CreateChunkMapIncrement(ui64 tabletId,
            ui64 vChunkIndex, TChunkIdx chunkIdx, const TIntegrityManager::TExtentRef* extentRef,
            const TIntegrityManager::TMappingSnapshot::TIntegrityChunkEntry* integrityChunk) {
        NKikimrBlobStorage::NDDisk::NInternal::TChunkMapLogRecord record;
        record.SetChecksumsDisabled(!Config.EnableChecksums);
        auto *increment = record.MutableIncrement();
        if (integrityChunk) {
            auto *chunk = increment->MutableIntegrityChunk();
            chunk->SetChunkIdx(integrityChunk->ChunkIdx);
            chunk->SetGeneration(integrityChunk->Generation);
        }

        auto *data = increment->MutableDataChunk();
        data->SetTabletId(tabletId);
        data->SetVChunkIndex(vChunkIndex);
        data->SetChunkIdx(chunkIdx);

        if (extentRef) {
            auto *ref = data->MutableExtentRef();
            ref->SetIntegrityChunkIdx(extentRef->IntegrityChunkIdx);
            ref->SetExtentSlot(extentRef->ExtentSlot);
            ref->SetVChunkGeneration(extentRef->VChunkGeneration);
        }

        ++*Counters.RecoveryLog.NumChunkMapIncrements;
        return record;
    }

    void TDDiskActor::Handle(TEvDeleteTabletChunks::TPtr ev) {
        if (!CheckQuery(*ev, nullptr)) {
            co_return;
        }

        const TQueryCredentials creds(ev->Get()->Record.GetCredentials());
        const ui64 tabletId = creds.TabletId;

        YDB_LOG_DEBUG("TDDiskActor::Handle(TEvDeleteTabletChunks)",
            {"marker", "BSDD51"},
            {"DDiskId", DDiskId},
            {"tabletId", tabletId});

        if (TabletChunkDeletionsInFlight.contains(tabletId)) {
            SendReply(*ev, std::make_unique<TEvDeleteTabletChunksResult>(
                NKikimrBlobStorage::NDDisk::TReplyStatus::BUSY,
                "tablet chunk deletion is in flight"));
            co_return;
        }

        // Source reads and target writes of an in-flight sync may not have reached the target
        // chunk yet. Deleting now could free the physical chunk underneath a write or let a late
        // source result recreate the just-deleted mapping.
        for (const auto& [syncId, sync] : SyncsInFlight) {
            Y_UNUSED(syncId);
            if (sync->Creds.TabletId == tabletId) {
                SendReply(*ev, std::make_unique<TEvDeleteTabletChunksResult>(
                    NKikimrBlobStorage::NDDisk::TReplyStatus::BUSY,
                    "sync is in flight for tablet"));
                co_return;
            }
        }

        // Reject if any chunk allocation for this tablet is in flight (covers both allocations
        // whose increment log record is pending and those still waiting for an extent ref).
        for (const auto& [key, allocation] : DataChunkAllocationsInFlight) {
            Y_UNUSED(allocation);
            if (key.first == tabletId) {
                SendReply(*ev, std::make_unique<TEvDeleteTabletChunksResult>(
                    NKikimrBlobStorage::NDDisk::TReplyStatus::BUSY,
                    "chunk allocation is in flight for tablet"));
                co_return;
            }
        }

        if (Config.EnableChecksums && IntegrityManager->HasInFlightOperationsForTablet(tabletId)) {
            SendReply(*ev, std::make_unique<TEvDeleteTabletChunksResult>(
                NKikimrBlobStorage::NDDisk::TReplyStatus::BUSY,
                "integrity I/O is in flight for tablet"));
            co_return;
        }

        const auto tabletIt = ChunkRefs.find(tabletId);

        if (tabletIt == ChunkRefs.end()) {
            // tablet has no chunks
            SendReply(*ev, std::make_unique<TEvDeleteTabletChunksResult>(NKikimrBlobStorage::NDDisk::TReplyStatus::OK));
            co_return;
        }

        // Allocation waiters pin their chunk entry, including before a physical chunk is
        // reserved and after readiness has scheduled their coroutine continuations.
        for (const auto& [vChunkIndex, chunkRef] : tabletIt->second) {
            if (chunkRef.AllocationPending || chunkRef.AllocationWaiters
                    || !chunkRef.ExtentWaiters.empty()
                    || chunkRef.IntegrityExtentWriteInFlight) {
                SendReply(*ev, std::make_unique<TEvDeleteTabletChunksResult>(
                    NKikimrBlobStorage::NDDisk::TReplyStatus::BUSY,
                    "chunk allocation or integrity-extent write is queued for tablet"));
                co_return;
            }
            if (chunkRef.InFlightDataIo) {
                SendReply(*ev, std::make_unique<TEvDeleteTabletChunksResult>(
                    NKikimrBlobStorage::NDDisk::TReplyStatus::BUSY,
                    "data chunk I/O is in flight for tablet"));
                co_return;
            }
        }

        // Collect physical data chunk IDs.
        TVector<TChunkIdx> chunksToDelete;
        for (const auto& [vChunkIndex, chunkRef] : tabletIt->second) {
            if (chunkRef.ChunkIdx) {
                chunksToDelete.push_back(chunkRef.ChunkIdx);
            }
        }

        if (chunksToDelete.empty()) {
            ChunkRefs.erase(tabletIt);
            SendReply(*ev, std::make_unique<TEvDeleteTabletChunksResult>(NKikimrBlobStorage::NDDisk::TReplyStatus::OK));
            co_return;
        }

        // Remove the logical mapping from the snapshot now, but quarantine the corresponding
        // integrity slots until this removal record commits. Formatting a reused slot earlier
        // could overwrite metadata that recovery still maps to this tablet after a crash.
        const bool inserted = TabletChunkDeletionsInFlight.insert(tabletId).second;
        Y_ABORT_UNLESS(inserted);
        if (Config.EnableChecksums) {
            IntegrityManager->PrepareTabletChunksDeletion(tabletId);
        }
        ChunkRefs.erase(tabletIt);

        *Counters.Chunks.ChunksOwned -= chunksToDelete.size();

        // Capture reply info before issuing the async log record
        const TActorId replyTo = ev->Sender;
        const ui64 replyCookie = ev->Cookie;
        const TActorId replySession = ev->InterconnectSession;
        const bool replyInserted = TabletChunkDeletionReplies.emplace(tabletId,
            TTabletChunkDeletionReply{
                .ReplyTo = replyTo,
                .Cookie = replyCookie,
                .InterconnectSession = replySession,
            }).second;
        Y_ABORT_UNLESS(replyInserted);

        // The first snapshot removes and deallocates only the data chunks. Integrity chunks stay
        // owned because their deleted extents are not reusable until this snapshot is durable.
        const ui64 lsn = IssuePDiskLogRecord(TLogSignature::SignatureDDiskChunkMap, 0,
            CreateChunkMapSnapshot(), &ChunkMapSnapshotLsn, std::move(chunksToDelete));
        if (!(co_await WaitForLog(lsn))) {
            co_return;
        }
        TabletChunkDeletionsInFlight.erase(tabletId);
        if (Config.EnableChecksums) {
            IntegrityManager->CommitTabletChunksDeletion(tabletId);
            if (!(co_await ReclaimUnusedIntegrityChunks())) {
                co_return;
            }
        }
        // Broken/Stopping consumes the registry and sends the terminal reply.
        if (TabletChunkDeletionReplies.erase(tabletId)) {
            // Session replacement cannot undo this already durable deletion.
            SendReply(*ev, std::make_unique<TEvDeleteTabletChunksResult>(
                NKikimrBlobStorage::NDDisk::TReplyStatus::OK));
        }
    }

} // NKikimr::NDDisk
