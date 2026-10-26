#include <ydb/apps/bridge_skipper/durable/durable.h>
#include <ydb/apps/bridge_skipper/durable/store.h>

#include <contrib/libs/sqlite3/sqlite3.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/folder/tempdir.h>
#include <util/string/cast.h>

#include <filesystem>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace NBridgeSkipper {
namespace {

TClusterView View() {
    TClusterView view;
    view.Converted = true;
    view.Generation = 10;
    view.Piles = {{"a", EPileState::Primary}, {"b", EPileState::Synchronized}, {"c", EPileState::Disconnected}, {"", EPileState::Inactive}};
    view.Endpoint = "grpcs://a.example:2135";
    return view;
}

TStoreBinding Binding(TString process = "process-1") {
    return {"cluster", "member", "local", "membership-1", process};
}

TOwnershipAcknowledgement Ownership(bool independent = false, TString reference = "plan") {
    TOwnershipAcknowledgement acknowledgement;
    acknowledgement.ExclusiveAdministration = true;
    acknowledgement.IndependentFailureDomain = independent;
    acknowledgement.DeploymentPlanReference = std::move(reference);
    return acknowledgement;
}

TPlanRequest Request(
    EOperationKind kind,
    std::optional<TString> target = {},
    std::optional<TString> successor = {},
    std::set<TString> failed = {},
    bool automatic = false)
{
    TPlanRequest request;
    request.Kind = kind;
    request.Target = std::move(target);
    request.Successor = std::move(successor);
    request.FailedTargets = std::move(failed);
    request.Automatic = automatic;
    request.ExternalPrerequisitesAcknowledged = true;
    return request;
}

struct TFixture {
    TDurableState State;
    TClusterView Current = View();
    ui64 Counter = 0;

    TFixture() {
        State.ClusterId = "cluster";
        State.MemberId = "member";
        State.MembershipIncarnation = "membership-1";
        auto mode = Command(ECommandKind::SetMode);
        mode.Reason = "take ownership";
        mode.Ownership = Ownership(false, "deployment-plan");
        Commit(mode);
    }

    TDurableCommand Command(ECommandKind kind) {
        TDurableCommand command;
        command.Id = "command-" + ToString(++Counter);
        command.Kind = kind;
        command.ExpectedControllerRevision = State.ControllerRevision;
        command.Ready = true;
        command.Fresh = true;
        command.Coherent = true;
        command.AuditTime = "2026-10-21T00:00:00Z";
        return command;
    }

    TCommandResult Commit(const TDurableCommand& command) {
        auto result = Apply(State, command);
        UNIT_ASSERT_C(result.Result.Ok, result.Result.Code);
        State = std::move(result.State);
        return result.Result;
    }

    TString Operation(EOperationKind kind, std::optional<TString> target = {}, std::optional<TString> successor = {}, std::set<TString> failed = {}) {
        TPlanRequest request;
        request.Kind = kind;
        request.Target = target;
        request.Successor = successor;
        request.FailedTargets = std::move(failed);
        request.ExternalPrerequisitesAcknowledged = true;
        request.DeletionAcknowledged = true;
        request.TypedPileName = target;
        request.EmptyNameAcknowledged = true;
        const auto planned = PlanWorkflow(Current, request);
        UNIT_ASSERT_C(planned.Plan, planned.Error);
        auto command = Command(ECommandKind::AdmitOperation);
        command.OperationId = "operation-" + ToString(Counter);
        command.Plan = *planned.Plan;
        Commit(command);
        return command.OperationId;
    }

    TString Intent(const TString& operationId) {
        auto command = Command(ECommandKind::AdmitAttempt);
        command.OperationId = operationId;
        const auto& operation = State.Operations.at(operationId);
        const auto& step = operation.Plan.Steps.at(operation.NextStep);
        command.ExpectedWorkflowRevision = operation.Revision;
        command.Attempt.Id = "attempt-" + ToString(Counter);
        command.Attempt.OperationId = operationId;
        command.Attempt.Step = operation.NextStep;
        command.Attempt.Method = step.Method;
        command.Attempt.Update = step.Update;
        command.Attempt.Target = step.Target;
        command.Attempt.Endpoint = Current.Endpoint;
        command.Attempt.NodeId = 1;
        command.Attempt.DesignatedMember = "member";
        command.Attempt.DesignatedProcess = "process-1";
        command.Attempt.Baseline = Current;
        command.Attempt.TopologyRevision = 1;
        command.Attempt.ArgumentsHash = MutationArgumentsHash(command.Attempt);
        Commit(command);
        return command.Attempt.Id;
    }

    TDurableCommand Rpc(const TString& id, bool success = true) {
        auto command = Command(ECommandKind::RpcOutcome);
        command.Outcome.AttemptId = id;
        command.Outcome.DesignatedMember = "member";
        command.Outcome.DesignatedProcess = "process-1";
        command.Outcome.Success = success;
        command.Outcome.Status = success ? "SUCCESS" : "TIMEOUT";
        return command;
    }

    TDurableCommand Acceptance(const TString& id, TClusterView observed) {
        auto command = Command(ECommandKind::AcceptByObservation);
        command.Attempt.Id = id;
        auto& proof = command.Acceptance;
        proof.Observed = std::move(observed);
        proof.TopologyRevision = 1;
        proof.Fresh = true;
        proof.IdentityVerified = true;
        proof.OrderedAfterSend = true;
        proof.Coherent = true;
        proof.NoReplayPathQualified = true;
        proof.MarkerQualified = true;
        proof.CompleteExclusiveHistory = true;
        proof.NoAlternativeProducer = true;
        proof.Provenance = "pinned fixture/test-only";
        return command;
    }

    void Reconcile(TClusterView observed, std::set<TString> healthy = {}) {
        auto command = Command(ECommandKind::Reconcile);
        command.Observed = std::move(observed);
        command.FreshHealthyPiles = std::move(healthy);
        Commit(command);
    }
};

void MutateDatabase(const TString& directory, const char* sql) {
    sqlite3* db = nullptr;
    UNIT_ASSERT_VALUES_EQUAL(sqlite3_open((directory + "/skipper.sqlite3").c_str(), &db), SQLITE_OK);
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    UNIT_ASSERT_VALUES_EQUAL(result, SQLITE_OK);
}

} // namespace

