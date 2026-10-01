#include "ddisk_actor.h"
#include "direct_io_op.h"

#include <ydb/core/blobstorage/pdisk/blobstorage_pdisk_data.h>

#include <util/generic/overloaded.h>
#include <ydb/core/util/stlog.h>

#include <cerrno>
#include <util/generic/scope.h>
#include <ydb/library/actors/async/wait_for_event.h>

namespace NKikimr::NDDisk {

    TDDiskActor::TPendingIoOp::TPendingIoOp(std::unique_ptr<TDirectIoOpBase> op)
        : Op(std::move(op))
    {}

    TDDiskActor::TPendingIoOp::TPendingIoOp(TPendingIoOp&&) noexcept = default;
    TDDiskActor::TPendingIoOp& TDDiskActor::TPendingIoOp::operator=(TPendingIoOp&&) noexcept = default;
    TDDiskActor::TPendingIoOp::~TPendingIoOp() = default;

    void TDDiskActor::SendPDiskWrite(std::unique_ptr<TDirectIoOpBase> op) {
        const ui64 cookie = NextCookie++;
        Send(BaseInfo.PDiskActorID, new NPDisk::TEvChunkWriteRaw(
            PDiskParams->Owner,
            PDiskParams->OwnerRound,
            op->GetChunkIdx(),
            op->GetChunkOffset(),
            op->ExtractData()), 0, cookie);

        WriteCallbacks.try_emplace(
            cookie,
            TPendingIoOp(std::move(op)));
    }

    void TDDiskActor::SendPDiskRead(std::unique_ptr<TDirectIoOpBase> op) {
        const ui64 cookie = NextCookie++;
        if (op->IsReadPartsIo()) {
            auto& read = static_cast<TReadPartsIoOp&>(*op);
            const size_t count = read.GetActivePartCount();
            ReadPartsRemaining.emplace(cookie, count);
            for (size_t i = 0; i < count; ++i) {
                const auto& part = read.GetFallbackPart(i);
                const ui64 partCookie = NextCookie++;
                ReadPartCallbacks.emplace(partCookie, TReadPartCallback{cookie, i});
                Send(BaseInfo.PDiskActorID, new NPDisk::TEvChunkReadRaw(
                    PDiskParams->Owner, PDiskParams->OwnerRound,
                    part.ChunkIdx, part.OffsetInBytes, part.Size), 0, partCookie);
            }
            ReadCallbacks.try_emplace(cookie, TPendingIoOp(std::move(op)));
            return;
        }
        Send(BaseInfo.PDiskActorID, new NPDisk::TEvChunkReadRaw(
            PDiskParams->Owner,
            PDiskParams->OwnerRound,
            op->GetChunkIdx(),
            op->GetChunkOffset(),
            op->GetTotalSize()), 0, cookie);

        ReadCallbacks.try_emplace(
            cookie,
            TPendingIoOp(std::move(op)));
    }

    void TDDiskActor::Handle(TEvWrite::TPtr ev) {
        YDB_LOG_TRACE_COMP(BS_DDISK, "TDDiskActor::Handle(TEvWrite)",
            {"marker", "BSDD50"},
            {"DDiskId", DDiskId},
            {"sender", ev->Sender},
            {"cookie", ev->Cookie});

        TQueryCredentials creds;
        if (!CheckQueryImpl<false>(*ev, &Counters.Interface.Write, creds)) {
            co_return;
        }

        const auto& record = ev->Get()->Record;
        const TQueryCredentials originalCredentials(record.GetCredentials());
        const TBlockSelector selector(record.GetSelector());
        const TWriteInstruction instr(record.GetInstruction());

        if (TabletChunkDeletionsInFlight.contains(creds.TabletId)) {
            Counters.Interface.Write.Request(selector.Size);
            Counters.Interface.Write.Reply(false, selector.Size);
            SendReply(*ev, std::make_unique<TEvWriteResult>(
                NKikimrBlobStorage::NDDisk::TReplyStatus::BUSY,
                "tablet chunk deletion is in flight"));
            co_return;
        }

        if (!ev->Get()->PayloadAlignmentChecked && instr.PayloadId) {
            ev->Get()->PayloadAlignmentChecked = true;
            const TRope& data = ev->Get()->GetPayload(*instr.PayloadId);
            const auto dataIter = data.Begin();
            if (dataIter.ContiguousSize() != data.size() ||
                    reinterpret_cast<uintptr_t>(dataIter.ContiguousData()) % DiskFormat->SectorSize != 0) {
                Counters.Interface.UnalignedWritePayloads->Inc();
            }
        }

        if (selector.OffsetInBytes % IntegrityUnitSize || selector.Size % IntegrityUnitSize) {
            Counters.Interface.Write.Request(selector.Size);
            Counters.Interface.Write.Reply(false, selector.Size);
            SendReply(*ev, std::make_unique<TEvWriteResult>(
                NKikimrBlobStorage::NDDisk::TReplyStatus::INCORRECT_REQUEST,
                "write offset and size must be aligned to 4 KiB"));
            co_return;
        }

        if (Config.EnableChecksums) {
            if (!HasRequiredBlockChecksums(record.ChecksumsSize(), selector.OffsetInBytes, selector.Size)) {
                if (record.ChecksumsSize() == 0) {
                    Counters.Checksums.WritesWithoutChecksums->Inc();
                }
                Counters.Interface.Write.Request(selector.Size);
                Counters.Interface.Write.Reply(false, selector.Size);
                SendReply(*ev, std::make_unique<TEvWriteResult>(
                    NKikimrBlobStorage::NDDisk::TReplyStatus::INCORRECT_REQUEST,
                    "one checksum per aligned 4 KiB block is required"));
                co_return;
            }

            Y_ABORT_UNLESS(instr.PayloadId, "TEvWrite without a payload, but with checksums");

            if (Config.CheckChecksumBeforeWrite) {
                const TRope& payload = ev->Get()->GetPayload(*instr.PayloadId);
                if (const auto result = ValidatePayloadChecksums(record, payload)) {
                    const bool isCorrupted = result->Status == NKikimrBlobStorage::NDDisk::TReplyStatus::CORRUPTED;
                    Counters.Interface.Write.Request(selector.Size);
                    Counters.Interface.Write.Reply(false, selector.Size);
                    if (isCorrupted) {
                        Counters.Checksums.ChecksumMismatch->Inc();
                    }
                    YDB_LOG_ERROR_COMP(NKikimrServices::BS_DDISK,
                        (isCorrupted
                            ? "TDDiskActor::Handle(TEvWrite) checksum mismatch"
                            : "TDDiskActor::Handle(TEvWrite) checksum count mismatch"),
                        {"marker", "BSDD52"},
                        {"DDiskId", DDiskId},
                        {"tabletId", creds.TabletId},
                        {"vChunkIndex", selector.VChunkIndex},
                        {"offsetInBytes", selector.OffsetInBytes},
                        {"checksumCount", result->ChecksumCount},
                        {"selectorSize", selector.Size},
                        {"blockIdx", result->MismatchedBlockIdx ? static_cast<i64>(*result->MismatchedBlockIdx) : -1});
                    SendReply(*ev, std::make_unique<TEvWriteResult>(result->Status, result->ErrorReason));
                    co_return;
                }
            }
        }

        Counters.Interface.Write.Request(selector.Size);
        const auto requestStartTs = HPNow();

        auto span = NWilson::TSpan(TWilson::DDiskTopLevel, std::move(ev->TraceId), "DDisk.Write",
                NWilson::EFlags::NONE, TActivationContext::ActorSystem());
        NPrivate::AddMessageWaitAttributes(span);
        span
            .Attribute("tablet_id", static_cast<i64>(creds.TabletId))
            .Attribute("vchunk_index", static_cast<i64>(selector.VChunkIndex))
            .Attribute("offset_in_bytes", selector.OffsetInBytes)
            .Attribute("size", selector.Size);

        TRope data;
        if (instr.PayloadId) {
            data = ev->Get()->GetPayload(*instr.PayloadId);
        }

        Y_ABORT_UNLESS(data.size() == selector.Size);

        auto pending = std::make_shared<TPendingDDiskWrite>();
        pending->Id = NextCookie++;
        pending->OriginalCredentials = originalCredentials;
        pending->ResolvedCredentials = creds;
        pending->Selector = selector;
        pending->Data = std::move(data);
        pending->Checksums.assign(record.GetChecksums().begin(), record.GetChecksums().end());
        pending->StartTs = requestStartTs;
        pending->MetadataDone = !Config.EnableChecksums;
        pending->Result.emplace(NPDisk::TUringOperationBase::EWRITE,
            NKikimrBlobStorage::NDDisk::TReplyStatus::OK, TString{}, TRope{}, ev->Sender,
            ev->InterconnectSession, ev->Cookie, std::move(span), selector.Size, 0,
            creds.TabletId, selector.VChunkIndex, true);
        auto& chunkRef = ChunkRefs[creds.TabletId][selector.VChunkIndex];
        ++chunkRef.AllocationWaiters;
        pending->AllocationPinned = true;
        Y_ABORT_UNLESS(PendingDDiskWrites.emplace(pending->Id, pending).second);
        // The aggregate owns all input needed beyond this turn, including the original token.
        ev.Reset(nullptr);
        QueuePendingWrite(pending->Id);
        co_await TDDiskWriteAwaiter(pending);
    }

