#include "ddisk_actor.h"

#include <ydb/core/util/stlog.h>

namespace NKikimr::NDDisk {
    using TStatus = NKikimrBlobStorage::NDDisk::TReplyStatus;

    void TDDiskActor::Handle(TEvSync::TPtr ev) {
        TQueryCredentials creds;
        if (!CheckQueryImpl<false>(*ev, &Counters.Interface.Sync, creds)) {
            co_return;
        }
        Counters.Interface.Sync.Request(0);
        const auto& record = ev->Get()->Record;
        const TQueryCredentials original(record.GetCredentials());

        auto reject = [&](TStatus::E status, TString reason) {
            Counters.Interface.Sync.Reply(false);
            SendReply(*ev, std::make_unique<TEvSyncResult>(status, std::move(reason)));
        };
        if (TabletChunkDeletionsInFlight.contains(creds.TabletId)) {
            reject(TStatus::BUSY, "tablet chunk deletion is in flight");
            co_return;
        }
        if (!record.SourcesSize()) {
            reject(TStatus::INCORRECT_REQUEST, "sources must be non-empty");
            co_return;
        }

        // Build and validate the entire request before sending even the first source read.
        auto sync = std::make_shared<TSyncInFlight>();
        sync->Id = NextSyncId++;
        sync->OriginalCredentials = original;
        sync->Creds = creds;
        sync->ReplyTo = ev->Sender;
        sync->InterconnectSession = ev->InterconnectSession;
        sync->ReplyCookie = ev->Cookie;
        std::optional<ui64> vchunk;
        for (const auto& source : record.GetSources()) {
            if (!source.HasDDiskId()) {
                reject(TStatus::INCORRECT_REQUEST, "source ddisk id must be set");
                co_return;
            }
            const auto& id = source.GetDDiskId();
            const auto sourceCreds = TQueryCredentials::ForInternal(creds.TabletId, creds.Generation,
                std::make_optional(source.GetDDiskInstanceGuid()), creds.DirectBlockGroupIndex);
            for (const auto& segment : source.GetSegments()) {
                const TBlockSelector selector(segment.GetSelector());
                const bool pb = segment.HasPersistentBufferSegment();
                if (pb == segment.HasDDiskSegment()
                        || !selector.Size || selector.OffsetInBytes % IntegrityUnitSize
                        || selector.Size % IntegrityUnitSize
                        || selector.OffsetInBytes > DiskFormat->ChunkSize
                        || selector.Size > DiskFormat->ChunkSize - selector.OffsetInBytes
                        || (vchunk && *vchunk != selector.VChunkIndex)) {
                    reject(TStatus::INCORRECT_REQUEST,
                        "segments must have exactly one kind and aligned nonempty ranges within one VChunk");
                    co_return;
                }
                vchunk = selector.VChunkIndex;
                auto& input = sync->Requests.emplace_back();
                input.Selector = selector;
                input.PersistentBufferSource = pb;
                if (pb) {
                    input.Source = MakeBlobStoragePersistentBufferId(
                        id.GetNodeId(), id.GetPDiskId(), id.GetDDiskSlotId());
                    input.Query = std::make_unique<TEvReadPersistentBuffer>(sourceCreds, selector,
                        segment.GetPersistentBufferSegment().GetLsn(),
                        segment.GetPersistentBufferSegment().GetGeneration(), TReadInstruction(true));
                } else {
                    input.Source = MakeBlobStorageDDiskId(
                        id.GetNodeId(), id.GetPDiskId(), id.GetDDiskSlotId());
                    input.Query = std::make_unique<TEvRead>(sourceCreds, selector, TReadInstruction(true));
                }
            }
        }
        if (sync->Requests.empty()) {
            reject(TStatus::INCORRECT_REQUEST, "segments must be non-empty");
            co_return;
        }
        sync->VChunkIndex = *vchunk;
        sync->Span = NWilson::TSpan(TWilson::DDiskTopLevel, std::move(ev->TraceId), "DDisk.Sync",
            NWilson::EFlags::NONE, TActivationContext::ActorSystem());
        NPrivate::AddMessageWaitAttributes(sync->Span);
        sync->Span.Attribute("tablet_id", static_cast<i64>(creds.TabletId))
            .Attribute("sync_id", static_cast<i64>(sync->Id));
        const ui64 syncId = sync->Id;
        Y_ABORT_UNLESS(SyncsInFlight.emplace(syncId, sync).second);

        // Register every cookie before any send. Independent sources can reply in any order.
        for (ui32 i = 0; i < sync->Requests.size(); ++i) {
            auto& input = sync->Requests[i];
            input.SourceCookie = NActors::AllocateWaitCookie();
            Y_ABORT_UNLESS(SyncSourceCookies.emplace(input.SourceCookie,
                TSyncSourceCookie{syncId, i, input.PersistentBufferSource}).second);
        }
        ev.Reset(nullptr);
        for (auto& input : sync->Requests) {
            Send(input.Source, input.Query.release(), IEventHandle::FlagTrackDelivery,
                input.SourceCookie, sync->Span.GetTraceId());
        }

        // Existing destination metadata loads do not depend on the source payload. The
        // parent's chunk-ref pin starts before preparation and transfers to the destination
        // write record without a gap, so tablet deletion cannot remove this chunk entry.
        if (Config.EnableChecksums) {
            const auto tablet = ChunkRefs.find(creds.TabletId);
            const auto chunkIt = tablet == ChunkRefs.end() ? nullptr : [&]() -> TChunkRef* {
                const auto it = tablet->second.find(sync->VChunkIndex);
                return it == tablet->second.end() ? nullptr : &it->second;
            }();
            if (chunkIt && chunkIt->ChunkIdx && IntegrityManager->FindExtentRef({creds.TabletId, sync->VChunkIndex})) {
                for (ui32 i = 0; i < sync->Requests.size(); ++i) {
                    auto& input = sync->Requests[i];
                    ++chunkIt->ChunkRefPins;
                    input.ChunkRefPinned = true;
                    input.MetadataStarted = true;
                    input.Metadata = IntegrityManager->PrepareWrite({creds.TabletId, sync->VChunkIndex},
                        input.Selector.OffsetInBytes, input.Selector.Size);
                    input.Metadata.Durable.SetCompletionCallback([this, syncId] {
                        QueueSync(syncId);
                    });
                }
            }
        }
        QueueSync(syncId);
        co_await TSyncAwaiter(sync);
    }