Y_UNIT_TEST_SUITE(SkipperDurable) {
    Y_UNIT_TEST(PreviewsDescribePrerequisitesWithoutAuthorizingDispatch) {
        TPlanRequest request;
        request.Kind = EOperationKind::Rejoin;
        request.Target = "c";
        auto plan = PlanWorkflow(View(), request);
        UNIT_ASSERT(plan.Plan);
        UNIT_ASSERT(!plan.Plan->PrerequisitesAcknowledged);
        UNIT_ASSERT_VALUES_EQUAL(plan.RequiredAcknowledgements.size(), 1);
        TFixture f;
        auto command = f.Command(ECommandKind::AdmitOperation);
        command.OperationId = "preview-only";
        command.Plan = *plan.Plan;
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, command).Result.Code, "WORKFLOW_PREREQUISITES_REQUIRED");
    }

    Y_UNIT_TEST(ManualMutableNotSynchronizedTargetDoesNotNeedAutoCertification) {
        auto view = View();
        view.Piles["b"] = EPileState::NotSynchronized;
        TPlanRequest request;
        request.Kind = EOperationKind::Failover;
        request.FailedTargets = {"b"};
        request.ExternalPrerequisitesAcknowledged = true;
        UNIT_ASSERT(PlanWorkflow(view, request).Plan);
        request.Automatic = true;
        UNIT_ASSERT(!PlanWorkflow(view, request).Plan);
    }

    Y_UNIT_TEST(FailoverBatchAndPromotionAreExact) {
        auto view = View();
        view.Piles["b"] = EPileState::Promoted;
        view.Piles["c"] = EPileState::Synchronized;
        TPlanRequest request;
        request.Kind = EOperationKind::Failover;
        request.FailedTargets = {"a"};
        request.Successor = "c";
        request.ExternalPrerequisitesAcknowledged = true;
        const auto plan = PlanWorkflow(view, request);
        UNIT_ASSERT(plan.Plan);
        const auto& update = plan.Plan->Steps.front().Update;
        UNIT_ASSERT(update.at("a") == EPileState::Disconnected);
        UNIT_ASSERT(update.at("b") == EPileState::Synchronized);
        UNIT_ASSERT(update.at("c") == EPileState::Primary);
        UNIT_ASSERT(!update.contains(""));
        request.FailedTargets = {"c"};
        request.Successor.reset();
        const auto retained = PlanWorkflow(view, request);
        UNIT_ASSERT(retained.Plan);
        UNIT_ASSERT(!retained.Plan->Steps.front().Update.contains("b"));
    }

    Y_UNIT_TEST(EveryManualWorkflowHasNarrowSteps) {
        TFixture f;
        UNIT_ASSERT_VALUES_EQUAL(f.Operation(EOperationKind::Switchover, {}, "b").empty(), false);
        const auto switched = PlanWorkflow(f.Current, Request(EOperationKind::PrimaryMaintenance, "a", "b"));
        UNIT_ASSERT(switched.Plan);
        UNIT_ASSERT_VALUES_EQUAL(switched.Plan->Steps.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(switched.Plan->Steps[0].Update.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(switched.Plan->Steps[1].Update.size(), 1);
        auto wrong = Request(EOperationKind::PrimaryMaintenance, "b", "b");
        UNIT_ASSERT(!PlanWorkflow(f.Current, wrong).Plan);
        UNIT_ASSERT(PlanWorkflow(f.Current, Request(EOperationKind::Takedown, "b")).Plan);
        UNIT_ASSERT(PlanWorkflow(f.Current, Request(EOperationKind::AddPile, TString())).Plan);
        UNIT_ASSERT(!PlanWorkflow(f.Current, Request(EOperationKind::RemovePile, "b")).Plan);
        auto prebridge = View();
        prebridge.Converted = false;
        prebridge.Piles = {{"a", EPileState::Primary}};
        UNIT_ASSERT(PlanWorkflow(prebridge, Request(EOperationKind::Convert)).Plan);
    }

    Y_UNIT_TEST(EmptyNameDeletionRequiresExplicitTypingAndCheckbox) {
        auto view = View();
        view.Piles[""] = EPileState::Disconnected;
        TPlanRequest request;
        request.Kind = EOperationKind::RemovePile;
        request.Target = TString();
        request.ExternalPrerequisitesAcknowledged = true;
        request.DeletionAcknowledged = true;
        auto plan = PlanWorkflow(view, request);
        UNIT_ASSERT(plan.Plan);
        UNIT_ASSERT(!plan.Plan->PrerequisitesAcknowledged);
        request.TypedPileName = TString();
        request.EmptyNameAcknowledged = true;
        plan = PlanWorkflow(view, request);
        UNIT_ASSERT(plan.Plan->PrerequisitesAcknowledged);
        UNIT_ASSERT(plan.Plan->Steps.front().Target.has_value());
        UNIT_ASSERT(plan.Plan->Steps.front().Target->empty());
    }

    Y_UNIT_TEST(FirstStartManualWithoutOwnershipAndModeAcknowledgementRules) {
        TDurableState initial;
        UNIT_ASSERT(initial.RequestedMode == ERequestedMode::Manual);
        UNIT_ASSERT(!initial.Ownership);
        TFixture f;
        UNIT_ASSERT(f.State.Ownership);
        UNIT_ASSERT(!f.State.Ownership->IndependentFailureDomain);
        auto observe = f.Command(ECommandKind::SetMode);
        observe.Mode = ERequestedMode::Observe;
        observe.Reason = "handover";
        f.Commit(observe);
        UNIT_ASSERT(!f.State.Ownership);
        UNIT_ASSERT(HandoverComplete(f.State));
        auto manual = f.Command(ECommandKind::SetMode);
        manual.Reason = "resume ownership";
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, manual).Result.Code, "OWNERSHIP_ACKNOWLEDGEMENT_REQUIRED");
        manual.Ownership = Ownership();
        f.Commit(manual);
        UNIT_ASSERT(f.State.RequestedMode == ERequestedMode::Manual);
    }

    Y_UNIT_TEST(RequestAndCommandDedupPrecedeStaleRevision) {
        TFixture f;
        auto command = f.Command(ECommandKind::AdvanceGeneration);
        command.RequestId = "request-id";
        command.RequestHash = "original-body";
        command.Observed = f.Current;
        const auto receipt = f.Commit(command);
        auto duplicate = command;
        duplicate.Id = "new-internal-command";
        duplicate.ExpectedControllerRevision = 0;
        const auto replay = Apply(f.State, duplicate);
        UNIT_ASSERT(replay.Duplicate);
        UNIT_ASSERT(replay.Result == receipt);
        duplicate.RequestHash = "different-body";
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, duplicate).Result.Code, "REQUEST_ID_REUSED");
        command.Reason = "changed command payload";
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, command).Result.Code, "COMMAND_ID_REUSED");
    }

    Y_UNIT_TEST(IntentSurvivesSerializationAndBlocksAllSecondInvocations) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::Rejoin, "c");
        const auto attempt = f.Intent(operation);
        const auto restored = DeserializeState(SerializeState(f.State));
        UNIT_ASSERT_VALUES_EQUAL(restored.UnresolvedAttempt.value(), attempt);
        UNIT_ASSERT_VALUES_EQUAL(restored.Attempts.at(attempt).DesignatedProcess, "process-1");
        UNIT_ASSERT_VALUES_EQUAL(SerializeState(restored), SerializeState(f.State));
        auto emergency = f.Command(ECommandKind::AdmitOperation);
        emergency.OperationId = "emergency";
        auto request = Request(EOperationKind::Failover, {}, {}, {"b"});
        emergency.Plan = *PlanWorkflow(f.Current, request).Plan;
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, emergency).Result.Code, "UNRESOLVED_ATTEMPT");
    }

    Y_UNIT_TEST(DefinitiveClosureAllowsPreemptionAndRejectsOldQueuedStep) {
        TFixture f;
        const auto maintenance = f.Operation(EOperationKind::PrimaryMaintenance, "a", "b");
        const auto attempt = f.Intent(maintenance);
        f.Commit(f.Rpc(attempt));
        auto promoted = f.Current;
        promoted.Generation += 2;
        promoted.Piles["a"] = EPileState::Synchronized;
        promoted.Piles["b"] = EPileState::Primary;
        f.Reconcile(promoted);
        f.Current = promoted;
        const auto oldRevision = f.State.Operations.at(maintenance).Revision;
        UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(maintenance).NextStep, 1);
        const auto emergency = f.Operation(EOperationKind::Failover, {}, "a", {"b"});
        UNIT_ASSERT(f.State.Operations.at(maintenance).Status == EOperationStatus::Preempted);
        UNIT_ASSERT_VALUES_EQUAL(*f.State.Operations.at(maintenance).PreemptedBy, emergency);
        auto stale = f.Command(ECommandKind::AdmitAttempt);
        stale.OperationId = maintenance;
        stale.ExpectedWorkflowRevision = oldRevision;
        stale.Attempt.Step = 1;
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, stale).Result.Code, "STALE_WORKFLOW_STEP");
    }

    Y_UNIT_TEST(TerminalTimeoutNeverGetsImaginarySecondFutureResult) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::Rejoin, "c");
        const auto attempt = f.Intent(operation);
        f.Commit(f.Rpc(attempt, false));
        UNIT_ASSERT(f.State.UnresolvedAttempt);
        UNIT_ASSERT(f.State.Attempts.at(attempt).RpcDisposition == ERpcDisposition::Unknown);
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, f.Rpc(attempt)).Result.Code, "RPC_ALREADY_TERMINAL");
        auto cancel = f.Command(ECommandKind::Cancel);
        cancel.OperationId = operation;
        UNIT_ASSERT(Apply(f.State, cancel).Result.Code.StartsWith("ALREADY_DISPATCHED"));
    }

    Y_UNIT_TEST(CancelStopsOnlyStepsWithoutIntentAfterEarlierStepsSettle) {
        TFixture f;
        const auto maintenance = f.Operation(EOperationKind::PrimaryMaintenance, "a", "b");
        const auto attempt = f.Intent(maintenance);
        auto cancel = f.Command(ECommandKind::Cancel);
        cancel.OperationId = maintenance;
        UNIT_ASSERT(Apply(f.State, cancel).Result.Code.StartsWith("ALREADY_DISPATCHED"));
        f.Commit(f.Rpc(attempt));
        cancel.ExpectedControllerRevision = f.State.ControllerRevision;
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, cancel).Result.Code, "BACKGROUND_WORK_PENDING");
        auto promoted = f.Current;
        promoted.Generation += 2;
        promoted.Piles["a"] = EPileState::Synchronized;
        promoted.Piles["b"] = EPileState::Primary;
        f.Reconcile(promoted);
        UNIT_ASSERT(f.State.Operations.at(maintenance).Status == EOperationStatus::Queued);
        UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(maintenance).NextStep, 1);
        cancel.ExpectedControllerRevision = f.State.ControllerRevision;
        f.Commit(cancel);
        UNIT_ASSERT(f.State.Operations.at(maintenance).Status == EOperationStatus::Cancelled);
        UNIT_ASSERT(f.State.Attempts.at(attempt).Accepted);
    }

    Y_UNIT_TEST(ReconcileDoesNotRequeueStepWithOpenOrAmbiguousIntent) {
        for (bool ambiguous : {false, true}) {
            TFixture f;
            const auto maintenance = f.Operation(EOperationKind::PrimaryMaintenance, "a", "b");
            f.Commit(f.Rpc(f.Intent(maintenance)));
            auto promoted = f.Current;
            promoted.Generation += 2;
            promoted.Piles["a"] = EPileState::Synchronized;
            promoted.Piles["b"] = EPileState::Primary;
            f.Reconcile(promoted);
            UNIT_ASSERT(f.State.Operations.at(maintenance).Status == EOperationStatus::Queued);
            f.Current = promoted;
            const auto second = f.Intent(maintenance);
            UNIT_ASSERT(f.State.Operations.at(maintenance).Status == EOperationStatus::Executing);
            if (ambiguous) {
                f.Commit(f.Rpc(second, false));
                UNIT_ASSERT(f.State.Attempts.at(second).Disposition == EAttemptDisposition::Ambiguous);
            }
            const auto status = f.State.Operations.at(maintenance).Status;
            f.Reconcile(promoted);
            UNIT_ASSERT(f.State.Operations.at(maintenance).Status == status);
            UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(maintenance).NextStep, 1);
        }
    }

    Y_UNIT_TEST(NegativeClosureMustBeFromTheDesignatedGateway) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::Rejoin, "c");
        const auto attempt = f.Intent(operation);
        auto negative = f.Command(ECommandKind::NeverDispatched);
        negative.Attempt.Id = attempt;
        negative.Attempt.DesignatedMember = "member";
        negative.Attempt.DesignatedProcess = "other-process";
        negative.GatewayProof = "serialized against queued sends";
        UNIT_ASSERT(!Apply(f.State, negative).Result.Ok);
        negative.Attempt.DesignatedProcess = "process-1";
        f.Commit(negative);
        UNIT_ASSERT(!f.State.UnresolvedAttempt);
        f.Commit(f.Rpc(attempt));
        UNIT_ASSERT(f.State.StateConflict);
    }

    Y_UNIT_TEST(RejoinAcceptanceNeedsEveryQualificationAndDistinguishingTransition) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::Rejoin, "c");
        const auto attempt = f.Intent(operation);
        auto after = f.Current;
        ++after.Generation;
        after.Piles["c"] = EPileState::NotSynchronized;
        auto command = f.Acceptance(attempt, after);
        command.Acceptance.NoReplayPathQualified = false;
        UNIT_ASSERT(!Apply(f.State, command).Result.Ok);
        command.Acceptance.NoReplayPathQualified = true;
        command.Acceptance.TopologyRevision = 2;
        UNIT_ASSERT(!Apply(f.State, command).Result.Ok);
        command.Acceptance.TopologyRevision = 1;
        command.Acceptance.Observed = f.Current;
        command.Acceptance.Observed.Generation++;
        UNIT_ASSERT(!Apply(f.State, command).Result.Ok);
        command.Acceptance.Observed = after;
        f.Commit(command);
        UNIT_ASSERT(!f.State.UnresolvedAttempt);
        UNIT_ASSERT(f.State.Attempts.at(attempt).Accepted);
        UNIT_ASSERT(f.State.Attempts.at(attempt).RpcDisposition == ERpcDisposition::Pending);
    }

    Y_UNIT_TEST(AcceptedObservationRetainsFutureAndContradictionCreatesConflict) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::Rejoin, "c");
        const auto attempt = f.Intent(operation);
        auto after = f.Current;
        ++after.Generation;
        after.Piles["c"] = EPileState::NotSynchronized;
        f.Commit(f.Acceptance(attempt, after));
        auto contradiction = f.Rpc(attempt, false);
        contradiction.Outcome.GenerationMismatchQualified = true;
        f.Commit(contradiction);
        UNIT_ASSERT(f.State.StateConflict);
        UNIT_ASSERT(f.State.Attempts.at(attempt).Accepted);
    }

    Y_UNIT_TEST(AllUpdateMarkersRequireExactRolesAndQualifiedFollowups) {
        for (const auto kind : {EOperationKind::Failover, EOperationKind::Switchover, EOperationKind::Takedown}) {
            TFixture f;
            const auto operation = kind == EOperationKind::Failover ? f.Operation(kind, {}, {}, {"b"}) : (kind == EOperationKind::Switchover ? f.Operation(kind, {}, "b") : f.Operation(kind, "b"));
            const auto attempt = f.Intent(operation);
            auto after = f.Current;
            ++after.Generation;
            after.Piles["b"] = kind == EOperationKind::Switchover ? EPileState::Promoted : (kind == EOperationKind::Takedown ? EPileState::Suspended : EPileState::Disconnected);
            auto acceptance = f.Acceptance(attempt, after);
            acceptance.Acceptance.Observed.Generation = f.Current.Generation;
            UNIT_ASSERT(!Apply(f.State, acceptance).Result.Ok);
            acceptance.Acceptance.Observed = after;
            UNIT_ASSERT(Apply(f.State, acceptance).Result.Ok);
            if (kind != EOperationKind::Failover) {
                ++after.Generation;
                after.Piles["b"] = kind == EOperationKind::Switchover ? EPileState::Primary : EPileState::Disconnected;
                if (kind == EOperationKind::Switchover) {
                    after.Piles["a"] = EPileState::Synchronized;
                }
                acceptance.Acceptance.Observed = after;
                UNIT_ASSERT(!Apply(f.State, acceptance).Result.Ok);
                acceptance.Acceptance.CertifiedAutomaticFollowup = true;
                f.Commit(acceptance);
            } else {
                f.Commit(acceptance);
            }
        }
    }

    Y_UNIT_TEST(NoopMarkerAndUnknownHistoryCannotReleaseAttempt) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::Rejoin, "c");
        const auto id = f.Intent(operation);
        auto after = f.Current;
        ++after.Generation;
        after.Piles["c"] = EPileState::NotSynchronized;
        auto proof = f.Acceptance(id, after).Acceptance;
        auto attempt = f.State.Attempts.at(id);
        attempt.Baseline.Piles["c"] = EPileState::NotSynchronized;
        UNIT_ASSERT(!ValidateAcceptance(f.State, attempt, proof).empty());
        f.State.ExclusiveHistory = false;
        UNIT_ASSERT(!ValidateAcceptance(f.State, f.State.Attempts.at(id), proof).empty());
        UNIT_ASSERT(f.State.UnresolvedAttempt);
    }

    Y_UNIT_TEST(ConversionAcceptanceRetainsGlobalBackgroundGuard) {
        TFixture f;
        f.Current.Piles = {{"a", EPileState::Primary}};
        f.Current.Converted = false;
        const auto operation = f.Operation(EOperationKind::Convert);
        const auto id = f.Intent(operation);
        auto after = f.Current;
        after.Converted = true;
        after.ConversionInProgress = true;
        f.Commit(f.Acceptance(id, after));
        UNIT_ASSERT(!f.State.UnresolvedAttempt);
        auto emergency = f.Command(ECommandKind::AdmitOperation);
        emergency.OperationId = "emergency";
        emergency.Plan.Kind = EOperationKind::Failover;
        emergency.Plan.PrerequisitesAcknowledged = true;
        emergency.Plan.Steps.push_back({});
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, emergency).Result.Code, "CONVERSION_PENDING");
    }

    Y_UNIT_TEST(PendingTargetCannotBeChangedOrLifecycleRepeatedAfterAcceptance) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::AddPile, TString());
        const auto id = f.Intent(operation);
        f.Commit(f.Rpc(id));
        auto command = f.Command(ECommandKind::AdmitOperation);
        command.OperationId = "other-lifecycle";
        auto remove = Request(EOperationKind::RemovePile, "c");
        remove.DeletionAcknowledged = true;
        remove.TypedPileName = "c";
        command.Plan = *PlanWorkflow(f.Current, remove).Plan;
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, command).Result.Code, "PENDING_TARGET_GUARD");
        command.OperationId = "targeted-emergency";
        command.Plan.Kind = EOperationKind::Failover;
        command.Plan.Steps = {{EBridgeMethod::UpdateClusterState, {{TString(), EPileState::Disconnected}}, {}, f.Current, CanonicalFingerprint(f.Current)}};
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, command).Result.Code, "PENDING_TARGET_GUARD");
    }

    Y_UNIT_TEST(AutomaticRejectedPlanStaysSuppressedUntilItsKeyChanges) {
        TFixture f;
        auto mode = f.Command(ECommandKind::SetMode);
        mode.Mode = ERequestedMode::Auto;
        mode.Reason = "test-qualified decision";
        mode.Ownership = Ownership(true, "test-placement");
        f.Commit(mode);
        auto admit = f.Command(ECommandKind::AdmitOperation);
        admit.OperationId = "automatic-operation";
        auto request = Request(EOperationKind::Failover, {}, {}, {"b"}, true);
        admit.Plan = *PlanWorkflow(f.Current, request).Plan;
        admit.Plan.SuppressionKey = "same-normalized-policy-and-fingerprint";
        f.Commit(admit);
        const auto id = f.Intent(admit.OperationId);
        auto reject = f.Rpc(id, false);
        reject.Outcome.GenerationMismatchQualified = true;
        f.Commit(reject);
        auto again = f.Command(ECommandKind::AdmitOperation);
        again.OperationId = "automatic-second";
        again.Plan = admit.Plan;
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, again).Result.Code, "AUTOMATIC_PLAN_SUPPRESSED_OR_MODE");
        again.Plan.SuppressionKey = "corroborated-changed-fingerprint";
        f.Commit(again);
    }

    Y_UNIT_TEST(ExactPinnedRejectionVersusArbitraryInternalError) {
        for (bool exact : {false, true}) {
            TFixture f;
            const auto operation = f.Operation(EOperationKind::Rejoin, "c");
            const auto attempt = f.Intent(operation);
            auto outcome = f.Rpc(attempt, false);
            outcome.Outcome.Status = "INTERNAL_ERROR";
            outcome.Outcome.IssueCode = "DEFAULT_ERROR";
            outcome.Outcome.Issue = exact ? "Unsupported pile state" : "prefix Unsupported pile state";
            f.Commit(outcome);
            UNIT_ASSERT_VALUES_EQUAL(f.State.UnresolvedAttempt.has_value(), !exact);
        }
    }

    Y_UNIT_TEST(NewPendingMembershipAcceptsAtUnchangedGeneration) {
        for (const auto kind : {EOperationKind::AddPile, EOperationKind::RemovePile}) {
            TFixture f;
            const TString target = kind == EOperationKind::AddPile ? TString() : TString("c");
            const auto operation = f.Operation(kind, target);
            const auto attempt = f.Intent(operation);
            auto after = f.Current;
            after.Pending.push_back({kind == EOperationKind::AddPile ? "ADD" : "REMOVE", target, {}, "building groups"});
            f.Commit(f.Acceptance(attempt, after));
            UNIT_ASSERT(!f.State.UnresolvedAttempt);
            UNIT_ASSERT(f.State.Attempts.at(attempt).RpcDisposition == ERpcDisposition::Pending);
            f.Commit(f.Rpc(attempt));
            UNIT_ASSERT(f.State.Attempts.at(attempt).Accepted);
            UNIT_ASSERT(!f.State.StateConflict);
        }
    }

    Y_UNIT_TEST(PinnedLifecycleBackgroundErrorNeedsExactPrefixAndMatchingTarget) {
        for (bool matching : {false, true}) {
            TFixture f;
            const auto operation = f.Operation(EOperationKind::AddPile, TString());
            const auto attempt = f.Intent(operation);
            auto outcome = f.Rpc(attempt, false);
            outcome.Outcome.Status = "GENERIC_ERROR";
            outcome.Outcome.IssueCode = "DEFAULT_ERROR";
            outcome.Outcome.Issue = "Pile is added to the cluster, but its parts of storage groups can't be created (this is retried until it succeeds): missing disk";
            outcome.Outcome.PinnedLifecycleErrorQualified = true;
            outcome.Outcome.Observed = f.Current;
            outcome.Outcome.Observed->Pending.push_back({"ADD", matching ? TString() : TString("c"), "missing disk", {}});
            f.Commit(outcome);
            UNIT_ASSERT_VALUES_EQUAL(f.State.UnresolvedAttempt.has_value(), !matching);
        }
    }

    Y_UNIT_TEST(PendingTargetGuardAllowsDisjointEmergencyButBlocksLifecycle) {
        for (const auto kind : {EOperationKind::AddPile, EOperationKind::RemovePile}) {
            TFixture f;
            const TString target = kind == EOperationKind::AddPile ? TString() : TString("c");
            const auto lifecycle = f.Operation(kind, target);
            const auto attempt = f.Intent(lifecycle);
            auto pending = f.Current;
            pending.Piles[target] = EPileState::Disconnected;
            pending.Pending.push_back({kind == EOperationKind::AddPile ? "ADD" : "REMOVE", target, {}, "groups still pending"});
            f.Commit(f.Acceptance(attempt, pending));
            f.Current = pending;
            const auto emergency = f.Operation(EOperationKind::Failover, {}, {}, {"b"});
            const auto emergencyAttempt = f.Intent(emergency);
            f.Commit(f.Rpc(emergencyAttempt));
            auto after = pending;
            ++after.Generation;
            after.Piles["b"] = EPileState::Disconnected;
            f.Reconcile(after);
            const auto& operation = f.State.Operations.at(lifecycle);
            UNIT_ASSERT(operation.Status == EOperationStatus::Preempted);
            UNIT_ASSERT_VALUES_EQUAL(operation.Background.size(), 1);
            UNIT_ASSERT(!operation.Background[0].Complete);
            UNIT_ASSERT(!operation.Background[0].Superseded);
            UNIT_ASSERT(f.State.Operations.at(emergency).Status == EOperationStatus::Succeeded);
            UNIT_ASSERT(!HandoverComplete(f.State));

            auto repeat = f.Command(ECommandKind::AdmitOperation);
            repeat.OperationId = "other-lifecycle";
            repeat.Plan = *PlanWorkflow(View(), Request(EOperationKind::AddPile, TString())).Plan;
            UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, repeat).Result.Code, "PENDING_TARGET_GUARD");
            f.Commit(f.Rpc(attempt));
            UNIT_ASSERT(!f.State.Operations.at(lifecycle).Background[0].Complete);
            after.Pending.clear();
            after.Piles[target] = kind == EOperationKind::AddPile ? EPileState::Disconnected : EPileState::Inactive;
            f.Reconcile(after);
            UNIT_ASSERT(f.State.Operations.at(lifecycle).Background[0].Complete);
            UNIT_ASSERT(!f.State.Operations.at(lifecycle).Background[0].Superseded);
            UNIT_ASSERT(HandoverComplete(f.State));
        }
    }

    Y_UNIT_TEST(RejoinSupersessionRequiresItsTargetInTheAcceptedEmergencyUpdate) {
        for (bool excludesRejoin : {false, true}) {
            TFixture f;
            const auto rejoin = f.Operation(EOperationKind::Rejoin, "c");
            f.Commit(f.Rpc(f.Intent(rejoin)));
            f.Current.Generation++;
            f.Current.Piles["c"] = EPileState::NotSynchronized;
            const auto emergency = f.Operation(EOperationKind::Failover, {}, {}, {excludesRejoin ? "c" : "b"});
            f.Commit(f.Rpc(f.Intent(emergency)));
            f.Reconcile(f.Current);
            UNIT_ASSERT(!f.State.Operations.at(rejoin).Background[0].Complete);
            auto after = f.Current;
            after.Generation++;
            after.Piles["c"] = EPileState::Disconnected;
            if (!excludesRejoin) {
                after.Piles["b"] = EPileState::Disconnected;
            }
            f.Reconcile(after);
            UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(rejoin).Background[0].Complete, excludesRejoin);
            UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(rejoin).Background[0].Superseded, excludesRejoin);
            UNIT_ASSERT_VALUES_EQUAL(HandoverComplete(f.State), excludesRejoin);
        }
    }

    Y_UNIT_TEST(PromotionSupersessionRequiresAnExplicitAcceptedRoleReplacement) {
        for (bool replacesPrimary : {false, true}) {
            TFixture f;
            f.Current.Piles["c"] = EPileState::Synchronized;
            const auto switchover = f.Operation(EOperationKind::Switchover, {}, "b");
            f.Commit(f.Rpc(f.Intent(switchover)));
            ++f.Current.Generation;
            f.Current.Piles["b"] = EPileState::Promoted;
            const auto emergency = replacesPrimary
                ? f.Operation(EOperationKind::Failover, {}, "c", {"a"})
                : f.Operation(EOperationKind::Failover, {}, {}, {"c"});
            f.Commit(f.Rpc(f.Intent(emergency)));
            f.Reconcile(f.Current);
            UNIT_ASSERT(!f.State.Operations.at(switchover).Background[0].Complete);
            auto after = f.Current;
            ++after.Generation;
            after.Piles["b"] = EPileState::Synchronized;
            if (replacesPrimary) {
                after.Piles["a"] = EPileState::Disconnected;
                after.Piles["c"] = EPileState::Primary;
            } else {
                after.Piles["c"] = EPileState::Disconnected;
            }
            f.Reconcile(after);
            UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(switchover).Background[0].Complete, replacesPrimary);
            UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(switchover).Background[0].Superseded, replacesPrimary);
        }
    }

    Y_UNIT_TEST(ObserveCompletesAcceptedBackgroundHandoverWithoutNewDispatch) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::Rejoin, "c");
        const auto attempt = f.Intent(operation);
        f.Commit(f.Rpc(attempt));
        auto observe = f.Command(ECommandKind::SetMode);
        observe.Mode = ERequestedMode::Observe;
        observe.Reason = "handover after sync";
        f.Commit(observe);
        UNIT_ASSERT(!f.State.Ownership);
        UNIT_ASSERT(f.State.ExclusiveHistory);
        UNIT_ASSERT(!HandoverComplete(f.State));
        auto after = f.Current;
        after.Generation += 2;
        after.Piles["c"] = EPileState::Synchronized;
        f.Reconcile(after, {"b"});
        UNIT_ASSERT(!HandoverComplete(f.State));
        f.Reconcile(after, {"c"});
        UNIT_ASSERT(HandoverComplete(f.State));
        UNIT_ASSERT(!f.State.ExclusiveHistory);
        UNIT_ASSERT(f.State.Operations.at(operation).Status == EOperationStatus::Succeeded);
    }

    Y_UNIT_TEST(ObserveBetweenStepsRequiresExplicitSameOperationResume) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::PrimaryMaintenance, "a", "b");
        const auto attempt = f.Intent(operation);
        f.Commit(f.Rpc(attempt));
        auto observe = f.Command(ECommandKind::SetMode);
        observe.Mode = ERequestedMode::Observe;
        observe.Reason = "pause between steps";
        f.Commit(observe);
        auto after = f.Current;
        after.Generation += 2;
        after.Piles["a"] = EPileState::Synchronized;
        after.Piles["b"] = EPileState::Primary;
        f.Reconcile(after);
        UNIT_ASSERT(f.State.Operations.at(operation).Status == EOperationStatus::Paused);
        auto manual = f.Command(ECommandKind::SetMode);
        manual.Reason = "take ownership again";
        manual.Ownership = Ownership();
        f.Commit(manual);
        UNIT_ASSERT(f.State.Operations.at(operation).Status == EOperationStatus::Paused);
        auto resume = f.Command(ECommandKind::Resume);
        resume.OperationId = operation;
        resume.ExpectedWorkflowRevision = f.State.Operations.at(operation).Revision;
        f.Commit(resume);
        UNIT_ASSERT(f.State.Operations.at(operation).Status == EOperationStatus::Queued);
    }

    Y_UNIT_TEST(ObserveBeforeLateAcceptanceCannotResumeMaintenanceImplicitly) {
        // Exercise both sides of the return-to-Manual race and all live/timeout
        // acceptance paths. None may turn the explicit pause into a next send.
        for (bool manualBeforeAcceptance : {false, true}) {
            for (unsigned acceptancePath : {0u, 1u, 2u}) {
                TFixture f;
                const auto operation = f.Operation(EOperationKind::PrimaryMaintenance, "a", "b");
                const auto attempt = f.Intent(operation);
                auto observe = f.Command(ECommandKind::SetMode);
                observe.Mode = ERequestedMode::Observe;
                observe.Reason = "pause while promotion RPC is pending";
                f.Commit(observe);
                auto manual = [&]() {
                    auto command = f.Command(ECommandKind::SetMode);
                    command.Reason = "take ownership again";
                    command.Ownership = Ownership();
                    f.Commit(command);
                };
                if (manualBeforeAcceptance) {
                    manual();
                }
                if (acceptancePath == 0) {
                    f.Commit(f.Rpc(attempt));
                } else {
                    if (acceptancePath == 2) {
                        f.Commit(f.Rpc(attempt, false));
                        UNIT_ASSERT(f.State.Operations.at(operation).Status == EOperationStatus::Paused);
                    }
                    auto promoted = f.Current;
                    ++promoted.Generation;
                    promoted.Piles["b"] = EPileState::Promoted;
                    f.Commit(f.Acceptance(attempt, promoted));
                    if (acceptancePath == 1) {
                        f.Commit(f.Rpc(attempt));
                    }
                }
                UNIT_ASSERT(f.State.Operations.at(operation).Status == EOperationStatus::Paused);
                if (!manualBeforeAcceptance) {
                    manual();
                }
                auto after = f.Current;
                after.Generation += 2;
                after.Piles["a"] = EPileState::Synchronized;
                after.Piles["b"] = EPileState::Primary;
                f.Reconcile(after);
                UNIT_ASSERT(f.State.Operations.at(operation).Status == EOperationStatus::Paused);
                UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(operation).NextStep, 1);
                UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(operation).Attempts.size(), 1);
                auto stale = f.Command(ECommandKind::AdmitAttempt);
                stale.OperationId = operation;
                stale.ExpectedWorkflowRevision = f.State.Operations.at(operation).Revision;
                stale.Attempt.Step = 1;
                UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, stale).Result.Code, "STALE_WORKFLOW_STEP");
                auto resume = f.Command(ECommandKind::Resume);
                resume.OperationId = operation;
                resume.ExpectedWorkflowRevision = f.State.Operations.at(operation).Revision;
                f.Commit(resume);
                f.Current = after;
                f.Intent(operation);
                UNIT_ASSERT_VALUES_EQUAL(f.State.Operations.at(operation).Attempts.size(), 2);
            }
        }
    }

    Y_UNIT_TEST(ExternalRecoveryIsExplicitUnfencedAdoptionAndPreservesUnknownRpc) {
        TFixture f;
        const auto operation = f.Operation(EOperationKind::Rejoin, "c");
        const auto attempt = f.Intent(operation);
        f.Commit(f.Rpc(attempt, false));
        const auto rpcDetail = f.State.Attempts.at(attempt).RpcDetail;
        auto resolve = f.Command(ECommandKind::ResolveExternal);
        resolve.OperationId = operation;
        resolve.Observed = f.Current;
        resolve.Reason = "producer stopped; remote queue investigated";
        UNIT_ASSERT(!Apply(f.State, resolve).Result.Ok);
        resolve.AcknowledgeUnfencedRecovery = true;
        resolve.ExecutorStopped = true;
        resolve.ServerWorkReconciled = true;
        f.Commit(resolve);
        UNIT_ASSERT(!f.State.UnresolvedAttempt);
        UNIT_ASSERT(f.State.RequestedMode == ERequestedMode::Manual);
        UNIT_ASSERT(f.State.Attempts.at(attempt).RpcDisposition == ERpcDisposition::Unknown);
        UNIT_ASSERT(f.State.Operations.at(operation).Status == EOperationStatus::ExternallyResolved);
        UNIT_ASSERT(!f.State.Ownership);
        UNIT_ASSERT(!f.State.ExclusiveHistory);
        UNIT_ASSERT(!f.State.Attempts.at(attempt).Accepted);
        UNIT_ASSERT(f.State.Attempts.at(attempt).Disposition == EAttemptDisposition::ExternallyResolved);

        auto admit = f.Command(ECommandKind::AdmitOperation);
        admit.OperationId = "post-recovery";
        admit.Plan = *PlanWorkflow(f.Current, Request(EOperationKind::Rejoin, "c")).Plan;
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, admit).Result.Code, "OWNERSHIP_ACKNOWLEDGEMENT_REQUIRED");
        auto manual = f.Command(ECommandKind::SetMode);
        manual.Reason = "new exclusive administration interval after recovery";
        UNIT_ASSERT_VALUES_EQUAL(Apply(f.State, manual).Result.Code, "OWNERSHIP_ACKNOWLEDGEMENT_REQUIRED");
        manual.Ownership = Ownership();
        f.Commit(manual);
        UNIT_ASSERT(f.State.Ownership);
        UNIT_ASSERT(f.State.ExclusiveHistory);
        const auto next = f.Operation(EOperationKind::Rejoin, "c");
        f.Commit(f.Rpc(f.Intent(next)));
        auto after = f.Current;
        after.Generation += 2;
        after.Piles["c"] = EPileState::Synchronized;
        f.Reconcile(after, {"c"});
        UNIT_ASSERT(f.State.Operations.at(next).Status == EOperationStatus::Succeeded);
        UNIT_ASSERT(HandoverComplete(f.State));
        UNIT_ASSERT(f.State.Attempts.at(attempt).RpcDisposition == ERpcDisposition::Unknown);
        UNIT_ASSERT_VALUES_EQUAL(f.State.Attempts.at(attempt).RpcDetail, rpcDetail);
        UNIT_ASSERT(!f.State.Attempts.at(attempt).Accepted);
    }

    Y_UNIT_TEST(CheckpointTailReplayReceiptsAndUint64HighWater) {
        TTempDir directory;
        chmod(directory.Name().c_str(), 0700);
        TDurableState saved;
        TDurableCommand command;
        {
            TLocalStore store(directory.Name(), Binding());
            auto state = store.Load();
            UNIT_ASSERT(!state.Ownership);
            command.Id = "durable-high-water";
            command.RequestId = "durable-request";
            command.RequestHash = "body";
            command.Kind = ECommandKind::AdvanceGeneration;
            command.Fresh = command.Coherent = true;
            command.Observed.Generation = std::numeric_limits<ui64>::max();
            auto proposal = store.Propose(state, command);
            UNIT_ASSERT(proposal.Status == EProposalStatus::Committed);
            UNIT_ASSERT(proposal.Result.Ok);
            saved = *proposal.State;
            store.Checkpoint(saved);
            UNIT_ASSERT(store.LookupRequest("durable-request"));
            auto next = command;
            next.Id = "tail";
            next.RequestId.reset();
            proposal = store.Propose(saved, next);
            UNIT_ASSERT(proposal.Status == EProposalStatus::Committed);
            saved = *proposal.State;
        }
        TLocalStore reopened(directory.Name(), Binding("process-2"));
        const auto recovered = reopened.Load();
        UNIT_ASSERT_VALUES_EQUAL(recovered.AcceptedGenerationHighWater, std::numeric_limits<ui64>::max());
        UNIT_ASSERT_VALUES_EQUAL(SerializeState(recovered), SerializeState(saved));
        UNIT_ASSERT_VALUES_EQUAL(reopened.Stats().ReplayCommands, 1);
        UNIT_ASSERT_VALUES_EQUAL(reopened.Stats().LogRows, 2);
        const auto replay = reopened.Propose(recovered, command);
        UNIT_ASSERT(replay.Status == EProposalStatus::Committed);
        UNIT_ASSERT_VALUES_EQUAL(replay.Result.Index, 1);
    }

    Y_UNIT_TEST(OversizedCheckpointKeepsPreviousSnapshotAndStoreAvailable) {
        TTempDir directory;
        chmod(directory.Name().c_str(), 0700);
        TDurableState saved;
        {
            TLocalStore store(directory.Name(), Binding());
            TDurableCommand command;
            command.Id = "first";
            command.Kind = ECommandKind::AdvanceGeneration;
            command.Fresh = command.Coherent = true;
            command.Observed.Generation = 1;
            auto proposal = store.Propose(store.Load(), command);
            UNIT_ASSERT(proposal.Status == EProposalStatus::Committed);
            saved = *proposal.State;
            UNIT_ASSERT(store.Checkpoint(saved));
            const auto snapshotBytes = store.Stats().SnapshotBytes;
            command.Id = "second";
            command.Observed.Generation = 2;
            proposal = store.Propose(saved, command);
            UNIT_ASSERT(proposal.Status == EProposalStatus::Committed);
            saved = *proposal.State;
            store.InjectFault(EStoreFault::OversizedCheckpoint);
            UNIT_ASSERT(!store.Checkpoint(saved));
            UNIT_ASSERT(!store.Failed());
            UNIT_ASSERT_VALUES_EQUAL(store.Stats().SnapshotBytes, snapshotBytes);
            command.Id = "third";
            command.Observed.Generation = 3;
            proposal = store.Propose(saved, command);
            UNIT_ASSERT(proposal.Status == EProposalStatus::Committed);
            saved = *proposal.State;
        }
        TLocalStore reopened(directory.Name(), Binding("process-2"));
        UNIT_ASSERT_VALUES_EQUAL(SerializeState(reopened.Load()), SerializeState(saved));
        UNIT_ASSERT_VALUES_EQUAL(reopened.Stats().ReplayCommands, 2);
    }

    Y_UNIT_TEST(StoreCreatesAndSyncsNestedStateDirectory) {
        TTempDir parent;
        const TString directory = parent.Name() + "/state/";
        {
            TLocalStore store(directory, Binding());
            UNIT_ASSERT(!store.Failed());
        }
        struct stat info;
        UNIT_ASSERT(lstat((parent.Name() + "/state").c_str(), &info) == 0 && S_ISDIR(info.st_mode) && (info.st_mode & 0777) == 0700);
        TLocalStore reopened(directory, Binding("process-2"));
        UNIT_ASSERT(!reopened.Failed());
    }

    Y_UNIT_TEST(StateDirectoryParentIsResolvedPhysically) {
        TTempDir root;
        const TString base = TString(std::filesystem::canonical(root.Name().c_str()).c_str());
        UNIT_ASSERT(mkdir((base + "/a").c_str(), 0700) == 0);
        UNIT_ASSERT(mkdir((base + "/b").c_str(), 0700) == 0);
        UNIT_ASSERT(mkdir((base + "/b/c").c_str(), 0700) == 0);
        UNIT_ASSERT(symlink((base + "/b/c").c_str(), (base + "/a/link").c_str()) == 0);
        const TString directory = base + "/a/link/../state";
        {
            TLocalStore store(directory, Binding());
            UNIT_ASSERT(!store.Failed());
        }
        struct stat info;
        UNIT_ASSERT(lstat((base + "/b/state").c_str(), &info) == 0 && S_ISDIR(info.st_mode));
        UNIT_ASSERT_VALUES_EQUAL(PhysicalParentDirectory(directory), base + "/b");
        UNIT_ASSERT_VALUES_EQUAL(PhysicalParentDirectory(base + "/b/state/"), base + "/b");
    }

    Y_UNIT_TEST(UnknownWriteCompletionLookupBeforeAnyNewWrite) {
        for (auto fault : {EStoreFault::BeforeCommit, EStoreFault::AfterCommit}) {
            TTempDir directory;
            chmod(directory.Name().c_str(), 0700);
            TLocalStore store(directory.Name(), Binding());
            const auto state = store.Load();
            TDurableCommand command;
            command.Id = "unknown-command";
            command.Kind = ECommandKind::AdvanceGeneration;
            command.Fresh = command.Coherent = true;
            command.Observed.Generation = 100;
            store.InjectFault(fault);
            const auto unknown = store.Propose(state, command);
            UNIT_ASSERT(unknown.Status == EProposalStatus::Unknown);
            UNIT_ASSERT(store.Failed());
            UNIT_ASSERT(store.Propose(state, command).Status == EProposalStatus::Rejected);
            const auto resolved = store.ResolveUnknown(command.Id);
            UNIT_ASSERT(resolved.State);
            UNIT_ASSERT_VALUES_EQUAL(resolved.State->AppliedIndex, fault == EStoreFault::AfterCommit ? 1 : 0);
            UNIT_ASSERT(!store.Failed());
            UNIT_ASSERT_VALUES_EQUAL(store.LookupCommand(command.Id).has_value(), fault == EStoreFault::AfterCommit);
        }
    }

    Y_UNIT_TEST(ExclusiveLockBindingsAndProcessIncarnationCannotBeReused) {
        TTempDir directory;
        chmod(directory.Name().c_str(), 0700);
        {
            TLocalStore store(directory.Name(), Binding());
            UNIT_ASSERT_EXCEPTION(TLocalStore(directory.Name(), Binding("process-2")), yexception);
        }
        UNIT_ASSERT_EXCEPTION(TLocalStore(directory.Name(), Binding()), yexception);
        auto binding = Binding("process-2");
        binding.ClusterId = "other-cluster";
        UNIT_ASSERT_EXCEPTION(TLocalStore(directory.Name(), binding), yexception);
        binding = Binding("process-2");
        binding.MemberId = "other-member";
        UNIT_ASSERT_EXCEPTION(TLocalStore(directory.Name(), binding), yexception);
    }

    Y_UNIT_TEST(LogCorruptionAndNewSchemaFailClosed) {
        for (bool schema : {false, true}) {
            TTempDir directory;
            chmod(directory.Name().c_str(), 0700);
            {
                TLocalStore store(directory.Name(), Binding());
                TDurableCommand command;
                command.Id = "command";
                command.Kind = ECommandKind::AdvanceGeneration;
                command.Fresh = command.Coherent = true;
                UNIT_ASSERT(store.Propose(store.Load(), command).Status == EProposalStatus::Committed);
            }
            MutateDatabase(directory.Name(), schema ? "UPDATE meta SET value='999' WHERE key='schema'" : "UPDATE log SET payload='corrupt' WHERE idx=1");
            UNIT_ASSERT_EXCEPTION(TLocalStore(directory.Name(), Binding("process-2")), yexception);
        }
    }

    Y_UNIT_TEST(SnapshotCorruptionAndMissingLogCannotSilentlyResetState) {
        for (bool missingLog : {false, true}) {
            TTempDir directory;
            chmod(directory.Name().c_str(), 0700);
            {
                TLocalStore store(directory.Name(), Binding());
                store.Checkpoint(store.Load());
            }
            MutateDatabase(directory.Name(), missingLog ? "DROP TABLE log" : "UPDATE snapshot SET state='{}'");
            UNIT_ASSERT_EXCEPTION(TLocalStore(directory.Name(), Binding("process-2")), yexception);
        }
    }

    Y_UNIT_TEST(InvalidOwnerPermissionsAreRejected) {
        TTempDir directory;
        chmod(directory.Name().c_str(), 0755);
        UNIT_ASSERT_EXCEPTION(TLocalStore(directory.Name(), Binding()), yexception);
    }

    Y_UNIT_TEST(CheckpointCannotPublishUncommittedProjectionAndFailureRetainsLog) {
        TTempDir directory;
        chmod(directory.Name().c_str(), 0700);
        {
            TLocalStore store(directory.Name(), Binding());
            auto state = store.Load();
            auto invented = state;
            invented.RequestedMode = ERequestedMode::Auto;
            UNIT_ASSERT_EXCEPTION(store.Checkpoint(invented), yexception);
            state = store.Load();
            store.InjectFault(EStoreFault::BeforeCheckpointCommit);
            UNIT_ASSERT_EXCEPTION(store.Checkpoint(state), yexception);
        }
        TLocalStore reopened(directory.Name(), Binding("process-2"));
        UNIT_ASSERT(reopened.Load().RequestedMode == ERequestedMode::Manual);
        UNIT_ASSERT_VALUES_EQUAL(reopened.Stats().SnapshotBytes, 0);
    }

    Y_UNIT_TEST(CommandIndexExhaustionFailsWithoutWraparound) {
        TDurableState state;
        state.AppliedIndex = std::numeric_limits<i64>::max();
        TDurableCommand command;
        command.Id = "cannot-wrap";
        const auto result = Apply(state, command);
        UNIT_ASSERT(!result.Result.Ok);
        UNIT_ASSERT_VALUES_EQUAL(result.State.AppliedIndex, state.AppliedIndex);
    }
}

} // namespace NBridgeSkipper
