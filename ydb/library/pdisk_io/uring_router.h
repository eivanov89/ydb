#pragma once

#include "uring_operation.h"

#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <library/cpp/threading/queue/mpsc_vinfarr_obstructive.h>

#include <util/generic/string.h>
#include <util/system/event.h>
#include <util/system/fhandle.h>

#include <sys/uio.h>

#include <atomic>
#include <memory>

struct io_uring;
struct io_uring_sqe;

namespace NActors {
    class TActorSystem;
} // namespace NActors

namespace NKikimr::NPDisk {

enum class EUringFavor {
    SingleIssuer,   // IORING_SETUP_SINGLE_ISSUER | DEFER_TASKRUN | TASKRUN_FLAG (kernel >= 6.1)
    Plain,          // fallback: plain ring, no modern flags, still one dedicated I/O thread
    FallbackPDisk,  // io_uring unavailable at all; caller routes I/O through PDisk instead
};

struct TUringRouterConfig {
    // Target SQ ring size (number of submission slots). The kernel creates a
    // CQ of twice this size by default. Typical devices have hardware queue
    // depth around 128; using 256 entries gives additional headroom to reduce
    // the risk of SQ exhaustion under load and a better device utilization:
    // there is an in-kernel queue in front of the device. Submissions beyond
    // this cap are absorbed by the submit queue (see Submit()).
    ui32 QueueDepth = 256;

    // How long (in microseconds) the dedicated I/O thread busy-polls the
    // submission queue and the completion ring before parking (blocking in
    // the kernel) when there is nothing to do. Lower values trade CPU for
    // submit-wakeup latency.
    ui32 IdleSpinUs = 200;

    TString ToString() const;
};

struct TUringCounters {
    NMonitoring::TDynamicCounters::TCounterPtr CompletionThreadCPU;
    NMonitoring::TDynamicCounters::TCounterPtr CompletionThreadBusyTimeNs;
};

// TUringRouter owns a single io_uring instance for one device and lets
// multiple callers (potentially on different threads, e.g. actors migrating
// across pool threads) share it. A single dedicated I/O thread is the ring's
// only submitter and reaper (required by IORING_SETUP_SINGLE_ISSUER /
// DEFER_TASKRUN on kernel >= 6.1): callers hand ops to that thread through an
// MPSC queue via Submit()/Read()/Write()/ReadFixed()/WriteFixed(), which are
// safe to call concurrently from any thread. The I/O thread batches
// submissions, reaps completions, and invokes op->OnComplete() -- never the
// calling thread.
//
// RegisterFile()/RegisterBuffers()/GetConfig() are the only methods that must
// be called before Start(). Everything else (Submit and friends, Stop,
// GetInflight, ...) is safe to call from multiple threads concurrently once
// Start() has returned.
class TUringRouter {
public:
    TUringRouter(
        FHANDLE fd,
        NActors::TActorSystem* actorSystem,
        TUringRouterConfig config = {},
        TUringCounters* counters = nullptr);

    ~TUringRouter();

    const TUringRouterConfig& GetConfig() const {
        return Config;
    }

    // --- Setup (call before Start) ---
    //
    // IORING_SETUP_SINGLE_ISSUER requires that only the ring's issuer thread
    // performs io_uring_register* calls. Since the issuer is the dedicated
    // I/O thread spawned by Start(), these calls only *request*
    // registration here; the I/O thread performs the actual
    // io_uring_register_files/io_uring_register_buffers calls right after it
    // enables the ring, before Start() returns. Check the outcome via
    // IsFileRegistered() / AreBuffersRegistered() (and GetRegisterFileErrno()
    // / GetRegisterBuffersErrno() for diagnostics) after Start() returns.

    // Request the fd to be registered as a fixed file. After a successful
    // registration all I/O uses the registered index, avoiding per-I/O
    // fget()/fput() overhead.
    void RegisterFile();

    // Request a set of pre-allocated aligned buffers to be registered for
    // fixed-buffer I/O. iovs must remain valid until Start() returns.
    // iovs[i].iov_base must be aligned to device sector size (typically 4096).
    void RegisterBuffers(const struct iovec* iovs, unsigned count);

    // Starts the dedicated I/O thread. That thread becomes the ring's
    // issuer: it enables the (R_DISABLED) ring, performs any requested
    // registrations, and begins processing submissions. Blocks until this
    // initialization has completed.
    void Start();