    void TDDiskActor::QueueSync(ui64 id) {
        SyncWork.push_back(id);
        DrainSyncWork();
    }

    void TDDiskActor::QueueSyncsForChunk(ui64 tabletId, ui64 vChunkIndex) {
        for (const auto& [id, sync] : SyncsInFlight) {
            if (sync->Creds.TabletId == tabletId && sync->VChunkIndex == vChunkIndex) {
                SyncWork.push_back(id);
            }
        }
        DrainSyncWork();
    }

    void TDDiskActor::DrainSyncWork() {
        if (DrainingSyncWork) {
            return;
        }
        DrainingSyncWork = true;
        while (!SyncWork.empty()) {
            const ui64 id = SyncWork.front();
            SyncWork.pop_front();
            if (SyncsInFlight.contains(id)) {
                AdvanceSync(id);
            }
        }
        DrainingSyncWork = false;
    }

    void TDDiskActor::AdvanceSync(ui64 id) {
        const auto it = SyncsInFlight.find(id);
        if (it == SyncsInFlight.end()) {
            return;
        }
        const auto sync = it->second;
        bool done = true;
        for (auto& input : sync->Requests) {
            if (!input.Terminal) {
                done = false;
                continue;
            }
            if (input.MetadataStarted && !input.DestinationStarted) {
                if (!input.Metadata.Durable.IsSettled()) {
                    done = false;
                    continue;
                }
                input.MetadataStarted = false;
            }
            if (input.ChunkRefPinned) {
                auto& chunk = ChunkRefs.at(sync->Creds.TabletId).at(sync->VChunkIndex);
                Y_ABORT_UNLESS(chunk.ChunkRefPins);
                --chunk.ChunkRefPins;
                input.ChunkRefPinned = false;
            }
        }
        if (!done) {
            return;
        }
        if (!IsChunkCommitted(sync->Creds.TabletId, sync->VChunkIndex) && !Stopping && !IsBroken()) {
            return;
        }
        TStringBuilder errors;
        for (auto& input : sync->Requests) {
            if (IsBroken() || !IsChunkCommitted(sync->Creds.TabletId, sync->VChunkIndex)) {
                input.Status = IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH;
                input.ErrorReason = IsBroken() ? GetBrokenReason() : TString(StoppingReason);
            }
            if (input.Status != TStatus::OK) {
                errors << input.ErrorReason << "; ";
            }
        }
        const auto status = errors
            ? (Stopping && !IsBroken() ? TStatus::SESSION_MISMATCH : TStatus::ERROR)
            : TStatus::OK;
        auto reply = std::make_unique<TEvSyncResult>(status, errors);
        for (const auto& input : sync->Requests) {
            reply->AddSegmentResult(input.Status, input.ErrorReason);
        }
        Counters.Interface.Sync.Reply(!errors);
        auto h = std::make_unique<IEventHandle>(sync->ReplyTo, SelfId(), reply.release(),
            0, sync->ReplyCookie, nullptr, sync->Span.GetTraceId());
        if (sync->InterconnectSession) {
            h->Rewrite(TEvInterconnect::EvForward, sync->InterconnectSession);
        }
        sync->Span.End();
        SyncsInFlight.erase(it);
        sync->Done = true;
        TActivationContext::Send(h.release());
        sync->Changed.NotifyAll();
        if (Stopping && !GetDirectIoInflight()) {
            FinishStopping();
        }
    }

