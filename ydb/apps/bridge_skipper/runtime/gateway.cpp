#include "gateway.h"
#include "controller.h"

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/public/api/protos/ydb_monitoring.pb.h>
#include <ydb/public/api/protos/draft/ydb_bridge.pb.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/config/config.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/discovery/discovery.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/draft/ydb_bridge.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/monitoring/monitoring.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/proto/accessor.h>

#include <ydb/apps/bridge_skipper/domain/clock.h>
#include <util/generic/algorithm.h>
#include <util/generic/yexception.h>
#include <util/string/cast.h>

namespace NBridgeSkipper {
namespace {

std::string Std(TStringBuf value) {
    return std::string(value.data(), value.size());
}

TString StatusName(NYdb::EStatus status) {
    switch (status) {
        case NYdb::EStatus::SUCCESS: return "SUCCESS";
        case NYdb::EStatus::BAD_REQUEST: return "BAD_REQUEST";
        case NYdb::EStatus::UNAUTHORIZED: return "UNAUTHORIZED";
        case NYdb::EStatus::INTERNAL_ERROR: return "INTERNAL_ERROR";
        case NYdb::EStatus::ABORTED: return "ABORTED";
        case NYdb::EStatus::UNAVAILABLE: return "UNAVAILABLE";
        case NYdb::EStatus::OVERLOADED: return "OVERLOADED";
        case NYdb::EStatus::SCHEME_ERROR: return "SCHEME_ERROR";
        case NYdb::EStatus::GENERIC_ERROR: return "GENERIC_ERROR";
        case NYdb::EStatus::TIMEOUT: return "TIMEOUT";
        case NYdb::EStatus::BAD_SESSION: return "BAD_SESSION";
        case NYdb::EStatus::PRECONDITION_FAILED: return "PRECONDITION_FAILED";
        case NYdb::EStatus::ALREADY_EXISTS: return "ALREADY_EXISTS";
        case NYdb::EStatus::NOT_FOUND: return "NOT_FOUND";
        case NYdb::EStatus::STATUS_UNDEFINED: return "STATUS_UNDEFINED";
        case NYdb::EStatus::SESSION_EXPIRED: return "SESSION_EXPIRED";
        case NYdb::EStatus::CANCELLED: return "CANCELLED";
        case NYdb::EStatus::UNDETERMINED: return "UNDETERMINED";
        case NYdb::EStatus::UNSUPPORTED: return "UNSUPPORTED";
        case NYdb::EStatus::SESSION_BUSY: return "SESSION_BUSY";
        case NYdb::EStatus::EXTERNAL_ERROR: return "EXTERNAL_ERROR";
        case NYdb::EStatus::TRANSPORT_UNAVAILABLE: return "TRANSPORT_UNAVAILABLE";
        case NYdb::EStatus::CLIENT_RESOURCE_EXHAUSTED: return "CLIENT_RESOURCE_EXHAUSTED";
        case NYdb::EStatus::CLIENT_DEADLINE_EXCEEDED: return "CLIENT_DEADLINE_EXCEEDED";
        case NYdb::EStatus::CLIENT_INTERNAL_ERROR: return "CLIENT_INTERNAL_ERROR";
        case NYdb::EStatus::CLIENT_CANCELLED: return "CLIENT_CANCELLED";
        case NYdb::EStatus::CLIENT_UNAUTHENTICATED: return "CLIENT_UNAUTHENTICATED";
        case NYdb::EStatus::CLIENT_CALL_UNIMPLEMENTED: return "CLIENT_CALL_UNIMPLEMENTED";
        case NYdb::EStatus::CLIENT_OUT_OF_RANGE: return "CLIENT_OUT_OF_RANGE";
        case NYdb::EStatus::CLIENT_DISCOVERY_FAILED: return "CLIENT_DISCOVERY_FAILED";
        case NYdb::EStatus::CLIENT_LIMITS_REACHED: return "CLIENT_LIMITS_REACHED";
        default: return "TRANSPORT_OR_UNSUPPORTED_STATUS";
    }
}

class TRedactor {
public:
    explicit TRedactor(const TLoadedCredentials& credentials) {
        for (const auto* secret : {&credentials.Ca, &credentials.ClientCert, &credentials.ClientKey, &credentials.Token}) {
            if (*secret && !(*secret)->empty()) {
                Secrets.push_back(**secret);
            }
        }
    }

