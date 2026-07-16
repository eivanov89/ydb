#include "uring_router.h"

#include <ydb/core/util/hp_timer_helpers.h>
#include <ydb/library/actors/core/actorsystem.h>

#include <util/string/builder.h>
#include <util/system/compiler.h>
#include <util/system/sanitizers.h>
#include <util/system/thread.h>
#include <util/system/yassert.h>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

// Must be included AFTER YDB headers because linux/uapi headers pulled by
// liburing may define macros that clash with project headers.
#include <ydb/library/uring/liburing_linux.h>

#include <cerrno>
#include <cstring>

using NActors::TActorSystem;

namespace NKikimr::NPDisk {

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace {

// Sentinel pushed into the submit queue by Stop() to mark "no more real ops
// will be pushed after this point"; never dereferenced, only compared by
// address. Distinct from StopCqeMarker below, which lives at the io_uring
// CQE level.
alignas(void*) char QueueStopSentinelStorage;

// user_data marker for the IO_DRAIN NOP submitted by Stop(): it only
// completes after every previously submitted SQE has completed.
alignas(void*) char StopCqeMarker;

// user_data marker for the self-armed poll on WakeEventFd used to wake a
// parked I/O thread.
alignas(void*) char WakePollMarker;

// Safety net for the parked wait: bounds worst-case wake latency even in the
// (never expected in practice) case the eventfd-based wake was somehow
// missed, without requiring a formal proof of the flag/queue race. Actual
// wakeups are driven by the eventfd poll and complete far faster than this.
constexpr ui32 ParkSafetyNetUs = 5000;

struct __kernel_timespec MicrosToTimespec(ui32 micros) {
    struct __kernel_timespec ts;
    ts.tv_sec = micros / 1'000'000;
    ts.tv_nsec = static_cast<long long>(micros % 1'000'000) * 1000;
    return ts;
}

int CreateWakeEventFd() {
    for (;;) {
        int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd >= 0 || errno != EINTR) {
            return fd;
        }
    }
}

void ConfigureParams(bool modern, struct io_uring_params& params) {
    memset(&params, 0, sizeof(params));
    // Created disabled: the I/O thread becomes the ring's issuer by being
    // the task that calls io_uring_enable_rings() (required for
    // IORING_SETUP_SINGLE_ISSUER when the ring isn't created on the issuer
    // thread itself).
    params.flags |= IORING_SETUP_R_DISABLED;
    if (modern) {
        // One thread submits and reaps (SINGLE_ISSUER); completions are
        // deferred as local task work processed only when that thread
        // enters the kernel for GETEVENTS (DEFER_TASKRUN) -- zero
        // IPIs/interrupts of any thread. TASKRUN_FLAG lets a userspace peek
        // notice pending task work without a syscall.
        params.flags |= IORING_SETUP_SINGLE_ISSUER
            | IORING_SETUP_DEFER_TASKRUN
            | IORING_SETUP_TASKRUN_FLAG;
    }
}

int InitRingWithFallback(struct io_uring* ring, ui32 queueDepth, bool* usedModernFlags) {
    int lastError = -EINVAL;
    for (bool modern : {true, false}) {
        struct io_uring_params params;
        ConfigureParams(modern, params);

        int ret = io_uring_queue_init_params(queueDepth, ring, &params);
        if (ret == 0) {
            *usedModernFlags = modern;
            return 0;
        }
        lastError = ret;
    }

    return lastError;
}

} // anonymous

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TUringRouter::TIoThread
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// The single submitter and reaper for the ring: owns io_uring_enter() calls
// for both submission and GETEVENTS, as required by
// IORING_SETUP_SINGLE_ISSUER / IORING_SETUP_DEFER_TASKRUN.
class TUringRouter::TIoThread : public ISimpleThread {
public:
    explicit TIoThread(TUringRouter& owner)
        : Owner(owner)
    {}

