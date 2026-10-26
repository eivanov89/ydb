#include <ydb/apps/bridge_skipper/http/http.h>
#include <ydb/apps/bridge_skipper/http/internal.h>

#include <library/cpp/testing/unittest/registar.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>
#include <ydb/library/actors/http/http_proxy.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <util/folder/tempdir.h>
#include <util/string/cast.h>
#include <sys/stat.h>
#include <limits>

namespace NBridgeSkipper {
namespace {

TString Uuid(ui64 index) {
    auto suffix = ToString(index);
    return "00000000-0000-0000-0000-" + TString(12 - suffix.size(), '0') + suffix;
}

class TClock final : public ITimeProvider {
public:
    TInstant Value = TInstant::Seconds(100000);

    TInstant Now() override {
        return Value;
    }
};

struct TFixture {
    TIntrusivePtr<TClock> Clock = new TClock;
    TControllerSnapshot Snapshot;
    TLocalStopReceipt Stop;
    TConfig Config;
    std::unique_ptr<THttpApi> Api;
    ui64 Id = 100;
    size_t StopCalls = 0;
    std::map<TString, TLocalStopReceipt> LocalReceipts;

    explicit TFixture(TString key = TString(32, 'a')) {
        TSkipperClock::RegisterProvider(Clock);
        Config.ClusterId = "cluster";
        Config.YdbEndpoints.push_back(ParseEndpoint("grpc://localhost:2135"));
        auto state = std::make_shared<TDurableState>();
        state->ClusterId = "cluster";
        state->MemberId = "member";
        state->MembershipIncarnation = Uuid(2);
        state->ControllerRevision = 5;
        TOwnershipAcknowledgement ack;
        ack.ExclusiveAdministration = true;
        ack.DeploymentPlanReference = "deployment-plan";
        state->Ownership = ack;
        state->ExclusiveHistory = true;
        Snapshot.Applied = state;
        Snapshot.ClusterId = "cluster";
        Snapshot.IncarnationId = Uuid(1);
        Snapshot.ControllerRevision = 5;
        Snapshot.TopologyRevision = 3;
        Snapshot.SnapshotRevision = 20;
        Snapshot.PolicyRevision = "policy-hash";
        Snapshot.Epoch.ReadinessGeneration = 7;
        Snapshot.Topology.ConfiguredPiles = {"a", "b", ""};
        Snapshot.ConfigStale = false;
        Snapshot.TopologyConfirmed = true;
        Snapshot.AuthorityReady = true;
        Snapshot.MutationReady = true;
        Snapshot.EffectiveState = "ready_manual";
        Snapshot.GeneratedAt = Clock->Value;
        TClusterView cluster;
        cluster.Converted = true;
        cluster.Generation = std::numeric_limits<ui64>::max() - 10;
        cluster.Piles = {{"a", EPileState::Primary}, {"b", EPileState::Synchronized}, {"", EPileState::Disconnected}};
        Snapshot.Cluster = cluster;
        Stop.ProcessId = Snapshot.IncarnationId;
        Stop.Stopped = false;
        TApiOptions options;
        options.Config = Config;
        options.ProcessId = Snapshot.IncarnationId;
        options.SigningKey = std::move(key);
        options.NextId = [this]() {
            return Uuid(++Id);
        };
        options.LookupLocalReceipt = [this](const TString& id) -> std::optional<TLocalStopReceipt> {
            const auto found = LocalReceipts.find(id);
            return found == LocalReceipts.end() ? std::nullopt : std::optional<TLocalStopReceipt>(found->second);
        };
        options.Stop = [this](const TString& id, const TString& hash, const TString& reason) {
            ++StopCalls;
            Stop.RequestId = id;
            Stop.PayloadHash = hash;
            Stop.Reason = reason;
            ++Stop.Revision;
            Stop.Stopped = true;
            Stop.Durable = false;
            LocalReceipts[id] = Stop;
            return Stop;
        };
        options.Release = [this](const TString& id, const TString& process, ui64 revision, const TString& hash, const TString& reason) {
            auto receipt = Stop;
            receipt.RequestId = id;
            receipt.PayloadHash = hash;
            receipt.Reason = reason;
            if (process != Snapshot.IncarnationId || revision != Stop.Revision) {
                receipt.Error = "STALE_LOCAL_STOP";
            } else {
                receipt.Pending = true;
            }
            LocalReceipts[id] = receipt;
            return receipt;
        };
        Api = std::make_unique<THttpApi>(std::move(options));
    }

    TApiAction Call(TString method, TString path, const TApiJson& body = Null()) {
        TApiRequest request;
        request.Method = std::move(method);
        request.Target = std::move(path);
        request.ContentType = "application/json";
        if (!body.IsNull()) {
            request.Body = CanonicalApiJson(body);
        }
        return Api->Handle(request, Snapshot, Stop);
    }

    TApiJson Preview(TApiJson request) {
        const auto response = Call("POST", "/api/plans", request);
        UNIT_ASSERT_C(response.Response.Status == 200, response.Response.Body);
        return ParseStrictJson(response.Response.Body);
    }

    TApiJson Action(TString action, std::optional<TString> target = {}) {
        auto value = Object();
        value["action"] = std::move(action);
        if (target) {
            value["target"] = *target;
        }
        return value;
    }

    TApiJson Admission(const TApiJson& preview, ui64 id = 50) {
        auto value = Object();
        value["preview"] = preview["preview"];
        value["request_id"] = Uuid(id);
        auto ack = Object();
        ack["confirmed"] = preview["required_acknowledgements"];
        ack["typed_pile_name"] = Null();
        ack["empty_name"] = false;
        ack["deletion"] = false;
        value["acknowledgements"] = std::move(ack);
        return value;
    }

