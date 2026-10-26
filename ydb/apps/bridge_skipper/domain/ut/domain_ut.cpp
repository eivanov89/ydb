#include <ydb/apps/bridge_skipper/domain/policy.h>
#include <ydb/apps/bridge_skipper/domain/scheduler.h>
#include <ydb/apps/bridge_skipper/domain/topology.h>

#include <library/cpp/testing/unittest/registar.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>

#include <util/string/builder.h>
#include <util/generic/yexception.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace NBridgeSkipper {
namespace {

class TManualClock : public ITimeProvider {
public:
    TInstant Value = TInstant::Seconds(100);

    TInstant Now() override {
        return Value;
    }
};

struct TClockScope {
    TIntrusivePtr<TManualClock> Clock = new TManualClock;

    TClockScope() {
        TSkipperClock::RegisterProvider(Clock);
    }

    ~TClockScope() {
        TSkipperClock::RegisterProvider(CreateDefaultTimeProvider());
    }
};

struct TClockProbe {
    std::mutex Mutex;
    std::condition_variable Changed;
    bool Entered = false;
    bool Release = false;
    bool Destroyed = false;
};

class TBlockingClock final : public ITimeProvider {
public:
    explicit TBlockingClock(std::shared_ptr<TClockProbe> probe)
        : Probe(std::move(probe)) {
    }

    ~TBlockingClock() override {
        std::lock_guard guard(Probe->Mutex);
        Probe->Destroyed = true;
    }

    TInstant Now() override {
        std::unique_lock lock(Probe->Mutex);
        Probe->Entered = true;
        Probe->Changed.notify_all();
        Probe->Changed.wait(lock, [&] { return Probe->Release; });
        return TInstant::Seconds(42);
    }

private:
    std::shared_ptr<TClockProbe> Probe;
};

TProfileQualification Qualified(ESourceKind source = ESourceKind::GlobalBsc) {
    TProfileQualification q;
    q.Profile = source == ESourceKind::GlobalBsc ? "global_bsc_v1" : "static_whiteboard_v1";
    q.SupportedBuild = "explicit-unit-fixture-only";
    q.FixtureRecord = "unit-policy-fixture-is-not-production-qualification";
    q.Source = source;
    q.Qualified = true;
    q.CapabilityPresent = true;
    q.IndependentLocalOrigins = source != ESourceKind::GlobalBsc;
    return q;
}

TClusterView View() {
    TClusterView result;
    result.Converted = true;
    result.Generation = 7;
    result.Piles = {{"a", EPileState::Primary}, {"b", EPileState::Synchronized}, {"c", EPileState::Synchronized}};
    result.PileOrder = {"a", "b", "c"};
    result.ReceivedAt = TInstant::Seconds(100);
    return result;
}

TPolicyInput Policy(ESourceKind source = ESourceKind::GlobalBsc) {
    TPolicyInput input;
    input.DecisionId = "decision";
    input.ProcessId = "process";
    input.RoundId = 1;
    input.RoundStartedAt = TInstant::Seconds(95);
    input.RoundDeadline = TInstant::Seconds(107);
    input.View = View();
    input.Epoch = {1, CanonicalFingerprint(input.View), "policy-v1", 1};
    input.Qualification = Qualified(source);
    input.PlacementAcknowledged = true;
    ui32 id = 1;
    for (const auto& [pile, state] : input.View.Piles) {
        (void)state;
        TNode node;
        node.NodeId = id;
        node.Pile = pile;
        node.Binding = EBinding::Explicit;
        node.DirectEndpoint = true;
        node.Endpoint = TStringBuilder() << "grpc://node-" << id << ":2135";
        node.AdminFingerprint = input.Epoch.AdminFingerprint;
        node.AdminObservedAt = TInstant::Seconds(100);
        input.Nodes.push_back(node);
        input.Samples.push_back({pile, {id}, 1});
        TObservation observation;
        observation.RequestId = TStringBuilder() << "request-" << id;
        observation.ProcessId = input.ProcessId;
        observation.RoundId = input.RoundId;
        observation.Epoch = input.Epoch;
        observation.NodeId = id;
        observation.ResponderPile = pile;
        observation.BeforeAt = TInstant::Seconds(95);
        observation.HealthStartedAt = TInstant::Seconds(96);
        observation.FinishedAt = TInstant::Seconds(99);
        observation.BeforeFingerprint = input.Epoch.AdminFingerprint;
        observation.AfterFingerprint = input.Epoch.AdminFingerprint;
        observation.Profile = input.Qualification.Profile;
        observation.Source = source;
        observation.IdentityVerified = true;
        observation.DirectLocalOrigin = source != ESourceKind::GlobalBsc;
        observation.Origin = pile;
        observation.Vantage = pile;
        for (const auto& [target, targetState] : input.View.Piles) {
            (void)targetState;
            observation.Targets[target] = {EAssessment::NoFailureReported, true, {}};
        }
        input.Observations.push_back(observation);
        ++id;
    }
    return input;
}

void FailAll(TPolicyInput& input, const TString& pile) {
    for (auto& observation : input.Observations) {
        observation.Targets[pile] = {EAssessment::Failed, true, {}};
    }
}

void Rebind(TPolicyInput& input) {
    input.Epoch.AdminFingerprint = CanonicalFingerprint(input.View);
    for (auto& observation : input.Observations) {
        observation.Epoch = input.Epoch;
        observation.BeforeFingerprint = input.Epoch.AdminFingerprint;
        observation.AfterFingerprint = input.Epoch.AdminFingerprint;
    }
    for (auto& node : input.Nodes) {
        node.AdminFingerprint = input.Epoch.AdminFingerprint;
    }
}

} // namespace

