#pragma once

#include "defs.h"

#include "ddisk_checksums.h"

#include <ydb/library/actors/util/rc_buf.h>
#include <ydb/library/actors/async/event.h>
#include <functional>
#include <optional>

#include <library/cpp/containers/absl/flat_hash_map.h>

#include <util/generic/bitmap.h>
#include <util/generic/intrlist.h>
#include <util/generic/array_ref.h>

#include <deque>
#include <memory>
#include <variant>
#include <vector>

namespace NKikimr::NDDisk {

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TIntegrityManager
//
// Owns integrity allocation, shared pair loads and serialized pair flush coroutines.
// Pair preparation and completion are ordinary actor-local operations; the host submits
// their descriptors without launching read coroutines. Handles retain completed results.
//
// PDisk never restarts separately from DDisk, so a reserved chunk may be formatted immediately
// (as if committed). Formatting writes (chunk headers, extent image) run in parallel; the actor
// logs a single combined increment only after the extent is Ready, and does not reply to the
// originating write until that record is durable. A crash before the increment just loses the
// reserved chunks.
//
// Persistence scope: the DataChunk -> IntegrityExtent mapping (plus generations and the monotonic
// generation counter) is persisted in the DDisk chunk-map log by the actor and restored on boot
// via ApplyMappingSnapshot(). A durable increment always references a fully formatted extent (and,
// when it carries an IntegrityChunk, a fully formatted chunk), so every restored chunk is Ready.
// Extent formatting writes valid TIntegrityBlock images with empty bitmaps. Data writes persist
// updated bitmaps, checksums and digests in ping-pong TIntegrityBlock pairs. Restored extents start
// with unknown bitmaps and lazily load the pairs needed by reads or writes.
//
// Memory: used-block bitmaps are small (1 bit per 4 KiB data block) and are kept per extent,
// never evicted - reads depend on them. The expected digest and current slot are also pinned per
// pair so an acknowledged lost metadata write remains detectable after cache eviction. Checksum
// arrays are kept sparsely, one TIntegrityBlockState (~4 KiB) per resident TIntegrityBlock,
// bounded by a manager-wide LRU budget and loaded again from the slot pair after eviction.
//
// On-disk layout of an integrity chunk (same size as a data chunk):
//   [0, IntegrityChunkHeaderRegionSize)  - TIntegrityChunkHeader replicas
//   then ExtentsPerChunk() extents, each occupying ExtentOnDiskSize() bytes: BlocksPerExtent()
//   ping-pong pairs of two adjacent 4 KiB TIntegrityBlock slots (A then B). Formatting writes both
//   slots with PairSequenceNumber 0 (A) and 1 (B), so slot B starts as the current one.
////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class TIntegrityManager {
public:
    struct TDataChunkKey {
        ui64 TabletId = 0;
        ui64 VChunkIndex = 0;

        friend constexpr auto operator<=>(const TDataChunkKey&, const TDataChunkKey&) = default;

        template <typename H>
        friend H AbslHashValue(H h, const TDataChunkKey& key) {
            return H::combine(std::move(h), key.TabletId, key.VChunkIndex);
        }
    };

    struct TExtentRef {
        TChunkIdx IntegrityChunkIdx = 0;
        ui32 ExtentSlot = 0;
        ui64 VChunkGeneration = 0;
    };

    enum class EWriteIoKind { Pair, ChunkHeader, ExtentFormat };
    enum class EOperationStatus { Ok, Corrupted, Failed };

    struct TIoResult {
        bool Ok = false;
        TRope Data;
    };

    struct TPairRead {
        ui64 Id = 0;
        TChunkIdx ChunkIdx = 0;
        ui32 OffsetInBytes = 0;
        ui32 Size = 0;
    };

    struct TPairReadResult {
        ui64 Id = 0;
        TIoResult Result;
    };

