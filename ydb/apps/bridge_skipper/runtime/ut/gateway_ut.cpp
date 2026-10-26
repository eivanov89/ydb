#include <ydb/apps/bridge_skipper/runtime/gateway.h>
#include <ydb/apps/bridge_skipper/runtime/controller.h>

#include <library/cpp/testing/unittest/registar.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <ydb/public/api/protos/ydb_monitoring.pb.h>
#include <util/folder/tempdir.h>
#include <util/string/cast.h>

namespace NBridgeSkipper {
namespace {

class TClock final : public ITimeProvider {
public:
    TInstant Value = TInstant::Seconds(100);
    std::deque<TInstant> Sequence;

    TInstant Now() override {
        if (!Sequence.empty()) {
            Value = Sequence.front();
            Sequence.pop_front();
        }
        return Value;
    }
};

struct TDispatchFixture {
    TDurableState State;
    TDispatchRequest Request;
    TReadResult Preflight;
    TInstant Now = TInstant::Seconds(100);
    TIntrusivePtr<TClock> Clock = MakeIntrusive<TClock>();

    explicit TDispatchFixture(bool prebridge = false, bool failover = false) {
        TSkipperClock::RegisterProvider(Clock);
        State.ClusterId = "cluster";
        State.MemberId = "member";
        State.MembershipIncarnation = "membership";
        auto apply = [this](TDurableCommand command) {
            command.ExpectedControllerRevision = State.ControllerRevision;
            command.Ready = true;
            command.Fresh = true;
            command.Coherent = true;
            const auto result = Apply(State, command);
            UNIT_ASSERT_C(result.Result.Ok, result.Result.Code);
            State = result.State;
        };
        TDurableCommand own;
        own.Id = "own";
        own.Kind = ECommandKind::SetMode;
        own.Reason = "ownership";
        own.Ownership = TOwnershipAcknowledgement{};
        own.Ownership->ExclusiveAdministration = true;
        own.Ownership->DeploymentPlanReference = "deployment-plan";
        apply(own);
        TClusterView baseline;
        baseline.Converted = !prebridge;
        baseline.Generation = prebridge ? 0 : 5;
        baseline.Piles = {{"a", EPileState::Primary}};
        if (!prebridge) {
            baseline.Piles.emplace("b", EPileState::Synchronized);
        }
        baseline.Endpoint = "grpc://node:2135";
        TPlanRequest plan;
        plan.Kind = prebridge ? EOperationKind::Convert : (failover ? EOperationKind::Failover : EOperationKind::Takedown);
        plan.Target = prebridge ? "a" : "b";
        plan.ExternalPrerequisitesAcknowledged = true;
        if (failover) {
            plan.FailedTargets = {"b"};
        }
        const auto planned = PlanWorkflow(baseline, plan);
        UNIT_ASSERT_C(planned.Plan, planned.Error);
        TDurableCommand operation;
        operation.Id = "operation-command";
        operation.OperationId = "operation";
        operation.Kind = ECommandKind::AdmitOperation;
        operation.Plan = *planned.Plan;
        apply(operation);
        TDurableCommand intent;
        intent.Id = "intent-command";
        intent.OperationId = "operation";
        intent.Kind = ECommandKind::AdmitAttempt;
        intent.ExpectedWorkflowRevision = State.Operations.at("operation").Revision;
        auto& attempt = intent.Attempt;
        attempt.Id = "attempt";
        attempt.OperationId = "operation";
        attempt.Method = planned.Plan->Steps.front().Method;
        attempt.Update = planned.Plan->Steps.front().Update;
        attempt.Target = planned.Plan->Steps.front().Target;
        attempt.Endpoint = baseline.Endpoint;
        attempt.NodeId = 17;
        attempt.DesignatedMember = "member";
        attempt.DesignatedProcess = "process";
        attempt.Baseline = baseline;
        attempt.TopologyRevision = 1;
        attempt.ArgumentsHash = MutationArgumentsHash(attempt);
        apply(intent);
        Request.Attempt = State.Attempts.at("attempt");
        Request.Permission = {"grant", "preflight", "process", Request.Attempt.ArgumentsHash, Request.Attempt.IntentIndex,
            Now + TDuration::Seconds(7), Now + TDuration::Seconds(20), false, {}, 0};
        Preflight.Request.Id = "preflight";
        Preflight.Request.ProcessId = "process";
        Preflight.Request.Endpoint = baseline.Endpoint;
        Preflight.Request.Method = EReadMethod::Admin;
        Preflight.Request.OperationPriority = true;
        Preflight.Request.Deadline = Now + TDuration::Seconds(10);
        Preflight.Success = !prebridge;
        if (prebridge) {
            Preflight.Status = "NOT_FOUND";
        } else {
            Preflight.Status = "SUCCESS";
            Preflight.Cluster = baseline;
        }
        Preflight.FinishedAt = Now;
    }

