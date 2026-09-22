#include "ddisk_actor.h"
#include "direct_io_op.h"
#include <ydb/core/util/stlog.h>
#include <ydb/library/actors/async/cancellation.h>
#include <ydb/library/actors/async/task_group.h>
#include <ydb/library/actors/async/wait_for_event.h>
#include <util/generic/scope.h>

namespace NKikimr::NDDisk {
    using TStatus = NKikimrBlobStorage::NDDisk::TReplyStatus;

    void TDDiskActor::Handle(TEvSync::TPtr ev) {
        if (!CheckQuery(*ev, &Counters.Interface.Sync)) { co_return; }
        Counters.Interface.Sync.Request(0);
        const auto& record = ev->Get()->Record;
        TSyncInFlight sync;
        sync.Creds = TQueryCredentials(record.GetCredentials());
        auto reject = [&](TStatus::E status, TString reason) {
            Counters.Interface.Sync.Reply(false);
            SendReply(*ev, std::make_unique<TEvSyncResult>(status, std::move(reason)));
        };
        if (TabletChunkDeletionsInFlight.contains(sync.Creds.TabletId)) {
            reject(TStatus::BUSY, "tablet chunk deletion is in flight");
            co_return;
        }
        if (!record.SourcesSize()) {
            reject(TStatus::INCORRECT_REQUEST, "sources must be non-empty");
            co_return;
        }
        // Validate the complete request before source sends or overlap mutations.
        std::optional<ui64> vchunk;
        for (const auto& source : record.GetSources()) {
            if (!source.HasDDiskId()) {
                reject(TStatus::INCORRECT_REQUEST, "source ddisk id must be set");
                co_return;
            }
            const auto& id = source.GetDDiskId();
            const auto credentials = TQueryCredentials::ForInternal(sync.Creds.TabletId, sync.Creds.Generation,
                std::make_optional(source.GetDDiskInstanceGuid()), sync.Creds.DirectBlockGroupIndex);
            for (const auto& segment : source.GetSegments()) {
                const TBlockSelector selector(segment.GetSelector());
                if ((!segment.HasDDiskSegment() && !segment.HasPersistentBufferSegment())
                        || !selector.Size || selector.OffsetInBytes % IntegrityUnitSize || selector.Size % IntegrityUnitSize
                        || selector.OffsetInBytes > DiskFormat->ChunkSize
                        || selector.Size > DiskFormat->ChunkSize - selector.OffsetInBytes
                        || (vchunk && *vchunk != selector.VChunkIndex)) {
                    reject(TStatus::INCORRECT_REQUEST, "segments must have a kind and aligned nonempty ranges within one VChunk");
                    co_return;
                }
                vchunk = selector.VChunkIndex;
                auto& request = sync.Requests.emplace_back();
                request.Selector = selector;
                if (segment.HasPersistentBufferSegment()) {
                    request.Source = MakeBlobStoragePersistentBufferId(id.GetNodeId(), id.GetPDiskId(), id.GetDDiskSlotId());
                    request.Query = std::make_unique<TEvReadPersistentBuffer>(credentials, selector,
                        segment.GetPersistentBufferSegment().GetLsn(),
                        segment.GetPersistentBufferSegment().GetGeneration(), TReadInstruction(true));
                } else {
                    request.Source = MakeBlobStorageDDiskId(id.GetNodeId(), id.GetPDiskId(), id.GetDDiskSlotId());
                    request.Query = std::make_unique<TEvRead>(credentials, selector, TReadInstruction(true));
                }
            }
        }
        if (sync.Requests.empty()) {
            reject(TStatus::INCORRECT_REQUEST, "segments must be non-empty");
            co_return;
        }
        sync.VChunkIndex = *vchunk;
        const ui64 syncId = NextSyncId++;
        sync.Span = NWilson::TSpan(TWilson::DDiskTopLevel, std::move(ev->TraceId), "DDisk.Sync",
            NWilson::EFlags::NONE, TActivationContext::ActorSystem());
        NPrivate::AddMessageWaitAttributes(sync.Span);
        sync.Span.Attribute("tablet_id", static_cast<i64>(sync.Creds.TabletId)).Attribute("sync_id", static_cast<i64>(syncId));
        SyncsInFlight.emplace(syncId, &sync);
        Y_DEFER {
            for (const auto& request : sync.Requests) {
                std::vector<TSegmentManager::TSegment> removed;
                SegmentManager.PopRequest(request.RequestId, &removed);
            }
            SyncsInFlight.erase(syncId);
        };
        for (auto& request : sync.Requests) {
            std::vector<TSegmentManager::TOutdatedRequest> outdated;
            SegmentManager.PushRequest(sync.Creds.TabletId, sync.VChunkIndex, syncId,
                {request.Selector.OffsetInBytes, request.Selector.OffsetInBytes + request.Selector.Size},
                &request.RequestId, &outdated);
            for (const auto& old : outdated) {
                const auto it = SyncsInFlight.find(old.SyncIndex);
                if (it == SyncsInFlight.end()) { continue; }
                for (auto& previous : it->second->Requests) {
                    if (previous.RequestId == old.RequestId && previous.Status == TStatus::UNKNOWN) {
                        previous.Status = TStatus::OUTDATED;
                        previous.Preparation.Cancel();
                        break;
                    }
                }
            }
        }
        co_await NActors::WithTaskGroup([&](NActors::TTaskGroup<void>& group) -> NActors::async<void> {
            for (auto& request : sync.Requests) {
                group.Add([this, &sync, &request] { return RunSyncSource(sync, request); });
            }
            while (group.Running() || group.Ready()) { co_await group.Next(); }
        });
        const bool committed = co_await WaitForChunkCommit(sync.Creds.TabletId, sync.VChunkIndex);
        TStringBuilder errors;
        for (auto& request : sync.Requests) {
            if (IsBroken() || !committed) {
                request.Status = IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH;
                request.ErrorReason = IsBroken() ? GetBrokenReason() : TString(StoppingReason);
            }
            if (request.Status != TStatus::OK && request.Status != TStatus::OUTDATED) {
                errors << request.ErrorReason << "; ";
            }
        }
        const auto status = errors ? (Stopping && !IsBroken() ? TStatus::SESSION_MISMATCH : TStatus::ERROR) : TStatus::OK;
        auto reply = std::make_unique<TEvSyncResult>(status, errors);
        for (const auto& request : sync.Requests) { reply->AddSegmentResult(request.Status, request.ErrorReason); }
        Counters.Interface.Sync.Reply(!errors);
        sync.Span.End();
        SendReply(*ev, std::move(reply));
    }