    void TDDiskActor::QueuePendingWrite(ui64 id) {
        PendingWriteWork.push_back(id);
        DrainPendingWriteWork();
    }

    void TDDiskActor::StartSyncDestination(ui64 syncId, ui32 input, TRope data,
            std::vector<ui64> checksums)
    {
        const auto parentIt = SyncsInFlight.find(syncId);
        if (parentIt == SyncsInFlight.end() || input >= parentIt->second->Requests.size()) {
            return;
        }
        const auto parent = parentIt->second;
        auto& source = parent->Requests[input];
        if (source.Terminal || source.DestinationStarted) {
            return;
        }

        auto write = std::make_shared<TPendingDDiskWrite>();
        write->Id = NextCookie++;
        write->SyncId = syncId;
        write->SyncInput = input;
        write->OriginalCredentials = parent->OriginalCredentials;
        write->ResolvedCredentials = parent->Creds;
        write->Selector = source.Selector;
        write->Data = std::move(data);
        write->Checksums = std::move(checksums);
        write->StartTs = HPNow();
        write->MetadataDone = !Config.EnableChecksums;
        write->Result.emplace(NPDisk::TUringOperationBase::EWRITE,
            NKikimrBlobStorage::NDDisk::TReplyStatus::OK, TString{}, TRope{},
            TActorId{}, TActorId{}, 0, NWilson::TSpan{}, source.Selector.Size, 0,
            parent->Creds.TabletId, source.Selector.VChunkIndex, true);
        if (source.AllocationPinned) {
            source.AllocationPinned = false;
            write->AllocationPinned = true;
        } else {
            ++ChunkRefs[parent->Creds.TabletId][source.Selector.VChunkIndex].AllocationWaiters;
            write->AllocationPinned = true;
        }
        if (source.MetadataStarted) {
            write->Metadata = std::move(source.Metadata);
            write->MetadataStarted = true;
            source.MetadataStarted = false;
        }
        source.DestinationStarted = true;
        Y_ABORT_UNLESS(PendingDDiskWrites.emplace(write->Id, write).second);
        if (write->MetadataStarted) {
            const ui64 id = write->Id;
            write->Metadata.Ready.SetCompletionCallback([this, id] {
                QueuePendingWrite(id);
            });
            write->Metadata.Durable.SetCompletionCallback([this, id] {
                QueuePendingWrite(id);
            });
        }
        QueuePendingWrite(write->Id);
    }

    void TDDiskActor::QueuePendingWritesForChunk(ui64 tabletId, ui64 vChunkIndex) {
        for (const auto& [id, write] : PendingDDiskWrites) {
            if (write->ResolvedCredentials.TabletId == tabletId && write->Selector.VChunkIndex == vChunkIndex) {
                PendingWriteWork.push_back(id);
            }
        }
        DrainPendingWriteWork();
        for (const auto& [id, sync] : SyncsInFlight) {
            if (sync->Creds.TabletId == tabletId && sync->VChunkIndex == vChunkIndex) {
                SyncWork.push_back(id);
            }
        }
        DrainSyncWork();
    }

    void TDDiskActor::QueueAllPendingWrites() {
        for (const auto& [id, _] : PendingDDiskWrites) {
            PendingWriteWork.push_back(id);
        }
        DrainPendingWriteWork();
    }

    void TDDiskActor::DrainPendingWriteWork() {
        if (DrainingPendingWriteWork) {
            return;
        }
        DrainingPendingWriteWork = true;
        Y_DEFER { DrainingPendingWriteWork = false; };
        while (!PendingWriteWork.empty()) {
            const ui64 id = PendingWriteWork.front();
            PendingWriteWork.pop_front();
            if (PendingDDiskWrites.contains(id)) {
                AdvancePendingWrite(id);
            }
        }
    }

