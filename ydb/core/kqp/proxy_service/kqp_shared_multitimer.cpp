#include "kqp_shared_multitimer.h"

#include <ydb/library/actors/core/actorsystem.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/events.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>

#include <library/cpp/threading/queue/mpsc_intrusive_unordered.h>

#include <atomic>
#include <queue>
#include <memory>

namespace NKikimr::NKqp {

using namespace NActors;

namespace {

#define KQP_PROXY_LOG_T(ctx, stream) LOG_TRACE_S (ctx, NKikimrServices::KQP_PROXY, stream)
#define KQP_PROXY_LOG_D(ctx, stream) LOG_DEBUG_S (ctx, NKikimrServices::KQP_PROXY, stream)

constexpr TDuration MIN_TIMEOUT = TDuration::MilliSeconds(1);
constexpr TDuration LONG_TIMER_THRESHOLD = TDuration::Seconds(15);

// note, that it is desctructed inside AS
class TMPSCIntrusiveUnorderedWithDelete: public NThreading::TMPSCIntrusiveUnordered {
public:
    ~TMPSCIntrusiveUnorderedWithDelete() {
        while (auto* node = Pop()) {
            delete node;
        }
    }
};

struct TTimeout: public NThreading::TIntrusiveNode, public TThrRefBase {
    size_t Id = 0;
    std::unique_ptr<IEventHandle> Event;
    TDuration Delta;
    TMonotonic Deadline;

    TTimeout(size_t id, std::unique_ptr<IEventHandle> event, TDuration delta, TMonotonic deadline)
        : Id(id)
        , Event(std::move(event))
        , Delta(delta)
        , Deadline(deadline)
    {
    }
};

using TTimeoutPtr = TIntrusivePtr<TTimeout>;

// earliest Deadline first
struct DeadlineComparator {
    bool operator()(const TTimeoutPtr& lhs, const TTimeoutPtr& rhs) const {
        return lhs->Deadline > rhs->Deadline;
    }
};

struct TCancelTimeout: public NThreading::TIntrusiveNode {
    size_t Id = 0;
};

struct TScheduledDeadline: public NThreading::TIntrusiveNode {
    TMonotonic Deadline;
};

class TMultiTimer: public IMultiTimerMtSafe  {
    friend class TMultiTimerActor;
public:
    TMultiTimer() = default;

    // note, we don't send poison to TimerActor
    ~TMultiTimer() = default;

    void Init(const TActorId& id) {
        TimerActor = id;
    }

    size_t ScheduleTimer(
        const TActorContext &ctx,
        TDuration delta,
        std::unique_ptr<IEventHandle> ev) override
    {
        delta = std::max(delta, MIN_TIMEOUT);

        auto now = ctx.Monotonic();
        auto deadline = now + delta;
        size_t id = NextId.fetch_add(1, std::memory_order_relaxed);

        PendingScheduleQueue.Push(new TTimeout(id, std::move(ev), delta, deadline));
        ScheduleDeadlineIfNeeded(ctx, deadline, now);

        KQP_PROXY_LOG_T(ctx, "" << TimerActor << " scheduled timer " << id << " on " << deadline
            <<  " (after " << delta << "), earliest wakeup on "
            << TMonotonic::MilliSeconds(EarliestTimerTsMs.load(std::memory_order_relaxed))
            << ", now " << now);
        return id;
    }

    void CancelTimer(size_t timerId) override {
        PendingCancelationQueue.Push(new TCancelTimeout{.Id = timerId});
    }

private:
    void ScheduleDeadlineIfNeeded(const TActorContext &ctx, TMonotonic deadline, TMonotonic now) {
        const auto desiredTs = deadline.MilliSeconds();
        auto ts = EarliestTimerTsMs.load(std::memory_order_acquire);
        bool changed = false;
        while (!changed && (ts == 0 || desiredTs < ts)) {
            changed = EarliestTimerTsMs.compare_exchange_weak(ts, desiredTs, std::memory_order_release);
        }

        if (changed) {
            PendingSetDeadlines.Push(new TScheduledDeadline{.Deadline = deadline});
            auto handle = std::make_unique<IEventHandle>(TimerActor, TimerActor, new TEvents::TEvWakeup());
            if ((deadline - now) <= LONG_TIMER_THRESHOLD) {
                ctx.ExecutorThread.Schedule(deadline, handle.release());
                KQP_PROXY_LOG_D(ctx, "" << TimerActor << " scheduled short timer wakeup on " << deadline);
            } else {
                ctx.Schedule(LONG_TIMER_THRESHOLD, std::move(handle));
                KQP_PROXY_LOG_D(ctx, "" << TimerActor << " scheduled long timer wakeup after " << LONG_TIMER_THRESHOLD);
            }
        } else {
            // another thread scheduled earlier deadline, than ours
            // or there was earlier deadline in the queue
        }
    }

private:
    TActorId TimerActor;

    std::atomic_uint64_t NextId;

    std::atomic_uint64_t EarliestTimerTsMs;