    NActors::async<TDDiskActor::TSyncData> TDDiskActor::PrepareSyncSource(
            TSyncInFlight& sync, TSyncReadRequest& request) {
        auto fail = [&](TStatus::E status, TString reason) {
            // This range will never submit destination work. Retire it now so a later
            // overlap cannot replace its terminal failure while siblings or commit wait.
            std::vector<TSegmentManager::TSegment> removed;
            SegmentManager.PopRequest(request.RequestId, &removed);
            request.Status = status;
            request.ErrorReason = std::move(reason);
            return TSyncData{};
        };
        if (request.Status == TStatus::OUTDATED) { co_return TSyncData{}; }
        if (Stopping || IsBroken()) {
            co_return fail(IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH,
                IsBroken() ? GetBrokenReason() : TString(StoppingReason));
        }
        const ui64 cookie = NActors::AllocateWaitCookie();
        const bool pb = request.Query->Type() == TEvReadPersistentBuffer::EventType;
        SyncReadCookiesInFlight.insert(cookie);
        Send(request.Source, request.Query.release(), IEventHandle::FlagTrackDelivery, cookie, sync.Span.GetTraceId());
        auto event = co_await NActors::ActorWaitForEvent<IEventHandle>(cookie);
        SyncReadCookiesInFlight.erase(cookie);
        if (event->GetTypeRewrite() == TEvents::TEvUndelivered::EventType) {
            co_return fail(TStatus::ERROR, "source read event undelivered");
        }
        Y_ABORT_UNLESS(event->GetTypeRewrite() == (pb ? TEvReadPersistentBufferResult::EventType : TEvReadResult::EventType));
        TSyncData data;
        auto decode = [&](auto& msg) {
            const auto& record = msg.Record;
            if (record.GetStatus() != TStatus::OK) {
                fail(record.GetStatus(), record.GetErrorReason());
                return false;
            }
            data.Data = msg.GetPayload(0);
            if (data.Data.size() != request.Selector.Size) {
                fail(TStatus::INCORRECT_REQUEST, "source payload size does not match requested size");
                return false;
            }
            if (Config.EnableChecksums) {
                if (!HasRequiredBlockChecksums(record.ChecksumsSize(), request.Selector.OffsetInBytes, request.Selector.Size)) {
                    fail(TStatus::INCORRECT_REQUEST, "source read must return one checksum per aligned 4 KiB block");
                    return false;
                }
                if (const auto validation = Config.CheckChecksumBeforeWrite
                        ? ValidatePayloadChecksums(record, data.Data) : std::nullopt) {
                    if (validation->Status == TStatus::CORRUPTED) { Counters.Checksums.ChecksumMismatch->Inc(); }
                    fail(validation->Status, validation->ErrorReason);
                    return false;
                }
                data.Checksums.assign(record.GetChecksums().begin(), record.GetChecksums().end());
            }
            return true;
        };
        if (!(pb ? decode(*event->Get<TEvReadPersistentBufferResult>()) : decode(*event->Get<TEvReadResult>()))) {
            co_return TSyncData{};
        }
        auto valid = [&] {
            if (request.Status == TStatus::OUTDATED) { return false; }
            if (Stopping || IsBroken()) {
                fail(IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH,
                    IsBroken() ? GetBrokenReason() : TString(StoppingReason));
                return false;
            }
            TQueryCredentials current;
            if (ResolveConnection(sync.Creds, &current) != EConnectionResolution::Resolved) {
                fail(TStatus::SESSION_MISMATCH, "session replaced while sync was waiting");
                return false;
            }
            return true;
        };
        if (!valid()) { co_return TSyncData{}; }
        co_await WaitForChunk(sync.Creds.TabletId, sync.VChunkIndex, true);
        if (!valid()) { co_return TSyncData{}; }
        if (Config.EnableChecksums) {
            request.Admitted = co_await AcquireIntegrityExtent(sync.Creds.TabletId, sync.VChunkIndex);
            if (!valid() || !request.Admitted) { co_return TSyncData{}; }
        }
        SegmentManager.PopRequest(request.RequestId, &data.Segments);
        std::sort(data.Segments.begin(), data.Segments.end());
        co_return data;
    }

