#include <library/cpp/testing/unittest/registar.h>
#include <ydb/core/testlib/basics/runtime.h>
#include <ydb/core/testlib/basics/appdata.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>

#include "kqp_shared_multitimer.h"

namespace {

using namespace NActors;
using namespace NKikimr::NKqp;

struct TEvPrivate {
    enum EEv {
        EvResult = EventSpaceBegin(NKikimr::TKikimrEvents::ES_PRIVATE),
        EvEnd
    };

    struct TEvResult : public TEventLocal<TEvResult, EvResult> {
        std::vector<int> RecordedWakeups;
    };
};

class TTimeoutUser : public TActorBootstrapped<TTimeoutUser> {
    using TBase = TActorBootstrapped<TTimeoutUser>;

    TMultiTimerMtSafePtr Multitimer;
    const std::vector<int> Timers;
    const std::vector<size_t> TimersToCancel; // indices in Timers/TimerIds

    std::vector<ui64> TimerIds;

    std::unique_ptr<TEvPrivate::TEvResult> Result;

public:
    TTimeoutUser(TMultiTimerMtSafePtr multitimer,
                 const std::vector<int>& timers,
                 const std::vector<size_t> timersToCancel)
        : Multitimer(std::move(multitimer))
        , Timers(timers)
        , TimersToCancel(timersToCancel)
    {
        Result = std::make_unique<TEvPrivate::TEvResult>();
    }

    void Bootstrap(const TActorContext& ctx) {
        Become(&TTimeoutUser::StateFunc);

        for (auto timer: Timers) {
            auto id = Multitimer->ScheduleTimer(
                ctx,
                TDuration::MilliSeconds(timer),
                std::make_unique<IEventHandle>(SelfId(), SelfId(), new TEvents::TEvWakeup())
            );
            TimerIds.push_back(id);
        }

        for (size_t i : TimersToCancel) {
            Multitimer->CancelTimer(TimerIds[i]);
        }
    }

    void HandleWakeup(TEvents::TEvWakeup::TPtr& ev, const TActorContext& ctx) {
        Result->RecordedWakeups.push_back(ctx.Monotonic().MilliSeconds());
    }

    void HandlePoison(TEvents::TEvPoisonPill::TPtr& ev, const TActorContext& ctx) {
        ctx.Send(ev->Sender, Result.release());
        Die(ctx);
    }

    STFUNC(StateFunc) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvents::TEvPoisonPill, HandlePoison);
            HFunc(TEvents::TEvWakeup, HandleWakeup);
            default:
                Y_ABORT();
        }
    }
};

} // namespace

Y_UNIT_TEST_SUITE(TSharedMultitimerTest) {
    Y_UNIT_TEST(InOrderSchedule) {
        const std::vector<int> goldWakeups = {1, 3, 4, 6, 7, 8, 9, 10};
        const std::vector<int> timers = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        const std::vector<size_t> timersToCancel = {1, 4}; // indices in Timers/TimerIds

        TTestBasicRuntime runtime;
        runtime.Initialize(NKikimr::TAppPrepare().Unwrap());
        runtime.SetLogPriority(NKikimrServices::KQP_PROXY,
                               NActors::NLog::PRI_TRACE);
        auto [multitimer, multitimerActorId] = CreateMultiTimer(
            [&runtime](IActor* actor) {
                return runtime.Register(actor);
        });

        std::vector<int> capturedScheduleEvents;
        auto scheduledFilter = [&](TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event, TDuration delay, TInstant& deadline) {
            if (event && event->Recipient == multitimerActorId) {
                capturedScheduleEvents.emplace_back(deadline.MilliSeconds());
            }
            return TTestActorRuntime::DefaultScheduledFilterFunc(runtime, event, delay, deadline);
        };
        runtime.SetScheduledEventFilter(scheduledFilter);
        runtime.EnableScheduleForActor(multitimerActorId, true);

        TActorId worker = runtime.Register(new TTimeoutUser(multitimer, timers, timersToCancel));
        runtime.DispatchEvents();

        TActorId edge = runtime.AllocateEdgeActor();
        runtime.Schedule(new IEventHandle(worker, edge, new TEvents::TEvPoisonPill), TDuration::Seconds(1));
        runtime.DispatchEvents();
        TAutoPtr<IEventHandle> handle;
        auto* result = runtime.GrabEdgeEventRethrow<TEvPrivate::TEvResult>(handle);

        UNIT_ASSERT_VALUES_EQUAL(result->RecordedWakeups.size(), goldWakeups.size());
        for (size_t i = 0; i < goldWakeups.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL(result->RecordedWakeups[i], goldWakeups[i]);
        }

        UNIT_ASSERT_VALUES_EQUAL(capturedScheduleEvents.size(), goldWakeups.size());
        for (size_t i = 0; i < goldWakeups.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL(goldWakeups[i], capturedScheduleEvents[i]);
        }
    }

    Y_UNIT_TEST(ReverseOrderSchedule) {
        const std::vector<int> goldWakeups = {1, 3, 4, 6, 7, 8, 9, 10};
        std::vector<int> timers = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        std::reverse(timers.begin(), timers.end());
        const std::vector<size_t> timersToCancel = {8, 5}; // indices in Timers/TimerIds

        TTestBasicRuntime runtime;
        runtime.Initialize(NKikimr::TAppPrepare().Unwrap());
        runtime.SetLogPriority(NKikimrServices::KQP_PROXY,
                               NActors::NLog::PRI_TRACE);
        auto [multitimer, multitimerActorId] = CreateMultiTimer(
            [&runtime](IActor* actor) {
                return runtime.Register(actor);
        });

        std::vector<int> capturedScheduleEvents;
        auto scheduledFilter = [&](TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event, TDuration delay, TInstant& deadline) {
            if (event->Recipient == multitimerActorId) {
                capturedScheduleEvents.emplace_back(delay.MilliSeconds());
            }
            return TTestActorRuntime::DefaultScheduledFilterFunc(runtime, event, delay, deadline);
        };
        runtime.SetScheduledEventFilter(scheduledFilter);
        runtime.EnableScheduleForActor(multitimerActorId, true);

        TActorId worker = runtime.Register(new TTimeoutUser(multitimer, timers, timersToCancel));
        runtime.DispatchEvents();

        TActorId edge = runtime.AllocateEdgeActor();
        runtime.Schedule(new IEventHandle(worker, edge, new TEvents::TEvPoisonPill), TDuration::Seconds(1));
        runtime.DispatchEvents();
        TAutoPtr<IEventHandle> handle;
        auto* result = runtime.GrabEdgeEventRethrow<TEvPrivate::TEvResult>(handle);

        UNIT_ASSERT_VALUES_EQUAL(result->RecordedWakeups.size(), goldWakeups.size());
        for (size_t i = 0; i < goldWakeups.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL(result->RecordedWakeups[i], goldWakeups[i]);
        }

        UNIT_ASSERT_VALUES_EQUAL(capturedScheduleEvents.size(), timers.size());
    }
}
