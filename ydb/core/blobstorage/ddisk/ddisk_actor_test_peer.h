#pragma once
#include "ddisk_actor.h"
#include "direct_io_op.h"
#include <ydb/library/actors/async/cancellation.h>
#include <ydb/library/actors/async/frame_cache.h>

namespace NKikimr::NDDisk {
class TDDiskActorTestPeer {
public:
    static bool ReadCredentialsUnchanged(TDDiskActor& actor, TQueryCredentials credentials) {
        TEvRead::TPtr request = reinterpret_cast<TEventHandle<TEvRead>*>(
            new IEventHandle(actor.SelfId(), actor.SelfId(),
                new TEvRead(credentials, {0, 0, IntegrityUnitSize}, {true})));
        const auto original = request->Get()->Record.SerializeAsString();
        TQueryCredentials resolved;
        return actor.CheckQuery(*request, nullptr, resolved)
            && resolved.TabletId == credentials.TabletId
            && request->Get()->Record.SerializeAsString() == original;
    }

    static void PrintReadFootprint() {
        Cerr << "DDisk read sizes: payload=" << sizeof(TReadPayload)
            << " checksums=" << sizeof(TReadChecksums)
            << " result=" << sizeof(TDDiskActor::TDDiskReadResult)
            << " completion=" << sizeof(TDDiskActor::TIoCompletion)
            << " batch=" << sizeof(TDDiskActor::TBatchedIOAwaiter) << Endl;
    }

    static ui64 ChecksumMismatches(const TDDiskActor& actor) {
        return actor.Counters.Checksums.ChecksumMismatch->Val();
    }

    static size_t ReservedChunks(const TDDiskActor& actor) {
        return actor.ChunkManager.GetReservedChunkCount();
    }

    static bool IoCountersBalanced(const TDDiskActor& actor) {
        const auto& io = actor.Counters.DirectIO;
        return !actor.GetDirectIoInflight() && !io.RunningCount->Val()
            && !io.Read.RequestsInFlight->Val() && !io.Read.BytesInFlight->Val()
            && !io.Write.RequestsInFlight->Val() && !io.Write.BytesInFlight->Val();
    }

    // Logical request waiters, not coroutine allocations or worker TLS frames. One per
    // live data-path coroutine frame, plus the aggregates which keep their own records.
    static size_t RequestWaiters(const TDDiskActor& actor) {
        return actor.DataRequestsInFlight + actor.SyncsInFlight.size()
            + actor.TabletChunkDeletionReplies.size();
    }

    // Data-path coroutine frames owning accepted device I/O or an unsent client reply.
    static size_t DataRequests(const TDDiskActor& actor) {
        return actor.DataRequestsInFlight;
    }

    // Logical reads TIntegrityManager still owes a checksum result to, including those
    // joined to pair loads started by another reader.
    static size_t PendingReads(const TDDiskActor& actor) {
        return actor.IntegrityManager ? actor.IntegrityManager->PendingReadCount() : 0;
    }

    static size_t PendingSyncs(const TDDiskActor& actor) {
        return actor.SyncsInFlight.size();
    }

    static size_t PendingSyncSources(const TDDiskActor& actor) {
        return actor.SyncSourceCookies.size();
    }

    static void CancellableSyncObserver(TDDiskActor& actor,
            NActors::TAsyncCancellationScope& scope, bool& finished, bool& completed)
    {
        Y_ABORT_UNLESS(actor.SyncsInFlight.size() == 1);
        auto sync = actor.SyncsInFlight.begin()->second;
        actor.LaunchIntegrity([&, sync = std::move(sync)]() -> NActors::async<void> {
            co_await scope.Wrap([&]() -> NActors::async<void> {
                TDDiskActor::TSyncAwaiter awaiter(sync);
                struct TSyncRef {
                    TDDiskActor::TSyncAwaiter& Awaiter;
                    auto& operator co_await() {
                        return Awaiter;
                    }
                };
                co_await TSyncRef{awaiter};
                completed = true;
            });
            finished = true;
        });
    }

#if defined(__linux__)
    // Observable outcome of one TBatchedIOAwaiter, as the data path uses it.
    struct TBatchProbe {
        // Set once the frame has resumed from the batch wait.
        bool Resumed = false;
        // Set when the frame left its cancellation scope and retired.
        bool Finished = false;
        // Observes callback ownership without prolonging its lifetime.
        std::weak_ptr<void> Callback;
        std::vector<NKikimrBlobStorage::NDDisk::TReplyStatus::E> Statuses;
        std::vector<TReadPayload> Data;
    };

