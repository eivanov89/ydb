#include "controller.h"
#include "resources.h"

#include <ydb/public/api/protos/ydb_monitoring.pb.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>

#include <ydb/apps/bridge_skipper/domain/clock.h>
#include <library/cpp/monlib/dynamic_counters/counters.h>

#include <util/generic/guid.h>
#include <util/generic/yexception.h>
#include <util/string/builder.h>

#include <algorithm>

namespace NBridgeSkipper {
namespace {

TSchedulerConfig SchedulerConfig(const TPollingConfig& p) {
    TSchedulerConfig c;
    c.SampleNodesPerPile = p.SampleNodesPerPile;
    c.PerPileHealthConcurrency = p.PerPileHealthConcurrency;
    c.GlobalAdminConcurrency = p.GlobalAdminConcurrency;
    c.RoundInterval = p.RoundInterval;
    c.RoundDeadline = p.RoundDeadline;
    c.BracketAdminTimeout = p.BracketAdminTimeout;
    c.HealthOperationTimeout = p.HealthOperationTimeout;
    c.HealthClientTimeout = p.HealthClientTimeout;
    c.ObservationTtl = p.ObservationTtl;
    c.AdmissionBudget = p.AdmissionBudget;
    c.DispatchMargin = p.DispatchMargin;
    c.SchedulerTolerance = p.SchedulerTolerance;
    c.ConfirmationStartGap = p.ConfirmationStartGap;
    return c;
}

TString Short(const TString& value) {
    return value.substr(0, 512);
}

void RpcTiming(TDiagnosticSummary& summary, const TReadResult& result) {
    summary.StartedAt = result.StartedAt;
    summary.Success = result.Success;
    if (result.StartedAt && result.FinishedAt >= result.StartedAt) {
        summary.Latency = result.FinishedAt - result.StartedAt;
    }
}

TString SuppressionKey(TDecision decision) {
    // Readiness restoration is not a change in the rejected YDB intent or its
    // qualified verdicts. Other canonical decision bindings remain intact.
    decision.Epoch.ReadinessGeneration = 0;
    return DomainHash(CanonicalDecision(decision));
}

} // namespace

struct TControllerCore::TImpl {
    enum class EReadPurpose { Poll, BootstrapIdentity, Config, Discovery, Before, Health, After, Preflight };

    struct TReadContext {
        TReadRequest Request;
        EReadPurpose Purpose = EReadPurpose::Poll;
        ui32 NodeId = 0;
        TString OperationId;
        ui64 WorkflowRevision = 0;
        TNode Node;
        std::set<TString> IssuedAfterHandoff;
    };

    struct TAdminObservation {
        TClusterView View;
        ui64 Serial = 0;
        TInstant StartedAt;
        std::set<TString> IssuedAfterHandoff;
    };

    struct TChain {
        ui64 RoundId = 0;
        TNode Node;
        TEpoch Epoch;
        TInstant Deadline;
        TObservation Observation;
    };

    struct TPending {
        TDurableCommand Command;
        TApplyResult Candidate;
        TString StorageId;
        bool External = false;
        bool Unknown = false;
        bool LookupInFlight = false;
        std::optional<TDecision> Binding;
        ui64 ConfirmationGeneration = 0;
        TInstant BindingRoundClose;
    };

    TControllerOptions Options;
    std::shared_ptr<const TDurableState> Applied;
    TBoundedScheduler Scheduler;
    TTopologyTracker Topology;
    TEpoch Epoch;
    TConfirmation Confirmation;
    ui64 ConfirmationGeneration = 1;
    std::optional<TClusterView> Cluster;
    std::optional<TClusterView> UnsupportedCluster;
    struct TUnsupportedObservation {
        TClusterView View;
        ui32 NodeId = 0;
        bool Replaced = false;
        ui64 Serial = 0;
    };
    std::map<TString, TUnsupportedObservation> UnsupportedObservations;
    std::vector<TFrozenSample> Samples;
    ui64 AdminObservationSerial = 0;
    // Replies are keyed by the endpoint that answered and are attributed to a
    // node only through the topology tracker's current binding.
    std::map<TString, TAdminObservation> AdminByEndpoint;
    std::map<TString, TConfigReply> ConfigByEndpoint;
    std::map<TString, TReadContext> Reads;
    std::map<ui32, TChain> Chains;
    std::map<TString, TDiscoveredEndpoint> Discovered;
    std::vector<TObservation> Observations;
    std::map<TString, TDiagnosticSummary> Diagnostics;
    std::optional<TRoundReservation> Round;
    std::optional<TDecision> Decision;
    std::vector<TPileReport> Reports;
    std::optional<TPending> Pending;
    std::deque<TControllerSubmission> Submissions;
    std::deque<TMutationResult> Outcomes;
    std::map<TString, TReadResult> Preflights;
    std::map<TString, ui32> LastPreflightNode;
    std::map<TString, std::set<ui32>> PreflightTried;
    std::map<TString, TDecision> AutomaticBindings;
    std::map<TString, ui64> BoundConfirmationGeneration;
    std::map<TString, TInstant> BoundRoundClose;
    TInstant LastRoundClosedAt;
    std::set<TString> Granted;
    std::set<TString> Closing;
    std::set<TString> OutcomeReservations;
    std::map<TString, TInstant> HandoffTimes;
    std::map<TString, TInstant> HealthyExpiries;
    TString LastReconcileKey;
    TString CheckpointId;
    ui64 CheckpointIndex = 0;
    ui64 CheckpointTargetIndex = 0;
    TInstant LastCheckpoint;
    TControllerEffects Effects;
    std::shared_ptr<const TControllerSnapshot> Published;
    TInstant NextAdmin;
    TInstant NextConfig;
    TInstant NextBootstrap;
    bool Started = false;
    bool Stopping = false;
    bool StorageReady = true;
    bool Stopped = false;
    bool LastReady = false;
    bool ReadinessInitialized = false;
    ui64 StopRevision = 0;
    ui64 SnapshotRevision = 0;
    size_t Skipped = 0;
    size_t ConfigCursor = 0;
    size_t BootstrapCursor = 0;
    TString AdminBlocker = "ADMIN_VIEW_UNCONFIRMED";
    TString ConfigBlocker = "TOPOLOGY_UNCONFIRMED";
    TString StorageBlocker;
    TString SamplingBlocker;
    ui64 ClosedRounds = 0;
    ui64 DeadlineRounds = 0;
    ui64 AdmissionBudgetOverruns = 0;
    ui64 FreshnessMisses = 0;
    size_t LastRoundSampledNodes = 0;
    size_t LastRoundFreshNodes = 0;
    std::map<TString, ui64> EvidenceRejects;
    std::optional<TDuration> LastHandoffSlack;
    std::optional<TDuration> LastRoundCloseToHandoff;

    explicit TImpl(TControllerOptions options)
        : Options(std::move(options))
        , Applied(std::make_shared<const TDurableState>(Options.Restored))
        , Scheduler(SchedulerConfig(Options.Config.Polling))
    {
        Y_ENSURE(Options.NextId && !Options.ProcessId.empty(), "controller requires injected IDs/process identity");
        const auto& p = Options.Config.Polling;
        Epoch.PolicyRevision = DomainHash(TStringBuilder() << EvidenceProfileName(Options.Config.EvidenceProfile)
            << ";policy-v1;independent-placement-v1;" << p.SampleNodesPerPile << ';' << p.PerPileHealthConcurrency << ';'
            << p.RoundInterval.GetValue() << ';' << p.RoundDeadline.GetValue() << ';' << p.BracketAdminTimeout.GetValue() << ';'
            << p.HealthOperationTimeout.GetValue() << ';' << p.HealthClientTimeout.GetValue() << ';' << p.ObservationTtl.GetValue() << ';'
            << p.AdmissionBudget.GetValue() << ';' << p.DispatchMargin.GetValue() << ';' << p.SchedulerTolerance.GetValue() << ';'
            << p.ConfirmationStartGap.GetValue() << ';' << Options.EvidenceQualification.SupportedBuild << ';'
            << Options.EvidenceQualification.FixtureRecord << ';' << Options.EvidenceQualification.CoversResynchronizing);
        Epoch.ReadinessGeneration = 1;
        CheckpointIndex = Applied->AppliedIndex;
        LastCheckpoint = TSkipperClock::Now();
        Publish();
    }

    TString Id() {
        return Options.NextId();
    }

    bool Fresh() const {
        const auto now = TSkipperClock::Now();
        return Cluster && Cluster->ReceivedAt <= now && now - Cluster->ReceivedAt < Options.Config.Polling.ObservationTtl
            && AdminBlocker.empty() && Cluster->Generation >= Applied->AcceptedGenerationHighWater;
    }

    bool Ready() const {
        return StorageReady && !Stopping && !Stopped && !Applied->StateConflict && Topology.CanDispatchEmergency() && Fresh();
    }

    bool ReadyForOperation(const TOperation& operation) const {
        return Ready() && (operation.Plan.Kind == EOperationKind::Failover || Topology.CanDispatchLifecycle());
    }

    TProfileQualification Qualification() const {
        if (Options.Config.EvidenceProfile == EEvidenceProfile::NodewardenLocalV1) {
            TProfileQualification unavailable;
            unavailable.Profile = "nodewarden_local_v1";
            unavailable.Source = ESourceKind::LocalNodewarden;
            return unavailable;
        }
        if (Options.EvidenceQualification.Profile == TString(EvidenceProfileName(Options.Config.EvidenceProfile))) {
            return Options.EvidenceQualification;
        }
        TProfileQualification q;
        q.Profile = TString(EvidenceProfileName(Options.Config.EvidenceProfile));
        q.Source = Options.Config.EvidenceProfile == EEvidenceProfile::GlobalBscV1 ? ESourceKind::GlobalBsc
            : (Options.Config.EvidenceProfile == EEvidenceProfile::StaticWhiteboardV1 ? ESourceKind::LocalWhiteboard : ESourceKind::LocalNodewarden);
        // No I-01 recording or I-02 capability is present in this source build.
        // Recognition supports diagnostics, never certification by config alone.
        return q;
    }

    bool BindingCurrent(const TString& operationId) const {
        const auto found = BoundConfirmationGeneration.find(operationId);
        return found != BoundConfirmationGeneration.end() && found->second == ConfirmationGeneration;
    }

    void Invalidate() {
        ++ConfirmationGeneration;
        Confirmation.Reset();
        Decision.reset();
        Reports.clear();
        HealthyExpiries.clear();
    }