    TString Validate(bool stopped = false, bool open = true) const {
        return ValidateDispatch(Request, State, "process", "member", Preflight, open, stopped, TDuration::Seconds(2), Now);
    }
};

class TFakeSdk final : public ISdkGateway {
public:
    std::vector<TReadRequest> Reads;
    std::vector<std::function<void(TReadResult)>> ReadCompletions;
    std::vector<TMutationAttempt> Mutations;
    std::vector<std::function<void(TRpcOutcome)>> MutationCompletions;

    void Read(TReadRequest request, std::function<void(TReadResult)> completed) override {
        Reads.push_back(std::move(request));
        ReadCompletions.push_back(std::move(completed));
    }

    void Mutate(TMutationAttempt attempt, std::function<void(TRpcOutcome)> completed) override {
        Mutations.push_back(std::move(attempt));
        MutationCompletions.push_back(std::move(completed));
    }

    void Stop() override {
    }
};

struct TGatewayActorFixture {
    TDispatchFixture Dispatch;
    TTempDir Directory;
    TConfig Config;
    std::shared_ptr<TRuntimeResources> Resources;
    std::shared_ptr<TFakeSdk> Sdk = std::make_shared<TFakeSdk>();
    NActors::TTestActorRuntimeBase Runtime;
    NActors::TActorId Gateway;
    NActors::TActorId Edge;

    explicit TGatewayActorFixture(bool prebridge = false, bool failover = false)
        : Dispatch(prebridge, failover)
    {
        Config = ParseConfig("schema_version: 1\ncluster_id: cluster\nydb_endpoints: [grpc://node:2135]\n");
        Resources = std::make_shared<TRuntimeResources>(Config, Directory.Name(), "member");
        Dispatch.Request.Attempt.DesignatedProcess = Resources->ProcessId();
        Dispatch.State.Attempts.at("attempt").DesignatedProcess = Resources->ProcessId();
        Dispatch.Request.Permission.ProcessId = Resources->ProcessId();
        Dispatch.Preflight.Request.ProcessId = Resources->ProcessId();
        auto snapshot = std::make_shared<TControllerSnapshot>();
        snapshot->Applied = std::make_shared<TDurableState>(Dispatch.State);
        snapshot->AuthorityReady = true;
        snapshot->DispatchReady = true;
        snapshot->AdminValidUntil = Dispatch.Now + TDuration::Seconds(20);
        snapshot->ConfigValidUntil = Dispatch.Now + TDuration::Seconds(60);
        snapshot->TopologyRevision = 1;
        snapshot->Cluster = Dispatch.Request.Attempt.Baseline;
        Resources->PublishSnapshot(snapshot);
        Runtime.Initialize();
        Resources->CompletionGate->Attach(Runtime.GetActorSystem(0));
        Gateway = Runtime.Register(CreateGatewayActor(Config, Resources, Sdk));
        Edge = Runtime.AllocateEdgeActor();
        NActors::TDispatchOptions bootstrap;
        bootstrap.FinalEvents.emplace_back([gateway = Gateway](NActors::IEventHandle& event) {
            return event.GetRecipientRewrite() == gateway && event.GetTypeRewrite() == NActors::TEvents::TSystem::Bootstrap;
        });
        Runtime.DispatchEvents(bootstrap);
    }

    ~TGatewayActorFixture() {
        Resources->CompletionGate->Close();
        Resources->StopStorage();
    }

