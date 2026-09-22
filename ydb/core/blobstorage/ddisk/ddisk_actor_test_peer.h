#pragma once
#include "ddisk_actor.h"

namespace NKikimr::NDDisk {
class TDDiskActorTestPeer {
public:
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