    NActors::async<void> TDDiskActor::RunSyncSource(TSyncInFlight& sync, TSyncReadRequest& request) {
        auto prepared = co_await request.Preparation.Wrap([this, &sync, &request] {
            return PrepareSyncSource(sync, request);
        });
        // Scope cancellation removes the FIFO ticket. Wake its successor on the actor,
        // rather than performing notifications from a forced-destruction RAII guard.
        auto& chunk = ChunkRefs[sync.Creds.TabletId][sync.VChunkIndex];
        chunk.ExtentAvailable.NotifyAll();
        bool admitted = request.Admitted;
        Y_DEFER { if (admitted) { chunk.IntegrityExtentWriteInFlight = false; } };
        if (prepared && !prepared->Segments.empty()) {
            auto& data = *prepared;
            co_await NActors::WithTaskGroup<TSyncSegmentResult>([&](auto& group) -> NActors::async<void> {
                ui32 consumed = request.Selector.OffsetInBytes;
                for (const auto& [begin, end] : data.Segments) {
                    data.Data.EraseFront(begin - consumed);
                    TRope segment;
                    data.Data.ExtractFront(end - begin, &segment);
                    consumed = end;
                    std::vector<ui64> checksums;
                    if (Config.EnableChecksums) {
                        const auto first = (begin - request.Selector.OffsetInBytes) / IntegrityUnitSize;
                        checksums.assign(data.Checksums.begin() + first, data.Checksums.begin() + first + (end - begin) / IntegrityUnitSize);
                    }
                    group.Add([this, &sync, begin, segment = std::move(segment), checksums = std::move(checksums)]() mutable {
                        return WriteSyncSegment(sync, begin, std::move(segment), std::move(checksums));
                    });
                }
                while (group.Running() || group.Ready()) {
                    auto result = co_await group.Next();
                    if (result.Status != TStatus::OK && request.Status == TStatus::UNKNOWN) {
                        request.Status = result.Status;
                        request.ErrorReason = std::move(result.ErrorReason);
                    }
                }
            });
            if (request.Status == TStatus::UNKNOWN) { request.Status = TStatus::OK; }
        } else if (!prepared && request.Status != TStatus::OUTDATED) {
            request.Status = IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH;
            request.ErrorReason = IsBroken() ? GetBrokenReason() : TString(StoppingReason);
        }
        if (admitted) {
            admitted = false;
            ReleaseIntegrityExtentWrite(sync.Creds.TabletId, sync.VChunkIndex);
        }
    }