    void TDDiskActor::AdvancePendingWrite(ui64 id) {
        auto it = PendingDDiskWrites.find(id);
        if (it == PendingDDiskWrites.end()) {
            return;
        }
        const auto write = it->second;
        const ui64 tabletId = write->ResolvedCredentials.TabletId;
        const ui64 vChunkIndex = write->Selector.VChunkIndex;
        auto& chunk = ChunkRefs.at(tabletId).at(vChunkIndex);
        using TStatus = NKikimrBlobStorage::NDDisk::TReplyStatus;

        auto failBeforeSubmission = [&](TStatus::E status, TString reason) {
            if (write->DataSubmitted || write->DataDone) {
                return;
            }
            write->Result->Status = status;
            write->Result->ErrorMessage = std::move(reason);
            write->DataDone = true;
            if (write->MetadataStarted && !write->MetadataConsumed) {
                IntegrityManager->CancelWrite(write->Metadata.Id);
            } else if (!write->MetadataStarted) {
                write->MetadataDone = true;
            }
        };

        if (!write->DataSubmitted && !write->DataDone) {
            if (Stopping || IsBroken()) {
                failBeforeSubmission(IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH,
                    IsBroken() ? GetBrokenReason() : TString(StoppingReason));
            }
            if (!write->DataDone) {
                TQueryCredentials current;
                const auto resolution = ResolveConnection(write->OriginalCredentials, &current);
                if (resolution != EConnectionResolution::Resolved
                        || current.TabletId != tabletId
                        || current.Generation != write->ResolvedCredentials.Generation
                        || current.DDiskSessionSeqNo != write->ResolvedCredentials.DDiskSessionSeqNo
                        || current.DirectBlockGroupIndex != write->ResolvedCredentials.DirectBlockGroupIndex
                        || current.DDiskInstanceGuid != write->ResolvedCredentials.DDiskInstanceGuid) {
                    failBeforeSubmission(TStatus::SESSION_MISMATCH,
                        "session replaced while write was waiting");
                }
            }
            if (!write->DataDone && !chunk.ChunkIdx) {
                if (!chunk.AllocationPending) {
                    IssueChunkAllocation(tabletId, vChunkIndex);
                }
                if (!chunk.ChunkIdx) {
                    return;
                }
            }
            if (!write->DataDone && Config.EnableChecksums && !write->MetadataStarted) {
                write->MetadataStarted = true;
                write->Metadata = IntegrityManager->PrepareWrite({tabletId, vChunkIndex},
                    write->Selector.OffsetInBytes, write->Selector.Size);
                write->Metadata.Ready.SetCompletionCallback([this, id] {
                    QueuePendingWrite(id);
                });
                write->Metadata.Durable.SetCompletionCallback([this, id] {
                    QueuePendingWrite(id);
                });
            }
            if (!write->DataDone && Config.EnableChecksums) {
                const auto* ready = write->Metadata.Ready.GetResult();
                if (!ready) {
                    return;
                }
                if (ready->Status != TIntegrityManager::EOperationStatus::Ok) {
                    failBeforeSubmission(ready->Status == TIntegrityManager::EOperationStatus::Corrupted
                        ? TStatus::CORRUPTED : TStatus::SESSION_MISMATCH, ready->ErrorReason);
                }
            }
            // A connection can change while metadata loads are pending.
            if (!write->DataDone) {
                TQueryCredentials current;
                const auto resolution = ResolveConnection(write->OriginalCredentials, &current);
                if (resolution != EConnectionResolution::Resolved
                        || current.TabletId != tabletId
                        || current.Generation != write->ResolvedCredentials.Generation
                        || current.DDiskSessionSeqNo != write->ResolvedCredentials.DDiskSessionSeqNo
                        || current.DirectBlockGroupIndex != write->ResolvedCredentials.DirectBlockGroupIndex
                        || current.DDiskInstanceGuid != write->ResolvedCredentials.DDiskInstanceGuid) {
                    failBeforeSubmission(TStatus::SESSION_MISMATCH,
                        "session replaced while write was waiting");
                }
            }
            if (!write->DataDone) {
                if (Config.EnableChecksums) {
                    write->MetadataConsumed = true;
                    IntegrityManager->ConsumeWrite(write->Metadata.Id, std::move(write->Checksums));
                }
                const auto* immediate = Config.EnableChecksums ? write->Metadata.Durable.GetResult() : nullptr;
                if (Stopping || IsBroken() || (immediate && immediate->Status != TIntegrityManager::EOperationStatus::Ok)) {
                    write->DataDone = true;
                } else {
                    std::unique_ptr<TDirectIoOpBase> op;
                    if (write->SyncId) {
                        op = AllocateOp<TInternalSyncWriteOp>();
                    } else {
                        op = AllocateOp<TDDiskIoOp>();
                        static_cast<TDDiskIoOp*>(op.get())->SetChunkKey(tabletId, vChunkIndex);
                    }
                    const ui64 cookie = NActors::AllocateWaitCookie();
                    op->SetCompletionCookie(cookie);
                    op->PrepareWrite(std::move(write->Data),
                        DiskFormat->Offset(chunk.ChunkIdx, 0, write->Selector.OffsetInBytes),
                        chunk.ChunkIdx, write->Selector.OffsetInBytes);
                    write->DataPin.emplace(chunk);
                    write->DataSubmitted = true;
                    Y_ABORT_UNLESS(PendingWriteDataCookies.emplace(cookie,
                        TPendingWriteDataCookie{id, static_cast<bool>(write->SyncId)}).second);
                    DirectUringOp(op);
                }
            }
        }

        if (write->MetadataStarted && !write->MetadataDone) {
            if (const auto* metadata = write->Metadata.Durable.GetResult();
                    metadata && write->Metadata.Durable.IsSettled()) {
                write->MetadataDone = true;
                CountIntegrityResult(*metadata);
                write->MetadataOutcome = *metadata;
            }
        }
        if (write->DataDone && write->MetadataDone) {
            if (write->MetadataOutcome
                    && write->MetadataOutcome->Status != TIntegrityManager::EOperationStatus::Ok) {
                write->Result->Status = write->MetadataOutcome->Status == TIntegrityManager::EOperationStatus::Corrupted
                    ? TStatus::CORRUPTED : TStatus::SESSION_MISMATCH;
                write->Result->ErrorMessage = write->MetadataOutcome->ErrorReason;
            }
            if ((write->DataSubmitted || write->MetadataConsumed) && !IsChunkCommitted(tabletId, vChunkIndex)) {
                if (!Stopping && !IsBroken()) {
                    return;
                }
                write->Result->Status = TStatus::SESSION_MISMATCH;
                write->Result->ErrorMessage = TString(StoppingReason);
            }
            FinishPendingWrite(id);
        }
    }

    void TDDiskActor::FinishPendingWrite(ui64 id) {
        const auto it = PendingDDiskWrites.find(id);
        if (it == PendingDDiskWrites.end()) {
            return;
        }
        auto write = std::move(it->second);
        PendingDDiskWrites.erase(it);
        auto& chunk = ChunkRefs.at(write->ResolvedCredentials.TabletId).at(write->Selector.VChunkIndex);
        if (write->AllocationPinned) {
            Y_ABORT_UNLESS(chunk.AllocationWaiters);
            --chunk.AllocationWaiters;
            write->AllocationPinned = false;
        }
        write->DataPin.reset();
        write->Result->RequestTimeMs = HPMilliSecondsFloat(HPNow() - write->StartTs);
        write->Done = true;
        if (write->SyncId) {
            CompleteSyncDestination(write->SyncId, write->SyncInput,
                write->Result->Status, std::move(write->Result->ErrorMessage));
        } else {
            FinishDDiskIoResult(*write->Result);
        }
        write->Changed.NotifyAll();
        if (Stopping && !GetDirectIoInflight()) {
            FinishStopping();
        }
    }

    void TDDiskActor::Handle(TEvPrivate::TEvDDiskIoResult::TPtr ev) {
        const auto it = PendingWriteDataCookies.find(ev->Cookie);
        if (it == PendingWriteDataCookies.end()) {
            return;
        }
        if (it->second.Sync) {
            return;
        }
        const ui64 id = it->second.Id;
        PendingWriteDataCookies.erase(it);
        const auto pending = PendingDDiskWrites.find(id);
        if (pending == PendingDDiskWrites.end()) {
            return;
        }
        auto& write = *pending->second;
        write.DataPin.reset();
        write.DataDone = true;
        write.Result->Status = ev->Get()->Status;
        write.Result->ErrorMessage = std::move(ev->Get()->ErrorMessage);
        QueuePendingWrite(id);
        if (Stopping && !GetDirectIoInflight()) {
            FinishStopping();
        }
    }

