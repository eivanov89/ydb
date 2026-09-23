#pragma once

#include <util/system/types.h>
#include <util/generic/array_ref.h>
#include <library/cpp/containers/stack_vector/stack_vec.h>

#include <vector>

#if defined(__linux__)
#include <sys/uio.h>
#endif

namespace NActors {
    class TActorSystem;
} // namespace NActors

namespace NKikimr::NPDisk {

// Callers derive from this and add their own context fields.
// Should be allocated from a pool to avoid dynamic allocation in the hot path.
class TUringOperationBase {
    friend class TUringRouter;

public:
    struct TReadPart {
        ui64 DiskOffset;
        size_t Size;
        void* Buffer;
    };

    // NHPTimer cycle count captured by TUringRouter right before the operation
    // is submitted to the kernel (SQE prepared). Used together with the
    // completion timestamp (captured by the I/O thread) to build a
    // TDeviceIoSample for device-overestimation tracking. 0 if unset.
    ui64 SubmitCycles = 0;

public:
    enum EOperationType {
        ENOT_SET = 0,
        EREAD,
        EWRITE,
    };

    virtual ~TUringOperationBase();

public:
    // TUringRouter invokes exactly one terminal callback for every operation
    // accepted by Submit().

    // Called from the dedicated I/O thread outside actor system,
    // thus MUST NOT use TActivationContext, instead should use actorSystem->Send().
    // After OnComplete() returns, TUringRouter will not access object anymore.
    virtual void OnComplete(NActors::TActorSystem* actorSystem) noexcept = 0;

    // Called from the dedicated I/O thread when shutdown drops an accepted
    // operation before kernel submission. Use this to release
    // operation-owned memory/resources. Use the supplied actor system for messages;
    // TActivationContext is unavailable here too.
    // After OnDrop() returns, TUringRouter will not access object anymore.
    virtual void OnDrop(NActors::TActorSystem*) noexcept = 0;

public:
    // Prepare a single-buffer I/O.
    // buf must remain valid until OnComplete/OnDrop is called.
    void PrepareIov(void* buf, size_t size, ui64 offset);

    // Independent disk ranges, completed as one accepted operation. Descriptors
    // are copied; all buffers must survive the single terminal callback. Call
    // again with only failed ranges to prepare a selective retry. Unlike
    // scatter/gather, the number of ranges is not limited by MAX_IOVS or SQ depth.
    void PrepareReadParts(TConstArrayRef<TReadPart> parts);
    TConstArrayRef<TReadPart> GetReadParts() const { return ReadParts; }
    i64 GetReadPartResult(size_t index) const;

    // Caller-side fallback backends use the same result slots. SetResult still
    // supplies their logical aggregate result before delivering completion.
    void SetReadPartResult(size_t index, i64 result);

#if defined(__linux__)
    // Begin a scatter-gather I/O: clears the iovec list, reserves room for
    // `count` segments and sets the disk offset.  Follow with `count` AddIov()
    // calls to append each segment.  count must be in (0, MAX_IOVS].
    void PrepareScatterGather(size_t count, ui64 offset);

    // Append one segment to the scatter-gather list started by
    // PrepareScatterGather.  buf must remain valid until OnComplete/OnDrop is
    // called.  Accumulates into TotalSize.
    void AddIov(void* buf, size_t size);
#endif

    void AdvanceIov(size_t bytesProcessed);

    void SetOperationType(EOperationType opType) { OperationType = opType; }
    EOperationType GetOperationType() const { return OperationType; }

    bool IsFixedBuffer() const { return FixedBuffer; }
    ui16 GetBufIndex() const { return BufIndex; }

    // Returns the number of bytes remaining in the current (possibly partially
    // advanced) iovec window. This is zero after a successful logical completion.
    // Multi-range reads sum the independently advanced ranges instead.
    size_t GetOperationBytes() const {
        if (ReadParts.size() > 1) {
            size_t remaining = 0;
            for (const auto& part : ReadCursors) {
                remaining += part.Size;
            }
            return remaining;
        }
#if defined(__linux__)
        return TotalSize - BytesProcessed;
#else
        return GetTotalSize();
#endif
    }

    // Logical terminal result: the total requested byte count on success,
    // or -errno on failure.
    void SetResult(i64 result) { Result = result; }
    i64 GetResult() const { return Result; }