    struct IHost {
        virtual ~IHost() = default;
        virtual NActors::IActor& Actor() = 0;
        // Must retain the factory until its coroutine finishes; async<T> is nonmovable.
        virtual void Launch(std::function<NActors::async<void>()> factory) = 0;
        virtual NActors::async<TChunkIdx> Allocate() = 0;
        virtual void ReturnChunk(TChunkIdx chunk) = 0;
        // Each descriptor must eventually reach CompletePairReads, including submission failure.
        virtual void SubmitPairReads(std::vector<TPairRead> reads) = 0;
        virtual NActors::async<bool> Write(TChunkIdx chunk, ui32 offset, TRcBuf data,
            EWriteIoKind kind) = 0;
    };

    // ---- read plans ----

    struct TReadPlan {
        enum EKind {
            Passthrough, // read from disk as-is because every block of the range is used
            AllZero,     // no block of the range was ever written: reply zeros without disk I/O
            Mixed,       // read from disk, then zero the unused blocks according to UsedBlocks
        };

        EKind Kind = Passthrough;
        // Mixed only: bit i corresponds to the i-th IntegrityUnitSize block of the requested range;
        // set = keep disk data, unset = zero-fill.
        TDynBitMap UsedBlocks;
    };

    struct TOperationResult {
        EOperationStatus Status = EOperationStatus::Ok;
        TString ErrorReason;
        bool LostWriteDetected = false;
        std::vector<ui64> Checksums;
        TReadPlan ReadPlan;
    };

    struct TOperationState {
        std::optional<TOperationResult> Result;
        // Stop can publish logical failure before a read's shared loads retire.
        bool Settled = true;
        // Notified only after Result is set; completion is retained for every waiter.
        NActors::TAsyncEvent Changed;
        std::function<void()> CompletionCallback;
    };

    class TOperation {
    public:
        class [[nodiscard]] TWaitAwaiter {
        public:
            static constexpr bool IsActorAwareAwaiter = true;

            explicit TWaitAwaiter(std::shared_ptr<TOperationState> state)
                : State(std::move(state))
                , EventWaiter(State->Changed.Wait())
            {}

            TWaitAwaiter(const TWaitAwaiter&) = delete;
            TWaitAwaiter(TWaitAwaiter&&) = delete;
            TWaitAwaiter& operator=(const TWaitAwaiter&) = delete;
            TWaitAwaiter& operator=(TWaitAwaiter&&) = delete;

            TWaitAwaiter& CoAwaitByValue() && noexcept { return *this; }
            bool await_ready() const noexcept { return State->Result.has_value(); }
            void await_suspend(std::coroutine_handle<> continuation) noexcept {
                EventWaiter.await_suspend(continuation);
            }
            bool await_cancel(std::coroutine_handle<> continuation) noexcept {
                return EventWaiter.await_cancel(continuation);
            }
            TOperationResult await_resume() const {
                Y_ABORT_UNLESS(State->Result);
                return *State->Result;
            }

        private:
            // Destroy the event waiter before releasing the state that owns its queue.
            std::shared_ptr<TOperationState> State;
            decltype(State->Changed.Wait()) EventWaiter;
        };

        TOperation() = default;
        explicit TOperation(std::shared_ptr<TOperationState> state) : State(std::move(state)) {}
        const TOperationResult* GetResult() const { return State && State->Result ? &*State->Result : nullptr; }
        bool IsSettled() const { return State && State->Settled; }
        // Actor-local callback, used by the read's ordinary aggregate awaiter. Passing an empty
        // callback detaches its observer without canceling shared metadata work.
        void SetCompletionCallback(std::function<void()> callback) const {
            Y_ABORT_UNLESS(State);
            if (State->Result && State->Settled) {
                if (callback) { callback(); }
            } else {
                State->CompletionCallback = std::move(callback);
            }
        }
        TWaitAwaiter Wait(NActors::IActor&) const {
            Y_ABORT_UNLESS(State);
            return TWaitAwaiter(State);
        }
    private:
        std::shared_ptr<TOperationState> State;
    };

    struct TReadPreparation {
        TOperation Pending;
        // Only newly claimed loads. Existing loads are joined through Pending.
        std::vector<TPairRead> Reads;
    };