    void Publish() {
        const bool ready = Ready() && Applied->Ownership && Applied->RequestedMode != ERequestedMode::Observe;
        if (ReadinessInitialized && ready != LastReady) {
            ++Epoch.ReadinessGeneration;
            Invalidate();
        }
        LastReady = ready;
        ReadinessInitialized = true;
        auto snapshot = std::make_shared<TControllerSnapshot>();
        snapshot->ClusterId = Options.Config.ClusterId;
        snapshot->IncarnationId = Options.ProcessId;
        snapshot->SnapshotRevision = ++SnapshotRevision;
        snapshot->TopologyRevision = Topology.Revision();
        snapshot->ControllerRevision = Applied->ControllerRevision;
        snapshot->PolicyRevision = Epoch.PolicyRevision;
        snapshot->GeneratedAt = TSkipperClock::Now();
        snapshot->Applied = Applied;
        snapshot->Cluster = Cluster;
        snapshot->UnsupportedCluster = UnsupportedCluster;
        snapshot->Topology = Topology.Topology();
        snapshot->Epoch = Epoch;
        snapshot->Decision = Decision;
        snapshot->ConfirmationGeneration = ConfirmationGeneration;
        snapshot->Reports = Reports;
        for (const auto& [endpoint, diagnostic] : Diagnostics) {
            (void)endpoint;
            snapshot->Diagnostics.push_back(diagnostic);
        }
        const auto add = [&](const TString& blocker) {
            if (!blocker.empty()) {
                snapshot->Blockers.push_back(blocker);
            }
        };
        add(AdminBlocker);
        add(ConfigBlocker);
        add(StorageBlocker);
        add(SamplingBlocker);
        if (Stopped) {
            add("LOCAL_STOP");
        }
        if (Applied->StateConflict) {
            add("STATE_CONFLICT");
        }
        if (Applied->UnresolvedAttempt) {
            add("UNRESOLVED_ATTEMPT");
        }
        if (!Applied->Ownership) {
            add("OWNERSHIP_ACKNOWLEDGEMENT_REQUIRED");
        }
        if (!Fresh()) {
            add("ADMIN_EVIDENCE_STALE");
        }
        if (!IsQualified(Qualification())) {
            add("UNQUALIFIED_EVIDENCE_PROFILE");
        }
        if (Options.Config.EvidenceProfile == EEvidenceProfile::NodewardenLocalV1) {
            add("UNSUPPORTED_DIAGNOSTIC_CAPABILITY");
        }
        snapshot->AuthorityReady = StorageReady && !Stopping;
        snapshot->DispatchReady = ready;
        snapshot->AutomaticDispatchReady = ready && IsQualified(Qualification()) && SamplingBlocker.empty()
            && Applied->RequestedMode == ERequestedMode::Auto;
        snapshot->AdminValidUntil = Cluster ? Cluster->ReceivedAt + Options.Config.Polling.ObservationTtl : TInstant::Zero();
        snapshot->ConfigValidUntil = Topology.ValidUntil();
        snapshot->AutonomyReady = snapshot->AutomaticDispatchReady && !Applied->UnresolvedAttempt;
        snapshot->MutationReady = ready && !Applied->UnresolvedAttempt;
        snapshot->ConfigStale = Topology.Stale();
        snapshot->TopologyConfirmed = Topology.Confirmed();
        snapshot->HandoverComplete = NBridgeSkipper::HandoverComplete(*Applied);
        snapshot->LocalStopped = Stopped;
        snapshot->LocalStopRevision = StopRevision;
        snapshot->InFlightReads = Reads.size();
        snapshot->InFlightHealth = Scheduler.InFlightChains();
        snapshot->SkippedReads = Skipped;
        snapshot->PendingCommands = Submissions.size() + bool(Pending) + !CheckpointId.empty();
        snapshot->AdmissionUnknown = Pending && Pending->Unknown;
        snapshot->ClosedRounds = ClosedRounds;
        snapshot->DeadlineRounds = DeadlineRounds;
        snapshot->AdmissionBudgetOverruns = AdmissionBudgetOverruns;
        snapshot->FreshnessMisses = FreshnessMisses;
        snapshot->LastRoundSampledNodes = LastRoundSampledNodes;
        snapshot->LastRoundFreshNodes = LastRoundFreshNodes;
        snapshot->EvidenceRejects = EvidenceRejects;
        snapshot->LastHandoffSlack = LastHandoffSlack;
        snapshot->LastRoundCloseToHandoff = LastRoundCloseToHandoff;
        snapshot->EffectiveState = Stopped ? "stopped" : (Applied->RequestedMode == ERequestedMode::Observe ? "observing"
            : (Applied->UnresolvedAttempt ? "reconciling" : (ready ? (Applied->RequestedMode == ERequestedMode::Auto
                ? (snapshot->AutonomyReady ? "ready_auto" : "blocked") : "ready_manual") : "blocked")));
        Published = std::move(snapshot);
    }

    bool Read(TReadContext context) {
        const auto method = context.Request.Method;
        size_t sameKind = 0;
        for (const auto& [id, live] : Reads) {
            (void)id;
            sameKind += live.Request.Method == method;
            if (live.Request.Endpoint == context.Request.Endpoint && live.Request.Method == method) {
                ++Skipped;
                return false;
            }
        }
        const size_t limit = method == EReadMethod::Config || method == EReadMethod::Discovery ? 4 : 1024;
        if (sameKind >= limit || Reads.size() >= 4096) {
            ++Skipped;
            return false;
        }
        context.Request.Id = Id();
        context.Request.ProcessId = Options.ProcessId;
        context.Request.Epoch = Epoch;
        if (method == EReadMethod::Admin) {
            for (const auto& [attemptId, handoff] : HandoffTimes) {
                (void)handoff;
                context.IssuedAfterHandoff.insert(attemptId);
            }
        }
        Effects.Reads.push_back(context.Request);
        const auto id = context.Request.Id;
        Reads.emplace(id, std::move(context));
        return true;
    }

    TReadContext Request(EReadMethod method, EReadPurpose purpose, const TString& endpoint, TDuration timeout) {
        TReadContext context;
        context.Purpose = purpose;
        context.Request.Method = method;
        context.Request.Endpoint = endpoint;
        context.Request.Timeout = timeout;
        context.Request.OperationTimeout = Options.Config.Polling.HealthOperationTimeout;
        context.Request.Deadline = TSkipperClock::Now() + timeout;
        return context;
    }

    void MakeSamples() {
        if (!Samples.empty() && Round) {
            return;
        }
        std::map<TString, std::vector<ui32>> piles;
        for (const auto& node : VerifiedInventory(Topology.Topology().Nodes)) {
            piles[*node.Pile].push_back(node.NodeId);
        }
        Samples.clear();
        for (auto& [pile, nodes] : piles) {
            nodes.resize(std::min<size_t>(nodes.size(), Options.Config.Polling.SampleNodesPerPile));
            Samples.push_back({pile, nodes, nodes.size() / 2 + 1});
        }
    }

    std::map<ui32, TAdminObservation> AdminByNode() const {
        std::map<ui32, TAdminObservation> result;
        for (const auto& node : Topology.Topology().Nodes) {
            if (!node.NodeId || !node.Pile) {
                continue;
            }
            if (const auto found = AdminByEndpoint.find(node.Endpoint); found != AdminByEndpoint.end()) {
                result.emplace(node.NodeId, found->second);
            }
        }
        return result;
    }

    std::vector<TConfigReply> ConfigReplies() const {
        std::map<ui32, TConfigReply> byNode;
        for (const auto& node : Topology.Topology().Nodes) {
            if (!node.NodeId || !node.Pile) {
                continue;
            }
            if (const auto found = ConfigByEndpoint.find(node.Endpoint); found != ConfigByEndpoint.end()) {
                byNode.emplace(node.NodeId, TConfigReply{node.NodeId, found->second.Topology, found->second.ReceivedAt});
            }
        }
        std::vector<TConfigReply> result;
        for (auto& [id, reply] : byNode) {
            (void)id;
            result.push_back(std::move(reply));
        }
        return result;
    }

    std::optional<TAdminSelection> ReconcileMembership(const std::vector<TAdminReply>& adminReplies) {
        const auto configReplies = ConfigReplies();
        std::set<std::set<TString>> memberships;
        const auto now = TSkipperClock::Now();
        for (const auto& reply : configReplies) {
            if (reply.ReceivedAt <= now && now - reply.ReceivedAt < TDuration::Seconds(60)) {
                memberships.emplace(reply.Topology.ConfiguredPiles.begin(), reply.Topology.ConfiguredPiles.end());
            }
        }
        for (const auto& membership : memberships) {
            auto selected = SelectAdminView(Samples, adminReplies, membership, Applied->AcceptedGenerationHighWater,
                now, Options.Config.Polling.ObservationTtl);
            if (!selected.View || !selected.Blocker.empty()) {
                continue;
            }
            // A config name-set is only a candidate here. Neither view may be
            // installed until the original frozen sample corroborates both the
            // complete normalized config and this exact named admin view.
            const auto revision = Topology.Revision();
            ConfigBlocker = Topology.Corroborate(Samples, configReplies, selected.View, !selected.View->Converted);
            ForgetReaddressed();
            if (!ConfigBlocker.empty()) {
                continue;
            }
            if (Topology.Revision() != revision) {
                Epoch.TopologyRevision = Topology.Revision();
                Invalidate();
            }
            return selected;
        }
        return std::nullopt;
    }

    void SelectAdmin() {
        MakeSamples();
        if (Samples.empty()) {
            return;
        }
        const auto byNode = AdminByNode();
        std::vector<TAdminReply> replies;
        for (const auto& [id, observation] : byNode) {
            replies.push_back({id, observation.View});
        }
        std::set<TString> names(Topology.Topology().ConfiguredPiles.begin(), Topology.Topology().ConfiguredPiles.end());
        auto selected = SelectAdminView(Samples, replies, names, Applied->AcceptedGenerationHighWater,
            TSkipperClock::Now(), Options.Config.Polling.ObservationTtl);
        if (selected.Blocker == "MEMBERSHIP_RECONCILIATION_REQUIRED") {
            if (auto reconciled = ReconcileMembership(replies)) {
                selected = std::move(*reconciled);
            }
        }
        AdminBlocker = selected.Blocker;
        if (selected.View && selected.Blocker.empty()) {
            for (const auto& [endpoint, observation] : UnsupportedObservations) {
                const auto node = KnownNode(endpoint);
                if (!observation.Replaced && node && node->NodeId == observation.NodeId
                    && observation.View.ReceivedAt <= TSkipperClock::Now()
                    && TSkipperClock::Now() - observation.View.ReceivedAt < Options.Config.Polling.ObservationTtl)
                {
                    AdminBlocker = "UNSUPPORTED_ADMIN_VIEW";
                    Invalidate();
                    return;
                }
            }
            if (!UnsupportedObservations.empty()) {
                ui64 after = 0;
                for (const auto& [endpoint, observation] : UnsupportedObservations) {
                    (void)endpoint;
                    after = std::max(after, observation.Serial);
                }
                std::vector<TAdminReply> newer;
                for (const auto& reply : replies) {
                    if (byNode.at(reply.NodeId).Serial > after) {
                        newer.push_back(reply);
                    }
                }
                std::set<TString> membership;
                for (const auto& [name, state] : selected.View->Piles) {
                    (void)state;
                    membership.insert(name);
                }
                auto resolved = SelectAdminView(Samples, newer, membership, Applied->AcceptedGenerationHighWater,
                    TSkipperClock::Now(), Options.Config.Polling.ObservationTtl);
                if (!resolved.View || !resolved.Blocker.empty()
                    || CanonicalFingerprint(*resolved.View) != CanonicalFingerprint(*selected.View))
                {
                    AdminBlocker = "UNSUPPORTED_ADMIN_VIEW";
                    Invalidate();
                    return;
                }
                selected = std::move(resolved);
                // Only a new supported quorum resolves replaced or expired
                // observations; an unrelated supported singleton cannot.
                UnsupportedObservations.clear();
            }
            UnsupportedCluster.reset();
            const auto fingerprint = CanonicalFingerprint(*selected.View);
            if (!Cluster || fingerprint != Epoch.AdminFingerprint) {
                Invalidate();
                Epoch.AdminFingerprint = fingerprint;
            }
            Cluster = std::move(selected.View);
            TryConfig();
        } else if (!selected.Blocker.empty()) {
            Invalidate();
        }
    }

