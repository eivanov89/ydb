#pragma once

#include <ydb/library/actors/core/actorid.h>
#include <ydb/library/actors/core/monotonic.h>

#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/system/spinlock.h>

namespace NKikimr::NKqp {

// a quick non-efficient impl
class TSessionRouterMt {
public:
    void AddIdleSessionActor(const TString& sessionId, const NActors::TActorId& actorId, NActors::TMonotonic deadline);
    void Erase(const TString& sessionId);

    // promotes idle time
    NActors::TActorId GetSessionActor(const TString& sessionId, NActors::TMonotonic deadline);

    void MarkIdle(const TString& sessionId, NActors::TMonotonic deadline);
    bool IsIdle(const TString& sessionId);

    void MarkNonIdle(const TString& sessionId);

    std::vector<TString> ForgetIdleSessions(NActors::TMonotonic now, const ui32 maxSessions);

private:
    struct TSessionInfo {
        NActors::TActorId SessionActorId;
    };

private:
    TAdaptiveLock Lock;

    THashMap<TString, TSessionInfo> AllSessions;
    THashMap<TString, NActors::TMonotonic> IdleSessions; // -> deadline
};

TSessionRouterMt& GetGlobalSessionRouter();

} // NKikimr::NKqp
