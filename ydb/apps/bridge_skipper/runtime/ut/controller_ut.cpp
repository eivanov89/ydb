#include <ydb/apps/bridge_skipper/runtime/controller.h>
#include <ydb/apps/bridge_skipper/runtime/gateway.h>
#include <ydb/apps/bridge_skipper/http/http.h>

#include <ydb/public/api/protos/ydb_monitoring.pb.h>

#include <library/cpp/testing/unittest/registar.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>

#include <util/string/builder.h>
#include <util/generic/guid.h>
#include <util/folder/tempdir.h>
#include <ydb/library/actors/testlib/test_runtime.h>

#include <algorithm>

namespace NBridgeSkipper {
namespace {

class TControllerClock : public ITimeProvider {
public:
    TInstant Value = TInstant::Seconds(100);

    TInstant Now() override {
        return Value;
    }
};

struct THarness {
    TIntrusivePtr<TControllerClock> Clock = new TControllerClock;
    std::unique_ptr<TControllerCore> Core;
    TControllerOptions Settings;
    TClusterView View;
    std::deque<TReadRequest> Reads;
    std::deque<TStorageRequest> Stores;
    std::vector<TDispatchRequest> Dispatches;
    std::vector<TString> Closed;
    std::vector<TControllerCommandReply> Replies;
    std::vector<TControllerEffects::THandoffSample> Handoffs;
    ui64 Next = 1;
    bool Qualified = false;
    bool PeerFailed = false;
    bool ContradictoryEvidence = false;
    bool UnknownEvidence = false;
    std::map<TString, TReadResult> ReadResults;
    bool WrongIdentity = false;
    bool FailConfig = false;
    bool BusyPreflight = false;
    std::set<TString> FailedPreflights;
    std::set<TString> HealthUnavailable;
    std::map<TString, TString> ConfigOverrides;
    std::map<TString, std::pair<ui32, TString>> IdentityOverrides;

    explicit THarness(bool qualified = false, ERequestedMode mode = ERequestedMode::Manual)
        : Qualified(qualified)
    {
        TSkipperClock::RegisterProvider(Clock);
        View.Generation = 7;
        View.Converted = true;
        View.Piles = {{"a", EPileState::Primary}, {"b", EPileState::Synchronized}};
        View.PileOrder = {"a", "b"};
        TControllerOptions options;
        options.Config.ClusterId = "test-cluster";
        options.Config.YdbEndpoints = {ParseEndpoint("grpc://127.0.0.1:21001"), ParseEndpoint("grpc://127.0.0.1:21002")};
        options.Restored.ClusterId = options.Config.ClusterId;
        options.Restored.MemberId = "member";
        options.Restored.MembershipIncarnation = "membership";
        options.Restored.RequestedMode = mode;
        options.Restored.ExclusiveHistory = true;
        options.Restored.Ownership = TOwnershipAcknowledgement{true, true, "fixture-independent-site", "test-cluster", "local", "test", "", 1};
        options.MemberId = "member";
        options.ProcessId = "process";
        options.NextId = [this] { return TStringBuilder() << "id-" << Next++; };
        if (Qualified) {
            options.EvidenceQualification = {"global_bsc_v1", "unit-fixture-build", "unit-fixture-not-production", ESourceKind::GlobalBsc,
                true, true, false, false};
            options.EvidenceDecoder = [this](const TReadResult& read, const TClusterView& view) {
                std::map<TString, TAssessment> targets;
                for (const auto& [name, state] : view.Piles) {
                    (void)state;
                    const bool failed = PeerFailed && name == "b"
                        && (!ContradictoryEvidence || read.Request.Endpoint.EndsWith("21001"));
                    targets[name] = {UnknownEvidence ? EAssessment::Unknown
                        : (failed ? EAssessment::Failed : EAssessment::NoFailureReported), !UnknownEvidence, {}};
                }
                return targets;
            };
            options.NoReplayPathQualified = true;
            options.AcceptanceMarkersQualified = true;
            options.AutomaticFollowupQualified = true;
            options.AcceptanceProvenance = "explicit-unit-fixture-only";
        }
        Settings = options;
        Core = std::make_unique<TControllerCore>(std::move(options));
    }

    ~THarness() {
        TSkipperClock::RegisterProvider(CreateDefaultTimeProvider());
    }

    TString Yaml() const {
        // Actual mock FetchAllConfigs shape: labels differ from numeric dial
        // addresses; port is interconnect and must not become the SDK port.
        return R"(metadata: {kind: MainConfig, cluster: fixture, version: 1}
config:
  bridge_config: {piles: [{name: a}, {name: b}]}
  hosts:
    - {host: mock-a-1, node_id: 1, port: 19001, location: {bridge_pile_name: a}}
    - {host: mock-b-1, node_id: 2, port: 19001, location: {bridge_pile_name: b}}
)";
    }

    void ThreeNodeSurvivingPile() {
        Settings.Config.YdbEndpoints.push_back(ParseEndpoint("grpc://127.0.0.1:21003"));
        const TString config = R"(config:
  bridge_config: {piles: [{name: a}, {name: b}]}
  hosts:
    - {host: mock-a-1, node_id: 1, port: 19001, location: {bridge_pile_name: a}}
    - {host: mock-a-2, node_id: 2, port: 19001, location: {bridge_pile_name: a}}
    - {host: mock-a-3, node_id: 3, port: 19001, location: {bridge_pile_name: a}}
)";
        for (ui32 index = 0; index < 3; ++index) {
            const auto& endpoint = Settings.Config.YdbEndpoints[index].Uri;
            ConfigOverrides[endpoint] = config;
            IdentityOverrides[endpoint] = {index + 1, "a"};
        }
        Core = std::make_unique<TControllerCore>(Settings);
    }

    void Collect() {
        auto effects = Core->DrainEffects();
        for (auto& read : effects.Reads) {
            Reads.push_back(std::move(read));
        }
        for (auto& store : effects.Stores) {
            Stores.push_back(std::move(store));
        }
        for (auto& dispatch : effects.Dispatches) {
            Dispatches.push_back(std::move(dispatch));
        }
        Closed.insert(Closed.end(), effects.CloseAttempts.begin(), effects.CloseAttempts.end());
        Replies.insert(Replies.end(), effects.Replies.begin(), effects.Replies.end());
        Handoffs.insert(Handoffs.end(), effects.Handoffs.begin(), effects.Handoffs.end());
    }

    TReadResult Response(const TReadRequest& read) {
        const bool first = read.Endpoint.EndsWith("21001");
        const auto identity = IdentityOverrides.find(read.Endpoint);
        const ui32 id = identity == IdentityOverrides.end() ? (first ? 1 : 2) : identity->second.first;
        const TString pile = identity == IdentityOverrides.end() ? (first ? "a" : "b") : identity->second.second;
        TReadResult result;
        result.Request = read;
        result.Success = true;
        result.Status = "SUCCESS";
        result.StartedAt = Clock->Value;
        result.FinishedAt = Clock->Value;
        if (read.Method == EReadMethod::Config) {
            if (FailConfig) {
                result.Success = false;
                result.Status = "TRANSPORT_UNAVAILABLE";
                return result;
            }
            const auto config = ConfigOverrides.find(read.Endpoint);
            result.Configs = {config == ConfigOverrides.end() ? Yaml() : config->second};
        } else if (read.Method == EReadMethod::Admin) {
            if (read.OperationPriority && FailedPreflights.contains(read.Endpoint)) {
                result.Success = false;
                result.Busy = BusyPreflight;
                result.Status = BusyPreflight ? "READ_CAPACITY_OR_DEADLINE" : "TRANSPORT_UNAVAILABLE";
                return result;
            }
            result.Cluster = View;
            result.Cluster->Endpoint = read.Endpoint;
            result.Cluster->ReceivedAt = Clock->Value;
        } else if (read.Method == EReadMethod::Health) {
            if (HealthUnavailable.contains(read.Endpoint)) {
                result.Success = false;
                result.Status = "TRANSPORT_UNAVAILABLE";
                return result;
            }
            if (Settings.Config.EvidenceProfile == EEvidenceProfile::NodewardenLocalV1 && !read.IdentityOnly) {
                result.Success = false;
                result.Status = "UNSUPPORTED_DIAGNOSTIC_CAPABILITY";
                return result;
            }
            auto health = std::make_shared<Ydb::Monitoring::SelfCheckResult>();
            health->set_self_check_result(Ydb::Monitoring::SelfCheck::GOOD);
            auto* location = health->mutable_location();
            location->set_id(WrongIdentity && first ? 99 : id);
            location->set_host(first ? "mock-a-1" : "mock-b-1");
            location->mutable_pile()->set_name(pile);
            result.Health = health;
        }
        return result;
    }

    void CompleteRead(TReadRequest read) {
        auto response = Response(read);
        ReadResults[read.Id] = response;
        Core->OnRead(std::move(response));
        Collect();
    }

    void CompleteStore(TStorageRequest request) {
        TStorageResult result;
        result.Id = request.Id;
        result.Kind = request.Kind;
        result.Success = true;
        if (request.Kind == EStorageKind::Checkpoint) {
            Core->OnStorage(std::move(result));
            Collect();
            return;
        }
        UNIT_ASSERT(request.Kind == EStorageKind::Commit);
        const auto candidate = Apply(*request.State, request.Command);
        result.Proposal = {EProposalStatus::Committed, candidate.Result, candidate.State, {}};
        Core->OnStorage(std::move(result));
        Collect();
    }

    void Drain(bool stores = true) {
        Collect();
        size_t iterations = 0;
        while ((!Reads.empty() || (stores && !Stores.empty())) && iterations++ < 1000) {
            if (!Reads.empty()) {
                auto read = std::move(Reads.front());
                Reads.pop_front();
                CompleteRead(std::move(read));
            } else {
                auto store = std::move(Stores.front());
                Stores.pop_front();
                CompleteStore(std::move(store));
            }
        }
        UNIT_ASSERT_C(iterations < 1000, "controller must not produce polling/journal catch-up bursts");
    }

