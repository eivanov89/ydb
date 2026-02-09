#include "uring_router.h"

#include <util/string/builder.h>
#include <util/system/yassert.h>

#include <unistd.h>

// liburing.h must be included AFTER YDB headers because <linux/fs.h> (pulled
// in by liburing) defines a BLOCK_SIZE macro that clashes with bitmap.h.
#include <liburing.h>

namespace NKikimr::NPDisk {

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TCompletionPoller
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class TUringRouter::TCompletionPoller : public ISimpleThread {
public:
    TCompletionPoller(TUringRouter& owner)
        : Owner(owner)
    {}

    void* ThreadProc() override {
        SetCurrentThreadName("UringCmpl");

        // With IOPOLL (and no SQPOLL), the kernel does not post CQEs via
        // interrupts.  We must call io_uring_enter(IORING_ENTER_GETEVENTS)
        // to trigger the kernel to poll the block device for completions.
        // With SQPOLL, the kernel SQPOLL thread handles reaping internally.
        const bool needIoPollReap = Owner.Config.UseIOPoll && !Owner.Config.UseSQPoll;

        while (Owner.Running.load(std::memory_order_acquire)) {
            // For IOPOLL without SQPOLL, ask the kernel to reap polled
            // completions before we peek the CQ ring.
            if (needIoPollReap) {
                io_uring_enter(Owner.Ring->ring_fd, 0, 0, IORING_ENTER_GETEVENTS, nullptr);
            }

            unsigned head;
            unsigned count = 0;
            struct io_uring_cqe* cqe;

            io_uring_for_each_cqe(Owner.Ring, head, cqe) {
                auto* op = reinterpret_cast<TUringOperation*>(io_uring_cqe_get_data(cqe));
                if (op) {
                    op->Result = cqe->res;
                    if (op->OnComplete) {
                        op->OnComplete(op, Owner.ActorSystem);
                    }
                }
                ++count;
            }

            if (count > 0) {
                io_uring_cq_advance(Owner.Ring, count);
            } else {
                // No completions available -- sleep briefly before retrying.
                // We deliberately avoid io_uring_wait_cqe_timeout here because
                // on kernels < 5.11 (lacking IORING_ENTER_EXT_ARG) it submits
                // a timeout SQE internally, racing with SQ submissions from
                // the main thread.
                usleep(100); // 100 us
            }
        }

        // Drain any remaining CQEs after stop (don't call OnComplete)
        unsigned head;
        unsigned count = 0;
        struct io_uring_cqe* cqe;
        io_uring_for_each_cqe(Owner.Ring, head, cqe) {
            ++count;
        }
        if (count > 0) {
            io_uring_cq_advance(Owner.Ring, count);
        }

        return nullptr;
    }

private:
    TUringRouter& Owner;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TUringRouter
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

TUringRouter::TUringRouter(FHANDLE fd, TActorSystem* actorSystem, TUringRouterConfig config)
    : Ring(new struct io_uring())
    , Fd(fd)
    , ActorSystem(actorSystem)
    , Config(config)
{
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    if (Config.UseSQPoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        params.sq_thread_idle = Config.SqThreadIdleMs;
    }
    if (Config.UseIOPoll) {
        params.flags |= IORING_SETUP_IOPOLL;
    }

    int ret = io_uring_queue_init_params(Config.QueueDepth, Ring, &params);
    Y_ABORT_UNLESS(ret == 0, "io_uring_queue_init_params failed: %s (errno %d)", strerror(-ret), -ret);

    Poller = MakeHolder<TCompletionPoller>(*this);
    Poller->Start();
}

TUringRouter::~TUringRouter() {
    Stop();
}

bool TUringRouter::RegisterFile() {
    int fd = Fd;
    int ret = io_uring_register_files(Ring, &fd, 1);
    if (ret == 0) {
        FixedFdIndex = 0;
        return true;
    }
    return false;
}

bool TUringRouter::RegisterBuffers(const struct iovec* iovs, unsigned count) {
    int ret = io_uring_register_buffers(Ring, iovs, count);
    if (ret == 0) {
        BuffersRegistered = true;
        return true;
    }
    return false;
}

struct io_uring_sqe* TUringRouter::GetSqe() {
    return io_uring_get_sqe(Ring);
}

void TUringRouter::PrepareSqe(struct io_uring_sqe* sqe, bool isRead, void* buf, ui32 size,
                               ui64 offset, TUringOperation* op) {
    // Use readv/writev (IORING_OP_READV/WRITEV) instead of read/write
    // (IORING_OP_READ/WRITE) for kernel 5.4 compatibility.
    // IORING_OP_READ/WRITE were added in 5.6; readv/writev exist since 5.1.
    op->Iov.iov_base = buf;
    op->Iov.iov_len = size;

    int fd = (FixedFdIndex >= 0) ? FixedFdIndex : Fd;
    if (isRead) {
        io_uring_prep_readv(sqe, fd, &op->Iov, 1, offset);
    } else {
        io_uring_prep_writev(sqe, fd, &op->Iov, 1, offset);
    }
    if (FixedFdIndex >= 0) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    io_uring_sqe_set_data(sqe, op);
}

bool TUringRouter::Read(void* buf, ui32 size, ui64 offset, TUringOperation* op) {
    struct io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        return false;
    }
    PrepareSqe(sqe, /*isRead=*/true, buf, size, offset, op);
    return true;
}

bool TUringRouter::Write(const void* buf, ui64 size, ui64 offset, TUringOperation* op) {
    struct io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        return false;
    }
    PrepareSqe(sqe, /*isRead=*/false, const_cast<void*>(buf), size, offset, op);
    return true;
}