    void* ThreadProc() override {
        SetCurrentThreadName("UringIo");

        Owner.InitializeOnIoThread();

        bool idle = false;
        NHPTimer::STime idleStart = 0;

        while (!Owner.StopSeen) {
            const NHPTimer::STime cycleStart = HPNow();

            bool didWork = Owner.DrainSubmitQueue();
            if (didWork) {
                io_uring_submit(Owner.Ring.get());
            }

            didWork = Owner.ReapCompletions() > 0 || didWork;

            if (Owner.StopSeen) {
                break;
            }

            auto accountBusy = [&]() {
                if (Owner.Counters) {
                    *Owner.Counters->CompletionThreadBusyTimeNs += HPNanoSeconds(HPNow() - cycleStart);
                }
            };

            if (didWork) {
                accountBusy();
                idle = false;
                continue;
            }

            if (!idle) {
                idle = true;
                idleStart = HPNow();
                accountBusy();
                continue;
            }

            const ui64 idleUs = HPMicroSeconds(HPNow() - idleStart);
            if (idleUs < Owner.Config.IdleSpinUs) {
                // Busy-spin burns CPU; count it so CompletionThreadBusyTimeNs
                // reflects actual thread cost, not only productive I/O cycles.
                accountBusy();
                continue;
            }

            Owner.ParkAndWait();
            idle = false;
        }

        Owner.HandleStop();
        return nullptr;
    }

private:
    TUringRouter& Owner;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TUringRouter
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TUringRouter::TUringRouter(FHANDLE fd, TActorSystem* actorSystem, TUringRouterConfig config, TUringCounters* counters)
    : Fd(fd)
    , ActorSystem(actorSystem)
    , Config(config)
    , Counters(counters)
    , Ring(new struct io_uring())
    , WakeEventFd(CreateWakeEventFd())
{
    Y_ABORT_UNLESS(WakeEventFd >= 0, "eventfd() failed: %s (errno %d)", strerror(errno), errno);

    bool usedModernFlags = false;
    int ret = InitRingWithFallback(Ring.get(), Config.QueueDepth, &usedModernFlags);
    Y_ABORT_UNLESS(ret == 0, "io_uring_queue_init_params failed after fallbacks: %s (errno %d)", strerror(-ret), -ret);
    UsedModernFlags = usedModernFlags;
}

TUringRouter::~TUringRouter() {
    Stop();
    if (WakeEventFd >= 0) {
        close(WakeEventFd);
    }
}

void TUringRouter::RegisterFile() {
    Y_ABORT_UNLESS(!IoThread, "RegisterFile() must be called before Start()");
    WantRegisterFile = true;
}

void TUringRouter::RegisterBuffers(const struct iovec* iovs, unsigned count) {
    Y_ABORT_UNLESS(!IoThread, "RegisterBuffers() must be called before Start()");
    PendingIovs = iovs;
    PendingIovsCount = count;
    WantRegisterBuffers = true;
}

void TUringRouter::Start() {
    Y_ABORT_UNLESS(!IoThread, "Start() called twice");
    IoThread = std::make_unique<TIoThread>(*this);
    IoThread->Start();
    ReadyEvent.WaitI();
}

void TUringRouter::InitializeOnIoThread() {
    // This thread becomes the ring's issuer by enabling it.
    int enableRet = io_uring_enable_rings(Ring.get());
    Y_ABORT_UNLESS(enableRet == 0, "io_uring_enable_rings failed: %s (errno %d)", strerror(-enableRet), -enableRet);

    if (WantRegisterFile) {
        int fd = Fd;
        int ret = io_uring_register_files(Ring.get(), &fd, 1);
        if (ret == 0) {
            FixedFdIndex = 0;
        } else {
            RegisterFileErrno = -ret;
        }
    }

    if (WantRegisterBuffers) {
        int ret = io_uring_register_buffers(Ring.get(), PendingIovs, PendingIovsCount);
        if (ret == 0) {
            BuffersRegistered = true;
        } else {
            RegisterBuffersErrno = -ret;
        }
    }

    ReadyEvent.Signal();
}

TUringOperationBase* TUringRouter::QueueStopSentinel() const {
    return reinterpret_cast<TUringOperationBase*>(&QueueStopSentinelStorage);
}

struct io_uring_sqe* TUringRouter::GetSqe() {
    return io_uring_get_sqe(Ring.get());
}

void TUringRouter::PrepareSqe(struct io_uring_sqe* sqe, TUringOperationBase* op) {
    int fd = (FixedFdIndex >= 0) ? FixedFdIndex : Fd;
    Y_ABORT_UNLESS(op->IovBegin < op->Iov.size(), "PrepareSqe called with empty iovec window");
    const unsigned iovCount = static_cast<unsigned>(op->Iov.size() - op->IovBegin);
    void* base = op->Iov[op->IovBegin].iov_base;
    const size_t len = op->Iov[op->IovBegin].iov_len;

    if (op->IsFixedBuffer()) {
        Y_ABORT_UNLESS(iovCount == 1, "fixed-buffer I/O does not support scatter-gather");
        switch (op->OperationType) {
        case TUringOperationBase::EREAD:
            io_uring_prep_read_fixed(sqe, fd, base, len, op->DiskOffset, op->GetBufIndex());
            break;
        case TUringOperationBase::EWRITE:
            io_uring_prep_write_fixed(sqe, fd, base, len, op->DiskOffset, op->GetBufIndex());
            break;
        default:
            Y_ABORT("Unknown OperationType");
        }
    } else if (iovCount == 1) {
        // Kernel >= 6.6: plain READ/WRITE for single-buffer ops. IORING_OP_
        // READV/WRITEV was only needed for kernel 5.4 compatibility
        // (READ/WRITE landed in 5.6); readv/writev is kept below for
        // genuine scatter-gather (multi-segment) ops.
        switch (op->OperationType) {
        case TUringOperationBase::EREAD:
            io_uring_prep_read(sqe, fd, base, len, op->DiskOffset);
            break;
        case TUringOperationBase::EWRITE:
            io_uring_prep_write(sqe, fd, base, len, op->DiskOffset);
            break;
        default:
            Y_ABORT("Unknown OperationType");
        }
    } else {
        switch (op->OperationType) {
        case TUringOperationBase::EREAD:
            io_uring_prep_readv(sqe, fd, &op->Iov[op->IovBegin], iovCount, op->DiskOffset);
            break;
        case TUringOperationBase::EWRITE:
            io_uring_prep_writev(sqe, fd, &op->Iov[op->IovBegin], iovCount, op->DiskOffset);
            break;
        default:
            Y_ABORT("Unknown OperationType");
        }
    }

    if (FixedFdIndex >= 0) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }

    io_uring_sqe_set_data(sqe, op);
    NSan::Release(op);
}

void TUringRouter::WakeIoThreadIfParked() {
    if (Parked.load(std::memory_order_seq_cst)) {
        // Best-effort: even if this write is interrupted or otherwise lost,
        // the I/O thread's parked wait has a bounded safety-net timeout, so
        // no wakeup can be lost forever.
        ui64 one = 1;
        write(WakeEventFd, &one, sizeof(one));
    }
}

void TUringRouter::Submit(TUringOperationBase* op) {
    Y_DEBUG_ABORT_UNLESS(Ring, "Submit() called after Stop()");
    Y_DEBUG_ABORT_UNLESS(op->GetOperationType() != TUringOperationBase::ENOT_SET,
        "Submit() called with an unprepared op");

    InFlightCount.fetch_add(1, std::memory_order_relaxed);
    Queue.Push(op);
    WakeIoThreadIfParked();
}

bool TUringRouter::Read(TUringOperationBase* op) {
    Y_ABORT_UNLESS(op->GetOperationType() == TUringOperationBase::EREAD);
    Submit(op);
    return true;
}

bool TUringRouter::Write(TUringOperationBase* op) {
    Y_ABORT_UNLESS(op->GetOperationType() == TUringOperationBase::EWRITE);
    Submit(op);
    return true;
}

bool TUringRouter::ReadFixed(void* buf, ui32 size, ui64 offset, ui16 bufIndex, TUringOperationBase* op) {
    Y_ABORT_UNLESS(BuffersRegistered, "RegisterBuffers must succeed before ReadFixed");
    op->SetOperationType(TUringOperationBase::EREAD);
    op->PrepareIov(buf, size, offset);
    op->SetFixedBuffer(bufIndex);
    Submit(op);
    return true;
}

bool TUringRouter::WriteFixed(const void* buf, ui32 size, ui64 offset, ui16 bufIndex, TUringOperationBase* op) {
    Y_ABORT_UNLESS(BuffersRegistered, "RegisterBuffers must succeed before WriteFixed");
    op->SetOperationType(TUringOperationBase::EWRITE);
    op->PrepareIov(const_cast<void*>(buf), size, offset);
    op->SetFixedBuffer(bufIndex);
    Submit(op);
    return true;
}