    TString Issue(const NYdb::TStatus& status) const {
        if (status.GetIssues().Empty()) {
            return {};
        }
        TString result(status.GetIssues().begin()->GetMessage());
        // Redact before truncation so a partial secret is never exposed.
        for (const auto& secret : Secrets) {
            size_t position = 0;
            while ((position = result.find(secret, position)) != TString::npos) {
                result.replace(position, secret.size(), "[REDACTED]");
                position += 10;
            }
        }
        result.resize(Min<size_t>(result.size(), 4096));
        return result;
    }

private:
    std::vector<TString> Secrets;
};

TString IssueCode(const NYdb::TStatus& status) {
    if (status.GetIssues().Empty()) {
        return {};
    }
    const auto code = status.GetIssues().begin()->GetCode();
    return code == 0 ? TString("DEFAULT_ERROR") : ToString(code);
}

TString ReadMethodName(EReadMethod method) {
    switch (method) {
        case EReadMethod::Admin: return "GetClusterState";
        case EReadMethod::Health: return "Diagnostic";
        case EReadMethod::Config: return "FetchAllConfigs";
        case EReadMethod::Discovery: return "ListEndpoints";
    }
    return "OTHER";
}

TString MutationMethodName(EBridgeMethod method) {
    switch (method) {
        case EBridgeMethod::UpdateClusterState: return "UpdateClusterState";
        case EBridgeMethod::Convert: return "ConvertToBridge";
        case EBridgeMethod::AddPile: return "AddPile";
        case EBridgeMethod::RemovePile: return "RemovePile";
    }
    return "OTHER";
}

TString MetricStatus(TStringBuf status) {
    for (const TStringBuf fixed : {"SUCCESS", "BAD_REQUEST", "UNAUTHORIZED", "INTERNAL_ERROR", "ABORTED", "UNAVAILABLE", "OVERLOADED",
            "SCHEME_ERROR", "GENERIC_ERROR", "TIMEOUT", "BAD_SESSION", "PRECONDITION_FAILED", "ALREADY_EXISTS", "NOT_FOUND",
            "STATUS_UNDEFINED", "SESSION_EXPIRED", "CANCELLED", "UNDETERMINED", "UNSUPPORTED", "SESSION_BUSY", "EXTERNAL_ERROR",
            "TRANSPORT_UNAVAILABLE", "CLIENT_RESOURCE_EXHAUSTED", "CLIENT_DEADLINE_EXCEEDED", "CLIENT_INTERNAL_ERROR", "CLIENT_CANCELLED",
            "CLIENT_UNAUTHENTICATED", "CLIENT_CALL_UNIMPLEMENTED", "CLIENT_OUT_OF_RANGE", "CLIENT_DISCOVERY_FAILED", "CLIENT_LIMITS_REACHED",
            "SDK_START_ERROR", "SDK_RESULT_ERROR", "UNSUPPORTED_DIAGNOSTIC_CAPABILITY", "TRANSPORT_OR_UNSUPPORTED_STATUS"}) {
        if (fixed == status) {
            return TString(fixed);
        }
    }
    return "OTHER";
}

class TSdkGateway final : public ISdkGateway {
public:
    TSdkGateway(const TConfig& config, const TLoadedCredentials& credentials)
        : Config(config)
        , Redactor(std::make_shared<TRedactor>(credentials))
        , Driver(MakeDriverConfig(config, credentials))
    {
    }