    void TryConfig() {
        const auto replies = ConfigReplies();
        if (replies.empty() || Samples.empty()) {
            return;
        }
        const auto revision = Topology.Revision();
        ConfigBlocker = Topology.Corroborate(Samples, replies, Cluster, Cluster && !Cluster->Converted);
        ForgetReaddressed();
        if (Topology.Revision() != revision) {
            Epoch.TopologyRevision = Topology.Revision();
            Invalidate();
        }
    }

    void FinishChain(ui32 nodeId) {
        const auto found = Chains.find(nodeId);
        if (found != Chains.end()) {
            Scheduler.Settle(found->second.RoundId, nodeId);
            Chains.erase(found);
        }
    }

    void ContinueChain(ui32 nodeId, EReadMethod method, EReadPurpose purpose) {
        auto found = Chains.find(nodeId);
        if (found == Chains.end()) {
            return;
        }
        auto& chain = found->second;
        const auto now = TSkipperClock::Now();
        if (now >= chain.Deadline || chain.Epoch != Epoch) {
            FinishChain(nodeId);
            return;
        }
        auto timeout = method == EReadMethod::Health ? Options.Config.Polling.HealthClientTimeout : Options.Config.Polling.BracketAdminTimeout;
        timeout = std::min(timeout, chain.Deadline - now);
        if (chain.Observation.HealthStartedAt) {
            const auto expiry = chain.Observation.HealthStartedAt + Options.Config.Polling.ObservationTtl;
            if (now >= expiry) {
                FinishChain(nodeId);
                return;
            }
            timeout = std::min(timeout, expiry - now);
        }
        auto request = Request(method, purpose, chain.Node.Endpoint, timeout);
        request.Request.Bracket = true;
        request.NodeId = nodeId;
        request.Request.Deadline = chain.Deadline;
        if (method == EReadMethod::Health) {
            chain.Observation.HealthStartedAt = now;
        }
        if (!Read(std::move(request))) {
            FinishChain(nodeId);
        }
    }

    void StartRound() {
        if (!Topology.Confirmed() || !Fresh()) {
            return;
        }
        auto reservation = Scheduler.Start(Topology.Topology().Nodes);
        if (!reservation) {
            return;
        }
        Round = *reservation;
        SamplingBlocker = reservation->Blocker == "CONFIG_BUDGET" ? reservation->Blocker : TString();
        Observations.clear();
        Samples = reservation->Samples;
        if (reservation->Launch.empty()) {
            Invalidate();
        }
        for (const auto& node : reservation->Launch) {
            TChain chain;
            chain.RoundId = reservation->Id;
            chain.Node = node;
            chain.Epoch = Epoch;
            chain.Deadline = reservation->Deadline;
            chain.Observation.NodeId = node.NodeId;
            chain.Observation.ResponderPile = node.Pile;
            chain.Observation.RoundId = reservation->Id;
            chain.Observation.Epoch = Epoch;
            chain.Observation.ProcessId = Options.ProcessId;
            Chains.emplace(node.NodeId, std::move(chain));
            ContinueChain(node.NodeId, EReadMethod::Admin, EReadPurpose::Before);
        }
    }

    void CloseRound() {
        if (!Round || TSkipperClock::Now() < Round->Deadline) {
            return;
        }
        ++ClosedRounds;
        DeadlineRounds += std::any_of(Chains.begin(), Chains.end(), [&](const auto& item) {
            return item.second.RoundId == Round->Id;
        });
        LastRoundClosedAt = TSkipperClock::Now();
        TPolicyInput input;
        input.DecisionId = Id();
        input.ProcessId = Options.ProcessId;
        input.RoundId = Round->Id;
        input.RoundStartedAt = Round->StartedAt;
        input.RoundDeadline = Round->Deadline;
        input.Epoch = Epoch;
        input.View = Cluster.value_or(TClusterView{});
        input.Qualification = Qualification();
        input.Samples = Round->Samples;
        input.Observations = Observations;
        input.Nodes = Topology.Topology().Nodes;
        input.PlacementAcknowledged = Applied->Ownership && Applied->Ownership->IndependentFailureDomain;
        input.Timing = {Options.Config.Polling.ObservationTtl, Options.Config.Polling.BracketAdminTimeout,
            Options.Config.Polling.DispatchMargin, Options.Config.Polling.ConfirmationStartGap};
        Decision = EvaluatePolicy(input);
        if (Options.Config.EvidenceProfile == EEvidenceProfile::NodewardenLocalV1) {
            Decision->Kind = EDecisionKind::Blocked;
            Decision->Reason = "UNSUPPORTED_DIAGNOSTIC_CAPABILITY";
            Decision->Update.clear();
        }
        if (!SamplingBlocker.empty()) {
            Decision->Kind = EDecisionKind::Blocked;
            Decision->Reason = SamplingBlocker;
            Decision->Update.clear();
        }
        if (!Confirmation.CompleteRound(*Decision, TSkipperClock::Now(), input.Timing)) {
            // Blocked/contradictory evidence, normalized decision changes and
            // excess start gaps revoke every earlier evidence interval. A later
            // fresh confirmation cannot revive permissions from that interval.
            ++ConfirmationGeneration;
        }
        if (Decision->Kind == EDecisionKind::Blocked) {
            ++EvidenceRejects[Decision->Reason];
        }
        Reports.clear();
        std::vector<const TObservation*> accepted;
        for (const auto& observation : Observations) {
            if (ValidateObservation(observation, Epoch, Options.ProcessId, input.Qualification,
                TSkipperClock::Now(), Round->Deadline, input.Timing).empty())
            {
                accepted.push_back(&observation);
            }
        }
        for (const auto& sample : Round->Samples) {
            Reports.push_back(AggregatePile(sample, input.View, accepted, input.Qualification, TSkipperClock::Now(), input.Timing));
        }
        LastRoundSampledNodes = 0;
        LastRoundFreshNodes = 0;
        for (const auto& report : Reports) {
            LastRoundSampledNodes += report.Sample.Nodes.size();
            LastRoundFreshNodes += report.Fresh;
        }
        if (IsQualified(input.Qualification) && Decision->Reason != "CONTRADICTORY_GLOBAL_ASSESSMENT") {
            for (const auto& report : Reports) {
                const auto self = report.Targets.find(report.Sample.Pile);
                if (report.Eligible && self != report.Targets.end() && self->second.Complete
                    && self->second.Value == EAssessment::NoFailureReported)
                {
                    std::vector<TInstant> starts;
                    for (const auto* observation : accepted) {
                        const auto health = observation->Targets.find(report.Sample.Pile);
                        if (observation->ResponderPile == std::optional<TString>(report.Sample.Pile)
                            && health != observation->Targets.end() && health->second.Complete
                            && health->second.Value == EAssessment::NoFailureReported)
                        {
                            starts.push_back(observation->HealthStartedAt);
                        }
                    }
                    std::sort(starts.begin(), starts.end(), std::greater<TInstant>());
                    if (starts.size() >= report.Sample.Threshold) {
                        HealthyExpiries[report.Sample.Pile] = starts[report.Sample.Threshold - 1] + Options.Config.Polling.ObservationTtl;
                    }
                }
            }
        }
        Scheduler.CloseDue();
        Round.reset();
    }

    TDurableCommand Command(ECommandKind kind) {
        TDurableCommand command;
        command.Id = Id();
        command.Kind = kind;
        command.AuditTime = TSkipperClock::Now().ToString();
        command.ExpectedControllerRevision = Applied->ControllerRevision;
        command.Ready = Ready();
        command.LocalStopped = Stopped;
        command.Fresh = Fresh();
        command.Coherent = Fresh();
        if (Cluster) {
            command.Observed = *Cluster;
        }
        return command;
    }

    void Propose(TDurableCommand command, bool external, std::optional<TDecision> binding = {}) {
        Y_ENSURE(!Pending, "one serialized authority proposal");
        TPending pending;
        pending.Command = std::move(command);
        pending.Candidate = Apply(*Applied, pending.Command);
        pending.External = external;
        pending.Binding = std::move(binding);
        pending.ConfirmationGeneration = ConfirmationGeneration;
        pending.BindingRoundClose = LastRoundClosedAt;
        if (pending.Candidate.Duplicate || pending.Candidate.State.AppliedIndex == Applied->AppliedIndex) {
            if (external) {
                Effects.Replies.push_back({pending.Command.Id, {pending.Candidate.Result.Ok ? EProposalStatus::Committed : EProposalStatus::Rejected,
                    pending.Candidate.Result, {}, pending.Candidate.Result.Code}, true});
            }
            return;
        }
        pending.StorageId = Id();
        TStorageRequest request;
        request.Id = pending.StorageId;
        request.Kind = EStorageKind::Commit;
        request.State = Applied;
        request.Command = pending.Command;
        Effects.Stores.push_back(std::move(request));
        Pending = std::move(pending);
    }

    bool Duplicate(const TControllerSubmission& submission) {
        const auto& command = submission.Command;
        if (command.RequestId) {
            const auto found = Applied->Requests.find(*command.RequestId);
            if (found != Applied->Requests.end()) {
                const bool same = found->second.PayloadHash == command.RequestHash;
                auto result = found->second.Result;
                if (!same) {
                    result = {false, "REQUEST_ID_REUSED", {}, Applied->AppliedIndex, Applied->ControllerRevision};
                }
                Effects.Replies.push_back({command.Id, {same ? EProposalStatus::Committed : EProposalStatus::Rejected, result, {}, result.Code}, true});
                return true;
            }
        }
        return false;
    }

    void Reject(const TString& id, const TString& code) {
        TCommandResult result{false, code, {}, Applied->AppliedIndex, Applied->ControllerRevision};
        Effects.Replies.push_back({id, {EProposalStatus::Rejected, result, {}, code}, true});
    }

