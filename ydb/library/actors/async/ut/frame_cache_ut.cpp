#include "common.h"

#include <ydb/library/actors/async/wait_for_event.h>

#include <thread>

#if defined(__linux__)
#include <util/system/tempfile.h>
#include <util/stream/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace NAsyncTest {
namespace {

    struct TCaptureFrame {
        static constexpr bool IsActorAwareAwaiter = true;
        void*& Address;

        bool await_ready() const noexcept { return false; }
        bool await_suspend(std::coroutine_handle<> handle) const noexcept {
            Address = handle.address();
            return false;
        }
        void await_resume() const noexcept {}
    };

    struct TThrowOnMove {
        bool Fail = true;
        TThrowOnMove() = default;
        TThrowOnMove(const TThrowOnMove&) = default;
        Y_NO_INLINE TThrowOnMove(TThrowOnMove&& other)
            : Fail(other.Fail)
        {
            // Keep a runtime-dependent throwing move: an unconditionally
            // throwing constructor lets the compiler remove frame allocation.
            if (Fail) {
                throw TTestException();
            }
        }
    };

    struct TCacheActorState {
        void* Root = nullptr;
        void* Child = nullptr;
        std::coroutine_handle<> Bridge;
        size_t RootDestroyed = 0;
        size_t ChildDestroyed = 0;
        size_t Finished = 0;
        size_t Caught = 0;
        int Value = 0;
        TAsyncFrameCache::TStats FinalStats;
    };

    class TCacheActor : public TAsyncTestActor {
    public:
        TAsyncFrameCache Cache;
        TCacheActorState& Counts;
        const bool CacheEnabled;

        TCacheActor(TState& state, TCacheActorState& counts, bool enabled)
            : TAsyncTestActor(state)
            , Counts(counts)
            , CacheEnabled(enabled)
        {}

        ~TCacheActor() {
            Counts.FinalStats = Cache.GetStats();
        }

        TAsyncFrameCache* GetAsyncFrameCache() noexcept override {
            return CacheEnabled ? &Cache : nullptr;
        }

        void Root(bool generic = false, bool throws = false) {
            Y_DEFER { ++Counts.RootDestroyed; };
            co_await TCaptureFrame{Counts.Root};
            try {
                Counts.Value = co_await Child(generic, throws);
            } catch (const TTestException&) {
                ++Counts.Caught;
            }
            ++Counts.Finished;
        }

        Y_NO_INLINE async<int> Child(bool generic, bool throws) {
            Y_DEFER { ++Counts.ChildDestroyed; };
            co_await TCaptureFrame{Counts.Child};
            if (generic) {
                co_await TSuspendStdAwaiterWithoutCancel{&Counts.Bridge};
            } else {
                co_await ActorWaitForEvent<TEvents::TEvWakeup>(0);
            }
            if (throws) {
                throw TTestException();
            }
            co_return 42;
        }

        Y_NO_INLINE async<void> LazyVoid() { co_return; }
        Y_NO_INLINE async<int> LazyValue() { co_return 42; }
        Y_NO_INLINE async<int> ConstMember() const { co_return 42; }
        Y_NO_INLINE async<void> ThrowingParameter(TThrowOnMove value) {
            Y_UNUSED(value);
            co_return;
        }
        Y_NO_INLINE void ThrowingRootParameter(TThrowOnMove value) {
            Y_UNUSED(value);
            co_return;
        }
    };

    struct TFixture {
        TAsyncTestActor::TState State;
        TCacheActorState Counts;
        TAsyncTestActorRuntime Runtime;
        TCacheActor* Self;
        TAsyncTestActorRuntime::TAsyncActorOperations Actor;

        explicit TFixture(bool enabled = true)
            : Self(new TCacheActor(State, Counts, enabled))
            , Actor(Runtime, Runtime.Register(Self))
        {
            Actor.Step();
        }
    };

    Y_NO_INLINE async<int> FreeWithActor(IActor& actor) {
        Y_UNUSED(actor);
        co_return 1;
    }

    Y_NO_INLINE async<int> FreeWithoutActor() { co_return 1; }

} // namespace