    void TDDiskActor::Handle(TEvPrivate::TEvInternalSyncWriteResult::TPtr ev) {
        const auto it = PendingWriteDataCookies.find(ev->Cookie);
        if (it == PendingWriteDataCookies.end() || !it->second.Sync) {
            return;
        }
        const ui64 id = it->second.Id;
        PendingWriteDataCookies.erase(it);
        const auto pending = PendingDDiskWrites.find(id);
        if (pending == PendingDDiskWrites.end() || !pending->second->SyncId) {
            return;
        }
        auto& write = *pending->second;
        write.DataPin.reset();
        write.DataDone = true;
        write.Result->Status = ev->Get()->Status;
        write.Result->ErrorMessage = std::move(ev->Get()->ErrorMessage);
        QueuePendingWrite(id);
        if (Stopping && !GetDirectIoInflight()) {
            FinishStopping();
        }
    }

	void TDDiskActor::Handle(NPDisk::TEvChunkWriteRawResult::TPtr ev) {
        auto& msg = *ev->Get();
        YDB_LOG_DEBUG_COMP(BS_DDISK, "TDDiskActor::Handle(TEvChunkWriteRawResult)",
            {"marker", "BSDD07"},
            {"DDiskId", DDiskId},
            {"msg", msg});

        auto it = WriteCallbacks.find(ev->Cookie);
        if (it == WriteCallbacks.end()) {
            Y_ABORT_UNLESS(IsBroken());
            return;
        }

        if (Y_UNLIKELY(IsBroken())) {
            std::unique_ptr<TDirectIoOpBase> op = std::move(it->second.Op);
            WriteCallbacks.erase(it);
            op->SetResult(-EIO);
            op.release()->OnComplete(TActivationContext::ActorSystem());
            return;
        }

        if (msg.Status != NKikimrProto::OK) {
            if (it->second.Op->IsCriticalDDiskIo()) {
                // A fallback integrity/format write is a DDisk failure, not a reason to enter
                // the passive PDisk-session termination state. Finish it through the same op path
                // as an io_uring EIO so the health latch is published before any success reply.
                std::unique_ptr<TDirectIoOpBase> op = std::move(it->second.Op);
                WriteCallbacks.erase(it);
                op->SetResult(-EIO);
                op.release()->OnComplete(TActivationContext::ActorSystem());
                return;
            }
            if (!CheckPDiskReply(msg.Status, msg.ErrorReason, "Handle(TEvChunkWriteRawResult)")) {
                return;
            }
        }

        std::unique_ptr<TDirectIoOpBase> op = std::move(it->second.Op);
        WriteCallbacks.erase(it);

        // fill the op with result and finish via common completion path
        Y_DEBUG_ABORT_UNLESS(op->GetTotalSize() <= static_cast<ui64>(Max<i32>()));
        op->SetResult(static_cast<i32>(op->GetTotalSize()));

        op.release()->OnComplete(TActivationContext::ActorSystem());
    }

    void TDDiskActor::Handle(TEvRead::TPtr ev) {
        YDB_LOG_TRACE_COMP(BS_DDISK, "TDDiskActor::Handle(TEvRead)",
            {"marker", "BSDD21"},
            {"DDiskId", DDiskId},
            {"msg", ev->Get()->Record});

        TQueryCredentials creds;
        if (!CheckQuery(*ev, &Counters.Interface.Read, creds)) {
            co_return;
        }

        const TBlockSelector selector(ev->Get()->Record.GetSelector());

        if (selector.OffsetInBytes % IntegrityUnitSize != 0
                || selector.Size % IntegrityUnitSize != 0) {
            Counters.Interface.Read.Request(selector.Size);
            Counters.Interface.Read.Reply(false, selector.Size);
            SendReply(*ev, std::make_unique<TEvReadResult>(
                NKikimrBlobStorage::NDDisk::TReplyStatus::INCORRECT_REQUEST,
                "read offset and size must be aligned to the 4 KiB integrity unit"));
            co_return;
        }

        if (TabletChunkDeletionsInFlight.contains(creds.TabletId)) {
            Counters.Interface.Read.Request(selector.Size);
            Counters.Interface.Read.Reply(false, selector.Size);
            SendReply(*ev, std::make_unique<TEvReadResult>(
                NKikimrBlobStorage::NDDisk::TReplyStatus::BUSY,
                "tablet chunk deletion is in flight"));
            co_return;
        }

        TChunkRef* readyChunk = &ChunkRefs[creds.TabletId][selector.VChunkIndex];
        if (readyChunk->AllocationPending) {
            auto span = NWilson::TSpan(TWilson::DDiskTopLevel, NWilson::TTraceId(ev->TraceId),
                "WaitChunkAllocation", NWilson::EFlags::AUTO_END, TActivationContext::ActorSystem());
            NPrivate::AddMessageWaitAttributes(span);
            co_await WaitForChunk(creds.TabletId, selector.VChunkIndex, false);
            if (Stopping || IsBroken()) {
                RejectQuery(*ev, Stopping
                    ? NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH
                    : NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR,
                    Stopping ? TString(StoppingReason) : GetBrokenReason());
                co_return;
            }
            if (!CheckQuery(*ev, &Counters.Interface.Read, creds)) {
                co_return;
            }
            readyChunk = &ChunkRefs.at(creds.TabletId).at(selector.VChunkIndex);
        }
        TChunkRef& chunkRef = *readyChunk;

        Counters.Interface.Read.Request(selector.Size);

        // No chunk allocated: the whole range was never written.
        if (!chunkRef.ChunkIdx) {
            auto zero = TRcBuf::Uninitialized(selector.Size);
            memset(zero.GetDataMut(), 0, zero.size());
            TRope result(std::move(zero));
            TReadChecksums checksums;
            if (Config.EnableChecksums) {
                if (selector.Size == IntegrityUnitSize) {
                    checksums.SetSingle(GetZeroBlockChecksum());
                }
                else {
                    checksums.SetMany(std::vector<ui64>(selector.Size / IntegrityUnitSize, GetZeroBlockChecksum()));
                }
            }
            Counters.Interface.Read.Reply(true, selector.Size, 0);
            SendReply(*ev, std::make_unique<TEvReadResult>(
                NKikimrBlobStorage::NDDisk::TReplyStatus::OK, std::nullopt,
                std::move(result), checksums.View()));
            co_return;
        }

        ++chunkRef.AllocationWaiters;
        Y_DEFER { --chunkRef.AllocationWaiters; };
        const TDDiskReadContext context{ev->Sender, ev->InterconnectSession, ev->Cookie,
            creds.TabletId, selector.VChunkIndex, HPNow()};
        auto span = NWilson::TSpan(TWilson::DDiskTopLevel, std::move(ev->TraceId), "DDisk.Read",
            NWilson::EFlags::NONE, TActivationContext::ActorSystem());
        NPrivate::AddMessageWaitAttributes(span);
        span.Attribute("tablet_id", static_cast<i64>(creds.TabletId))
            .Attribute("vchunk_index", static_cast<i64>(selector.VChunkIndex))
            .Attribute("offset_in_bytes", selector.OffsetInBytes).Attribute("size", selector.Size);
        auto result = co_await ReadDDisk(ev, chunkRef, creds.TabletId, selector, span);
        FinishDDiskRead(context, result);
    }