    void Admit(TControllerSubmission submission) {
        if (Duplicate(submission)) {
            return;
        }
        auto command = std::move(submission.Command);
        const auto kind = command.Kind;
        if (kind != ECommandKind::SetMode && kind != ECommandKind::AdmitOperation && kind != ECommandKind::Cancel
            && kind != ECommandKind::Resume && kind != ECommandKind::ResolveExternal)
        {
            Reject(command.Id, "INTERNAL_COMMAND_NOT_PUBLIC");
            return;
        }
        if (Stopping || !StorageReady) {
            Reject(command.Id, "AUTHORITY_UNAVAILABLE");
            return;
        }
        if (submission.IncarnationId != Options.ProcessId) {
            Reject(command.Id, "STALE_PROCESS_INCARCATION");
            return;
        }
        if (kind == ECommandKind::AdmitOperation || kind == ECommandKind::Resume || kind == ECommandKind::ResolveExternal) {
            if (!Fresh() || submission.TopologyRevision != Topology.Revision() || submission.StateFingerprint != Epoch.AdminFingerprint) {
                Reject(command.Id, "STALE_PLAN_BINDING");
                return;
            }
        }
        std::optional<TDecision> binding;
        if (kind == ECommandKind::AdmitOperation) {
            if (!submission.PlanRequest) {
                Reject(command.Id, "PLAN_REQUEST_REQUIRED");
                return;
            }
            auto plan = PlanWorkflow(*Cluster, *submission.PlanRequest);
            if (!plan.Plan || !plan.RequiredAcknowledgements.empty()) {
                Reject(command.Id, plan.Error.empty() ? "ACKNOWLEDGEMENTS_REQUIRED" : plan.Error);
                return;
            }
            command.Plan = *plan.Plan;
            if (command.Plan.Kind != EOperationKind::Failover && !Topology.CanDispatchLifecycle()) {
                Reject(command.Id, "CONFIG_STALE_OR_CONFLICTED");
                return;
            }
            if (submission.DecisionId || command.Plan.Automatic) {
                if (!Decision || !submission.DecisionId || *submission.DecisionId != Decision->Id || !Decision->Confirmed
                    || !IsQualified(Qualification()) || Decision->Update != command.Plan.Steps.front().Update
                    || TSkipperClock::Now() + Options.Config.Polling.DispatchMargin > Decision->EvidenceExpiry)
                {
                    Reject(command.Id, "AUTOMATIC_DECISION_UNAVAILABLE");
                    return;
                }
                command.Plan.EvidenceBound = true;
                binding = *Decision;
                command.Plan.SuppressionKey = SuppressionKey(*Decision);
            }
        }
        if (kind == ECommandKind::Resume) {
            const auto operation = Applied->Operations.find(command.OperationId);
            if (operation != Applied->Operations.end() && operation->second.Plan.EvidenceBound) {
                if (!submission.DecisionId || !Decision || *submission.DecisionId != Decision->Id || !Decision->Confirmed
                    || !IsQualified(Qualification()) || operation->second.NextStep >= operation->second.Plan.Steps.size()
                    || operation->second.Plan.Steps[operation->second.NextStep].Update != Decision->Update
                    || TSkipperClock::Now() + Options.Config.Polling.DispatchMargin > Decision->EvidenceExpiry)
                {
                    Reject(command.Id, "AUTOMATIC_DECISION_UNAVAILABLE");
                    return;
                }
                binding = *Decision;
            }
        }
        command.Ready = Ready();
        command.LocalStopped = Stopped;
        command.Fresh = Fresh();
        command.Coherent = Fresh();
        command.AuditTime = TSkipperClock::Now().ToString();
        if (Cluster) {
            command.Observed = *Cluster;
        }
        Propose(std::move(command), true, std::move(binding));
    }

    bool BackgroundChanges() {
        if (!Cluster || !Fresh() || !Applied->ExclusiveHistory) {
            return false;
        }
        bool work = false;
        for (const auto& [id, operation] : Applied->Operations) {
            (void)id;
            for (const auto& background : operation.Background) {
                work = work || !background.Complete;
            }
        }
        if (!work) {
            return false;
        }
        auto command = Command(ECommandKind::Reconcile);
        for (const auto& [pile, expiry] : HealthyExpiries) {
            if (TSkipperClock::Now() < expiry) {
                command.FreshHealthyPiles.insert(pile);
            }
        }
        TString key = TStringBuilder() << Epoch.AdminFingerprint << ';' << Applied->ControllerRevision << ';';
        for (const auto& pile : command.FreshHealthyPiles) {
            key += TStringBuilder() << pile.size() << ':' << pile;
        }
        if (key == LastReconcileKey) {
            return false;
        }
        LastReconcileKey = key;
        const auto candidate = Apply(*Applied, command);
        bool changed = candidate.State.ExclusiveHistory != Applied->ExclusiveHistory
            || candidate.State.AcceptedGenerationHighWater != Applied->AcceptedGenerationHighWater;
        for (const auto& [id, operation] : candidate.State.Operations) {
            const auto& old = Applied->Operations.at(id);
            changed = changed || operation.Revision != old.Revision || operation.Status != old.Status || operation.NextStep != old.NextStep;
        }
        if (changed) {
            Propose(std::move(command), false);
            return true;
        }
        return false;
    }

    void Pump() {
        if (Pending || !StorageReady) {
            return;
        }
        if (!Outcomes.empty()) {
            auto result = std::move(Outcomes.front());
            Outcomes.pop_front();
            const auto found = Applied->Attempts.find(result.AttemptId);
            if (found == Applied->Attempts.end() || found->second.DesignatedProcess != result.ProcessId) {
                return;
            }
            auto command = Command(result.NeverDispatched ? ECommandKind::NeverDispatched : ECommandKind::RpcOutcome);
            command.OperationId = found->second.OperationId;
            command.Attempt = found->second;
            command.Outcome = std::move(result.Outcome);
            command.GatewayProof = result.GatewayProof;
            Propose(std::move(command), false);
            return;
        }
        if (!Submissions.empty()) {
            auto submission = std::move(Submissions.front());
            Submissions.pop_front();
            Admit(std::move(submission));
            return;
        }
        if (Cluster && Fresh() && Cluster->Generation > Applied->AcceptedGenerationHighWater) {
            Propose(Command(ECommandKind::AdvanceGeneration), false);
            return;
        }
        if (BackgroundChanges()) {
            return;
        }
        if (TryAcceptance() || TryAutomatic()) {
            return;
        }
        ScheduleWorkflow();
    }

    bool TryAcceptance() {
        if (!Applied->UnresolvedAttempt || !Fresh() || !Options.NoReplayPathQualified || !Options.AcceptanceMarkersQualified
            || Options.AcceptanceProvenance.empty() || !Applied->ExclusiveHistory)
        {
            return false;
        }
        const auto& attempt = Applied->Attempts.at(*Applied->UnresolvedAttempt);
        const auto handoff = HandoffTimes.find(attempt.Id);
        if (handoff == HandoffTimes.end()) {
            return false;
        }
        const auto byNode = AdminByNode();
        bool orderedQuorum = false;
        for (const auto& sample : Samples) {
            size_t count = 0;
            std::set<ui32> seen;
            for (const auto id : sample.Nodes) {
                const auto reply = byNode.find(id);
                if (seen.insert(id).second && reply != byNode.end()
                    && reply->second.StartedAt > handoff->second && reply->second.IssuedAfterHandoff.contains(attempt.Id)
                    && reply->second.View.ReceivedAt <= TSkipperClock::Now()
                    && TSkipperClock::Now() - reply->second.View.ReceivedAt < Options.Config.Polling.ObservationTtl
                    && CanonicalFingerprint(reply->second.View) == Epoch.AdminFingerprint)
                {
                    ++count;
                }
            }
            orderedQuorum = orderedQuorum || (sample.Threshold == sample.Nodes.size() / 2 + 1 && count >= sample.Threshold);
        }
        if (!orderedQuorum) {
            return false;
        }
        auto command = Command(ECommandKind::AcceptByObservation);
        command.Attempt = attempt;
        command.OperationId = attempt.OperationId;
        auto& proof = command.Acceptance;
        proof.Observed = *Cluster;
        proof.TopologyRevision = Topology.Revision();
        proof.Fresh = true;
        proof.IdentityVerified = true;
        proof.OrderedAfterSend = true;
        proof.Coherent = true;
        proof.NoReplayPathQualified = true;
        proof.MarkerQualified = true;
        // Evidence validity follows the exclusive interval, which Observe keeps
        // until handover; the ownership acknowledgement only permits new work.
        proof.CompleteExclusiveHistory = Applied->ExclusiveHistory;
        proof.NoAlternativeProducer = Applied->ExclusiveHistory;
        proof.CertifiedAutomaticFollowup = Options.AutomaticFollowupQualified;
        proof.Provenance = Options.AcceptanceProvenance;
        if (ValidateAcceptance(*Applied, attempt, proof).empty()) {
            Propose(std::move(command), false);
            return true;
        }
        return false;
    }

    bool TryAutomatic() {
        if (!Decision || Decision->Kind != EDecisionKind::Failover || !Decision->Confirmed || !Ready()
            || !IsQualified(Qualification()) || Applied->RequestedMode != ERequestedMode::Auto || !Applied->Ownership
            || Applied->UnresolvedAttempt || TSkipperClock::Now() + Options.Config.Polling.DispatchMargin > Decision->EvidenceExpiry)
        {
            return false;
        }
        const auto suppression = SuppressionKey(*Decision);
        if (Applied->SuppressedAutomaticPlans.contains(suppression)) {
            return false;
        }
        if (Applied->OrdinaryOwner) {
            const auto& current = Applied->Operations.at(*Applied->OrdinaryOwner);
            if (!IsTerminal(current.Status) && current.Plan.Automatic && current.Plan.SuppressionKey == suppression) {
                const auto binding = AutomaticBindings.find(current.Id);
                if (current.Status == EOperationStatus::Queued && (binding == AutomaticBindings.end() || !BindingCurrent(current.Id)
                    || binding->second.Epoch != Epoch || TSkipperClock::Now() + Options.Config.Polling.DispatchMargin > binding->second.EvidenceExpiry))
                {
                    // A queued step has no intent, so it adopts the fresh
                    // confirmation of the same plan instead of a duplicate.
                    AutomaticBindings[current.Id] = *Decision;
                    BoundConfirmationGeneration[current.Id] = ConfirmationGeneration;
                    BoundRoundClose[current.Id] = LastRoundClosedAt;
                }
                return false;
            }
        }
        TPlanRequest request;
        request.Kind = EOperationKind::Failover;
        request.FailedTargets = Decision->Targets;
        if (Decision->Primary && Decision->Targets.contains(*Decision->Primary)) {
            request.Successor = Decision->Candidate;
        }
        request.Automatic = true;
        request.ExternalPrerequisitesAcknowledged = true;
        request.ResynchronizingTargetQualified = Qualification().CoversResynchronizing;
        auto plan = PlanWorkflow(*Cluster, request);
        if (!plan.Plan) {
            return false;
        }
        auto command = Command(ECommandKind::AdmitOperation);
        command.OperationId = Id();
        command.Plan = *plan.Plan;
        command.Plan.EvidenceBound = true;
        command.Plan.SuppressionKey = suppression;
        const auto candidate = Apply(*Applied, command);
        if (!candidate.Result.Ok) {
            return false;
        }
        Propose(std::move(command), false, *Decision);
        return true;
    }