    void CachePreflight() {
        auto request = Dispatch.Preflight.Request;
        Runtime.SendAsync(new NActors::IEventHandle(Gateway, Edge, new TEvRuntime::TEvGatewayRead(request)));
        // A second same-method request gets a bounded busy result, proving the
        // first SDK request has actually been dispatched before completion.
        request.Id = "overlap";
        Runtime.SendAsync(new NActors::IEventHandle(Gateway, Edge, new TEvRuntime::TEvGatewayRead(request)));
        const auto busy = Runtime.GrabEdgeEvent<TEvRuntime::TEvGatewayReadResult>(Edge);
        UNIT_ASSERT(busy->Get()->Result.Busy);
        UNIT_ASSERT_VALUES_EQUAL(Sdk->Reads.size(), 1);
        auto result = Dispatch.Preflight;
        result.Request = Sdk->Reads.front();
        Sdk->ReadCompletions.front()(std::move(result));
        const auto completed = Runtime.GrabEdgeEvent<TEvRuntime::TEvGatewayReadResult>(Edge);
        UNIT_ASSERT_VALUES_EQUAL(completed->Get()->Result.Success, Dispatch.Preflight.Success);
        UNIT_ASSERT_VALUES_EQUAL(completed->Get()->Result.Status, Dispatch.Preflight.Status);
    }

    void SendClose() {
        auto event = MakeHolder<TEvRuntime::TEvCloseAttempt>();
        event->AttemptId = "attempt";
        event->ProcessId = Resources->ProcessId();
        Runtime.SendAsync(new NActors::IEventHandle(Gateway, Edge, event.Release()));
    }
};

} // namespace

Y_UNIT_TEST_SUITE(TSkipperGateway) {
    Y_UNIT_TEST(PinnedSdkClientsUseExactAuthorityAndInheritDriverTlsCredentials) {
        for (const auto& [uri, authority] : std::vector<std::pair<TString, std::string>>{
                {"grpc://127.0.0.1:21001", "127.0.0.1:21001"},
                {"grpcs://node.example:2136", "node.example:2136"},
                {"grpcs://[::1]:32135", "[::1]:32135"}})
        {
            const auto settings = MakePinnedClientSettings(ParseEndpoint(uri));
            UNIT_ASSERT(settings.DiscoveryEndpoint_);
            UNIT_ASSERT_VALUES_EQUAL(*settings.DiscoveryEndpoint_, authority);
            UNIT_ASSERT(settings.Database_ && settings.Database_->empty());
            UNIT_ASSERT(settings.DiscoveryMode_ == NYdb::EDiscoveryMode::Off);
            // Overriding this with scheme-only credentials would erase the
            // configured private CA and client certificate inherited by SDK.
            UNIT_ASSERT(!settings.SslCredentials_);
            UNIT_ASSERT(!settings.CredentialsProviderFactory_);
        }
    }

    Y_UNIT_TEST(RealAppliedIntentProducesExactlyOneValidLiveGrant) {
        TDispatchFixture fixture;
        UNIT_ASSERT_VALUES_EQUAL(fixture.State.Operations.at("operation").Revision, fixture.Request.Attempt.WorkflowRevision + 1);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "");
        TGatewayBudget budget(16);
        UNIT_ASSERT(budget.AcquireMutation(fixture.Request.Attempt.Id));
        UNIT_ASSERT(!budget.AcquireMutation(fixture.Request.Attempt.Id));
        UNIT_ASSERT(!budget.CloseAttempt(fixture.Request.Attempt.Id));
        UNIT_ASSERT(budget.SettleMutation(fixture.Request.Attempt.Id));
        UNIT_ASSERT(!budget.AcquireMutation(fixture.Request.Attempt.Id));
    }

    Y_UNIT_TEST(NegativeClosureWinsAgainstEveryLateSend) {
        TGatewayBudget budget(16);
        UNIT_ASSERT(budget.CloseAttempt("attempt"));
        UNIT_ASSERT(budget.CloseAttempt("attempt"));
        UNIT_ASSERT(!budget.AcquireMutation("attempt"));
        UNIT_ASSERT(budget.IsClosed("attempt"));
    }

    Y_UNIT_TEST(AcceptanceDoesNotReleaseUnsettledMutationFutureCapacity) {
        TGatewayBudget budget(16);
        for (size_t i = 0; i < 16; ++i) {
            UNIT_ASSERT(budget.AcquireMutation(ToString(i)));
        }
        UNIT_ASSERT(!budget.AcquireMutation("seventeenth"));
        UNIT_ASSERT_VALUES_EQUAL(budget.Mutations(), 16);
        UNIT_ASSERT(!budget.CloseAttempt("0"));
        UNIT_ASSERT(budget.SettleMutation("0"));
        UNIT_ASSERT(budget.AcquireMutation("seventeenth"));
    }