    void Read(TReadRequest request, std::function<void(TReadResult)> completed) override {
        TReadResult result;
        result.Request = request;
        try {
            auto& clients = Clients(request.Endpoint);
            const auto now = TSkipperClock::Now();
            if (request.Deadline <= now) {
                result.Status = "READ_DEADLINE";
                result.FinishedAt = now;
                completed(std::move(result));
                return;
            }
            const auto timeout = Min(request.Timeout, request.Deadline - now);
            auto subscribe = [result = std::move(result), completed, redactor = Redactor](auto future, auto extract, TInstant startedAt) mutable {
                result.StartedAt = startedAt;
                future.Subscribe([result = std::move(result), completed, redactor, extract](const auto& settled) mutable {
                    try {
                        const auto& value = settled.GetValueSync();
                        result.Success = value.IsSuccess();
                        result.Status = StatusName(value.GetStatus());
                        result.IssueCode = IssueCode(value);
                        result.Issue = redactor->Issue(value);
                        if (result.Success) {
                            extract(result, value);
                        }
                    } catch (...) {
                        result.Success = false;
                        result.Status = "SDK_RESULT_ERROR";
                    }
                    result.FinishedAt = TSkipperClock::Now();
                    completed(std::move(result));
                });
            };
            // Timestamp is the actual diagnostic/RPC start, after client lookup.
            // Set it in the captured completion envelope immediately before call.
            auto startSubscribe = [&](auto launch, auto extract) {
                const auto start = TSkipperClock::Now();
                auto future = launch();
                subscribe(std::move(future), extract, start);
            };
            switch (request.Method) {
                case EReadMethod::Admin:
                    startSubscribe([&] { return clients.Bridge.GetClusterState(NYdb::NBridge::TGetClusterStateSettings().ClientTimeout(timeout).OperationTimeout(timeout)); },
                        [](TReadResult& target, const NYdb::NBridge::TGetClusterStateResult& value) {
                            TClusterView view;
                            view.Generation = value.GetGeneration();
                            view.Converted = true;
                            view.ConversionInProgress = value.IsConversionInProgress();
                            view.Endpoint = target.Request.Endpoint;
                            view.ReceivedAt = TSkipperClock::Now();
                            for (const auto& pile : value.GetState()) {
                                const TString name(pile.PileName);
                                if (!view.Piles.emplace(name, static_cast<EPileState>(pile.State)).second) {
                                    target.Success = false;
                                    target.Status = "DUPLICATE_PILE_NAME";
                                }
                                view.PileOrder.push_back(name);
                            }
                            if (const auto& pending = value.GetPendingPileOperation()) {
                                view.Pending.push_back({pending->Kind == NYdb::NBridge::TPendingPileOperation::EKind::ADD ? TString("ADD") : pending->Kind == NYdb::NBridge::TPendingPileOperation::EKind::REMOVE ? TString("REMOVE") : TString("UNKNOWN"), TString(pending->PileName), TString(pending->ErrorReason), {}});
                            }
                            const auto error = ValidateClusterView(view);
                            if (!error.empty()) {
                                target.Success = false;
                                target.Status = "UNSUPPORTED_BRIDGE_RESULT";
                            }
                            target.Cluster = std::move(view);
                        });
                    break;
                case EReadMethod::Health:
                    if (Config.EvidenceProfile == EEvidenceProfile::NodewardenLocalV1 && !request.IdentityOnly) {
                        TReadResult unsupported;
                        unsupported.Request = request;
                        unsupported.Status = "UNSUPPORTED_DIAGNOSTIC_CAPABILITY";
                        unsupported.FinishedAt = TSkipperClock::Now();
                        completed(std::move(unsupported));
                        break;
                    }
                    startSubscribe([&] { return clients.Monitoring.SelfCheck(NYdb::NMonitoring::TSelfCheckSettings().ReturnVerboseStatus(true).ClientTimeout(timeout).OperationTimeout(Min(request.OperationTimeout, timeout))); },
                        [](TReadResult& target, const NYdb::NMonitoring::TSelfCheckResult& value) {
                            target.Health = std::make_shared<Ydb::Monitoring::SelfCheckResult>(NYdb::TProtoAccessor::GetProto(value));
                            target.ResponseBytes = target.Health->ByteSizeLong();
                        });
                    break;
                case EReadMethod::Config:
                    startSubscribe([&] { return clients.Config.FetchAllConfigs(NYdb::NConfig::TFetchAllConfigsSettings().ClientTimeout(timeout).OperationTimeout(timeout)); },
                        [](TReadResult& target, const NYdb::NConfig::TFetchConfigResult& value) {
                            size_t bytes = 0;
                            for (const auto& config : value.GetConfigs()) {
                                bytes += config.Config.size();
                                if (bytes > 64 * 1024 * 1024) {
                                    target.Success = false;
                                    target.Status = "CONFIG_SIZE_LIMIT";
                                    target.Configs.clear();
                                    return;
                                }
                                target.Configs.emplace_back(config.Config);
                            }
                            target.ResponseBytes = bytes;
                        });
                    break;
                case EReadMethod::Discovery:
                    startSubscribe([&] { return clients.Discovery.ListEndpoints(NYdb::NDiscovery::TListEndpointsSettings().ClientTimeout(timeout)); },
                        [](TReadResult& target, const NYdb::NDiscovery::TListEndpointsResult& value) {
                            const auto scheme = ParseEndpoint(target.Request.Endpoint).Scheme;
                            for (const auto& endpoint : value.GetEndpointsInfo()) {
                                if (endpoint.Port == 0 || endpoint.Port > 65535 || endpoint.Address.empty()) {
                                    continue;
                                }
                                TString host(endpoint.Address);
                                if (host.find(':') != TString::npos && !host.StartsWith('[')) {
                                    host = "[" + host + "]";
                                }
                                const auto uri = scheme + "://" + host + ":" + ToString(endpoint.Port);
                                try {
                                    ParseEndpoint(uri);
                                    target.Discovered.push_back({uri, endpoint.NodeId, TString(endpoint.BridgePileName)});
                                } catch (const yexception&) {
                                    // Discovery is supplementary, never a dialing override.
                                }
                            }
                        });
                    break;
            }
        } catch (...) {
            result.Request = request;
            result.Success = false;
            result.Status = "SDK_START_ERROR";
            result.FinishedAt = TSkipperClock::Now();
            completed(std::move(result));
        }
    }

    void Mutate(TMutationAttempt attempt, std::function<void(TRpcOutcome)> completed) override {
        TRpcOutcome outcome;
        outcome.AttemptId = attempt.Id;
        outcome.DesignatedMember = attempt.DesignatedMember;
        outcome.DesignatedProcess = attempt.DesignatedProcess;
        try {
            auto& client = Clients(attempt.Endpoint).Bridge;
            NYdb::TAsyncStatus future;
            const auto ordinary = Config.Operations.MutationClientTimeout;
            const auto membership = Config.Operations.MembershipClientTimeout;
            switch (attempt.Method) {
                case EBridgeMethod::UpdateClusterState: {
                    std::vector<NYdb::NBridge::TPileStateUpdate> updates;
                    for (const auto& [pile, state] : attempt.Update) {
                        updates.push_back({Std(pile), static_cast<NYdb::NBridge::EPileState>(state), 0});
                    }
                    future = client.UpdateClusterState(updates, {}, NYdb::NBridge::TUpdateClusterStateSettings().ClientTimeout(ordinary).UseClientTimeoutForOperation(false));
                    break;
                }
                case EBridgeMethod::Convert:
                    future = client.ConvertToBridge(NYdb::NBridge::TConvertToBridgeSettings().ClientTimeout(ordinary).UseClientTimeoutForOperation(false));
                    break;
                case EBridgeMethod::AddPile:
                    Y_ENSURE(attempt.Target, "missing exact Add target");
                    future = client.AddPile(Std(*attempt.Target), NYdb::NBridge::TAddPileSettings().ClientTimeout(membership).UseClientTimeoutForOperation(false));
                    break;
                case EBridgeMethod::RemovePile:
                    Y_ENSURE(attempt.Target, "missing exact Remove target");
                    future = client.RemovePile(Std(*attempt.Target), NYdb::NBridge::TRemovePileSettings().ClientTimeout(membership).UseClientTimeoutForOperation(false));
                    break;
            }
            future.Subscribe([outcome, completed, redactor = Redactor](const auto& settled) mutable {
                try {
                    const auto& value = settled.GetValueSync();
                    outcome.Success = value.IsSuccess();
                    outcome.Status = StatusName(value.GetStatus());
                    outcome.IssueCode = IssueCode(value);
                    outcome.Issue = redactor->Issue(value);
                } catch (...) {
                    outcome.Status = "SDK_RESULT_ERROR";
                }
                completed(std::move(outcome));
            });
        } catch (...) {
            // Once the permission is consumed, synchronous SDK failure remains
            // conservative ambiguity unless a qualified negative proof exists.
            outcome.Status = "SDK_START_ERROR";
            completed(std::move(outcome));
        }
    }

