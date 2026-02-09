#include <ydb/library/pdisk_io/uring_router.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/system/tempfile.h>
#include <util/system/file.h>
#include <util/system/event.h>

#include <cstring>
#include <unistd.h>
#include <sys/uio.h>

using namespace NKikimr::NPDisk;

namespace {

TUringRouterConfig TestConfig(ui32 queueDepth = 16) {
    return TUringRouterConfig{
        .QueueDepth = queueDepth,
        .SqThreadIdleMs = 100,
        .UseSQPoll = false,
        .UseIOPoll = false,
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
};

// Completion op that signals a TManualEvent
struct TTestOp : TUringOperation {
    TManualEvent* Event = nullptr;

    static void SignalComplete(TUringOperation* op, TActorSystem*) {
        static_cast<TTestOp*>(op)->Event->Signal();
    }
};

// Completion op that increments an atomic counter and signals when target reached
struct TCountingOp : TUringOperation {
    std::atomic<int>* Counter = nullptr;
    int Target = 0;
    TManualEvent* Event = nullptr;

    static void CountComplete(TUringOperation* op, TActorSystem*) {
        auto* self = static_cast<TCountingOp*>(op);
        int val = self->Counter->fetch_add(1, std::memory_order_relaxed) + 1;
        if (val >= self->Target) {
            self->Event->Signal();
        }
    }
};

} // anonymous namespace

#define SKIP_IF_NO_URING() \
    do { \
        if (!TUringRouter::Probe()) { \
            Cerr << "io_uring not available on this system, skipping test" << Endl; \
            return; \
        } \
    } while (false)

Y_UNIT_TEST_SUITE(TUringRouterTest) {

    Y_UNIT_TEST(CreateAndDestroy) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20); // 1 MB
        TUringRouter router(f.GetHandle(), nullptr, TestConfig());
        router.Stop();
    }

    Y_UNIT_TEST(WriteAndReadBack) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20);
        TUringRouter router(f.GetHandle(), nullptr, TestConfig());

        constexpr ui32 size = 4096;

        // Write
        TAlignedBuf writeBuf(size);
        memset(writeBuf.Data(), 0xAB, size);

        TManualEvent writeEv;
        TTestOp writeOp;
        writeOp.OnComplete = TTestOp::SignalComplete;
        writeOp.Event = &writeEv;

        UNIT_ASSERT(router.Write(writeBuf.Data(), size, 0, &writeOp));
        router.Flush();
        writeEv.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(writeOp.Result, (i32)size);

        // Read back
        TAlignedBuf readBuf(size);
        memset(readBuf.Data(), 0, size);

        TManualEvent readEv;
        TTestOp readOp;
        readOp.OnComplete = TTestOp::SignalComplete;
        readOp.Event = &readEv;

        UNIT_ASSERT(router.Read(readBuf.Data(), size, 0, &readOp));
        router.Flush();
        readEv.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(readOp.Result, (i32)size);
        UNIT_ASSERT(memcmp(writeBuf.Data(), readBuf.Data(), size) == 0);

        router.Stop();
    }

    Y_UNIT_TEST(MultipleConcurrentOps) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20);
        TUringRouter router(f.GetHandle(), nullptr, TestConfig());

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
                ops[i].OnComplete = TCountingOp::CountComplete;
                ops[i].Counter = &counter;
                ops[i].Target = N;
                ops[i].Event = &allDone;

                UNIT_ASSERT(router.Write(writeBufs[i].Data(), size, i * size, &ops[i]));
            }
            router.Flush();
            allDone.WaitI();

            for (int i = 0; i < N; ++i) {
                UNIT_ASSERT_VALUES_EQUAL(ops[i].Result, (i32)size);
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
                ops[i].OnComplete = TCountingOp::CountComplete;
                ops[i].Counter = &counter;
                ops[i].Target = N;
                ops[i].Event = &allDone;

                UNIT_ASSERT(router.Read(readBufs[i].Data(), size, i * size, &ops[i]));
            }
            router.Flush();
            allDone.WaitI();

            for (int i = 0; i < N; ++i) {
                UNIT_ASSERT_VALUES_EQUAL(ops[i].Result, (i32)size);
                UNIT_ASSERT(memcmp(writeBufs[i].Data(), readBufs[i].Data(), size) == 0);
            }
        }

        router.Stop();
    }

    Y_UNIT_TEST(SubmitQueueFull) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20);

        // Very small queue
        TUringRouter router(f.GetHandle(), nullptr, TestConfig(/*queueDepth=*/4));

        constexpr ui32 size = 4096;
        TAlignedBuf buf(size);
        memset(buf.Data(), 0, size);

        TTestOp ops[5];
        TManualEvent events[5];
        int submitted = 0;

        for (int i = 0; i < 5; ++i) {
            ops[i].OnComplete = TTestOp::SignalComplete;
            ops[i].Event = &events[i];
            if (router.Write(buf.Data(), size, 0, &ops[i])) {
                ++submitted;
            }
        }

        // At least one should have been rejected (SQ ring size is 4)
        UNIT_ASSERT_LT(submitted, 5);
        UNIT_ASSERT_GE(submitted, 1);

        // Flush and wait for the submitted ones
        router.Flush();
        for (int i = 0; i < submitted; ++i) {
            events[i].WaitI();
        }

        router.Stop();
    }

    Y_UNIT_TEST(RegisterFile) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20);
        TUringRouter router(f.GetHandle(), nullptr, TestConfig());

        UNIT_ASSERT(router.RegisterFile());

        // Verify I/O still works after registration
        constexpr ui32 size = 4096;
        TAlignedBuf writeBuf(size);
        memset(writeBuf.Data(), 0xCD, size);

        TManualEvent ev;
        TTestOp op;
        op.OnComplete = TTestOp::SignalComplete;
        op.Event = &ev;

        UNIT_ASSERT(router.Write(writeBuf.Data(), size, 0, &op));
        router.Flush();
        ev.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(op.Result, (i32)size);

        router.Stop();
    }

    Y_UNIT_TEST(RegisterBuffersAndFixedIO) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20);
        TUringRouter router(f.GetHandle(), nullptr, TestConfig());

        constexpr ui32 size = 4096;
        TAlignedBuf writeBuf(size);
        TAlignedBuf readBuf(size);
        memset(writeBuf.Data(), 0xEF, size);
        memset(readBuf.Data(), 0, size);

        // Register both buffers
        struct iovec iovs[2];
        iovs[0].iov_base = writeBuf.Data();
        iovs[0].iov_len = size;
        iovs[1].iov_base = readBuf.Data();
        iovs[1].iov_len = size;
        UNIT_ASSERT(router.RegisterBuffers(iovs, 2));

        // WriteFixed using buffer index 0
        TManualEvent writeEv;
        TTestOp writeOp;
        writeOp.OnComplete = TTestOp::SignalComplete;
        writeOp.Event = &writeEv;

        UNIT_ASSERT(router.WriteFixed(writeBuf.Data(), size, 0, /*bufIndex=*/0, &writeOp));
        router.Flush();
        writeEv.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(writeOp.Result, (i32)size);

        // ReadFixed using buffer index 1
        TManualEvent readEv;
        TTestOp readOp;
        readOp.OnComplete = TTestOp::SignalComplete;
        readOp.Event = &readEv;

        UNIT_ASSERT(router.ReadFixed(readBuf.Data(), size, 0, /*bufIndex=*/1, &readOp));
        router.Flush();
        readEv.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(readOp.Result, (i32)size);
        UNIT_ASSERT(memcmp(writeBuf.Data(), readBuf.Data(), size) == 0);

        router.Stop();
    }

    Y_UNIT_TEST(SubmitItemsLeft) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20);

        constexpr ui32 queueDepth = 8;
        TUringRouter router(f.GetHandle(), nullptr, TestConfig(queueDepth));

        // Initially all slots should be available
        UNIT_ASSERT_VALUES_EQUAL(router.SubmitItemsLeft(), queueDepth);

        // Submit a few ops and check the count decreases
        constexpr ui32 size = 4096;
        TAlignedBuf buf(size);
        memset(buf.Data(), 0, size);

        constexpr int N = 3;
        TTestOp ops[N];
        TManualEvent events[N];
        for (int i = 0; i < N; ++i) {
            ops[i].OnComplete = TTestOp::SignalComplete;
            ops[i].Event = &events[i];
            UNIT_ASSERT(router.Write(buf.Data(), size, 0, &ops[i]));
        }

        UNIT_ASSERT_VALUES_EQUAL(router.SubmitItemsLeft(), queueDepth - N);

        // After flush + completion, slots should be reclaimed
        router.Flush();
        for (int i = 0; i < N; ++i) {
            events[i].WaitI();
        }
        // After completions are consumed by the poller, SQ slots are available again
        // (small sleep to let the poller advance the CQ head)
        usleep(1000);
        UNIT_ASSERT_VALUES_EQUAL(router.SubmitItemsLeft(), queueDepth);

        router.Stop();
    }

    Y_UNIT_TEST(LargeMultiPageIO) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        constexpr ui32 size = 256 * 1024; // 256 KB
        f.Resize(size);
        TUringRouter router(f.GetHandle(), nullptr, TestConfig());

        // Write 256K of a pattern
        TAlignedBuf writeBuf(size);
        for (ui32 i = 0; i < size; ++i) {
            static_cast<ui8*>(writeBuf.Data())[i] = (ui8)(i % 251); // prime modulus for pattern
        }

        TManualEvent writeEv;
        TTestOp writeOp;
        writeOp.OnComplete = TTestOp::SignalComplete;
        writeOp.Event = &writeEv;

        UNIT_ASSERT(router.Write(writeBuf.Data(), size, 0, &writeOp));
        router.Flush();
        writeEv.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(writeOp.Result, (i32)size);

        // Read it back
        TAlignedBuf readBuf(size);
        memset(readBuf.Data(), 0, size);

        TManualEvent readEv;
        TTestOp readOp;
        readOp.OnComplete = TTestOp::SignalComplete;
        readOp.Event = &readEv;

        UNIT_ASSERT(router.Read(readBuf.Data(), size, 0, &readOp));
        router.Flush();
        readEv.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(readOp.Result, (i32)size);
        UNIT_ASSERT(memcmp(writeBuf.Data(), readBuf.Data(), size) == 0);

        router.Stop();
    }

    Y_UNIT_TEST(NonZeroOffsets) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20);
        TUringRouter router(f.GetHandle(), nullptr, TestConfig());

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
            op.OnComplete = TTestOp::SignalComplete;
            op.Event = &ev;

            UNIT_ASSERT(router.Write(writeBufs[i].Data(), size, offsets[i], &op));
            router.Flush();
            ev.WaitI();
            UNIT_ASSERT_VALUES_EQUAL(op.Result, (i32)size);
        }

        // Read back each offset and verify
        for (int i = 0; i < N; ++i) {
            TAlignedBuf readBuf(size);
            memset(readBuf.Data(), 0, size);

            TManualEvent ev;
            TTestOp op;
            op.OnComplete = TTestOp::SignalComplete;
            op.Event = &ev;

            UNIT_ASSERT(router.Read(readBuf.Data(), size, offsets[i], &op));
            router.Flush();
            ev.WaitI();
            UNIT_ASSERT_VALUES_EQUAL(op.Result, (i32)size);
            UNIT_ASSERT(memcmp(writeBufs[i].Data(), readBuf.Data(), size) == 0);
        }

        router.Stop();
    }

    Y_UNIT_TEST(DoubleStop) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20);
        TUringRouter router(f.GetHandle(), nullptr, TestConfig());

        // Explicit stop, then destructor calls Stop() again -- must not crash
        router.Stop();
        router.Stop();
        // Destructor will call Stop() a third time
    }

    Y_UNIT_TEST(FlushWithNothingPending) {
        SKIP_IF_NO_URING();
        TTempFile tmp("uring_test_XXXXXX");
        TFile f(tmp.Name(), CreateAlways | RdWr);
        f.Resize(1 << 20);
        TUringRouter router(f.GetHandle(), nullptr, TestConfig());

        // Flush on an empty ring must not crash or hang
        router.Flush();
        router.Flush();

        // Verify I/O still works after empty flushes
        constexpr ui32 size = 4096;
        TAlignedBuf buf(size);
        memset(buf.Data(), 0x42, size);

        TManualEvent ev;
        TTestOp op;
        op.OnComplete = TTestOp::SignalComplete;
        op.Event = &ev;

        UNIT_ASSERT(router.Write(buf.Data(), size, 0, &op));
        router.Flush();
        ev.WaitI();
        UNIT_ASSERT_VALUES_EQUAL(op.Result, (i32)size);

        router.Stop();
    }
}
