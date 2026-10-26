#pragma once

#include "events.h"
#include "resources.h"

#include <ydb/library/actors/core/actor.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/common_client/settings.h>

namespace NBridgeSkipper {

class ISdkGateway {
public:
    virtual ~ISdkGateway() = default;
    virtual void Read(TReadRequest request, std::function<void(TReadResult)> completed) = 0;
    virtual void Mutate(TMutationAttempt attempt, std::function<void(TRpcOutcome)> completed) = 0;
    // Coordinator only; Stop(true) drains foreign SDK producers.
    virtual void Stop() = 0;
};

// SDK client overrides take a bare gRPC authority, unlike driver connection
// strings. TLS credentials remain inherited from the validated driver config.
NYdb::TCommonClientSettings MakePinnedClientSettings(const TEndpoint& endpoint);

std::shared_ptr<ISdkGateway> CreateSdkGateway(const TConfig& config, const TLoadedCredentials& credentials);

// Actor-serialized bookkeeping, independently exercised with a fake SDK.
class TGatewayBudget {
public:
    static constexpr size_t HealthLimit = 32;
    explicit TGatewayBudget(size_t adminConcurrency);
    bool AcquireRead(const TReadRequest& request);
    bool SettleRead(const TString& requestId);
    bool AcquireMutation(const TString& attemptId);
    bool SettleMutation(const TString& attemptId);
    // Only during the acquiring actor turn, before calling the SDK.
    bool WithdrawUnsentMutation(const TString& attemptId);
    bool CloseAttempt(const TString& attemptId);
    bool IsClosed(const TString& attemptId) const;
    size_t Reads() const;
    size_t Mutations() const;

private:
    size_t AdminLimit;
    std::map<TString, TReadRequest> ReadJobs;
    std::set<std::pair<TString, EReadMethod>> Methods;
    std::set<TString> MutationJobs;
    std::set<TString> HandedOff;
    std::set<TString> Closed;
};

NActors::IActor* CreateGatewayActor(const TConfig& config, std::shared_ptr<TRuntimeResources> resources,
    std::shared_ptr<ISdkGateway> sdk);

// Also checks live controller readiness and absolute quorum/config validity;
// safe when the published snapshot has aged while its owner was stalled.
TString ValidateLiveDispatch(const TDispatchRequest& request, const TControllerSnapshot& snapshot,
    const TString& processId, const TString& memberId, const std::optional<TReadResult>& preflight,
    bool admissionOpen, bool localStopped, TDuration margin, TInstant now);

// Recheck immutable installed state, exact same-node preflight and expiry at handoff.
TString ValidateDispatch(const TDispatchRequest& request, const TDurableState& installed,
    const TString& processId, const TString& memberId, const std::optional<TReadResult>& preflight,
    bool admissionOpen, bool localStopped, TDuration margin, TInstant now);

} // namespace NBridgeSkipper
