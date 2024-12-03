#pragma once

#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/event.h>

#include <util/datetime/base.h>

#include <memory>

namespace NKikimr::NKqp {

// IMultiTimerMtSafe is a thread-safe timer service shared by multiple actors,
// including session actors. It is designed for scenarios where most timers are
// typically canceled before they fire, which is common for most queries.
//
// IMultiTimerMtSafe enables scheduling multiple cancelable timers efficiently.
// However, cancellation is not guaranteed: a timer may already have fired, and
// its corresponding message might still be in the queue.
class IMultiTimerMtSafe: public TThrRefBase {
public:
    virtual size_t ScheduleTimer(
        const NActors::TActorContext &ctx,
        TDuration delta,
        std::unique_ptr<NActors::IEventHandle> ev) = 0;

    virtual void CancelTimer(size_t timerId) = 0;
};

using TMultiTimerMtSafePtr = TIntrusivePtr<IMultiTimerMtSafe>;

// Can be casted to IMultiTimerMtSafe
std::pair<TMultiTimerMtSafePtr, NActors::TActorId> CreateMultiTimer();

}; // NKikimr::NKqp
