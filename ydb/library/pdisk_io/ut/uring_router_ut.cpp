#include <ydb/library/actors/core/actorsystem.h>
#include <ydb/library/pdisk_io/uring_router.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/system/tempfile.h>
#include <util/system/file.h>
#include <util/system/event.h>
#include <util/thread/pool.h>

#include <sys/uio.h>

#include <unistd.h>

#include <atomic>
#include <cstring>
#include <vector>

using NActors::TActorSystem;
using namespace NKikimr::NPDisk;

namespace {

TUringRouterConfig DefaultConfig(ui32 queueDepth = 16) {
    return TUringRouterConfig{
        .QueueDepth = queueDepth,
        .IdleSpinUs = 100,
    };
}

// Simple RAII page-aligned buffer for tests
struct TAlignedBuf {
    void* Ptr = nullptr;
    size_t Size = 0;

    explicit TAlignedBuf(size_t size)
        : Size(size)
    {
        int ret = posix_memalign(&Ptr, 4096, size);
        Y_ABORT_UNLESS(ret == 0 && Ptr);
    }

    ~TAlignedBuf() {
        free(Ptr);
    }

    void* Data() { return Ptr; }
    const void* Data() const { return Ptr; }

    TAlignedBuf(const TAlignedBuf&) = delete;
    TAlignedBuf& operator=(const TAlignedBuf&) = delete;

    TAlignedBuf(TAlignedBuf&& other) noexcept
        : Ptr(other.Ptr)
        , Size(other.Size)
    {
        other.Ptr = nullptr;
        other.Size = 0;
    }

    TAlignedBuf& operator=(TAlignedBuf&& other) noexcept {
        if (this != &other) {
            free(Ptr);
            Ptr = other.Ptr;
            Size = other.Size;
            other.Ptr = nullptr;
            other.Size = 0;
        }
        return *this;
    }
};

// Completion op that signals a TManualEvent
struct TTestOp : TUringOperationBase {
    TManualEvent* Event = nullptr;

    void OnComplete(TActorSystem*) noexcept override {
        if (Event) {
            Event->Signal();
        }
    }

    void OnDrop() noexcept override {
        if (Event) {
            Event->Signal();
        }
    }
};

// Completion op that increments an atomic counter and signals when target reached
struct TCountingOp : TUringOperationBase {
    std::atomic<int>* Counter = nullptr;
    int Target = 0;
    TManualEvent* Event = nullptr;

    void CountAndMaybeSignal() noexcept {
        Y_ABORT_UNLESS(Counter);
        int val = Counter->fetch_add(1, std::memory_order_relaxed) + 1;
        if (Event && val >= Target) {
            Event->Signal();
        }
    }

    void OnComplete(TActorSystem*) noexcept override {
        CountAndMaybeSignal();
    }

    void OnDrop() noexcept override {
        CountAndMaybeSignal();
    }
};

#define SKIP_IF_NO_URING(config) \
    do { \
        if (!TUringRouter::Probe(config)) { \
            Cerr << "io_uring not available on this system, skipping test" << Endl; \
            return; \
        } \
    } while (false)

void PrepareWriteOp(TUringOperationBase& op, void* buf, ui32 size, ui64 offset) {
    op.SetOperationType(TUringOperationBase::EWRITE);
    op.PrepareIov(buf, size, offset);
}

void PrepareReadOp(TUringOperationBase& op, void* buf, ui32 size, ui64 offset) {
    op.SetOperationType(TUringOperationBase::EREAD);
    op.PrepareIov(buf, size, offset);
}

void DoCreateAndDestroy(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20); // 1 MB
    TUringRouter router(f.GetHandle(), nullptr, config);
    router.Start();
    router.Stop();
}