    void TDDiskActor::CompleteSyncDestination(ui64 syncId, ui32 input,
            TStatus::E status, TString reason)
    {
        const auto it = SyncsInFlight.find(syncId);
        if (it == SyncsInFlight.end() || input >= it->second->Requests.size()) {
            return;
        }
        auto& source = it->second->Requests[input];
        if (source.Terminal || !source.DestinationStarted) {
            return;
        }
        source.Status = status;
        source.ErrorReason = std::move(reason);
        source.Terminal = true;
        QueueSync(syncId);
    }

    void TDDiskActor::CompleteSyncSource(ui64 cookie, bool persistentBuffer,
            TStatus::E status, TString reason, bool hasPayload, TRope data,
            std::vector<ui64> checksums)
    {
        const auto cookieIt = SyncSourceCookies.find(cookie);
        if (cookieIt == SyncSourceCookies.end() || cookieIt->second.PersistentBuffer != persistentBuffer) {
            return;
        }
        const auto source = cookieIt->second;
        SyncSourceCookies.erase(cookieIt);
        const auto parentIt = SyncsInFlight.find(source.SyncId);
        if (parentIt == SyncsInFlight.end() || source.Input >= parentIt->second->Requests.size()) {
            return;
        }
        const auto sync = parentIt->second;
        auto& input = sync->Requests[source.Input];
        if (input.Terminal || input.SourceCookie != cookie) {
            return;
        }
        input.SourceCookie = 0;
        auto fail = [&](TStatus::E failure, TString message) {
            input.Status = failure;
            input.ErrorReason = std::move(message);
            input.Terminal = true;
            if (input.MetadataStarted) {
                IntegrityManager->CancelWrite(input.Metadata.Id);
            }
            QueueSync(sync->Id);
        };
        if (status != TStatus::OK) {
            fail(status, std::move(reason));
            return;
        }
        if (!hasPayload) {
            fail(TStatus::INCORRECT_REQUEST, "source payload is missing");
            return;
        }
        if (data.size() != input.Selector.Size) {
            fail(TStatus::INCORRECT_REQUEST, "source payload size does not match requested size");
            return;
        }
        if (Config.EnableChecksums) {
            if (!HasRequiredBlockChecksums(checksums.size(), input.Selector.OffsetInBytes, input.Selector.Size)) {
                fail(TStatus::INCORRECT_REQUEST, "source read must return one checksum per aligned 4 KiB block");
                return;
            }
            if (Config.CheckChecksumBeforeWrite) {
                if (const auto validation = ValidatePayloadChecksums(checksums, data)) {
                    if (validation->Status == TStatus::CORRUPTED) {
                        Counters.Checksums.ChecksumMismatch->Inc();
                    }
                    fail(validation->Status, validation->ErrorReason);
                    return;
                }
            }
        } else {
            checksums.clear();
        }
        if (Stopping || IsBroken()) {
            fail(IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH,
                IsBroken() ? GetBrokenReason() : TString(StoppingReason));
            return;
        }
        TQueryCredentials current;
        if (ResolveConnection(sync->OriginalCredentials, &current) != EConnectionResolution::Resolved
                || current.TabletId != sync->Creds.TabletId
                || current.Generation != sync->Creds.Generation
                || current.DDiskSessionSeqNo != sync->Creds.DDiskSessionSeqNo
                || current.DirectBlockGroupIndex != sync->Creds.DirectBlockGroupIndex
                || current.DDiskInstanceGuid != sync->Creds.DDiskInstanceGuid) {
            fail(TStatus::SESSION_MISMATCH, "session replaced while sync source was waiting");
            return;
        }
        StartSyncDestination(sync->Id, source.Input, std::move(data), std::move(checksums));
    }