bool TUringRouter::DrainSubmitQueue() {
    bool didWork = false;
    for (;;) {
        TUringOperationBase* op = PendingSubmit;
        if (op) {
            PendingSubmit = nullptr;
        } else {
            op = Queue.Pop();
            if (!op) {
                break;
            }
            if (op == QueueStopSentinel()) {
                StopSeen = true;
                break;
            }
        }

        struct io_uring_sqe* sqe = GetSqe();
        if (!sqe) {
            // Ring is saturated (in-flight cap reached); hold this op and
            // retry once completions free up space.
            PendingSubmit = op;
            break;
        }
        PrepareSqe(sqe, op);
        didWork = true;
    }
    return didWork;
}

ui32 TUringRouter::ReapCompletions() {
    struct io_uring* ring = Ring.get();
    ui32 count = 0;

    // With IORING_SETUP_DEFER_TASKRUN, completions are queued as deferred
    // task work inside the kernel and are not visible in the CQ ring until
    // the issuer makes an io_uring_enter(GETEVENTS) call. A raw, syscall-free
    // walk of the CQ ring (e.g. io_uring_for_each_cqe) will therefore never
    // observe them. io_uring_peek_cqe() checks IORING_SQ_TASKRUN and issues
    // that syscall itself when needed, while still being non-blocking when
    // there is nothing pending.
    for (;;) {
        struct io_uring_cqe* cqe = nullptr;
        int ret = io_uring_peek_cqe(ring, &cqe);
        if (ret != 0 || !cqe) {
            break;
        }
        void* data = io_uring_cqe_get_data(cqe);
        if (data == &WakePollMarker) {
            WakePollArmed = false;
            ui64 drained = 0;
            while (read(WakeEventFd, &drained, sizeof(drained)) > 0) {
                // keep draining; EFD_NONBLOCK makes this loop terminate
            }
        } else if (data == &StopCqeMarker) {
            SawStopCqeMarker = true;
        } else if (auto* op = reinterpret_cast<TUringOperationBase*>(data)) {
            // The synchronization between the submitter and this thread
            // goes through io_uring's kernel-mediated SQ/CQ rings, which
            // TSAN cannot observe. Acquire here pairs with Release in
            // PrepareSqe.
            NSan::Acquire(op);
            op->Result = cqe->res;
            // For read operations the kernel fills the buffer via a syscall
            // that MSAN cannot observe. Mark each iovec segment as initialized
            // so that subsequent reads do not trigger false use-of-uninitialized-
            // value reports. Walk the active iovec window (starting at IovBegin)
            // and unpoison up to cqe->res bytes across all segments.
            if constexpr (NSan::MSanIsOn()) {
                if (op->OperationType == TUringOperationBase::EREAD && cqe->res > 0) {
                    size_t remaining = static_cast<size_t>(cqe->res);
                    for (size_t i = op->IovBegin; i < op->Iov.size() && remaining > 0; ++i) {
                        size_t unpoisonSize = op->Iov[i].iov_len;
                        if (unpoisonSize > remaining) {
                            unpoisonSize = remaining;
                        }
                        NSan::Unpoison(op->Iov[i].iov_base, unpoisonSize);
                        remaining -= unpoisonSize;
                    }
                }
            }
            op->OnComplete(ActorSystem);
            InFlightCount.fetch_sub(1, std::memory_order_release);
        }
        io_uring_cqe_seen(ring, cqe);
        ++count;
    }

    if (count > 0 && Counters) {
        *Counters->CompletionThreadCPU = ThreadCPUTime();
    }

    return count;
}

void TUringRouter::ParkAndWait() {
    Parked.store(true, std::memory_order_seq_cst);

    // Close the race: if something was pushed right before we published
    // Parked=true, pick it up now instead of blocking.
    if (DrainSubmitQueue()) {
        Parked.store(false, std::memory_order_seq_cst);
        io_uring_submit(Ring.get());
        return;
    }

    if (!WakePollArmed) {
        struct io_uring_sqe* sqe = GetSqe();
        if (sqe) {
            io_uring_prep_poll_add(sqe, WakeEventFd, POLLIN);
            io_uring_sqe_set_data(sqe, &WakePollMarker);
            WakePollArmed = true;
        }
    }

    struct __kernel_timespec ts = MicrosToTimespec(ParkSafetyNetUs);
    struct io_uring_cqe* cqe = nullptr;
    io_uring_submit_and_wait_timeout(Ring.get(), &cqe, 1, &ts, nullptr);

    Parked.store(false, std::memory_order_seq_cst);
}