bool TUringRouter::ReadFixed(void* buf, ui32 size, ui64 offset, ui16 bufIndex, TUringOperation* op) {
    Y_ABORT_UNLESS(BuffersRegistered, "RegisterBuffers must be called before ReadFixed");
    struct io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        return false;
    }
    int fd = (FixedFdIndex >= 0) ? FixedFdIndex : Fd;
    io_uring_prep_read_fixed(sqe, fd, buf, size, offset, bufIndex);
    if (FixedFdIndex >= 0) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    io_uring_sqe_set_data(sqe, op);
    return true;
}

bool TUringRouter::WriteFixed(const void* buf, ui64 size, ui64 offset, ui16 bufIndex, TUringOperation* op) {
    Y_ABORT_UNLESS(BuffersRegistered, "RegisterBuffers must be called before WriteFixed");
    struct io_uring_sqe* sqe = GetSqe();
    if (!sqe) {
        return false;
    }
    int fd = (FixedFdIndex >= 0) ? FixedFdIndex : Fd;
    io_uring_prep_write_fixed(sqe, fd, buf, size, offset, bufIndex);
    if (FixedFdIndex >= 0) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
    io_uring_sqe_set_data(sqe, op);
    return true;
}

void TUringRouter::Flush() {
    // Always call io_uring_submit().  It does two things:
    // 1. Flushes the SQ ring tail (__io_uring_flush_sq) so the kernel (or
    //    the SQPOLL thread) can see newly prepared SQEs.  Without this,
    //    io_uring_get_sqe() only updates an internal userspace counter;
    //    the kernel-visible *sq->ktail stays stale.
    // 2. Calls io_uring_enter() when needed: always for non-SQPOLL,
    //    and only for SQPOLL wakeup when IORING_SQ_NEED_WAKEUP is set.
    io_uring_submit(Ring);
}

void TUringRouter::Stop() {
    bool expected = true;
    if (!Running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return; // Already stopped
    }

    if (Poller) {
        Poller->Join();
        Poller.Reset();
    }

    io_uring_queue_exit(Ring);
    delete Ring;
    Ring = nullptr;
}

ui32 TUringRouter::SubmitItemsLeft() const {
    return io_uring_sq_space_left(Ring);
}

bool TUringRouter::Probe() {
    struct io_uring ring;
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    int ret = io_uring_queue_init_params(1, &ring, &params);
    if (ret == 0) {
        io_uring_queue_exit(&ring);
        return true;
    }
    return false;
}

} // namespace NKikimr::NPDisk