    void ScheduleWorkflow() {
        if (!Ready() || Applied->RequestedMode == ERequestedMode::Observe || !Applied->Ownership || Applied->UnresolvedAttempt
            || !Applied->OrdinaryOwner)
        {
            return;
        }
        const auto found = Applied->Operations.find(*Applied->OrdinaryOwner);
        if (found == Applied->Operations.end()) {
            return;
        }
        const auto& operation = found->second;
        if (operation.Status != EOperationStatus::Queued || operation.NextStep >= operation.Plan.Steps.size()) {
            return;
        }
        for (const auto& [id, read] : Reads) {
            (void)id;
            if (read.Purpose == EReadPurpose::Preflight && read.OperationId == operation.Id) {
                return;
            }
        }
        const auto& step = operation.Plan.Steps[operation.NextStep];
        if (!step.ExpectedFingerprint.empty() && step.ExpectedFingerprint != Epoch.AdminFingerprint) {
            return;
        }
        if (operation.Plan.Kind != EOperationKind::Failover && !Topology.CanDispatchLifecycle()) {
            return;
        }
        std::optional<TNode> chosen;
        if (operation.Plan.EvidenceBound) {
            const auto binding = AutomaticBindings.find(operation.Id);
            if (binding == AutomaticBindings.end() || !BindingCurrent(operation.Id) || !binding->second.Endpoint || !binding->second.Confirmed
                || binding->second.Epoch != Epoch || TSkipperClock::Now() + Options.Config.Polling.DispatchMargin > binding->second.EvidenceExpiry)
            {
                return;
            }
            chosen = binding->second.Endpoint;
        } else {
            auto candidates = VerifiedInventory(Topology.Topology().Nodes);
            const auto last = LastPreflightNode.find(operation.Id);
            if (last != LastPreflightNode.end()) {
                const auto next = std::find_if(candidates.begin(), candidates.end(), [&](const auto& node) {
                    return node.NodeId > last->second;
                });
                std::rotate(candidates.begin(), next, candidates.end());
            }
            auto& tried = PreflightTried[operation.Id];
            for (const auto& node : candidates) {
                if (tried.size() >= Options.Config.Polling.GlobalAdminConcurrency) {
                    break;
                }
                if (tried.contains(node.NodeId) || !node.Pile || !Cluster->Piles.contains(*node.Pile) || (step.Update.contains(*node.Pile)
                    && step.Update.at(*node.Pile) == EPileState::Disconnected))
                {
                    continue;
                }
                const auto state = Cluster->Piles.at(*node.Pile);
                if (state == EPileState::Disconnected || state == EPileState::Inactive) {
                    continue;
                }
                // Retry only before an intent exists. A fresh exact same-node
                // preflight is still mandatory for every alternate endpoint.
                LastPreflightNode[operation.Id] = node.NodeId;
                if (Scheduler.AcquireAdmin(node.Endpoint, true)) {
                    chosen = node;
                    break;
                }
            }
            if (!chosen) {
                return;
            }
        }
        if (!chosen || (operation.Plan.EvidenceBound && !Scheduler.AcquireAdmin(chosen->Endpoint, true))) {
            return;
        }
        auto timeout = Options.Config.Polling.ReadRpcTimeout;
        if (operation.Plan.EvidenceBound) {
            const auto expiry = AutomaticBindings.at(operation.Id).EvidenceExpiry;
            const auto now = TSkipperClock::Now();
            if (now + Options.Config.Polling.DispatchMargin >= expiry) {
                Scheduler.ReleaseAdmin(chosen->Endpoint);
                return;
            }
            timeout = std::min(timeout, expiry - now - Options.Config.Polling.DispatchMargin);
        }
        auto request = Request(EReadMethod::Admin, EReadPurpose::Preflight, chosen->Endpoint, timeout);
        request.Node = *chosen;
        request.NodeId = chosen->NodeId;
        request.OperationId = operation.Id;
        request.WorkflowRevision = operation.Revision;
        request.Request.OperationPriority = true;
        if (!Read(std::move(request))) {
            Scheduler.ReleaseAdmin(chosen->Endpoint);
        } else if (!operation.Plan.EvidenceBound) {
            // An occupied slot is not a failed RPC: retry it when that read
            // settles, without waiting for another poll to occupy it again.
            PreflightTried[operation.Id].insert(chosen->NodeId);
        }
    }

    void CompletePreflight(const TReadContext& read, const TReadResult& result) {
        const auto found = Applied->Operations.find(read.OperationId);
        if (!result.Success || !result.Cluster || !Fresh() || found == Applied->Operations.end() || Pending
            || found->second.Status != EOperationStatus::Queued || found->second.Revision != read.WorkflowRevision
            || read.Request.Epoch != Epoch || !Ready() || Applied->UnresolvedAttempt || Applied->RequestedMode == ERequestedMode::Observe
            || OutcomeReservations.size() >= 16)
        {
            return;
        }
        if (result.FinishedAt > read.Request.Deadline || result.FinishedAt > TSkipperClock::Now()) {
            return;
        }
        const auto& operation = found->second;
        if (!ReadyForOperation(operation)) {
            return;
        }
        const auto& step = operation.Plan.Steps[operation.NextStep];
        if (CanonicalFingerprint(*result.Cluster) != Epoch.AdminFingerprint || result.Cluster->Generation < Applied->AcceptedGenerationHighWater
            || result.Cluster->Endpoint != read.Node.Endpoint)
        {
            Invalidate();
            return;
        }
        if (operation.Plan.EvidenceBound) {
            const auto binding = AutomaticBindings.find(operation.Id);
            if (binding == AutomaticBindings.end() || !BindingCurrent(operation.Id) || binding->second.Epoch != Epoch
                || TSkipperClock::Now() + Options.Config.Polling.DispatchMargin > binding->second.EvidenceExpiry)
            {
                return;
            }
        }
        auto command = Command(ECommandKind::AdmitAttempt);
        command.OperationId = operation.Id;
        command.ExpectedWorkflowRevision = operation.Revision;
        auto& attempt = command.Attempt;
        attempt.Id = Id();
        attempt.OperationId = operation.Id;
        attempt.Step = operation.NextStep;
        attempt.WorkflowRevision = operation.Revision;
        attempt.Method = step.Method;
        attempt.Update = step.Update;
        attempt.Target = step.Target;
        attempt.Endpoint = read.Node.Endpoint;
        attempt.NodeId = read.Node.NodeId;
        attempt.DesignatedMember = Options.MemberId;
        attempt.DesignatedProcess = Options.ProcessId;
        attempt.Baseline = *result.Cluster;
        attempt.TopologyRevision = Topology.Revision();
        attempt.ArgumentsHash = MutationArgumentsHash(attempt);
        if (!Apply(*Applied, command).Result.Ok) {
            return;
        }
        LastPreflightNode.erase(operation.Id);
        PreflightTried.erase(operation.Id);
        OutcomeReservations.insert(attempt.Id);
        Preflights[attempt.Id] = result;
        Propose(std::move(command), false);
    }

    void DispatchCommitted(const TMutationAttempt& attempt) {
        if (Granted.contains(attempt.Id)) {
            return;
        }
        const auto preflight = Preflights.find(attempt.Id);
        const auto operation = Applied->Operations.find(attempt.OperationId);
        const auto now = TSkipperClock::Now();
        bool valid = preflight != Preflights.end() && operation != Applied->Operations.end() && ReadyForOperation(operation->second)
            && preflight->second.Request.Epoch == Epoch
            && Applied->RequestedMode != ERequestedMode::Observe && Applied->Ownership
            && attempt.DesignatedProcess == Options.ProcessId && attempt.TopologyRevision == Topology.Revision()
            && CanonicalFingerprint(attempt.Baseline) == Epoch.AdminFingerprint
            && operation->second.Status == EOperationStatus::Executing
            && operation->second.Revision == attempt.WorkflowRevision + 1;
        TInstant expiry;
        TInstant evidenceExpiry;
        if (valid) {
            // AdmissionBudget is a measured sizing input, never a second lease.
            // An ordinary manual invocation retains its actual admin-read TTL;
            // an evidence-bound invocation retains its original health TTL.
            expiry = preflight->second.StartedAt + Options.Config.Polling.ObservationTtl;
            if (operation->second.Plan.EvidenceBound) {
                const auto binding = AutomaticBindings.find(operation->first);
                valid = binding != AutomaticBindings.end() && BindingCurrent(operation->first)
                    && binding->second.Confirmed && binding->second.Epoch == Epoch;
                if (valid) {
                    evidenceExpiry = binding->second.EvidenceExpiry;
                    expiry = evidenceExpiry;
                    if (now + Options.Config.Polling.DispatchMargin > evidenceExpiry) {
                        ++FreshnessMisses;
                    }
                }
            }
            valid = valid && now + Options.Config.Polling.DispatchMargin <= expiry;
        }
        if (!valid) {
            if (Closing.insert(attempt.Id).second) {
                Effects.CloseAttempts.push_back(attempt.Id);
            }
            return;
        }
        TDispatchRequest dispatch;
        dispatch.Attempt = attempt;
        dispatch.Permission = {Id(), preflight->second.Request.Id, Options.ProcessId, attempt.ArgumentsHash,
            attempt.IntentIndex, expiry, evidenceExpiry, operation->second.Plan.Automatic, Epoch,
            operation->second.Plan.EvidenceBound ? ConfirmationGeneration : 0};
        Effects.Dispatches.push_back(std::move(dispatch));
        Granted.insert(attempt.Id);
        Preflights.erase(preflight);
    }

    void MaybeCheckpoint() {
        const auto now = TSkipperClock::Now();
        if (!CheckpointId.empty() || Pending || !StorageReady || Applied->AppliedIndex == CheckpointIndex) {
            return;
        }
        if (!Stopping && Applied->AppliedIndex - CheckpointIndex < 1000 && now - LastCheckpoint < TDuration::Seconds(60)) {
            return;
        }
        if (Stopping && (!Outcomes.empty() || !OutcomeReservations.empty())) {
            return;
        }
        TStorageRequest request;
        request.Id = Id();
        request.Kind = EStorageKind::Checkpoint;
        request.State = Applied;
        CheckpointId = request.Id;
        CheckpointTargetIndex = Applied->AppliedIndex;
        Effects.Stores.push_back(std::move(request));
    }

    std::optional<TNode> KnownNode(const TString& endpoint) const {
        for (const auto& node : Topology.Topology().Nodes) {
            if (node.Endpoint == endpoint && node.NodeId && node.Pile) {
                return node;
            }
        }
        return std::nullopt;
    }

    void ForgetEndpoint(const TString& endpoint, ui32 nodeId, const TString& reason) {
        if (nodeId) {
            Observations.erase(std::remove_if(Observations.begin(), Observations.end(), [&](const auto& observation) {
                return observation.NodeId == nodeId;
            }), Observations.end());
        }
        UnsupportedObservations.erase(endpoint);
        AdminByEndpoint.erase(endpoint);
        ConfigByEndpoint.erase(endpoint);
        auto& diagnostic = Diagnostics[endpoint];
        diagnostic.Endpoint = endpoint;
        diagnostic.Reasons = {reason};
        ++Epoch.ReadinessGeneration;
        Invalidate();
        NextConfig = TSkipperClock::Now();
        NextBootstrap = TSkipperClock::Now();
    }

