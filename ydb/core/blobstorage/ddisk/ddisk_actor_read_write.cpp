#include "ddisk_actor.h"
#include "direct_io_op.h"

#include <ydb/core/blobstorage/pdisk/blobstorage_pdisk_data.h>

#include <util/generic/overloaded.h>
#include <ydb/core/util/stlog.h>

#include <cerrno>
#include <util/generic/scope.h>
#include <ydb/library/actors/async/wait_for_event.h>

namespace NKikimr::NDDisk {

    using TStatus = NKikimrBlobStorage::NDDisk::TReplyStatus;

    TDDiskActor::TPendingIoOp::TPendingIoOp(std::unique_ptr<TDirectIoOpBase> op)
        : Op(std::move(op))
    {}

    TDDiskActor::TPendingIoOp::TPendingIoOp(TPendingIoOp&&) noexcept = default;
    TDDiskActor::TPendingIoOp& TDDiskActor::TPendingIoOp::operator=(TPendingIoOp&&) noexcept = default;
    TDDiskActor::TPendingIoOp::~TPendingIoOp() = default;

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // TDDiskActor::TIoBatch
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    TDDiskActor::TIoBatch::TIoBatch(TDDiskActor& self)
        : ActorSystem(TActivationContext::ActorSystem())
        , DDiskId(self.SelfId())
        , Cookie(NActors::AllocateWaitCookie())
    {}

    void TDDiskActor::TIoBatch::Done() noexcept {
        if (Pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // This runs on the io_uring thread, where the mailbox is the only way back in.
            ActorSystem->Send(new IEventHandle(DDiskId, {},
                new TEvPrivate::TEvIoBatchDone, 0, Cookie));
        }
    }

    bool TDDiskActor::TIoBatch::Release() noexcept {
        // Releases the submission guard. From here a completion can only reach the frame as a
        // mailbox event, which cannot be handled before Suspend has registered for it.
        return Pending.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // PDisk fallback submission
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Write
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    void TDDiskActor::Handle(TEvWrite::TPtr ev) {
        YDB_LOG_TRACE_COMP(BS_DDISK, "TDDiskActor::Handle(TEvWrite)",
            {"marker", "BSDD50"},
            {"DDiskId", DDiskId},
            {"sender", ev->Sender},
            {"cookie", ev->Cookie});

        TQueryCredentials creds;
        if (!CheckQueryImpl<false>(*ev, &Counters.Interface.Write, creds)) {
            return;
        }

        const auto& record = ev->Get()->Record;
        const TQueryCredentials originalCredentials(record.GetCredentials());
        const TBlockSelector selector(record.GetSelector());
        const TWriteInstruction instr(record.GetInstruction());

        if (TabletChunkDeletionsInFlight.contains(creds.TabletId)) {
            Counters.Interface.Write.Request(selector.Size);
            Counters.Interface.Write.Reply(false, selector.Size);
            SendReply(*ev, std::make_unique<TEvWriteResult>(
                TStatus::BUSY,
                "tablet chunk deletion is in flight"));
            return;
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
                TStatus::INCORRECT_REQUEST,
                "write offset and size must be aligned to 4 KiB"));
            return;
        }

        if (Config.EnableChecksums) {
            if (!HasRequiredBlockChecksums(record.ChecksumsSize(), selector.OffsetInBytes, selector.Size)) {
                if (record.ChecksumsSize() == 0) {
                    Counters.Checksums.WritesWithoutChecksums->Inc();
                }
                Counters.Interface.Write.Request(selector.Size);
                Counters.Interface.Write.Reply(false, selector.Size);
                SendReply(*ev, std::make_unique<TEvWriteResult>(
                    TStatus::INCORRECT_REQUEST,
                    "one checksum per aligned 4 KiB block is required"));
                return;
            }

            Y_ABORT_UNLESS(instr.PayloadId, "TEvWrite without a payload, but with checksums");

            if (Config.CheckChecksumBeforeWrite) {
                const TRope& payload = ev->Get()->GetPayload(*instr.PayloadId);
                if (const auto result = ValidatePayloadChecksums(record, payload)) {
                    const bool isCorrupted = result->Status == TStatus::CORRUPTED;
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
                    return;
                }
            }
        }

        Counters.Interface.Write.Request(selector.Size);