    bool TDDiskActor::SubmitDDiskDataRead(TEvRead::TPtr& request, TChunkIdx chunkIdx,
            const TBlockSelector& selector, NWilson::TSpan& span, ui64 indexedReadToken,
            TDDiskReadResult& result)
    {
        if (Stopping || IsBroken()) {
            result.Span = std::move(span);
            result.Status = IsBroken() ? NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR
                : NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH;
            result.ErrorMessage = IsBroken() ? GetBrokenReason() : TString(StoppingReason);
            request.Reset();
            return false;
        }
        std::unique_ptr<TDirectIoOpBase> op = AllocateOp<TDDiskIoOp>();
        op->SetSpan(std::move(span));
        op->PrepareRead(selector.Size, DiskFormat->Offset(chunkIdx, 0, selector.OffsetInBytes),
            chunkIdx, selector.OffsetInBytes);
        static_cast<TDDiskIoOp*>(op.get())->SetIndexedReadToken(indexedReadToken);
        // The coroutine owns reply routing; the operation owns its completion span.
        // Drop the request while it is hot instead of retaining its protobuf across I/O.
        request.Reset();
        FindIndexedRead(indexedReadToken)->Submitted = true;
        DirectUringOp(op);
        return true;
    }

    ui64 TDDiskActor::ReserveIndexedRead(TDDiskReadAwaiter& waiter, TChunkRef& chunk) {
        ui32 index = FirstFreeIndexedRead;
        if (index == Max<ui32>()) {
            Y_ABORT_UNLESS(IndexedReads.size() < Max<ui32>());
            index = IndexedReads.size();
            IndexedReads.emplace_back();
        } else {
            FirstFreeIndexedRead = IndexedReads[index].NextFree;
        }
        auto& slot = IndexedReads[index];
        Y_ABORT_UNLESS(!slot.Pin && !slot.Waiter);
        slot.Pin.emplace(chunk);
        slot.Waiter = &waiter;
        slot.Submitted = false;
        ++ActiveIndexedReads;
        return (ui64(slot.Generation) << 32) | (ui64(index) + 1);
    }

    TDDiskActor::TIndexedReadSlot* TDDiskActor::FindIndexedRead(ui64 token) {
        const ui32 encodedIndex = token;
        if (!encodedIndex || encodedIndex > IndexedReads.size()) {
            return nullptr;
        }
        auto& slot = IndexedReads[encodedIndex - 1];
        return slot.Pin && slot.Generation == (token >> 32) ? &slot : nullptr;
    }

    void TDDiskActor::ReleaseIndexedRead(ui64 token) {
        auto* slot = FindIndexedRead(token);
        Y_ABORT_UNLESS(slot && ActiveIndexedReads);
        slot->Waiter = nullptr;
        slot->Pin.reset();
        slot->Submitted = false;
        --ActiveIndexedReads;
        // Never wrap a generation: an ancient queued token must not match a new read.
        if (slot->Generation != Max<ui32>()) {
            ++slot->Generation;
            slot->NextFree = FirstFreeIndexedRead;
            FirstFreeIndexedRead = ui32(token) - 1;
        }
    }

    void TDDiskActor::DetachIndexedRead(ui64 token) {
        if (auto* slot = FindIndexedRead(token)) {
            slot->Waiter = nullptr;
            if (!slot->Submitted) {
                ReleaseIndexedRead(token);
            }
        }
    }

    void TDDiskActor::Handle(TEvPrivate::TEvIndexedReadResult::TPtr ev) {
        const ui64 token = ev->Get()->Token;
        auto* slot = FindIndexedRead(token);
        if (!slot || !slot->Submitted) {
            return;
        }
        auto* waiter = slot->Waiter;
        std::coroutine_handle<> continuation;
        if (waiter) {
            auto& outcome = *ev->Get();
            waiter->Result.Status = outcome.Status;
            waiter->Result.ErrorMessage = std::move(outcome.ErrorMessage);
            waiter->Result.Span = std::move(outcome.Span);
            waiter->Result.Data = std::move(outcome.Data);
            waiter->Token = 0;
            continuation = std::exchange(waiter->Continuation, {});
        }
        ReleaseIndexedRead(token);
        // Inline resumption may destroy the awaiter or grow/reuse the registry.
        if (continuation) {
            continuation.resume();
        }
        if (Stopping && !GetDirectIoInflight() && !ActiveIndexedReads) {
            FinishStopping();
        }
    }

    TDDiskActor::TDDiskReadAwaiter TDDiskActor::ReadDDisk(TEvRead::TPtr& request,
            TChunkRef& chunk, ui64 tabletId, const TBlockSelector& selector, NWilson::TSpan& span)
    {
        return TDDiskReadAwaiter(*this, request, chunk, tabletId, selector, span);
    }

    TDDiskActor::TDDiskReadAwaiter::TDDiskReadAwaiter(TDDiskActor& self, TEvRead::TPtr& request,
            TChunkRef& chunk, ui64 tabletId, const TBlockSelector& selector, NWilson::TSpan& span)
        : Self(self) {
        Result.TotalSize = selector.Size;
        TIntegrityManager::TReadPreparation preparation;
        if (Self.Config.EnableChecksums) {
            preparation = Self.IntegrityManager->PrepareRead({tabletId, selector.VChunkIndex},
                selector.OffsetInBytes, selector.Size, Metadata);
        }

        if (Metadata && (Metadata->Status != TIntegrityManager::EOperationStatus::Ok
                || Metadata->ReadPlan.Kind == TIntegrityManager::TReadPlan::AllZero)) {
            Result.Span = std::move(span);
            Self.ApplyDDiskReadMetadata(Result, std::move(*Metadata));
            Metadata.reset();
            request.Reset();
            return;
        }
        if (!Self.Config.EnableChecksums || Metadata) {
            Token = Self.ReserveIndexedRead(*this, chunk);
            bool submitted = false;
            Y_DEFER {
                if (!submitted) {
                    Self.DetachIndexedRead(std::exchange(Token, 0));
                }
            };
            submitted = Self.SubmitDDiskDataRead(request, chunk.ChunkIdx,
                selector, span, Token, Result);
            if (submitted) {
                Mode = EMode::DataEvent;
            }
            return;
        }

        Mode = EMode::Cold;
        auto context = std::make_shared<TPendingDDiskRead>(chunk);
        context->Metadata = std::move(preparation.Pending);
        context->Result.TotalSize = selector.Size;
        context->Result.Span = std::move(span);
        request.Reset();
        const ui64 cookie = NActors::AllocateWaitCookie();
        Self.PendingDDiskReads.emplace(cookie, context);
        Cold.emplace(context);

        std::vector<TReadPartsIoOp::TPart> parts;
        // Requested blocks are not written concurrently. Neighboring writes may change
        // the same pair, but cannot turn a known hole in this range into used data.
        if (Self.IntegrityManager->MakeReadPlan({tabletId, selector.VChunkIndex},
                selector.OffsetInBytes, selector.Size).Kind != TIntegrityManager::TReadPlan::AllZero) {
            parts.push_back({0, chunk.ChunkIdx, selector.OffsetInBytes, selector.Size,
                Self.DiskFormat->Offset(chunk.ChunkIdx, 0, selector.OffsetInBytes)});
        }
        for (const auto& read : preparation.Reads) {
            parts.push_back({read.Id, read.ChunkIdx, read.OffsetInBytes, read.Size,
                Self.DiskFormat->Offset(read.ChunkIdx, 0, read.OffsetInBytes)});
        }
        *Self.Counters.Checksums.IntegrityPairReads += preparation.Reads.size();
        context->IoPending = !parts.empty();
        context->Metadata.SetCompletionCallback([&self, cookie] {
            self.TryFinishDDiskRead(cookie);
        });
        if (!parts.empty()) {
            auto readOp = std::make_unique<TReadPartsIoOp>(Self);
            readOp->SetCompletionCookie(cookie);
            readOp->PrepareParts(parts);
            std::unique_ptr<TDirectIoOpBase> op = std::move(readOp);
            Self.DirectUringOp(op);
        }
        Self.TryFinishDDiskRead(cookie);
    }