void DoWriteAndReadBack(TUringRouterConfig config, bool registerFile = true) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);
    TUringRouter router(f.GetHandle(), nullptr, config);
    if (registerFile) {
        router.RegisterFile();
    }
    router.Start();
    if (registerFile) {
        UNIT_ASSERT(router.IsFileRegistered());
    }

    constexpr ui32 size = 4096;

    // Write
    TAlignedBuf writeBuf(size);
    memset(writeBuf.Data(), 0xAB, size);

    TManualEvent writeEv;
    TTestOp writeOp;
    writeOp.Event = &writeEv;

    PrepareWriteOp(writeOp, writeBuf.Data(), size, 0);
    UNIT_ASSERT(router.Write(&writeOp));
    writeEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(writeOp.GetResult(), (i32)size);

    // Read back
    TAlignedBuf readBuf(size);
    memset(readBuf.Data(), 0, size);

    TManualEvent readEv;
    TTestOp readOp;
    readOp.Event = &readEv;

    PrepareReadOp(readOp, readBuf.Data(), size, 0);
    UNIT_ASSERT(router.Read(&readOp));
    readEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(readOp.GetResult(), (i32)size);
    UNIT_ASSERT(memcmp(writeBuf.Data(), readBuf.Data(), size) == 0);

    router.Stop();
}

void DoMultipleConcurrentOps(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);
    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    constexpr int N = 8;
    constexpr ui32 size = 4096;

    // Write N buffers with unique patterns
    TAlignedBuf writeBufs[N] = {
        TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size),
        TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size),
    };

    {
        std::atomic<int> counter{0};
        TManualEvent allDone;
        TCountingOp ops[N];
        for (int i = 0; i < N; ++i) {
            memset(writeBufs[i].Data(), (ui8)(i + 1), size);
            ops[i].Counter = &counter;
            ops[i].Target = N;
            ops[i].Event = &allDone;

            PrepareWriteOp(ops[i], writeBufs[i].Data(), size, i * size);
            UNIT_ASSERT(router.Write(&ops[i]));
        }
        allDone.WaitI();

        for (int i = 0; i < N; ++i) {
            UNIT_ASSERT_VALUES_EQUAL(ops[i].GetResult(), (i32)size);
        }
    }

    // Read back each buffer and verify contents
    {
        TAlignedBuf readBufs[N] = {
            TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size),
            TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size),
        };

        std::atomic<int> counter{0};
        TManualEvent allDone;
        TCountingOp ops[N];
        for (int i = 0; i < N; ++i) {
            memset(readBufs[i].Data(), 0, size);
            ops[i].Counter = &counter;
            ops[i].Target = N;
            ops[i].Event = &allDone;

            PrepareReadOp(ops[i], readBufs[i].Data(), size, i * size);
            UNIT_ASSERT(router.Read(&ops[i]));
        }
        allDone.WaitI();

        for (int i = 0; i < N; ++i) {
            UNIT_ASSERT_VALUES_EQUAL(ops[i].GetResult(), (i32)size);
            UNIT_ASSERT(memcmp(writeBufs[i].Data(), readBufs[i].Data(), size) == 0);
        }
    }

    router.Stop();
}

// Submits far more ops than QueueDepth without ever waiting in between.
// Submit() must never fail/block -- the router absorbs the overflow in its
// own submit queue and feeds the ring at its own pace.
void DoOverloadBeyondQueueDepth(TUringRouterConfig config) {
    config.QueueDepth = 4; // Very small ring, test-specific
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    constexpr ui32 size = 4096;
    TAlignedBuf buf(size);
    memset(buf.Data(), 0, size);

    constexpr int N = 64; // much larger than QueueDepth
    std::atomic<int> counter{0};
    TManualEvent allDone;
    std::vector<TCountingOp> ops(N);
    for (int i = 0; i < N; ++i) {
        ops[i].Counter = &counter;
        ops[i].Target = N;
        ops[i].Event = &allDone;
        PrepareWriteOp(ops[i], buf.Data(), size, 0);
        UNIT_ASSERT(router.Write(&ops[i]));
    }

    UNIT_ASSERT(allDone.WaitT(TDuration::Seconds(10)));

    router.Stop();
}