    void Bootstrap() {
        Core->Tick();
        Drain();
        UNIT_ASSERT(Core->Snapshot()->TopologyRevision > 0);
        UNIT_ASSERT(Core->Snapshot()->Cluster);
    }

    void Advance(ui64 seconds, bool stores = true) {
        Clock->Value += TDuration::Seconds(seconds);
        Core->Tick();
        Drain(stores);
    }

    TControllerSubmission Manual(EOperationKind kind = EOperationKind::Failover) {
        const auto snapshot = Core->Snapshot();
        TControllerSubmission submission;
        submission.Command.Id = TStringBuilder() << "client-command-" << Next++;
        submission.Command.RequestId = TStringBuilder() << "client-request-" << Next++;
        submission.Command.RequestHash = "canonical-http-body";
        submission.Command.Kind = ECommandKind::AdmitOperation;
        submission.Command.OperationId = TStringBuilder() << "operation-" << Next++;
        submission.Command.ExpectedControllerRevision = snapshot->Applied->ControllerRevision;
        submission.IncarnationId = snapshot->IncarnationId;
        submission.TopologyRevision = snapshot->TopologyRevision;
        submission.StateFingerprint = snapshot->Epoch.AdminFingerprint;
        TPlanRequest request;
        request.Kind = kind;
        request.ExternalPrerequisitesAcknowledged = true;
        request.FailedTargets = {"b"};
        request.Target = "b";
        submission.PlanRequest = request;
        return submission;
    }

    void Handoff(const TDispatchRequest& dispatch) {
        TMutationResult result;
        result.Terminal = false;
        result.HandedOff = true;
        result.AttemptId = dispatch.Attempt.Id;
        result.ProcessId = dispatch.Attempt.DesignatedProcess;
        result.HandoffAt = Clock->Value;
        if (dispatch.Permission.EvidenceExpiry > Clock->Value) {
            result.EvidenceSlack = dispatch.Permission.EvidenceExpiry - Clock->Value;
        }
        Core->OnMutation(std::move(result));
        Collect();
    }

    void Finish(const TDispatchRequest& dispatch, bool success = true) {
        TMutationResult result;
        result.AttemptId = dispatch.Attempt.Id;
        result.ProcessId = dispatch.Attempt.DesignatedProcess;
        result.HandedOff = true;
        result.Outcome.AttemptId = result.AttemptId;
        result.Outcome.DesignatedMember = dispatch.Attempt.DesignatedMember;
        result.Outcome.DesignatedProcess = result.ProcessId;
        result.Outcome.Success = success;
        result.Outcome.Status = success ? "SUCCESS" : "TIMEOUT";
        Core->OnMutation(std::move(result));
        Collect();
    }

    TControllerSubmission Mode(ERequestedMode mode) {
        auto submission = Manual();
        submission.PlanRequest.reset();
        submission.Command.Kind = ECommandKind::SetMode;
        submission.Command.Mode = mode;
        submission.Command.Reason = "operator-fixture";
        if (mode != ERequestedMode::Observe) {
            submission.Command.Ownership = TOwnershipAcknowledgement{true, true, "fixture-site", "test-cluster", "local", "", "", 0};
        }
        return submission;
    }

    TDispatchRequest StartSwitch(bool stores = true) {
        auto submission = Manual(EOperationKind::Switchover);
        submission.PlanRequest->Successor = View.Piles.at("a") == EPileState::Primary ? "b" : "a";
        Core->Submit(std::move(submission));
        Drain(stores);
        UNIT_ASSERT(!Dispatches.empty());
        return Dispatches.back();
    }

    void ObserveSwitch(const TDispatchRequest& dispatch) {
        Handoff(dispatch);
        const TString successor = *dispatch.Attempt.Target;
        for (auto& [name, state] : View.Piles) {
            state = name == successor ? EPileState::Primary : EPileState::Synchronized;
        }
        ++View.Generation;
        Advance(1);
        UNIT_ASSERT(Core->Snapshot()->Applied->Attempts.at(dispatch.Attempt.Id).Accepted);
    }

    void Restart() {
        Settings.Restored = *Core->Snapshot()->Applied;
        Settings.ProcessId = "new-process";
        Core = std::make_unique<TControllerCore>(Settings);
        Reads.clear();
        Stores.clear();
        Dispatches.clear();
        Closed.clear();
        Replies.clear();
        Bootstrap();
    }
};

class TDispatchProbeSdk final : public ISdkGateway {
public:
    std::vector<TReadRequest> Reads;
    std::vector<std::function<void(TReadResult)>> ReadCallbacks;
    std::vector<TMutationAttempt> Mutations;

    void Read(TReadRequest request, std::function<void(TReadResult)> callback) override {
        Reads.push_back(std::move(request));
        ReadCallbacks.push_back(std::move(callback));
    }

    void Mutate(TMutationAttempt attempt, std::function<void(TRpcOutcome)>) override {
        Mutations.push_back(std::move(attempt));
    }

    void Stop() override {
    }
};

struct TDispatchProbe {
    TTempDir Directory;
    std::shared_ptr<TRuntimeResources> Resources;
    std::shared_ptr<TDispatchProbeSdk> Sdk = std::make_shared<TDispatchProbeSdk>();
    NActors::TTestActorRuntimeBase Runtime;
    NActors::TActorId Gateway;
    NActors::TActorId Edge;

    explicit TDispatchProbe(const TConfig& config)
        : Resources(std::make_shared<TRuntimeResources>(config, Directory.Name(), "member"))
    {
        Runtime.Initialize();
        Resources->CompletionGate->Attach(Runtime.GetActorSystem(0));
        Gateway = Runtime.Register(CreateGatewayActor(config, Resources, Sdk));
        Edge = Runtime.AllocateEdgeActor();
        NActors::TDispatchOptions bootstrap;
        bootstrap.FinalEvents.emplace_back([gateway = Gateway](NActors::IEventHandle& event) {
            return event.GetRecipientRewrite() == gateway && event.GetTypeRewrite() == NActors::TEvents::TSystem::Bootstrap;
        });
        Runtime.DispatchEvents(bootstrap);
    }

    ~TDispatchProbe() {
        Resources->CompletionGate->Close();
        Resources->StopStorage();
    }

    void CachePreflight(TReadResult result) {
        auto request = result.Request;
        Runtime.SendAsync(new NActors::IEventHandle(Gateway, Edge, new TEvRuntime::TEvGatewayRead(request)));
        request.Id = TGUID::Create().AsUuidString();
        Runtime.SendAsync(new NActors::IEventHandle(Gateway, Edge, new TEvRuntime::TEvGatewayRead(request)));
        UNIT_ASSERT(Runtime.GrabEdgeEvent<TEvRuntime::TEvGatewayReadResult>(Edge)->Get()->Result.Busy);
        Sdk->ReadCallbacks.back()(std::move(result));
        UNIT_ASSERT(Runtime.GrabEdgeEvent<TEvRuntime::TEvGatewayReadResult>(Edge)->Get()->Result.Success);
    }

    TMutationResult Dispatch(const TDispatchRequest& request) {
        Runtime.SendAsync(new NActors::IEventHandle(Gateway, Edge, new TEvRuntime::TEvDispatchAttempt(request)));
        auto response = Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(Edge);
        return std::move(response->Get()->Result);
    }
};

TApiAction HttpPost(THttpApi& api, const THarness& harness, TString target, const TApiJson& body) {
    const auto snapshot = harness.Core->Snapshot();
    TLocalStopReceipt stop;
    stop.ProcessId = snapshot->IncarnationId;
    stop.Revision = snapshot->LocalStopRevision;
    stop.Stopped = snapshot->LocalStopped;
    return api.Handle({"POST", std::move(target), "application/json", CanonicalApiJson(body)}, *snapshot, stop);
}

TApiJson AdmissionBody(const TApiJson& preview) {
    TApiJson body(NJson::JSON_MAP);
    body["preview"] = preview["preview"];
    body["request_id"] = TGUID::Create().AsUuidString();
    TApiJson acknowledgements(NJson::JSON_MAP);
    acknowledgements["confirmed"] = preview["required_acknowledgements"];
    acknowledgements["typed_pile_name"] = TApiJson(NJson::JSON_NULL);
    acknowledgements["empty_name"] = false;
    acknowledgements["deletion"] = false;
    body["acknowledgements"] = std::move(acknowledgements);
    return body;
}

} // namespace