    TDDiskActor::TDDiskReadAwaiter::~TDDiskReadAwaiter() {
        if (Token) {
            Self.DetachIndexedRead(Token);
        }
        if (Cold) {
            Cold->Context->Detached = true;
        }
    }

    bool TDDiskActor::TDDiskReadAwaiter::await_ready() const noexcept {
        return Mode == EMode::Ready || (Mode == EMode::Cold && Cold->Context->Done);
    }

    TDDiskActor::TDDiskReadResult
    TDDiskActor::TDDiskReadAwaiter::await_resume() {
        if (Mode == EMode::DataEvent) {
            Y_ABORT_UNLESS(!Token);
            if (Metadata) {
                Self.ApplyDDiskReadMetadata(Result, std::move(*Metadata));
            }
        } else if (Mode == EMode::Cold) {
            Y_ABORT_UNLESS(Cold->Context->Done);
            Result = std::move(Cold->Context->Result);
        }
        return std::move(Result);
    }

    void TDDiskActor::ApplyDDiskReadMetadata(TDDiskReadResult& result,
            TIntegrityManager::TOperationResult&& metadata)
    {
        ApplyDDiskReadMetadataImpl(result, std::move(metadata));
    }

    void TDDiskActor::ApplyDDiskReadMetadata(TDDiskReadResult& result,
            const TIntegrityManager::TOperationResult& metadata)
    {
        ApplyDDiskReadMetadataImpl(result, metadata);
    }

    template<class TMetadata>
    void TDDiskActor::ApplyDDiskReadMetadataImpl(TDDiskReadResult& result,
            TMetadata&& metadata)
    {
        CountIntegrityResult(metadata);
        if (metadata.Status != TIntegrityManager::EOperationStatus::Ok) {
            result.Status = metadata.Status == TIntegrityManager::EOperationStatus::Corrupted
                ? NKikimrBlobStorage::NDDisk::TReplyStatus::CORRUPTED
                : NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH;
            result.ErrorMessage = std::forward<TMetadata>(metadata).ErrorReason;
            return;
        }
        result.Checksums = std::forward<TMetadata>(metadata).Checksums;
        if (metadata.ReadPlan.Kind == TIntegrityManager::TReadPlan::AllZero) {
            auto zero = TRcBuf::Uninitialized(result.TotalSize);
            memset(zero.GetDataMut(), 0, zero.size());
            result.Data = TReadPayload(std::move(zero));
            result.Status = NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
            result.ErrorMessage.clear();
        } else if (result.Status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK
                && metadata.ReadPlan.Kind == TIntegrityManager::TReadPlan::Mixed) {
            auto data = result.Data.MutableSpan();
            for (size_t i = 0; i < data.size() / IntegrityUnitSize; ++i) {
                if (!metadata.ReadPlan.UsedBlocks.Get(i)) {
                    memset(data.data() + i * IntegrityUnitSize, 0, IntegrityUnitSize);
                }
            }
        }
    }

    void TDDiskActor::TryFinishDDiskRead(ui64 cookie) {
        const auto it = PendingDDiskReads.find(cookie);
        if (it == PendingDDiskReads.end()) {
            return;
        }
        auto context = it->second;
        const auto* metadata = context->Metadata.GetResult();
        if (context->IoPending || !metadata || !context->Metadata.IsSettled()) {
            return;
        }
        ApplyDDiskReadMetadata(context->Result, *metadata);
        context->Done = true;
        context->Metadata.SetCompletionCallback({});
        PendingDDiskReads.erase(it);
        if (!context->Detached) {
            context->Changed.NotifyAll();
        }
    }

    void TDDiskActor::Handle(TEvPrivate::TEvReadPartsResult::TPtr ev) {
        std::vector<TIntegrityManager::TPairReadResult> metadata;
        TString metadataError;
        bool metadataFailed = false;
        // Keep the context stable across CompletePairReads, which can resolve joined
        // reads and synchronously remove their entries from the registry.
        auto it = PendingDDiskReads.find(ev->Cookie);
        auto context = it == PendingDDiskReads.end() ? nullptr : it->second;
        for (auto& part : ev->Get()->Parts) {
            if (part.Id) {
                const bool ok = part.Status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
                if (!ok) {
                    metadataFailed = true;
                    if (!metadataError) {
                        metadataError = part.ErrorMessage;
                    }
                }
                metadata.push_back({part.Id, {ok, std::move(part.Data)}});
            } else if (context) {
                context->Result.Status = part.Status;
                context->Result.ErrorMessage = std::move(part.ErrorMessage);
                context->Result.Data = std::move(part.Data);
            }
        }
        if (metadataFailed && !Stopping) {
            EnterBroken(std::move(metadataError));
        }
        IntegrityManager->CompletePairReads(metadata);
        if (context) {
            context->IoPending = false;
            TryFinishDDiskRead(ev->Cookie);
        }
        if (Stopping && !GetDirectIoInflight()) {
            FinishStopping();
        }
        if (!Stopping && !IsBroken()) {
            RunIntegrityReclamation();
        }
    }

    void TDDiskActor::FinishDDiskRead(const TDDiskReadContext& context, TDDiskReadResult& result) {
        auto status = result.Status;
        TString error = std::move(result.ErrorMessage);
        if (IsBroken()) {
            status = NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR;
            error = GetBrokenReason();
        }
        TRope data = std::move(result.Data).IntoRope();
        if (status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK
                && Config.EnableChecksums && Config.CheckChecksumWhenRead && !result.Checksums.View().empty()) {
            if (const auto failure = ValidatePayloadChecksums(result.Checksums.View(), data)) {
                status = failure->Status;
                error = failure->ErrorReason;
                if (status == NKikimrBlobStorage::NDDisk::TReplyStatus::CORRUPTED) {
                    Counters.Checksums.ChecksumMismatch->Inc();
                }
                YDB_LOG_ERROR_COMP(NKikimrServices::BS_DDISK, "DDisk read checksum validation failed",
                    {"DDiskId", DDiskId}, {"tabletId", context.TabletId},
                    {"vChunkIndex", context.VChunkIndex}, {"reason", error});
            }
        }
        const bool ok = status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
        auto reply = std::make_unique<TEvReadResult>(status,
            error ? std::optional<TString>(std::move(error)) : std::nullopt,
            ok ? std::move(data) : TRope{}, ok ? result.Checksums.View() : TConstArrayRef<ui64>{});
        Counters.Interface.Read.Reply(ok, result.TotalSize, HPMilliSecondsFloat(HPNow() - context.Start));
        auto handle = std::make_unique<IEventHandle>(context.OriginalRequester, SelfId(), reply.release(),
            0, context.Cookie, nullptr, result.Span.GetTraceId());
        if (context.InterconnectSession) {
            handle->Rewrite(TEvInterconnect::EvForward, context.InterconnectSession);
        }
        result.Span.End();
        TActivationContext::Send(handle.release());
    }