    Y_UNIT_TEST(TimeoutDoesNotReleaseSameMethodReadUntilUnderlyingSettlement) {
        TGatewayBudget budget(3);
        TReadRequest request;
        request.Id = "first";
        request.Endpoint = "grpc://node:2135";
        request.Method = EReadMethod::Health;
        request.Deadline = TInstant::Seconds(101);
        UNIT_ASSERT(budget.AcquireRead(request));
        request.Id = "next-round";
        request.Deadline = TInstant::Seconds(116);
        UNIT_ASSERT(!budget.AcquireRead(request));
        UNIT_ASSERT(budget.SettleRead("first"));
        UNIT_ASSERT(budget.AcquireRead(request));
    }

    Y_UNIT_TEST(AdminReservesTwoSlotsForOperationsWithoutStealingBrackets) {
        TGatewayBudget budget(3);
        TReadRequest request;
        request.Id = "ordinary";
        request.Endpoint = "grpc://a:1";
        UNIT_ASSERT(budget.AcquireRead(request));
        request.Id = "another";
        request.Endpoint = "grpc://b:1";
        UNIT_ASSERT(!budget.AcquireRead(request));
        request.OperationPriority = true;
        UNIT_ASSERT(budget.AcquireRead(request));
        request.Id = "third";
        request.Endpoint = "grpc://c:1";
        UNIT_ASSERT(budget.AcquireRead(request));
        request.Id = "bracket";
        request.Endpoint = "grpc://d:1";
        request.Bracket = true;
        UNIT_ASSERT(budget.AcquireRead(request));
        request.Id = "same-node-bracket";
        request.Endpoint = "grpc://a:1";
        UNIT_ASSERT(!budget.AcquireRead(request));
    }