Y_UNIT_TEST_SUITE(TBridgeSkipperController) {
    Y_UNIT_TEST(BootstrapUsesMockInventoryAndResponderIdentityWithoutInventedPorts) {
        THarness harness;
        harness.Bootstrap();
        const auto snapshot = harness.Core->Snapshot();
        UNIT_ASSERT_VALUES_EQUAL(snapshot->Topology.Nodes.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(snapshot->Topology.Nodes[0].NodeId, 1);
        UNIT_ASSERT_VALUES_EQUAL(snapshot->Topology.Nodes[0].Host, "mock-a-1");
        UNIT_ASSERT_VALUES_EQUAL(snapshot->Topology.Nodes[0].Endpoint, "grpc://127.0.0.1:21001");
        UNIT_ASSERT(!snapshot->AutonomyReady);
        UNIT_ASSERT(std::find(snapshot->Blockers.begin(), snapshot->Blockers.end(), "UNQUALIFIED_EVIDENCE_PROFILE") != snapshot->Blockers.end());
        UNIT_ASSERT(harness.Dispatches.empty());
    }

    Y_UNIT_TEST(ManualIntentMustCommitBeforeOneUseSendGrant) {
        THarness harness;
        harness.Bootstrap();
        auto submission = harness.Manual();
        harness.Core->Submit(submission);
        harness.Drain(false);
        UNIT_ASSERT(harness.Dispatches.empty());
        UNIT_ASSERT_VALUES_EQUAL(harness.Stores.size(), 1);
        auto operation = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        UNIT_ASSERT(operation.Command.Kind == ECommandKind::AdmitOperation);
        harness.CompleteStore(std::move(operation));
        harness.Drain(false);
        UNIT_ASSERT(harness.Dispatches.empty());
        UNIT_ASSERT_VALUES_EQUAL(harness.Stores.size(), 1);
        auto intent = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        UNIT_ASSERT(intent.Command.Kind == ECommandKind::AdmitAttempt);
        const auto intentId = intent.Command.Attempt.Id;
        harness.CompleteStore(std::move(intent));
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.front().Attempt.Id, intentId);
        UNIT_ASSERT(harness.Dispatches.front().Permission.IntentIndex > 0);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.front().Permission.PreflightRequestId.empty(), false);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->UnresolvedAttempt == intentId);
    }

    Y_UNIT_TEST(ManualPreflightRotatesAfterFailureOrBusyBeforeAnyIntent) {
        for (const bool busy : {false, true}) {
            THarness harness;
            harness.ThreeNodeSurvivingPile();
            harness.Bootstrap();
            harness.FailedPreflights.insert("grpc://127.0.0.1:21001");
            harness.BusyPreflight = busy;
            harness.Core->Submit(harness.Manual());
            harness.Drain();
            UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.front().Attempt.NodeId, 2);
            UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.front().Attempt.Endpoint, "grpc://127.0.0.1:21002");
            UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Applied->Attempts.size(), 1);
            // A late recovery of the lowest node cannot move an admitted intent.
            harness.FailedPreflights.clear();
            harness.Advance(1);
            UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Applied->Attempts.begin()->second.NodeId, 2);
        }
    }

    Y_UNIT_TEST(ExhaustedPreflightAlternativesWaitForTickAndRetryReleasedPollSlots) {
        THarness harness;
        harness.ThreeNodeSurvivingPile();
        harness.Bootstrap();
        for (const auto& endpoint : harness.Settings.Config.YdbEndpoints) {
            harness.FailedPreflights.insert(endpoint.Uri);
        }
        harness.Core->Submit(harness.Manual());
        harness.Drain();
        UNIT_ASSERT(harness.Reads.empty());
        UNIT_ASSERT(harness.Stores.empty());
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Attempts.empty());
        UNIT_ASSERT(harness.Dispatches.empty());
        harness.FailedPreflights.erase("grpc://127.0.0.1:21002");
        harness.Advance(1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.front().Attempt.NodeId, 2);
    }

    Y_UNIT_TEST(LifecycleConfigMustRemainFreshAcrossPreflightAndIntentCommit) {
        for (const bool duringCommit : {false, true}) {
            THarness harness;
            harness.Bootstrap();
            harness.FailConfig = true;
            harness.Advance(59);
            UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->ConfigValidUntil, TInstant::Seconds(160));
            harness.Core->Submit(harness.Manual(EOperationKind::Takedown));
            harness.Drain(false);
            auto operation = std::move(harness.Stores.front());
            harness.Stores.pop_front();
            harness.CompleteStore(std::move(operation));
            UNIT_ASSERT_VALUES_EQUAL(harness.Reads.size(), 1);
            if (!duringCommit) {
                harness.Clock->Value += TDuration::Seconds(2);
            }
            auto preflight = std::move(harness.Reads.front());
            harness.Reads.pop_front();
            harness.CompleteRead(std::move(preflight));
            if (duringCommit) {
                UNIT_ASSERT_VALUES_EQUAL(harness.Stores.size(), 1);
                auto intent = std::move(harness.Stores.front());
                harness.Stores.pop_front();
                harness.Clock->Value += TDuration::Seconds(2);
                harness.CompleteStore(std::move(intent));
                UNIT_ASSERT_VALUES_EQUAL(harness.Closed.size(), 1);
                UNIT_ASSERT(harness.Core->Snapshot()->Applied->UnresolvedAttempt);
            } else {
                UNIT_ASSERT(harness.Stores.empty());
                UNIT_ASSERT(harness.Core->Snapshot()->Applied->Attempts.empty());
            }
            UNIT_ASSERT(harness.Dispatches.empty());
        }
    }

    Y_UNIT_TEST(EmergencyMayUseStaleUncontradictedConfigWithFreshAdminQuorum) {
        THarness harness;
        harness.Bootstrap();
        harness.FailConfig = true;
        harness.Advance(61);
        UNIT_ASSERT(harness.Core->Snapshot()->ConfigStale);
        UNIT_ASSERT(harness.Core->Snapshot()->MutationReady);
        harness.Core->Submit(harness.Manual());
        harness.Drain();
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        UNIT_ASSERT(harness.Dispatches.front().Attempt.Update.at("b") == EPileState::Disconnected);
    }

    Y_UNIT_TEST(UnsupportedAdminViewNeedsNewSupportedQuorumToRecoverWithoutRestart) {
        THarness harness;
        harness.ThreeNodeSurvivingPile();
        harness.Bootstrap();
        harness.Clock->Value += TDuration::Seconds(1);
        harness.Core->Tick();
        harness.Collect();
        const auto invalid = std::find_if(harness.Reads.begin(), harness.Reads.end(), [](const auto& read) {
            return read.Method == EReadMethod::Admin && read.Endpoint.EndsWith("21001");
        });
        UNIT_ASSERT(invalid != harness.Reads.end());
        auto read = std::move(*invalid);
        harness.Reads.erase(invalid);
        auto unsupported = harness.Response(read);
        unsupported.Cluster->Piles["b"] = static_cast<EPileState>(123);
        unsupported.Success = false;
        unsupported.Status = "UNSUPPORTED_BRIDGE_RESULT";
        harness.Core->OnRead(std::move(unsupported));
        harness.Drain();
        UNIT_ASSERT(harness.Core->Snapshot()->UnsupportedCluster);
        UNIT_ASSERT(!harness.Core->Snapshot()->MutationReady);
        harness.Clock->Value += TDuration::Seconds(1);
        harness.Core->Tick();
        harness.Collect();
        const auto restored = std::find_if(harness.Reads.begin(), harness.Reads.end(), [](const auto& request) {
            return request.Method == EReadMethod::Admin && request.Endpoint.EndsWith("21001");
        });
        UNIT_ASSERT(restored != harness.Reads.end());
        read = std::move(*restored);
        harness.Reads.erase(restored);
        harness.CompleteRead(std::move(read));
        UNIT_ASSERT(harness.Core->Snapshot()->UnsupportedCluster);
        UNIT_ASSERT(!harness.Core->Snapshot()->MutationReady);
        harness.Drain();
        const auto recovered = harness.Core->Snapshot();
        UNIT_ASSERT(!recovered->UnsupportedCluster);
        UNIT_ASSERT(recovered->MutationReady);
        auto manual = harness.Manual();
        UNIT_ASSERT(PlanWorkflow(*recovered->Cluster, *manual.PlanRequest).Plan);
        harness.Core->Submit(std::move(manual));
        harness.Drain();
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
    }

    Y_UNIT_TEST(AdmissionUnknownPublishesNoCandidateAndLookupPrecedesEffect) {
        THarness harness;
        harness.Bootstrap();
        const auto installed = harness.Core->Snapshot()->Applied;
        auto submission = harness.Manual();
        harness.Core->Submit(submission);
        harness.Drain(false);
        auto pending = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        TStorageResult unknown;
        unknown.Id = pending.Id;
        unknown.Kind = pending.Kind;
        unknown.Proposal.Status = EProposalStatus::Unknown;
        harness.Core->OnStorage(unknown);
        harness.Collect();
        UNIT_ASSERT(harness.Core->Snapshot()->Applied == installed);
        UNIT_ASSERT(harness.Core->Snapshot()->AdmissionUnknown);
        UNIT_ASSERT(harness.Dispatches.empty());
        harness.Core->Tick();
        harness.Collect();
        UNIT_ASSERT_VALUES_EQUAL(harness.Stores.size(), 1);
        auto lookup = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        UNIT_ASSERT(lookup.Kind == EStorageKind::ResolveUnknown);
        UNIT_ASSERT_VALUES_EQUAL(lookup.LookupId, pending.Command.Id);
        const auto candidate = Apply(*pending.State, pending.Command);
        TStorageResult resolved;
        resolved.Id = lookup.Id;
        resolved.Kind = lookup.Kind;
        resolved.Success = true;
        resolved.Proposal = {EProposalStatus::Committed, candidate.Result, candidate.State, {}};
        harness.Core->OnStorage(std::move(resolved));
        harness.Drain();
        UNIT_ASSERT(!harness.Core->Snapshot()->AdmissionUnknown);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
    }

    Y_UNIT_TEST(DuplicateReceiptPrecedesRestartPreviewBindings) {
        THarness harness;
        harness.Bootstrap();
        auto submission = harness.Manual();
        harness.Core->Submit(submission);
        harness.Drain();
        const auto receipt = harness.Replies.front().Result.Result;
        submission.IncarnationId = "expired-process";
        submission.TopologyRevision = 999;
        submission.StateFingerprint = "expired-preview";
        harness.Core->Submit(submission);
        harness.Collect();
        UNIT_ASSERT(harness.Replies.back().Result.Result == receipt);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
    }

    Y_UNIT_TEST(LocalStopDuringIntentCommitRequiresGatewayNegativeClosure) {
        THarness harness;
        harness.Bootstrap();
        harness.Core->Submit(harness.Manual());
        harness.Drain(false);
        auto operation = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        harness.CompleteStore(std::move(operation));
        harness.Drain(false);
        auto intent = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        const auto attemptId = intent.Command.Attempt.Id;
        harness.Core->SetLocalStop(true, 1);
        harness.CompleteStore(std::move(intent));
        UNIT_ASSERT(harness.Dispatches.empty());
        UNIT_ASSERT_VALUES_EQUAL(harness.Closed.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Closed.front(), attemptId);
        TMutationResult closed;
        closed.AttemptId = attemptId;
        closed.ProcessId = "process";
        closed.NeverDispatched = true;
        closed.GatewayProof = "live-gateway-closed-before-send";
        harness.Core->OnMutation(std::move(closed));
        harness.Drain();
        UNIT_ASSERT(!harness.Core->Snapshot()->Applied->UnresolvedAttempt);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Operations.begin()->second.Status == EOperationStatus::Paused);
    }

    Y_UNIT_TEST(QualifiedTwoRoundsDriveAutomaticDurableAdmission) {
        THarness harness(true, ERequestedMode::Auto);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Advance(1);
        harness.Advance(12);
        UNIT_ASSERT(harness.Dispatches.empty());
        UNIT_ASSERT(harness.Core->Snapshot()->Decision);
        UNIT_ASSERT(!harness.Core->Snapshot()->Decision->Confirmed);
        harness.Advance(3);
        harness.Advance(12);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        UNIT_ASSERT(harness.Dispatches.front().Permission.Automatic);
        UNIT_ASSERT(harness.Dispatches.front().Attempt.Update.at("b") == EPileState::Disconnected);
    }

    Y_UNIT_TEST(ProductionUnqualifiedProfileNeverDispatchesAuto) {
        THarness harness(false, ERequestedMode::Auto);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Advance(1);
        harness.Advance(12);
        harness.Advance(3);
        harness.Advance(12);
        UNIT_ASSERT(harness.Dispatches.empty());
        UNIT_ASSERT(harness.Core->Snapshot()->Decision);
        UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Decision->Reason, "UNQUALIFIED_EVIDENCE_PROFILE");
    }

    Y_UNIT_TEST(UnavailableNodewardenProfileKeepsManualBootstrapWithoutRelabelingSelfCheckVotes) {
        THarness harness;
        harness.Settings.Config.EvidenceProfile = EEvidenceProfile::NodewardenLocalV1;
        harness.Core = std::make_unique<TControllerCore>(harness.Settings);
        harness.Bootstrap();
        const auto snapshot = harness.Core->Snapshot();
        UNIT_ASSERT(snapshot->MutationReady);
        UNIT_ASSERT(!snapshot->AutonomyReady);
        UNIT_ASSERT(std::find(snapshot->Blockers.begin(), snapshot->Blockers.end(), "UNSUPPORTED_DIAGNOSTIC_CAPABILITY") != snapshot->Blockers.end());
        bool identityDiagnostic = false;
        for (const auto& diagnostic : snapshot->Diagnostics) {
            UNIT_ASSERT(diagnostic.Source == ESourceKind::Unsupported);
            identityDiagnostic = identityDiagnostic || diagnostic.Profile == "selfcheck_identity_v1";
            for (const auto& [target, assessment] : diagnostic.Targets) {
                (void)target;
                UNIT_ASSERT(assessment.Value == EAssessment::Unknown);
            }
        }
        UNIT_ASSERT(identityDiagnostic);
        harness.Core->Submit(harness.Manual());
        harness.Drain();
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        UNIT_ASSERT(!harness.Dispatches.front().Permission.Automatic);
    }

    Y_UNIT_TEST(MismatchingResponderInvalidatesPreviouslyVerifiedInventory) {
        THarness harness;
        harness.Bootstrap();
        const auto epoch = harness.Core->Snapshot()->Epoch;
        harness.WrongIdentity = true;
        harness.Advance(1);
        const auto snapshot = harness.Core->Snapshot();
        UNIT_ASSERT(snapshot->Epoch.ReadinessGeneration > epoch.ReadinessGeneration);
        const auto found = std::find_if(snapshot->Topology.Nodes.begin(), snapshot->Topology.Nodes.end(), [](const auto& node) {
            return node.InventoryNodeId == 1;
        });
        UNIT_ASSERT(found != snapshot->Topology.Nodes.end());
        UNIT_ASSERT_VALUES_EQUAL(found->NodeId, 0);
        UNIT_ASSERT(harness.Dispatches.empty());
    }

    Y_UNIT_TEST(ConfiguredReaddressDropsRetainedResponderIdentity) {
        THarness harness;
        harness.Settings.Config.Polling.ConfigInterval = TDuration::Seconds(1);
        harness.Core = std::make_unique<TControllerCore>(harness.Settings);
        harness.Bootstrap();
        auto node = [&] {
            const auto snapshot = harness.Core->Snapshot();
            const auto found = std::find_if(snapshot->Topology.Nodes.begin(), snapshot->Topology.Nodes.end(), [](const auto& node) {
                return node.InventoryNodeId == 1;
            });
            UNIT_ASSERT(found != snapshot->Topology.Nodes.end());
            return *found;
        };
        const TString old = "grpc://127.0.0.1:21001";
        const TString moved = "grpc://127.0.0.1:21011";
        UNIT_ASSERT_VALUES_EQUAL(node().Endpoint, old);
        UNIT_ASSERT_VALUES_EQUAL(node().NodeId, 1);
        const TString config = R"(config:
  bridge_config: {piles: [{name: a}, {name: b}]}
  hosts:
    - {host: "grpc://127.0.0.1:21011", node_id: 1, port: 19001, location: {bridge_pile_name: a}}
    - {host: mock-b-1, node_id: 2, port: 19001, location: {bridge_pile_name: b}}
)";
        for (const auto& endpoint : {old, TString("grpc://127.0.0.1:21002"), moved}) {
            harness.ConfigOverrides[endpoint] = config;
        }
        harness.HealthUnavailable = {old, moved};
        for (unsigned i = 0; i < 20; ++i) {
            harness.Advance(1);
            UNIT_ASSERT_VALUES_UNEQUAL(node().Endpoint, old);
        }
        UNIT_ASSERT_VALUES_EQUAL(node().Endpoint, moved);
        UNIT_ASSERT_VALUES_EQUAL(node().NodeId, 0);
        harness.HealthUnavailable.erase(moved);
        harness.IdentityOverrides[moved] = {1, "a"};
        for (unsigned i = 0; i < 20 && !node().NodeId; ++i) {
            harness.Advance(1);
        }
        UNIT_ASSERT_VALUES_EQUAL(node().Endpoint, moved);
        UNIT_ASSERT_VALUES_EQUAL(node().NodeId, 1);
    }

    Y_UNIT_TEST(ConfiguredReaddressDropsEveryVerifiedAliasOfNode) {
        THarness harness;
        const TString old = "grpc://127.0.0.1:21001";
        const TString alias = "grpc://127.0.0.1:21003";
        const TString moved = "grpc://127.0.0.1:21011";
        harness.Settings.Config.Polling.ConfigInterval = TDuration::Seconds(1);
        harness.Settings.Config.YdbEndpoints.push_back(ParseEndpoint(alias));
        harness.IdentityOverrides[alias] = {1, "a"};
        harness.Core = std::make_unique<TControllerCore>(harness.Settings);
        harness.Bootstrap();
        auto node = [&] {
            const auto snapshot = harness.Core->Snapshot();
            const auto found = std::find_if(snapshot->Topology.Nodes.begin(), snapshot->Topology.Nodes.end(), [](const auto& node) {
                return node.InventoryNodeId == 1;
            });
            UNIT_ASSERT(found != snapshot->Topology.Nodes.end());
            return *found;
        };
        UNIT_ASSERT_VALUES_EQUAL(node().NodeId, 1);
        const TString config = R"(config:
  bridge_config: {piles: [{name: a}, {name: b}]}
  hosts:
    - {host: "grpc://127.0.0.1:21011", node_id: 1, port: 19001, location: {bridge_pile_name: a}}
    - {host: mock-b-1, node_id: 2, port: 19001, location: {bridge_pile_name: b}}
)";
        for (const auto& endpoint : {old, TString("grpc://127.0.0.1:21002"), alias, moved}) {
            harness.ConfigOverrides[endpoint] = config;
        }
        harness.HealthUnavailable = {old, alias, moved};
        for (unsigned i = 0; i < 20; ++i) {
            harness.Advance(1);
        }
        UNIT_ASSERT_VALUES_EQUAL(node().Endpoint, moved);
        UNIT_ASSERT_VALUES_EQUAL(node().NodeId, 0);
    }

    Y_UNIT_TEST(MembershipRefreshRequiresMatchingFrozenQuorumsBeforeExactEmptyNameAdd) {
        for (const bool configFirst : {false, true}) {
            for (const bool sameGeneration : {false, true}) {
                THarness harness;
                harness.Settings.Config.YdbEndpoints.push_back(ParseEndpoint("grpc://127.0.0.1:21003"));
                harness.Settings.Config.Polling.ConfigInterval = TDuration::Seconds(1);
                const TString hosts = R"(
  hosts:
    - {host: mock-a-1, node_id: 1, port: 19001, location: {bridge_pile_name: a}}
    - {host: mock-a-2, node_id: 2, port: 19001, location: {bridge_pile_name: a}}
    - {host: mock-a-3, node_id: 3, port: 19001, location: {bridge_pile_name: a}}
)";
                const TString oldConfig = "config:\n  bridge_config: {piles: [{name: a}]}" + hosts;
                const TString newConfig = "config:\n  bridge_config: {piles: [{name: a}, {name: b}, {name: \"\"}]}" + hosts;
                for (ui32 index = 0; index < 3; ++index) {
                    const auto& endpoint = harness.Settings.Config.YdbEndpoints[index].Uri;
                    harness.ConfigOverrides[endpoint] = oldConfig;
                    harness.IdentityOverrides[endpoint] = {index + 1, "a"};
                }
                harness.View.Piles.erase("b");
                harness.View.PileOrder = {"a"};
                harness.Core = std::make_unique<TControllerCore>(harness.Settings);
                harness.Bootstrap();
                const auto before = harness.Core->Snapshot();
                UNIT_ASSERT(before->MutationReady);
                UNIT_ASSERT_VALUES_EQUAL(before->Topology.Nodes.size(), 3);
                UNIT_ASSERT_VALUES_EQUAL(VerifiedInventory(before->Topology.Nodes).size(), 3);
                auto expandAdmin = [&] {
                    harness.View.Piles["b"] = EPileState::Inactive;
                    harness.View.Piles[""] = EPileState::Inactive;
                    harness.View.PileOrder = {"a", "b", ""};
                    if (!sameGeneration) {
                        ++harness.View.Generation;
                    }
                };
                if (!configFirst) {
                    expandAdmin();
                    harness.Advance(1);
                    UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->TopologyRevision, before->TopologyRevision);
                    UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Cluster->Piles.size(), 1);
                    UNIT_ASSERT(!harness.Core->Snapshot()->MutationReady);
                }
                for (auto& [endpoint, config] : harness.ConfigOverrides) {
                    (void)endpoint;
                    config = newConfig;
                }
                harness.Clock->Value += TDuration::Seconds(1);
                harness.Core->Tick();
                harness.Collect();
                const auto configRead = std::find_if(harness.Reads.begin(), harness.Reads.end(), [](const auto& read) {
                    return read.Method == EReadMethod::Config;
                });
                UNIT_ASSERT(configRead != harness.Reads.end());
                auto firstConfig = std::move(*configRead);
                harness.Reads.erase(configRead);
                harness.CompleteRead(std::move(firstConfig));
                // One changed config cannot replace the frozen 3-node sample's
                // q=2 contract or be stitched with the retained old body.
                UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->TopologyRevision, before->TopologyRevision);
                UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Cluster->Piles.size(), 1);
                UNIT_ASSERT(!harness.Core->Snapshot()->MutationReady);
                harness.Drain();
                if (configFirst) {
                    UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->TopologyRevision, before->TopologyRevision);
                    UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Cluster->Piles.size(), 1);
                    UNIT_ASSERT(!harness.Core->Snapshot()->MutationReady);
                    expandAdmin();
                    harness.Advance(1);
                }
                const auto refreshed = harness.Core->Snapshot();
                UNIT_ASSERT_VALUES_EQUAL(refreshed->TopologyRevision, before->TopologyRevision + 1);
                UNIT_ASSERT(refreshed->MutationReady);
                UNIT_ASSERT(refreshed->Cluster->Piles.at("") == EPileState::Inactive);
                UNIT_ASSERT_VALUES_EQUAL(refreshed->Topology.ConfiguredPiles.size(), 3);
                auto add = harness.Manual(EOperationKind::AddPile);
                add.PlanRequest->Target = "";
                harness.Core->Submit(std::move(add));
                harness.Drain();
                UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
                UNIT_ASSERT(harness.Dispatches.front().Attempt.Method == EBridgeMethod::AddPile);
                UNIT_ASSERT(harness.Dispatches.front().Attempt.Target == std::optional<TString>(""));
            }
        }
    }

    Y_UNIT_TEST(HeartbeatDoesNotWriteIdleHealthSnapshotsToJournal) {
        THarness harness;
        harness.Bootstrap();
        const auto index = harness.Core->Snapshot()->Applied->AppliedIndex;
        for (size_t i = 0; i < 8; ++i) {
            harness.Advance(1);
        }
        UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Applied->AppliedIndex, index);
        UNIT_ASSERT(harness.Core->Snapshot()->SnapshotRevision > index);
    }

    Y_UNIT_TEST(LateForeignReadAndStorageRepliesCannotInstallState) {
        THarness harness;
        harness.Core->Tick();
        harness.Collect();
        const auto before = harness.Core->Snapshot()->Applied;
        auto fake = harness.Response(harness.Reads.front());
        fake.Request.ProcessId = "other-process";
        harness.Core->OnRead(std::move(fake));
        TStorageResult storage;
        storage.Id = "unknown-storage-job";
        storage.Success = true;
        storage.Proposal.Status = EProposalStatus::Committed;
        storage.Proposal.State = TDurableState{};
        harness.Core->OnStorage(std::move(storage));
        UNIT_ASSERT(harness.Core->Snapshot()->Applied == before);
    }

    Y_UNIT_TEST(RestartPreservesModeAndUnknownIntentWithoutMintingDispatchGrant) {
        THarness harness;
        harness.Bootstrap();
        harness.Core->Submit(harness.Manual());
        harness.Drain();
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        const auto old = harness.Dispatches.front();
        harness.Restart();
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->UnresolvedAttempt == old.Attempt.Id);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->RequestedMode == ERequestedMode::Manual);
        UNIT_ASSERT(harness.Dispatches.empty());
        harness.Finish(old);
        harness.Drain();
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->UnresolvedAttempt == old.Attempt.Id);
        UNIT_ASSERT(harness.Dispatches.empty());
    }

    Y_UNIT_TEST(ManualDecisionApplicationRetainsEvidenceAndPinnedCandidate) {
        THarness harness(true);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Advance(1);
        harness.Advance(12);
        harness.Advance(3);
        harness.Advance(12);
        const auto decision = *harness.Core->Snapshot()->Decision;
        UNIT_ASSERT(decision.Confirmed);
        auto submission = harness.Manual();
        submission.DecisionId = decision.Id;
        harness.Core->Submit(submission);
        harness.Drain();
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        const auto& dispatch = harness.Dispatches.front();
        UNIT_ASSERT(!dispatch.Permission.Automatic);
        UNIT_ASSERT_VALUES_EQUAL(dispatch.Attempt.Endpoint, decision.Endpoint->Endpoint);
        UNIT_ASSERT_VALUES_EQUAL(dispatch.Permission.EvidenceExpiry, decision.EvidenceExpiry);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Operations.at(dispatch.Attempt.OperationId).Plan.EvidenceBound);
    }

    Y_UNIT_TEST(RevokedConfirmationCannotAdmitPreflightOrDispatchDelayedIntent) {
        for (const bool automatic : {false, true}) {
            for (const bool beforeIntent : {false, true}) {
                THarness harness(true, automatic ? ERequestedMode::Auto : ERequestedMode::Manual);
                harness.Settings.Config.Polling.ObservationTtl = TDuration::Seconds(120);
                harness.Settings.Config.Polling.ReadRpcTimeout = TDuration::Seconds(60);
                harness.Core = std::make_unique<TControllerCore>(harness.Settings);
                harness.PeerFailed = true;
                harness.Bootstrap();
                harness.Advance(1);
                harness.Advance(12);
                harness.Advance(3);
                harness.Advance(12, false);
                const auto original = harness.Core->Snapshot();
                UNIT_ASSERT(original->Decision && original->Decision->Confirmed);
                const auto expiry = original->Decision->EvidenceExpiry;
                if (!automatic) {
                    auto submission = harness.Manual();
                    submission.DecisionId = original->Decision->Id;
                    harness.Core->Submit(std::move(submission));
                    harness.Collect();
                }
                UNIT_ASSERT_VALUES_EQUAL(harness.Stores.size(), 1);
                auto operation = std::move(harness.Stores.front());
                harness.Stores.pop_front();
                harness.CompleteStore(std::move(operation));
                UNIT_ASSERT_VALUES_EQUAL(harness.Reads.size(), 1);
                auto preflight = std::move(harness.Reads.front());
                harness.Reads.pop_front();
                std::optional<TStorageRequest> intent;
                if (!beforeIntent) {
                    harness.CompleteRead(preflight);
                    UNIT_ASSERT_VALUES_EQUAL(harness.Stores.size(), 1);
                    intent = std::move(harness.Stores.front());
                    harness.Stores.pop_front();
                }
                harness.ContradictoryEvidence = true;
                harness.Advance(3, false);
                harness.Advance(12, false);
                const auto revoked = harness.Core->Snapshot();
                UNIT_ASSERT(revoked->Epoch == original->Epoch);
                UNIT_ASSERT(revoked->ConfirmationGeneration > original->ConfirmationGeneration);
                UNIT_ASSERT(revoked->Decision && !revoked->Decision->Confirmed);
                UNIT_ASSERT(harness.Clock->Value + harness.Settings.Config.Polling.DispatchMargin < expiry);
                if (beforeIntent) {
                    harness.CompleteRead(std::move(preflight));
                    UNIT_ASSERT(harness.Core->Snapshot()->Applied->Attempts.empty());
                    UNIT_ASSERT(harness.Stores.empty());
                } else {
                    UNIT_ASSERT_VALUES_EQUAL(revoked->Decision->Reason, "CONTRADICTORY_GLOBAL_ASSESSMENT");
                    const auto attemptId = intent->Command.Attempt.Id;
                    harness.CompleteStore(std::move(*intent));
                    UNIT_ASSERT_VALUES_EQUAL(harness.Closed.size(), 1);
                    UNIT_ASSERT_VALUES_EQUAL(harness.Closed.front(), attemptId);
                    UNIT_ASSERT(harness.Core->Snapshot()->Applied->UnresolvedAttempt == attemptId);
                }
                UNIT_ASSERT(harness.Dispatches.empty());
                if (automatic && beforeIntent) {
                    // Identical fresh evidence re-binds the queued automatic
                    // operation; no operator action or duplicate plan is needed.
                    harness.ContradictoryEvidence = false;
                    harness.Advance(3);
                    harness.Advance(12);
                    harness.Advance(3);
                    harness.Advance(12);
                    const auto recovered = harness.Core->Snapshot();
                    UNIT_ASSERT(recovered->Decision && recovered->Decision->Confirmed);
                    UNIT_ASSERT_VALUES_EQUAL(recovered->Applied->Operations.size(), 1);
                    UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
                    const auto& dispatch = harness.Dispatches.front();
                    UNIT_ASSERT(dispatch.Permission.Automatic);
                    UNIT_ASSERT_VALUES_EQUAL(dispatch.Permission.ConfirmationGeneration, recovered->ConfirmationGeneration);
                    UNIT_ASSERT_VALUES_EQUAL(dispatch.Permission.EvidenceExpiry, recovered->Decision->EvidenceExpiry);
                    UNIT_ASSERT_VALUES_EQUAL(dispatch.Attempt.OperationId, recovered->Applied->Operations.begin()->first);
                }
            }
        }
    }

    Y_UNIT_TEST(RevokedQueuedGatewayGrantStaysRevokedAcrossRecoveryAndHttpResumeRebinds) {
        for (const bool automatic : {false, true}) {
            for (const bool recoverBeforeDelivery : {false, true}) {
                THarness harness(true, automatic ? ERequestedMode::Auto : ERequestedMode::Manual);
                harness.Settings.Config.Polling.ObservationTtl = TDuration::Seconds(120);
                harness.Settings.Config.Polling.ReadRpcTimeout = TDuration::Seconds(60);
                TDispatchProbe probe(harness.Settings.Config);
                harness.Settings.ProcessId = probe.Resources->ProcessId();
                harness.Settings.NextId = [] { return TString(TGUID::Create().AsUuidString()); };
                harness.Core = std::make_unique<TControllerCore>(harness.Settings);
                TApiOptions apiOptions;
                apiOptions.Config = harness.Settings.Config;
                apiOptions.ProcessId = harness.Settings.ProcessId;
                apiOptions.NextId = harness.Settings.NextId;
                THttpApi api(std::move(apiOptions));
                harness.PeerFailed = true;
                harness.Bootstrap();
                harness.Advance(1);
                harness.Advance(12);
                harness.Advance(3);
                harness.Advance(12);
                if (!automatic) {
                    TApiJson action(NJson::JSON_MAP);
                    action["action"] = "apply_decision";
                    action["decision_id"] = harness.Core->Snapshot()->Decision->Id;
                    const auto preview = HttpPost(api, harness, "/api/plans", action);
                    UNIT_ASSERT_VALUES_EQUAL(preview.Response.Status, 200);
                    const auto plan = ParseStrictJson(preview.Response.Body);
                    UNIT_ASSERT_C(plan["dispatchable"].GetBoolean(), preview.Response.Body);
                    auto accepted = HttpPost(api, harness, "/api/operations", AdmissionBody(plan));
                    UNIT_ASSERT_C(accepted.Submission, accepted.Response.Body);
                    harness.Core->Submit(std::move(*accepted.Submission));
                    harness.Drain();
                }
                UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
                const auto old = harness.Dispatches.front();
                const auto original = harness.Core->Snapshot();
                UNIT_ASSERT(old.Permission.ConfirmationGeneration == original->ConfirmationGeneration);
                probe.Resources->PublishSnapshot(original);
                probe.CachePreflight(harness.ReadResults.at(old.Permission.PreflightRequestId));
                harness.ContradictoryEvidence = true;
                harness.Advance(3);
                harness.Advance(12);
                UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Decision->Reason, "CONTRADICTORY_GLOBAL_ASSESSMENT");
                UNIT_ASSERT(harness.Core->Snapshot()->ConfirmationGeneration > old.Permission.ConfirmationGeneration);
                const auto recover = [&] {
                    harness.ContradictoryEvidence = false;
                    harness.Advance(3);
                    harness.Advance(12);
                    harness.Advance(3);
                    harness.Advance(12);
                    UNIT_ASSERT(harness.Core->Snapshot()->Decision->Confirmed);
                    UNIT_ASSERT(harness.Core->Snapshot()->ConfirmationGeneration > old.Permission.ConfirmationGeneration);
                };
                if (recoverBeforeDelivery) {
                    recover();
                }
                // Publish the actual current core snapshot. All admin, config,
                // mode and own-unresolved-slot gates still permit dispatch;
                // only the evidence revocation interval rejects the old grant.
                const auto current = harness.Core->Snapshot();
                UNIT_ASSERT(current->DispatchReady);
                UNIT_ASSERT(current->Epoch == old.Permission.Epoch);
                UNIT_ASSERT(harness.Clock->Value + harness.Settings.Config.Polling.DispatchMargin < old.Permission.EvidenceExpiry);
                probe.Resources->PublishSnapshot(current);
                auto rejected = probe.Dispatch(old);
                UNIT_ASSERT(rejected.NeverDispatched);
                UNIT_ASSERT_VALUES_EQUAL(rejected.Outcome.Status, "CONFIRMATION_REVOKED");
                UNIT_ASSERT(probe.Sdk->Mutations.empty());
                harness.Core->OnMutation(std::move(rejected));
                harness.Drain();
                UNIT_ASSERT(harness.Core->Snapshot()->Applied->Attempts.at(old.Attempt.Id).Disposition == EAttemptDisposition::NeverDispatched);
                const auto status = harness.Core->Snapshot()->Applied->Operations.at(old.Attempt.OperationId).Status;
                // Automatic work reschedules itself; manual work waits for an
                // explicit same-operation resume.
                UNIT_ASSERT(automatic ? status != EOperationStatus::Paused : status == EOperationStatus::Paused);
                if (!recoverBeforeDelivery) {
                    UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
                    recover();
                }
                const auto fresh = *harness.Core->Snapshot()->Decision;
                if (!automatic) {
                    TApiJson action(NJson::JSON_MAP);
                    action["action"] = "resume_workflow";
                    action["operation_id"] = old.Attempt.OperationId;
                    const auto preview = HttpPost(api, harness, "/api/plans", action);
                    UNIT_ASSERT_VALUES_EQUAL(preview.Response.Status, 200);
                    const auto plan = ParseStrictJson(preview.Response.Body);
                    UNIT_ASSERT_C(plan["dispatchable"].GetBoolean(), preview.Response.Body);
                    auto resume = HttpPost(api, harness, "/api/operations/" + old.Attempt.OperationId + "/resume", AdmissionBody(plan));
                    UNIT_ASSERT_C(resume.Submission, resume.Response.Body);
                    UNIT_ASSERT(!resume.Submission->PlanRequest);
                    UNIT_ASSERT(resume.Submission->DecisionId == fresh.Id);
                    UNIT_ASSERT_VALUES_EQUAL(resume.Submission->Command.OperationId, old.Attempt.OperationId);
                    harness.Core->Submit(std::move(*resume.Submission));
                    harness.Drain();
                }
                UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 2);
                UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Applied->Operations.size(), 1);
                const auto retried = harness.Dispatches.back();
                UNIT_ASSERT_VALUES_EQUAL(retried.Attempt.OperationId, old.Attempt.OperationId);
                UNIT_ASSERT(retried.Attempt.Update == old.Attempt.Update);
                UNIT_ASSERT_VALUES_EQUAL(retried.Attempt.Endpoint, fresh.Endpoint->Endpoint);
                UNIT_ASSERT_VALUES_EQUAL(retried.Permission.EvidenceExpiry, fresh.EvidenceExpiry);
                UNIT_ASSERT(retried.Permission.ConfirmationGeneration > old.Permission.ConfirmationGeneration);
                UNIT_ASSERT_VALUES_EQUAL(old.Permission.EvidenceExpiry, original->Decision->EvidenceExpiry);
                probe.Resources->PublishSnapshot(harness.Core->Snapshot());
                probe.CachePreflight(harness.ReadResults.at(retried.Permission.PreflightRequestId));
                const auto handedOff = probe.Dispatch(retried);
                UNIT_ASSERT(handedOff.HandedOff && !handedOff.Terminal);
                UNIT_ASSERT_VALUES_EQUAL(probe.Sdk->Mutations.size(), 1);
            }
        }
    }

    Y_UNIT_TEST(NoLaunchReservationAndExcessStartGapRevokeLiveConfirmation) {
        for (const bool noLaunch : {false, true}) {
            THarness harness(true);
            harness.Settings.Config.Polling.ObservationTtl = TDuration::Seconds(120);
            harness.Core = std::make_unique<TControllerCore>(harness.Settings);
            harness.PeerFailed = true;
            harness.Bootstrap();
            harness.Advance(1);
            harness.Advance(12);
            harness.Advance(3);
            harness.Advance(12);
            auto submission = harness.Manual();
            submission.DecisionId = harness.Core->Snapshot()->Decision->Id;
            harness.Core->Submit(std::move(submission));
            harness.Drain();
            const auto granted = harness.Dispatches.front();
            if (noLaunch) {
                // Retain both underlying bracket RPCs past their round close.
                harness.Clock->Value += TDuration::Seconds(3);
                harness.Core->Tick();
                harness.Collect();
                UNIT_ASSERT(!harness.Reads.empty());
                harness.Clock->Value += TDuration::Seconds(12);
                harness.Core->Tick();
                harness.Collect();
                const auto closedGeneration = harness.Core->Snapshot()->ConfirmationGeneration;
                harness.Clock->Value += TDuration::Seconds(3);
                harness.Core->Tick();
                harness.Collect();
                UNIT_ASSERT(harness.Core->Snapshot()->ConfirmationGeneration > closedGeneration);
                UNIT_ASSERT(!harness.Core->Snapshot()->Decision);
            } else {
                // Actual starts 116 -> 146 exceed the configured 20-second gap.
                harness.Advance(18);
                harness.Advance(12);
                UNIT_ASSERT(harness.Core->Snapshot()->Decision);
                UNIT_ASSERT(harness.Core->Snapshot()->Decision->Kind == EDecisionKind::Failover);
                UNIT_ASSERT(!harness.Core->Snapshot()->Decision->Confirmed);
            }
            UNIT_ASSERT(harness.Core->Snapshot()->ConfirmationGeneration > granted.Permission.ConfirmationGeneration);
            UNIT_ASSERT(harness.Clock->Value + harness.Settings.Config.Polling.DispatchMargin < granted.Permission.EvidenceExpiry);
            UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        }
    }

    Y_UNIT_TEST(ConsecutiveCompatibleConfirmationKeepsOriginalGrantExpiry) {
        THarness harness(true);
        harness.Settings.Config.Polling.ObservationTtl = TDuration::Seconds(120);
        harness.Core = std::make_unique<TControllerCore>(harness.Settings);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Advance(1);
        harness.Advance(12);
        harness.Advance(3);
        harness.Advance(12);
        auto submission = harness.Manual();
        submission.DecisionId = harness.Core->Snapshot()->Decision->Id;
        harness.Core->Submit(std::move(submission));
        harness.Drain();
        const auto granted = harness.Dispatches.front();
        harness.Advance(3);
        harness.Advance(12);
        const auto current = harness.Core->Snapshot();
        UNIT_ASSERT(current->Decision->Confirmed);
        UNIT_ASSERT_VALUES_EQUAL(current->ConfirmationGeneration, granted.Permission.ConfirmationGeneration);
        UNIT_ASSERT(current->Decision->EvidenceExpiry > granted.Permission.EvidenceExpiry);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.front().Permission.EvidenceExpiry, granted.Permission.EvidenceExpiry);
    }

    Y_UNIT_TEST(ExpiredDecisionCannotBeAdmittedAsOrdinaryManualFailover) {
        THarness harness(true);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Advance(1);
        harness.Advance(12);
        harness.Advance(3);
        harness.Advance(12);
        auto submission = harness.Manual();
        submission.DecisionId = harness.Core->Snapshot()->Decision->Id;
        harness.Clock->Value += TDuration::Seconds(9);
        harness.Core->Submit(submission);
        harness.Drain();
        UNIT_ASSERT(harness.Dispatches.empty());
        UNIT_ASSERT_VALUES_EQUAL(harness.Replies.back().Result.Result.Code, "AUTOMATIC_DECISION_UNAVAILABLE");
    }

    Y_UNIT_TEST(AdmissionP99OverrunWithinOriginalTtlStillDispatches) {
        THarness harness(true);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Advance(1);
        harness.Advance(12);
        harness.Advance(3);
        harness.Advance(12);
        const auto decision = *harness.Core->Snapshot()->Decision;
        auto submission = harness.Manual();
        submission.DecisionId = decision.Id;
        harness.Core->Submit(submission);
        harness.Drain(false);
        auto operation = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        harness.CompleteStore(std::move(operation));
        UNIT_ASSERT_VALUES_EQUAL(harness.Reads.size(), 1);
        UNIT_ASSERT(harness.Reads.front().OperationPriority);
        UNIT_ASSERT_VALUES_EQUAL(harness.Reads.front().Timeout, TDuration::Seconds(6));
        harness.Drain(false);
        auto intent = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        harness.Clock->Value += TDuration::Seconds(6); // configured p99 is 5s
        harness.CompleteStore(std::move(intent));
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.front().Permission.ExpiresAt, decision.EvidenceExpiry);
        harness.Handoff(harness.Dispatches.front());
        UNIT_ASSERT_VALUES_EQUAL(harness.Handoffs.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Handoffs.front().RoundCloseToHandoff, TDuration::Seconds(6));
        UNIT_ASSERT_VALUES_EQUAL(harness.Handoffs.front().EvidenceSlack, TDuration::Seconds(2));
        harness.Handoff(harness.Dispatches.front());
        UNIT_ASSERT_VALUES_EQUAL(harness.Handoffs.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->AdmissionBudgetOverruns, 1);
        UNIT_ASSERT_VALUES_EQUAL(*harness.Core->Snapshot()->LastRoundCloseToHandoff, TDuration::Seconds(6));
        UNIT_ASSERT_VALUES_EQUAL(*harness.Core->Snapshot()->LastHandoffSlack, TDuration::Seconds(2));
    }

    Y_UNIT_TEST(OriginalTtlExpiryDuringCommitRequiresNegativeClosure) {
        THarness harness(true);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Advance(1);
        harness.Advance(12);
        harness.Advance(3);
        harness.Advance(12);
        auto submission = harness.Manual();
        submission.DecisionId = harness.Core->Snapshot()->Decision->Id;
        harness.Core->Submit(submission);
        harness.Drain(false);
        auto operation = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        harness.CompleteStore(std::move(operation));
        harness.Drain(false);
        auto intent = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        const auto attempt = intent.Command.Attempt.Id;
        harness.Clock->Value += TDuration::Seconds(7);
        harness.CompleteStore(std::move(intent));
        UNIT_ASSERT(harness.Dispatches.empty());
        UNIT_ASSERT_VALUES_EQUAL(harness.Closed.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Closed.front(), attempt);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->UnresolvedAttempt == attempt);
        UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->FreshnessMisses, 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->AdmissionBudgetOverruns, 0);
    }

    Y_UNIT_TEST(RestoredQueuedEvidenceBoundOperationCannotBecomeOrdinaryManualAction) {
        THarness harness(true);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Advance(1);
        harness.Advance(12);
        harness.Advance(3);
        harness.Advance(12);
        auto submission = harness.Manual();
        submission.DecisionId = harness.Core->Snapshot()->Decision->Id;
        const auto operationId = submission.Command.OperationId;
        harness.Core->Submit(submission);
        harness.Drain(false);
        auto operation = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        harness.CompleteStore(std::move(operation));
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Operations.at(operationId).Plan.EvidenceBound);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Operations.at(operationId).Attempts.empty());
        harness.Restart();
        harness.Advance(1);
        harness.Advance(12);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Operations.at(operationId).Attempts.empty());
        UNIT_ASSERT(harness.Dispatches.empty());
    }

    Y_UNIT_TEST(ChangedDecisionWithSameAdminFingerprintRejectsEarlierPreview) {
        THarness harness(true);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Advance(1);
        harness.Advance(12);
        harness.Advance(3);
        harness.Advance(12);
        auto submission = harness.Manual();
        submission.DecisionId = harness.Core->Snapshot()->Decision->Id;
        harness.PeerFailed = false;
        harness.Advance(3);
        harness.Advance(12);
        UNIT_ASSERT_VALUES_EQUAL(harness.Core->Snapshot()->Epoch.AdminFingerprint, submission.StateFingerprint);
        harness.Core->Submit(submission);
        harness.Drain();
        UNIT_ASSERT(harness.Dispatches.empty());
        UNIT_ASSERT_VALUES_EQUAL(harness.Replies.back().Result.Result.Code, "AUTOMATIC_DECISION_UNAVAILABLE");
    }

    Y_UNIT_TEST(PreHandoffReadsDeliveredLaterCannotProveAcceptance) {
        THarness harness(true);
        harness.Bootstrap();
        harness.Core->Submit(harness.Manual());
        harness.Drain();
        const auto dispatch = harness.Dispatches.front();
        harness.Clock->Value += TDuration::Seconds(1);
        harness.Core->Tick();
        harness.Collect();
        auto beforeHandoff = std::move(harness.Reads);
        harness.Reads.clear();
        harness.Handoff(dispatch);
        harness.View.Piles["b"] = EPileState::Disconnected;
        ++harness.View.Generation;
        harness.Clock->Value += TDuration::Seconds(1);
        for (const auto& read : beforeHandoff) {
            auto result = harness.Response(read);
            result.StartedAt = TInstant::Seconds(100);
            harness.Core->OnRead(std::move(result));
        }
        harness.Drain();
        UNIT_ASSERT(!harness.Core->Snapshot()->Applied->Attempts.at(dispatch.Attempt.Id).Accepted);
        harness.Advance(1);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Attempts.at(dispatch.Attempt.Id).Accepted);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
    }

    Y_UNIT_TEST(ObserveKeepsQualifiedAcceptanceOfAdmittedMutation) {
        THarness harness(true);
        harness.Bootstrap();
        harness.Core->Submit(harness.Manual());
        harness.Drain();
        const auto dispatch = harness.Dispatches.front();
        harness.Handoff(dispatch);
        harness.Core->Submit(harness.Mode(ERequestedMode::Observe));
        harness.Drain();
        UNIT_ASSERT(!harness.Core->Snapshot()->Applied->Ownership);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->ExclusiveHistory);
        harness.View.Piles["b"] = EPileState::Disconnected;
        ++harness.View.Generation;
        for (unsigned i = 0; i < 5 && !harness.Core->Snapshot()->Applied->Attempts.at(dispatch.Attempt.Id).Accepted; ++i) {
            harness.Advance(1);
        }
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Attempts.at(dispatch.Attempt.Id).Accepted);
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
    }

    Y_UNIT_TEST(RejoinNeedsFreshQualifiedSelfHealthAfterSynchronizedPostcondition) {
        THarness harness(true);
        harness.View.Piles["b"] = EPileState::Disconnected;
        harness.Bootstrap();
        auto submission = harness.Manual(EOperationKind::Rejoin);
        harness.Core->Submit(submission);
        harness.Drain();
        const auto dispatch = harness.Dispatches.front();
        harness.Finish(dispatch);
        harness.Drain();
        harness.View.Piles["b"] = EPileState::Synchronized;
        ++harness.View.Generation;
        harness.Advance(1);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Operations.at(dispatch.Attempt.OperationId).Status != EOperationStatus::Succeeded);
        harness.Advance(12);
        harness.Advance(3);
        harness.Advance(12);
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Operations.at(dispatch.Attempt.OperationId).Status == EOperationStatus::Succeeded);
    }

    Y_UNIT_TEST(ObserveDoesNotResumeMaintenanceNextStepWithoutExplicitResume) {
        THarness harness;
        harness.Bootstrap();
        auto submission = harness.Manual(EOperationKind::PrimaryMaintenance);
        submission.PlanRequest->Target = "a";
        submission.PlanRequest->Successor = "b";
        harness.Core->Submit(submission);
        harness.Drain();
        const auto first = harness.Dispatches.front();
        harness.Finish(first);
        harness.Drain();
        harness.Core->Submit(harness.Mode(ERequestedMode::Observe));
        harness.Drain();
        harness.View.Piles["a"] = EPileState::Synchronized;
        harness.View.Piles["b"] = EPileState::Primary;
        ++harness.View.Generation;
        harness.Advance(1);
        const auto id = first.Attempt.OperationId;
        UNIT_ASSERT(harness.Core->Snapshot()->Applied->Operations.at(id).Status == EOperationStatus::Paused);
        harness.Core->Submit(harness.Mode(ERequestedMode::Manual));
        harness.Drain();
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        auto resume = harness.Manual();
        resume.PlanRequest.reset();
        resume.Command.Kind = ECommandKind::Resume;
        resume.Command.OperationId = id;
        resume.Command.ExpectedWorkflowRevision = harness.Core->Snapshot()->Applied->Operations.at(id).Revision;
        harness.Core->Submit(std::move(resume));
        harness.Drain();
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 2);
        UNIT_ASSERT(harness.Dispatches.back().Attempt.Update.at("a") == EPileState::Suspended);
    }

    Y_UNIT_TEST(EmergencyPreemptionRejectsOldMaintenancePreflightCallback) {
        THarness harness;
        harness.Bootstrap();
        auto maintenance = harness.Manual(EOperationKind::PrimaryMaintenance);
        maintenance.PlanRequest->Target = "a";
        maintenance.PlanRequest->Successor = "b";
        harness.Core->Submit(maintenance);
        harness.Drain();
        const auto first = harness.Dispatches.front();
        harness.Finish(first);
        harness.Drain();
        harness.View.Piles["a"] = EPileState::Synchronized;
        harness.View.Piles["b"] = EPileState::Primary;
        ++harness.View.Generation;
        harness.Advance(1, false);
        bool reconciled = false;
        while (!harness.Stores.empty()) {
            auto store = std::move(harness.Stores.front());
            harness.Stores.pop_front();
            const auto kind = store.Command.Kind;
            harness.CompleteStore(std::move(store));
            if (kind == ECommandKind::Reconcile) {
                reconciled = true;
                break;
            }
        }
        UNIT_ASSERT(reconciled);
        UNIT_ASSERT_VALUES_EQUAL(harness.Reads.size(), 1);
        const auto oldPreflight = harness.Reads.front();
        harness.Reads.pop_front();
        UNIT_ASSERT(oldPreflight.OperationPriority);
        auto emergency = harness.Manual();
        emergency.PlanRequest->FailedTargets = {"a"};
        emergency.PlanRequest->Target = "a";
        const auto emergencyId = emergency.Command.OperationId;
        harness.Core->Submit(emergency);
        harness.Collect();
        UNIT_ASSERT_VALUES_EQUAL(harness.Stores.size(), 1);
        auto admitted = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        harness.CompleteStore(std::move(admitted));
        // Node b's cached admin view predates the primary change. Preemption
        // must still issue its own fresh preflight, rather than wait for a poll
        // or reuse the old maintenance read from the now-failed node a.
        UNIT_ASSERT_VALUES_EQUAL(harness.Reads.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(harness.Reads.front().Endpoint, "grpc://127.0.0.1:21002");
        UNIT_ASSERT(harness.Reads.front().OperationPriority);
        UNIT_ASSERT(harness.Stores.empty());
        harness.CompleteRead(oldPreflight);
        UNIT_ASSERT(harness.Stores.empty());
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 1);
        harness.Drain();
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 2);
        UNIT_ASSERT(harness.Dispatches.back().Attempt.Update.at("a") == EPileState::Disconnected);
        const auto& old = harness.Core->Snapshot()->Applied->Operations.at(first.Attempt.OperationId);
        UNIT_ASSERT(old.Status == EOperationStatus::Preempted);
        UNIT_ASSERT(old.PreemptedBy == emergencyId);
        UNIT_ASSERT_VALUES_EQUAL(old.Attempts.size(), 1);
    }

    Y_UNIT_TEST(ReadinessLossAndRecoveryDiscardsPreLossBracketReplies) {
        THarness harness(true);
        harness.PeerFailed = true;
        harness.Bootstrap();
        harness.Clock->Value += TDuration::Seconds(1);
        harness.Core->Tick();
        harness.Collect();
        std::vector<TReadRequest> heldHealth;
        while (!harness.Reads.empty()) {
            auto read = std::move(harness.Reads.front());
            harness.Reads.pop_front();
            if (read.Method == EReadMethod::Health && read.Bracket) {
                heldHealth.push_back(read);
            } else {
                harness.CompleteRead(std::move(read));
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(heldHealth.size(), 2);
        const auto epoch = harness.Core->Snapshot()->Epoch;
        harness.Core->SetLocalStop(true, 1);
        harness.Core->SetLocalStop(false, 2);
        UNIT_ASSERT(harness.Core->Snapshot()->Epoch.ReadinessGeneration > epoch.ReadinessGeneration);
        for (const auto& read : heldHealth) {
            harness.CompleteRead(read);
        }
        harness.Advance(12);
        UNIT_ASSERT(harness.Core->Snapshot()->Decision);
        UNIT_ASSERT(!harness.Core->Snapshot()->Decision->Confirmed);
        UNIT_ASSERT(harness.Core->Snapshot()->Decision->EvidenceRequestIds.empty());
        UNIT_ASSERT(harness.Dispatches.empty());
    }

    Y_UNIT_TEST(ConfigPollingRotatesBeyondFourEndpointsWithoutCatchupQueue) {
        THarness harness;
        harness.Settings.Config.YdbEndpoints.clear();
        for (size_t index = 0; index < 10; ++index) {
            harness.Settings.Config.YdbEndpoints.push_back(ParseEndpoint(TStringBuilder() << "grpc://host-" << index << ":2135"));
        }
        harness.Core = std::make_unique<TControllerCore>(harness.Settings);
        std::set<TString> visited;
        for (size_t round = 0; round < 3; ++round) {
            harness.Core->Tick();
            harness.Collect();
            size_t configCount = 0;
            while (!harness.Reads.empty()) {
                auto read = std::move(harness.Reads.front());
                harness.Reads.pop_front();
                if (read.Method == EReadMethod::Config) {
                    ++configCount;
                    visited.insert(read.Endpoint);
                }
                TReadResult failure;
                failure.Request = read;
                failure.Status = "UNAVAILABLE";
                failure.StartedAt = harness.Clock->Value;
                failure.FinishedAt = harness.Clock->Value;
                harness.Core->OnRead(std::move(failure));
                harness.Collect();
            }
            UNIT_ASSERT_VALUES_EQUAL(configCount, 4);
            harness.Clock->Value += TDuration::Seconds(30);
        }
        UNIT_ASSERT_VALUES_EQUAL(visited.size(), 10);
        UNIT_ASSERT(harness.Stores.empty());
    }

    Y_UNIT_TEST(SixteenCorrelatedOutcomesSurvivePendingIntentAndCompletionPermutations) {
        THarness harness(true);
        harness.Bootstrap();
        std::vector<TDispatchRequest> running;
        for (size_t index = 0; index < 15; ++index) {
            const auto dispatch = harness.StartSwitch();
            running.push_back(dispatch);
            harness.ObserveSwitch(dispatch);
        }
        auto sixteenth = harness.Manual(EOperationKind::Switchover);
        sixteenth.PlanRequest->Successor = harness.View.Piles.at("a") == EPileState::Primary ? "b" : "a";
        harness.Core->Submit(std::move(sixteenth));
        harness.Drain(false);
        UNIT_ASSERT_VALUES_EQUAL(harness.Stores.size(), 1);
        auto operation = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        harness.CompleteStore(std::move(operation));
        harness.Drain(false);
        UNIT_ASSERT_VALUES_EQUAL(harness.Stores.size(), 1);
        auto pendingIntent = std::move(harness.Stores.front());
        harness.Stores.pop_front();
        UNIT_ASSERT(pendingIntent.Command.Kind == ECommandKind::AdmitAttempt);
        std::reverse(running.begin(), running.end());
        for (const auto& dispatch : running) {
            harness.Finish(dispatch);
        }
        harness.CompleteStore(std::move(pendingIntent));
        UNIT_ASSERT_VALUES_EQUAL(harness.Dispatches.size(), 16);
        const auto last = harness.Dispatches.back();
        harness.Finish(last);
        harness.Drain();
        for (const auto& dispatch : harness.Dispatches) {
            UNIT_ASSERT(harness.Core->Snapshot()->Applied->Attempts.at(dispatch.Attempt.Id).RpcDisposition == ERpcDisposition::Success);
        }
        UNIT_ASSERT(!harness.Core->Snapshot()->Applied->UnresolvedAttempt);
    }

    Y_UNIT_TEST(DiagnosticLatencyMeasuresRpcElapsedTimeIncludingFailedResponses) {
        for (size_t index = 0; index < 3; ++index) {
            THarness harness;
            harness.Core->Tick();
            harness.Collect();
            const auto request = std::find_if(harness.Reads.begin(), harness.Reads.end(), [](const auto& read) {
                return read.Method == EReadMethod::Health;
            });
            UNIT_ASSERT(request != harness.Reads.end());
            auto result = harness.Response(*request);
            const auto endpoint = result.Request.Endpoint;
            result.Success = index == 0;
            result.Status = result.Success ? "SUCCESS" : "UNAVAILABLE";
            result.StartedAt = TInstant::Seconds(100);
            result.FinishedAt = TInstant::Seconds(index == 2 ? 99 : 102);
            if (!result.Success) {
                result.Health.reset();
            }
            harness.Clock->Value = TInstant::Seconds(102);
            harness.Core->OnRead(std::move(result));
            const auto snapshot = harness.Core->Snapshot();
            const auto found = std::find_if(snapshot->Diagnostics.begin(), snapshot->Diagnostics.end(), [&](const auto& summary) {
                return summary.Endpoint == endpoint;
            });
            UNIT_ASSERT(found != snapshot->Diagnostics.end());
            UNIT_ASSERT_VALUES_EQUAL(found->Success, index == 0);
            UNIT_ASSERT_VALUES_EQUAL(found->StartedAt, TInstant::Seconds(100));
            if (index == 2) {
                UNIT_ASSERT(!found->Latency);
            } else {
                UNIT_ASSERT(found->Latency);
                UNIT_ASSERT_VALUES_EQUAL(*found->Latency, TDuration::Seconds(2));
            }
        }
    }

}

} // namespace NBridgeSkipper