    void Stop() override {
        Driver.Stop(true);
    }

private:
    struct TClients {
        NYdb::NBridge::TBridgeClient Bridge;
        NYdb::NMonitoring::TMonitoringClient Monitoring;
        NYdb::NConfig::TConfigClient Config;
        NYdb::NDiscovery::TDiscoveryClient Discovery;

        TClients(const NYdb::TDriver& driver, const NYdb::TCommonClientSettings& settings)
            : Bridge(driver, settings)
            , Monitoring(driver, settings)
            , Config(driver, settings)
            , Discovery(driver, settings)
        {
        }
    };

    TClients& Clients(const TString& endpoint) {
        const auto parsed = ParseEndpoint(endpoint);
        Y_ENSURE(parsed.Scheme == Config.YdbEndpoints.front().Scheme, "pinned endpoint TLS scheme mismatch");
        auto& client = EndpointClients[endpoint];
        if (!client) {
            client = std::make_unique<TClients>(Driver, MakePinnedClientSettings(parsed));
        }
        return *client;
    }

    static NYdb::TDriverConfig MakeDriverConfig(const TConfig& config, const TLoadedCredentials& credentials) {
        NYdb::TDriverConfig result(Std(config.YdbEndpoints.front().Uri));
        result.SetDatabase("").SetDiscoveryMode(NYdb::EDiscoveryMode::Off).SetMaxInboundMessageSize(64 * 1024 * 1024).SetClientThreadsNum(4);
        if (config.YdbEndpoints.front().Scheme == "grpcs") {
            result.UseSecureConnection(credentials.Ca ? Std(*credentials.Ca) : std::string());
        }
        if (credentials.ClientCert && credentials.ClientKey) {
            result.UseClientCertificate(Std(*credentials.ClientCert), Std(*credentials.ClientKey));
        }
        if (credentials.Token) {
            result.SetAuthToken(Std(*credentials.Token));
        }
        return result;
    }

    TConfig Config;
    std::shared_ptr<TRedactor> Redactor;
    NYdb::TDriver Driver;
    std::map<TString, std::unique_ptr<TClients>> EndpointClients;
};

struct TEvSdkReadComplete : NActors::TEventLocal<TEvSdkReadComplete, TEvRuntime::EvSdkReadComplete> {
    TReadResult Result;
};

struct TEvSdkMutationComplete : NActors::TEventLocal<TEvSdkMutationComplete, TEvRuntime::EvSdkMutationComplete> {
    TMutationResult Result;
};

class TYdbGatewayActor final : public NActors::TActorBootstrapped<TYdbGatewayActor> {
public:
    TYdbGatewayActor(TConfig config, std::shared_ptr<TRuntimeResources> resources, std::shared_ptr<ISdkGateway> sdk)
        : Config(std::move(config))
        , Resources(std::move(resources))
        , Sdk(std::move(sdk))
        , Budget(Config.Polling.GlobalAdminConcurrency)
    {
    }

    void Bootstrap() {
        Become(&TThis::StateWork);
    }

private:
    void Read(TEvRuntime::TEvGatewayRead::TPtr& ev) {
        const auto request = ev->Get()->Request;
        if (Quiescing || !Resources->AdmissionOpen() || request.ProcessId != Resources->ProcessId() || request.Deadline <= TSkipperClock::Now() || !Budget.AcquireRead(request)) {
            TReadResult result;
            result.Request = request;
            result.Busy = true;
            result.Status = "READ_CAPACITY_OR_DEADLINE";
            result.FinishedAt = TSkipperClock::Now();
            Send(ev->Sender, new TEvRuntime::TEvGatewayReadResult(std::move(result)));
            return;
        }
        ReadRecipients.emplace(request.Id, ev->Sender);
        Resources->Counters->GetCounter("inflight_reads")->Set(Budget.Reads());
        Sdk->Read(request, [gate = Resources->CompletionGate, recipient = SelfId()](TReadResult result) {
            auto event = MakeHolder<TEvSdkReadComplete>();
            event->Result = std::move(result);
            gate->Send(recipient, event.Release());
        });
    }