    TMPSCIntrusiveUnorderedWithDelete PendingScheduleQueue;
    TMPSCIntrusiveUnorderedWithDelete PendingCancelationQueue;
    TMPSCIntrusiveUnorderedWithDelete PendingSetDeadlines;
};

class TMultiTimerActor : public TActor<TMultiTimerActor> {
public:
    TMultiTimerActor(TIntrusivePtr<TMultiTimer> multiTimer)
        : TActor(&TThis::StateWork)
        , MultiTimer(std::move(multiTimer))
    {
    }

    ~TMultiTimerActor() = default;

    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            CFunc(TEvents::TEvPoisonPill::EventType, PoisonPill);
            CFunc(TEvents::TEvWakeup::EventType, Wakeup);
        }
    }

private:
    void PoisonPill(const TActorContext &ctx) {
        return Die(ctx);
    }

    void Wakeup(const TActorContext &ctx) {
        MultiTimer->EarliestTimerTsMs.store(0, std::memory_order_release);

        auto now = ctx.Monotonic();
        size_t timersFired = 0;

        // clear expired timers (the timers which will not wakeup us anymore)
        while (!SetTimerDeadlines.empty() && SetTimerDeadlines.top() <= now) {
            SetTimerDeadlines.pop();
        }

        while (auto* node = MultiTimer->PendingCancelationQueue.Pop()) {
            auto* cancelation = static_cast<TCancelTimeout*>(node);
            CancelledTimeouts.insert(cancelation->Id);
            delete cancelation;
        }

        while (auto* node = MultiTimer->PendingScheduleQueue.Pop()) {
            auto* timeout = static_cast<TTimeout*>(node);
            if (auto it = CancelledTimeouts.find(timeout->Id); it != CancelledTimeouts.end()) {
                CancelledTimeouts.erase(it);
                delete timeout;
                continue;
            }
            TimeoutQueue.emplace(timeout);
        }

        while (auto* node = MultiTimer->PendingSetDeadlines.Pop()) {
            auto* deadline = static_cast<TScheduledDeadline*>(node);
            if (deadline->Deadline > now) {
                SetTimerDeadlines.push(deadline->Deadline);
            }
            delete deadline;
        }

        // fire ready timers
        while (!TimeoutQueue.empty() && TimeoutQueue.top()->Deadline <= now) {
            const auto& timeout = TimeoutQueue.top();
            if (auto it = CancelledTimeouts.find(timeout->Id); it == CancelledTimeouts.end()) {
                ctx.Send(timeout->Event.release());
                ++timersFired;
            } else {
                CancelledTimeouts.erase(it);
            }
            TimeoutQueue.pop();
        }

        // skip cancelled timers at the top of the queue
        while (!TimeoutQueue.empty()) {
            const auto& timeout = TimeoutQueue.top();
            if (auto it = CancelledTimeouts.find(timeout->Id); it == CancelledTimeouts.end()) {
                break;
            } else {
                CancelledTimeouts.erase(it);
                TimeoutQueue.pop();
            }
        }

        // finally check if we have a timer to schedule
        if (!TimeoutQueue.empty()) {
            const auto& nextTimer = TimeoutQueue.top();
            auto nextDeadline = nextTimer->Deadline;
            bool needSchedule = true;
            if (!SetTimerDeadlines.empty()) {
                auto nextSetDeadline = SetTimerDeadlines.top();
                if (nextSetDeadline <= nextDeadline) {
                    needSchedule = false;
                }
            }
            if (needSchedule) {
                MultiTimer->ScheduleDeadlineIfNeeded(ctx, nextTimer->Deadline, now);
            }
        }

        KQP_PROXY_LOG_T(ctx, "" << SelfId() << " multitimer wakeup at " << now
            << ", timers fired: " << timersFired
            << ", timers in queue left: " << TimeoutQueue.size()
            << ", next wakeup "
            << TMonotonic::MilliSeconds(MultiTimer->EarliestTimerTsMs.load(std::memory_order_relaxed)));
    }

private:
    TIntrusivePtr<TMultiTimer> MultiTimer;

    // user timers with callback events
    std::priority_queue<TTimeoutPtr, std::vector<TTimeoutPtr>, DeadlineComparator> TimeoutQueue;

    // deadlines when we scheduled wakeup
    std::priority_queue<TMonotonic, std::vector<TMonotonic>, std::greater<TMonotonic>> SetTimerDeadlines;

    // timeouts cancelled by user
    THashSet<ui64> CancelledTimeouts;
};

} // namespace

std::pair<TMultiTimerMtSafePtr, NActors::TActorId> CreateMultiTimer(
    std::function<NActors::TActorId(NActors::IActor*)> actorRegistrator)
{
    auto multiTimer = MakeIntrusive<TMultiTimer>();
    auto* timerActor = new TMultiTimerActor(multiTimer);
    TActorId id = actorRegistrator(timerActor);
    multiTimer->Init(id);

    return std::make_pair(multiTimer, id);
}

} // namespace NKikimr::NKqp