    struct TExtentState {
        bool Placed = false;
        bool Ready = false;
        bool Failed = false;
        NActors::TAsyncEvent Changed;
    };
    class TExtent {
    public:
        explicit TExtent(std::shared_ptr<TExtentState> state) : State(std::move(state)) {}
        std::optional<bool> GetPlacedResult() const {
            if (State->Placed || State->Failed) { return State->Placed; }
            return std::nullopt;
        }
        std::optional<bool> GetReadyResult() const {
            if (State->Ready || State->Failed) { return State->Ready; }
            return std::nullopt;
        }
        NActors::async<bool> WaitPlaced(NActors::IActor& actor) const { return WaitImpl(actor, State, false); }
        NActors::async<bool> WaitReady(NActors::IActor& actor) const { return WaitImpl(actor, State, true); }
    private:
        static NActors::async<bool> WaitImpl(NActors::IActor& actor,
            std::shared_ptr<TExtentState> state, bool ready);
        std::shared_ptr<TExtentState> State;
    };

    // ---- persistence hooks ----

    struct TMappingSnapshot {
        struct TIntegrityChunkEntry {
            TChunkIdx ChunkIdx = 0;
            ui64 Generation = 0;
        };

        struct TExtentEntry {
            TDataChunkKey Key;
            TChunkIdx DataChunkIdx = 0;
            TExtentRef Ref;
        };

        std::vector<TIntegrityChunkEntry> IntegrityChunks;
        std::vector<TExtentEntry> Extents;
        // Last generation value ever assigned (see AllocateGeneration); restore resumes past the
        // maximum of this watermark and every generation in the restored records.
        ui64 GenerationCounter = 0;
    };

public:
    // Approximate memory cost of one cached TIntegrityBlockState; the ctor's checksumCacheBytes
    // budget is converted to a state count with it (tests pass N * BlockStateApproxBytes).
    static constexpr size_t BlockStateApproxBytes =
        128 /* struct + hash map overhead */ + ChecksumsPerIntegrityBlock * sizeof(ui64)
        + ChecksumsPerIntegrityBlock / 8;

    static constexpr ui64 DefaultChecksumCacheBytes = 64ull << 20;

public:
    // Geometry is derived from the data chunk size so that unit tests can use small chunks.
    // ddiskId / pdiskGuid are stamped into TIntegrityChunkHeader. checksumCacheBytes bounds the
    // memory spent on evictable checksum arrays and their cache state (see the memory note above).
    TIntegrityManager(IHost& host, ui64 dataChunkSizeBytes, ui64 ddiskId, ui64 pdiskGuid,
        ui64 checksumCacheBytes = DefaultChecksumCacheBytes);

    TExtent StartExtent(TDataChunkKey key, TChunkIdx dataChunkIdx);
    // Preparation claims and pins metadata but does not submit I/O. The caller can combine Reads
    // and its data range into one vector operation. Every claimed descriptor requires completion.
    TReadPreparation PrepareRead(TDataChunkKey key, ui32 offsetInBytes, ui32 size,
        std::optional<TOperationResult>& readyResult);
    void CompletePairReads(TConstArrayRef<TPairReadResult> results);
    TOperation StartRead(TDataChunkKey key, ui32 offsetInBytes, ui32 size);
    TOperation StartWrite(TDataChunkKey key, ui32 offsetInBytes, ui32 size,
        const std::vector<ui64>& checksums);
    // Close admission and wake logical waits. Shared loads retain their pins until completion.
    void Stop();

    // Starts a durable tablet deletion. Matching extents stop participating in snapshots and
    // pending assignment, but their slots remain withheld until CommitTabletChunksDeletion():
    // reusing a slot before the deletion snapshot commits could overwrite an extent that recovery
    // would still map to the old data chunk.
    void PrepareTabletChunksDeletion(ui64 tabletId);

    // Completes a prepared deletion after its removal snapshot commits. The extents are erased and
    // their slots become available for pending allocations / integrity-chunk reclamation.
    void CommitTabletChunksDeletion(ui64 tabletId);

    // ---- integrity chunk / I/O completions ----

    ui64 GetGenerationCounter() const { return GenerationCounter; }