    void ReadComplete(TEvSdkReadComplete::TPtr& ev) {
        auto result = std::move(ev->Get()->Result);
        if (!Budget.SettleRead(result.Request.Id)) {
            return;
        }
        Resources->Counters->GetCounter("inflight_reads")->Set(Budget.Reads());
        Resources->Counters->GetCounter("read_rpc_total", true)->Inc();
        Resources->Counters->GetCounter("read_rpc_bytes_total", true)->Add(result.ResponseBytes);
        Resources->Counters->GetCounter("read_rpc_errors_total", true)->Add(!result.Success);
        Resources->Counters->GetCounter("read_rpc_microseconds_total", true)->Add(result.FinishedAt >= result.StartedAt ? (result.FinishedAt - result.StartedAt).MicroSeconds() : 0);
        auto metrics = Resources->Counters->GetSubgroup("method", ReadMethodName(result.Request.Method))->GetSubgroup("status", MetricStatus(result.Status));
        metrics->GetCounter("skipper_rpc_total", true)->Inc();
        metrics->GetCounter("skipper_rpc_returned_payload_bytes_total", true)->Add(result.ResponseBytes);
        metrics->GetCounter("skipper_rpc_payload_measurements_total", true)->Add(result.ResponseBytes != 0);
        if (result.StartedAt && result.FinishedAt >= result.StartedAt) {
            metrics->GetHistogram("skipper_rpc_latency_milliseconds", NMonitoring::ExplicitHistogram({1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000, 10000, 30000, 120000}))->Collect((result.FinishedAt - result.StartedAt).MilliSeconds());
        }
        Resources->Log("read_rpc_result", {{"request_id", result.Request.Id}, {"endpoint", result.Request.Endpoint},
            {"method", ReadMethodName(result.Request.Method)}, {"status", MetricStatus(result.Status)}, {"response_bytes", ToString(result.ResponseBytes)}});
        const auto recipient = ReadRecipients.extract(result.Request.Id);
        if (result.Request.Method == EReadMethod::Admin && result.Request.OperationPriority
            && ((result.Success && result.Cluster) || (!result.Success && result.Status == "NOT_FOUND" && !result.Cluster)))
        {
            // Only operation preflights are retained: an ordinary poll of the
            // same endpoint must not replace the reply a permission names.
            // Existing Bridge API represents a pre-bridge cluster as NOT_FOUND.
            // Preserve this raw, correlated reply: the controller synthesizes
            // its attested single-pile baseline only in its own result copy.
            // ValidateDispatch permits this response exclusively for Convert.
            Preflights[result.Request.Endpoint] = result;
        }
        if (!recipient.empty()) {
            Send(recipient.mapped(), new TEvRuntime::TEvGatewayReadResult(std::move(result)));
        }
    }

    void Dispatch(TEvRuntime::TEvDispatchAttempt::TPtr& ev) {
        const auto& request = ev->Get()->Request;
        const auto snapshot = Resources->ReadSnapshot();
        std::optional<TReadResult> preflight;
        if (const auto it = Preflights.find(request.Attempt.Endpoint); it != Preflights.end()) {
            preflight = it->second;
        }
        TString error = snapshot ? ValidateLiveDispatch(request, *snapshot, Resources->ProcessId(), Resources->MemberId(), preflight,
            Resources->AdmissionOpen() && !Quiescing, Resources->StopLatch->IsStopped(), Config.Polling.DispatchMargin, TSkipperClock::Now()) : TString("NO_INSTALLED_SNAPSHOT");
        if (error.empty() && !Budget.AcquireMutation(request.Attempt.Id)) {
            error = "MUTATION_CAPACITY_OR_CLOSED";
        }
        if (!error.empty()) {
            TMutationResult result;
            result.AttemptId = request.Attempt.Id;
            result.ProcessId = Resources->ProcessId();
            result.NeverDispatched = request.Attempt.DesignatedProcess == Resources->ProcessId()
                && request.Attempt.DesignatedMember == Resources->MemberId() && Budget.CloseAttempt(request.Attempt.Id);
            if (result.NeverDispatched) {
                result.GatewayProof = "never_dispatched:" + Resources->ProcessId() + ":" + request.Attempt.Id;
            }
            result.Outcome.Status = error;
            Send(ev->Sender, new TEvRuntime::TEvMutationResult(std::move(result)));
            return;
        }
        MutationRecipients.emplace(request.Attempt.Id, ev->Sender);
        MutationTelemetry.emplace(request.Attempt.Id, request.Attempt);
        Resources->Counters->GetCounter("inflight_mutations")->Set(Budget.Mutations());
        TMutationResult base;
        base.AttemptId = request.Attempt.Id;
        base.ProcessId = Resources->ProcessId();
        base.HandedOff = true;
        base.HandoffAt = TSkipperClock::Now();
        if (request.Permission.EvidenceExpiry > base.HandoffAt) {
            base.EvidenceSlack = request.Permission.EvidenceExpiry - base.HandoffAt;
        }
        // Allocation/metrics work above may have raced independent stop or a
        // newer controller publication. Recheck immediately before SDK handoff.
        const auto latest = Resources->ReadSnapshot();
        const auto handoffAt = TSkipperClock::Now();
        const auto finalError = latest ? ValidateLiveDispatch(request, *latest, Resources->ProcessId(), Resources->MemberId(), preflight,
            Resources->AdmissionOpen() && !Quiescing, Resources->StopLatch->IsStopped(), Config.Polling.DispatchMargin, handoffAt) : TString("CURRENT_READINESS_CHANGED");
        if (!finalError.empty()) {
            Budget.WithdrawUnsentMutation(request.Attempt.Id);
            MutationRecipients.erase(request.Attempt.Id);
            MutationTelemetry.erase(request.Attempt.Id);
            Resources->Counters->GetCounter("inflight_mutations")->Set(Budget.Mutations());
            base.HandedOff = false;
            base.NeverDispatched = true;
            base.GatewayProof = "never_dispatched:" + Resources->ProcessId() + ":" + request.Attempt.Id;
            base.Outcome.Status = finalError;
            Send(ev->Sender, new TEvRuntime::TEvMutationResult(std::move(base)));
            return;
        }
        base.HandoffAt = handoffAt;
        base.EvidenceSlack = request.Permission.EvidenceExpiry > handoffAt ? request.Permission.EvidenceExpiry - handoffAt : TDuration::Zero();
        Sdk->Mutate(request.Attempt, [gate = Resources->CompletionGate, recipient = SelfId(), base](TRpcOutcome outcome) mutable {
            base.Outcome = std::move(outcome);
            base.FinishedAt = TSkipperClock::Now();
            auto event = MakeHolder<TEvSdkMutationComplete>();
            event->Result = std::move(base);
            gate->Send(recipient, event.Release());
        });
        base.Terminal = false;
        Send(ev->Sender, new TEvRuntime::TEvMutationResult(std::move(base)));
    }