Y_UNIT_TEST_SUITE(AsyncFrameCache) {
    Y_UNIT_TEST(ExactSizeLifoAndConcurrentAllocations) {
        TAsyncFrameCache cache;
        void* first = cache.Allocate(100);
        void* second = cache.Allocate(100);
        void* other = cache.Allocate(101);
        UNIT_ASSERT(first != second && first != other && second != other);
        TAsyncFrameCache::Free(first, 100);
        TAsyncFrameCache::Free(second, 100);
        TAsyncFrameCache::Free(other, 101);
        UNIT_ASSERT_VALUES_EQUAL(cache.Allocate(100), second);
        UNIT_ASSERT_VALUES_EQUAL(cache.Allocate(101), other);
        UNIT_ASSERT_VALUES_EQUAL(cache.Allocate(100), first);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().HeapAllocations, 3);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, 3);
        TAsyncFrameCache::Free(first, 100);
        TAsyncFrameCache::Free(second, 100);
        TAsyncFrameCache::Free(other, 101);
    }

    Y_UNIT_TEST(InterleavedClassesKeepLiveFramesReusable) {
        TAsyncFrameCache cache;
        std::array<std::array<void*, 2>, TAsyncFrameCache::MaxClasses> frames;
        // Keep frames alive while the class table grows to its limit.
        for (size_t i = 0; i < frames.size(); ++i) {
            frames[i][0] = cache.Allocate(i + 1);
        }
        for (size_t i = frames.size(); i-- > 0;) {
            frames[i][1] = cache.Allocate(i + 1);
            UNIT_ASSERT(frames[i][0] != frames[i][1]);
        }

        const size_t heapAllocations = frames.size() * 2;
        for (size_t cycle = 0; cycle < 3; ++cycle) {
            const size_t first = cycle % 2;
            const size_t second = 1 - first;
            // Free the older frame first and mix classes instead of following
            // the allocation order. Both headers must retain their own class.
            for (size_t step = 0; step < frames.size(); ++step) {
                const size_t i = step % 2 ? frames.size() - 1 - step / 2 : step / 2;
                TAsyncFrameCache::Free(frames[i][first], i + 1);
                TAsyncFrameCache::Free(frames[i][second], i + 1);
            }
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().SizeClasses, frames.size());
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, 0);
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().CachedFrames, heapAllocations);

            for (size_t i = frames.size(); i-- > 0;) {
                UNIT_ASSERT_VALUES_EQUAL(cache.Allocate(i + 1), frames[i][second]);
                UNIT_ASSERT_VALUES_EQUAL(cache.Allocate(i + 1), frames[i][first]);
            }
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, heapAllocations);
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().CachedFrames, 0);
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().HeapAllocations, heapAllocations);
        }

        for (size_t i = 0; i < frames.size(); ++i) {
            TAsyncFrameCache::Free(frames[i][0], i + 1);
            TAsyncFrameCache::Free(frames[i][1], i + 1);
        }
    }

    Y_UNIT_TEST(PerClassRetentionLimit) {
        TAsyncFrameCache cache;
        std::array<void*, TAsyncFrameCache::MaxCachedPerClass + 1> frames;
        for (auto& frame : frames) {
            frame = cache.Allocate(100);
        }
        for (void* frame : frames) {
            TAsyncFrameCache::Free(frame, 100);
        }
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, 0);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().CachedFrames, TAsyncFrameCache::MaxCachedPerClass);
        const auto allocations = cache.GetStats().HeapAllocations;
        for (auto& frame : frames) {
            frame = cache.Allocate(100);
        }
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().HeapAllocations, allocations + 1);
        for (void* frame : frames) {
            TAsyncFrameCache::Free(frame, 100);
        }
    }

    Y_UNIT_TEST(ClassLimitKeepsExistingClassesReusable) {
        TAsyncFrameCache cache;
        std::array<void*, TAsyncFrameCache::MaxClasses> frames;
        for (size_t i = 0; i < frames.size(); ++i) {
            frames[i] = cache.Allocate(i + 1);
            TAsyncFrameCache::Free(frames[i], i + 1);
        }
        for (size_t i = 0; i < 2; ++i) {
            void* overflow = cache.Allocate(1000);
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, 0);
            TAsyncFrameCache::Free(overflow, 1000);
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().CachedFrames, frames.size());
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().SizeClasses, frames.size());
        }
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().HeapAllocations, frames.size() + 2);
        for (size_t i = 0; i < frames.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL(cache.Allocate(i + 1), frames[i]);
            TAsyncFrameCache::Free(frames[i], i + 1);
        }
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().HeapAllocations, frames.size() + 2);
    }

    Y_UNIT_TEST(ClassOverflowPreservesLiveCachedFrameAccounting) {
        TAsyncFrameCache cache;
        for (size_t size = 1; size <= TAsyncFrameCache::MaxClasses; ++size) {
            auto* frame = cache.Allocate(size);
            TAsyncFrameCache::Free(frame, size);
        }
        auto* retained = cache.Allocate(1);
        const auto before = cache.GetStats();
        UNIT_ASSERT_VALUES_EQUAL(before.LiveFrames, 1);
        auto* overflow = cache.Allocate(TAsyncFrameCache::MaxClasses + 1);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, 1);
        TAsyncFrameCache::Free(overflow, TAsyncFrameCache::MaxClasses + 1);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, 1);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().CachedFrames, before.CachedFrames);
        TAsyncFrameCache::Free(retained, 1);
        UNIT_ASSERT_VALUES_EQUAL(cache.Allocate(1), retained);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().HeapAllocations, before.HeapAllocations + 1);
        TAsyncFrameCache::Free(retained, 1);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, 0);
    }