    // Transfer accumulated short-I/O accounting to the completion consumer.
    ui64 TakeShortIoCount() {
        const ui64 count = ShortIoCount;
        ShortIoCount = 0;
        return count;
    }

    ui64 GetTotalSize() const { return TotalSize; }

    ui64 GetDiskOffset() const { return DiskOffset; }

    const void* GetIovBase() const {
        if (ReadParts.size() > 1) {
            return ReadParts.front().Buffer;
        }
#if defined(__linux__)
        if (IovBegin < Iov.size()) {
            return Iov[IovBegin].iov_base;
        }
        return nullptr;
#else
        return nullptr;
#endif
    }

    // Reset all submission/completion state so the object can be reused from a pool.
    // Must be called before PrepareIov() when recycling an operation.
    void ResetSubmissionState() {
        SubmitCycles = 0;
        Result = 0;
        ShortIoCount = 0;
        IsContinuation = false;
        OperationType = ENOT_SET;
        TotalSize = 0;
        DiskOffset = 0;
        FixedBuffer = false;
        BufIndex = 0;
        ReadParts.clear();
        ReadCursors.clear();
        NextReadPart = 0;
        RemainingReadParts = 0;
        ReadPartReachedKernel = false;
#if defined(__linux__)
        Iov.clear();
        IovBegin = 0;
        BytesProcessed = 0;
#endif
    }

#if defined(__linux__)
    // Number of iovecs kept inline (on-stack) without heap allocation.
    static constexpr size_t MAX_STACK_IOVS = 16;

    // Hard upper bound on scatter-gather segments per operation.  Beyond
    // MAX_STACK_IOVS the iovec vector spills to the heap, so this can exceed it.
    static constexpr size_t MAX_IOVS = 64;
#endif

private:
    // Stable cursors are allocated only for a genuine multi-range read. An SQE
    // refers to its cursor, never to a child operation or an additional admission.
    struct TReadCursor {
        TUringOperationBase* Parent = nullptr;
        ui64 DiskOffset = 0;
        void* Buffer = nullptr;
        size_t Size = 0;
        i64 Result = 0;
        ui64 SubmitCycles = 0;
#if defined(__linux__)
        struct iovec Iov{};
#endif
    };

    TStackVec<TReadPart, 1> ReadParts;
    std::vector<TReadCursor> ReadCursors;
    size_t NextReadPart = 0;
    size_t RemainingReadParts = 0;
    bool ReadPartReachedKernel = false;

    // Set by TUringRouter::ReadFixed/WriteFixed before the operation is handed
    // to the I/O thread.
    void SetFixedBuffer(ui16 bufIndex) {
        FixedBuffer = true;
        BufIndex = bufIndex;
    }

    // Filled by TUringRouter at logical completion, across all physical CQEs.
    i64 Result = 0;

    ui64 ShortIoCount = 0;

    // Router-owned continuation of an operation that has already made progress.
    bool IsContinuation = false;

    // Submission metadata for non-fixed Read/Write operations.

    EOperationType OperationType = ENOT_SET;

    // Originally requested byte count; set by PrepareIov/PrepareScatterGather/AddIov
    // and preserved across short-I/O retries so OnComplete knows the full request size.
    // Reset to 0 by ResetSubmissionState() when the op is recycled.
    ui64 TotalSize = 0;

    ui64 DiskOffset = 0;

    // Fixed-buffer operations must remember their registered-buffer index
    // until the I/O thread prepares the SQE.
    bool FixedBuffer = false;
    ui16 BufIndex = 0;

#if defined(__linux__)
    // Iovec array for readv/writev submissions.  Supports scatter-gather: holds one
    // entry for single-buffer I/O or N entries for multi-segment writes.
    // All iov_base pointers must remain valid until OnComplete/OnDrop is called.
    TStackVec<struct iovec, MAX_STACK_IOVS> Iov;

    // Index into Iov of the first not-yet-completed iovec.
    // Advanced by AdvanceIov() on successful physical completions.
    size_t IovBegin = 0;

    // Cumulative bytes consumed by AdvanceIov() across physical completions.
    // GetOperationBytes() == TotalSize - BytesProcessed (remaining window).
    ui64 BytesProcessed = 0;
#endif
};

} // namespace NKikimr::NPDisk