    void MutationComplete(TEvSdkMutationComplete::TPtr& ev) {
        auto result = std::move(ev->Get()->Result);
        if (!Budget.SettleMutation(result.AttemptId)) {
            return;
        }
        Resources->Counters->GetCounter("inflight_mutations")->Set(Budget.Mutations());
        Resources->Counters->GetCounter("mutation_rpc_total", true)->Inc();
        Resources->Counters->GetCounter("mutation_rpc_errors_total", true)->Add(!result.Outcome.Success);
        Resources->Counters->GetCounter("dispatch_evidence_slack_microseconds_total", true)->Add(result.EvidenceSlack.MicroSeconds());
        const auto telemetry = MutationTelemetry.extract(result.AttemptId);
        if (!telemetry.empty()) {
            const auto& attempt = telemetry.mapped();
            auto metrics = Resources->Counters->GetSubgroup("method", MutationMethodName(attempt.Method))->GetSubgroup("status", MetricStatus(result.Outcome.Status));
            metrics->GetCounter("skipper_rpc_total", true)->Inc();
            if (result.FinishedAt >= result.HandoffAt) {
                const auto elapsed = result.FinishedAt - result.HandoffAt;
                Resources->Counters->GetCounter("mutation_rpc_microseconds_total", true)->Add(elapsed.MicroSeconds());
                metrics->GetHistogram("skipper_rpc_latency_milliseconds", NMonitoring::ExplicitHistogram({1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000, 10000, 30000, 120000, 600000}))->Collect(elapsed.MilliSeconds());
            }
            Resources->Counters->GetCounter("mutation_rpc_retained_issue_bytes_total", true)->Add(result.Outcome.Issue.size());
            Resources->Log("mutation_rpc_result", {{"operation_id", attempt.OperationId}, {"attempt_id", attempt.Id},
                {"endpoint", attempt.Endpoint}, {"method", MutationMethodName(attempt.Method)}, {"status", MetricStatus(result.Outcome.Status)}});
        }
        const auto recipient = MutationRecipients.extract(result.AttemptId);
        if (!recipient.empty()) {
            Send(recipient.mapped(), new TEvRuntime::TEvMutationResult(std::move(result)));
        }
    }

    void Close(TEvRuntime::TEvCloseAttempt::TPtr& ev) {
        TMutationResult result;
        result.AttemptId = ev->Get()->AttemptId;
        result.ProcessId = Resources->ProcessId();
        if (ev->Get()->ProcessId == Resources->ProcessId()) {
            result.NeverDispatched = Budget.CloseAttempt(result.AttemptId);
            if (result.NeverDispatched) {
                result.GatewayProof = "never_dispatched:" + result.ProcessId + ":" + result.AttemptId;
            }
        }
        Send(ev->Sender, new TEvRuntime::TEvMutationResult(std::move(result)));
    }

    void Quiesce(TEvRuntime::TEvGatewayQuiesce::TPtr& ev) {
        Quiescing = true;
        auto result = MakeHolder<TEvRuntime::TEvGatewayQuiesced>();
        result->Reads = Budget.Reads();
        result->Mutations = Budget.Mutations();
        Send(ev->Sender, result.Release());
    }