void DoRegisterBuffersAndFixedIO(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);
    TUringRouter router(f.GetHandle(), nullptr, config);

    constexpr ui32 size = 4096;
    TAlignedBuf writeBuf(size);
    TAlignedBuf readBuf(size);
    memset(writeBuf.Data(), 0xEF, size);
    memset(readBuf.Data(), 0, size);

    // Register file and buffers before Start(); the actual io_uring_register*
    // calls happen on the I/O thread as part of the Start() handshake.
    router.RegisterFile();

    struct iovec iovs[2];
    iovs[0].iov_base = writeBuf.Data();
    iovs[0].iov_len = size;
    iovs[1].iov_base = readBuf.Data();
    iovs[1].iov_len = size;
    router.RegisterBuffers(iovs, 2);

    router.Start();

    UNIT_ASSERT(router.IsFileRegistered());
    UNIT_ASSERT(router.AreBuffersRegistered());

    // WriteFixed using buffer index 0
    TManualEvent writeEv;
    TTestOp writeOp;
    writeOp.Event = &writeEv;

    UNIT_ASSERT(router.WriteFixed(writeBuf.Data(), size, 0, /*bufIndex=*/0, &writeOp));
    writeEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(writeOp.GetResult(), (i32)size);

    // ReadFixed using buffer index 1
    TManualEvent readEv;
    TTestOp readOp;
    readOp.Event = &readEv;

    UNIT_ASSERT(router.ReadFixed(readBuf.Data(), size, 0, /*bufIndex=*/1, &readOp));
    readEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(readOp.GetResult(), (i32)size);
    UNIT_ASSERT(memcmp(writeBuf.Data(), readBuf.Data(), size) == 0);

    router.Stop();
}

void DoInflightTracking(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    UNIT_ASSERT_VALUES_EQUAL(router.GetInflight(), 0u);

    constexpr ui32 size = 4096;
    TAlignedBuf buf(size);
    memset(buf.Data(), 0, size);

    constexpr int N = 5;
    TTestOp ops[N];
    TManualEvent events[N];
    for (int i = 0; i < N; ++i) {
        ops[i].Event = &events[i];
        PrepareWriteOp(ops[i], buf.Data(), size, 0);
        UNIT_ASSERT(router.Write(&ops[i]));
    }

    // GetInflight() is an approximate/eventually-consistent counter, but it
    // must never report fewer than "not yet completed" ops.
    for (int i = 0; i < N; ++i) {
        events[i].WaitI();
    }
    for (int i = 0; i < 1000 && router.GetInflight() != 0; ++i) {
        usleep(1000);
    }
    UNIT_ASSERT_VALUES_EQUAL(router.GetInflight(), 0u);

    router.Stop();
}

void DoLargeMultiPageIO(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    constexpr ui32 size = 256 * 1024; // 256 KB
    f.Resize(size);
    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    // Write 256K of a pattern
    TAlignedBuf writeBuf(size);
    for (ui32 i = 0; i < size; ++i) {
        static_cast<ui8*>(writeBuf.Data())[i] = (ui8)(i % 251); // prime modulus for pattern
    }

    TManualEvent writeEv;
    TTestOp writeOp;
    writeOp.Event = &writeEv;

    PrepareWriteOp(writeOp, writeBuf.Data(), size, 0);
    UNIT_ASSERT(router.Write(&writeOp));
    writeEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(writeOp.GetResult(), (i32)size);

    // Read it back
    TAlignedBuf readBuf(size);
    memset(readBuf.Data(), 0, size);

    TManualEvent readEv;
    TTestOp readOp;
    readOp.Event = &readEv;

    PrepareReadOp(readOp, readBuf.Data(), size, 0);
    UNIT_ASSERT(router.Read(&readOp));
    readEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(readOp.GetResult(), (i32)size);
    UNIT_ASSERT(memcmp(writeBuf.Data(), readBuf.Data(), size) == 0);

    router.Stop();
}