    NActors::async<TDDiskActor::TSyncSegmentResult> TDDiskActor::WriteSyncSegment(
            TSyncInFlight& sync, ui32 begin, TRope data, std::vector<ui64> checksums) {
        if (Stopping || IsBroken()) {
            co_return TSyncSegmentResult{IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH,
                IsBroken() ? GetBrokenReason() : TString(StoppingReason)};
        }
        TIntegrityManager::TOperation metadata;
        if (Config.EnableChecksums) {
            metadata = IntegrityManager->StartWrite({sync.Creds.TabletId, sync.VChunkIndex}, begin, data.size(), checksums);
        }
        TSyncSegmentResult result;
        const auto* immediate = metadata.GetResult();
        if (!immediate || immediate->Status == TIntegrityManager::EOperationStatus::Ok) {
            const auto chunk = ChunkRefs.at(sync.Creds.TabletId).at(sync.VChunkIndex).ChunkIdx;
            std::unique_ptr<TDirectIoOpBase> op = AllocateOp<TInternalSyncWriteOp>();
            op->PrepareWrite(std::move(data), DiskFormat->Offset(chunk, 0, begin), chunk, begin);
            auto event = co_await AwaitSyncIo(std::move(op), sync.Creds.TabletId, sync.VChunkIndex);
            result.Status = event->Get()->Status;
            result.ErrorReason = std::move(event->Get()->ErrorMessage);
        }
        if (Config.EnableChecksums) {
            auto integrity = co_await metadata.Wait(*this);
            CountIntegrityResult(integrity);
            if (integrity.Status != TIntegrityManager::EOperationStatus::Ok) {
                result.Status = integrity.Status == TIntegrityManager::EOperationStatus::Corrupted
                    ? TStatus::CORRUPTED : TStatus::SESSION_MISMATCH;
                result.ErrorReason = std::move(integrity.ErrorReason);
            }
        }
        if (IsBroken()) { result = {TStatus::ERROR, GetBrokenReason()}; }
        co_return result;
    }

    void TDDiskActor::HandleLateSyncSource(ui64 cookie) {
        if (!SyncReadCookiesInFlight.erase(cookie)) {
            YDB_LOG_ERROR_COMP(BS_DDISK, "TDDiskActor::InternalSyncReadResult unknown sync for cookie", {"marker", "BSDD24"}, {"DDiskId", DDiskId}, {"cookie", cookie});
        }
    }
    void TDDiskActor::Handle(TEvReadResult::TPtr ev) { HandleLateSyncSource(ev->Cookie); }
    void TDDiskActor::Handle(TEvReadPersistentBufferResult::TPtr ev) { HandleLateSyncSource(ev->Cookie); }
}
