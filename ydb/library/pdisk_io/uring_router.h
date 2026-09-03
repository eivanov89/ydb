#pragma once

#include "uring_operation.h"
#include "uring_router_client.h"
#include "device_io_sample.h"

#include <library/cpp/monlib/dynamic_counters/counters.h>
#include <library/cpp/threading/queue/mpsc_vinfarr_obstructive.h>

#include <util/generic/string.h>
#include <util/system/event.h>
#include <util/system/file.h>
#include <util/system/fhandle.h>

#include <sys/uio.h>

#include <atomic>
#include <functional>
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

struct TUringCounters {
    NMonitoring::TDynamicCounters::TCounterPtr CompletionThreadCPU;
    NMonitoring::TDynamicCounters::TCounterPtr CompletionThreadBusyTimeNs;
};

// TUringRouter owns one io_uring instance for one device, including the
// duplicated disk fd passed to the constructor. Submit(), Read(), Write(),
// ReadFixed(), and WriteFixed() are safe to call concurrently: callers only
// publish operations to an MPSC queue. One dedicated I/O thread is the ring's
// sole submitter and reaper, as required by IORING_SETUP_SINGLE_ISSUER and
// IORING_SETUP_DEFER_TASKRUN. It batches submissions, reaps completions, and
// invokes operation callbacks.
//
// DDisk and PersistentBuffer should hold IUringRouterClient, not TUringRouter,
// so they cannot Start()/Stop() a shared ring.
//
// RegisterFile(), RegisterBuffers(), SetSampleSink(), and Start() are setup
// operations and must be called by one thread before concurrent submission.
// Stop() closes admission, waits for producers already inside Submit(), then
// drains every accepted operation through OnComplete() before returning. It
// may run concurrently with Submit() and with another Stop().
//
// Optional device I/O sample sink: if set via SetSampleSink() before Start(),
// the I/O thread invokes it once per successfully completed Read/Write CQE.
// The sink must be cheap and thread-safe on its own.
using TDeviceIoSampleSink = std::function<void(const TDeviceIoSample&)>;

class TUringRouter : public IUringRouterClient {
public:
    TUringRouter(
        TFileHandle fd,
        NActors::TActorSystem* actorSystem,
        TUringRouterConfig config = {},
        TUringCounters counters = {});
    TUringRouter(FHANDLE, NActors::TActorSystem*, TUringRouterConfig = {}, TUringCounters = {}) = delete;

    ~TUringRouter() override;

    const TUringRouterConfig& GetConfig() const override {
        return Config;
    }

    // Must be called before Start().
    void SetSampleSink(TDeviceIoSampleSink sink) {
        SampleSink = std::move(sink);
    }

    // --- Setup (call before Start) ---
    //
    // IORING_SETUP_SINGLE_ISSUER requires registration to be performed by the
    // ring's issuer. These methods only record requests; the dedicated I/O
    // thread performs the registrations during Start(). Start() blocks until
    // initialization completes. Inspect the results afterwards with
    // IsFileRegistered()/AreBuffersRegistered() and the corresponding errno
    // accessors.

    void RegisterFile();

    // iovs must remain valid until Start() returns.
    void RegisterBuffers(const struct iovec* iovs, unsigned count);

    // Starts the dedicated I/O thread and blocks until the ring has been
    // enabled and requested registrations have completed.
    void Start();

    // --- Submission (thread-safe) ---

    // Enqueue a prepared operation. Publishing transfers its lifetime to the
    // router, and the I/O thread may invoke OnComplete() even before Submit()
    // returns. A caller transferring a smart pointer must therefore release it
    // before this call and restore it only if false is returned. False means
    // the router has not been started or is stopping/stopped and no callback
    // will be delivered. Every accepted operation gets exactly one OnComplete()
    // callback before Stop() returns.
    bool Submit(TUringOperationBase* op);