void DoNonZeroOffsets(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);
    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    constexpr ui32 size = 4096;

    // Write different patterns at offsets 0, 4K, 64K, 512K
    const ui64 offsets[] = {0, 4096, 65536, 524288};
    constexpr int N = 4;

    TAlignedBuf writeBufs[N] = {
        TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size), TAlignedBuf(size),
    };

    for (int i = 0; i < N; ++i) {
        memset(writeBufs[i].Data(), (ui8)(0xA0 + i), size);

        TManualEvent ev;
        TTestOp op;
        op.Event = &ev;

        PrepareWriteOp(op, writeBufs[i].Data(), size, offsets[i]);
        UNIT_ASSERT(router.Write(&op));
        ev.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(op.GetResult(), (i32)size);
    }

    // Read back each offset and verify
    for (int i = 0; i < N; ++i) {
        TAlignedBuf readBuf(size);
        memset(readBuf.Data(), 0, size);

        TManualEvent ev;
        TTestOp op;
        op.Event = &ev;

        PrepareReadOp(op, readBuf.Data(), size, offsets[i]);
        UNIT_ASSERT(router.Read(&op));
        ev.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(op.GetResult(), (i32)size);
        UNIT_ASSERT(memcmp(writeBufs[i].Data(), readBuf.Data(), size) == 0);
    }

    router.Stop();
}

void DoDoubleStop(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);
    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    // Explicit stop, then destructor calls Stop() again -- must not crash
    router.Stop();
    router.Stop();
    // Destructor will call Stop() a third time
}

void DoErrorResultPropagation(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    // Create a small file (4K) so that I/O at a large offset fails
    constexpr ui32 fileSize = 4096;
    f.Resize(fileSize);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    constexpr ui32 ioSize = 4096;
    TAlignedBuf buf(ioSize);
    memset(buf.Data(), 0xCC, ioSize);

    // Write at a huge offset -- the kernel should return an error (e.g. -EFBIG or
    // short write).  We just verify that op.Result is not the requested size,
    // demonstrating that errors propagate through the completion path.
    const ui64 badOffset = static_cast<ui64>(1) << 60;

    TManualEvent ev;
    TTestOp op;
    op.Event = &ev;

    PrepareWriteOp(op, buf.Data(), ioSize, badOffset);
    UNIT_ASSERT(router.Write(&op));
    ev.WaitI();
    // The kernel should have rejected this; Result should be negative errno
    UNIT_ASSERT_LT(op.GetResult(), 0);

    router.Stop();
}

void DoSubmitDirect(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);
    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    constexpr ui32 size = 4096;
    TAlignedBuf buf(size);
    memset(buf.Data(), 0x42, size);

    TManualEvent ev;
    TTestOp op;
    op.Event = &ev;
    PrepareWriteOp(op, buf.Data(), size, 0);
    // Exercise Submit() itself (Read/Write wrappers also call it, but this
    // is the public fire-and-forget entry point for multi-caller use).
    router.Submit(&op);
    UNIT_ASSERT(ev.WaitT(TDuration::Seconds(5)));
    UNIT_ASSERT_VALUES_EQUAL(op.GetResult(), (i32)size);

    router.Stop();
}

// Submits ops and immediately calls Stop() without waiting for completion.
// Use a ring smaller than N so at least some ops are still sitting in the
// submit queue (never having reached the kernel ring) at the moment Stop()
// runs -- Stop() must still drive every one of them through OnComplete().
void DoStopDrainsQueueBeforeCompletions(TUringRouterConfig config) {
    config.QueueDepth = 4;
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    constexpr ui32 size = 4096;
    TAlignedBuf buf(size);
    memset(buf.Data(), 0xDD, size);

    constexpr int N = 32; // well beyond QueueDepth
    TTestOp ops[N];
    TManualEvent events[N];
    for (int i = 0; i < N; ++i) {
        ops[i].Event = &events[i];
        PrepareWriteOp(ops[i], buf.Data(), size, 0);
        UNIT_ASSERT(router.Write(&ops[i]));
    }

    // Don't wait for anything -- just stop. Must not crash or deadlock, and
    // every single op (whether already in the ring or still queued) must be
    // driven to completion before Stop() returns.
    router.Stop();
    for (int i = 0; i < N; ++i) {
        UNIT_ASSERT(events[i].WaitT(TDuration::Seconds(5)));
    }
}

// Completion op that signals "entered" then blocks until "proceed" is signaled or times out
struct TBlockingOp : TUringOperationBase {
    TManualEvent* EnteredEvent = nullptr;
    TManualEvent* ProceedEvent = nullptr;

