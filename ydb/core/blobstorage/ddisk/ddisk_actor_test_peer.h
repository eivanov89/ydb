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
            << " indexed_event=" << sizeof(TDDiskActor::TEvPrivate::TEvIndexedReadResult)
            << " generic_event=" << sizeof(TDDiskActor::TEvPrivate::TEvDDiskIoResult)
            << " awaiter=" << sizeof(TDDiskActor::TDDiskReadAwaiter) << Endl;
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
    // Logical request waiters, not coroutine allocations or worker TLS frames.
    static size_t RequestWaiters(const TDDiskActor& actor) {
        size_t count = actor.SyncsInFlight.size() + actor.TabletChunkDeletionReplies.size();
        for (const auto& [_, write] : actor.PendingDDiskWrites) {
            count += !write->SyncId;
        }
        for (const auto& [_, read] : actor.PendingDDiskReads) {
            count += !read->Detached;
        }
        for (const auto& slot : actor.IndexedReads) {
            count += slot.Waiter != nullptr;
        }
        return count;
    }

    static size_t PendingReads(const TDDiskActor& actor) {
        return actor.PendingDDiskReads.size();
    }

    static size_t PendingWrites(const TDDiskActor& actor) {
        return actor.PendingDDiskWrites.size();
    }

    static size_t PendingWriteResults(const TDDiskActor& actor) {
        return actor.PendingWriteDataCookies.size();
    }

    static void CancellableWriteObserver(TDDiskActor& actor,
            NActors::TAsyncCancellationScope& scope, bool& finished, bool& completed)
    {
        Y_ABORT_UNLESS(actor.PendingDDiskWrites.size() == 1);
        auto write = actor.PendingDDiskWrites.begin()->second;
        actor.LaunchIntegrity([&, write = std::move(write)]() -> NActors::async<void> {
            co_await scope.Wrap([&]() -> NActors::async<void> {
                TDDiskActor::TDDiskWriteAwaiter awaiter(write);
                struct TWriteRef {
                    TDDiskActor::TDDiskWriteAwaiter& Awaiter;
                    auto& operator co_await() {
                        return Awaiter;
                    }
                };
                co_await TWriteRef{awaiter};
                completed = true;
            });
            finished = true;
        });
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

    static std::vector<NWilson::TTraceId> PendingReadTraceIds(const TDDiskActor& actor) {
        std::vector<NWilson::TTraceId> traces;
        for (const auto& [cookie, context] : actor.PendingDDiskReads) {
            traces.push_back(context->Result.Span.GetTraceId());
        }
        return traces;
    }

    static size_t ActiveIndexedReads(const TDDiskActor& actor) {
        return actor.ActiveIndexedReads;
    }

    static size_t IndexedReadCapacity(const TDDiskActor& actor) {
        return actor.IndexedReads.size();
    }

    static std::vector<ui64> IndexedReadTokens(const TDDiskActor& actor) {
        std::vector<ui64> tokens;
        for (size_t i = 0; i < actor.IndexedReads.size(); ++i) {
            const auto& slot = actor.IndexedReads[i];
            if (slot.Pin) {
                tokens.push_back((ui64(slot.Generation) << 32) | (i + 1));
            }
        }
        return tokens;
    }

    static void ExhaustNextIndexedReadGeneration(TDDiskActor& actor) {
        Y_ABORT_UNLESS(actor.FirstFreeIndexedRead != Max<ui32>());
        actor.IndexedReads[actor.FirstFreeIndexedRead].Generation = Max<ui32>();
    }

    static bool IsIndexedReadCompletion(IEventHandle& ev) {
        return ev.GetTypeRewrite() == TDDiskActor::TEvPrivate::TEvIndexedReadResult::EventType;
    }

    static IEventBase* IndexedReadCompletion(ui64 token) {
        auto* result = new TDDiskActor::TEvPrivate::TEvIndexedReadResult(
            token, NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR,
            "injected stale completion", {}, {});
        return result;
    }

    static bool AbandonReadReservation(TDDiskActor& actor, TQueryCredentials creds) {
        Y_ABORT_UNLESS(!actor.Config.EnableChecksums && !actor.Stopping);
        actor.Stopping = true;
        TEvRead::TPtr request = reinterpret_cast<TEventHandle<TEvRead>*>(
            new IEventHandle(actor.SelfId(), actor.SelfId(),
                new TEvRead(creds, {0, 0, IntegrityUnitSize}, {true})));
        auto& chunk = actor.ChunkRefs.at(creds.TabletId).at(0);
        const auto pins = chunk.InFlightDataIo;
        NWilson::TSpan span;
        auto awaiter = actor.ReadDDisk(request, chunk, creds.TabletId, {0, 0, IntegrityUnitSize}, span);
        actor.Stopping = false;
        return awaiter.await_ready() && !actor.ActiveIndexedReads && chunk.InFlightDataIo == pins
            && awaiter.await_resume().Status == NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH;
    }

    static void CancellableRead(TDDiskActor& actor, TQueryCredentials creds,
            NActors::TAsyncCancellationScope& scope, bool cancelBeforeSuspend,
            bool& finished, bool& completed, NWilson::TTraceId traceId = {})
    {
        actor.LaunchIntegrity([&, creds, cancelBeforeSuspend, traceId = std::move(traceId)]() -> NActors::async<void> {
            co_await scope.Wrap([&]() -> NActors::async<void> {
                TEvRead::TPtr request = reinterpret_cast<TEventHandle<TEvRead>*>(
                    new IEventHandle(actor.SelfId(), actor.SelfId(),
                        new TEvRead(creds, {0, 0, IntegrityUnitSize}, {true})));
                auto& chunk = actor.ChunkRefs.at(creds.TabletId).at(0);
                NWilson::TSpan span(TWilson::DDiskTopLevel, NWilson::TTraceId(traceId),
                    "DDisk.Read.TestCancelled", NWilson::EFlags::NONE);
                auto awaiter = actor.ReadDDisk(request, chunk, creds.TabletId,
                    {0, 0, IntegrityUnitSize}, span);
                if (cancelBeforeSuspend) {
                    scope.Cancel();
                }
                struct TReadRef {
                    TDDiskActor::TDDiskReadAwaiter& Awaiter;
                    auto& operator co_await() {
                        return Awaiter;
                    }
                };
                auto result = co_await TReadRef{awaiter};
                completed = result.Status == NKikimrBlobStorage::NDDisk::TReplyStatus::OK;
            });
            finished = true;
        });
    }
#if defined(__linux__)
    static void SubmitScalarReadParts(TDDiskActor& actor, size_t count) {
        auto read = std::make_unique<TDDiskActor::TReadPartsIoOp>(actor);
        std::vector<TDDiskActor::TReadPartsIoOp::TPart> parts;
        for (size_t i = 0; i < count; ++i) {
            parts.push_back({i + 1, 100, ui32(i * IntegrityUnitSize), IntegrityUnitSize,
                actor.DiskFormat->Offset(100, 0, i * IntegrityUnitSize)});
        }
        read->PrepareParts(parts);
        std::unique_ptr<TDDiskActor::TDirectIoOpBase> op = std::move(read);
        actor.DirectUringOp(op);
    }

    static bool IsIndexedRead(const NPDisk::TUringOperationBase& op) {
        const auto& direct = static_cast<const TDDiskActor::TDDiskIoOp&>(op);
        return direct.GetIndexedReadToken() && !direct.GetCompletionCookie();
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
            && actor.FormattingChunks.empty() && !actor.ChunkManager.HasAllocations()
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
                || !actor.ReadPartCallbacks.empty() || !actor.PendingDDiskReads.empty()
                || actor.ActiveIndexedReads
                || !actor.DelayedRetries.empty() || !actor.IntegrityWriteCookies.empty()
                || !actor.PendingDataAllocationTokens.empty()
                || !actor.PendingDDiskWrites.empty() || !actor.PendingWriteDataCookies.empty()
                || !actor.PendingWriteWork.empty() || !actor.SyncsInFlight.empty()
                || !actor.SyncSourceCookies.empty() || !actor.SyncWork.empty()
                || (actor.IntegrityManager
                    && actor.IntegrityManager->HasInFlightOperationsForTablet(tabletId))) {
            return false;
        }
        const auto tablet = actor.ChunkRefs.find(tabletId);
        if (tablet != actor.ChunkRefs.end()) {
            for (const auto& [_, chunk] : tablet->second) {
                if (chunk.AllocationPending || chunk.AllocationWaiters || chunk.InFlightDataIo) {
                    return false;
                }
            }
        }
        return true;
    }
    struct TBenchmarkSnapshot {
        bool Healthy = false;
        bool UsesRouter = false;
        bool RouterDevNullMode = false;
        ui64 CompletedIo = 0;
        ui64 PairReads = 0;
        ui64 PairWrites = 0;
        size_t CachedBlockStates = 0;
        size_t CachedFrameBytes = 0;
    };

    // Identical read-only adapter in the historical and refactored checkouts.
    // Dependent feature checks account for records owned by only one revision.
    template<class TActor = TDDiskActor>
    static TBenchmarkSnapshot BenchmarkSnapshot(const TActor& actor, ui64 tabletId) {
        TBenchmarkSnapshot result;
        result.Healthy = !actor.Stopping && !actor.IsBroken()
            && ReservationsSettled(actor) && IoCountersBalanced(actor)
            && actor.PendingChunkRelease.empty() && actor.ChunkMapIncrementsInFlight.empty()
            && actor.LogWaiters.empty() && actor.WriteCallbacks.empty()
            && actor.ReadCallbacks.empty() && actor.ReadPartCallbacks.empty()
            && actor.ReadPartsRemaining.empty() && actor.PendingDDiskReads.empty()
            && !actor.ActiveIndexedReads && actor.DelayedRetries.empty()
            && actor.SyncsInFlight.empty() && actor.IntegrityAllocations.empty()
            && (!actor.IntegrityManager
                || !actor.IntegrityManager->HasInFlightOperationsForTablet(tabletId));
        if constexpr (requires { actor.Reservation; }) {
            result.Healthy &= !actor.Reservation;
        }
        if constexpr (requires { actor.FormatSlices.empty(); }) {
            result.Healthy &= actor.FormatSlices.empty();
        }
        if constexpr (requires { actor.IntegrityWriteCookies.empty(); }) {
            result.Healthy &= actor.IntegrityWriteCookies.empty();
        }
        if constexpr (requires { actor.PendingDataAllocationTokens.empty(); }) {
            result.Healthy &= actor.PendingDataAllocationTokens.empty();
        }
        if constexpr (requires { actor.PendingDDiskWrites.empty(); }) {
            result.Healthy &= actor.PendingDDiskWrites.empty();
        }
        if constexpr (requires { actor.PendingWriteDataCookies.empty(); }) {
            result.Healthy &= actor.PendingWriteDataCookies.empty();
        }
        if constexpr (requires { actor.PendingWriteWork.empty(); }) {
            result.Healthy &= actor.PendingWriteWork.empty();
        }
        if constexpr (requires { actor.SyncSourceCookies.empty(); }) {
            result.Healthy &= actor.SyncSourceCookies.empty();
        }
        if constexpr (requires { actor.SyncWork.empty(); }) {
            result.Healthy &= actor.SyncWork.empty();
        }
        if constexpr (requires { actor.SyncReadCookiesInFlight.empty(); }) {
            result.Healthy &= actor.SyncReadCookiesInFlight.empty();
        }
        const auto tablet = actor.ChunkRefs.find(tabletId);
        if (tablet != actor.ChunkRefs.end()) {
            for (const auto& [_, chunk] : tablet->second) {
                result.Healthy &= !chunk.AllocationPending && !chunk.AllocationWaiters
                    && !chunk.InFlightDataIo;
                if constexpr (requires { chunk.IntegrityExtentWriteInFlight; }) {
                    result.Healthy &= !chunk.IntegrityExtentWriteInFlight && chunk.ExtentWaiters.empty();
                }
            }
        }
        if constexpr (requires { actor.UringRouter; }) {
            result.UsesRouter = bool(actor.UringRouter);
            result.RouterDevNullMode = actor.UringRouter
                ? actor.UringRouter->GetConfig().DevNullMode : false;
        }
        result.CompletedIo = actor.Counters.DirectIO.Read.Requests->Val()
            + actor.Counters.DirectIO.Write.Requests->Val();
        result.PairReads = actor.Counters.Checksums.IntegrityPairReads->Val();
        result.PairWrites = actor.Counters.Checksums.IntegrityPairWrites->Val();
        result.CachedBlockStates = actor.IntegrityManager
            ? actor.IntegrityManager->CachedBlockStates() : 0;
        if (const auto* cache = NActors::TAsyncFrameCache::GetCurrent()) {
            result.CachedFrameBytes = cache->GetStats().CachedBytes;
        }
        return result;
    }

    static void SetDestructionClock(TDDiskActor& actor,
            std::function<TMonotonic()> now, std::function<void()> sleep) {
        actor.DestructionNow = std::move(now);
        actor.DestructionSleep = std::move(sleep);
    }
};
}