#if defined(__linux__)
    Y_UNIT_TEST(LazyFrameOutlivingOwnerAbortsWithDiagnostic) {
        TTempFile diagnostic(MakeTempName(nullptr, "lazy_frame_owner"));
        const pid_t pid = fork();
        UNIT_ASSERT(pid >= 0);
        if (!pid) {
            const rlimit noCore{0, 0};
            setrlimit(RLIMIT_CORE, &noCore);
            TFile output(diagnostic.Name(), CreateAlways | WrOnly);
            Y_ABORT_UNLESS(dup2(output.GetHandle(), STDERR_FILENO) >= 0);
            TFixture f;
            // Direct initialization is required: async<T> is not movable.
            auto lazy = f.Self->LazyValue();
            Y_ABORT_UNLESS(lazy.GetHandle());
            Y_ABORT_UNLESS(f.Self->Cache.GetStats().LiveFrames == 1);
            f.Runtime.CleanupNode();
            _exit(1);
        }
        int status = 0;
        UNIT_ASSERT_VALUES_EQUAL(waitpid(pid, &status, 0), pid);
        UNIT_ASSERT(WIFSIGNALED(status));
        UNIT_ASSERT_VALUES_EQUAL(WTERMSIG(status), SIGABRT);
        UNIT_ASSERT_STRING_CONTAINS(TFileInput(diagnostic.Name()).ReadAll(),
            "Coroutine frame cache destroyed with live frames");
    }