    void OnComplete(TActorSystem*) noexcept override {
        // Signal to the main thread that we've entered the callback
        if (EnteredEvent) {
            EnteredEvent->Signal();
        }
        // Block inside the callback until proceed is signaled or timeout (200 ms)
        if (ProceedEvent) {
            ProceedEvent->WaitT(TDuration::MilliSeconds(200));
        }
    }

    void OnDrop() noexcept override {
    }
};

void DoStopWhileCallbackRunning(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    constexpr ui32 size = 4096;
    TAlignedBuf buf(size);
    memset(buf.Data(), 0xFF, size);

    TManualEvent enteredEvent;
    TManualEvent proceedEvent;
    TBlockingOp op;
    op.EnteredEvent = &enteredEvent;
    op.ProceedEvent = &proceedEvent;

    PrepareWriteOp(op, buf.Data(), size, 0);
    UNIT_ASSERT(router.Write(&op));

    // Wait until the callback is actively running on the I/O thread
    enteredEvent.WaitI();

    // Now Stop() while the callback is still blocked inside OnComplete.
    // Stop() submits a drain stop marker and calls IoThread->Join(), which
    // blocks until the callback's WaitT times out and the I/O thread exits.
    // Must not crash or deadlock.
    router.Stop();
}

// Idles past IdleSpinUs so the I/O thread actually parks, then submits from
// the test thread and verifies the op completes promptly -- i.e. the
// eventfd-based wake protocol actually wakes a parked thread.
void DoParkThenWake(TUringRouterConfig config) {
    config.IdleSpinUs = 200; // small, so the thread reliably parks below
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(1 << 20);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    // Give the I/O thread plenty of time to go idle and park.
    usleep(20000);

    constexpr ui32 size = 4096;
    TAlignedBuf buf(size);
    memset(buf.Data(), 0x5A, size);

    TManualEvent ev;
    TTestOp op;
    op.Event = &ev;
    PrepareWriteOp(op, buf.Data(), size, 0);
    UNIT_ASSERT(router.Write(&op));

    // If the wake protocol is broken this will time out; a correct
    // implementation wakes within microseconds.
    UNIT_ASSERT(ev.WaitT(TDuration::Seconds(5)));
    UNIT_ASSERT_VALUES_EQUAL(op.GetResult(), (i32)size);

    router.Stop();
}

// Multiple threads submit concurrently to the same router/device, exercising
// the MPSC queue's multi-producer path.
void DoMultiProducerConcurrentSubmit(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    constexpr int NumThreads = 8;
    constexpr int OpsPerThread = 20;
    constexpr int N = NumThreads * OpsPerThread;
    constexpr ui32 size = 4096;
    f.Resize(N * size);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    std::vector<TTestOp> ops(N);
    std::vector<TManualEvent> events(N);
    std::vector<TAlignedBuf> bufs;
    bufs.reserve(N);
    for (int i = 0; i < N; ++i) {
        bufs.emplace_back(size);
        memset(bufs[i].Data(), (ui8)(i & 0xFF), size);
    }

    TThreadPool pool;
    pool.Start(NumThreads);
    std::atomic<int> nextSlot{0};
    for (int t = 0; t < NumThreads; ++t) {
        pool.SafeAddFunc([&]() {
            for (int j = 0; j < OpsPerThread; ++j) {
                int i = nextSlot.fetch_add(1, std::memory_order_relaxed);
                ops[i].Event = &events[i];
                PrepareWriteOp(ops[i], bufs[i].Data(), size, i * size);
                UNIT_ASSERT(router.Write(&ops[i]));
            }
        });
    }
    pool.Stop();

    for (int i = 0; i < N; ++i) {
        UNIT_ASSERT(events[i].WaitT(TDuration::Seconds(10)));
        UNIT_ASSERT_VALUES_EQUAL(ops[i].GetResult(), (i32)size);
    }

    router.Stop();
}

// Prepare a vectored write op from a pre-built iovec array.
void PrepareWriteVectored(TUringOperationBase& op, const struct iovec* iovs, int count, ui64 offset) {
    op.SetOperationType(TUringOperationBase::EWRITE);
    op.PrepareScatterGather(count, offset);
    for (int i = 0; i < count; ++i) {
        op.AddIov(iovs[i].iov_base, iovs[i].iov_len);
    }
}

