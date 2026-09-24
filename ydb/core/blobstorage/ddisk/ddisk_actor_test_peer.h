#pragma once
#include "ddisk_actor.h"
#include "direct_io_op.h"
#include <ydb/library/actors/async/cancellation.h>

namespace NKikimr::NDDisk {
class TDDiskActorTestPeer {
public:
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
    static NActors::TAsyncFrameCache::TStats FrameCacheStats(const TDDiskActor& actor) {
        return actor.AsyncFrameCache.GetStats();
    }
    static size_t PendingReads(const TDDiskActor& actor) {
        return actor.PendingDDiskReads.size();
    }
    static size_t ActiveIndexedReads(const TDDiskActor& actor) { return actor.ActiveIndexedReads; }
    static size_t IndexedReadCapacity(const TDDiskActor& actor) { return actor.IndexedReads.size(); }
    static std::vector<ui64> IndexedReadTokens(const TDDiskActor& actor) {
        std::vector<ui64> tokens;
        for (size_t i = 0; i < actor.IndexedReads.size(); ++i) {
            const auto& slot = actor.IndexedReads[i];
            if (slot.Pin) { tokens.push_back((ui64(slot.Generation) << 32) | (i + 1)); }
        }
        return tokens;
    }
    static void ExhaustNextIndexedReadGeneration(TDDiskActor& actor) {
        Y_ABORT_UNLESS(actor.FirstFreeIndexedRead != Max<ui32>());
        actor.IndexedReads[actor.FirstFreeIndexedRead].Generation = Max<ui32>();
    }
    static bool IsIndexedReadCompletion(IEventHandle& ev) {
        return ev.GetTypeRewrite() == TDDiskActor::TEvPrivate::TEvDDiskIoResult::EventType
            && ev.Get<TDDiskActor::TEvPrivate::TEvDDiskIoResult>()->IndexedReadToken;
    }
    static IEventBase* IndexedReadCompletion(ui64 token) {
        auto* result = new TDDiskActor::TEvPrivate::TEvDDiskIoResult(
            NPDisk::TUringOperationBase::EREAD, NKikimrBlobStorage::NDDisk::TReplyStatus::ERROR,
            "injected stale completion", {}, {}, {}, 0, {}, 0, 0);
        result->IndexedReadToken = token;
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
            && awaiter.await_resume()->Status == NKikimrBlobStorage::NDDisk::TReplyStatus::SESSION_MISMATCH;
    }
    static void CancellableRead(TDDiskActor& actor, TQueryCredentials creds,
            NActors::TAsyncCancellationScope& scope, bool cancelBeforeSuspend,
            bool& finished, bool& completed) {
        actor.LaunchIntegrity([&, creds, cancelBeforeSuspend]() -> NActors::async<void> {
            co_await scope.Wrap([&]() -> NActors::async<void> {
                TEvRead::TPtr request = reinterpret_cast<TEventHandle<TEvRead>*>(
                    new IEventHandle(actor.SelfId(), actor.SelfId(),
                        new TEvRead(creds, {0, 0, IntegrityUnitSize}, {true})));
                auto& chunk = actor.ChunkRefs.at(creds.TabletId).at(0);
                NWilson::TSpan span;
                auto awaiter = actor.ReadDDisk(request, chunk, creds.TabletId,
                    {0, 0, IntegrityUnitSize}, span);
                if (cancelBeforeSuspend) { scope.Cancel(); }
                struct TReadRef {
                    TDDiskActor::TDDiskReadAwaiter& Awaiter;
                    auto& operator co_await() { return Awaiter; }
                };
                auto result = co_await TReadRef{awaiter};
                completed = bool(result);
            });
            finished = true;
        });
    }
#if defined(__linux__)
    static bool IsIndexedRead(const NPDisk::TUringOperationBase& op) {
        const auto& direct = static_cast<const TDDiskActor::TDDiskIoOp&>(op);
        return direct.GetIndexedReadToken() && !direct.GetCompletionCookie();
    }
    static std::pair<ui32, ui32> IoAddress(const NPDisk::TUringOperationBase& op) {
        const auto& direct = static_cast<const TDDiskActor::TDirectIoOpBase&>(op);
        return {direct.GetChunkIdx(), direct.GetChunkOffset()};
    }
    static std::pair<ui32, ui32> IoPartAddress(const NPDisk::TUringOperationBase& op, size_t index) {
        const auto& direct = static_cast<const TDDiskActor::TReadPartsIoOp&>(op);
        const auto& part = direct.GetFallbackPart(index);
        return {part.ChunkIdx, part.OffsetInBytes};
    }
    static bool UsesRouter(const TDDiskActor& actor) { return bool(actor.UringRouter); }
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
    static void SetDestructionClock(TDDiskActor& actor,
            std::function<TMonotonic()> now, std::function<void()> sleep) {
        actor.DestructionNow = std::move(now);
        actor.DestructionSleep = std::move(sleep);
    }
};
}
