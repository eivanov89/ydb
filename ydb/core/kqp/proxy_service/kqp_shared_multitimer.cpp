#include "kqp_shared_multitimer.h"

#include <ydb/library/actors/core/actorsystem.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/events.h>
#include <ydb/library/actors/core/hfunc.h>

#include <library/cpp/threading/queue/mpsc_intrusive_unordered.h>

#include <atomic>
#include <queue>
#include <memory>

namespace NKikimr::NKqp {

using namespace NActors;

namespace {

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

class TMultiTimerActor : public TActorBootstrapped<TMultiTimerActor>, public IMultiTimerMtSafe {
public:
    TMultiTimerActor() = default;
    ~TMultiTimerActor() = default;

    void Bootstrap() {
        Become(&TThis::StateWork);

        // This is a very rare case, when ScheduleTimer() is called before bootstrapping:
        // ScheduleTimer() needs to know this actor's ID, which we obtain after bootstrapping.
        // An alternative would be to force having Init(actorId), which is error prone.
        SelfId_ = static_cast<TActorId>(SelfId());
        Bootstrapped.test_and_set(std::memory_order_release);

        Wakeup(TlsActivationContext->AsActorContext());
    }

    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            CFunc(TEvents::TEvPoisonPill::EventType, PoisonPill);
            CFunc(TEvents::TEvWakeup::EventType, Wakeup);
        }
    }

public:
    size_t ScheduleTimer(
        const TActorContext &ctx,
        TDuration delta,
        std::unique_ptr<IEventHandle> ev) override
    {
        delta = std::max(delta, MIN_TIMEOUT);

        auto now = TMonotonic::Now();
        auto deadline = now + delta;
        size_t id = NextId.fetch_add(1, std::memory_order_relaxed);

        PendingScheduleQueue.Push(new TTimeout(id, std::move(ev), delta, deadline));

        if (Y_UNLIKELY(!Bootstrapped.test(std::memory_order_acquire))) {
            return id;
        }

        ScheduleDeadlineIfNeededMtSafe(ctx, deadline, delta);
        return id;
    }

    void CancelTimer(const TActorContext &ctx, size_t timerId) override
    {
        PendingCancelationQueue.Push(new TCancelTimeout{.Id = timerId});
    }

private:
    void PoisonPill(const TActorContext &ctx) {
        // we don't care to cancel
        return Die(ctx);
    }

    void Wakeup(const TActorContext &ctx) {
        EarliestTimerTsMs.store(0, std::memory_order_release);

        auto now = ctx.Monotonic();

        // clear expired timers (the timers which will not wakeup us anymore)
        while (!SetTimerDeadlines.empty() && SetTimerDeadlines.top() <= now) {
            SetTimerDeadlines.pop();
        }

        while (auto* node = PendingCancelationQueue.Pop()) {
            auto* cancelation = static_cast<TCancelTimeout*>(node);
            CancelledTimeouts.insert(cancelation->Id);
            delete cancelation;
        }

        while (auto* node = PendingScheduleQueue.Pop()) {
            auto* timeout = static_cast<TTimeout*>(node);
            if (auto it = CancelledTimeouts.find(timeout->Id); it != CancelledTimeouts.end()) {
                CancelledTimeouts.erase(it);
                delete timeout;
                continue;
            }
            TimeoutQueue.emplace(timeout);
        }

        while (auto* node = PendingSetDeadlines.Pop()) {
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
            }
            TimeoutQueue.pop();
        }

        // finally check if we have a timer to schedule
        if (!TimeoutQueue.empty()) {
            const auto& nextTimer = TimeoutQueue.top();
            auto nextDeadline = nextTimer->Deadline;
            if (!SetTimerDeadlines.empty()) {
                auto nextSetDeadline = SetTimerDeadlines.top();
                if (nextSetDeadline <= nextDeadline) {
                    return;
                }
            }
            ScheduleDeadlineIfNeededMtSafe(ctx, nextTimer->Deadline, nextTimer->Delta);
        }
    }

    void ScheduleDeadlineIfNeededMtSafe(const TActorContext &ctx, TMonotonic deadline, TDuration delta) {
        const auto desiredTs = deadline.MilliSeconds();
        auto ts = EarliestTimerTsMs.load(std::memory_order_acquire);
        bool changed = false;
        while (ts == 0 || desiredTs < ts) {
            changed = EarliestTimerTsMs.compare_exchange_weak(ts, desiredTs, std::memory_order_release);
        }

        if (changed) {
            PendingSetDeadlines.Push(new TScheduledDeadline{.Deadline = deadline});

            if (delta <= LONG_TIMER_THRESHOLD) {
                ctx.ExecutorThread.Schedule(deadline, new IEventHandle(SelfId_, SelfId_, new TEvents::TEvWakeup()));
            } else {
                auto handle = std::make_unique<IEventHandle>(SelfId_, SelfId_, new TEvents::TEvWakeup());
                ctx.Schedule(deadline, std::move(handle));
            }
        } else {
            // another thread scheduled earlier deadline, than ours
        }
    }

private:
    // user timers with callback events
    std::priority_queue<TTimeoutPtr, std::vector<TTimeoutPtr>, DeadlineComparator> TimeoutQueue;

    // deadlines when we scheduled wakeup
    std::priority_queue<TMonotonic, std::vector<TMonotonic>, std::greater<TMonotonic>> SetTimerDeadlines;

    // timeouts cancelled by user
    THashSet<ui64> CancelledTimeouts;

private:
    // Mt stuff for IMultiTimerMtSafe

    std::atomic_flag Bootstrapped;
    TActorId SelfId_;

    std::atomic_uint64_t NextId;

    std::atomic_uint64_t EarliestTimerTsMs;

    TMPSCIntrusiveUnorderedWithDelete PendingScheduleQueue;
    TMPSCIntrusiveUnorderedWithDelete PendingCancelationQueue;
    TMPSCIntrusiveUnorderedWithDelete PendingSetDeadlines;
};

} // namespace

IActor* CreateMultiTimer() {
    return new TMultiTimerActor();
}

} // namespace NKikimr::NKqp