    STRICT_STFUNC(StateWork,
        hFunc(TEvRuntime::TEvGatewayRead, Read);
        hFunc(TEvRuntime::TEvDispatchAttempt, Dispatch);
        hFunc(TEvRuntime::TEvCloseAttempt, Close);
        hFunc(TEvRuntime::TEvGatewayQuiesce, Quiesce);
        hFunc(TEvSdkReadComplete, ReadComplete);
        hFunc(TEvSdkMutationComplete, MutationComplete);
        cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
    )

    TConfig Config;
    std::shared_ptr<TRuntimeResources> Resources;
    std::shared_ptr<ISdkGateway> Sdk;
    TGatewayBudget Budget;
    bool Quiescing = false;
    std::map<TString, NActors::TActorId> ReadRecipients;
    std::map<TString, NActors::TActorId> MutationRecipients;
    std::map<TString, TMutationAttempt> MutationTelemetry;
    std::map<TString, TReadResult> Preflights;
};

} // namespace

NYdb::TCommonClientSettings MakePinnedClientSettings(const TEndpoint& endpoint) {
    // DiscoveryEndpoint is copied verbatim to the SDK's gRPC locator. Passing
    // grpc:// here is not parsed as a connection string and never reaches the
    // configured node. IPv6 brackets and the exact SDK port are retained.
    const auto parsed = ParseEndpoint(endpoint.Uri);
    return NYdb::TCommonClientSettings().Database("")
        .DiscoveryEndpoint(Std(parsed.Host + ":" + ToString(parsed.Port)))
        .DiscoveryMode(NYdb::EDiscoveryMode::Off);
}

std::shared_ptr<ISdkGateway> CreateSdkGateway(const TConfig& config, const TLoadedCredentials& credentials) {
    return std::make_shared<TSdkGateway>(config, credentials);
}

TGatewayBudget::TGatewayBudget(size_t adminConcurrency)
    : AdminLimit(adminConcurrency)
{
}

bool TGatewayBudget::AcquireRead(const TReadRequest& request) {
    if (request.Id.empty() || ReadJobs.size() >= 128 || ReadJobs.contains(request.Id) || Methods.contains({request.Endpoint, request.Method})) {
        return false;
    }
    size_t same = 0;
    for (const auto& [id, job] : ReadJobs) {
        Y_UNUSED(id);
        same += job.Method == request.Method && (request.Method != EReadMethod::Admin || job.Bracket == request.Bracket);
    }
    const size_t limit = request.Method == EReadMethod::Admin && !request.Bracket ? (request.OperationPriority ? AdminLimit : AdminLimit - Min<size_t>(2, AdminLimit)) : request.Method == EReadMethod::Health || request.Bracket ? HealthLimit : 4;
    if (same >= limit) {
        return false;
    }
    ReadJobs.emplace(request.Id, request);
    Methods.emplace(request.Endpoint, request.Method);
    return true;
}

bool TGatewayBudget::SettleRead(const TString& requestId) {
    const auto job = ReadJobs.extract(requestId);
    if (job.empty()) {
        return false;
    }
    Methods.erase({job.mapped().Endpoint, job.mapped().Method});
    return true;
}

bool TGatewayBudget::AcquireMutation(const TString& attemptId) {
    if (attemptId.empty() || MutationJobs.size() >= 16 || HandedOff.contains(attemptId) || Closed.contains(attemptId)) {
        return false;
    }
    MutationJobs.insert(attemptId);
    HandedOff.insert(attemptId);
    return true;
}

bool TGatewayBudget::SettleMutation(const TString& attemptId) {
    return MutationJobs.erase(attemptId) != 0;
}

bool TGatewayBudget::WithdrawUnsentMutation(const TString& attemptId) {
    if (!MutationJobs.erase(attemptId)) {
        return false;
    }
    HandedOff.erase(attemptId);
    Closed.insert(attemptId);
    return true;
}

bool TGatewayBudget::CloseAttempt(const TString& attemptId) {
    if (attemptId.empty() || HandedOff.contains(attemptId)) {
        return false;
    }
    Closed.insert(attemptId);
    return true;
}

bool TGatewayBudget::IsClosed(const TString& attemptId) const {
    return Closed.contains(attemptId);
}

size_t TGatewayBudget::Reads() const {
    return ReadJobs.size();
}

size_t TGatewayBudget::Mutations() const {
    return MutationJobs.size();
}

NActors::IActor* CreateGatewayActor(const TConfig& config, std::shared_ptr<TRuntimeResources> resources, std::shared_ptr<ISdkGateway> sdk) {
    return new TYdbGatewayActor(config, std::move(resources), std::move(sdk));
}

TString ValidateLiveDispatch(const TDispatchRequest& request, const TControllerSnapshot& snapshot,
    const TString& processId, const TString& memberId, const std::optional<TReadResult>& preflight,
    bool admissionOpen, bool localStopped, TDuration margin, TInstant now)
{
    if (!snapshot.Applied || !snapshot.AuthorityReady || !snapshot.DispatchReady || snapshot.AdmissionUnknown) {
        return "CURRENT_READINESS_CHANGED";
    }
    if (request.Permission.Epoch != snapshot.Epoch) {
        return "READINESS_EPOCH_CHANGED";
    }
    if (snapshot.AdminValidUntil <= now) {
        return "ADMIN_QUORUM_EXPIRED";
    }
    const auto operation = snapshot.Applied->Operations.find(request.Attempt.OperationId);
    if (operation != snapshot.Applied->Operations.end() && operation->second.Plan.EvidenceBound
        && (!request.Permission.ConfirmationGeneration || request.Permission.ConfirmationGeneration != snapshot.ConfirmationGeneration))
    {
        return "CONFIRMATION_REVOKED";
    }
    if (operation != snapshot.Applied->Operations.end() && operation->second.Plan.Kind != EOperationKind::Failover
        && snapshot.ConfigValidUntil <= now)
    {
        return "CONFIG_EVIDENCE_EXPIRED";
    }
    if (snapshot.TopologyRevision != request.Attempt.TopologyRevision || !snapshot.Cluster
        || CanonicalFingerprint(*snapshot.Cluster) != CanonicalFingerprint(request.Attempt.Baseline))
    {
        return "CURRENT_TOPOLOGY_OR_STATE_CHANGED";
    }
    if (request.Permission.Automatic && !snapshot.AutomaticDispatchReady) {
        return "AUTO_PROFILE_UNQUALIFIED_OR_BLOCKED";
    }
    return ValidateDispatch(request, *snapshot.Applied, processId, memberId, preflight,
        admissionOpen, localStopped, margin, now);
}

TString ValidateDispatch(const TDispatchRequest& request, const TDurableState& installed,
    const TString& processId, const TString& memberId, const std::optional<TReadResult>& preflight,
    bool admissionOpen, bool localStopped, TDuration margin, TInstant now)
{
    const auto& attempt = request.Attempt;
    const auto& permission = request.Permission;
    if (!admissionOpen || localStopped || installed.RequestedMode == ERequestedMode::Observe || !installed.Ownership || !installed.Ownership->ExclusiveAdministration || installed.StateConflict) {
        return "DISPATCH_NOT_READY";
    }
    if (permission.Automatic && installed.RequestedMode != ERequestedMode::Auto) {
        return "AUTOMATIC_MODE_CHANGED";
    }
    if (attempt.DesignatedProcess != processId || attempt.DesignatedMember != memberId || permission.ProcessId != processId || permission.Id.empty()) {
        return "FOREIGN_PROCESS_OR_PERMISSION";
    }
    const auto found = installed.Attempts.find(attempt.Id);
    if (!installed.UnresolvedAttempt || *installed.UnresolvedAttempt != attempt.Id || found == installed.Attempts.end() || found->second.IntentIndex != attempt.IntentIndex || found->second.ArgumentsHash != attempt.ArgumentsHash || found->second.DesignatedProcess != processId || found->second.Endpoint != attempt.Endpoint || found->second.Disposition != EAttemptDisposition::IntentCommitted || attempt.IntentIndex == 0 || attempt.IntentIndex > installed.AppliedIndex || permission.IntentIndex != attempt.IntentIndex || permission.ArgumentsHash != attempt.ArgumentsHash || MutationArgumentsHash(attempt) != attempt.ArgumentsHash) {
        return "INTENT_NOT_INSTALLED";
    }
    const auto& stored = found->second;
    if (stored.OperationId != attempt.OperationId || stored.Step != attempt.Step || stored.WorkflowRevision != attempt.WorkflowRevision
            || stored.TopologyRevision != attempt.TopologyRevision || stored.NodeId != attempt.NodeId || stored.DesignatedMember != memberId
            || stored.Method != attempt.Method || stored.Update != attempt.Update || stored.Target != attempt.Target
            || CanonicalFingerprint(stored.Baseline) != CanonicalFingerprint(attempt.Baseline)) {
        return "IMMUTABLE_ATTEMPT_CHANGED";
    }
    const auto operation = installed.Operations.find(attempt.OperationId);
    if (operation == installed.Operations.end() || operation->second.Revision != attempt.WorkflowRevision + 1 || operation->second.Status != EOperationStatus::Executing) {
        return "WORKFLOW_CHANGED";
    }
    if (permission.ExpiresAt <= now || permission.ExpiresAt - now < margin || (permission.EvidenceExpiry != TInstant::Zero() && (permission.EvidenceExpiry <= now || permission.EvidenceExpiry - now < margin))) {
        return "EVIDENCE_EXPIRED";
    }
    const bool prebridge = preflight && attempt.Method == EBridgeMethod::Convert && preflight->Status == "NOT_FOUND" && !attempt.Baseline.Converted && attempt.Baseline.Generation == 0 && attempt.Baseline.Piles.size() == 1;
    if (!preflight || (!prebridge && (!preflight->Success || !preflight->Cluster || CanonicalFingerprint(*preflight->Cluster) != CanonicalFingerprint(attempt.Baseline))) || preflight->Request.Id != permission.PreflightRequestId || preflight->Request.ProcessId != processId || preflight->Request.Endpoint != attempt.Endpoint || preflight->Request.Method != EReadMethod::Admin || preflight->FinishedAt > now) {
        return "SAME_NODE_PREFLIGHT_REQUIRED";
    }
    return {};
}

} // namespace NBridgeSkipper