    // Removes and returns the integrity chunks that can be released back to PDisk: header writes
    // settled (Ready), every slot free (slots withheld by in-flight orphaned format writes do not
    // count as free, so no extent I/O targets these chunks) and no pending extent demand - pending
    // extents are assigned into free slots first, which may queue their format writes.
    std::vector<TChunkIdx> TakeReleasableIntegrityChunks();

    bool IsExtentReady(TDataChunkKey key) const;
    TReadPlan MakeReadPlan(TDataChunkKey key, ui32 offsetInBytes, ui32 size) const;

    // ---- persistence hooks ----

    // Captures the Ready part of the mapping (integrity chunks + key -> extent). In-flight
    // allocations are deliberately excluded: they will be redone after recovery.
    TMappingSnapshot SnapshotMapping() const;

    // Rebuilds the mapping from a snapshot; the manager must be freshly constructed. Bitmaps and
    // checksums are not part of the snapshot, so restored extents are marked with unknown bitmaps and load the
    // required pairs before returning checksums and a matching read plan.
    // Every restored chunk is Ready: a durable increment is only logged after formatting.
    void ApplyMappingSnapshot(const TMappingSnapshot& snapshot);

    // ---- geometry / introspection (for the actor and unit tests) ----

    ui32 DataBlocksInChunk() const { return DataBlocksPerChunkCount; }
    ui32 BlocksPerExtent() const { return BlocksPerExtentCount; }
    ui32 ExtentsPerChunk() const { return ExtentsPerChunkCount; }
    size_t ExtentOnDiskSize() const { return ExtentOnDiskSizeBytes; }

    static constexpr ui32 ChunkHeaderReplicaCount = 3;
    // Offset of the i-th TIntegrityChunkHeader replica within the chunk.
    ui32 ChunkHeaderReplicaOffset(ui32 replica) const;
    // Offset of the extent slot within the chunk.
    ui32 ExtentOffset(ui32 extentSlot) const;

    const TExtentRef* FindExtentRef(TDataChunkKey key) const;
    ui64 GetIntegrityChunkGeneration(TChunkIdx chunkIdx) const;
    // Includes chunks whose headers or extents are still being formatted.
    std::vector<TChunkIdx> GetIntegrityChunkIdxs() const;
    // True once all header replicas of the chunk were written (State == Ready). False for chunks
    // the manager does not know yet.
    bool IsIntegrityChunkFormatted(TChunkIdx chunkIdx) const;
    // Digest of the given TIntegrityBlock (pair) of the key's extent; 0 when nothing was recorded
    // (or the state was evicted).
    ui64 GetIntegrityBlockDigest(TDataChunkKey key, ui32 integrityBlockIdx) const;
    // Recorded checksum of the given data block; returns false when the block has no known checksum.
    bool GetBlockChecksum(TDataChunkKey key, ui32 blockIdx, ui64* checksum) const;
    // Currently cached TIntegrityBlockState count and the cache capacity (for unit tests).
    size_t CachedBlockStates() const { return BlockStateCount; }
    size_t MaxCachedBlockStates() const { return MaxBlockStates; }
    bool HasInFlightOperationsForTablet(ui64 tabletId) const;

private:
    enum class EChunkState {
        Formatting, // TIntegrityChunkHeader replica writes are in flight
        Ready,
    };

    struct TIntegrityChunkInfo {
        EChunkState State = EChunkState::Formatting;
        ui64 Generation = 0;
        std::vector<ui32> FreeSlots; // kept descending, so the smallest slot is assigned first
        ui32 HeaderWritesRemaining = 0;
        std::shared_ptr<TExtentState> Completion = std::make_shared<TExtentState>();
    };

    enum class EExtentState {
        Pending,    // waiting for an integrity chunk with a free slot
        Formatting, // extent-format write is in flight, or done and waiting for chunk headers
        Ready,
    };