    void TDDiskActor::FinishDDiskIoResult(TEvPrivate::TEvDDiskIoResult& msg) {
        static const std::vector<ui64> emptyChecksums;
        auto status = msg.Status;
        TString errorMessage = std::move(msg.ErrorMessage);
        if (Y_UNLIKELY(IsBroken())) {
            status = NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR;
            errorMessage = GetBrokenReason();
        }

        const bool isOkBeforeReadCheck = status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
        if (msg.OperationType == NPDisk::TUringOperationBase::EREAD
                && isOkBeforeReadCheck
                && Config.EnableChecksums
                && Config.CheckChecksumWhenRead
                && !msg.Checksums.empty())
        {
            if (const auto result = ValidatePayloadChecksums(msg.Checksums, msg.Data)) {
                status = result->Status;
                errorMessage = result->ErrorReason;
                if (result->Status == NKikimrBlobStorage::NDDisk::TReplyStatus::CORRUPTED) {
                    Counters.Checksums.ChecksumMismatch->Inc();
                }
                YDB_LOG_ERROR_COMP(NKikimrServices::BS_DDISK,
                    (result->Status == NKikimrBlobStorage::NDDisk::TReplyStatus::CORRUPTED
                        ? "TDDiskActor::Handle(TEvDDiskIoResult) checksum mismatch"
                        : "TDDiskActor::Handle(TEvDDiskIoResult) checksum count mismatch"),
                    {"marker", "BSDD53"},
                    {"DDiskId", DDiskId},
                    {"tabletId", msg.TabletId},
                    {"vChunkIndex", msg.VChunkIndex},
                    {"checksumCount", result->ChecksumCount},
                    {"payloadSize", msg.Data.size()},
                    {"blockIdx", result->MismatchedBlockIdx ? static_cast<i64>(*result->MismatchedBlockIdx) : -1});
            }
        }

        const bool isOk = status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
        std::optional<TString> errorReason;
        if (errorMessage) {
            errorReason.emplace(std::move(errorMessage));
        }

        std::unique_ptr<IEventBase> reply;
        switch (msg.OperationType) {
            case NPDisk::TUringOperationBase::EREAD:
                reply = std::make_unique<TEvReadResult>(
                    status, errorReason, isOk ? std::move(msg.Data) : TRope{},
                    isOk ? msg.Checksums : emptyChecksums);
                Counters.Interface.Read.Reply(isOk, msg.TotalSize, msg.RequestTimeMs);
                break;
            case NPDisk::TUringOperationBase::EWRITE:
                reply = std::make_unique<TEvWriteResult>(status, errorReason);
                Counters.Interface.Write.Reply(isOk, msg.TotalSize, msg.RequestTimeMs);
                break;
            default:
                Y_ABORT("Unknown OperationType");
        }

        auto h = std::make_unique<IEventHandle>(msg.OriginalRequester, SelfId(), reply.release(),
            0, msg.Cookie, nullptr, msg.Span.GetTraceId());
        if (msg.InterconnectSession) {
            h->Rewrite(TEvInterconnect::EvForward, msg.InterconnectSession);
        }
        msg.Span.End();
        TActivationContext::Send(h.release());
    }


	void TDDiskActor::Handle(NPDisk::TEvChunkReadRawResult::TPtr ev) {
        auto& msg = *ev->Get();
        YDB_LOG_DEBUG_COMP(BS_DDISK, "TDDiskActor::Handle(TEvChunkReadRawResult)",
            {"marker", "BSDD08"},
            {"DDiskId", DDiskId},
            {"msg", msg});

        if (auto partIt = ReadPartCallbacks.find(ev->Cookie); partIt != ReadPartCallbacks.end()) {
            const auto [parentCookie, index] = partIt->second;
            auto parent = ReadCallbacks.find(parentCookie);
            Y_ABORT_UNLESS(parent != ReadCallbacks.end());
            auto& read = static_cast<TReadPartsIoOp&>(*parent->second.Op);
            if (msg.Status != NKikimrProto::OK
                    && (!read.GetFallbackPart(index).Id
                        || msg.Status == NKikimrProto::INVALID_OWNER
                        || msg.Status == NKikimrProto::INVALID_ROUND)) {
                // Match standalone data reads and stop on explicit session loss.
                // Stopping cancels the parent and invalidates all callback maps.
                CheckPDiskReply(msg.Status, msg.ErrorReason, "Handle(TEvChunkReadRawResult parts)");
                return;
            }
            ReadPartCallbacks.erase(partIt);
            const i64 result = msg.Status == NKikimrProto::OK && !IsBroken()
                ? static_cast<i64>(msg.Data.size()) : -EIO;
            read.SetFallbackPartResult(index, result, std::move(msg.Data));
            if (!--ReadPartsRemaining.at(parentCookie)) {
                ReadPartsRemaining.erase(parentCookie);
                auto op = std::move(parent->second.Op);
                ReadCallbacks.erase(parent);
                read.FinishFallbackReadParts();
                op.release()->OnComplete(TActivationContext::ActorSystem());
            }
            return;
        }

        auto it = ReadCallbacks.find(ev->Cookie);
        if (it == ReadCallbacks.end()) {
            Y_ABORT_UNLESS(IsBroken());
            return;
        }

        if (Y_UNLIKELY(IsBroken())) {
            std::unique_ptr<TDirectIoOpBase> op = std::move(it->second.Op);
            ReadCallbacks.erase(it);
            op->SetResult(-EIO);
            op.release()->OnComplete(TActivationContext::ActorSystem());
            return;
        }

        if (msg.Status != NKikimrProto::OK) {
            if (it->second.Op->IsCriticalDDiskIo() || it->second.Op->IsRestoreIo()) {
                // Complete fallback integrity and PB restore reads through the same path as an io_uring EIO.
                // TEvIntegrityIoResult will latch Broken and fail every joined client request.
                std::unique_ptr<TDirectIoOpBase> op = std::move(it->second.Op);
                ReadCallbacks.erase(it);
                op->SetResult(-EIO);
                op.release()->OnComplete(TActivationContext::ActorSystem());
                return;
            }
            if (!CheckPDiskReply(msg.Status, msg.ErrorReason, "Handle(TEvChunkReadRawResult)")) {
                return;
            }
        }

        std::unique_ptr<TDirectIoOpBase> op = std::move(it->second.Op);
        ReadCallbacks.erase(it);

        // fill the op with result and finish via common completion path
        Y_DEBUG_ABORT_UNLESS(op->GetTotalSize() <= static_cast<ui64>(Max<i32>()));
        op->SetResult(static_cast<i32>(op->GetTotalSize()), std::move(msg.Data));

        op.release()->OnComplete(TActivationContext::ActorSystem());
    }