    std::shared_ptr<TDurableState> MutableState() {
        auto state = std::make_shared<TDurableState>(*Snapshot.Applied);
        Snapshot.Applied = state;
        return state;
    }
};

TString PauseEvidenceWorkflow(TFixture& fixture, bool automatic) {
    auto state = fixture.MutableState();
    state->RequestedMode = automatic ? ERequestedMode::Auto : ERequestedMode::Manual;
    fixture.Snapshot.Cluster->Endpoint = "grpc://localhost:2135";
    TPlanRequest request;
    request.Kind = EOperationKind::Failover;
    request.FailedTargets = {"b"};
    request.Automatic = automatic;
    request.ExternalPrerequisitesAcknowledged = true;
    auto plan = *PlanWorkflow(*fixture.Snapshot.Cluster, request).Plan;
    plan.EvidenceBound = true;
    auto commit = [&](TDurableCommand command) {
        command.ExpectedControllerRevision = state->ControllerRevision;
        command.Ready = command.Fresh = command.Coherent = true;
        const auto result = Apply(*state, command);
        UNIT_ASSERT_C(result.Result.Ok, result.Result.Code);
        *state = result.State;
    };
    TDurableCommand admit;
    admit.Id = Uuid(71);
    admit.Kind = ECommandKind::AdmitOperation;
    admit.OperationId = Uuid(70);
    admit.Plan = plan;
    commit(admit);
    TDurableCommand intent;
    intent.Id = Uuid(72);
    intent.Kind = ECommandKind::AdmitAttempt;
    intent.OperationId = admit.OperationId;
    intent.ExpectedWorkflowRevision = state->Operations.at(admit.OperationId).Revision;
    auto& attempt = intent.Attempt;
    attempt.Id = Uuid(73);
    attempt.OperationId = admit.OperationId;
    attempt.Method = EBridgeMethod::UpdateClusterState;
    attempt.Update = plan.Steps[0].Update;
    attempt.Endpoint = fixture.Snapshot.Cluster->Endpoint;
    attempt.NodeId = 1;
    attempt.DesignatedMember = state->MemberId;
    attempt.DesignatedProcess = fixture.Snapshot.IncarnationId;
    attempt.Baseline = *fixture.Snapshot.Cluster;
    attempt.ArgumentsHash = MutationArgumentsHash(attempt);
    commit(intent);
    TDurableCommand close;
    close.Id = Uuid(74);
    close.Kind = ECommandKind::NeverDispatched;
    close.Attempt = attempt;
    close.GatewayProof = "fixture gateway serialized negative closure";
    // Auto mode would requeue automatic work instead of pausing it.
    state->RequestedMode = ERequestedMode::Manual;
    commit(close);
    state->RequestedMode = automatic ? ERequestedMode::Auto : ERequestedMode::Manual;
    fixture.Snapshot.ControllerRevision = state->ControllerRevision;

    TDecision decision;
    decision.Id = Uuid(20);
    decision.Kind = EDecisionKind::Failover;
    decision.Confirmed = true;
    decision.Candidate = "a";
    decision.Primary = "a";
    decision.Targets = {"b"};
    decision.Update = plan.Steps[0].Update;
    decision.Epoch = fixture.Snapshot.Epoch;
    decision.EvidenceExpiry = fixture.Clock->Value + TDuration::Seconds(20);
    decision.RoundStartedAt = fixture.Clock->Value - TDuration::Seconds(1);
    decision.EvidenceRequestIds = {Uuid(30)};
    TNode endpoint;
    endpoint.Endpoint = fixture.Snapshot.Cluster->Endpoint;
    endpoint.NodeId = 1;
    endpoint.Pile = "a";
    decision.Endpoint = endpoint;
    fixture.Snapshot.Decision = decision;
    return admit.OperationId;
}

} // namespace

Y_UNIT_TEST_SUITE(SkipperHttp) {
    Y_UNIT_TEST(StrictJsonRejectsDuplicateEscapedAndNestedKeys) {
        for (const TString& text : {TString("{\"x\":1,\"x\":2}"), TString("{\"x\":1,\"\\u0078\":2}"), TString("{\"a\":[{\"b\":1,\"b\":2}]}"), TString("{\"a\":NaN}"), TString("[]")}) {
            UNIT_ASSERT_EXCEPTION(ParseStrictJson(text), TApiException);
        }
        UNIT_ASSERT(ParseStrictJson("{\"x\":\"\",\"a\":[{},null,true]}").IsMap());
    }

    Y_UNIT_TEST(StrictJsonRejectsInvalidUtf8OversizeAndDeepNesting) {
        UNIT_ASSERT_EXCEPTION(ParseStrictJson(TString("{\"x\":\"") + TString(1, '\xff') + "\"}"), TApiException);
        UNIT_ASSERT_EXCEPTION(ParseStrictJson(TString(1024 * 1024 + 1, ' ')), TApiException);
        UNIT_ASSERT_EXCEPTION(ParseStrictJson("{\"x\":" + TString(40, '[') + "0" + TString(40, ']') + "}"), TApiException);
        UNIT_ASSERT_EXCEPTION(Decimal("01"), TApiException);
        UNIT_ASSERT_EXCEPTION(Decimal("18446744073709551616"), TApiException);
        UNIT_ASSERT_VALUES_EQUAL(Decimal("18446744073709551615"), std::numeric_limits<ui64>::max());
    }

    Y_UNIT_TEST(UuidsAndActionSchemasAreStrict) {
        UNIT_ASSERT(IsRequestUuid(Uuid(1)));
        UNIT_ASSERT(!IsRequestUuid(""));
        UNIT_ASSERT(!IsRequestUuid("not-a-uuid"));
        TFixture f;
        auto action = f.Action("takedown", "b");
        action["username"] = "operator";
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/plans", action).Response.Status, 400);
        action = f.Action("takedown", "b");
        auto preview = f.Preview(action);
        auto body = f.Admission(preview);
        body["request_id"] = "123";
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", body).Response.Status, 400);
    }

    Y_UNIT_TEST(ContentTypeAndAllApiSecurityHeaders) {
        TFixture f;
        TApiRequest request;
        request.Method = "POST";
        request.Target = "/api/plans";
        request.ContentType = "text/plain";
        request.Body = "{}";
        auto response = f.Api->Handle(request, f.Snapshot, f.Stop).Response;
        UNIT_ASSERT_VALUES_EQUAL(response.Status, 415);
        UNIT_ASSERT_VALUES_EQUAL(ParseStrictJson(response.Body)["api_version"].GetInteger(), 1);
        UNIT_ASSERT_VALUES_EQUAL(response.Headers.at("Cache-Control"), "no-store");
        UNIT_ASSERT_STRING_CONTAINS(response.Headers.at("Content-Security-Policy"), "frame-ancestors 'none'");
        UNIT_ASSERT_VALUES_EQUAL(response.Headers.at("X-Content-Type-Options"), "nosniff");
        request.ContentType = "Application/JSON; charset=utf-8";
        request.Body = CanonicalApiJson(f.Action("takedown", "b"));
        UNIT_ASSERT_VALUES_EQUAL(f.Api->Handle(request, f.Snapshot, f.Stop).Response.Status, 200);
    }

    Y_UNIT_TEST(UiRoutesAreUnavailableAndReadOnlyMonitorExists) {
        TFixture f;
        for (const TString& path : {TString("/"), TString("/app.css"), TString("/app.js"), TString("/logic.js"), TString("/tests/index.html"), TString("/api/mock/state"), TString("/mon/exec"), TString("/api/apply")}) {
            UNIT_ASSERT_VALUES_EQUAL(f.Call("GET", path).Response.Status, 404);
        }
        UNIT_ASSERT_VALUES_EQUAL(f.Call("GET", "/mon/").Response.Status, 200);
    }

    Y_UNIT_TEST(ManualPreviewLives120SecondsWithoutHealthIds) {
        TFixture f;
        const auto preview = f.Preview(f.Action("takedown", "b"));
        UNIT_ASSERT(preview["dispatchable"].GetBoolean());
        UNIT_ASSERT_VALUES_EQUAL(TInstant::ParseIso8601(preview["expires_at"].GetString()), f.Clock->Value + TDuration::Seconds(120));
        f.Clock->Value += TDuration::Seconds(50);
        f.Snapshot.GeneratedAt = f.Clock->Value;
        ++f.Snapshot.SnapshotRevision;
        const auto admission = f.Call("POST", "/api/operations", f.Admission(preview));
        UNIT_ASSERT(admission.Submission);
        UNIT_ASSERT(admission.Submission->PlanRequest->ExternalPrerequisitesAcknowledged);
        UNIT_ASSERT(admission.Submission->PlanRequest->Target == "b");
    }

    Y_UNIT_TEST(SignatureTamperingRestartAndExpiryAreStalePlans) {
        TFixture f;
        auto preview = f.Preview(f.Action("takedown", "b"));
        auto body = f.Admission(preview);
        auto encoded = body["preview"].GetString();
        encoded.back() = encoded.back() == 'A' ? 'B' : 'A';
        body["preview"] = encoded;
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", body).Response.Status, 409);
        TFixture restarted(TString(32, 'b'));
        UNIT_ASSERT_VALUES_EQUAL(restarted.Call("POST", "/api/operations", f.Admission(preview)).Response.Status, 409);
        TSkipperClock::RegisterProvider(f.Clock);
        f.Clock->Value += TDuration::Seconds(121);
        f.Snapshot.GeneratedAt = f.Clock->Value;
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", f.Admission(preview)).Response.Status, 409);
    }

    Y_UNIT_TEST(EveryAuthoritativeBindingInvalidatesManualPreview) {
        for (unsigned change = 0; change < 6; ++change) {
            TFixture f;
            const auto preview = f.Preview(f.Action("takedown", "b"));
            switch (change) {
                case 0: f.Snapshot.IncarnationId = Uuid(9); break;
                case 1: ++f.Snapshot.TopologyRevision; break;
                case 2: ++f.Snapshot.Epoch.ReadinessGeneration; break;
                case 3: ++f.MutableState()->ControllerRevision; break;
                case 4: f.Snapshot.PolicyRevision = "new-policy"; break;
                case 5: f.Snapshot.Cluster->Pending.push_back({"ADD", TString("b"), "new error", {}}); break;
            }
            UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", f.Admission(preview)).Response.Status, 409);
        }
    }

    Y_UNIT_TEST(DurableReceiptLookupPrecedesSignatureExpiryAndRestartChecks) {
        TFixture f;
        const auto preview = f.Preview(f.Action("takedown", "b"));
        auto body = f.Admission(preview);
        const auto original = f.Call("POST", "/api/operations", body);
        UNIT_ASSERT(original.Submission);
        TFixture restarted(TString(32, 'z'));
        const auto state = restarted.MutableState();
        TCommandResult result;
        result.Ok = true;
        result.Index = 99;
        result.ControllerRevision = 9;
        result.OperationId = Uuid(70);
        state->Requests[Uuid(50)] = {original.Submission->Command.RequestHash, result};
        restarted.Clock->Value += TDuration::Seconds(1000);
        const auto duplicate = restarted.Call("POST", "/api/operations", body);
        UNIT_ASSERT_VALUES_EQUAL(duplicate.Response.Status, 202);
        UNIT_ASSERT_VALUES_EQUAL(ParseStrictJson(duplicate.Response.Body)["operation_id"].GetString(), Uuid(70));
        body["preview"] = "different-envelope";
        UNIT_ASSERT_VALUES_EQUAL(restarted.Call("POST", "/api/operations", body).Response.Status, 409);
    }

    Y_UNIT_TEST(PendingAdmissionLookupAndReplayStayUnknownAfterExpiry) {
        TFixture f;
        const auto preview = f.Preview(f.Action("takedown", "b"));
        const auto body = f.Admission(preview);
        const auto original = f.Call("POST", "/api/operations", body);
        UNIT_ASSERT(original.Submission);
        f.Api->BeginAdmission(Uuid(50), original.Submission->Command.RequestHash);
        f.Clock->Value += TDuration::Seconds(1000);
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", body).Response.Status, 503);
        UNIT_ASSERT_VALUES_EQUAL(f.Call("GET", "/api/requests/" + Uuid(50)).Response.Status, 503);
        f.Api->EndAdmission(Uuid(50));
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", body).Response.Status, 409);
    }

    Y_UNIT_TEST(DeletionNeedsEveryExactServerValidatedAcknowledgement) {
        TFixture f;
        auto preview = f.Preview(f.Action("remove_pile", TString()));
        auto body = f.Admission(preview);
        body["acknowledgements"]["typed_pile_name"] = "";
        body["acknowledgements"]["deletion"] = true;
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", body).Response.Status, 422);
        body["acknowledgements"]["empty_name"] = true;
        const auto accepted = f.Call("POST", "/api/operations", body);
        UNIT_ASSERT(accepted.Submission);
        UNIT_ASSERT(accepted.Submission->PlanRequest->Target->empty());
        UNIT_ASSERT(accepted.Submission->PlanRequest->TypedPileName->empty());
        body["acknowledgements"]["typed_pile_name"] = " ";
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", body).Response.Status, 422);
        auto ordinary = f.Admission(f.Preview(f.Action("takedown", "b")));
        ordinary["acknowledgements"]["deletion"] = true;
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", ordinary).Response.Status, 400);
    }

    Y_UNIT_TEST(BlockedPreviewsRemainExplanatory200AndSubmit422) {
        TFixture f;
        f.Stop.Stopped = true;
        const auto preview = f.Preview(f.Action("takedown", "b"));
        UNIT_ASSERT(!preview["dispatchable"].GetBoolean());
        UNIT_ASSERT(!preview["blockers"].GetArray().empty());
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", f.Admission(preview)).Response.Status, 422);
        auto bad = f.Preview(f.Action("rejoin", "b"));
        UNIT_ASSERT(!bad["dispatchable"].GetBoolean());
    }

    Y_UNIT_TEST(AllEightManualActionsProduceExactServerPlans) {
        TFixture f;
        auto view = *f.Snapshot.Cluster;
        auto failover = f.Action("failover");
        failover["targets"] = Array();
        failover["targets"].AppendValue("b");
        failover["successor"] = Null();
        auto maintenance = f.Action("primary_maintenance", "a");
        maintenance["successor"] = "b";
        for (auto action : {failover, f.Action("switchover", "b"), f.Action("takedown", "b"), maintenance, f.Action("rejoin", TString()), f.Action("remove_pile", TString())}) {
            const auto preview = f.Preview(action);
            UNIT_ASSERT(preview["dispatchable"].GetBoolean());
            UNIT_ASSERT(!preview["steps"].GetArray().empty());
        }
        f.Snapshot.Cluster->Piles[""] = EPileState::Inactive;
        UNIT_ASSERT(f.Preview(f.Action("add_pile", TString()))["dispatchable"].GetBoolean());
        f.Snapshot.Cluster = view;
        f.Snapshot.Cluster->Converted = false;
        f.Snapshot.Cluster->Piles = {{"a", EPileState::Primary}};
        f.Snapshot.Topology.ConfiguredPiles = {"a"};
        UNIT_ASSERT(f.Preview(f.Action("convert"))["dispatchable"].GetBoolean());
    }

    Y_UNIT_TEST(DecisionPreviewsBindExactDecisionAndEvidenceExpiry) {
        TFixture f;
        TDecision decision;
        decision.Id = Uuid(20);
        decision.Kind = EDecisionKind::Failover;
        decision.Confirmed = true;
        decision.Targets = {"b"};
        decision.Candidate = "a";
        decision.Update = {{"b", EPileState::Disconnected}};
        decision.EvidenceExpiry = f.Clock->Value + TDuration::Seconds(20);
        decision.EvidenceRequestIds = {Uuid(30)};
        decision.RoundStartedAt = f.Clock->Value - TDuration::Seconds(1);
        decision.Endpoint = TNode();
        f.Snapshot.Decision = decision;
        auto action = f.Action("apply_decision");
        action["decision_id"] = decision.Id;
        const auto preview = f.Preview(action);
        UNIT_ASSERT_VALUES_EQUAL(TInstant::ParseIso8601(preview["expires_at"].GetString()), decision.EvidenceExpiry);
        auto accepted = f.Call("POST", "/api/operations", f.Admission(preview));
        UNIT_ASSERT(accepted.Submission);
        UNIT_ASSERT(accepted.Submission->DecisionId == decision.Id);
        f.Snapshot.Decision->EvidenceRequestIds = {Uuid(31)};
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", f.Admission(preview)).Response.Status, 409);
    }

    Y_UNIT_TEST(ReadTimeAgesAndStallMaskChangeAtSameRevision) {
        TFixture f;
        auto first = ParseStrictJson(f.Call("GET", "/api/state").Response.Body);
        UNIT_ASSERT(!first["read_status"]["controller_stalled"].GetBoolean());
        f.Clock->Value += TDuration::MilliSeconds(5001);
        const auto second = ParseStrictJson(f.Call("GET", "/api/state").Response.Body);
        UNIT_ASSERT_VALUES_EQUAL(first["snapshot_revision"].GetString(), second["snapshot_revision"].GetString());
        UNIT_ASSERT(second["read_status"]["controller_stalled"].GetBoolean());
        UNIT_ASSERT(!second["controller"]["authority"]["ready"].GetBoolean());
        UNIT_ASSERT_VALUES_EQUAL(second["cluster"]["generation"].GetString(), ToString(std::numeric_limits<ui64>::max() - 10));
    }

    Y_UNIT_TEST(AbsentViewIsUnknownAndUnsupportedStatesRemainVisible) {
        TFixture f;
        f.Snapshot.Cluster.reset();
        auto body = ParseStrictJson(f.Call("GET", "/api/state").Response.Body);
        UNIT_ASSERT_VALUES_EQUAL(body["cluster"]["bridge_state"].GetString(), "UNKNOWN");
        TClusterView unsupported;
        unsupported.Converted = true;
        unsupported.Piles = {{"a", EPileState::Primary}, {"b", static_cast<EPileState>(99)}};
        f.Snapshot.UnsupportedCluster = unsupported;
        body = ParseStrictJson(f.Call("GET", "/api/state").Response.Body);
        bool found = false;
        for (const auto& pile : body["piles"].GetArray()) {
            if (pile["name"].GetString() == "b") {
                found = true;
                UNIT_ASSERT_VALUES_EQUAL(pile["state"].GetString(), "UNKNOWN");
            }
        }
        UNIT_ASSERT(found);
    }

    Y_UNIT_TEST(LocalStopBypassesAuthorityAndKeepsVolatileDuplicateReceipt) {
        TFixture f;
        f.Snapshot.AuthorityReady = false;
        f.Snapshot.MutationReady = false;
        f.Snapshot.GeneratedAt = TInstant();
        auto body = Object();
        body["request_id"] = Uuid(60);
        body["reason"] = "emergency stop";
        auto first = f.Call("POST", "/api/local-stop", body);
        UNIT_ASSERT_VALUES_EQUAL(first.Response.Status, 200);
        UNIT_ASSERT(f.Stop.Stopped);
        UNIT_ASSERT_VALUES_EQUAL(ParseStrictJson(first.Response.Body)["receipt_durability"].GetString(), "volatile");
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/local-stop", body).Response.Body, first.Response.Body);
        UNIT_ASSERT_VALUES_EQUAL(f.StopCalls, 1);
        body["reason"] = "changed";
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/local-stop", body).Response.Status, 409);
    }

    Y_UNIT_TEST(LocalStopReleaseCannotClaimCompletedPersistenceOrClearNewerStop) {
        TFixture f;
        f.Stop.Stopped = true;
        f.Stop.Revision = 3;
        auto body = Object();
        body["request_id"] = Uuid(61);
        body["incarnation_id"] = Uuid(1);
        body["stop_revision"] = "2";
        body["reason"] = "release";
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/local-stop/release", body).Response.Status, 409);
        body["request_id"] = Uuid(62);
        body["stop_revision"] = "3";
        const auto response = f.Call("POST", "/api/local-stop/release", body).Response;
        UNIT_ASSERT_VALUES_EQUAL(response.Status, 202);
        UNIT_ASSERT(ParseStrictJson(response.Body)["latched"].GetBoolean());
        UNIT_ASSERT(ParseStrictJson(response.Body)["persistence_pending"].GetBoolean());
    }

    Y_UNIT_TEST(AuditPaginationAndLogOverflowHaveExactDecimalCursors) {
        TFixture f;
        auto audit = f.Call("GET", "/api/audit?after=9&limit=1000");
        UNIT_ASSERT(audit.ReadAudit);
        UNIT_ASSERT_VALUES_EQUAL(audit.AuditAfter, 9);
        UNIT_ASSERT_VALUES_EQUAL(audit.AuditLimit, 1000);
        UNIT_ASSERT_VALUES_EQUAL(f.Call("GET", "/api/audit?limit=1001").Response.Status, 400);
        UNIT_ASSERT_VALUES_EQUAL(f.Call("GET", "/api/audit?after=1&after=2").Response.Status, 400);
        TDiagnosticRing ring(2);
        for (unsigned index = 0; index < 3; ++index) {
            auto line = Object();
            line["event"] = "transition";
            ring.Append(line);
        }
        auto lines = ring.Read(0, 2, false);
        UNIT_ASSERT(lines["overflow"].GetBoolean());
        UNIT_ASSERT_VALUES_EQUAL(lines["oldest"].GetString(), "2");
        UNIT_ASSERT_VALUES_EQUAL(lines["next"].GetString(), "3");
        UNIT_ASSERT(ring.Read(100, 2, false)["reset"].GetBoolean());
        UNIT_ASSERT(ring.Read(0, 2, true)["reset"].GetBoolean());
        UNIT_ASSERT_VALUES_EQUAL(BoundedDiagnostic("Bearer secret-token /Users/private.pem"), "[redacted diagnostic]");
    }

    Y_UNIT_TEST(OperationCreationOrderUsesDurableIndexAndAuditSurvivesRoundtrip) {
        TFixture f;
        auto state = f.MutableState();
        state->AppliedIndex = 17;
        for (unsigned index = 0; index < 2; ++index) {
            TOperation operation;
            operation.Id = Uuid(index == 0 ? 90 : 10);
            operation.CreatedIndex = index + 1;
            operation.CreatedAt = f.Clock->Value.ToString();
            operation.Plan.EvidenceBound = true;
            operation.Audit.push_back({Uuid(index + 200), index + 1, operation.CreatedAt, "OPERATION_ADMITTED", "test"});
            state->Operations[operation.Id] = operation;
        }
        const auto restored = DeserializeState(SerializeState(*state));
        UNIT_ASSERT(restored.Operations.at(Uuid(90)).Plan.EvidenceBound);
        const auto listed = ParseStrictJson(f.Call("GET", "/api/operations?limit=1").Response.Body);
        UNIT_ASSERT_VALUES_EQUAL(listed["items"].GetArray().front()["id"].GetString(), Uuid(90));
        UNIT_ASSERT_VALUES_EQUAL(listed["next"].GetString(), "1");
        UNIT_ASSERT(listed["has_more"].GetBoolean());
        UNIT_ASSERT_VALUES_EQUAL(listed["state_index"].GetString(), "17");
        UNIT_ASSERT_VALUES_EQUAL(listed["items"].GetArray().front()["state_index"].GetString(), "17");
        const auto next = ParseStrictJson(f.Call("GET", "/api/operations?after=1&limit=1").Response.Body);
        UNIT_ASSERT_VALUES_EQUAL(next["items"].GetArray().front()["id"].GetString(), Uuid(10));
        UNIT_ASSERT_VALUES_EQUAL(next["items"].GetArray().front()["audit"].GetArray().size(), 1);
    }

    Y_UNIT_TEST(ResumeIsBoundToSameOperationRevisionAndRemainingStep) {
        TFixture f;
        auto state = f.MutableState();
        auto request = TPlanRequest();
        request.Kind = EOperationKind::PrimaryMaintenance;
        request.Target = "a";
        request.Successor = "b";
        request.ExternalPrerequisitesAcknowledged = true;
        TOperation operation;
        operation.Id = Uuid(70);
        operation.Plan = *PlanWorkflow(*f.Snapshot.Cluster, request).Plan;
        operation.Status = EOperationStatus::Paused;
        operation.Revision = 7;
        operation.NextStep = 1;
        state->Operations[operation.Id] = operation;
        state->OrdinaryOwner = operation.Id;
        f.Snapshot.Cluster->Piles["a"] = EPileState::Synchronized;
        f.Snapshot.Cluster->Piles["b"] = EPileState::Primary;
        auto requestBody = f.Action("resume_workflow");
        requestBody["operation_id"] = operation.Id;
        const auto preview = f.Preview(requestBody);
        auto admission = f.Admission(preview);
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations", admission).Response.Status, 409);
        const auto accepted = f.Call("POST", "/api/operations/" + operation.Id + "/resume", admission);
        UNIT_ASSERT(accepted.Submission);
        UNIT_ASSERT(accepted.Submission->Command.Kind == ECommandKind::Resume);
        UNIT_ASSERT_VALUES_EQUAL(accepted.Submission->Command.OperationId, operation.Id);
        UNIT_ASSERT_VALUES_EQUAL(accepted.Submission->Command.ExpectedWorkflowRevision, 7);
        f.Snapshot.ConfigStale = true;
        UNIT_ASSERT(!f.Preview(requestBody)["dispatchable"].GetBoolean());
        f.Snapshot.ConfigStale = false;
        ++state->Operations.at(operation.Id).Revision;
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations/" + operation.Id + "/resume", admission).Response.Status, 409);
        state->Operations.at(operation.Id).Status = EOperationStatus::Preempted;
        UNIT_ASSERT(!f.Preview(requestBody)["dispatchable"].GetBoolean());
    }

    Y_UNIT_TEST(EvidenceBoundResumeSelectsFreshMatchingDecisionWithoutReplanning) {
        for (bool automatic : {false, true}) {
            TFixture f;
            const auto operationId = PauseEvidenceWorkflow(f, automatic);
            const auto before = SerializeState(*f.Snapshot.Applied);
            const auto& operation = f.Snapshot.Applied->Operations.at(operationId);
            UNIT_ASSERT(operation.Status == EOperationStatus::Paused);
            UNIT_ASSERT(f.Snapshot.Applied->Attempts.at(operation.Attempts.front()).Disposition == EAttemptDisposition::NeverDispatched);
            auto action = f.Action("resume_workflow");
            action["operation_id"] = operationId;
            const auto preview = f.Preview(action);
            UNIT_ASSERT(preview["dispatchable"].GetBoolean());
            UNIT_ASSERT_VALUES_EQUAL(preview["bindings"]["decision_id"].GetString(), f.Snapshot.Decision->Id);
            UNIT_ASSERT_VALUES_EQUAL(TInstant::ParseIso8601(preview["expires_at"].GetString()), f.Snapshot.Decision->EvidenceExpiry);
            UNIT_ASSERT_VALUES_EQUAL(CanonicalApiJson(preview["steps"].GetArray().front()["updates"]), CanonicalApiJson(NamedStates(operation.Plan.Steps[operation.NextStep].Update)));
            const auto response = f.Call("POST", "/api/operations/" + operationId + "/resume", f.Admission(preview));
            UNIT_ASSERT(response.Submission);
            const auto& submission = *response.Submission;
            UNIT_ASSERT(submission.Command.Kind == ECommandKind::Resume);
            UNIT_ASSERT_VALUES_EQUAL(submission.Command.OperationId, operationId);
            UNIT_ASSERT_VALUES_EQUAL(submission.Command.ExpectedWorkflowRevision, operation.Revision);
            UNIT_ASSERT(!submission.PlanRequest);
            UNIT_ASSERT(submission.DecisionId == f.Snapshot.Decision->Id);
            UNIT_ASSERT_VALUES_EQUAL(SerializeState(*f.Snapshot.Applied), before);
        }
    }

    Y_UNIT_TEST(EvidenceBoundResumeRejectsUnavailableOrMismatchingReplacementDecision) {
        for (unsigned replacement : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) {
            TFixture f;
            const auto operationId = PauseEvidenceWorkflow(f, false);
            auto action = f.Action("resume_workflow");
            action["operation_id"] = operationId;
            const auto original = f.Preview(action);
            switch (replacement) {
                case 0:
                    f.Snapshot.Decision.reset();
                    break;
                case 1:
                    f.Snapshot.Decision->Confirmed = false;
                    break;
                case 2:
                    // Candidate and target labels agree, but the actual effect does not.
                    f.Snapshot.Decision->Update["b"] = EPileState::Suspended;
                    break;
                case 3:
                    f.Snapshot.Decision->Candidate = TString();
                    break;
                case 4:
                    ++f.Snapshot.Decision->Epoch.ReadinessGeneration;
                    break;
                case 5:
                    f.Snapshot.Decision->EvidenceExpiry = f.Clock->Value;
                    break;
                case 6:
                    f.Snapshot.Decision->Endpoint.reset();
                    break;
                case 7:
                    ++f.Snapshot.Cluster->Generation;
                    break;
            }
            const auto blocked = f.Preview(action);
            UNIT_ASSERT(!blocked["dispatchable"].GetBoolean());
            const auto route = "/api/operations/" + operationId + "/resume";
            UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", route, f.Admission(original)).Response.Status, 409);
            const auto response = f.Call("POST", route, f.Admission(blocked));
            UNIT_ASSERT(!response.Submission);
            UNIT_ASSERT(response.Response.Status == 409 || response.Response.Status == 422);
        }
    }

    Y_UNIT_TEST(EvidenceBoundResumeAllowsNonprimaryCandidateWhileKeepingExactRoleMap) {
        TFixture f;
        f.Snapshot.Cluster->Piles[""] = EPileState::Synchronized;
        const auto operationId = PauseEvidenceWorkflow(f, false);
        f.Snapshot.Decision->Candidate = TString();
        f.Snapshot.Decision->Endpoint->Pile = TString();
        f.Snapshot.Decision->Endpoint->Endpoint = "grpc://other-survivor:2135";
        auto action = f.Action("resume_workflow");
        action["operation_id"] = operationId;
        const auto preview = f.Preview(action);
        UNIT_ASSERT(preview["dispatchable"].GetBoolean());
        const auto response = f.Call("POST", "/api/operations/" + operationId + "/resume", f.Admission(preview));
        UNIT_ASSERT(response.Submission);
        UNIT_ASSERT(response.Submission->DecisionId == f.Snapshot.Decision->Id);
        const auto& stored = f.Snapshot.Applied->Operations.at(operationId);
        UNIT_ASSERT(!stored.Plan.Successor);
        UNIT_ASSERT_VALUES_EQUAL(stored.Plan.Steps[stored.NextStep].Update.size(), 1);
        UNIT_ASSERT(stored.Plan.Steps[stored.NextStep].Update.at("b") == EPileState::Disconnected);
        UNIT_ASSERT(f.Snapshot.Cluster->Piles.at("a") == EPileState::Primary);
    }

    Y_UNIT_TEST(EvidenceBoundResumeSignsCurrentIdentityEvidenceEndpointAndExpiry) {
        for (unsigned replacement : {0u, 1u, 2u, 3u}) {
            TFixture f;
            const auto operationId = PauseEvidenceWorkflow(f, true);
            auto action = f.Action("resume_workflow");
            action["operation_id"] = operationId;
            const auto original = f.Preview(action);
            switch (replacement) {
                case 0:
                    f.Snapshot.Decision->Id = Uuid(21);
                    break;
                case 1:
                    f.Snapshot.Decision->EvidenceRequestIds = {Uuid(31)};
                    break;
                case 2:
                    f.Snapshot.Decision->Endpoint->Endpoint = "grpc://replacement:2135";
                    break;
                case 3:
                    f.Snapshot.Decision->EvidenceExpiry -= TDuration::Seconds(1);
                    break;
            }
            const auto route = "/api/operations/" + operationId + "/resume";
            UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", route, f.Admission(original)).Response.Status, 409);
            const auto fresh = f.Preview(action);
            UNIT_ASSERT(fresh["dispatchable"].GetBoolean());
            UNIT_ASSERT(f.Call("POST", route, f.Admission(fresh)).Submission);
        }
    }

    Y_UNIT_TEST(OrdinaryResumeDoesNotBindUnrelatedHealthEvidence) {
        TFixture f;
        auto state = f.MutableState();
        TPlanRequest request;
        request.Kind = EOperationKind::PrimaryMaintenance;
        request.Target = "a";
        request.Successor = "b";
        request.ExternalPrerequisitesAcknowledged = true;
        TOperation operation;
        operation.Id = Uuid(70);
        operation.Plan = *PlanWorkflow(*f.Snapshot.Cluster, request).Plan;
        operation.Status = EOperationStatus::Paused;
        operation.NextStep = 1;
        state->Operations[operation.Id] = operation;
        state->OrdinaryOwner = operation.Id;
        f.Snapshot.Cluster->Piles["a"] = EPileState::Synchronized;
        f.Snapshot.Cluster->Piles["b"] = EPileState::Primary;
        auto action = f.Action("resume_workflow");
        action["operation_id"] = operation.Id;
        const auto preview = f.Preview(action);
        UNIT_ASSERT_VALUES_EQUAL(TInstant::ParseIso8601(preview["expires_at"].GetString()), f.Clock->Value + TDuration::Seconds(120));
        f.Snapshot.Decision = TDecision();
        f.Snapshot.Decision->Id = Uuid(99);
        f.Snapshot.Decision->EvidenceExpiry = f.Clock->Value;
        const auto response = f.Call("POST", "/api/operations/" + operation.Id + "/resume", f.Admission(preview));
        UNIT_ASSERT(response.Submission);
        UNIT_ASSERT(!response.Submission->DecisionId);
        UNIT_ASSERT(!response.Submission->PlanRequest);
    }

    Y_UNIT_TEST(ModeAndExternalRecoverySchemasDoNotAcceptCallerOrNumericRevisions) {
        TFixture f;
        auto mode = Object();
        mode["mode"] = "Observe";
        mode["controller_revision"] = "5";
        mode["request_id"] = Uuid(55);
        mode["reason"] = "handover";
        mode["ownership_acknowledgement"] = Null();
        auto submit = f.Call("POST", "/api/mode", mode);
        UNIT_ASSERT(submit.Submission);
        UNIT_ASSERT(submit.Submission->Command.Kind == ECommandKind::SetMode);
        mode["controller_revision"] = 5;
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/mode", mode).Response.Status, 400);
        mode["controller_revision"] = "5";
        mode["caller_id"] = "operator";
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/mode", mode).Response.Status, 400);
        auto recovery = Object();
        recovery["request_id"] = Uuid(56);
        recovery["resolution"] = "resolve_external";
        recovery["state_fingerprint"] = CanonicalFingerprint(*f.Snapshot.Cluster);
        recovery["reason"] = "stopped producer and investigated server work";
        recovery["acknowledge_unfenced_recovery"] = true;
        recovery["executor_stopped"] = true;
        recovery["server_work_reconciled"] = true;
        submit = f.Call("POST", "/api/operations/" + Uuid(70) + "/resolve", recovery);
        UNIT_ASSERT(submit.Submission);
        UNIT_ASSERT(submit.Submission->Command.AcknowledgeUnfencedRecovery);
        recovery["state_fingerprint"] = "changed";
        UNIT_ASSERT_VALUES_EQUAL(f.Call("POST", "/api/operations/" + Uuid(70) + "/resolve", recovery).Response.Status, 409);
    }

    Y_UNIT_TEST(MutationReadinessAndUnknownPrebridgeCannotAuthorizePreview) {
        TFixture f;
        f.Snapshot.MutationReady = false;
        UNIT_ASSERT(!f.Preview(f.Action("takedown", "b"))["dispatchable"].GetBoolean());
        f.Snapshot.MutationReady = true;
        f.Snapshot.Cluster.reset();
        f.Snapshot.Topology.ConfiguredPiles = {"a"};
        UNIT_ASSERT(!f.Preview(f.Action("convert"))["dispatchable"].GetBoolean());
    }

    Y_UNIT_TEST(HttpActorStopBypassesExhaustedCommandCreditsAndControllerQueue) {
        TFixture fixture;
        TTempDir directory;
        chmod(directory.Name().c_str(), 0700);
        auto resources = std::make_shared<TRuntimeResources>(fixture.Config, directory.Name(), "member");
        NActors::TTestActorRuntimeBase runtime(1, true);
        runtime.Initialize();
        resources->CompletionGate->Attach(runtime.GetActorSystem(0));
        fixture.Snapshot.IncarnationId = resources->ProcessId();
        resources->PublishSnapshot(std::make_shared<const TControllerSnapshot>(fixture.Snapshot));
        const auto proxy = runtime.AllocateEdgeActor();
        const auto controller = runtime.AllocateEdgeActor();
        const auto sender = runtime.AllocateEdgeActor();
        const auto actor = runtime.Register(CreateHttpActor(fixture.Config, resources, controller, proxy));
        for (unsigned credit = 0; credit < 256; ++credit) {
            UNIT_ASSERT(resources->AcquireSubmission());
        }
        UNIT_ASSERT(!resources->AcquireSubmission());
        auto body = Object();
        body["request_id"] = Uuid(80);
        body["reason"] = "stop even when command queue is full";
        const auto json = CanonicalApiJson(body);
        const TString wire = "POST /api/local-stop HTTP/1.1\r\nHost: arbitrary-host\r\nContent-Type: application/json\r\nContent-Length: " + ToString(json.size()) + "\r\n\r\n" + json;
        NHttp::THttpIncomingRequestPtr request = new NHttp::THttpIncomingRequest();
        request->EnsureEnoughSpaceAvailable(wire.size());
        memcpy(request->Pos(), wire.data(), wire.size());
        request->Advance(wire.size());
        runtime.Send(new NActors::IEventHandle(actor, sender, new NHttp::TEvHttpProxy::TEvHttpIncomingRequest(request)), 0, true);
        TAutoPtr<NActors::IEventHandle> event;
        const auto* response = runtime.GrabEdgeEvent<NHttp::TEvHttpProxy::TEvHttpOutgoingResponse>(event);
        UNIT_ASSERT_VALUES_EQUAL(response->Response->Status, "200");
        UNIT_ASSERT(resources->StopLatch->IsStopped());
        for (unsigned credit = 0; credit < 256; ++credit) {
            resources->ReleaseSubmission();
        }
        resources->CompletionGate->Close();
        resources->StopStorage();
        runtime.Send(new NActors::IEventHandle(actor, sender, new NActors::TEvents::TEvPoison()), 0, true);
    }
    Y_UNIT_TEST(ReadTimeoutNeverReportsUnknownMutationAdmission) {
        const auto response = ReadTimeoutResponse();
        UNIT_ASSERT_VALUES_EQUAL(response.Status, 503);
        const auto body = ParseStrictJson(response.Body);
        UNIT_ASSERT_VALUES_EQUAL(body["error"]["code"].GetString(), "AUDIT_READ_PENDING");
        UNIT_ASSERT(!response.Body.Contains("ADMISSION_UNKNOWN"));
        UNIT_ASSERT(!response.Body.Contains("request_id"));
    }

    Y_UNIT_TEST(GlobalDisplayUsesActualWholeVectorAndNeverStitchesSamples) {
        TFixture f;
        TPileReport first;
        first.Sample.Pile = "a";
        first.Targets["a"] = {EAssessment::NoFailureReported, true, {}};
        TPileReport second;
        second.Sample.Pile = "b";
        second.Targets["b"] = {EAssessment::Failed, true, {}};
        f.Snapshot.Reports = {first, second};
        auto projected = ParseStrictJson(f.Call("GET", "/api/state").Response.Body);
        UNIT_ASSERT_VALUES_EQUAL(projected["evidence"]["source_kind"].GetString(), "GLOBAL_BSC");
        UNIT_ASSERT_VALUES_EQUAL(projected["evidence"]["global_assessment"]["status"].GetString(), "UNKNOWN");
        TDecision decision;
        decision.Source = ESourceKind::GlobalBsc;
        decision.Kind = EDecisionKind::Failover;
        decision.EvidenceExpiry = f.Clock->Value + TDuration::Seconds(20);
        decision.GlobalAssessment = {{"a", first.Targets.at("a")}, {"b", second.Targets.at("b")}};
        TObservation observation;
        observation.RequestId = Uuid(88);
        observation.HealthStartedAt = f.Clock->Value;
        observation.FinishedAt = f.Clock->Value;
        observation.Source = ESourceKind::GlobalBsc;
        decision.Evidence.push_back(observation);
        f.Snapshot.Decision = decision;
        projected = ParseStrictJson(f.Call("GET", "/api/state").Response.Body);
        UNIT_ASSERT_VALUES_EQUAL(projected["evidence"]["global_assessment"]["status"].GetString(), "CORROBORATED_POLICY_VECTOR");
        UNIT_ASSERT_VALUES_EQUAL(projected["evidence"]["provenance"].GetArray().size(), 1);
        UNIT_ASSERT(!projected["evidence"]["qualified"].GetBoolean());
    }

    Y_UNIT_TEST(NodeLatencyAndReachabilityComeFromActualDiagnostic) {
        TFixture f;
        TNode node;
        node.NodeId = 1;
        node.Pile = "a";
        node.Endpoint = "grpc://a:2135";
        f.Snapshot.Topology.Nodes.push_back(node);
        TDiagnosticSummary diagnostic;
        diagnostic.Endpoint = node.Endpoint;
        diagnostic.NodeId = 1;
        diagnostic.CheckedAt = f.Clock->Value;
        diagnostic.Success = true;
        diagnostic.Latency = TDuration::MicroSeconds(1500);
        diagnostic.Profile = "selfcheck_identity_v1";
        f.Snapshot.Diagnostics.push_back(diagnostic);
        const auto body = ParseStrictJson(f.Call("GET", "/api/state").Response.Body);
        for (const auto& pile : body["piles"].GetArray()) {
            if (pile["name"].GetString() == "a") {
                const auto& projected = pile["nodes"].GetArray().front();
                UNIT_ASSERT_DOUBLES_EQUAL(projected["latency_ms"].GetDouble(), 1.5, 0.001);
                UNIT_ASSERT_VALUES_EQUAL(projected["reachability"].GetString(), "RESPONDED");
                UNIT_ASSERT_VALUES_EQUAL(projected["profile"].GetString(), "selfcheck_identity_v1");
            }
        }
    }

    Y_UNIT_TEST(MetricsUseActualCountersAndFoldUnboundedReasons) {
        TFixture f;
        f.Snapshot.ClosedRounds = 9;
        f.Snapshot.DeadlineRounds = 2;
        f.Snapshot.AdmissionBudgetOverruns = 1;
        f.Snapshot.FreshnessMisses = 3;
        f.Snapshot.LastRoundSampledNodes = 6;
        f.Snapshot.LastRoundFreshNodes = 4;
        f.Snapshot.EvidenceRejects["NO_REPORTERS"] = 4;
        f.Snapshot.EvidenceRejects["free-form/request-123"] = 5;
        f.Snapshot.Cluster->Pending.push_back({"ADD", TString("b"), "missing disk", {}});
        auto counters = MakeIntrusive<NMonitoring::TDynamicCounters>();
        UpdateHttpMetrics(f.Snapshot, f.Config, f.Stop, 7, counters);
        UNIT_ASSERT_VALUES_EQUAL(counters->GetCounter("skipper_round_closed_total", true)->Val(), 9);
        UNIT_ASSERT_VALUES_EQUAL(counters->GetCounter("skipper_pending_lifecycle_errors")->Val(), 1);
        UNIT_ASSERT_VALUES_EQUAL(counters->GetCounter("skipper_storage_pending")->Val(), 7);
        UNIT_ASSERT_VALUES_EQUAL(counters->GetSubgroup("reason", "OTHER")->GetCounter("skipper_evidence_rejected_total", true)->Val(), 5);
        f.Clock->Value += TDuration::Seconds(6);
        UpdateHttpMetrics(f.Snapshot, f.Config, f.Stop, 0, counters);
        UNIT_ASSERT_VALUES_EQUAL(counters->GetCounter("skipper_controller_stalled")->Val(), 1);
        UNIT_ASSERT_VALUES_EQUAL(counters->GetCounter("skipper_authority_ready")->Val(), 0);
    }
}

} // namespace NBridgeSkipper
