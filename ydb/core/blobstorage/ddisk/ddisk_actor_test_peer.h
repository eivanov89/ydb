#pragma once
#include "ddisk_actor.h"
#include "direct_io_op.h"

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
#if defined(__linux__)
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