    void InvalidateIdentity(const TString& endpoint, const TString& reason) {
        const auto node = KnownNode(endpoint);
        ForgetEndpoint(endpoint, node ? node->NodeId : 0, reason);
        Topology.InvalidateEndpoint(endpoint);
        AdminBlocker = "RESPONDER_IDENTITY_REDISCOVERY_REQUIRED";
    }

    void ForgetReaddressed() {
        for (const auto& node : Topology.TakeReaddressed()) {
            ForgetEndpoint(node.Endpoint, node.NodeId, "CONFIGURED_ADDRESS_CHANGED");
        }
    }

    // A probe is consumed once; the tracker owns the binding it establishes.
    void ResolveIdentity(const TNode& responder) {
        const auto& endpoint = responder.Endpoint;
        std::vector<TNode> matches;
        for (const auto& node : Topology.Topology().Nodes) {
            if ((node.InventoryNodeId && node.InventoryNodeId == responder.NodeId)
                || (!node.InventoryNodeId && (node.Host == responder.Host || node.Endpoint == endpoint)))
            {
                matches.push_back(node);
            }
        }
        if (matches.size() != 1) {
            if (KnownNode(endpoint)) {
                InvalidateIdentity(endpoint, "RESPONDER_IDENTITY_MISMATCH");
            }
            return;
        }
        auto node = matches.front();
        const std::set<TString> names(Topology.Topology().ConfiguredPiles.begin(), Topology.Topology().ConfiguredPiles.end());
        const auto error = VerifyBinding(node, responder.NodeId, responder.Pile, names, false);
        if (!error.empty()) {
            InvalidateIdentity(endpoint, error);
            return;
        }
        if (const auto previous = KnownNode(endpoint); previous && previous->LogicalId != node.LogicalId) {
            ForgetEndpoint(endpoint, previous->NodeId, "RESPONDER_IDENTITY_CHANGED");
        }
        node.Endpoint = endpoint;
        node.DirectEndpoint = true;
        if (const auto admin = AdminByEndpoint.find(endpoint); admin != AdminByEndpoint.end()) {
            node.AdminFingerprint = CanonicalFingerprint(admin->second.View);
            node.AdminObservedAt = admin->second.View.ReceivedAt;
        }
        Topology.RememberVerified(node);
    }

    void Diagnostic(const TReadResult& result) {
        TDiagnosticSummary summary;
        summary.Endpoint = result.Request.Endpoint;
        summary.Profile = TString(EvidenceProfileName(Options.Config.EvidenceProfile));
        summary.Source = Qualification().Source;
        summary.CheckedAt = result.FinishedAt;
        summary.Status = Short(result.Status);
        RpcTiming(summary, result);
        summary.Reasons = {"UNQUALIFIED_EVIDENCE_PROFILE"};
        const bool identityOnly = result.Request.IdentityOnly;
        const bool capabilityUnavailable = Options.Config.EvidenceProfile == EEvidenceProfile::NodewardenLocalV1;
        if (identityOnly) {
            summary.Profile = "selfcheck_identity_v1";
            summary.Source = ESourceKind::Unsupported;
            summary.Reasons = {"IDENTITY_ONLY_NOT_DECISION_EVIDENCE"};
        } else if (capabilityUnavailable) {
            summary.Source = ESourceKind::Unsupported;
            summary.Reasons = {"UNSUPPORTED_DIAGNOSTIC_CAPABILITY"};
            if (Cluster) {
                for (const auto& [name, state] : Cluster->Piles) {
                    (void)state;
                    summary.Targets[name].Reasons = summary.Reasons;
                }
            }
        }
        std::optional<TNode> responder;
        if (result.Health) {
            const auto& health = *result.Health;
            if (health.has_location()) {
                const auto& location = health.location();
                summary.NodeId = location.id();
                if (location.has_pile()) {
                    summary.Pile = TString(location.pile().name());
                }
                responder.emplace();
                responder->NodeId = location.id();
                responder->Host = location.host();
                responder->Pile = summary.Pile;
                responder->Endpoint = result.Request.Endpoint;
            }
            TDiagnosticInput input;
            input.RpcSuccess = result.Success;
            input.SupportedOverallStatus = health.self_check_result() >= Ydb::Monitoring::SelfCheck::GOOD
                && health.self_check_result() <= Ydb::Monitoring::SelfCheck::EMERGENCY;
            for (const auto& issue : health.issue_log()) {
                if (issue.type() == "HEALTHCHECK_STATUS" && issue.status() == Ydb::Monitoring::StatusFlag::RED) {
                    input.Truncated = true;
                }
                TDiagnosticIssue parsed;
                parsed.Id = issue.id();
                parsed.Type = issue.type();
                parsed.Red = issue.status() == Ydb::Monitoring::StatusFlag::RED;
                if (issue.has_location() && issue.location().has_storage() && issue.location().storage().has_pool()
                    && issue.location().storage().pool().has_group() && issue.location().storage().pool().group().has_pile())
                {
                    parsed.Pile = TString(issue.location().storage().pool().group().pile().name());
                }
                for (const auto& reason : issue.reason()) {
                    parsed.Reasons.emplace_back(reason);
                }
                input.Issues.push_back(std::move(parsed));
            }
            // The verbose group list does not establish a release-qualified
            // expected physical inventory. Preserve UNKNOWN rather than
            // manufacture complete coverage from returned groups alone.
            if (!identityOnly && !capabilityUnavailable && IsQualified(Qualification()) && Options.EvidenceDecoder && Cluster) {
                summary.Targets = Options.EvidenceDecoder(result, *Cluster);
                summary.Reasons.clear();
            } else {
                auto qualification = Qualification();
                if (identityOnly || capabilityUnavailable) {
                    qualification.Qualified = false;
                }
                summary.Targets = ParseDiagnostic(Cluster.value_or(TClusterView{}), qualification, input);
            }
        }
        Diagnostics[summary.Endpoint] = std::move(summary);
        if (responder) {
            ResolveIdentity(*responder);
        }
        MakeSamples();
    }

    std::optional<TClusterView> Prebridge(const TReadResult& result) const {
        if (result.Status != "NOT_FOUND" || Topology.Topology().ConfiguredPiles.size() != 1) {
            return std::nullopt;
        }
        TClusterView view;
        view.Converted = false;
        view.PileOrder = Topology.Topology().ConfiguredPiles;
        view.Piles[view.PileOrder.front()] = EPileState::Primary;
        view.Endpoint = result.Request.Endpoint;
        view.ReceivedAt = result.FinishedAt;
        return view;
    }

    void ReadCompleted(TReadResult result) {
        const auto found = Reads.find(result.Request.Id);
        if (found == Reads.end() || result.Request.ProcessId != Options.ProcessId
            || result.Request.Endpoint != found->second.Request.Endpoint || result.Request.Method != found->second.Request.Method)
        {
            return;
        }
        auto read = std::move(found->second);
        Reads.erase(found);
        result.Request = read.Request;
        if (read.Request.Method == EReadMethod::Admin && !read.Request.Bracket) {
            Scheduler.ReleaseAdmin(read.Request.Endpoint);
        }
        if (Stopping) {
            FinishChain(read.NodeId);
            return;
        }
        if ((read.Request.Method == EReadMethod::Admin || read.Request.Method == EReadMethod::Health)
            && read.Request.Epoch != Epoch && !Epoch.AdminFingerprint.empty())
        {
            FinishChain(read.NodeId);
            return;
        }
        if (read.Request.Method == EReadMethod::Config) {
            if (result.Success) {
                for (const auto& yaml : result.Configs) {
                    try {
                        const auto parsed = ParseMainConfig(yaml, Options.Config.YdbEndpoints.front().Scheme);
                        Topology.ProvisionTentative(parsed);
                        ConfigByEndpoint[read.Request.Endpoint] = {0, parsed, result.FinishedAt};
                        SelectAdmin();
                        TryConfig();
                        break;
                    } catch (const std::exception&) {
                        ConfigBlocker = "MALFORMED_OR_UNSUPPORTED_MAIN_CONFIG";
                    }
                }
            }
            return;
        }
        if (read.Request.Method == EReadMethod::Discovery) {
            for (auto& endpoint : result.Discovered) {
                if (Discovered.size() < 4096 && !endpoint.Endpoint.empty()) {
                    try {
                        const auto parsed = ParseEndpoint(endpoint.Endpoint);
                        if (parsed.Scheme == Options.Config.YdbEndpoints.front().Scheme) {
                            Discovered[endpoint.Endpoint] = std::move(endpoint);
                        }
                    } catch (const std::exception&) {
                        ++Skipped;
                    }
                }
            }
            return;
        }
        if (read.Request.Method == EReadMethod::Health) {
            Diagnostic(result);
            if (read.Purpose == EReadPurpose::BootstrapIdentity) {
                if (!result.Health && !read.Request.IdentityOnly
                    && Options.Config.EvidenceProfile == EEvidenceProfile::NodewardenLocalV1)
                {
                    auto identity = Request(EReadMethod::Health, EReadPurpose::BootstrapIdentity, read.Request.Endpoint,
                        Options.Config.Polling.HealthClientTimeout);
                    identity.Request.IdentityOnly = true;
                    Read(std::move(identity));
                }
                SelectAdmin();
                return;
            }
        }
        if (read.Request.Method == EReadMethod::Admin) {
            if (result.Cluster && !ValidateClusterView(*result.Cluster).empty()) {
                result.Cluster->Endpoint = read.Request.Endpoint;
                result.Cluster->ReceivedAt = result.FinishedAt;
                const auto node = KnownNode(read.Request.Endpoint);
                UnsupportedObservations[read.Request.Endpoint] = {*result.Cluster, node ? node->NodeId : 0, false, ++AdminObservationSerial};
                UnsupportedCluster = *result.Cluster;
                AdminByEndpoint.erase(read.Request.Endpoint);
                AdminBlocker = "UNSUPPORTED_ADMIN_VIEW";
                Invalidate();
                FinishChain(read.NodeId);
                return;
            }
            if (!result.Cluster) {
                result.Cluster = Prebridge(result);
                result.Success = bool(result.Cluster);
            }
            if (result.Success && result.Cluster) {
                result.Cluster->Endpoint = read.Request.Endpoint;
                result.Cluster->ReceivedAt = result.FinishedAt;
                if (const auto unsupported = UnsupportedObservations.find(read.Request.Endpoint); unsupported != UnsupportedObservations.end()) {
                    unsupported->second.Replaced = true;
                }
                AdminByEndpoint[read.Request.Endpoint] = {*result.Cluster, ++AdminObservationSerial, result.StartedAt,
                    read.IssuedAfterHandoff};
                if (const auto node = KnownNode(read.Request.Endpoint)) {
                    auto updated = *node;
                    updated.AdminFingerprint = CanonicalFingerprint(*result.Cluster);
                    updated.AdminObservedAt = result.FinishedAt;
                    Topology.RememberVerified(updated);
                }
                SelectAdmin();
            }
            if (read.Purpose == EReadPurpose::Preflight) {
                CompletePreflight(read, result);
                return;
            }
            if (read.Purpose == EReadPurpose::Poll) {
                return;
            }
        }
        const auto chainIt = Chains.find(read.NodeId);
        if (chainIt == Chains.end()) {
            return;
        }
        auto& chain = chainIt->second;
        if (!result.Success || result.FinishedAt > chain.Deadline || chain.Epoch != Epoch || !Round || Round->Id != chain.RoundId) {
            FinishChain(read.NodeId);
            return;
        }
        if (read.Purpose == EReadPurpose::Before && result.Cluster) {
            chain.Observation.BeforeAt = result.FinishedAt;
            chain.Observation.BeforeFingerprint = CanonicalFingerprint(*result.Cluster);
            ContinueChain(read.NodeId, EReadMethod::Health, EReadPurpose::Health);
        } else if (read.Purpose == EReadPurpose::Health) {
            auto& observation = chain.Observation;
            observation.RequestId = read.Request.Id;
            observation.HealthStartedAt = result.StartedAt ? result.StartedAt : observation.HealthStartedAt;
            observation.Profile = Qualification().Profile;
            observation.Source = Qualification().Source;
            const auto& diagnostic = Diagnostics[read.Request.Endpoint];
            observation.IdentityVerified = result.Health && result.Health->has_location() && result.Health->location().id() == chain.Node.NodeId
                    && result.Health->location().has_pile() && chain.Node.Pile
                    && TString(result.Health->location().pile().name()) == *chain.Node.Pile;
            observation.DirectLocalOrigin = diagnostic.DirectLocalOrigin;
            observation.Origin = diagnostic.Origin;
            observation.Vantage = diagnostic.Vantage;
            observation.SnapshotId = diagnostic.SnapshotId;
            if (Options.ProvenanceDecoder) {
                Options.ProvenanceDecoder(observation, result);
            }
            observation.Targets = Diagnostics[read.Request.Endpoint].Targets;
            ContinueChain(read.NodeId, EReadMethod::Admin, EReadPurpose::After);
        } else if (read.Purpose == EReadPurpose::After && result.Cluster) {
            chain.Observation.FinishedAt = result.FinishedAt;
            chain.Observation.AfterFingerprint = CanonicalFingerprint(*result.Cluster);
            Observations.push_back(std::move(chain.Observation));
            FinishChain(read.NodeId);
        } else {
            FinishChain(read.NodeId);
        }
    }