    // --- Submission (thread-safe, callable concurrently from any thread) ---

    // Fire-and-forget submission: enqueues op for the dedicated I/O thread
    // and returns immediately without touching the kernel. The I/O thread
    // submits (batched with other pending ops) and, once complete, invokes
    // op->OnComplete() from its own thread -- never the caller's.
    // op->Iov/op->DiskOffset/op->OperationType must be initialized before
    // calling (PrepareIov/PrepareScatterGather+AddIov, SetOperationType).
    // op must remain alive until OnComplete()/OnDrop() is called.
    void Submit(TUringOperationBase* op);

    // Convenience wrappers over Submit(). They always return true: unlike
    // the previous single-caller design, the submit queue absorbs bursts
    // beyond the ring's capacity, so submission never fails here. The bool
    // return is kept only for source compatibility with existing callers.
    bool Read(TUringOperationBase* op);
    bool Write(TUringOperationBase* op);

    // Fixed-buffer variants (requires a prior successful RegisterBuffers()).
    // bufIndex is the index into the registered iovec array.
    bool ReadFixed(void* buf, ui32 size, ui64 offset, ui16 bufIndex, TUringOperationBase* op);
    bool WriteFixed(const void* buf, ui32 size, ui64 offset, ui16 bufIndex, TUringOperationBase* op);

    // No-op kept for source compatibility with existing callers: batching and
    // submission to the kernel now happen exclusively on the dedicated I/O
    // thread, driven by Submit().
    void Flush() {
    }

    // --- Lifecycle ---

    // Requests that no more Submit() calls (from any caller) happen once
    // Stop() begins. Drains everything already queued/submitted -- every op
    // handed to Submit() before Stop() was called is guaranteed to reach
    // OnComplete() -- then stops the I/O thread and tears down the ring.
    void Stop();

    bool IsFileRegistered() const;
    bool AreBuffersRegistered() const;
    int GetRegisterFileErrno() const;
    int GetRegisterBuffersErrno() const;

    EUringFavor GetUringFavor() const;

    // Number of ops accepted via Submit() (queued, in the ring, or actually
    // on-device) that have not yet reached OnComplete()/OnDrop().
    ui32 GetInflight() const;

    // Returns true if an io_uring instance can be created and enabled on
    // this system with either the given config or a plain fallback config.
    // Always use in tests to skip when running in restricted environments
    // (seccomp, containers, etc.).
    static bool Probe(TUringRouterConfig config = {});

private:
    class TIoThread;

    struct io_uring_sqe* GetSqe();
    void PrepareSqe(struct io_uring_sqe* sqe, TUringOperationBase* op);

    // Runs on the dedicated I/O thread only.
    void InitializeOnIoThread();
    bool DrainSubmitQueue();
    ui32 ReapCompletions();
    void ParkAndWait();
    void HandleStop();

    void WakeIoThreadIfParked();

    TUringOperationBase* QueueStopSentinel() const;

private:
    FHANDLE Fd;
    NActors::TActorSystem* ActorSystem;
    TUringRouterConfig Config;
    TUringCounters* Counters;

    std::unique_ptr<struct io_uring> Ring;
    bool UsedModernFlags = false;

    int FixedFdIndex = -1; // -1 means fd is not registered
    bool BuffersRegistered = false;
    int RegisterFileErrno = 0;
    int RegisterBuffersErrno = 0;

    bool WantRegisterFile = false;
    bool WantRegisterBuffers = false;
    const struct iovec* PendingIovs = nullptr;
    unsigned PendingIovsCount = 0;

    // Wakes the I/O thread when it is parked; written by Submit()/Stop(),
    // watched by the I/O thread via a self-armed IORING_OP_POLL_ADD so a
    // single thread remains the ring's only submitter/reaper.
    int WakeEventFd = -1;
    std::atomic<bool> Parked{false};
    bool WakePollArmed = false;

    // Popped from the submit queue but could not get an SQE this round
    // (ring momentarily saturated); retried on the next iteration.
    TUringOperationBase* PendingSubmit = nullptr;

    bool StopSeen = false;
    bool SawStopCqeMarker = false;

    NThreading::TObstructiveConsumerQueue<TUringOperationBase, /*DeleteItems=*/false> Queue;

    std::atomic<ui32> InFlightCount{0};

    TManualEvent ReadyEvent;
    std::unique_ptr<TIoThread> IoThread;
};

} // namespace NKikimr::NPDisk