    void TDDiskActor::HandleSyncSourceUndelivered(ui64 cookie, bool persistentBuffer) {
        CompleteSyncSource(cookie, persistentBuffer, TStatus::ERROR,
            "source read event undelivered", false, {}, {});
    }

    void TDDiskActor::CancelPendingSyncSources() {
        const auto status = IsBroken() ? TStatus::ERROR : TStatus::SESSION_MISMATCH;
        const TString reason = IsBroken() ? GetBrokenReason() : TString(StoppingReason);
        std::vector<ui64> ids;
        std::vector<ui64> preparations;
        ids.reserve(SyncsInFlight.size());
        for (const auto& [id, sync] : SyncsInFlight) {
            ids.push_back(id);
            for (auto& input : sync->Requests) {
                if (input.Terminal || input.DestinationStarted) {
                    continue;
                }
                if (input.SourceCookie) {
                    SyncSourceCookies.erase(input.SourceCookie);
                    input.SourceCookie = 0;
                }
                input.Status = status;
                input.ErrorReason = reason;
                input.Terminal = true;
                if (input.MetadataStarted) {
                    preparations.push_back(input.Metadata.Id);
                }
            }
        }
        for (ui64 preparation : preparations) {
            IntegrityManager->CancelWrite(preparation);
        }
        for (ui64 id : ids) {
            QueueSync(id);
        }
    }

    void TDDiskActor::Handle(TEvReadResult::TPtr ev) {
        const auto cookie = SyncSourceCookies.find(ev->Cookie);
        if (cookie == SyncSourceCookies.end() || cookie->second.PersistentBuffer) {
            return;
        }
        auto& msg = *ev->Get();
        const bool hasPayload = msg.GetPayloadCount() > 0;
        TRope data = hasPayload ? msg.GetPayload(0) : TRope{};
        CompleteSyncSource(ev->Cookie, false, msg.Record.GetStatus(), msg.Record.GetErrorReason(),
            hasPayload, std::move(data),
            {msg.Record.GetChecksums().begin(), msg.Record.GetChecksums().end()});
    }

    void TDDiskActor::Handle(TEvReadPersistentBufferResult::TPtr ev) {
        const auto cookie = SyncSourceCookies.find(ev->Cookie);
        if (cookie == SyncSourceCookies.end() || !cookie->second.PersistentBuffer) {
            return;
        }
        auto& msg = *ev->Get();
        const bool hasPayload = msg.GetPayloadCount() > 0;
        TRope data = hasPayload ? msg.GetPayload(0) : TRope{};
        CompleteSyncSource(ev->Cookie, true, msg.Record.GetStatus(), msg.Record.GetErrorReason(),
            hasPayload, std::move(data),
            {msg.Record.GetChecksums().begin(), msg.Record.GetChecksums().end()});
    }
}
