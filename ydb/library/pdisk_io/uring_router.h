#pragma once

#include <util/system/fhandle.h>
#include <util/system/thread.h>
#include <ydb/library/actors/core/actorsystem.h>

#include <atomic>

struct io_uring;
struct io_uring_sqe;

#include <sys/uio.h>

namespace NKikimr::NPDisk {

using NActors::TActorSystem;

// Configuration for TUringRouter
struct TUringRouterConfig {
    ui32 QueueDepth = 128;          // SQ/CQ ring size (= max inflight I/O operations)
    ui32 SqThreadIdleMs = 2000;     // SQPOLL thread idle timeout before sleeping
    bool UseSQPoll = true;          // IORING_SETUP_SQPOLL
    bool UseIOPoll = true;          // IORING_SETUP_IOPOLL (for NVMe/polled devices)
};

// Base operation struct passed through io_uring user_data.
// Callers derive from this and add their own context fields.
// Must be allocated from a pool -- no dynamic allocation in the hot path.
struct TUringOperation {
    // Filled by TUringRouter on completion:
    i32 Result = 0;  // io_uring cqe->res: bytes transferred on success, -errno on failure

    // Called from the dedicated completion polling thread.
    // MUST NOT use TActivationContext -- use actorSystem->Send() instead.
    // After OnComplete returns, the caller is free to return this object to its pool.
    void (*OnComplete)(TUringOperation* op, TActorSystem* actorSystem) = nullptr;

    // Scratch space for the iovec used by readv/writev submissions.
    // Populated by TUringRouter; must remain valid until OnComplete is called.
    struct iovec Iov = {};
};

class TUringRouter {
public:
    // fd:          duplicated file descriptor for the block device (from IBlockDevice::DuplicateFd)
    // actorSystem: for use in completion callbacks (may be nullptr in tests)
    // config:      io_uring setup parameters
    TUringRouter(FHANDLE fd, TActorSystem* actorSystem, TUringRouterConfig config = {});
    ~TUringRouter();

    // --- Setup (call before I/O) ---

    // Register the fd as a fixed file. After this, all I/O uses the registered
    // index, avoiding per-I/O fget()/fput() overhead. Returns true on success.
    bool RegisterFile();

    // Register a set of pre-allocated aligned buffers for fixed-buffer I/O.
    // iovs[i].iov_base must be aligned to device sector size (typically 4096).
    // Returns true on success. (For future PDisk use; DDisk uses TRcBuf directly.)
    bool RegisterBuffers(const struct iovec* iovs, unsigned count);

    // --- Submission (call from a single thread, e.g., DDisk actor) ---

    // Submit a read. Buffer must be aligned and large enough.
    // op must remain alive until op->OnComplete is called.
    // Returns true if SQE was written to the ring, false if SQ is full.
    bool Read(void* buf, ui32 size, ui64 offset, TUringOperation* op);
    bool Write(const void* buf, ui64 size, ui64 offset, TUringOperation* op);

    // Fixed-buffer variants (requires prior RegisterBuffers).
    // bufIndex is the index into the registered iovec array.
    // (For future PDisk use; DDisk uses the non-fixed variants above.)
    bool ReadFixed(void* buf, ui32 size, ui64 offset, ui16 bufIndex, TUringOperation* op);
    bool WriteFixed(const void* buf, ui64 size, ui64 offset, ui16 bufIndex, TUringOperation* op);

    // Ensure all prepared SQEs are visible to the kernel.
    // With SQPOLL, this is typically a no-op; if the SQPOLL thread is sleeping,
    // this wakes it up via io_uring_enter().
    // Without SQPOLL, this calls io_uring_submit().
    void Flush();

    // --- Lifecycle ---

    // Stop the completion thread and tear down the io_uring instance.
    // Outstanding operations' OnComplete will NOT be called.
    void Stop();

    // Returns the number of SQEs still available in the ring.
    ui32 SubmitItemsLeft() const;

    // Returns true if a basic io_uring instance can be created on this system.
    // Use in tests to skip when running in restricted environments (seccomp, containers, etc.).
    static bool Probe();

private:
    struct io_uring* Ring;
    FHANDLE Fd;
    int FixedFdIndex = -1;             // -1 means fd is not registered
    bool BuffersRegistered = false;
    TActorSystem* ActorSystem;
    TUringRouterConfig Config;

    // Dedicated completion polling thread
    class TCompletionPoller;
    THolder<TCompletionPoller> Poller;
    std::atomic<bool> Running{true};

    // Internal helpers
    struct io_uring_sqe* GetSqe();
    void PrepareSqe(struct io_uring_sqe* sqe, bool isRead, void* buf, ui32 size,
                    ui64 offset, TUringOperation* op);
};

} // namespace NKikimr::NPDisk
