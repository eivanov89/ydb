#include "kqp_session_router.h"

#include <util/generic/singleton.h>

namespace NKikimr::NKqp {

void TSessionRouterMt::AddIdleSessionActor(
    const TString& sessionId,
    const NActors::TActorId& actorId,
    NActors::TMonotonic deadline)
{
    TGuard<TAdaptiveLock> guard(Lock);
    AllSessions.emplace(sessionId, TSessionInfo {.SessionActorId = actorId});
    IdleSessions[sessionId] = deadline;
}

void TSessionRouterMt::Erase(const TString& sessionId) {
    AllSessions.erase(sessionId);
    IdleSessions.erase(sessionId);
}

// promotes idle time
NActors::TActorId TSessionRouterMt::GetSessionActor(const TString& sessionId, NActors::TMonotonic deadline) {
    TGuard<TAdaptiveLock> guard(Lock);

    auto it = AllSessions.find(sessionId);
    if (it == AllSessions.end()) {
        return {};
    }

    auto it2 = IdleSessions.find(sessionId);
    if (it2 != IdleSessions.end()) {
        it2->second = deadline;
    }

    return it->second.SessionActorId;
}

void TSessionRouterMt::MarkIdle(const TString& sessionId, NActors::TMonotonic deadline) {
    TGuard<TAdaptiveLock> guard(Lock);

    auto it = AllSessions.find(sessionId);
    if (it == AllSessions.end()) {
        // TODO: verify?
        return;
    }

    IdleSessions[sessionId] = deadline;
}

bool TSessionRouterMt::IsIdle(const TString& sessionId) {
    TGuard<TAdaptiveLock> guard(Lock);

    return IdleSessions.find(sessionId) != IdleSessions.end();
}

void TSessionRouterMt::MarkNonIdle(const TString& sessionId) {
    TGuard<TAdaptiveLock> guard(Lock);
    IdleSessions.erase(sessionId);
}

std::vector<TString> TSessionRouterMt::ForgetIdleSessions(
    NActors::TMonotonic now,
    const ui32 maxSessions)
{
    TGuard<TAdaptiveLock> guard(Lock);

    std::vector<TString> forgotten;

    if (IdleSessions.empty()) {
        return forgotten;
    }

    for (auto it = IdleSessions.begin(); it != IdleSessions.end();) {
        const auto& [sessionId, deadline] = *it;
        if (now >= deadline) {
            AllSessions.erase(sessionId);
            forgotten.emplace_back(sessionId);
            IdleSessions.erase(it++);
            if (forgotten.size() == maxSessions) {
                return forgotten;
            }
        } else {
            ++it;
        }
    }

    return forgotten;
}

TSessionRouterMt& GetGlobalSessionRouter() {
    return *Singleton<TSessionRouterMt>();
}

} // NKikimr::NKqp