void TUringRouter::HandleStop() {
    struct io_uring* ring = Ring.get();

    // Flush anything already popped from the queue but not yet submitted.
    // io_uring_submit() alone normally frees local SQE slots; the
    // reap-and-block fallback only matters in the pathological case where
    // the kernel itself is refusing to accept more submissions.
    while (PendingSubmit) {
        io_uring_submit(ring);
        struct io_uring_sqe* sqe = GetSqe();
        if (sqe) {
            PrepareSqe(sqe, PendingSubmit);
            PendingSubmit = nullptr;
            break;
        }
        if (ReapCompletions() == 0) {
            struct io_uring_cqe* cqe = nullptr;
            io_uring_wait_cqe(ring, &cqe);
        }
    }

    // If a park's safety-net timeout fired instead of an actual eventfd
    // wakeup, the IORING_OP_POLL_ADD armed on WakeEventFd is still pending
    // in the ring (WakePollArmed stays true). IOSQE_IO_DRAIN below waits
    // for every previously submitted SQE to complete, and nothing will
    // ever write to WakeEventFd again once we're stopping -- so force the
    // poll to fire now and reap it, or the drain barrier would wait forever.
    if (WakePollArmed) {
        ui64 one = 1;
        write(WakeEventFd, &one, sizeof(one));
        while (WakePollArmed) {
            if (ReapCompletions() == 0) {
                struct io_uring_cqe* cqe = nullptr;
                io_uring_wait_cqe(ring, &cqe);
            }
        }
    }

    // Append an IO_DRAIN NOP barrier: it only completes after every
    // previously submitted SQE has completed, so once we see its CQE we
    // know every real op already reached OnComplete().
    struct io_uring_sqe* sqe;
    for (;;) {
        sqe = GetSqe();
        if (sqe) {
            break;
        }
        io_uring_submit(ring);
        if (ReapCompletions() == 0) {
            struct io_uring_cqe* cqe = nullptr;
            io_uring_wait_cqe(ring, &cqe);
        }
    }
    io_uring_prep_nop(sqe);
    sqe->flags |= IOSQE_IO_DRAIN;
    io_uring_sqe_set_data(sqe, &StopCqeMarker);
    io_uring_submit(ring);

    SawStopCqeMarker = false;
    while (!SawStopCqeMarker) {
        if (ReapCompletions() == 0) {
            struct io_uring_cqe* cqe = nullptr;
            io_uring_wait_cqe(ring, &cqe);
        }
    }

    // Defensive: anything found after the sentinel violates the Stop()
    // contract (Submit() racing with Stop()); drop it via OnDrop() instead
    // of leaking it. Submit() already bumped InFlightCount, so pair that
    // with a decrement here the same way OnComplete does.
    for (;;) {
        TUringOperationBase* op = Queue.Pop();
        if (!op || op == QueueStopSentinel()) {
            break;
        }
        op->OnDrop();
        InFlightCount.fetch_sub(1, std::memory_order_release);
    }
}

void TUringRouter::Stop() {
    if (!Ring) {
        return; // Already stopped
    }

    if (IoThread) {
        Queue.Push(QueueStopSentinel());
        WakeIoThreadIfParked();

        IoThread->Join();
        IoThread.reset();
    }

    io_uring_queue_exit(Ring.get());
    Ring.reset();
}

bool TUringRouter::IsFileRegistered() const {
    return FixedFdIndex >= 0;
}

bool TUringRouter::AreBuffersRegistered() const {
    return BuffersRegistered;
}

int TUringRouter::GetRegisterFileErrno() const {
    return RegisterFileErrno;
}

int TUringRouter::GetRegisterBuffersErrno() const {
    return RegisterBuffersErrno;
}

EUringFavor TUringRouter::GetUringFavor() const {
    return UsedModernFlags ? EUringFavor::SingleIssuer : EUringFavor::Plain;
}

ui32 TUringRouter::GetInflight() const {
    return InFlightCount.load(std::memory_order_relaxed);
}

bool TUringRouter::Probe(TUringRouterConfig config) {
    struct io_uring ring;
    bool usedModernFlags = false;
    int ret = InitRingWithFallback(&ring, config.QueueDepth, &usedModernFlags);
    if (ret != 0) {
        return false;
    }
    ret = io_uring_enable_rings(&ring);
    io_uring_queue_exit(&ring);
    return ret == 0;
}

TString TUringRouterConfig::ToString() const {
    return TStringBuilder()
        << "QueueDepth=" << QueueDepth
        << " IdleSpinUs=" << IdleSpinUs;
}

} // namespace NKikimr::NPDisk