    bool Read(TUringOperationBase* op) override;
    bool Write(TUringOperationBase* op) override;

    // Fixed-buffer variants require successful RegisterBuffers() during Start().
    bool ReadFixed(void* buf, ui32 size, ui64 offset, ui16 bufIndex, TUringOperationBase* op);
    bool WriteFixed(const void* buf, ui32 size, ui64 offset, ui16 bufIndex, TUringOperationBase* op);

    // Compatibility no-op. The I/O thread owns batching and submission.
    void Flush();

    // Close admission, drain accepted operations, stop the I/O thread, and
    // tear down the ring. Safe to call repeatedly and concurrently.
    void Stop();

    bool IsFileRegistered() const;
    bool AreBuffersRegistered() const;
    int GetRegisterFileErrno() const;
    int GetRegisterBuffersErrno() const;

    EUringFavor GetUringFavor() const;

    // Number of accepted operations that are queued, submitted, or currently
    // executing their completion callback.
    ui32 GetInflight() const;

    // Returns true if a disabled io_uring instance can be created and enabled
    // with either the modern flags or the plain fallback configuration.
    static bool Probe(TUringRouterConfig config = {});

private:
    static constexpr ui32 LifecycleCreated = 0;
    static constexpr ui32 LifecycleRunning = 1;
    static constexpr ui32 LifecycleStopping = 2;
    static constexpr ui32 LifecycleStopped = 3;

    class TIoThread;

    struct io_uring_sqe* GetSqe();
    void PrepareSqe(struct io_uring_sqe* sqe, TUringOperationBase* op);

    // Dedicated-I/O-thread methods.
    void InitializeOnIoThread();
    bool DrainSubmitQueue();
    ui32 ReapCompletions();
    void SubmitPendingSqes();
    void ParkAndWait();
    void HandleStop();

    bool BeginSubmit();
    void EndSubmit();
    void WakeIoThreadIfParked();

    static ui64 PackLifecycle(ui32 state, ui32 producers);
    static ui32 UnpackLifecycle(ui64 value);
    static ui32 UnpackProducers(ui64 value);
    static TUringOperationBase* QueueStopSentinel();

private:
    TFileHandle Fd;
    NActors::TActorSystem* ActorSystem;
    TUringRouterConfig Config;
    TUringCounters Counters;
    TDeviceIoSampleSink SampleSink;

    std::unique_ptr<struct io_uring> Ring;
    bool UsedModernFlags = false;

    int FixedFdIndex = -1;
    bool BuffersRegistered = false;
    int RegisterFileErrno = 0;
    int RegisterBuffersErrno = 0;

    bool WantRegisterFile = false;
    bool WantRegisterBuffers = false;
    const struct iovec* PendingIovs = nullptr;
    unsigned PendingIovsCount = 0;

    // Wakes the I/O thread while it is parked. The I/O thread arms an
    // IORING_OP_POLL_ADD on this eventfd so it remains the only ring issuer.
    int WakeEventFd = -1;
    std::atomic<bool> Parked{false};
    bool WakePollArmed = false;

    // Operation popped from Queue while the SQ was full.
    TUringOperationBase* PendingSubmit = nullptr;

    bool StopSeen = false;
    bool SawStopCqeMarker = false;

    NThreading::TObstructiveConsumerQueue<TUringOperationBase, /*DeleteItems=*/false> Queue;

    // High 32 bits: lifecycle state. Low 32 bits: producers between admission
    // and completed queue publication. Stop closes the state, waits for that
    // count to reach zero, and only then appends the stop sentinel.
    std::atomic<ui64> Lifecycle{PackLifecycle(LifecycleCreated, 0)};
    TManualEvent SubmittersDrained;
    TManualEvent StoppedEvent;

    std::atomic<ui32> InFlightCount{0};

    TManualEvent ReadyEvent;
    std::unique_ptr<TIoThread> IoThread;
};

} // namespace NKikimr::NPDisk