    // Mirrors the metadata batch of a cold read: one critical operation per pair, a single
    // resume for the whole group, and results readable only after every operation is back.
    // Optionally wraps the wait in a cancellation scope so callers can assert that an
    // accepted batch keeps its buffers until the device is done with them.
    static void SubmitPairReadBatch(TDDiskActor& actor, size_t count, TBatchProbe& probe,
            NActors::TAsyncCancellationScope& scope, bool cancelBeforeWait = false)
    {
        probe.Statuses.assign(count, NKikimrBlobStorage::NDDisk::TReplyStatus::UNKNOWN);
        probe.Data.resize(count);
        actor.LaunchIntegrity([&actor, count, &probe, &scope, cancelBeforeWait]()
                -> NActors::async<void> {
            co_await scope.Wrap([&]() -> NActors::async<void> {
                auto batch = std::make_shared<TDDiskActor::TBatchedIOAwaiter>(actor);
                probe.Callback = batch;
                batch->PairResults.resize(count);
                for (size_t i = 0; i < count; ++i) {
                    const TIntegrityManager::TPairRead read{i + 1, 100,
                        ui32(i * IntegrityUnitSize), IntegrityUnitSize};
                    actor.SubmitPairRead(batch, i, read);
                }
                if (cancelBeforeWait) {
                    scope.Cancel();
                }
                co_await batch->Wait();
                for (size_t i = 0; i < count; ++i) {
                    probe.Statuses[i] = batch->PairResults[i].Status;
                    probe.Data[i] = std::move(batch->PairResults[i].Data);
                }
                probe.Resumed = true;
            });
            probe.Finished = true;
        });
    }

    // Metadata pair loads are submitted as critical I/O so they share the integrity retry
    // and fail-stop policy; client data I/O is not.
    static bool IsCriticalIo(const NPDisk::TUringOperationBase& op) {
        return static_cast<const TDDiskActor::TDirectIoOpBase&>(op).IsCriticalDDiskIo();
    }

    static std::pair<ui32, ui32> IoAddress(const NPDisk::TUringOperationBase& op) {
        const auto& direct = static_cast<const TDDiskActor::TDirectIoOpBase&>(op);
        return {direct.GetChunkIdx(), direct.GetChunkOffset()};
    }

    static bool UsesRouter(const TDDiskActor& actor) {
        return bool(actor.UringRouter);
    }
#endif
    static void EnterBroken(TDDiskActor& actor, TString reason) {
        NActors::TActorRunnableQueue queue(&actor);
        actor.EnterBroken(std::move(reason));
    }

    static bool IsShutdownDrained(const TDDiskActor& actor) {
        return actor.OwnDrainComplete && actor.PersistentBufferGone;
    }
    static bool IsBroken(const TDDiskActor& actor) { return actor.IsBroken(); }
    static bool IsAllocationPending(const TDDiskActor& actor, ui64 tabletId, ui64 vChunkIndex) {
        return actor.ChunkRefs.at(tabletId).at(vChunkIndex).AllocationPending;
    }
    // Called in the actor's mailbox, after setup writes have completed.
    static bool ReservationsSettled(const TDDiskActor& actor) {
        return actor.LogReplayComplete && !actor.ChunkManager.IsReservationInFlight()
            && actor.FormattingChunks.empty() && !actor.ChunkManager.HasPendingAllocations()
            && actor.DataChunkAllocationsInFlight.empty()
            && !actor.IssuePersistentBufferChunkAllocationInflight
            && actor.PersistentBufferChunks.size() >= actor.PersistentBufferFormat.InitChunks;
    }
    static bool RequestFollowupsQuiescent(const TDDiskActor& actor, ui64 tabletId) {
        if (actor.Stopping || actor.IsBroken()
                || !ReservationsSettled(actor) || !IoCountersBalanced(actor)
                || actor.Reservation || !actor.FormatSlices.empty()
                || !actor.PendingChunkRelease.empty()
                || !actor.ChunkMapIncrementsInFlight.empty() || !actor.LogWaiters.empty()
                || !actor.WriteCallbacks.empty() || !actor.ReadCallbacks.empty()
                || actor.DataRequestsInFlight
                || !actor.DelayedRetries.empty() || !actor.IntegrityWriteCookies.empty()
                || !actor.PendingDataAllocationTokens.empty()
                || !actor.SyncsInFlight.empty()
                || !actor.SyncSourceCookies.empty() || !actor.SyncWork.empty()
                || (actor.IntegrityManager
                    && actor.IntegrityManager->HasInFlightOperationsForTablet(tabletId))) {
            return false;
        }
        const auto tablet = actor.ChunkRefs.find(tabletId);
        if (tablet != actor.ChunkRefs.end()) {
            for (const auto& [_, chunk] : tablet->second) {
                if (chunk.AllocationPending || chunk.ChunkRefPins || chunk.InFlightDataIo) {
                    return false;
                }
            }
        }
        return true;
    }

    static void SetDestructionClock(TDDiskActor& actor,
            std::function<TMonotonic()> now, std::function<void()> sleep) {
        actor.DestructionNow = std::move(now);
        actor.DestructionSleep = std::move(sleep);
    }
};
}