    Y_UNIT_TEST(DispatchRejectsForeignProcessReplayedGrantAndTamperedArguments) {
        TDispatchFixture fixture;
        fixture.Request.Permission.ProcessId = "old-process";
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "FOREIGN_PROCESS_OR_PERMISSION");
        fixture.Request.Permission.ProcessId = "process";
        fixture.Request.Attempt.Update["b"] = EPileState::Disconnected;
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "INTENT_NOT_INSTALLED");
    }

    Y_UNIT_TEST(LocalStopObserveAndLostReadinessCloseDispatchIndependently) {
        TDispatchFixture fixture;
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(true), "DISPATCH_NOT_READY");
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(false, false), "DISPATCH_NOT_READY");
        fixture.State.RequestedMode = ERequestedMode::Observe;
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "DISPATCH_NOT_READY");
    }

    Y_UNIT_TEST(SameNodeExactFreshPreflightAndActualHandoffMargin) {
        TDispatchFixture fixture;
        fixture.Now += TDuration::Seconds(3); // SQLite admission may take seconds.
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "");
        fixture.Preflight.Request.Endpoint = "grpc://other:2135";
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "SAME_NODE_PREFLIGHT_REQUIRED");
        fixture.Preflight.Request.Endpoint = fixture.Request.Attempt.Endpoint;
        fixture.Preflight.Cluster->Generation++;
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "SAME_NODE_PREFLIGHT_REQUIRED");
        fixture.Now += TDuration::Seconds(3);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "EVIDENCE_EXPIRED");
    }

    Y_UNIT_TEST(PreemptionAndAutomaticEvidenceExpiryCannotDispatch) {
        TDispatchFixture fixture;
        fixture.State.Operations.at("operation").Revision++;
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "WORKFLOW_CHANGED");
        fixture.State.Operations.at("operation").Revision--;
        fixture.Request.Permission.Automatic = true;
        fixture.State.RequestedMode = ERequestedMode::Auto;
        fixture.Request.Permission.EvidenceExpiry = fixture.Now + TDuration::Seconds(1);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Validate(), "EVIDENCE_EXPIRED");
    }

    Y_UNIT_TEST(FakeSdkCallbackAfterGateClosureDoesNotTouchActorSystem) {
        TGatewayActorFixture fixture;
        auto request = fixture.Dispatch.Preflight.Request;
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge, new TEvRuntime::TEvGatewayRead(request)));
        request.Id = "busy";
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge, new TEvRuntime::TEvGatewayRead(request)));
        fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvGatewayReadResult>(fixture.Edge);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Sdk->Reads.size(), 1);
        fixture.Resources->CompletionGate->Close();
        bool destroyed = false;
        TReadResult result;
        result.Request = request;
        result.Health = std::shared_ptr<const Ydb::Monitoring::SelfCheckResult>(new Ydb::Monitoring::SelfCheckResult(), [&destroyed](const auto* payload) {
            delete payload;
            destroyed = true;
        });
        fixture.Sdk->ReadCompletions.front()(std::move(result));
        UNIT_ASSERT(destroyed);
    }

    Y_UNIT_TEST(QualifiedAutomaticControllerIntentUsesOwnSlotAndReachesSdkExactlyOnce) {
        TGatewayActorFixture fixture;
        TControllerOptions options;
        options.Config = fixture.Config;
        options.MemberId = fixture.Resources->MemberId();
        options.ProcessId = fixture.Resources->ProcessId();
        options.Restored.ClusterId = options.Config.ClusterId;
        options.Restored.MemberId = options.MemberId;
        options.Restored.MembershipIncarnation = "membership";
        options.Restored.RequestedMode = ERequestedMode::Auto;
        options.Restored.Ownership = fixture.Dispatch.State.Ownership;
        options.Restored.ExclusiveHistory = true;
        // Manual gateway fixtures need exclusive ownership only. A sole global
        // reporter also requires an explicit durable placement attestation.
        UNIT_ASSERT(!options.Restored.Ownership->IndependentFailureDomain);
        TDurableCommand ownership;
        ownership.Id = "independent-placement-acknowledgement";
        ownership.Kind = ECommandKind::SetMode;
        ownership.Mode = ERequestedMode::Auto;
        ownership.ExpectedControllerRevision = options.Restored.ControllerRevision;
        ownership.Reason = "explicit unit fixture independent failure-domain attestation";
        ownership.AuditTime = fixture.Dispatch.Clock->Value.ToString();
        ownership.Ownership = options.Restored.Ownership;
        ownership.Ownership->IndependentFailureDomain = true;
        auto acknowledged = Apply(options.Restored, ownership);
        UNIT_ASSERT_C(acknowledged.Result.Ok, acknowledged.Result.Code);
        options.Restored = std::move(acknowledged.State);
        ui64 id = 0;
        options.NextId = [&] { return "controller-" + ToString(++id); };
        options.EvidenceQualification = {"global_bsc_v1", "explicit-unit-build", "fixture-not-production",
            ESourceKind::GlobalBsc, true, true, false, false};
        options.EvidenceDecoder = [](const TReadResult&, const TClusterView&) {
            return std::map<TString, TAssessment>{{"a", {EAssessment::NoFailureReported, true, {}}},
                {"b", {EAssessment::Failed, true, {}}}};
        };
        TControllerCore core(std::move(options));
        const auto view = fixture.Dispatch.Request.Attempt.Baseline;
        std::map<TString, TReadResult> admin;
        std::vector<TDispatchRequest> dispatches;
        const auto drain = [&] {
            for (size_t turn = 0; turn < 100; ++turn) {
                auto effects = core.DrainEffects();
                if (effects.Reads.empty() && effects.Stores.empty() && effects.Dispatches.empty()) {
                    return;
                }
                for (const auto& read : effects.Reads) {
                    TReadResult result;
                    result.Request = read;
                    result.Success = true;
                    result.Status = "SUCCESS";
                    result.StartedAt = fixture.Dispatch.Clock->Value;
                    result.FinishedAt = fixture.Dispatch.Clock->Value;
                    if (read.Method == EReadMethod::Config) {
                        result.Configs = {R"(config:
  bridge_config: {piles: [{name: a}, {name: b}]}
  hosts: [{host: node, node_id: 17, port: 19001, location: {bridge_pile_name: a}}]
)"};
                    } else if (read.Method == EReadMethod::Health) {
                        auto health = std::make_shared<Ydb::Monitoring::SelfCheckResult>();
                        health->set_self_check_result(Ydb::Monitoring::SelfCheck::GOOD);
                        health->mutable_location()->set_id(17);
                        health->mutable_location()->set_host("node");
                        health->mutable_location()->mutable_pile()->set_name("a");
                        result.Health = std::move(health);
                    } else if (read.Method == EReadMethod::Admin) {
                        result.Cluster = view;
                        result.Cluster->Endpoint = read.Endpoint;
                        result.Cluster->ReceivedAt = result.FinishedAt;
                        admin[read.Id] = result;
                    }
                    core.OnRead(std::move(result));
                }
                for (const auto& store : effects.Stores) {
                    UNIT_ASSERT(store.Kind == EStorageKind::Commit);
                    const auto candidate = Apply(*store.State, store.Command);
                    TStorageResult result;
                    result.Id = store.Id;
                    result.Kind = store.Kind;
                    result.Success = true;
                    result.Proposal = {EProposalStatus::Committed, candidate.Result, candidate.State, {}};
                    core.OnStorage(std::move(result));
                }
                for (auto& dispatch : effects.Dispatches) {
                    dispatches.push_back(std::move(dispatch));
                }
            }
            UNIT_FAIL("controller must not produce an unbounded effect cycle");
        };
        for (const ui64 second : {100, 101, 113, 116, 128}) {
            fixture.Dispatch.Clock->Value = TInstant::Seconds(second);
            core.Tick();
            drain();
        }
        const auto snapshot = core.Snapshot();
        UNIT_ASSERT(snapshot->Applied->Ownership->IndependentFailureDomain);
        UNIT_ASSERT(snapshot->Decision);
        UNIT_ASSERT_C(snapshot->Decision->Kind == EDecisionKind::Failover, snapshot->Decision->Reason);
        UNIT_ASSERT(snapshot->Decision->Confirmed);
        UNIT_ASSERT_VALUES_EQUAL(dispatches.size(), 1);
        UNIT_ASSERT(snapshot->Applied->UnresolvedAttempt == dispatches.front().Attempt.Id);
        UNIT_ASSERT(!snapshot->AutonomyReady);
        UNIT_ASSERT(!snapshot->MutationReady);
        UNIT_ASSERT(snapshot->DispatchReady);
        UNIT_ASSERT(snapshot->AutomaticDispatchReady);
        fixture.Resources->PublishSnapshot(snapshot);
        fixture.Dispatch.Request = dispatches.front();
        fixture.Dispatch.Preflight = admin.at(dispatches.front().Permission.PreflightRequestId);
        fixture.CachePreflight();
        for (size_t duplicate = 0; duplicate < 2; ++duplicate) {
            fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge,
                new TEvRuntime::TEvDispatchAttempt(dispatches.front())));
        }
        const auto handoff = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT(handoff->Get()->Result.HandedOff);
        UNIT_ASSERT(!handoff->Get()->Result.Terminal);
        fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Sdk->Mutations.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Sdk->Mutations.front().Id, dispatches.front().Attempt.Id);
        TRpcOutcome outcome;
        outcome.AttemptId = dispatches.front().Attempt.Id;
        outcome.Success = true;
        fixture.Sdk->MutationCompletions.front()(std::move(outcome));
        UNIT_ASSERT(fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge)->Get()->Result.Terminal);
    }

    Y_UNIT_TEST(CurrentConflictClosesAlreadyQueuedManualDispatch) {
        TGatewayActorFixture fixture;
        fixture.CachePreflight();
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge,
            new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
        auto snapshot = std::make_shared<TControllerSnapshot>(*fixture.Resources->ReadSnapshot());
        snapshot->DispatchReady = false;
        snapshot->Blockers = {"STATE_CONFLICT"};
        fixture.Resources->PublishSnapshot(snapshot);
        const auto result = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT(result->Get()->Result.NeverDispatched);
        UNIT_ASSERT_VALUES_EQUAL(result->Get()->Result.Outcome.Status, "CURRENT_READINESS_CHANGED");
        UNIT_ASSERT(fixture.Sdk->Mutations.empty());
    }

    Y_UNIT_TEST(ReadinessRecoveryCannotRevivePreviouslyQueuedGrant) {
        TGatewayActorFixture fixture;
        fixture.CachePreflight();
        auto snapshot = std::make_shared<TControllerSnapshot>(*fixture.Resources->ReadSnapshot());
        ++snapshot->Epoch.ReadinessGeneration;
        fixture.Resources->PublishSnapshot(snapshot);
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge,
            new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
        const auto result = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT(result->Get()->Result.NeverDispatched);
        UNIT_ASSERT_VALUES_EQUAL(result->Get()->Result.Outcome.Status, "READINESS_EPOCH_CHANGED");
        UNIT_ASSERT(fixture.Sdk->Mutations.empty());
    }

    Y_UNIT_TEST(AbsoluteQuorumAndLifecycleExpiryBlockStalledSnapshotDispatch) {
        for (const bool configExpires : {false, true}) {
            TGatewayActorFixture fixture;
            fixture.CachePreflight();
            auto snapshot = std::make_shared<TControllerSnapshot>(*fixture.Resources->ReadSnapshot());
            if (configExpires) {
                snapshot->ConfigValidUntil = TInstant::Seconds(102);
            } else {
                snapshot->AdminValidUntil = TInstant::Seconds(102);
            }
            fixture.Resources->PublishSnapshot(snapshot);
            fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge,
                new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
            // No controller heartbeat refresh: readiness booleans still say
            // true and the exact-node preflight remains within its own TTL.
            fixture.Dispatch.Clock->Value = TInstant::Seconds(103);
            const auto result = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
            UNIT_ASSERT(result->Get()->Result.NeverDispatched);
            UNIT_ASSERT_VALUES_EQUAL(result->Get()->Result.Outcome.Status,
                configExpires ? "CONFIG_EVIDENCE_EXPIRED" : "ADMIN_QUORUM_EXPIRED");
            UNIT_ASSERT(fixture.Sdk->Mutations.empty());
        }
    }

    Y_UNIT_TEST(AbsoluteValidityIsRecheckedImmediatelyBeforeSdkHandoff) {
        for (const bool configExpires : {false, true}) {
            TGatewayActorFixture fixture;
            fixture.CachePreflight();
            auto snapshot = std::make_shared<TControllerSnapshot>(*fixture.Resources->ReadSnapshot());
            if (configExpires) {
                snapshot->ConfigValidUntil = TInstant::Seconds(102);
            } else {
                snapshot->AdminValidUntil = TInstant::Seconds(102);
            }
            fixture.Resources->PublishSnapshot(snapshot);
            fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge,
                new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
            fixture.Dispatch.Clock->Sequence = {TInstant::Seconds(100), TInstant::Seconds(100), TInstant::Seconds(103)};
            const auto result = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
            UNIT_ASSERT(result->Get()->Result.NeverDispatched);
            UNIT_ASSERT_VALUES_EQUAL(result->Get()->Result.Outcome.Status,
                configExpires ? "CONFIG_EVIDENCE_EXPIRED" : "ADMIN_QUORUM_EXPIRED");
            UNIT_ASSERT(fixture.Sdk->Mutations.empty());
        }
    }

    Y_UNIT_TEST(EmergencyDispatchRetainsStaleUncontradictedConfigPermission) {
        TGatewayActorFixture fixture(false, true);
        fixture.CachePreflight();
        auto snapshot = std::make_shared<TControllerSnapshot>(*fixture.Resources->ReadSnapshot());
        snapshot->ConfigValidUntil = TInstant::Seconds(99);
        snapshot->ConfigStale = true;
        fixture.Resources->PublishSnapshot(snapshot);
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge,
            new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
        const auto result = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT(result->Get()->Result.HandedOff);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Sdk->Mutations.size(), 1);
        UNIT_ASSERT(fixture.Sdk->Mutations.front().Update.at("b") == EPileState::Disconnected);
    }

    Y_UNIT_TEST(RawNotFoundPreflightAuthorizesOnlyAttestedSameNodeConversion) {
        TGatewayActorFixture fixture(true);
        UNIT_ASSERT(!fixture.Dispatch.Preflight.Success);
        UNIT_ASSERT(!fixture.Dispatch.Preflight.Cluster);
        fixture.CachePreflight();
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge,
            new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
        const auto handoff = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT(handoff->Get()->Result.HandedOff);
        UNIT_ASSERT(!handoff->Get()->Result.Terminal);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Sdk->Mutations.size(), 1);
        UNIT_ASSERT(fixture.Sdk->Mutations.front().Method == EBridgeMethod::Convert);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Sdk->Mutations.front().Endpoint, fixture.Dispatch.Preflight.Request.Endpoint);
        UNIT_ASSERT(fixture.Sdk->Mutations.front().Target == std::optional<TString>("a"));
    }

    Y_UNIT_TEST(OrdinaryPollAfterPreflightDoesNotReplaceNamedPreflight) {
        TGatewayActorFixture fixture;
        fixture.CachePreflight();
        auto poll = fixture.Dispatch.Preflight.Request;
        poll.Id = "ordinary-poll";
        poll.OperationPriority = false;
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge, new TEvRuntime::TEvGatewayRead(poll)));
        poll.Id = "overlapping-poll";
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge, new TEvRuntime::TEvGatewayRead(poll)));
        UNIT_ASSERT(fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvGatewayReadResult>(fixture.Edge)->Get()->Result.Busy);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Sdk->Reads.size(), 2);
        auto polled = fixture.Dispatch.Preflight;
        polled.Request = fixture.Sdk->Reads.back();
        fixture.Sdk->ReadCompletions.back()(std::move(polled));
        UNIT_ASSERT(fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvGatewayReadResult>(fixture.Edge)->Get()->Result.Success);
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge,
            new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
        const auto handoff = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT_C(handoff->Get()->Result.HandedOff, handoff->Get()->Result.Outcome.Status);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Sdk->Mutations.size(), 1);
    }

    Y_UNIT_TEST(RawNotFoundCannotAuthorizeOrdinaryMutation) {
        TGatewayActorFixture fixture;
        fixture.Dispatch.Preflight.Success = false;
        fixture.Dispatch.Preflight.Cluster.reset();
        fixture.Dispatch.Preflight.Status = "NOT_FOUND";
        fixture.CachePreflight();
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge,
            new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
        const auto result = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT(result->Get()->Result.NeverDispatched);
        UNIT_ASSERT_VALUES_EQUAL(result->Get()->Result.Outcome.Status, "SAME_NODE_PREFLIGHT_REQUIRED");
        UNIT_ASSERT(fixture.Sdk->Mutations.empty());
    }

    Y_UNIT_TEST(NotFoundConversionPreflightRetainsExactCorrelationGuards) {
        const TDispatchFixture baseline(true);
        UNIT_ASSERT_VALUES_EQUAL(baseline.Validate(), "");
        for (const auto field : {0, 1, 2}) {
            auto changed = baseline;
            if (field == 0) {
                changed.Preflight.Request.Id = "other-read";
            } else if (field == 1) {
                changed.Preflight.Request.ProcessId = "other-process";
            } else {
                changed.Preflight.Request.Endpoint = "grpc://other:2135";
            }
            UNIT_ASSERT_VALUES_EQUAL(changed.Validate(), "SAME_NODE_PREFLIGHT_REQUIRED");
        }
    }

    Y_UNIT_TEST(GatewaySerializesClosureBeforeEveryQueuedLateDispatch) {
        TGatewayActorFixture fixture;
        fixture.CachePreflight();
        fixture.SendClose();
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge, new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
        UNIT_ASSERT(fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge)->Get()->Result.NeverDispatched);
        UNIT_ASSERT(fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge)->Get()->Result.NeverDispatched);
        UNIT_ASSERT(fixture.Sdk->Mutations.empty());
    }

    Y_UNIT_TEST(GatewayHandoffWinsClosureAndFutureRemainsCorrelated) {
        TGatewayActorFixture fixture;
        fixture.CachePreflight();
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge, new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
        fixture.SendClose();
        const auto handoff = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT(handoff->Get()->Result.HandedOff);
        UNIT_ASSERT(!handoff->Get()->Result.Terminal);
        UNIT_ASSERT_VALUES_EQUAL(fixture.Sdk->Mutations.size(), 1);
        UNIT_ASSERT(!fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge)->Get()->Result.NeverDispatched);
        TRpcOutcome outcome;
        outcome.AttemptId = "attempt";
        outcome.Success = true;
        fixture.Sdk->MutationCompletions.front()(outcome);
        const auto terminal = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT(terminal->Get()->Result.Terminal);
        UNIT_ASSERT(terminal->Get()->Result.Outcome.Success);
        UNIT_ASSERT_VALUES_EQUAL(terminal->Get()->Result.AttemptId, "attempt");
    }

    Y_UNIT_TEST(IndependentLocalStopBlocksAlreadyQueuedDispatch) {
        TGatewayActorFixture fixture;
        fixture.CachePreflight();
        fixture.Runtime.SendAsync(new NActors::IEventHandle(fixture.Gateway, fixture.Edge, new TEvRuntime::TEvDispatchAttempt(fixture.Dispatch.Request)));
        fixture.Resources->StopLocal("stop", {}, "stop-hash", "emergency");
        UNIT_ASSERT(fixture.Resources->StopLatch->IsStopped());
        const auto result = fixture.Runtime.GrabEdgeEvent<TEvRuntime::TEvMutationResult>(fixture.Edge);
        UNIT_ASSERT(result->Get()->Result.NeverDispatched);
        UNIT_ASSERT(fixture.Sdk->Mutations.empty());
    }
}

} // namespace NBridgeSkipper