    // Checksums of one TIntegrityBlock (ChecksumsPerIntegrityBlock data blocks), allocated lazily
    // on the first checksummed write to its range and evictable via the manager-wide LRU.
    struct TIntegrityBlockState : TIntrusiveListItem<TIntegrityBlockState> {
        TDataChunkKey Key;   // owning extent, for eviction
        ui32 PairIdx = 0;    // TIntegrityBlock pair index within the extent
        ui64 PairSequenceNumber = 0;
        TDynBitMap Known;              // per checksum slot: Checksums[slot] is recorded
        std::vector<ui64> Checksums;   // ChecksumsPerIntegrityBlock entries
    };

    enum class EPairSlot : ui8 {
        Unknown,
        A,
        B,
    };

    // Small, pinned state used for lost-write detection. Checksum arrays remain evictable, but the
    // expected digest must survive their eviction.
    struct TPairMeta {
        ui64 Digest = 0;
        ui32 OperationPins = 0;
        EPairSlot CurrentSlot = EPairSlot::Unknown;
        bool DigestKnown : 1 = false;
        bool BitmapKnown : 1 = false;
        bool Resident : 1 = false;
        bool Corrupted : 1 = false;
    };

    static_assert(sizeof(TPairMeta) <= 16);

    // Sparse state only for pairs with queued/in-flight work (or a remembered corruption). It is
    // removed when a pair becomes idle, keeping the per-disk pinned footprint at TPairMeta size.
    struct TPendingRead;

    struct TPairRuntime {
        ui64 MutationVersion = 0;
        ui64 DurableVersion = 0;
        bool Loading = false;
        bool Flushing = false;
        bool Dirty = false;
        bool Failed = false;
        bool LostWriteCorruption = false;
        TString CorruptionReason;
        NActors::TAsyncEvent Changed;
        std::vector<std::weak_ptr<TPendingRead>> Readers;
    };

    struct TPendingRead {
        ui64 Id = 0;
        TDataChunkKey Key;
        ui32 Offset = 0;
        ui32 Size = 0;
        ui32 Remaining = 0;
        std::shared_ptr<TOperationState> Completion;
    };

    struct TPairLoad {
        TDataChunkKey Key;
        ui32 PairIdx = 0;
        std::shared_ptr<TPairRuntime> Runtime;
    };

    struct TExtentInfo {
        TExtentRef Ref; // valid once State >= Formatting
        EExtentState State = EExtentState::Pending;
        TChunkIdx DataChunkIdx = 0;
        bool Formatting = false;
        std::shared_ptr<TExtentState> Completion = std::make_shared<TExtentState>();
        bool FormatComplete = false;
        // Set while the actor's tablet-removal snapshot is in flight. The extent is absent from
        // logical snapshots, but its physical slot is quarantined until that record is durable.
        bool DeletionPending = false;