    void Poll() {
        const auto now = TSkipperClock::Now();
        if (!Started || now >= NextConfig) {
            NextConfig = now + Options.Config.Polling.ConfigInterval;
            std::set<TString> endpoints;
            for (const auto& seed : Options.Config.YdbEndpoints) {
                endpoints.insert(seed.Uri);
            }
            for (const auto& node : VerifiedInventory(Topology.Topology().Nodes)) {
                endpoints.insert(node.Endpoint);
            }
            std::vector<TString> ordered(endpoints.begin(), endpoints.end());
            for (size_t index = 0; index < std::min<size_t>(4, ordered.size()); ++index) {
                const auto& endpoint = ordered[(ConfigCursor + index) % ordered.size()];
                Read(Request(EReadMethod::Config, EReadPurpose::Config, endpoint, Options.Config.Polling.ReadRpcTimeout));
                Read(Request(EReadMethod::Discovery, EReadPurpose::Discovery, endpoint, Options.Config.Polling.ReadRpcTimeout));
            }
            if (!ordered.empty()) {
                ConfigCursor = (ConfigCursor + std::min<size_t>(4, ordered.size())) % ordered.size();
            }
        }
        if (!Started || now >= NextBootstrap) {
            NextBootstrap = now + Options.Config.Polling.RoundInterval;
            std::set<TString> endpoints;
            for (const auto& seed : Options.Config.YdbEndpoints) {
                endpoints.insert(seed.Uri);
            }
            for (const auto& [endpoint, discovered] : Discovered) {
                (void)discovered;
                endpoints.insert(endpoint);
            }
            for (const auto& node : Topology.Topology().Nodes) {
                if (!node.Endpoint.empty()) {
                    endpoints.insert(node.Endpoint);
                }
            }
            std::vector<TString> tentative;
            for (const auto& endpoint : endpoints) {
                if (!KnownNode(endpoint)) {
                    tentative.push_back(endpoint);
                }
            }
            const auto offered = std::min<size_t>(tentative.size(), Options.Config.Polling.GlobalAdminConcurrency);
            for (size_t index = 0; index < offered; ++index) {
                const auto& endpoint = tentative[(BootstrapCursor + index) % tentative.size()];
                Read(Request(EReadMethod::Health, EReadPurpose::BootstrapIdentity, endpoint, Options.Config.Polling.HealthClientTimeout));
            }
            if (!tentative.empty()) {
                BootstrapCursor = (BootstrapCursor + offered) % tentative.size();
            }
        }
        if (!Started || now >= NextAdmin) {
            NextAdmin = now + Options.Config.Polling.AdminInterval;
            std::set<TString> endpoints;
            if (Samples.empty()) {
                for (const auto& seed : Options.Config.YdbEndpoints) {
                    endpoints.insert(seed.Uri);
                }
            }
            for (const auto& sample : Samples) {
                for (const auto id : sample.Nodes) {
                    for (const auto& node : Topology.Topology().Nodes) {
                        if (node.NodeId == id) {
                            endpoints.insert(node.Endpoint);
                        }
                    }
                }
            }
            for (const auto& [id, attempt] : Applied->Attempts) {
                (void)id;
                const auto operation = Applied->Operations.find(attempt.OperationId);
                if (!IsAttemptClosed(attempt) || (operation != Applied->Operations.end() && !IsTerminal(operation->second.Status))) {
                    endpoints.insert(attempt.Endpoint);
                }
            }
            for (const auto& endpoint : endpoints) {
                if (Scheduler.AcquireAdmin(endpoint, false)) {
                    if (!Read(Request(EReadMethod::Admin, EReadPurpose::Poll, endpoint, Options.Config.Polling.ReadRpcTimeout))) {
                        Scheduler.ReleaseAdmin(endpoint);
                    }
                } else {
                    ++Skipped;
                }
            }
        }
        Started = true;
    }
};

TControllerCore::TControllerCore(TControllerOptions options)
    : Impl(std::make_unique<TImpl>(std::move(options))) {
}

TControllerCore::~TControllerCore() = default;

void TControllerCore::Tick() {
    if (!Impl->Stopping) {
        Impl->PreflightTried.clear();
        Impl->CloseRound();
        Impl->SelectAdmin();
        Impl->Publish();
        Impl->StartRound();
        Impl->Poll();
    }
    if (Impl->Pending && Impl->Pending->Unknown && !Impl->Pending->LookupInFlight) {
        auto& pending = *Impl->Pending;
        pending.StorageId = Impl->Id();
        pending.LookupInFlight = true;
        TStorageRequest request;
        request.Id = pending.StorageId;
        request.Kind = EStorageKind::ResolveUnknown;
        request.State = Impl->Applied;
        request.LookupId = pending.Command.Id;
        Impl->Effects.Stores.push_back(std::move(request));
    }
    Impl->Pump();
    Impl->MaybeCheckpoint();
    Impl->Publish();
}

void TControllerCore::Submit(TControllerSubmission submission) {
    if (Impl->Duplicate(submission)) {
        return;
    }
    if (Impl->Stopping) {
        Impl->Reject(submission.Command.Id, "SHUTTING_DOWN");
        return;
    }
    if (Impl->Submissions.size() >= 256) {
        Impl->Reject(submission.Command.Id, "BUSY");
        return;
    }
    Impl->Submissions.push_back(std::move(submission));
    Impl->Pump();
    Impl->Publish();
}

void TControllerCore::OnRead(TReadResult result) {
    Impl->ReadCompleted(std::move(result));
    Impl->Publish();
    Impl->Pump();
    Impl->Publish();
}

void TControllerCore::OnStorage(TStorageResult result) {
    if (result.Kind == EStorageKind::Checkpoint && result.Id == Impl->CheckpointId) {
        Impl->CheckpointId.clear();
        if (result.Success) {
            Impl->CheckpointIndex = Impl->CheckpointTargetIndex;
            Impl->LastCheckpoint = TSkipperClock::Now();
        } else {
            Impl->StorageReady = false;
            Impl->StorageBlocker = "CHECKPOINT_FAILED";
            Impl->Invalidate();
        }
        Impl->Publish();
        return;
    }
    if (!Impl->Pending || result.Id != Impl->Pending->StorageId) {
        return;
    }
    auto& pending = *Impl->Pending;
    pending.LookupInFlight = false;
    if (result.Proposal.Status == EProposalStatus::Unknown || (!result.Success && result.Proposal.Status != EProposalStatus::Rejected)) {
        pending.Unknown = true;
        Impl->StorageReady = false;
        Impl->StorageBlocker = "ADMISSION_UNKNOWN";
        Impl->Invalidate();
        ++Impl->Epoch.ReadinessGeneration;
        if (pending.External) {
            Impl->Effects.Replies.push_back({pending.Command.Id, result.Proposal, false});
        }
        Impl->Publish();
        return;
    }
    const bool committed = result.Proposal.Status == EProposalStatus::Committed;
    if (committed && (!result.Proposal.State || result.Proposal.State->AppliedIndex != pending.Candidate.State.AppliedIndex
        || result.Proposal.State->ControllerRevision != pending.Candidate.State.ControllerRevision
        || result.Proposal.Result != pending.Candidate.Result))
    {
        Impl->StorageReady = false;
        Impl->StorageBlocker = "DURABLE_CANDIDATE_MISMATCH";
        pending.Unknown = true;
        Impl->Invalidate();
        Impl->Publish();
        return;
    }
    const auto command = pending.Command;
    const bool external = pending.External;
    const auto binding = pending.Binding;
    const auto bindingGeneration = pending.ConfirmationGeneration;
    const auto bindingRoundClose = pending.BindingRoundClose;
    if (committed) {
        Impl->Applied = std::make_shared<const TDurableState>(std::move(*result.Proposal.State));
    } else if (result.Proposal.State && (result.Proposal.State->AppliedIndex != Impl->Applied->AppliedIndex
        || result.Proposal.State->ControllerRevision != Impl->Applied->ControllerRevision))
    {
        Impl->StorageReady = false;
        Impl->StorageBlocker = "DURABLE_PREFIX_MISMATCH";
        Impl->Publish();
        return;
    }
    Impl->Pending.reset();
    if (committed && result.Proposal.Result.Ok && binding) {
        // Rejected or unknown Resume/admission must not change live authority.
        // Keep the generation captured at proposal, even if commit was delayed
        // through a revocation. Its original evidence TTL is never extended.
        Impl->AutomaticBindings[command.OperationId] = *binding;
        Impl->BoundConfirmationGeneration[command.OperationId] = bindingGeneration;
        Impl->BoundRoundClose[command.OperationId] = bindingRoundClose;
    }
    if (!Impl->StorageReady) {
        ++Impl->Epoch.ReadinessGeneration;
        Impl->Invalidate();
    }
    Impl->StorageReady = true;
    Impl->StorageBlocker.clear();
    Impl->Publish();
    if (external) {
        Impl->Effects.Replies.push_back({command.Id, result.Proposal, true});
    }
    if (committed && result.Proposal.Result.Ok && command.Kind == ECommandKind::AdmitAttempt) {
        Impl->DispatchCommitted(Impl->Applied->Attempts.at(command.Attempt.Id));
    } else if (command.Kind == ECommandKind::AdmitAttempt) {
        Impl->OutcomeReservations.erase(command.Attempt.Id);
    }
    if (committed && (command.Kind == ECommandKind::RpcOutcome || command.Kind == ECommandKind::NeverDispatched)) {
        Impl->OutcomeReservations.erase(command.Kind == ECommandKind::RpcOutcome ? command.Outcome.AttemptId : command.Attempt.Id);
    }
    if (committed && (command.Kind == ECommandKind::SetMode || command.Kind == ECommandKind::ResolveExternal)) {
        ++Impl->Epoch.ReadinessGeneration;
        Impl->Invalidate();
    }
    Impl->Pump();
    Impl->MaybeCheckpoint();
    Impl->Publish();
}

void TControllerCore::OnMutation(TMutationResult result) {
    const auto found = Impl->Applied->Attempts.find(result.AttemptId);
    if (found == Impl->Applied->Attempts.end() || found->second.DesignatedProcess != result.ProcessId) {
        return;
    }
    if (!result.Terminal) {
        if (result.HandedOff && Impl->Granted.contains(result.AttemptId)
            && Impl->HandoffTimes.emplace(result.AttemptId, result.HandoffAt).second)
        {
            const auto& operation = Impl->Applied->Operations.at(found->second.OperationId);
            if (operation.Plan.EvidenceBound) {
                Impl->LastHandoffSlack = result.EvidenceSlack;
                const auto close = Impl->BoundRoundClose.find(operation.Id);
                if (close != Impl->BoundRoundClose.end() && result.HandoffAt >= close->second) {
                    Impl->LastRoundCloseToHandoff = result.HandoffAt - close->second;
                    Impl->Effects.Handoffs.push_back({result.AttemptId, *Impl->LastRoundCloseToHandoff, result.EvidenceSlack});
                    if (*Impl->LastRoundCloseToHandoff > Impl->Options.Config.Polling.AdmissionBudget) {
                        ++Impl->AdmissionBudgetOverruns;
                    }
                }
            }
        }
        Impl->Publish();
        return;
    }
    if (!Impl->OutcomeReservations.contains(result.AttemptId)) {
        return;
    }
    for (const auto& queued : Impl->Outcomes) {
        if (queued.AttemptId == result.AttemptId) {
            return;
        }
    }
    // The reservation was acquired before intent admission and is retained
    // until this terminal result is durably applied, including early acceptance.
    Impl->Outcomes.push_back(std::move(result));
    Impl->Pump();
    Impl->Publish();
}

void TControllerCore::SetLocalStop(bool stopped, ui64 revision) {
    if (revision < Impl->StopRevision) {
        return;
    }
    if (stopped != Impl->Stopped || revision != Impl->StopRevision) {
        Impl->Stopped = stopped;
        Impl->StopRevision = revision;
        ++Impl->Epoch.ReadinessGeneration;
        Impl->Invalidate();
        if (stopped && Impl->Applied->UnresolvedAttempt) {
            const auto& attempt = Impl->Applied->Attempts.at(*Impl->Applied->UnresolvedAttempt);
            if (attempt.DesignatedProcess == Impl->Options.ProcessId && !Impl->Granted.contains(attempt.Id)
                && Impl->Closing.insert(attempt.Id).second)
            {
                Impl->Effects.CloseAttempts.push_back(attempt.Id);
            }
        }
    }
    Impl->Publish();
}

void TControllerCore::Shutdown() {
    if (Impl->Stopping) {
        return;
    }
    Impl->Stopping = true;
    Impl->Invalidate();
    while (!Impl->Submissions.empty()) {
        Impl->Reject(Impl->Submissions.front().Command.Id, "SHUTTING_DOWN");
        Impl->Submissions.pop_front();
    }
    Impl->MaybeCheckpoint();
    Impl->Publish();
}

TControllerEffects TControllerCore::DrainEffects() {
    auto result = std::move(Impl->Effects);
    Impl->Effects = {};
    return result;
}

std::shared_ptr<const TControllerSnapshot> TControllerCore::Snapshot() const {
    return Impl->Published;
}

namespace {

TControllerOptions RuntimeOptions(const TConfig& config, const std::shared_ptr<TRuntimeResources>& resources) {
    TControllerOptions options;
    options.Config = config;
    options.Restored = resources->InitialState();
    options.MemberId = resources->MemberId();
    options.ProcessId = resources->ProcessId();
    options.NextId = [] { return TGUID::Create().AsUuidString(); };
    return options;
}

class TControllerActor final : public NActors::TActorBootstrapped<TControllerActor> {
public:
    static constexpr char ActorName[] = "BRIDGE_SKIPPER_CONTROLLER";