// -------------------------------------------------------------------------
// Scatter-gather round-trip helpers
// -------------------------------------------------------------------------

// Write N 4K segments via one scatter-gather writev, read back into a single
// flat buffer, verify each segment.
void DoScatterGatherWriteReadBack(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    constexpr int N = 3;
    constexpr ui32 segSize = 4096;
    constexpr ui32 totalSize = N * segSize;
    f.Resize(totalSize);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    // Three distinct page-aligned write buffers
    TAlignedBuf wBufs[N] = {TAlignedBuf(segSize), TAlignedBuf(segSize), TAlignedBuf(segSize)};
    for (int i = 0; i < N; ++i) {
        memset(wBufs[i].Data(), (ui8)(0x11 * (i + 1)), segSize);
    }

    struct iovec iovs[N];
    for (int i = 0; i < N; ++i) {
        iovs[i].iov_base = wBufs[i].Data();
        iovs[i].iov_len  = segSize;
    }

    TManualEvent writeEv;
    TTestOp writeOp;
    writeOp.Event = &writeEv;
    PrepareWriteVectored(writeOp, iovs, N, /*offset=*/0);
    UNIT_ASSERT(router.Write(&writeOp));
    writeEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(writeOp.GetResult(), (i32)totalSize);

    // Read back into one flat buffer and verify per-segment patterns.
    TAlignedBuf readBuf(totalSize);
    memset(readBuf.Data(), 0, totalSize);

    TManualEvent readEv;
    TTestOp readOp;
    readOp.Event = &readEv;
    PrepareReadOp(readOp, readBuf.Data(), totalSize, 0);
    UNIT_ASSERT(router.Read(&readOp));
    readEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(readOp.GetResult(), (i32)totalSize);

    for (int i = 0; i < N; ++i) {
        UNIT_ASSERT(memcmp(wBufs[i].Data(),
                           static_cast<ui8*>(readBuf.Data()) + i * segSize,
                           segSize) == 0);
    }

    router.Stop();
}

void DoScatterGatherSingleIovec(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    constexpr ui32 size = 4096;
    f.Resize(size);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    TAlignedBuf writeBuf(size);
    memset(writeBuf.Data(), 0xBB, size);

    struct iovec iov;
    iov.iov_base = writeBuf.Data();
    iov.iov_len  = size;

    TManualEvent writeEv;
    TTestOp writeOp;
    writeOp.Event = &writeEv;
    PrepareWriteVectored(writeOp, &iov, 1, 0);
    UNIT_ASSERT(router.Write(&writeOp));
    writeEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(writeOp.GetResult(), (i32)size);

    TAlignedBuf readBuf(size);
    memset(readBuf.Data(), 0, size);

    TManualEvent readEv;
    TTestOp readOp;
    readOp.Event = &readEv;
    PrepareReadOp(readOp, readBuf.Data(), size, 0);
    UNIT_ASSERT(router.Read(&readOp));
    readEv.WaitI();
    UNIT_ASSERT_VALUES_EQUAL(readOp.GetResult(), (i32)size);
    UNIT_ASSERT(memcmp(writeBuf.Data(), readBuf.Data(), size) == 0);

    router.Stop();
}

void DoScatterGatherErrorPropagation(TUringRouterConfig config) {
    SKIP_IF_NO_URING(config);
    TTempFile tmp(MakeTempName(nullptr, "uring_test"));
    TFile f(tmp.Name(), CreateAlways | RdWr);
    f.Resize(4096);

    TUringRouter router(f.GetHandle(), nullptr, config);
    router.RegisterFile();
    router.Start();

    TAlignedBuf buf1(4096), buf2(4096);
    memset(buf1.Data(), 0xCC, 4096);
    memset(buf2.Data(), 0xCC, 4096);

    struct iovec iovs[2];
    iovs[0].iov_base = buf1.Data(); iovs[0].iov_len = 4096;
    iovs[1].iov_base = buf2.Data(); iovs[1].iov_len = 4096;

    const ui64 badOffset = static_cast<ui64>(1) << 60;

    TManualEvent ev;
    TTestOp op;
    op.Event = &ev;
    PrepareWriteVectored(op, iovs, 2, badOffset);
    UNIT_ASSERT(router.Write(&op));
    ev.WaitI();
    UNIT_ASSERT_LT(op.GetResult(), 0);

    router.Stop();
}

} // anonymous namespace

