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

        if (!CheckQuery(*ev, &Counters.Interface.Write)) {
            co_return;
        }

        const auto& record = ev->Get()->Record;
        const TQueryCredentials creds(record.GetCredentials());
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

        if (!ChunkRefs[creds.TabletId][selector.VChunkIndex].ChunkIdx) {
            auto span = NWilson::TSpan(TWilson::DDiskTopLevel, NWilson::TTraceId(ev->TraceId),
                "WaitChunkAllocation", NWilson::EFlags::AUTO_END, TActivationContext::ActorSystem());
            NPrivate::AddMessageWaitAttributes(span);
            co_await WaitForChunk(creds.TabletId, selector.VChunkIndex, true);
            if (Stopping || IsBroken()) {
                RejectQuery(*ev, Stopping
                    ? NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH
                    : NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR,
                    Stopping ? TString(StoppingReason) : GetBrokenReason());
                co_return;
            }
            // The event belongs to this frame, but the session may have been replaced
            // while reservation or integrity-extent placement was in progress.
            if (!CheckQuery(*ev, &Counters.Interface.Write)) {
                co_return;
            }
        }
        if (Config.EnableChecksums) {
            if (!(co_await AcquireIntegrityExtent(creds.TabletId, selector.VChunkIndex))) {
                RejectQuery(*ev, Stopping ? NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH
                    : NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR,
                    Stopping ? TString(StoppingReason) : GetBrokenReason());
                co_return;
            }
            if (!CheckQuery(*ev, &Counters.Interface.Write)) {
                ReleaseIntegrityExtentWrite(creds.TabletId, selector.VChunkIndex);
                co_return;
            }
        }
        auto& chunkRef = ChunkRefs.at(creds.TabletId).at(selector.VChunkIndex);

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

        ++chunkRef.AllocationWaiters;
        Y_DEFER { --chunkRef.AllocationWaiters; };
        bool admitted = Config.EnableChecksums;
        Y_DEFER { if (admitted) { chunkRef.IntegrityExtentWriteInFlight = false; } };
        TIntegrityManager::TOperation metadata;
        if (Config.EnableChecksums) {
            metadata = IntegrityManager->StartWrite({creds.TabletId, selector.VChunkIndex},
                selector.OffsetInBytes, selector.Size,
                std::vector<ui64>(record.GetChecksums().begin(), record.GetChecksums().end()));
        }
        TEvPrivate::TEvDDiskIoResult result(NPDisk::TUringOperationBase::EWRITE,
            NKikimrBlobStorage::NDDisk::TReplyStatus::OK, {}, {}, ev->Sender,
            ev->InterconnectSession, ev->Cookie, std::move(span), selector.Size, 0,
            creds.TabletId, selector.VChunkIndex, true);
        const auto* immediate = metadata.GetResult();
        if (!immediate || immediate->Status == TIntegrityManager::EOperationStatus::Ok) {
            std::unique_ptr<TDirectIoOpBase> op = AllocateOp<TDDiskIoOp>(ev.Get());
            static_cast<TDDiskIoOp*>(op.get())->SetChunkKey(creds.TabletId, selector.VChunkIndex);
            op->PrepareWrite(std::move(data), DiskFormat->Offset(chunkRef.ChunkIdx, 0, selector.OffsetInBytes),
                chunkRef.ChunkIdx, selector.OffsetInBytes);
            auto event = co_await AwaitDataIo(std::move(op), creds.TabletId, selector.VChunkIndex);
            result.Status = event->Get()->Status;
            result.ErrorMessage = std::move(event->Get()->ErrorMessage);
        }
        if (Config.EnableChecksums) {
            auto integrity = co_await metadata.Wait(*this);
            CountIntegrityResult(integrity);
            if (integrity.Status != TIntegrityManager::EOperationStatus::Ok) {
                result.Status = integrity.Status == TIntegrityManager::EOperationStatus::Corrupted
                    ? NKikimrBlobStorage::NDDisk::TReplyStatus::CORRUPTED
                    : NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH;
                result.ErrorMessage = std::move(integrity.ErrorReason);
            }
            admitted = false;
            ReleaseIntegrityExtentWrite(creds.TabletId, selector.VChunkIndex);
        }
        if (!(co_await WaitForChunkCommit(creds.TabletId, selector.VChunkIndex))) {
            result.Status = NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH;
            result.ErrorMessage = TString(StoppingReason);
        }
        result.RequestTimeMs = HPMilliSecondsFloat(HPNow() - requestStartTs);
        FinishDDiskIoResult(result);
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

        if (!CheckQuery(*ev, &Counters.Interface.Read)) {
            co_return;
        }

        const auto& record = ev->Get()->Record;
        const TQueryCredentials creds(record.GetCredentials());
        const TBlockSelector selector(record.GetSelector());

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

        if (ChunkRefs[creds.TabletId][selector.VChunkIndex].AllocationPending) {
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
            if (!CheckQuery(*ev, &Counters.Interface.Read)) {
                co_return;
            }
        }
        TChunkRef& chunkRef = ChunkRefs.at(creds.TabletId).at(selector.VChunkIndex);

        Counters.Interface.Read.Request(selector.Size);

        // No chunk allocated: the whole range was never written.
        if (!chunkRef.ChunkIdx) {
            auto zero = TRcBuf::Uninitialized(selector.Size);
            memset(zero.GetDataMut(), 0, zero.size());
            TRope result(std::move(zero));
            std::vector<ui64> checksums;
            if (Config.EnableChecksums) {
                checksums.assign(selector.Size / IntegrityUnitSize, GetZeroBlockChecksum());
            }
            Counters.Interface.Read.Reply(true, selector.Size, 0);
            SendReply(*ev, std::make_unique<TEvReadResult>(
                NKikimrBlobStorage::NDDisk::TReplyStatus::OK, std::nullopt,
                std::move(result), checksums));
            co_return;
        }

        ++chunkRef.AllocationWaiters;
        Y_DEFER { --chunkRef.AllocationWaiters; };
        const auto start = HPNow();
        auto span = NWilson::TSpan(TWilson::DDiskTopLevel, std::move(ev->TraceId), "DDisk.Read",
            NWilson::EFlags::NONE, TActivationContext::ActorSystem());
        NPrivate::AddMessageWaitAttributes(span);
        span.Attribute("tablet_id", static_cast<i64>(creds.TabletId))
            .Attribute("vchunk_index", static_cast<i64>(selector.VChunkIndex))
            .Attribute("offset_in_bytes", selector.OffsetInBytes).Attribute("size", selector.Size);
        TEvPrivate::TEvDDiskIoResult result(NPDisk::TUringOperationBase::EREAD,
            NKikimrBlobStorage::NDDisk::TReplyStatus::OK, {}, {}, ev->Sender,
            ev->InterconnectSession, ev->Cookie, std::move(span), selector.Size, 0,
            creds.TabletId, selector.VChunkIndex, true);
        TIntegrityManager::TOperation metadata;
        bool needsData = true;
        if (Config.EnableChecksums) {
            metadata = IntegrityManager->StartRead({creds.TabletId, selector.VChunkIndex},
                selector.OffsetInBytes, selector.Size);
            const auto* immediate = metadata.GetResult();
            needsData = (!immediate || immediate->Status == TIntegrityManager::EOperationStatus::Ok)
                && IntegrityManager->MakeReadPlan({creds.TabletId, selector.VChunkIndex},
                    selector.OffsetInBytes, selector.Size).Kind != TIntegrityManager::TReadPlan::AllZero;
        }
        if (needsData) {
            co_await ReadDDiskData(*ev, chunkRef.ChunkIdx, selector.OffsetInBytes, selector.Size, result);
        }
        if (Config.EnableChecksums) {
            auto integrity = co_await metadata.Wait(*this);
            CountIntegrityResult(integrity);
            if (integrity.Status != TIntegrityManager::EOperationStatus::Ok) {
                result.Status = integrity.Status == TIntegrityManager::EOperationStatus::Corrupted
                    ? NKikimrBlobStorage::NDDisk::TReplyStatus::CORRUPTED
                    : NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH;
                result.ErrorMessage = std::move(integrity.ErrorReason);
            } else {
                // A writer sharing a cold pair load can turn a previously known hole into
                // used data before this read captures its metadata snapshot.
                if (!needsData && integrity.ReadPlan.Kind != TIntegrityManager::TReadPlan::AllZero) {
                    co_await ReadDDiskData(*ev, chunkRef.ChunkIdx, selector.OffsetInBytes, selector.Size, result);
                }
                result.Checksums = std::move(integrity.Checksums);
                if (integrity.ReadPlan.Kind == TIntegrityManager::TReadPlan::AllZero) {
                    auto zero = TRcBuf::Uninitialized(selector.Size);
                    memset(zero.GetDataMut(), 0, zero.size());
                    result.Data = TRope(std::move(zero));
                    result.Status = NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
                    result.ErrorMessage.clear();
                } else if (result.Status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK
                        && integrity.ReadPlan.Kind == TIntegrityManager::TReadPlan::Mixed) {
                    auto data = result.Data.UnsafeGetContiguousSpanMut();
                    for (size_t i = 0; i < data.size() / IntegrityUnitSize; ++i) {
                        if (!integrity.ReadPlan.UsedBlocks.Get(i)) {
                            memset(data.data() + i * IntegrityUnitSize, 0, IntegrityUnitSize);
                        }
                    }
                }
            }
        }
        result.RequestTimeMs = HPMilliSecondsFloat(HPNow() - start);
        FinishDDiskIoResult(result);
    }

    NActors::async<void> TDDiskActor::ReadDDiskData(IEventHandle& request, TChunkIdx chunkIdx,
            ui32 offset, ui32 size, TEvPrivate::TEvDDiskIoResult& result) {
        if (Stopping || IsBroken()) {
            result.Status = IsBroken() ? NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR
                : NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH;
            result.ErrorMessage = IsBroken() ? GetBrokenReason() : TString(StoppingReason);
            co_return;
        }
        std::unique_ptr<TDirectIoOpBase> op = AllocateOp<TDDiskIoOp>(&request);
        static_cast<TDDiskIoOp*>(op.get())->SetChunkKey(result.TabletId, result.VChunkIndex);
        op->PrepareRead(size, DiskFormat->Offset(chunkIdx, 0, offset), chunkIdx, offset);
        auto event = co_await AwaitDataIo(std::move(op), result.TabletId, result.VChunkIndex);
        result.Status = event->Get()->Status;
        result.ErrorMessage = std::move(event->Get()->ErrorMessage);
        result.Data = std::move(event->Get()->Data);
    }

    void TDDiskActor::FinishDDiskIoResult(TEvPrivate::TEvDDiskIoResult& msg) {
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
                    isOk ? msg.Checksums : std::vector<ui64>{});
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
                        Counters.DirectIO.Read.Done(op->GetTotalSize());
                        break;
                    case NPDisk::TUringOperationBase::EWRITE:
                        Counters.DirectIO.Write.Done(op->GetTotalSize());
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
                Counters.DirectIO.Read.Request(op->GetTotalSize());
                break;
            case NPDisk::TUringOperationBase::EWRITE:
                Counters.DirectIO.Write.Request(op->GetTotalSize());
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
            Counters.DirectIO.Read.Done(op->GetTotalSize());
            break;
        case NPDisk::TUringOperationBase::EWRITE:
            Counters.DirectIO.Write.Done(op->GetTotalSize());
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
