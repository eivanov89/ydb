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
        if (!Stopping && !IsBroken()) {
            RunIntegrityReclamation();
        }
    }

    void TDDiskActor::SubmitIntegrityAllocation(ui64 token) {
        if (Stopping || IsBroken()) {
            IntegrityManager->CompleteAllocation(token, 0); return;
        }
        IntegrityAllocations.push_back(token);
        ChunkManager.Enqueue(TChunkForIntegrity{token});
        HandleChunkReserved();
    }

    void TDDiskActor::SubmitIntegrityPairReads(std::vector<TIntegrityManager::TPairRead> reads) {
        if (reads.empty()) {
            return;
        }
        if (Stopping || IsBroken()) {
            std::vector<TIntegrityManager::TPairReadResult> results;
            for (const auto& read : reads) {
                results.push_back({read.Id, {}});
            }
            IntegrityManager->CompletePairReads(results);
            return;
        }
        std::vector<TReadPartsIoOp::TPart> parts;
        for (const auto& read : reads) {
            parts.push_back({read.Id, read.ChunkIdx, read.OffsetInBytes, read.Size,
                DiskFormat->Offset(read.ChunkIdx, 0, read.OffsetInBytes)});
        }
        *Counters.Checksums.IntegrityPairReads += reads.size();
        auto readOp = std::make_unique<TReadPartsIoOp>(*this);
        readOp->PrepareParts(parts);
        std::unique_ptr<TDirectIoOpBase> op = std::move(readOp);
        DirectUringOp(op);
    }

    void TDDiskActor::SubmitIntegrityWrite(ui64 id, TChunkIdx chunk, ui32 offset, TRcBuf data,
            TIntegrityManager::EWriteIoKind kind)
    {
        if (Stopping || IsBroken()) {
            IntegrityManager->CompleteWrite(id, false);
            return;
        }
        if (kind == TIntegrityManager::EWriteIoKind::Pair) {
            Counters.Checksums.IntegrityPairWrites->Inc();
        }
        const ui64 cookie = NActors::AllocateWaitCookie();
        IntegrityWriteCookies.emplace(cookie, id);
        std::unique_ptr<TDirectIoOpBase> op = AllocateOp<TIntegrityIoOp>();
        op->SetCompletionCookie(cookie);
        op->PrepareWrite(TRope(std::move(data)), DiskFormat->Offset(chunk, 0, offset), chunk, offset);
        DirectUringOp(op);
    }

    void TDDiskActor::Handle(TEvPrivate::TEvIntegrityIoResult::TPtr ev) {
        const auto it = IntegrityWriteCookies.find(ev->Cookie);
        if (it == IntegrityWriteCookies.end()) {
            return;
        }
        const auto id = it->second;
        IntegrityWriteCookies.erase(it);
        const bool ok = ev->Get()->Status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
        if (!ok && !Stopping) {
            EnterBroken(ev->Get()->ErrorMessage);
        }
        IntegrityManager->CompleteWrite(id, ok && !IsBroken());
        if (Stopping && !GetDirectIoInflight()) {
            FinishStopping();
        }
        if (!Stopping && !IsBroken()) {
            RunIntegrityReclamation();
        }
    }

    void TDDiskActor::CountIntegrityResult(const TIntegrityManager::TOperationResult& result) {
        if (result.Status == TIntegrityManager::EOperationStatus::Corrupted) {
            Counters.Checksums.IntegrityCorruption->Inc();
            if (result.LostWriteDetected) {
                Counters.Checksums.IntegrityLostWriteDetected->Inc();
            }
        }
    }

    bool TDDiskActor::IsChunkCommitted(ui64 tabletId, ui64 vChunkIndex) const {
        return !IsBroken() && !DataChunkAllocationsInFlight.contains({tabletId, vChunkIndex});
    }

    void TDDiskActor::IssueChunkAllocation(ui64 tabletId, ui64 vChunkIndex) {
        if (Stopping || Y_UNLIKELY(IsBroken())) {
            return;
        }
        ChunkRefs.at(tabletId).at(vChunkIndex).AllocationPending = true;
        const ui64 token = NextDataAllocationToken++;
        Y_ABORT_UNLESS(PendingDataAllocationTokens.emplace(std::make_pair(tabletId, vChunkIndex), token).second);
        ChunkManager.Enqueue(TChunkForData{tabletId, vChunkIndex, token});
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
        IssuePDiskLogRecord(TLogSignature::SignaturePersistentBufferChunkMap, 0,
            CreatePersistentBufferChunkMapSnapshot(), &PersistentBufferChunkMapSnapshotLsn,
            {chunkIdx}, TLogPersistentBufferDeallocated{chunkIdx});
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
        Y_ABORT_UNLESS(!Reservation);
        Reservation.emplace(TReservation{count, cookie});
        auto request = std::make_unique<NPDisk::TEvChunkReserve>(PDiskParams->Owner,
            PDiskParams->OwnerRound, count);
        request->IsDDisk = true;
        Send(BaseInfo.PDiskActorID, request.release(), IEventHandle::FlagTrackDelivery, cookie);
    }

    void TDDiskActor::Handle(NPDisk::TEvChunkReserveResult::TPtr ev) {
        if (!Reservation || Reservation->Cookie != ev->Cookie) {
            return;
        }
        Reservation.reset();
        ChunkManager.FinishReservation();
        const auto& msg = *ev->Get();
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
            return;
        }
        if (!CheckPDiskReply(msg.Status, msg.ErrorReason, "ReserveChunks")) {
            return;
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
        Y_ABORT_UNLESS(Stopping && !GetDirectIoInflight() && !ActiveIndexedReads
            && IntegrityWriteCookies.empty() && FormatSlices.empty());

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
        auto it = FormattingChunks.find(chunkIdx);
        if (it == FormattingChunks.end()) {
            return;
        }
        const ui32 offset = it->second;
        if (Stopping || IsBroken()) {
            PendingChunkRelease.insert(chunkIdx);
            FormattingChunks.erase(it);
            return;
        }
        Y_ABORT_UNLESS(offset < DiskFormat->ChunkSize);
        const ui32 size = Min(FormatSliceSize, static_cast<ui32>(DiskFormat->ChunkSize) - offset);
        auto zero = TRcBuf::UninitializedPageAligned(size);
        memset(zero.GetDataMut(), 0, size);
        const ui64 cookie = NActors::AllocateWaitCookie();
        Y_ABORT_UNLESS(FormatSlices.emplace(cookie, TFormatSlice{chunkIdx, offset, size}).second);
        std::unique_ptr<TDirectIoOpBase> op = std::make_unique<TChunkFormatIoOp>(*this);
        op->Reinit();
        op->SetCompletionCookie(cookie);
        static_cast<TChunkFormatIoOp*>(op.get())->SetFormatRange(chunkIdx, offset, size);
        op->PrepareWrite(TRope(std::move(zero)), DiskFormat->Offset(chunkIdx, 0, offset), chunkIdx, offset);
        DirectUringOp(op);
    }

    void TDDiskActor::Handle(TEvPrivate::TEvChunkFormatIoResult::TPtr ev) {
        auto it = FormatSlices.find(ev->Cookie);
        if (it == FormatSlices.end()) {
            return;
        }
        const auto slice = it->second;
        FormatSlices.erase(it);
        const auto& msg = *ev->Get();
        Y_ABORT_UNLESS(msg.ChunkIdx == slice.ChunkIdx && msg.OffsetInBytes == slice.Offset
            && msg.Size == slice.Size);
        if (Stopping || IsBroken() || msg.Status != NKikimrBlobStorage::NDDisk::TReplyStatus::OK) {
            PendingChunkRelease.insert(slice.ChunkIdx);
            FormattingChunks.erase(slice.ChunkIdx);
            if (msg.Status != NKikimrBlobStorage::NDDisk::TReplyStatus::OK && !Stopping && !IsBroken()) {
                EnterBroken(TStringBuilder() << "failed to zero-format newly reserved chunk " << slice.ChunkIdx
                    << " at offset " << slice.Offset << ": " << msg.ErrorMessage);
            }
        } else {
            const ui32 offset = slice.Offset + slice.Size;
            FormattingChunks.at(slice.ChunkIdx) = offset;
            if (offset == DiskFormat->ChunkSize) {
                FormattingChunks.erase(slice.ChunkIdx);
                ChunkManager.ReturnChunk(slice.ChunkIdx);
                HandleChunkReserved();
            } else {
                FormatChunk(slice.ChunkIdx);
            }
        }
        if (Stopping && !GetDirectIoInflight()) {
            FinishStopping();
        }
    }

    void TDDiskActor::HandleChunkReserved() {
        if (Stopping) {
            return;
        }
        if (HandlingChunkReserved) {
            ChunkReservedAgain = true;
            return;
        }
        HandlingChunkReserved = true;
        Y_DEFER { HandlingChunkReserved = false; };
        Y_ABORT_UNLESS(!IsPersistentBufferActor);
        if (IsBroken()) {
            // Broken retains PB service but must never hand a physical chunk to a canceled
            // data/integrity request while manager Stop is draining its callbacks.
            ChunkManager.RetainPersistentBufferAllocations();
        }
        do {
            ChunkReservedAgain = false;
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
        while (ChunkReservedAgain && !Stopping);
    }

    void TDDiskActor::AllocateChunk(TChunkManager::TAllocation allocation, TChunkIdx chunkIdx) {
        if (const auto* data = std::get_if<TChunkForData>(&allocation)) {
            AllocateDataChunk(data->TabletId, data->VChunkIndex, data->Token, chunkIdx);
        } else if (const auto* integrity = std::get_if<TChunkForIntegrity>(&allocation)) {
            Y_ABORT_UNLESS(!IntegrityAllocations.empty());
            const ui64 token = IntegrityAllocations.front();
            IntegrityAllocations.pop_front();
            Y_ABORT_UNLESS(token == integrity->Token);
            IntegrityManager->CompleteAllocation(token, chunkIdx);
        } else {
            Y_DEBUG_ABORT_UNLESS(std::find(PersistentBufferChunks.begin(),
                PersistentBufferChunks.end(), chunkIdx) == PersistentBufferChunks.end());
            PersistentBufferChunks.emplace_back(chunkIdx);
            IssuePDiskLogRecord(TLogSignature::SignaturePersistentBufferChunkMap,
                chunkIdx, CreatePersistentBufferChunkMapSnapshot(), &PersistentBufferChunkMapSnapshotLsn,
                {}, TLogPersistentBufferAllocated{chunkIdx});
        }
    }

    void TDDiskActor::AllocateDataChunk(ui64 tabletId, ui64 vChunkIndex, ui64 token, TChunkIdx chunkIdx) {
        const auto key = std::make_pair(tabletId, vChunkIndex);
        const auto pending = PendingDataAllocationTokens.find(key);
        if (pending == PendingDataAllocationTokens.end() || pending->second != token || Stopping || IsBroken()) {
            ChunkManager.ReturnChunk(chunkIdx);
            return;
        }
        PendingDataAllocationTokens.erase(pending);
        const bool inserted = DataChunkAllocationsInFlight.try_emplace(
            key, TDataChunkAllocationInFlight{.Token = token, .ChunkIdx = chunkIdx}).second;
        Y_ABORT_UNLESS(inserted);
        // Pin before starting extent: a synchronous placement callback may wake other actor work.
        auto& chunk = ChunkRefs.at(tabletId).at(vChunkIndex);
        ++chunk.AllocationWaiters;
        if (Config.EnableChecksums) {
            auto extent = IntegrityManager->StartExtent({tabletId, vChunkIndex}, chunkIdx);
            extent.SetProgressCallback([this, tabletId, vChunkIndex, token] {
                AdvanceDataChunkAllocation(tabletId, vChunkIndex, token);
            });
            if (auto it = DataChunkAllocationsInFlight.find(key);
                    it != DataChunkAllocationsInFlight.end() && it->second.Token == token) {
                it->second.Extent.emplace(std::move(extent));
            }
        } else {
            DataChunkAllocationsInFlight.at(key).Placed = true;
        }
        AdvanceDataChunkAllocation(tabletId, vChunkIndex, token);
    }

    void TDDiskActor::AdvanceDataChunkAllocation(ui64 tabletId, ui64 vChunkIndex, ui64 token) {
        const auto key = std::make_pair(tabletId, vChunkIndex);
        auto it = DataChunkAllocationsInFlight.find(key);
        if (it == DataChunkAllocationsInFlight.end() || it->second.Token != token) {
            return;
        }
        auto& allocation = it->second;
        if (Stopping || IsBroken()) {
            return;
        }
        if (allocation.Extent) {
            const auto placed = allocation.Extent->GetPlacedResult();
            if (placed && *placed) {
                allocation.Placed = true;
            }
            if (placed && !*placed) {
                return;
            }
        }
        if (allocation.Placed) {
            auto& chunk = ChunkRefs.at(tabletId).at(vChunkIndex);
            if (chunk.AllocationPending) {
                chunk.ChunkIdx = allocation.ChunkIdx;
                chunk.AllocationPending = false;
                chunk.AllocationReady.NotifyAll();
                QueuePendingWritesForChunk(tabletId, vChunkIndex);
                it = DataChunkAllocationsInFlight.find(key);
                if (it == DataChunkAllocationsInFlight.end() || it->second.Token != token) {
                    return;
                }
            }
        }
        if (it->second.Extent) {
            const auto ready = it->second.Extent->GetReadyResult();
            if (!ready || !*ready) {
                return;
            }
        }
        if (!it->second.LogIssued) {
            CommitDataChunk(tabletId, vChunkIndex, token);
        }
    }

    bool TDDiskActor::IsIntegrityChunkCommitted(TChunkIdx chunkIdx) const {
        return std::any_of(CommittedIntegrityChunks.begin(), CommittedIntegrityChunks.end(),
            [chunkIdx](const auto& entry) { return entry.ChunkIdx == chunkIdx; });
    }

    void TDDiskActor::RunIntegrityReclamation() {
        PrepareIntegrityReclamation();
    }

    NActors::async<bool> TDDiskActor::ReclaimUnusedIntegrityChunks() {
        const auto reclamation = PrepareIntegrityReclamation(true);
        co_return reclamation.Ok && (!reclamation.Ticket || (co_await WaitForLog(reclamation.Ticket)));
    }

    TDDiskActor::TIntegrityReclamation TDDiskActor::PrepareIntegrityReclamation(bool wait) {
        if (Stopping) {
            return {false, std::nullopt, {}};
        }
        if (!Config.EnableChecksums) {
            return {true, std::nullopt, {}};
        }
        Y_ABORT_UNLESS(Config.EnableChecksums && IntegrityManager);
        if (Y_UNLIKELY(IsBroken())) {
            return {false, std::nullopt, {}};
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
            return {true, std::nullopt, {}};
        }

        *Counters.Chunks.ChunksOwned -= chunksToDelete.size();
        auto ticket = wait ? std::make_shared<TLogTicket>() : nullptr;
        if (ticket) {
            ticket->IsDDisk = true;
        }
        const ui64 lsn = IssuePDiskLogRecord(TLogSignature::SignatureDDiskChunkMap, TChunkIdx(0), CreateChunkMapSnapshot(),
            &ChunkMapSnapshotLsn, std::move(chunksToDelete), TLogReclamationFinished{ticket});
        return {true, lsn, std::move(ticket)};
    }

    void TDDiskActor::CommitDataChunk(ui64 tabletId, ui64 vChunkIndex, ui64 token) {
        if (Stopping || Y_UNLIKELY(IsBroken())) {
            return;
        }

        const auto it = DataChunkAllocationsInFlight.find({tabletId, vChunkIndex});
        if (it == DataChunkAllocationsInFlight.end() || it->second.Token != token) {
            return;
        }
        auto& allocation = it->second;
        if (allocation.LogIssued) {
            return;
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

        IssuePDiskLogRecord(TLogSignature::SignatureDDiskChunkMap, std::move(commitChunks),
            CreateChunkMapIncrement(tabletId, vChunkIndex, chunkIdx, ref, integrityChunk),
            nullptr, {}, TLogDataAllocationCommitted{tabletId, vChunkIndex, token});
    }

    void TDDiskActor::CompleteDataChunkAllocation(ui64 tabletId, ui64 vChunkIndex, ui64 token) {
        if (Y_UNLIKELY(IsBroken())) {
            return;
        }

        const auto it = DataChunkAllocationsInFlight.find({tabletId, vChunkIndex});
        if (it == DataChunkAllocationsInFlight.end() || it->second.Token != token) {
            return;
        }
        auto allocation = std::move(it->second);
        DataChunkAllocationsInFlight.erase(it);
        if (allocation.Extent) {
            allocation.Extent->SetProgressCallback({});
        }

        TChunkRef& chunkRef = ChunkRefs[tabletId][vChunkIndex];
        Y_ABORT_UNLESS(chunkRef.ChunkIdx == allocation.ChunkIdx);
        Y_ABORT_UNLESS(chunkRef.AllocationWaiters);
        --chunkRef.AllocationWaiters;

        const size_t numErased = ChunkMapIncrementsInFlight.erase({tabletId, vChunkIndex, allocation.ChunkIdx});
        Y_ABORT_UNLESS(numErased == 1);
        *Counters.Chunks.ChunksOwned += allocation.NewlyCommittedChunks;

        chunkRef.CommitReady.NotifyAll();
        QueuePendingWritesForChunk(tabletId, vChunkIndex);
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
        // reserved and while actor-owned destination records wait for readiness.
        for (const auto& [vChunkIndex, chunkRef] : tabletIt->second) {
            if (chunkRef.AllocationPending || chunkRef.AllocationWaiters) {
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
        auto ticket = std::make_shared<TLogTicket>();
        ticket->IsDDisk = true;
        IssuePDiskLogRecord(TLogSignature::SignatureDDiskChunkMap, 0,
            CreateChunkMapSnapshot(), &ChunkMapSnapshotLsn, std::move(chunksToDelete),
            TLogLegacyCompletion{ticket});
        if (!(co_await WaitForLog(ticket))) {
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