    void TDDiskActor::DirectUringOpImpl(std::unique_ptr<TDirectIoOpBase>& op) {
#if defined(__linux__)
        Y_ABORT_UNLESS(UringRouter);

        // The router may complete the operation on its I/O thread before the
        // submission call returns. Transfer ownership and publish the running
        // counter before making the call, and do not touch rawOp after acceptance.
        TDirectIoOpBase* rawOp = op.release();
        Counters.DirectIO.RunningCount->Inc();
        DirectIoState.fetch_add(1, std::memory_order_relaxed);

        if (rawOp->IsReadPartsIo()) {
            if (!static_cast<TReadPartsIoOp*>(rawOp)->SubmitParts(*UringRouter, TActivationContext::ActorSystem())) {
                Send(SelfId(), new TEvPrivate::TEvBeginStopping);
            }
            return;
        }

        bool accepted = false;
        switch (rawOp->GetOperationType()) {
        case NPDisk::TUringOperationBase::EREAD:
            accepted = UringRouter->Read(rawOp);
            break;
        case NPDisk::TUringOperationBase::EWRITE:
            accepted = UringRouter->Write(rawOp);
            break;
        default:
            Y_ABORT("Unknown OperationType");
        }

        if (Y_UNLIKELY(!accepted)) {
            // StopAsync() makes rejection expected while PDisk is shutting
            // down. Submit() did not take ownership, so restore it and fail on
            // the actor thread; OnDrop() is reserved for accepted operations
            // and would violate the I/O-thread producer side of the op pool.
            op.reset(rawOp);
            FailDirectIoOp(std::move(op), "io_uring router stopped before submission");
            Send(SelfId(), new TEvPrivate::TEvBeginStopping);
        }
#else
        Y_UNUSED(op);
        Y_ABORT("DirectUringOpImpl is only available on Linux");
#endif
    }

    void TDDiskActor::DirectUringOp(std::unique_ptr<TDirectIoOpBase>& op, bool isRetry) {
        Y_ABORT_UNLESS(!Stopping);
        if (Y_UNLIKELY(IsBroken())) {
            if (isRetry) {
                switch (op->GetOperationType()) {
                    case NPDisk::TUringOperationBase::EREAD:
                        Counters.DirectIO.Read.Done(op->GetAccountingSize());
                        break;
                    case NPDisk::TUringOperationBase::EWRITE:
                        Counters.DirectIO.Write.Done(op->GetAccountingSize());
                        break;
                    default:
                        Y_ABORT("Unknown OperationType");
                }
            }
            op->Reply(TActivationContext::ActorSystem(),
                NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR, GetBrokenReason());
            op.reset();
            return;
        }

        if (Y_LIKELY(!isRetry)) {
            switch (op->GetOperationType()) {
            case NPDisk::TUringOperationBase::EREAD:
                Counters.DirectIO.Read.Request(op->GetAccountingSize());
                break;
            case NPDisk::TUringOperationBase::EWRITE:
                Counters.DirectIO.Write.Request(op->GetAccountingSize());
                break;
            default:
                Y_ABORT("Unknown OperationType");
            }
        }

#if defined(__linux__)
        if (Y_LIKELY(UringRouter)) {
            DirectUringOpImpl(op);
            return;
        }
#endif

        Counters.DirectIO.RunningCount->Inc();

        // fallback path: either not linux or uring disabled / not available
        switch (op->GetOperationType()) {
        case NPDisk::TUringOperationBase::EREAD:
            SendPDiskRead(std::move(op));
            return;
        case NPDisk::TUringOperationBase::EWRITE:
            SendPDiskWrite(std::move(op));
            return;
        default:
            Y_ABORT("Unknown OperationType");
        }
    }

    TDDiskActor::TEvPrivate::TEvRetryIO::TEvRetryIO(std::unique_ptr<TDirectIoOpBase> op)
        : Op(std::move(op))
    {}

    TDDiskActor::TEvPrivate::TEvRetryIO::~TEvRetryIO() = default;

    void TDDiskActor::CancelPendingIo(std::unique_ptr<TDirectIoOpBase> op) {
        using TStatus = NKikimrBlobStorage::NDDisk::TReplyStatus;
        switch (op->GetOperationType()) {
        case NPDisk::TUringOperationBase::EREAD:
            Counters.DirectIO.Read.Done(op->GetAccountingSize());
            break;
        case NPDisk::TUringOperationBase::EWRITE:
            Counters.DirectIO.Write.Done(op->GetAccountingSize());
            break;
        default:
            Y_ABORT("Unknown OperationType");
        }
        op->Reply(TActivationContext::ActorSystem(), Stopping ? TStatus::SESSION_MISMATCH : TStatus::ERROR,
            Stopping ? TString(StoppingReason) : GetBrokenReason());
        // This is the actor thread, not the SPSC return-pool producer.
    }

    void TDDiskActor::CancelRetries() {
        while (!DelayedRetries.empty()) {
            auto it = DelayedRetries.begin();
            auto op = std::move(it->second.Op);
            DelayedRetries.erase(it);
            CancelPendingIo(std::move(op));
        }
    }

    void TDDiskActor::HandleRetryIO(TEvPrivate::TEvRetryIO::TPtr ev) {
        auto op = std::move(ev->Get()->Op);
        if (Stopping || IsBroken()) {
            CancelPendingIo(std::move(op));
            return;
        }
        Y_ABORT_UNLESS(op->RetryCount && op->RetryCount <= TDirectIoOpBase::MaxResubmissions);
        const auto delay = TDuration::MilliSeconds(Min<ui32>(1u << (op->RetryCount - 1), 100));
        const ui64 id = ++NextRetryId;
        DelayedRetries.emplace(id, TPendingIoOp(std::move(op)));
        Schedule(delay, new TEvPrivate::TEvRetryIODelayed(id));
    }

    void TDDiskActor::HandleRetryIODelayed(TEvPrivate::TEvRetryIODelayed::TPtr ev) {
        const auto it = DelayedRetries.find(ev->Get()->Id);
        if (it == DelayedRetries.end()) {
            return;
        }
        auto op = std::move(it->second.Op);
        DelayedRetries.erase(it);
        if (Stopping || IsBroken()) {
            CancelPendingIo(std::move(op));
            return;
        }
        DirectUringOp(op, /*isRetry=*/true);
    }

    void TDDiskActor::HandleWakeup(TEvents::TEvWakeup::TPtr &ev) {
        switch (ev->Get()->Tag) {
            case EWakeupTag::WakeupUpdateFreeSpaceInfo: {
                UpdateFreeSpaceInfo();
                break;
            }
            case EWakeupTag::WakeupCollectPbStats: {
                CollectPbStatsSnapshot();
                break;
            }
            case EWakeupTag::WakeupProcessPersistentBufferBatchWrite: {
                ProcessPersistentBufferBatchWrite();
                break;
            }
            case EWakeupTag::WakeupProcessDeallocatePersistentBufferChunk: {
                ProcessDeallocatePersistentBufferChunk(true);
                break;
            }
        }
    }

} // NKikimr::NDDisk