// =========================================================================
// Pure logic tests for TUringOperationBase (no kernel ring required)
// =========================================================================

Y_UNIT_TEST_SUITE(TUringOperationBaseTest) {

    Y_UNIT_TEST(PrepareIovSingleBuffer) {
        TTestOp op;
        char buf[4096];
        op.SetOperationType(TUringOperationBase::EWRITE);
        op.PrepareIov(buf, 4096, 1024);

        UNIT_ASSERT_VALUES_EQUAL(op.GetTotalSize(), 4096u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetOperationBytes(), 4096u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetDiskOffset(), 1024u);
        UNIT_ASSERT_EQUAL(op.GetIovBase(), static_cast<void*>(buf));
    }

#if defined(__linux__)
    Y_UNIT_TEST(PrepareIovVectored) {
        TTestOp op;
        char buf1[4096], buf2[4096], buf3[4096];
        struct iovec iovs[3];
        iovs[0] = {buf1, 4096};
        iovs[1] = {buf2, 4096};
        iovs[2] = {buf3, 4096};

        PrepareWriteVectored(op, iovs, 3, 8192);

        UNIT_ASSERT_VALUES_EQUAL(op.GetTotalSize(), 3 * 4096u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetOperationBytes(), 3 * 4096u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetDiskOffset(), 8192u);
        UNIT_ASSERT_EQUAL(op.GetIovBase(), static_cast<void*>(buf1));
    }

    Y_UNIT_TEST(AdvanceIovFullSegments) {
        TTestOp op;
        char buf1[4096], buf2[4096], buf3[4096];
        struct iovec iovs[3];
        iovs[0] = {buf1, 4096};
        iovs[1] = {buf2, 4096};
        iovs[2] = {buf3, 4096};

        PrepareWriteVectored(op, iovs, 3, 0);

        // Advance past the first full segment.
        op.AdvanceIov(4096);
        UNIT_ASSERT_VALUES_EQUAL(op.GetOperationBytes(), 2 * 4096u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetDiskOffset(), 4096u);
        UNIT_ASSERT_EQUAL(op.GetIovBase(), static_cast<void*>(buf2));

        // Advance past the second full segment.
        op.AdvanceIov(4096);
        UNIT_ASSERT_VALUES_EQUAL(op.GetOperationBytes(), 1 * 4096u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetDiskOffset(), 8192u);
        UNIT_ASSERT_EQUAL(op.GetIovBase(), static_cast<void*>(buf3));

        // TotalSize is unchanged throughout.
        UNIT_ASSERT_VALUES_EQUAL(op.GetTotalSize(), 3 * 4096u);
    }

    Y_UNIT_TEST(AdvanceIovPartialSegment) {
        TTestOp op;
        char buf1[4096], buf2[4096];
        struct iovec iovs[2];
        iovs[0] = {buf1, 4096};
        iovs[1] = {buf2, 4096};

        PrepareWriteVectored(op, iovs, 2, 0);

        // Partial advance within the first iovec.
        op.AdvanceIov(1024);
        UNIT_ASSERT_VALUES_EQUAL(op.GetOperationBytes(), 4096u + 3072u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetDiskOffset(), 1024u);
        // iov_base of the first remaining iovec should be advanced.
        UNIT_ASSERT_EQUAL(op.GetIovBase(), static_cast<void*>(buf1 + 1024));
    }

    Y_UNIT_TEST(AdvanceIovCrossSegmentBoundary) {
        TTestOp op;
        char buf1[4096], buf2[8192];
        struct iovec iovs[2];
        iovs[0] = {buf1, 4096};
        iovs[1] = {buf2, 8192};

        PrepareWriteVectored(op, iovs, 2, 0);

        // Advance exactly one full segment + 2048 into the next.
        op.AdvanceIov(4096 + 2048);
        UNIT_ASSERT_VALUES_EQUAL(op.GetOperationBytes(), 8192u - 2048u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetDiskOffset(), 4096u + 2048u);
        UNIT_ASSERT_EQUAL(op.GetIovBase(), static_cast<void*>(buf2 + 2048));
        UNIT_ASSERT_VALUES_EQUAL(op.GetTotalSize(), 4096u + 8192u);
    }

    Y_UNIT_TEST(ResetSubmissionStateClearsIov) {
        TTestOp op;
        char buf1[4096], buf2[4096];
        struct iovec iovs[2];
        iovs[0] = {buf1, 4096};
        iovs[1] = {buf2, 4096};

        PrepareWriteVectored(op, iovs, 2, 512);
        op.AdvanceIov(4096);

        op.ResetSubmissionState();
        UNIT_ASSERT_VALUES_EQUAL(op.GetTotalSize(), 0u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetOperationBytes(), 0u);
        UNIT_ASSERT_VALUES_EQUAL(op.GetDiskOffset(), 0u);
        UNIT_ASSERT_EQUAL(op.GetIovBase(), nullptr);
        UNIT_ASSERT(!op.IsFixedBuffer());
        UNIT_ASSERT_VALUES_EQUAL(op.GetBufIndex(), 0u);
    }
#endif // __linux__

}