    TControllerActor(const TConfig& config, std::shared_ptr<TRuntimeResources> resources, NActors::TActorId gateway)
        : Resources(std::move(resources))
        , Gateway(gateway)
        , Core(RuntimeOptions(config, Resources))
    {
    }

    void Bootstrap() {
        Become(&TThis::StateWork);
        ObserveStop();
        Core.Tick();
        Flush();
        ScheduleTick();
    }

private:
    struct TWaiter {
        NActors::TActorId Sender;
        ui64 Cookie = 0;
        std::shared_ptr<void> Credit;
    };

    std::shared_ptr<TRuntimeResources> Resources;
    NActors::TActorId Gateway;
    TControllerCore Core;
    std::map<TString, std::vector<TWaiter>> Waiters;
    NMonitoring::THistogramPtr AdmissionLatency;
    NMonitoring::THistogramPtr HandoffSlack;
    ui64 TimerGeneration = 1;
    bool Dying = false;
    TInstant ShutdownDeadline;

    void ObserveStop() {
        const auto stop = Resources->StopLatch->Current();
        Core.SetLocalStop(stop.Stopped, stop.Revision);
    }

    void ScheduleTick() {
        auto* event = new TEvController::TEvWakeup;
        event->Generation = TimerGeneration;
        Schedule(TDuration::MilliSeconds(250), event);
    }

    void Flush() {
        // Install publication before sending effects: gateway validates against
        // this exact committed prefix independently immediately before handoff.
        Resources->PublishSnapshot(Core.Snapshot());
        auto effects = Core.DrainEffects();
        for (const auto& sample : effects.Handoffs) {
            if (!AdmissionLatency) {
                AdmissionLatency = Resources->Counters->GetHistogram("skipper_admission_latency_milliseconds",
                    NMonitoring::ExplicitHistogram({1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000, 120000}));
                HandoffSlack = Resources->Counters->GetHistogram("skipper_handoff_slack_milliseconds",
                    NMonitoring::ExplicitHistogram({0, 1, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000, 120000}));
            }
            AdmissionLatency->Collect(static_cast<double>(sample.RoundCloseToHandoff.MicroSeconds()) / 1000.0);
            HandoffSlack->Collect(static_cast<double>(sample.EvidenceSlack.MicroSeconds()) / 1000.0);
        }
        for (auto& request : effects.Reads) {
            Send(Gateway, new TEvRuntime::TEvGatewayRead(std::move(request)));
        }
        for (auto& request : effects.Stores) {
            const auto id = request.Id;
            const auto kind = request.Kind;
            if (!Resources->SubmitStore(std::move(request), SelfId())) {
                TStorageResult result;
                result.Id = id;
                result.Kind = kind;
                result.Proposal.Status = EProposalStatus::Unknown;
                result.Proposal.Error = "STORAGE_EXECUTOR_BUSY";
                Send(SelfId(), new TEvRuntime::TEvStorageResult(std::move(result)));
            }
        }
        for (auto& dispatch : effects.Dispatches) {
            Send(Gateway, new TEvRuntime::TEvDispatchAttempt(std::move(dispatch)));
        }
        for (const auto& id : effects.CloseAttempts) {
            auto* close = new TEvRuntime::TEvCloseAttempt;
            close->AttemptId = id;
            close->ProcessId = Resources->ProcessId();
            Send(Gateway, close);
        }
        for (auto& reply : effects.Replies) {
            const auto found = Waiters.find(reply.Id);
            if (found == Waiters.end()) {
                continue;
            }
            for (const auto& waiter : found->second) {
                auto* event = new TEvController::TEvResult;
                event->Reply = reply;
                Send(waiter.Sender, event, 0, waiter.Cookie);
            }
            if (reply.Final) {
                Waiters.erase(found);
            }
        }
    }

    void Handle(TEvController::TEvSubmit::TPtr& event) {
        const auto id = event->Get()->Submission.Command.Id;
        Waiters[id].push_back({event->Sender, event->Cookie, event->Get()->AdmissionCredit});
        ObserveStop();
        Core.Submit(std::move(event->Get()->Submission));
        Flush();
    }

    void Handle(TEvController::TEvWakeup::TPtr& event) {
        if (event->Get()->Generation != TimerGeneration) {
            return;
        }
        ObserveStop();
        Core.Tick();
        Flush();
        if (Dying && TSkipperClock::Now() >= ShutdownDeadline) {
            Become(&TThis::StateDying);
            PassAway();
            return;
        }
        ScheduleTick();
    }

    void Handle(TEvRuntime::TEvGatewayReadResult::TPtr& event) {
        ObserveStop();
        Core.OnRead(std::move(event->Get()->Result));
        Flush();
    }

    void Handle(TEvRuntime::TEvStorageResult::TPtr& event) {
        ObserveStop();
        Core.OnStorage(std::move(event->Get()->Result));
        Flush();
    }

    void Handle(TEvRuntime::TEvMutationResult::TPtr& event) {
        Core.OnMutation(std::move(event->Get()->Result));
        Flush();
    }

    void Handle(TEvRuntime::TEvLocalStopChanged::TPtr&) {
        ObserveStop();
        Flush();
    }

    void BeginShutdown() {
        if (Dying) {
            return;
        }
        Dying = true;
        ++TimerGeneration;
        ShutdownDeadline = TSkipperClock::Now() + TDuration::Seconds(10);
        Core.Shutdown();
        Flush();
        Resources->ControllerDraining.store(true);
        // Callback gates/storage producers belong to the coordinator. Actor
        // destruction neither joins them nor infers cancellation of YDB work.
        ScheduleTick();
    }

    STRICT_STFUNC(StateWork,
        hFunc(TEvController::TEvSubmit, Handle);
        hFunc(TEvController::TEvWakeup, Handle);
        hFunc(TEvRuntime::TEvGatewayReadResult, Handle);
        hFunc(TEvRuntime::TEvStorageResult, Handle);
        hFunc(TEvRuntime::TEvMutationResult, Handle);
        hFunc(TEvRuntime::TEvLocalStopChanged, Handle);
        cFunc(NActors::TEvents::TEvPoison::EventType, BeginShutdown);
    )

    STFUNC(StateDying) {
        (void)ev;
    }
};

} // namespace

NActors::IActor* CreateControllerActor(const TConfig& config,
    std::shared_ptr<TRuntimeResources> resources, NActors::TActorId gateway)
{
    return new TControllerActor(config, std::move(resources), gateway);
}

} // namespace NBridgeSkipper