#endif

    Y_UNIT_TEST(ExplicitOwnerWinsOverCurrentActorActivation) {
        TFixture a;
        TFixture b;
        b.Actor.RunSync([&] {
            const auto beforeA = a.Self->Cache.GetStats();
            const auto beforeB = b.Self->Cache.GetStats();
            {
                auto lazy = FreeWithActor(*a.Self);
                UNIT_ASSERT(lazy.GetHandle());
                UNIT_ASSERT_VALUES_EQUAL(a.Self->Cache.GetStats().LiveFrames, beforeA.LiveFrames + 1);
                UNIT_ASSERT_VALUES_EQUAL(b.Self->Cache.GetStats().LiveFrames, beforeB.LiveFrames);
                UNIT_ASSERT_VALUES_EQUAL(b.Self->Cache.GetStats().HeapAllocations, beforeB.HeapAllocations);
            }
            UNIT_ASSERT_VALUES_EQUAL(a.Self->Cache.GetStats().LiveFrames, beforeA.LiveFrames);
            UNIT_ASSERT_VALUES_EQUAL(a.Self->Cache.GetStats().CachedFrames, beforeA.CachedFrames + 1);
            UNIT_ASSERT_VALUES_EQUAL(b.Self->Cache.GetStats().CachedFrames, beforeB.CachedFrames);
        });
    }

    Y_UNIT_TEST(UncachedClassOverflowBlockOutlivesCache) {
        void* overflow = nullptr;
        constexpr size_t overflowSize = TAsyncFrameCache::MaxClasses + 1;
        {
            TAsyncFrameCache cache;
            for (size_t size = 1; size <= TAsyncFrameCache::MaxClasses; ++size) {
                void* frame = cache.Allocate(size);
                TAsyncFrameCache::Free(frame, size);
            }
            overflow = cache.Allocate(overflowSize);
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().SizeClasses, TAsyncFrameCache::MaxClasses);
            UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, 0);
        }
        TAsyncFrameCache::Free(overflow, overflowSize);
    }

    Y_UNIT_TEST(AlignmentAndAllocationOverflow) {
        TAsyncFrameCache cache;
        for (size_t size : {size_t(0), size_t(1), size_t(127)}) {
            void* cached = cache.Allocate(size);
            void* uncached = TAsyncFrameCache::AllocateUncached(size);
            UNIT_ASSERT_VALUES_EQUAL(reinterpret_cast<uintptr_t>(cached) % __STDCPP_DEFAULT_NEW_ALIGNMENT__, 0);
            UNIT_ASSERT_VALUES_EQUAL(reinterpret_cast<uintptr_t>(uncached) % __STDCPP_DEFAULT_NEW_ALIGNMENT__, 0);
            TAsyncFrameCache::Free(cached, size);
            TAsyncFrameCache::Free(uncached, size);
        }
        UNIT_ASSERT_EXCEPTION(cache.Allocate(std::numeric_limits<size_t>::max()), std::bad_alloc);
        UNIT_ASSERT_EXCEPTION(TAsyncFrameCache::AllocateUncached(std::numeric_limits<size_t>::max()), std::bad_alloc);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().SizeClasses, 3);
        UNIT_ASSERT_VALUES_EQUAL(cache.GetStats().LiveFrames, 0);
    }

    Y_UNIT_TEST(SerializedWorkerMigrationWithoutActorTls) {
        TAsyncFrameCache cache;
        void* frame = nullptr;
        std::thread([&] { frame = cache.Allocate(100); }).join();
        std::thread([&] { TAsyncFrameCache::Free(frame, 100); }).join();
        UNIT_ASSERT_VALUES_EQUAL(cache.Allocate(100), frame);
        TAsyncFrameCache::Free(frame, 100);
    }

    Y_UNIT_TEST(RootAndNestedFramesReusedAfterCompletion) {
        TFixture f;
        f.Actor.RunSync([&] { f.Self->Root(); });
        void* root = f.Counts.Root;
        void* child = f.Counts.Child;
        UNIT_ASSERT(root && child && root != child);
        UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().LiveFrames, 2);
        f.Actor.Receive(new TEvents::TEvWakeup);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.Value, 42);
        UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().LiveFrames, 0);
        const auto allocations = f.Self->Cache.GetStats().HeapAllocations;
        f.Actor.RunSync([&] { f.Self->Root(); });
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.Root, root);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.Child, child);
        UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().HeapAllocations, allocations);
        f.Actor.Receive(new TEvents::TEvWakeup);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.Finished, 2);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.RootDestroyed, 2);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.ChildDestroyed, 2);
    }

    Y_UNIT_TEST(DefaultActorStillUsesHeap) {
        TFixture f(false);
        f.Actor.RunSync([&] { f.Self->Root(); });
        UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().HeapAllocations, 0);
        f.Actor.Receive(new TEvents::TEvWakeup);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.Finished, 1);
    }

    Y_UNIT_TEST(UnstartedMembersAndFreeFunctions) {
        TFixture f;
        f.Actor.RunSync([&] {
            {
                auto child = f.Self->LazyVoid();
                UNIT_ASSERT(child.GetHandle());
                UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().LiveFrames, 1);
            }
            {
                auto child = f.Self->LazyValue();
                UNIT_ASSERT(child.GetHandle());
                UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().LiveFrames, 1);
            }
            {
                auto child = FreeWithActor(*f.Self);
                UNIT_ASSERT(child.GetHandle());
                UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().LiveFrames, 1);
            }
            const auto allocations = f.Self->Cache.GetStats().HeapAllocations;
            auto free = FreeWithoutActor();
            auto member = f.Self->ConstMember();
            auto lambda = []() -> async<int> { co_return 1; };
            auto closure = lambda();
            UNIT_ASSERT(free.GetHandle() && member.GetHandle() && closure.GetHandle());
            UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().LiveFrames, 0);
            UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().HeapAllocations, allocations);
        });
    }

    Y_UNIT_TEST(ParameterCopyFailureReturnsAllocatedFrame) {
        TFixture f;
        f.Actor.RunSync([&] {
            TThrowOnMove value;
            UNIT_ASSERT_EXCEPTION(f.Self->ThrowingParameter(value), TTestException);
            UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().LiveFrames, 0);
            UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().HeapAllocations, 1);
            UNIT_ASSERT_EXCEPTION(f.Self->ThrowingRootParameter(value), TTestException);
            UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().LiveFrames, 0);
            UNIT_ASSERT(f.Self->Cache.GetStats().CachedFrames >= 1);
        });
    }

    Y_UNIT_TEST(NestedExceptionReturnsBothFrames) {
        TFixture f;
        f.Actor.RunSync([&] { f.Self->Root(false, true); });
        f.Actor.Receive(new TEvents::TEvWakeup);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.Caught, 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.RootDestroyed, 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.ChildDestroyed, 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Self->Cache.GetStats().LiveFrames, 0);
    }

    Y_UNIT_TEST(PassAwayDestroysSuspendedRootAndChildBeforeCache) {
        TFixture f;
        f.Actor.RunSync([&] { f.Self->Root(); });
        f.Actor.Poison();
        UNIT_ASSERT(f.State.Destroyed);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.Finished, 0);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.RootDestroyed, 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.ChildDestroyed, 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.FinalStats.LiveFrames, 0);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.FinalStats.CachedFrames, 2);
    }

    Y_UNIT_TEST(PassAwayWaitsForGenericCompletion) {
        TFixture f;
        f.Actor.RunSync([&] { f.Self->Root(true); });
        f.Actor.Poison();
        UNIT_ASSERT(!f.State.Destroyed);
        f.Counts.Bridge.resume();
        UNIT_ASSERT(!f.State.Destroyed);
        f.Actor.Step();
        UNIT_ASSERT(f.State.Destroyed);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.RootDestroyed, 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.ChildDestroyed, 1);
        UNIT_ASSERT_VALUES_EQUAL(f.Counts.FinalStats.LiveFrames, 0);
    }

    Y_UNIT_TEST(ForcedCleanupThenLateGenericCompletion) {
        TAsyncTestActor::TState state;
        TCacheActorState counts;
        {
            TAsyncTestActorRuntime runtime;
            auto* self = new TCacheActor(state, counts, true);
            TAsyncTestActorRuntime::TAsyncActorOperations actor(runtime, runtime.Register(self));
            actor.Step();
            actor.RunSync([&] { self->Root(true); });
            // The generic awaiter receives a separately allocated bridge adapter,
            // which can survive forced destruction of both actor frames and their cache.
            UNIT_ASSERT(counts.Bridge && counts.Root && counts.Child);
            UNIT_ASSERT(counts.Bridge.address() != counts.Root);
            UNIT_ASSERT(counts.Bridge.address() != counts.Child);
            runtime.CleanupNode();
            UNIT_ASSERT(state.Destroyed);
            UNIT_ASSERT_VALUES_EQUAL(counts.Finished, 0);
            UNIT_ASSERT_VALUES_EQUAL(counts.RootDestroyed, 1);
            UNIT_ASSERT_VALUES_EQUAL(counts.ChildDestroyed, 1);
            UNIT_ASSERT_VALUES_EQUAL(counts.FinalStats.LiveFrames, 0);
            // As in the generic-awaiter tests, the runtime destructor cleans up
            // the late event; a cleaned mailbox cannot be dispatched again.
            counts.Bridge.resume();
        }
        UNIT_ASSERT_VALUES_EQUAL(counts.Finished, 0);
        UNIT_ASSERT_VALUES_EQUAL(counts.RootDestroyed, 1);
        UNIT_ASSERT_VALUES_EQUAL(counts.ChildDestroyed, 1);
    }
}

} // namespace NAsyncTest