Y_UNIT_TEST_SUITE(TUringRouterTest) {

    Y_UNIT_TEST(CreateAndDestroy) {
        DoCreateAndDestroy(DefaultConfig());
    }

    Y_UNIT_TEST(WriteAndReadBack) {
        DoWriteAndReadBack(DefaultConfig());
    }

    Y_UNIT_TEST(WriteAndReadBackNoFixedFile) {
        DoWriteAndReadBack(DefaultConfig(), /*registerFile=*/false);
    }

    Y_UNIT_TEST(MultipleConcurrentOps) {
        DoMultipleConcurrentOps(DefaultConfig());
    }

    Y_UNIT_TEST(OverloadBeyondQueueDepth) {
        DoOverloadBeyondQueueDepth(DefaultConfig());
    }

    Y_UNIT_TEST(RegisterBuffersAndFixedIO) {
        DoRegisterBuffersAndFixedIO(DefaultConfig());
    }

    Y_UNIT_TEST(InflightTracking) {
        DoInflightTracking(DefaultConfig());
    }

    Y_UNIT_TEST(LargeMultiPageIO) {
        DoLargeMultiPageIO(DefaultConfig());
    }

    Y_UNIT_TEST(NonZeroOffsets) {
        DoNonZeroOffsets(DefaultConfig());
    }

    Y_UNIT_TEST(DoubleStop) {
        DoDoubleStop(DefaultConfig());
    }

    Y_UNIT_TEST(ErrorResultPropagation) {
        DoErrorResultPropagation(DefaultConfig());
    }

    Y_UNIT_TEST(SubmitDirect) {
        DoSubmitDirect(DefaultConfig());
    }

    Y_UNIT_TEST(StopDrainsQueueBeforeCompletions) {
        DoStopDrainsQueueBeforeCompletions(DefaultConfig());
    }

    Y_UNIT_TEST(StopWhileCallbackRunning) {
        DoStopWhileCallbackRunning(DefaultConfig());
    }

    Y_UNIT_TEST(ParkThenWake) {
        DoParkThenWake(DefaultConfig());
    }

    Y_UNIT_TEST(MultiProducerConcurrentSubmit) {
        DoMultiProducerConcurrentSubmit(DefaultConfig());
    }

    Y_UNIT_TEST(ScatterGatherWriteReadBack) {
        DoScatterGatherWriteReadBack(DefaultConfig());
    }

    Y_UNIT_TEST(ScatterGatherSingleIovec) {
        DoScatterGatherSingleIovec(DefaultConfig());
    }

    Y_UNIT_TEST(ScatterGatherErrorPropagation) {
        DoScatterGatherErrorPropagation(DefaultConfig());
    }
}