        TDataWrite write;
        write.OriginalCredentials = originalCredentials;
        write.ResolvedCredentials = creds;
        write.Selector = selector;
        if (instr.PayloadId) {
            write.Data = ev->Get()->GetPayload(*instr.PayloadId);
        }
        Y_ABORT_UNLESS(write.Data.size() == selector.Size);
        write.Checksums.assign(record.GetChecksums().begin(), record.GetChecksums().end());
        write.Reply = {ev->Sender, ev->InterconnectSession, ev->Cookie};
        write.StartTs = HPNow();

        write.Span = NWilson::TSpan(TWilson::DDiskTopLevel, std::move(ev->TraceId), "DDisk.Write",
                NWilson::EFlags::NONE, TActivationContext::ActorSystem());
        NPrivate::AddMessageWaitAttributes(write.Span);
        write.Span
            .Attribute("tablet_id", static_cast<i64>(creds.TabletId))
            .Attribute("vchunk_index", static_cast<i64>(selector.VChunkIndex))
            .Attribute("offset_in_bytes", selector.OffsetInBytes)
            .Attribute("size", selector.Size);

        // THashMap keeps references stable; the pin forbids deleting this entry while the
        // write coroutine owns it. The coroutine releases it after replying.
        ++ChunkRefs[creds.TabletId][selector.VChunkIndex].ChunkRefPins;
        // The write owns every input it needs beyond this turn, including the original token.
        ev.Reset(nullptr);
        ExecuteDataWrite(std::move(write));
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

        TDataWrite write;
        write.OriginalCredentials = parent->OriginalCredentials;
        write.ResolvedCredentials = parent->Creds;
        write.Selector = source.Selector;
        write.Data = std::move(data);
        write.Checksums = std::move(checksums);
        write.StartTs = HPNow();
        write.SyncId = syncId;
        write.SyncInput = input;