Y_UNIT_TEST_SUITE(TBridgeSkipperDomain) {
    Y_UNIT_TEST(SkipperClockSupportsDefaultAndExplicitProviderReplacement) {
        TClockScope scope;
        UNIT_ASSERT_VALUES_EQUAL(TSkipperClock::Now(), TInstant::Seconds(100));
        auto other = MakeIntrusive<TManualClock>();
        other->Value = TInstant::Seconds(200);
        TSkipperClock::RegisterProvider(other);
        UNIT_ASSERT_VALUES_EQUAL(TSkipperClock::Now(), TInstant::Seconds(200));
        TSkipperClock::RegisterProvider({});
        UNIT_ASSERT(TSkipperClock::Now());
    }

    Y_UNIT_TEST(SkipperClockRetainsInFlightProviderAcrossConcurrentReplacement) {
        TClockScope scope;
        const auto probe = std::make_shared<TClockProbe>();
        TSkipperClock::RegisterProvider(MakeIntrusive<TBlockingClock>(probe));
        TInstant observed;
        std::thread reader([&] { observed = TSkipperClock::Now(); });
        {
            std::unique_lock lock(probe->Mutex);
            probe->Changed.wait(lock, [&] { return probe->Entered; });
        }
        TSkipperClock::RegisterProvider(scope.Clock);
        bool retained = false;
        {
            std::lock_guard guard(probe->Mutex);
            retained = !probe->Destroyed;
            probe->Release = true;
        }
        probe->Changed.notify_all();
        reader.join();
        UNIT_ASSERT(retained);
        UNIT_ASSERT_VALUES_EQUAL(observed, TInstant::Seconds(42));
        UNIT_ASSERT_VALUES_EQUAL(TSkipperClock::Now(), TInstant::Seconds(100));
        {
            std::lock_guard guard(probe->Mutex);
            UNIT_ASSERT(probe->Destroyed);
        }
    }

    Y_UNIT_TEST(FingerprintSeparatesGenerationMetadataAndNames) {
        auto view = View();
        const auto original = CanonicalFingerprint(view);
        UNIT_ASSERT_VALUES_EQUAL(original.size(), 64);
        std::reverse(view.PileOrder.begin(), view.PileOrder.end());
        UNIT_ASSERT_VALUES_EQUAL(original, CanonicalFingerprint(view));
        view.Endpoint = "another-node";
        view.ReceivedAt += TDuration::Seconds(1);
        UNIT_ASSERT_VALUES_EQUAL(original, CanonicalFingerprint(view));
        ++view.Generation;
        UNIT_ASSERT(original != CanonicalFingerprint(view));
        --view.Generation;
        view.Pending.push_back({"ADD", TString(""), "", ""});
        UNIT_ASSERT(original != CanonicalFingerprint(view));
        const auto emptyTarget = CanonicalFingerprint(view);
        view.Pending.front().Pile.reset();
        UNIT_ASSERT(emptyTarget != CanonicalFingerprint(view));
        view.Pending.clear();
        view.Metadata["conversion-progress"] = "updated-without-generation";
        UNIT_ASSERT(original != CanonicalFingerprint(view));
    }

    Y_UNIT_TEST(UnknownPendingMetadataCannotBypassDisjointTargetGuards) {
        auto view = View();
        view.Pending.push_back({"future-operation", TString("c"), "", ""});
        UNIT_ASSERT_VALUES_EQUAL(ValidateExactUpdate(view, {{"b", EPileState::Disconnected}}), "UNSUPPORTED_PENDING_METADATA");
        view.Pending.front().Kind = "ADD";
        view.Pending.front().Pile.reset();
        UNIT_ASSERT_VALUES_EQUAL(ValidateClusterView(view), "UNSUPPORTED_PENDING_METADATA");
        view.Pending.front().Pile = "c";
        UNIT_ASSERT(ValidateExactUpdate(view, {{"b", EPileState::Disconnected}}).empty());
        view.Pending.push_back({"REMOVE", TString("c"), "", ""});
        UNIT_ASSERT_VALUES_EQUAL(ValidateClusterView(view), "CONTRADICTORY_PENDING_METADATA");
    }

    Y_UNIT_TEST(ExactBatchPreservesOmittedAndDemotesAbandonedPromotion) {
        TClockScope clock;
        auto input = Policy();
        input.View.Piles["c"] = EPileState::Promoted;
        input.View.Piles["untouched"] = EPileState::NotSynchronized;
        input.View.PileOrder.push_back("untouched");
        Rebind(input);
        FailAll(input, "a");
        const auto result = EvaluatePolicy(input);
        UNIT_ASSERT(result.Kind == EDecisionKind::Failover);
        UNIT_ASSERT_VALUES_EQUAL(*result.Candidate, "b");
        UNIT_ASSERT(result.Update.at("a") == EPileState::Disconnected);
        UNIT_ASSERT(result.Update.at("b") == EPileState::Primary);
        UNIT_ASSERT(result.Update.at("c") == EPileState::Synchronized);
        UNIT_ASSERT(!result.Update.contains("untouched"));
    }

    Y_UNIT_TEST(SoleGlobalSurvivorDisconnectsAllFailedInOneVector) {
        TClockScope clock;
        auto input = Policy();
        FailAll(input, "a");
        FailAll(input, "c");
        input.Observations = {input.Observations[1]};
        const auto result = EvaluatePolicy(input);
        UNIT_ASSERT(result.Kind == EDecisionKind::Failover);
        UNIT_ASSERT_VALUES_EQUAL(result.Update.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(result.Targets.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(result.Endpoint->NodeId, 2);
    }

    Y_UNIT_TEST(DeterministicCompletionPermutations) {
        TClockScope clock;
        auto input = Policy();
        FailAll(input, "b");
        const auto expected = CanonicalDecision(EvaluatePolicy(input));
        std::vector<size_t> order = {0, 1, 2};
        const auto original = input.Observations;
        do {
            input.Observations.clear();
            for (const auto index : order) {
                input.Observations.push_back(original[index]);
            }
            UNIT_ASSERT_VALUES_EQUAL(CanonicalDecision(EvaluatePolicy(input)), expected);
        } while (std::next_permutation(order.begin(), order.end()));
    }

    Y_UNIT_TEST(UnknownAndUnqualifiedNeverCertify) {
        TClockScope clock;
        auto input = Policy();
        FailAll(input, "b");
        input.Qualification.Qualified = false;
        UNIT_ASSERT_VALUES_EQUAL(EvaluatePolicy(input).Reason, "UNQUALIFIED_EVIDENCE_PROFILE");
        input.Qualification = Qualified();
        input.Qualification.CapabilityPresent = false;
        UNIT_ASSERT_VALUES_EQUAL(EvaluatePolicy(input).Reason, "UNQUALIFIED_EVIDENCE_PROFILE");
        input.Qualification = Qualified();
        for (auto& observation : input.Observations) {
            observation.Targets["b"] = {};
        }
        UNIT_ASSERT(EvaluatePolicy(input).Kind != EDecisionKind::Failover);
    }

    Y_UNIT_TEST(GlobalContradictionBlocksEvenMinorityAndPartialUnknownDoesNot) {
        TClockScope clock;
        auto input = Policy();
        FailAll(input, "b");
        input.Observations[0].Targets.clear();
        UNIT_ASSERT(EvaluatePolicy(input).Kind == EDecisionKind::Failover);
        input.Observations[0].Targets["b"] = {EAssessment::NoFailureReported, true, {}};
        UNIT_ASSERT_VALUES_EQUAL(EvaluatePolicy(input).Reason, "CONTRADICTORY_GLOBAL_ASSESSMENT");
    }

    Y_UNIT_TEST(GlobalDoesNotStitchPerTargetMajorities) {
        TClockScope clock;
        auto input = Policy();
        input.Samples = {{"a", {1, 4, 5}, 2}};
        auto one = input.Observations[0];
        auto two = one;
        auto three = one;
        two.NodeId = 4;
        two.RequestId = "two";
        three.NodeId = 5;
        three.RequestId = "three";
        for (auto* observation : {&one, &two, &three}) {
            observation->Targets["b"] = {EAssessment::Failed, true, {}};
            observation->Targets["c"] = {EAssessment::Failed, true, {}};
        }
        one.Targets["c"] = {};
        two.Targets["b"] = {};
        input.Observations = {one, two, three};
        UNIT_ASSERT_VALUES_EQUAL(EvaluatePolicy(input).Reason, "NO_COMPLETE_CORROBORATED_GLOBAL_VECTOR");
    }

    Y_UNIT_TEST(LocalStrictMajorityRetainsUnknownInDenominator) {
        TClockScope clock;
        auto input = Policy(ESourceKind::LocalWhiteboard);
        input.Observations[0].Targets["b"] = {EAssessment::Failed, true, {}};
        input.Observations[1].Targets["b"] = {EAssessment::NoFailureReported, true, {}};
        input.Observations[2].Targets["b"] = {};
        UNIT_ASSERT(EvaluatePolicy(input).Kind != EDecisionKind::Failover);
    }

    Y_UNIT_TEST(LocalMajorityChoosesBytewiseHealthyCandidate) {
        TClockScope clock;
        auto input = Policy(ESourceKind::LocalWhiteboard);
        input.Observations[0].Targets["b"] = {EAssessment::Failed, true, {}};
        input.Observations[0].Targets["c"] = {EAssessment::Failed, true, {}};
        input.Observations[1].Targets["a"] = {EAssessment::Failed, true, {}};
        input.Observations[2].Targets["a"] = {EAssessment::Failed, true, {}};
        const auto result = EvaluatePolicy(input);
        UNIT_ASSERT(result.Kind == EDecisionKind::Failover);
        UNIT_ASSERT_VALUES_EQUAL(*result.Candidate, "b");
        UNIT_ASSERT_VALUES_EQUAL(result.Targets.size(), 1);
        UNIT_ASSERT(result.Targets.contains("a"));
    }

    Y_UNIT_TEST(TwoActiveLocalMutualPreferenceRequiresCompleteTopology) {
        TClockScope clock;
        auto input = Policy(ESourceKind::LocalWhiteboard);
        input.View.Piles.erase("c");
        input.View.PileOrder.pop_back();
        input.Samples.pop_back();
        input.Observations.pop_back();
        Rebind(input);
        input.Observations[0].Targets["b"] = {EAssessment::Failed, true, {}};
        input.Observations[1].Targets["a"] = {EAssessment::Failed, true, {}};
        auto result = EvaluatePolicy(input);
        UNIT_ASSERT_VALUES_EQUAL(result.Reason, "TWO_ACTIVE_LOCAL_PRIMARY_PREFERENCE");
        UNIT_ASSERT_VALUES_EQUAL(*result.Candidate, "a");
        input.View.Piles["c"] = EPileState::NotSynchronized;
        input.View.PileOrder.push_back("c");
        Rebind(input);
        UNIT_ASSERT(EvaluatePolicy(input).Kind != EDecisionKind::Failover);
    }

    Y_UNIT_TEST(SharedLocalOriginCannotCountAsIndependentReporters) {
        TClockScope clock;
        auto input = Policy(ESourceKind::LocalWhiteboard);
        FailAll(input, "b");
        for (auto& observation : input.Observations) {
            observation.Origin = "one-shared-root";
        }
        UNIT_ASSERT_VALUES_EQUAL(EvaluatePolicy(input).Reason, "NO_REPORTERS");
    }

    Y_UNIT_TEST(EmptyNameAndUnsupportedStateRemainDistinct) {
        TClockScope clock;
        auto input = Policy();
        const auto old = input.View.Piles.extract("a");
        input.View.Piles[""] = old.mapped();
        input.View.PileOrder[0] = "";
        input.Nodes[0].Pile = "";
        input.Samples[0].Pile = "";
        input.Observations[0].ResponderPile = "";
        for (auto& observation : input.Observations) {
            observation.Targets.erase("a");
            observation.Targets[""] = {EAssessment::Failed, true, {}};
        }
        Rebind(input);
        auto result = EvaluatePolicy(input);
        UNIT_ASSERT(result.Kind == EDecisionKind::Failover);
        UNIT_ASSERT(result.Primary && result.Primary->empty());
        UNIT_ASSERT(result.Update.contains(""));
        input.View.Piles["b"] = static_cast<EPileState>(99);
        Rebind(input);
        UNIT_ASSERT_VALUES_EQUAL(EvaluatePolicy(input).Reason, "UNSUPPORTED_PUBLIC_STATE");
    }

    Y_UNIT_TEST(SuspendedPendingAndResyncFailuresRequireOperator) {
        TClockScope clock;
        auto input = Policy();
        FailAll(input, "b");
        input.View.Piles["b"] = EPileState::Suspended;
        Rebind(input);
        UNIT_ASSERT_VALUES_EQUAL(EvaluatePolicy(input).Reason, "OPERATOR_ACTION_REQUIRED");
        input.View.Piles["b"] = EPileState::NotSynchronized;
        Rebind(input);
        UNIT_ASSERT_VALUES_EQUAL(EvaluatePolicy(input).Reason, "RESYNC_FAILURE_EVIDENCE_UNSUPPORTED");
        input.View.Piles["b"] = EPileState::Synchronized;
        input.View.Pending.push_back({"REMOVE", TString("b"), "", ""});
        Rebind(input);
        UNIT_ASSERT_VALUES_EQUAL(EvaluatePolicy(input).Reason, "PENDING_TARGET");
        input.View.Pending.front().Pile = "c";
        Rebind(input);
        UNIT_ASSERT(EvaluatePolicy(input).Kind == EDecisionKind::Failover);
    }

    Y_UNIT_TEST(FixedTwoNodeSampleNeverShrinksDenominator) {
        auto view = View();
        auto input = Policy();
        auto& observation = input.Observations[0];
        const auto report = AggregatePile({"a", {1, 2}, 2}, view, {&observation}, Qualified(), TInstant::Seconds(100));
        UNIT_ASSERT_VALUES_EQUAL(report.Fresh, 1);
        UNIT_ASSERT(!report.Eligible);
        UNIT_ASSERT(report.Targets.at("a").Value == EAssessment::Unknown);
    }

    Y_UNIT_TEST(AdminSelectionRejectsHigherSingletonAndSameGenerationMetadataConflict) {
        const auto now = TInstant::Seconds(100);
        auto view = View();
        auto higher = view;
        ++higher.Generation;
        std::vector<TFrozenSample> samples = {{"a", {1, 2, 3}, 2}};
        std::vector<TAdminReply> replies = {{1, view}, {2, view}, {3, higher}};
        auto result = SelectAdminView(samples, replies, {"a", "b", "c"}, 7, now, TDuration::Seconds(20));
        UNIT_ASSERT(result.View && result.View->Generation == 7);
        UNIT_ASSERT_VALUES_EQUAL(result.Blocker, "UNCORROBORATED_HIGHER_GENERATION");
        replies[2].View = view;
        replies[2].View.Pending.push_back({"ADD", TString("c"), "progress", ""});
        result = SelectAdminView(samples, replies, {"a", "b", "c"}, 7, now, TDuration::Seconds(20));
        UNIT_ASSERT_VALUES_EQUAL(result.Blocker, "LIFECYCLE_METADATA_RESAMPLE");
        replies[2].View = view;
        replies[2].View.Piles["b"] = EPileState::Disconnected;
        result = SelectAdminView(samples, replies, {"a", "b", "c"}, 7, now, TDuration::Seconds(20));
        UNIT_ASSERT_VALUES_EQUAL(result.Blocker, "STATE_CONFLICT");
    }

    Y_UNIT_TEST(AdminValidityUsesFreshestRequiredQuorumWithoutReaging) {
        auto view = View();
        std::vector<TAdminReply> replies = {{1, view}, {2, view}, {3, view}};
        replies[0].View.ReceivedAt = TInstant::Seconds(90);
        replies[1].View.ReceivedAt = TInstant::Seconds(99);
        replies[2].View.ReceivedAt = TInstant::Seconds(100);
        const std::vector<TFrozenSample> samples = {{"a", {1, 2, 3}, 2}};
        auto selected = SelectAdminView(samples, replies, {"a", "b", "c"}, 7, TInstant::Seconds(100), TDuration::Seconds(20));
        UNIT_ASSERT(selected.View && selected.Blocker.empty());
        UNIT_ASSERT_VALUES_EQUAL(selected.View->ReceivedAt, TInstant::Seconds(99));
        selected = SelectAdminView(samples, replies, {"a", "b", "c"}, 7, TInstant::Seconds(118), TDuration::Seconds(20));
        UNIT_ASSERT(selected.View && selected.Blocker.empty());
        UNIT_ASSERT_VALUES_EQUAL(selected.View->ReceivedAt, TInstant::Seconds(99));
        // Refreshing only the exact candidate cannot renew the q=2 baseline.
        replies[2].View.ReceivedAt = TInstant::Seconds(119);
        selected = SelectAdminView(samples, replies, {"a", "b", "c"}, 7, TInstant::Seconds(119), TDuration::Seconds(20));
        UNIT_ASSERT(!selected.View);
        UNIT_ASSERT_VALUES_EQUAL(selected.Blocker, "NO_CORROBORATED_ADMIN_VIEW");
    }

    Y_UNIT_TEST(ObservationBracketIdentityAndTtlAreHardGates) {
        auto input = Policy();
        auto observation = input.Observations[0];
        UNIT_ASSERT(ValidateObservation(observation, input.Epoch, input.ProcessId, input.Qualification,
            TInstant::Seconds(100), input.RoundDeadline).empty());
        observation.BeforeAt = TInstant::Seconds(94);
        UNIT_ASSERT(!ValidateObservation(observation, input.Epoch, input.ProcessId, input.Qualification,
            TInstant::Seconds(100), input.RoundDeadline).empty());
        observation = input.Observations[0];
        UNIT_ASSERT(!ValidateObservation(observation, input.Epoch, input.ProcessId, input.Qualification,
            TInstant::Seconds(116), input.RoundDeadline).empty());
        observation.ResponderPile.reset();
        UNIT_ASSERT_VALUES_EQUAL(ValidateObservation(observation, input.Epoch, input.ProcessId, input.Qualification,
            TInstant::Seconds(100), input.RoundDeadline), "INVALID_RESPONDER_IDENTITY");
    }

    Y_UNIT_TEST(ConfirmationUsesActualStartsAndNeverRefreshesExpiry) {
        TClockScope clock;
        auto input = Policy();
        FailAll(input, "b");
        auto decision = EvaluatePolicy(input);
        TConfirmation confirmation;
        UNIT_ASSERT(!confirmation.CompleteRound(decision, TInstant::Seconds(100)));
        decision.RoundId = 2;
        decision.RoundStartedAt += TDuration::Seconds(15);
        decision.EvidenceExpiry = TInstant::Seconds(130);
        UNIT_ASSERT(confirmation.CompleteRound(decision, TInstant::Seconds(120)));
        decision.RoundId = 3;
        decision.RoundStartedAt += TDuration::Seconds(21);
        decision.EvidenceExpiry = TInstant::Seconds(150);
        UNIT_ASSERT(!confirmation.CompleteRound(decision, TInstant::Seconds(140)));
        decision.Kind = EDecisionKind::Blocked;
        UNIT_ASSERT(!confirmation.CompleteRound(decision, TInstant::Seconds(140)));
        decision.Kind = EDecisionKind::Failover;
        decision.RoundId = 4;
        UNIT_ASSERT(!confirmation.CompleteRound(decision, TInstant::Seconds(140)));
    }

    Y_UNIT_TEST(ConfirmationRejectsChangedNormalizedDecisionOrProfile) {
        TClockScope clock;
        for (const bool profileChanges : {false, true}) {
            auto input = Policy();
            FailAll(input, "b");
            auto decision = EvaluatePolicy(input);
            decision.EvidenceExpiry = TInstant::Seconds(300);
            TConfirmation confirmation;
            UNIT_ASSERT(!confirmation.CompleteRound(decision, TInstant::Seconds(100)));
            ++decision.RoundId;
            decision.RoundStartedAt += TDuration::Seconds(15);
            UNIT_ASSERT(confirmation.CompleteRound(decision, TInstant::Seconds(120)));
            ++decision.RoundId;
            decision.RoundStartedAt += TDuration::Seconds(15);
            if (profileChanges) {
                decision.Profile = "different-qualified-profile";
            } else {
                decision.Targets.insert("c");
                decision.Update["c"] = EPileState::Disconnected;
            }
            UNIT_ASSERT(!confirmation.CompleteRound(decision, TInstant::Seconds(135)));
        }
    }

    Y_UNIT_TEST(SchedulerRetainsUnsettledCreditsAndHasNoCatchupBurst) {
        TClockScope clock;
        auto input = Policy();
        TBoundedScheduler scheduler({});
        const auto first = scheduler.Start(input.Nodes);
        UNIT_ASSERT(first && first->Launch.size() == 3);
        clock.Clock->Value += TDuration::Seconds(12);
        UNIT_ASSERT(scheduler.CloseDue());
        UNIT_ASSERT_VALUES_EQUAL(scheduler.InFlightChains(), 3);
        clock.Clock->Value += TDuration::Seconds(3);
        const auto second = scheduler.Start(input.Nodes);
        UNIT_ASSERT(second && second->Blocker == "UNSETTLED_CHAIN_CAPACITY" && second->Launch.empty());
        for (const auto& node : input.Nodes) {
            UNIT_ASSERT(scheduler.Settle(first->Id, node.NodeId));
            UNIT_ASSERT(!scheduler.Settle(first->Id, node.NodeId));
        }
        clock.Clock->Value += TDuration::Seconds(200);
        const auto later = scheduler.Start(input.Nodes);
        UNIT_ASSERT(later && later->Launch.size() == 3);
        UNIT_ASSERT(!scheduler.Start(input.Nodes));
    }

    Y_UNIT_TEST(AdminCapacityReservesOperationSlots) {
        TClockScope clock;
        TSchedulerConfig config;
        config.GlobalAdminConcurrency = 3;
        TBoundedScheduler scheduler(config);
        UNIT_ASSERT(scheduler.AcquireAdmin("one", false));
        UNIT_ASSERT(!scheduler.AcquireAdmin("two", false));
        UNIT_ASSERT(scheduler.AcquireAdmin("two", true));
        UNIT_ASSERT(scheduler.AcquireAdmin("three", true));
        UNIT_ASSERT(!scheduler.AcquireAdmin("four", true));
        UNIT_ASSERT(!scheduler.AcquireAdmin("one", true));
        UNIT_ASSERT(scheduler.ReleaseAdmin("one"));
        UNIT_ASSERT_VALUES_EQUAL(scheduler.InFlightAdmin(), 2);
    }

    Y_UNIT_TEST(BlackholedNodeDoesNotSuppressFreshSurvivingMajority) {
        TClockScope clock;
        auto input = Policy();
        for (auto& node : input.Nodes) {
            node.Pile = "a";
        }
        TBoundedScheduler scheduler({});
        const auto first = scheduler.Start(input.Nodes);
        UNIT_ASSERT(first && first->Samples.size() == 1);
        UNIT_ASSERT_VALUES_EQUAL(first->Samples.front().Threshold, 2);
        UNIT_ASSERT(scheduler.Settle(first->Id, 2));
        UNIT_ASSERT(scheduler.Settle(first->Id, 3));
        clock.Clock->Value += TDuration::Seconds(15);
        const auto second = scheduler.Start(input.Nodes);
        UNIT_ASSERT(second && second->Launch.size() == 2);
        UNIT_ASSERT_VALUES_EQUAL(second->Samples.front().Nodes.size(), 3);
        UNIT_ASSERT_VALUES_EQUAL(second->Samples.front().Threshold, 2);
        UNIT_ASSERT_VALUES_EQUAL(scheduler.InFlightChains(), 3);
        for (const auto& node : second->Launch) {
            UNIT_ASSERT(node.NodeId != 1);
        }
    }

    Y_UNIT_TEST(GlobalUsesNewestNecessaryQuorumForExpiry) {
        TClockScope clock;
        auto input = Policy();
        FailAll(input, "b");
        input.Samples = {{"a", {1, 4, 5}, 2}};
        auto one = input.Observations[0];
        auto two = one;
        auto three = one;
        two.NodeId = 4;
        two.RequestId = "new-two";
        three.NodeId = 5;
        three.RequestId = "new-three";
        one.BeforeAt = TInstant::Seconds(80);
        one.HealthStartedAt = TInstant::Seconds(81);
        input.Observations = {one, two, three};
        const auto result = EvaluatePolicy(input);
        UNIT_ASSERT(result.Kind == EDecisionKind::Failover);
        UNIT_ASSERT_VALUES_EQUAL(result.EvidenceRequestIds.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(result.EvidenceExpiry, TInstant::Seconds(116));
    }

    Y_UNIT_TEST(LocalUsesFreshNecessaryQuorumsWithoutOlderExtraResponseExpiry) {
        TClockScope clock;
        auto input = Policy(ESourceKind::LocalWhiteboard);
        input.Samples[0] = {"a", {1, 4, 5}, 2};
        input.Observations[0].Targets["b"] = {EAssessment::Failed, true, {}};
        input.Observations[2].Targets["b"] = {EAssessment::Failed, true, {}};
        auto two = input.Observations[0];
        two.NodeId = 4;
        two.RequestId = "new-a-two";
        auto three = two;
        three.NodeId = 5;
        three.RequestId = "new-a-three";
        input.Observations[0].BeforeAt = TInstant::Seconds(80);
        input.Observations[0].HealthStartedAt = TInstant::Seconds(81);
        input.Observations.push_back(two);
        input.Observations.push_back(three);
        const auto result = EvaluatePolicy(input);
        UNIT_ASSERT(result.Kind == EDecisionKind::Failover);
        UNIT_ASSERT_VALUES_EQUAL(result.EvidenceExpiry, TInstant::Seconds(116));
        UNIT_ASSERT_VALUES_EQUAL(result.EvidenceRequestIds.size(), 4);
        UNIT_ASSERT(std::find(result.EvidenceRequestIds.begin(), result.EvidenceRequestIds.end(), "request-1") == result.EvidenceRequestIds.end());
    }

    Y_UNIT_TEST(BudgetAndImplicitIdentityNeverInventMapping) {
        TClockScope clock;
        TSchedulerConfig config;
        config.PerPileHealthConcurrency = 1;
        auto input = Policy();
        auto alias = input.Nodes[0];
        alias.NodeId = 4;
        alias.Endpoint = "grpc://another:2135";
        input.Nodes.push_back(alias);
        TBoundedScheduler scheduler(config);
        UNIT_ASSERT_VALUES_EQUAL(scheduler.Start(input.Nodes)->Blocker, "CONFIG_BUDGET");
        TNode unlabeled;
        UNIT_ASSERT_VALUES_EQUAL(VerifyBinding(unlabeled, 8, TString(""), {"", "a"}, false), "IMPLICIT_PILE_UNRESOLVED");
        UNIT_ASSERT(!unlabeled.Pile);
        UNIT_ASSERT(VerifyBinding(unlabeled, 8, TString(""), {"", "a"}, true).empty());
        UNIT_ASSERT(unlabeled.Pile && unlabeled.Pile->empty());
        UNIT_ASSERT(unlabeled.Binding == EBinding::ImplicitVerified);
        UNIT_ASSERT_VALUES_EQUAL(VerifyBinding(unlabeled, 9, TString(""), {"", "a"}, true), "NODE_ID_CHANGED_REDISCOVER");
    }

    Y_UNIT_TEST(PhysicalCoverageAndOrangeAggregatePreserveRedChildEvidence) {
        auto view = View();
        TDiagnosticInput input;
        input.RpcSuccess = true;
        input.SupportedOverallStatus = true; // ORANGE overall is supported.
        input.ExpectedGroups = {{"g-a", "a"}, {"g-b", "b"}, {"g-c", "c"}};
        input.Groups = {{"g-a", TString("a"), true, false, false}, {"g-b", TString("b"), true, false, false},
            {"g-c", TString("c"), true, false, false}};
        input.Issues = {{"parent", "BRIDGE_GROUP", false, std::nullopt, {"child"}},
            {"child", "BRIDGE_GROUP", true, TString("b"), {}}};
        auto parsed = ParseDiagnostic(view, Qualified(), input);
        UNIT_ASSERT(parsed.at("b").Value == EAssessment::Failed);
        UNIT_ASSERT(parsed.at("a").Value == EAssessment::NoFailureReported);
        input.Groups.pop_back();
        parsed = ParseDiagnostic(view, Qualified(), input);
        UNIT_ASSERT(parsed.at("c").Value == EAssessment::Unknown);
        input.Truncated = true;
        parsed = ParseDiagnostic(view, Qualified(), input);
        UNIT_ASSERT(parsed.at("b").Value == EAssessment::Unknown);
        input.Truncated = false;
        input.Issues = {{"unknown-attribution", "STORAGE_GROUP", true, std::nullopt, {}}};
        parsed = ParseDiagnostic(view, Qualified(), input);
        UNIT_ASSERT(parsed.at("b").Value == EAssessment::Unknown);
    }

    Y_UNIT_TEST(PreflightPinsExactNodeAndConsumesOriginalEvidenceLife) {
        TClockScope clock;
        auto input = Policy();
        FailAll(input, "b");
        auto decision = EvaluatePolicy(input);
        decision.Confirmed = true;
        TDispatchReadiness readiness;
        readiness.Auto = true;
        readiness.AuthorityReady = true;
        readiness.OwnershipAcknowledged = true;
        readiness.DurableStorage = true;
        readiness.BackgroundGuardsPermit = true;
        readiness.Epoch = input.Epoch;
        auto preflight = input.View;
        preflight.Endpoint = decision.Endpoint->Endpoint;
        UNIT_ASSERT(ValidatePreflight(decision, preflight, *decision.Endpoint, readiness, TInstant::Seconds(100)).empty());
        auto replaced = *decision.Endpoint;
        ++replaced.Revision;
        UNIT_ASSERT_VALUES_EQUAL(ValidatePreflight(decision, preflight, replaced, readiness, TInstant::Seconds(100)), "PREFLIGHT_ENDPOINT_REPLACED");
        UNIT_ASSERT_VALUES_EQUAL(ValidatePreflight(decision, preflight, *decision.Endpoint, readiness, TInstant::Seconds(115)), "HANDOFF_EVIDENCE_EXPIRED");
        preflight.Pending.push_back({"ADD", TString("c"), "", ""});
        UNIT_ASSERT_VALUES_EQUAL(ValidatePreflight(decision, preflight, *decision.Endpoint, readiness, TInstant::Seconds(100)), "PREFLIGHT_FINGERPRINT_MISMATCH");
    }

    Y_UNIT_TEST(MainConfigUsesExactNamesAndNeverInterconnectPortForSdk) {
        const auto topology = ParseMainConfig(R"(
metadata: {version: 123}
config:
  arbitrary_future_ydb_setting: {anything: true}
  bridge_config:
    piles: [{name: ""}, {name: a}, {name: no_hosts}]
  hosts:
    - host: node-without-label
      port: 19001
    - host: node-with-empty-label
      location: {bridge_pile_name: ""}
    - host: "grpc://direct-node:2135"
      location: {bridge_pile_name: a}
)", "grpc");
        UNIT_ASSERT_VALUES_EQUAL(topology.ConfiguredPiles.size(), 3);
        UNIT_ASSERT(!topology.Nodes[0].Pile);
        UNIT_ASSERT(topology.Nodes[0].Endpoint.empty());
        UNIT_ASSERT(topology.Nodes[1].Pile && topology.Nodes[1].Pile->empty());
        UNIT_ASSERT(topology.Nodes[1].Binding == EBinding::Explicit);
        UNIT_ASSERT_VALUES_EQUAL(topology.Nodes[2].Endpoint, "grpc://direct-node:2135");
        UNIT_ASSERT_VALUES_EQUAL(topology.Nodes[2].NodeId, 0);
        UNIT_ASSERT_EXCEPTION(ParseMainConfig("config: {bridge_config: {piles: [{name: a}]}, hosts: [{host: x, location: {bridge_pile_name: null}}]}", "grpc"), yexception);
    }

    Y_UNIT_TEST(TopologyRequiresMajorityAndSeparatesStalenessFromEmergency) {
        TClockScope clock;
        TTopology topology;
        topology.ConfiguredPiles = {"a", "b", "c"};
        TTopologyTracker tracker;
        tracker.ProvisionTentative(topology);
        UNIT_ASSERT(!tracker.Confirmed());
        const std::vector<TFrozenSample> samples = {{"a", {1, 2}, 2}};
        const auto now = clock.Clock->Value;
        UNIT_ASSERT_VALUES_EQUAL(tracker.Corroborate(samples, {{1, topology, now}}, View(), false), "NO_CORROBORATED_CONFIG");
        UNIT_ASSERT(tracker.Corroborate(samples, {{1, topology, now}, {2, topology, now}}, View(), false).empty());
        UNIT_ASSERT_VALUES_EQUAL(tracker.Revision(), 1);
        UNIT_ASSERT(tracker.CanDispatchLifecycle());
        std::reverse(topology.ConfiguredPiles.begin(), topology.ConfiguredPiles.end());
        UNIT_ASSERT(tracker.Corroborate(samples, {{1, topology, now}, {2, topology, now}}, View(), false).empty());
        UNIT_ASSERT_VALUES_EQUAL(tracker.Revision(), 1);
        clock.Clock->Value += TDuration::Seconds(61);
        UNIT_ASSERT(tracker.RefreshDue());
        UNIT_ASSERT(tracker.Stale());
        UNIT_ASSERT(!tracker.CanDispatchLifecycle());
        UNIT_ASSERT(tracker.CanDispatchEmergency());
        auto changed = topology;
        changed.ConfiguredPiles.push_back("new-name");
        const auto later = clock.Clock->Value;
        UNIT_ASSERT_VALUES_EQUAL(tracker.Corroborate(samples, {{1, changed, later}, {2, changed, later}}, View(), false), "MEMBERSHIP_RECONCILIATION_REQUIRED");
        UNIT_ASSERT(!tracker.CanDispatchEmergency());
    }

    Y_UNIT_TEST(TopologyRetainsVerifiedEndpointWithoutAdvancingConfigRevision) {
        TClockScope clock;
        auto topology = ParseMainConfig("config: {bridge_config: {piles: [{name: a}]}, hosts: [{host: x, location: {bridge_pile_name: a}}]}", "grpc");
        TTopologyTracker tracker;
        tracker.ProvisionTentative(topology);
        std::vector<TFrozenSample> samples = {{"a", {1}, 1}};
        UNIT_ASSERT(tracker.Corroborate(samples, {{1, topology, clock.Clock->Value}}, std::nullopt, true).empty());
        auto verified = topology.Nodes.front();
        verified.NodeId = 1;
        verified.Endpoint = "grpc://x:2135";
        verified.DirectEndpoint = true;
        tracker.RememberVerified(verified);
        UNIT_ASSERT(tracker.Corroborate(samples, {{1, topology, clock.Clock->Value}}, std::nullopt, true).empty());
        UNIT_ASSERT_VALUES_EQUAL(tracker.Revision(), 1);
        UNIT_ASSERT_VALUES_EQUAL(tracker.Topology().Nodes.front().Endpoint, "grpc://x:2135");
    }

    Y_UNIT_TEST(TopologyDropsVerifiedEndpointWhenConfiguredAddressChanges) {
        for (bool uri : {false, true}) {
            TClockScope clock;
            const TString before = uri ? "grpc://old:2135" : "old";
            const TString after = uri ? "grpc://new:2135" : "new";
            auto parse = [](const TString& host) {
                return ParseMainConfig("config: {bridge_config: {piles: [{name: a}]}, hosts: [{host: '" + host + "', node_id: 1, location: {bridge_pile_name: a}}]}", "grpc");
            };
            const auto original = parse(before);
            TTopologyTracker tracker;
            tracker.ProvisionTentative(original);
            std::vector<TFrozenSample> samples = {{"a", {1}, 1}};
            UNIT_ASSERT(tracker.Corroborate(samples, {{1, original, clock.Clock->Value}}, std::nullopt, true).empty());
            auto verified = original.Nodes.front();
            verified.NodeId = 1;
            verified.Endpoint = "grpc://old:2135";
            verified.DirectEndpoint = true;
            tracker.RememberVerified(verified);
            UNIT_ASSERT(tracker.Corroborate(samples, {{1, original, clock.Clock->Value}}, std::nullopt, true).empty());
            UNIT_ASSERT_VALUES_EQUAL(tracker.Topology().Nodes.front().NodeId, 1);
            const auto changed = parse(after);
            UNIT_ASSERT(tracker.Corroborate(samples, {{1, changed, clock.Clock->Value}}, std::nullopt, true).empty());
            const auto& node = tracker.Topology().Nodes.front();
            UNIT_ASSERT_VALUES_EQUAL(node.Host, "new");
            UNIT_ASSERT_VALUES_EQUAL(node.Endpoint, uri ? "grpc://new:2135" : "");
            UNIT_ASSERT_VALUES_EQUAL(node.NodeId, 0);
            const auto dropped = tracker.TakeReaddressed();
            UNIT_ASSERT_VALUES_EQUAL(dropped.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(dropped.front().Endpoint, "grpc://old:2135");
            UNIT_ASSERT(tracker.TakeReaddressed().empty());
            UNIT_ASSERT(tracker.Corroborate(samples, {{1, original, clock.Clock->Value}}, std::nullopt, true).empty());
            UNIT_ASSERT_VALUES_EQUAL(tracker.Topology().Nodes.front().NodeId, 0);
        }
    }

    Y_UNIT_TEST(EndpointBindsAtMostOneVerifiedNode) {
        TClockScope clock;
        const auto topology = ParseMainConfig(R"(config:
  bridge_config: {piles: [{name: a}]}
  hosts:
    - {host: x, node_id: 1, location: {bridge_pile_name: a}}
    - {host: y, node_id: 2, location: {bridge_pile_name: a}}
)", "grpc");
        TTopologyTracker tracker;
        tracker.ProvisionTentative(topology);
        const TString endpoint = "grpc://shared:2135";
        for (ui32 id : {1, 2}) {
            auto verified = topology.Nodes[id - 1];
            verified.NodeId = id;
            verified.Endpoint = endpoint;
            verified.DirectEndpoint = true;
            tracker.RememberVerified(verified);
        }
        auto check = [&] {
            size_t bound = 0;
            for (const auto& node : tracker.Topology().Nodes) {
                if (node.NodeId) {
                    ++bound;
                    UNIT_ASSERT_VALUES_EQUAL(node.NodeId, 2);
                    UNIT_ASSERT_VALUES_EQUAL(node.Endpoint, endpoint);
                }
            }
            UNIT_ASSERT_VALUES_EQUAL(bound, 1);
        };
        check();
        std::vector<TFrozenSample> samples = {{"a", {2}, 1}};
        UNIT_ASSERT(tracker.Corroborate(samples, {{2, topology, clock.Clock->Value}}, std::nullopt, true).empty());
        check();
    }

    Y_UNIT_TEST(MultipleInventoryNodesOnOneHostHaveDistinctUnverifiedIdentities) {
        const auto topology = ParseMainConfig(R"(config:
  bridge_config: {piles: [{name: a}]}
  hosts:
    - {host: shared-host, node_id: 1, port: 19001, location: {bridge_pile_name: a}}
    - {host: shared-host, node_id: 2, port: 19002, location: {bridge_pile_name: a}}
)", "grpc");
        UNIT_ASSERT_VALUES_EQUAL(topology.Nodes.size(), 2);
        UNIT_ASSERT(topology.Nodes[0].LogicalId != topology.Nodes[1].LogicalId);
        UNIT_ASSERT_VALUES_EQUAL(topology.Nodes[0].NodeId, 0);
        UNIT_ASSERT_VALUES_EQUAL(topology.Nodes[0].InventoryNodeId, 1);
        UNIT_ASSERT(topology.Nodes[0].Endpoint.empty());
        auto wrong = topology.Nodes[0];
        UNIT_ASSERT_VALUES_EQUAL(VerifyBinding(wrong, 2, TString("a"), {"a"}, false), "INVENTORY_NODE_ID_MISMATCH");
    }

    Y_UNIT_TEST(RepeatedConfigCorroborationCannotRefreshCachedReplyAge) {
        TClockScope clock;
        TTopology topology;
        topology.ConfiguredPiles = {"a", "b", "c"};
        TTopologyTracker tracker;
        tracker.ProvisionTentative(topology);
        const std::vector<TFrozenSample> samples = {{"a", {1, 2}, 2}};
        const std::vector<TConfigReply> replies = {{1, topology, clock.Clock->Value}, {2, topology, clock.Clock->Value}};
        UNIT_ASSERT(tracker.Corroborate(samples, replies, View(), false).empty());
        clock.Clock->Value += TDuration::Seconds(59);
        UNIT_ASSERT(tracker.Corroborate(samples, replies, View(), false).empty());
        UNIT_ASSERT(!tracker.Stale());
        UNIT_ASSERT_VALUES_EQUAL(tracker.ValidUntil(), TInstant::Seconds(160));
        clock.Clock->Value += TDuration::Seconds(1);
        UNIT_ASSERT(tracker.Stale());
        UNIT_ASSERT(!tracker.CanDispatchLifecycle());
    }
}

} // namespace NBridgeSkipper