        // Empty until the first write to the chunk; never evicted (reads depend on it).
        TDynBitMap UsedBlocks; // per data block of the chunk
        // One pinned entry per on-disk A/B pair.
        std::vector<TPairMeta> Pairs;
        absl::flat_hash_map<ui32, std::shared_ptr<TPairRuntime>> PairRuntime;
        // Sparse per-TIntegrityBlock checksum states, keyed by TIntegrityBlock index.
        absl::flat_hash_map<ui32, std::unique_ptr<TIntegrityBlockState>> BlockStates;
    };

private:
    ui64 AllocateGeneration() { return ++GenerationCounter; }
    void EnsureChunkCapacity();
    void TryAssignExtents();
    void FreeExtent(TDataChunkKey key, TExtentInfo& extent);
    void ReleaseSlot(TChunkIdx chunkIdx, ui32 extentSlot);
    bool CancelChunkAllocationIfExcess();
    void OnIntegrityChunkAllocated(TChunkIdx chunkIdx);
    ui32 FirstPair(ui32 offsetInBytes) const;
    ui32 EndPair(ui32 offsetInBytes, ui32 size) const;
    static NActors::async<void> AllocateChunk(NActors::IActor& actor, TIntegrityManager& self);
    static NActors::async<void> FormatHeader(NActors::IActor& actor, TIntegrityManager& self,
        TChunkIdx chunkIdx, ui32 replica);
    static NActors::async<void> FormatExtent(NActors::IActor& actor, TIntegrityManager& self,
        TDataChunkKey key, TExtentRef ref, std::shared_ptr<TExtentState> completion);
    TPairRead ClaimPairRead(TDataChunkKey key, TExtentInfo& extent, ui32 pairIdx,
        const std::shared_ptr<TPairRuntime>& runtime);
    void FinishPendingRead(const std::shared_ptr<TPendingRead>& read);
    static void CompleteOperation(const std::shared_ptr<TOperationState>& completion, TOperationResult result);
    static NActors::async<void> FlushPair(NActors::IActor& actor, TIntegrityManager& self,
        TDataChunkKey key, ui32 pairIdx, std::shared_ptr<TPairRuntime> runtime);
    static NActors::async<void> RunOperation(NActors::IActor& actor, TIntegrityManager& self,
        TDataChunkKey key, ui32 offset, ui32 size, std::vector<ui64> checksums,
        std::shared_ptr<TOperationState> completion);
    TOperation StartOperation(TDataChunkKey key, ui32 offset, ui32 size,
        std::vector<ui64> checksums);
    void ValidateOperationRange(ui32 offset, ui32 size) const;
    void CollectReadResult(TExtentInfo& extent, ui32 offset, ui32 size,
        TOperationResult& result, bool touch);
    TRcBuf MakePairImage(TDataChunkKey key, TExtentInfo& extent, ui32 pairIdx);
    bool LoadPairImage(TDataChunkKey key, TExtentInfo& extent, ui32 pairIdx, const TRope& data,
        TString* errorReason, bool* lostWriteDetected);
    TIntegrityBlockIdentity MakeBlockIdentity(TDataChunkKey key, const TExtentInfo& extent,
        ui32 pairIdx) const;
    bool PairHasAllUsedChecksums(const TExtentInfo& extent, ui32 pairIdx,
        const TIntegrityBlockState& state) const;
    std::shared_ptr<TPairRuntime> GetPairRuntime(TExtentInfo& extent, ui32 pairIdx);
    void MaybeDropPairRuntime(TExtentInfo& extent, ui32 pairIdx);

    // Get-or-create the state of the given TIntegrityBlock, touching the LRU; creation may evict
    // the least recently used state (of any extent) when over budget.
    TIntegrityBlockState& GetOrCreateBlockState(TDataChunkKey key, TExtentInfo& extent, ui32 pairIdx);
    // Lookup without creation (still touches the LRU); nullptr when absent (never written/evicted).
    TIntegrityBlockState* FindBlockState(TExtentInfo& extent, ui32 pairIdx);
    void EvictBlockStatesOverBudget();
    void DropBlockStates(TExtentInfo& extent);

private:
    IHost& Host;
    bool Stopped = false;
    // Geometry, computed once in the ctor.
    const ui64 DataChunkSize;
    const ui32 DataBlocksPerChunkCount;
    const ui32 BlocksPerExtentCount;
    const size_t ExtentOnDiskSizeBytes;
    const ui32 ExtentsPerChunkCount;

    const ui64 DDiskId;
    const ui64 PDiskGuid;

    absl::flat_hash_map<TChunkIdx, TIntegrityChunkInfo> IntegrityChunks;
    absl::flat_hash_map<TDataChunkKey, TExtentInfo> Extents;
    absl::flat_hash_map<ui64, TPairLoad> PairLoads;
    absl::flat_hash_map<ui64, std::shared_ptr<TPendingRead>> PendingReads;
    ui64 NextPairReadId = 1;
    ui64 NextPendingReadId = 1;

    // Monotonic source of every VChunkGeneration / IntegrityChunkGeneration; persisted as a
    // snapshot watermark, so reuse after free keeps bumping generations even across restarts
    // (lost-write protection).
    ui64 GenerationCounter = 0;

    std::deque<TDataChunkKey> PendingExtents;
    ui32 PendingChunkAllocations = 0; // awaitable reservations not yet fulfilled

    // LRU over all cached TIntegrityBlockStates: front is the eviction victim.
    TIntrusiveList<TIntegrityBlockState> BlockStateLru;
    size_t BlockStateCount = 0;
    const size_t MaxBlockStates;


};

} // namespace NKikimr::NDDisk