        // The parent's pin transfers to the write without a gap, so tablet deletion cannot
        // remove this chunk entry in between.
        if (source.ChunkRefPinned) {
            source.ChunkRefPinned = false;
        } else {
            ++ChunkRefs[parent->Creds.TabletId][source.Selector.VChunkIndex].ChunkRefPins;
        }
        if (source.MetadataStarted) {
            // The prefetch owned the completion callbacks; the coroutine waits on the
            // operations directly instead.
            source.Metadata.Ready.SetCompletionCallback({});
            source.Metadata.Durable.SetCompletionCallback({});
            write.Metadata = std::move(source.Metadata);
            source.MetadataStarted = false;
        }
        source.DestinationStarted = true;
        ExecuteDataWrite(std::move(write));
    }

    void TDDiskActor::ExecuteDataWrite(TDataWrite write) {
        const ui64 tabletId = write.ResolvedCredentials.TabletId;
        const ui64 vChunkIndex = write.Selector.VChunkIndex;
        TChunkRef& chunkRef = ChunkRefs.at(tabletId).at(vChunkIndex);
        // The caller pinned the entry for this coroutine. The guard also releases it when
        // forced teardown destroys the frame.
        Y_DEFER { --chunkRef.ChunkRefPins; };

        TDataRequestGuard requestGuard(*this);
        TIoBatch batch(*this);
        auto status = TStatus::OK;
        TString error;
        std::optional<TDataIoPin> dataPin;
        bool dataSubmitted = false;
        bool metadataConsumed = false;

        auto onData = [&](TIoCompletion&& completion) noexcept {
            status = completion.Status;
            error = std::move(completion.ErrorMessage);
            batch.Done();
        };

        auto sessionLost = [&] {
            TQueryCredentials current;
            return ResolveConnection(write.OriginalCredentials, &current) != EConnectionResolution::Resolved
                || current.TabletId != tabletId
                || current.Generation != write.ResolvedCredentials.Generation
                || current.DDiskSessionSeqNo != write.ResolvedCredentials.DDiskSessionSeqNo
                || current.DirectBlockGroupIndex != write.ResolvedCredentials.DirectBlockGroupIndex
                || current.DDiskInstanceGuid != write.ResolvedCredentials.DDiskInstanceGuid;
        };

        auto failNow = [&] {
            status = IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH;
            error = IsBroken() ? GetBrokenReason() : TString(StoppingReason);
        };

        // Submission phase. Leaving it early always publishes a failure status, except for an
        // already failed metadata preparation, whose outcome is reported below.
        do {
            if (Stopping || IsBroken()) {
                failNow();
                break;
            }
            if (sessionLost()) {
                status = TStatus::SESSION_MISMATCH;
                error = "session replaced while write was waiting";
                break;
            }

            if (!chunkRef.ChunkIdx && !chunkRef.AllocationPending) {
                IssueChunkAllocation(tabletId, vChunkIndex);
            }
            // Allocation may complete synchronously from the reserve, and notifications are
            // not sticky, so always check the state before subscribing.
            while (chunkRef.AllocationPending && !Stopping && !IsBroken()) {
                auto waiter = chunkRef.AllocationReady.Wait();
                co_await NonCancellable(waiter);
            }
            if (!chunkRef.ChunkIdx) {
                failNow();
                break;
            }

            if (Config.EnableChecksums) {
                if (!write.Metadata) {
                    write.Metadata = IntegrityManager->PrepareWrite({tabletId, vChunkIndex},
                        write.Selector.OffsetInBytes, write.Selector.Size);
                }
                // Ready means every pair covering this range can be mutated.
                while (!write.Metadata->Ready.IsDone()) {
                    auto waiter = write.Metadata->Ready.WaitChanged();
                    co_await NonCancellable(waiter);
                }
                const auto& ready = *write.Metadata->Ready.GetResult();
                if (ready.Status != TIntegrityManager::EOperationStatus::Ok) {
                    status = ready.Status == TIntegrityManager::EOperationStatus::Corrupted
                        ? TStatus::CORRUPTED : TStatus::SESSION_MISMATCH;
                    error = ready.ErrorReason;
                    break;
                }
            }

            // A connection can change while the metadata loads are pending.
            if (sessionLost()) {
                status = TStatus::SESSION_MISMATCH;
                error = "session replaced while write was waiting";
                break;
            }

            if (Config.EnableChecksums) {
                metadataConsumed = true;
                IntegrityManager->ConsumeWrite(write.Metadata->Id, std::move(write.Checksums));
                const auto* immediate = write.Metadata->Durable.GetResult();
                if (immediate && immediate->Status != TIntegrityManager::EOperationStatus::Ok) {
                    // The metadata outcome below carries the reason.
                    break;
                }
            }
            if (Stopping || IsBroken()) {
                failNow();
                break;
            }

            dataPin.emplace(chunkRef);
            auto writeOp = AllocateOp<TDDiskIoOp>();
            writeOp->SetCallback(onData);
            writeOp->PrepareWrite(std::move(write.Data),
                DiskFormat->Offset(chunkRef.ChunkIdx, 0, write.Selector.OffsetInBytes),
                chunkRef.ChunkIdx, write.Selector.OffsetInBytes);
            std::unique_ptr<TDirectIoOpBase> op = std::move(writeOp);
            dataSubmitted = true;
            batch.Add();
            DirectUringOp(op);
        } while (false);

        co_await batch.Wait();
        dataPin.reset();

        if (write.Metadata) {
            if (!metadataConsumed) {
                // Nothing was applied, so the preparation still pins pair versions.
                IntegrityManager->CancelWrite(write.Metadata->Id);
            }
            // Durable completes only after every pair version this write depends on persists.
            while (!write.Metadata->Durable.IsDone()) {
                auto waiter = write.Metadata->Durable.WaitChanged();
                co_await NonCancellable(waiter);
            }
            const auto& outcome = *write.Metadata->Durable.GetResult();
            CountIntegrityResult(outcome);
            // A write that already failed keeps its own reason: a cancelled preparation
            // reports a generic metadata failure that says nothing about the cause.
            if (outcome.Status != TIntegrityManager::EOperationStatus::Ok
                    && (status == TStatus::OK
                        || outcome.Status == TIntegrityManager::EOperationStatus::Corrupted))
            {
                status = outcome.Status == TIntegrityManager::EOperationStatus::Corrupted
                    ? TStatus::CORRUPTED : TStatus::SESSION_MISMATCH;
                error = outcome.ErrorReason;
            }
        }

        // A write that touched the chunk may only be acknowledged once the chunk-map
        // increment carrying its extent placement is durable.
        if (dataSubmitted || metadataConsumed) {
            while (!IsChunkCommitted(tabletId, vChunkIndex) && !Stopping && !IsBroken()) {
                auto waiter = chunkRef.CommitReady.Wait();
                co_await NonCancellable(waiter);
            }
            if (!IsChunkCommitted(tabletId, vChunkIndex)) {
                status = TStatus::SESSION_MISMATCH;
                error = TString(StoppingReason);
            }
        }

        if (write.SyncId) {
            CompleteSyncDestination(write.SyncId, write.SyncInput, status, std::move(error));
        } else {
            FinishDDiskWrite(write, status, std::move(error));
        }

        requestGuard.Release();
        if (Stopping && !GetDirectIoInflight()) {
            FinishStopping();
        }
    }

    void TDDiskActor::FinishDDiskWrite(TDataWrite& write, TStatus::E status, TString error) {
        if (Y_UNLIKELY(IsBroken())) {
            status = TStatus::ERROR;
            error = GetBrokenReason();
        }
        const bool ok = status == TStatus::OK;
        auto reply = std::make_unique<TEvWriteResult>(status,
            error ? std::optional<TString>(std::move(error)) : std::nullopt);
        Counters.Interface.Write.Reply(ok, write.Selector.Size,
            HPMilliSecondsFloat(HPNow() - write.StartTs));
        auto h = std::make_unique<IEventHandle>(write.Reply.OriginalRequester, SelfId(), reply.release(),
            0, write.Reply.Cookie, nullptr, write.Span.GetTraceId());
        if (write.Reply.InterconnectSession) {
            h->Rewrite(TEvInterconnect::EvForward, write.Reply.InterconnectSession);
        }
        write.Span.End();
        TActivationContext::Send(h.release());
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

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Read
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    void TDDiskActor::Handle(TEvRead::TPtr ev) {
        YDB_LOG_TRACE_COMP(BS_DDISK, "TDDiskActor::Handle(TEvRead)",
            {"marker", "BSDD21"},
            {"DDiskId", DDiskId},
            {"msg", ev->Get()->Record});

        TQueryCredentials creds;
        if (!CheckQuery(*ev, &Counters.Interface.Read, creds)) {
            return;
        }

        const TBlockSelector selector(ev->Get()->Record.GetSelector());

        if (selector.OffsetInBytes % IntegrityUnitSize != 0
                || selector.Size % IntegrityUnitSize != 0) {
            Counters.Interface.Read.Request(selector.Size);
            Counters.Interface.Read.Reply(false, selector.Size);
            SendReply(*ev, std::make_unique<TEvReadResult>(
                TStatus::INCORRECT_REQUEST,
                "read offset and size must be aligned to the 4 KiB integrity unit"));
            return;
        }

        if (TabletChunkDeletionsInFlight.contains(creds.TabletId)) {
            Counters.Interface.Read.Request(selector.Size);
            Counters.Interface.Read.Reply(false, selector.Size);
            SendReply(*ev, std::make_unique<TEvReadResult>(
                TStatus::BUSY,
                "tablet chunk deletion is in flight"));
            return;
        }

        // A read never waits for chunk allocation. Until placement publishes a physical chunk -
        // including while another request's allocation for this virtual chunk is still pending -
        // the range was never written and reads as zeroes. No write can have completed into an
        // unpublished chunk, because every write awaits that publication first.
        TChunkRef* publishedChunk = nullptr;
        if (const auto tabletIt = ChunkRefs.find(creds.TabletId); tabletIt != ChunkRefs.end()) {
            const auto chunkIt = tabletIt->second.find(selector.VChunkIndex);
            if (chunkIt != tabletIt->second.end() && chunkIt->second.ChunkIdx
                    && !chunkIt->second.AllocationPending) {
                publishedChunk = &chunkIt->second;
            }
        }

        Counters.Interface.Read.Request(selector.Size);

        if (!publishedChunk) {
            auto zero = TRcBuf::Uninitialized(selector.Size);
            memset(zero.GetDataMut(), 0, zero.size());
            TRope result(std::move(zero));
            auto reply = std::make_unique<TEvReadResult>(
                TStatus::OK, std::nullopt, std::move(result));
            if (Config.EnableChecksums) {
                const ui64 checksum = GetZeroBlockChecksum();
                const ui32 blocks = selector.Size / IntegrityUnitSize;
                reply->Record.MutableChecksums()->Reserve(static_cast<int>(blocks));
                for (ui32 block = 0; block < blocks; ++block) {
                    reply->Record.AddChecksums(checksum);
                }
            }
            Counters.Interface.Read.Reply(true, selector.Size, 0);
            SendReply(*ev, std::move(reply));
            return;
        }

        TDataRead read;
        read.ResolvedCredentials = creds;
        read.Selector = selector;
        read.Reply = {ev->Sender, ev->InterconnectSession, ev->Cookie};
        read.StartTs = HPNow();

        read.Span = NWilson::TSpan(TWilson::DDiskTopLevel, std::move(ev->TraceId), "DDisk.Read",
            NWilson::EFlags::NONE, TActivationContext::ActorSystem());
        NPrivate::AddMessageWaitAttributes(read.Span);
        read.Span.Attribute("tablet_id", static_cast<i64>(creds.TabletId))
            .Attribute("vchunk_index", static_cast<i64>(selector.VChunkIndex))
            .Attribute("offset_in_bytes", selector.OffsetInBytes).Attribute("size", selector.Size);

        // THashMap keeps references stable; the pin forbids deleting this entry while the
        // read coroutine owns it. The coroutine releases it after replying.
        ++publishedChunk->ChunkRefPins;
        // The read owns every input it needs beyond this turn, including the reply route.
        ev.Reset(nullptr);
        ExecuteDataRead(std::move(read));
    }

    void TDDiskActor::ExecuteDataRead(TDataRead read) {
        const ui64 tabletId = read.ResolvedCredentials.TabletId;
        const TBlockSelector& selector = read.Selector;
        TChunkRef& chunkRef = ChunkRefs.at(tabletId).at(selector.VChunkIndex);
        // The caller pinned the entry for this coroutine. The guard also releases it when
        // forced teardown destroys the frame.
        Y_DEFER { --chunkRef.ChunkRefPins; };

        TDDiskReadResult result;
        result.TotalSize = selector.Size;

        TDataRequestGuard requestGuard(*this);
        TDataIoPin dataPin(chunkRef);
        const TChunkIdx chunkIdx = chunkRef.ChunkIdx;

        TIoBatch batch(*this);
        auto onData = [&](TIoCompletion&& completion) noexcept {
            result.Status = completion.Status;
            result.ErrorMessage = std::move(completion.ErrorMessage);
            result.Data = std::move(completion.Data);
            batch.Done();
        };

        TPairLoads loads;
        auto makePairCallback = [&](size_t index) {
            // Each completion owns its own slot, which is what makes concurrent
            // io_uring threads writing into this frame safe.
            return [&loads, &batch, index](TIoCompletion&& completion) noexcept {
                auto& slot = loads.Results[index];
                slot.Result.Ok = completion.Status == TStatus::OK;
                if (slot.Result.Ok) {
                    slot.Result.Data = std::move(completion.Data);
                } else {
                    loads.Errors[index] = std::move(completion.ErrorMessage);
                }
                batch.Done();
            };
        };
        std::vector<decltype(makePairCallback(0))> pairCallbacks;

        TIntegrityManager::TOperation pending;
        std::optional<TIntegrityManager::TOperationResult> warmMetadata;

        do {
            if (!Config.EnableChecksums) {
                SubmitDataRead(batch, onData, chunkIdx, selector, result);
                co_await batch.Wait();
                break;
            }

            auto preparation = IntegrityManager->PrepareRead({tabletId, selector.VChunkIndex},
                selector.OffsetInBytes, selector.Size, warmMetadata);

            // A known failure or an all-zero range is answered without touching the device.
            if (warmMetadata && (warmMetadata->Status != TIntegrityManager::EOperationStatus::Ok
                    || warmMetadata->ReadPlan.Kind == TIntegrityManager::TReadPlan::AllZero)) {
                ApplyDDiskReadMetadata(result, std::move(*warmMetadata));
                break;
            }
            if (warmMetadata) {
                SubmitDataRead(batch, onData, chunkIdx, selector, result);
                co_await batch.Wait();
                ApplyDDiskReadMetadata(result, std::move(*warmMetadata));
                break;
            }

            // Cold metadata. This request owns the pair loads nobody else started yet;
            // the rest of its range is joined through the shared operation.
            pending = std::move(preparation.Pending);
            *Counters.Checksums.IntegrityPairReads += preparation.Reads.size();
            loads.Results.resize(preparation.Reads.size());
            loads.Errors.resize(preparation.Reads.size());
            pairCallbacks.reserve(preparation.Reads.size());
            for (size_t i = 0; i < preparation.Reads.size(); ++i) {
                loads.Results[i].Id = preparation.Reads[i].Id;
                pairCallbacks.push_back(makePairCallback(i));
            }
            // A large read pays for the extra round trip to avoid reading data it may
            // discard; a small one speculates and issues both at once. Requested blocks
            // are not written concurrently: a neighboring write may change the same pair,
            // but cannot turn a known hole in this range into used data.
            const bool metadataFirst = selector.Size >= MetadataFirstReadThreshold;
            if (!metadataFirst
                    && IntegrityManager->MakeReadPlan({tabletId, selector.VChunkIndex},
                        selector.OffsetInBytes, selector.Size).Kind
                            != TIntegrityManager::TReadPlan::AllZero) {
                SubmitDataRead(batch, onData, chunkIdx, selector, result);
            }
            for (size_t i = 0; i < preparation.Reads.size(); ++i) {
                SubmitPairRead(batch, pairCallbacks[i], preparation.Reads[i]);
            }
            co_await batch.Wait();

            CompleteMetadataReads(loads);
            while (!pending.IsDone()) {
                auto waiter = pending.WaitChanged();
                co_await NonCancellable(waiter);
            }

            if (metadataFirst) {
                const auto& metadata = *pending.GetResult();
                if (metadata.Status != TIntegrityManager::EOperationStatus::Ok
                        || metadata.ReadPlan.Kind == TIntegrityManager::TReadPlan::AllZero) {
                    ApplyDDiskReadMetadata(result, metadata);
                    break;
                }
                SubmitDataRead(batch, onData, chunkIdx, selector, result);
                co_await batch.Wait();
            }
            ApplyDDiskReadMetadata(result, *pending.GetResult());
        } while (false);

        FinishDDiskRead(read, result);
        requestGuard.Release();
        if (Stopping && !GetDirectIoInflight()) {
            FinishStopping();
        }
    }

    bool TDDiskActor::SubmitDataRead(TIoBatch& batch, TIoCallback callback, TChunkIdx chunkIdx,
            const TBlockSelector& selector, TDDiskReadResult& result)
    {
        if (Stopping || IsBroken()) {
            result.Status = IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH;
            result.ErrorMessage = IsBroken() ? GetBrokenReason() : TString(StoppingReason);
            return false;
        }
        auto readOp = AllocateOp<TDDiskIoOp>();
        readOp->SetCallback(callback);
        readOp->PrepareRead(selector.Size, DiskFormat->Offset(chunkIdx, 0, selector.OffsetInBytes),
            chunkIdx, selector.OffsetInBytes);
        std::unique_ptr<TDirectIoOpBase> op = std::move(readOp);
        // DirectUringOp always produces exactly one completion, inline when the disk is broken.
        batch.Add();
        DirectUringOp(op);
        return true;
    }

    bool TDDiskActor::SubmitPairRead(TIoBatch& batch, TIoCallback callback,
            const TIntegrityManager::TPairRead& read)
    {
        if (Stopping) {
            return false;
        }
        auto pairOp = AllocateOp<TDDiskIoOp>();
        pairOp->SetCritical();
        pairOp->SetCallback(callback);
        pairOp->PrepareRead(read.Size, DiskFormat->Offset(read.ChunkIdx, 0, read.OffsetInBytes),
            read.ChunkIdx, read.OffsetInBytes);
        std::unique_ptr<TDirectIoOpBase> op = std::move(pairOp);
        batch.Add();
        DirectUringOp(op);
        return true;
    }

    void TDDiskActor::CompleteMetadataReads(TPairLoads& loads) {
        if (loads.Results.empty()) {
            return;
        }
        bool failed = false;
        TString error;
        for (size_t i = 0; i < loads.Results.size(); ++i) {
            if (!loads.Results[i].Result.Ok) {
                failed = true;
                if (!error) {
                    error = std::move(loads.Errors[i]);
                }
            }
        }
        // Latch the failure before handing the images over: CompletePairReads resolves joined
        // reads, and none of them may observe a healthy disk after a critical load failed.
        if (failed && !Stopping) {
            EnterBroken(std::move(error));
        }
        IntegrityManager->CompletePairReads(loads.Results);
        loads.Results.clear();
        loads.Errors.clear();
        if (!Stopping && !IsBroken()) {
            RunIntegrityReclamation();
        }
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
                ? TStatus::CORRUPTED
                : TStatus::SESSION_MISMATCH;
            result.ErrorMessage = std::forward<TMetadata>(metadata).ErrorReason;
            return;
        }
        result.Checksums = std::forward<TMetadata>(metadata).Checksums;
        if (metadata.ReadPlan.Kind == TIntegrityManager::TReadPlan::AllZero) {
            auto zero = TRcBuf::Uninitialized(result.TotalSize);
            memset(zero.GetDataMut(), 0, zero.size());
            result.Data = TReadPayload(std::move(zero));
            result.Status = TStatus::OK;
            result.ErrorMessage.clear();
        } else if (result.Status == TStatus::OK
                && metadata.ReadPlan.Kind == TIntegrityManager::TReadPlan::Mixed) {
            auto data = result.Data.MutableSpan();
            for (size_t i = 0; i < data.size() / IntegrityUnitSize; ++i) {
                if (!metadata.ReadPlan.UsedBlocks.Get(i)) {
                    memset(data.data() + i * IntegrityUnitSize, 0, IntegrityUnitSize);
                }
            }
        }
    }

    void TDDiskActor::FinishDDiskRead(TDataRead& read, TDDiskReadResult& result) {
        auto status = result.Status;
        TString error = std::move(result.ErrorMessage);
        if (IsBroken()) {
            status = TStatus::ERROR;
            error = GetBrokenReason();
        }
        TRope data = std::move(result.Data).IntoRope();
        if (status == TStatus::OK
                && Config.EnableChecksums && Config.CheckChecksumWhenRead && !result.Checksums.View().empty()) {
            if (const auto failure = ValidatePayloadChecksums(result.Checksums.View(), data)) {
                status = failure->Status;
                error = failure->ErrorReason;
                if (status == TStatus::CORRUPTED) {
                    Counters.Checksums.ChecksumMismatch->Inc();
                }
                YDB_LOG_ERROR_COMP(NKikimrServices::BS_DDISK, "DDisk read checksum validation failed",
                    {"DDiskId", DDiskId}, {"tabletId", read.ResolvedCredentials.TabletId},
                    {"vChunkIndex", read.Selector.VChunkIndex}, {"reason", error});
            }
        }
        const bool ok = status == TStatus::OK;
        auto reply = std::make_unique<TEvReadResult>(status,
            error ? std::optional<TString>(std::move(error)) : std::nullopt,
            ok ? std::move(data) : TRope{}, ok ? result.Checksums.View() : TConstArrayRef<ui64>{});
        Counters.Interface.Read.Reply(ok, result.TotalSize, HPMilliSecondsFloat(HPNow() - read.StartTs));
        auto handle = std::make_unique<IEventHandle>(read.Reply.OriginalRequester, SelfId(), reply.release(),
            0, read.Reply.Cookie, nullptr, read.Span.GetTraceId());
        if (read.Reply.InterconnectSession) {
            handle->Rewrite(TEvInterconnect::EvForward, read.Reply.InterconnectSession);
        }
        read.Span.End();
        TActivationContext::Send(handle.release());
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
            // A metadata pair load matches a standalone data read on explicit session loss:
            // stopping cancels it instead of latching Broken.
            const bool sessionLost = it->second.Op->IsCriticalDDiskIo()
                && (msg.Status == NKikimrProto::INVALID_OWNER || msg.Status == NKikimrProto::INVALID_ROUND);
            if (!sessionLost && (it->second.Op->IsCriticalDDiskIo() || it->second.Op->IsRestoreIo())) {
                // Complete fallback integrity and PB restore reads through the same path as an io_uring EIO
                // so that Broken is latched before any joined client request can be answered.
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

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Submission and retries
    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

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
                TStatus::ERROR, GetBrokenReason());
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
