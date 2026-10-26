#include "evidence.h"

#include <algorithm>
#include <functional>

namespace NBridgeSkipper {

bool IsRecognizedProfile(const TString& profile) {
    return profile == "global_bsc_v1" || profile == "static_whiteboard_v1" || profile == "nodewarden_local_v1";
}

bool IsQualified(const TProfileQualification& q) {
    const bool sourceMatches = (q.Profile == "global_bsc_v1" && q.Source == ESourceKind::GlobalBsc)
        || (q.Profile == "static_whiteboard_v1" && q.Source == ESourceKind::LocalWhiteboard)
        || (q.Profile == "nodewarden_local_v1" && q.Source == ESourceKind::LocalNodewarden);
    return sourceMatches && q.Qualified && q.CapabilityPresent && !q.SupportedBuild.empty() && !q.FixtureRecord.empty()
        && (q.Source == ESourceKind::GlobalBsc || q.IndependentLocalOrigins);
}

std::map<TString, TAssessment> ParseDiagnostic(
    const TClusterView& view,
    const TProfileQualification& qualification,
    const TDiagnosticInput& input)
{
    std::map<TString, TAssessment> result;
    for (const auto& [name, state] : view.Piles) {
        result[name].Reasons.push_back(state == EPileState::NotSynchronized ? "RESYNC_FAILURE_EVIDENCE_UNSUPPORTED" : "INCOMPLETE_COVERAGE");
    }
    TString invalid;
    if (!IsQualified(qualification)) {
        invalid = "UNQUALIFIED_PROFILE";
    } else if (!input.RpcSuccess || !input.SupportedOverallStatus) {
        invalid = "INVALID_DIAGNOSTIC_RESPONSE";
    } else if (input.Truncated) {
        invalid = "HEALTHCHECK_STATUS_TRUNCATION";
    } else if (input.MandatoryShapeUnknown || input.MissingStorageData) {
        invalid = "INCOMPLETE_STORAGE_DIAGNOSTICS";
    }
    if (!invalid.empty()) {
        for (auto& [name, assessment] : result) {
            (void)name;
            assessment.Reasons = {invalid};
        }
        return result;
    }

    std::map<TString, const TPhysicalGroup*> groups;
    std::set<TString> invalidTargets;
    for (const auto& group : input.Groups) {
        if (!groups.emplace(group.Id, &group).second || !input.ExpectedGroups.contains(group.Id)) {
            return result;
        }
    }
    std::map<TString, size_t> expected;
    std::map<TString, bool> failed;
    for (const auto& [id, pile] : input.ExpectedGroups) {
        if (!view.Piles.contains(pile)) {
            return result;
        }
        ++expected[pile];
        const auto found = groups.find(id);
        if (found == groups.end() || !found->second->Covered || found->second->UnknownShape
            || !found->second->Pile || *found->second->Pile != pile)
        {
            invalidTargets.insert(pile);
        } else {
            failed[pile] = failed[pile] || found->second->Failed;
        }
    }

    std::map<TString, const TDiagnosticIssue*> issues;
    for (const auto& issue : input.Issues) {
        if (!issues.emplace(issue.Id, &issue).second) {
            return result;
        }
    }
    std::set<TString> visiting;
    std::function<std::optional<std::set<TString>>(const TString&)> resolve;
    resolve = [&](const TString& id) -> std::optional<std::set<TString>> {
        const auto found = issues.find(id);
        if (found == issues.end() || !visiting.insert(id).second) {
            return std::nullopt;
        }
        const auto& issue = *found->second;
        std::set<TString> piles;
        if (issue.Pile) {
            if (!view.Piles.contains(*issue.Pile)) {
                visiting.erase(id);
                return std::nullopt;
            }
            piles.insert(*issue.Pile);
        }
        for (const auto& child : issue.Reasons) {
            const auto resolved = resolve(child);
            if (!resolved) {
                visiting.erase(id);
                return std::nullopt;
            }
            piles.insert(resolved->begin(), resolved->end());
        }
        visiting.erase(id);
        return piles;
    };
    for (const auto& issue : input.Issues) {
        if (!issue.Red || (issue.Type != "BRIDGE_GROUP" && issue.Type != "STORAGE_GROUP")) {
            continue;
        }
        const auto piles = resolve(issue.Id);
        if (!piles || piles->empty()) {
            return result;
        }
        // Multi-child aggregates have no inherited direct attribution. Their
        // fully attributed children are handled independently below.
        if (issue.Pile || issue.Reasons.size() <= 1) {
            for (const auto& pile : *piles) {
                failed[pile] = true;
            }
        }
    }
    for (auto& [pile, assessment] : result) {
        const auto state = view.Piles.at(pile);
        if (!expected[pile] || invalidTargets.contains(pile)) {
            continue;
        }
        if (state == EPileState::NotSynchronized && (!qualification.CoversResynchronizing || qualification.Source == ESourceKind::GlobalBsc)) {
            continue;
        }
        assessment = {failed[pile] ? EAssessment::Failed : EAssessment::NoFailureReported, true, {}};
    }
    return result;
}

TString ValidateObservation(
    const TObservation& o,
    const TEpoch& epoch,
    const TString& processId,
    const TProfileQualification& q,
    TInstant now,
    TInstant roundDeadline,
    const TEvidenceTiming& timing)
{
    if (!IsQualified(q) || o.Profile != q.Profile || o.Source != q.Source) {
        return "UNQUALIFIED_PROFILE";
    }
    if (o.ProcessId != processId || o.Epoch != epoch) {
        return "STALE_READ_EPOCH";
    }
    if (!o.IdentityVerified || !o.NodeId || !o.ResponderPile || o.RequestId.empty() || o.ProcessId.empty()) {
        return "INVALID_RESPONDER_IDENTITY";
    }
    if (o.BeforeFingerprint != epoch.AdminFingerprint || o.AfterFingerprint != epoch.AdminFingerprint) {
        return "ADMIN_BRACKET_MISMATCH";
    }
    if (o.BeforeAt > o.HealthStartedAt || o.HealthStartedAt - o.BeforeAt > timing.BeforeMaxAge
        || o.FinishedAt < o.HealthStartedAt || o.FinishedAt > roundDeadline || o.FinishedAt > now
        || now < o.HealthStartedAt || now - o.HealthStartedAt >= timing.ObservationTtl)
    {
        return "OBSERVATION_EXPIRED_OR_INVALID_TIME";
    }
    if (q.Source != ESourceKind::GlobalBsc && (!o.DirectLocalOrigin || o.Origin.empty() || o.Vantage.empty())) {
        return "UNVERIFIED_LOCAL_ORIGIN";
    }
    return {};
}

TPileReport AggregatePile(
    const TFrozenSample& sample,
    const TClusterView& view,
    const std::vector<const TObservation*>& observations,
    const TProfileQualification& qualification,
    TInstant now,
    const TEvidenceTiming& timing)
{
    TPileReport report;
    report.Sample = sample;
    const std::set<ui32> frozen(sample.Nodes.begin(), sample.Nodes.end());
    std::map<ui32, const TObservation*> fresh;
    for (const auto* observation : observations) {
        if (!frozen.contains(observation->NodeId) || !observation->ResponderPile || *observation->ResponderPile != sample.Pile) {
            continue;
        }
        ++report.Received;
        if (observation->HealthStartedAt <= now && now - observation->HealthStartedAt < timing.ObservationTtl) {
            if (!fresh.emplace(observation->NodeId, observation).second) {
                report.Reasons.push_back("DUPLICATE_RESPONDER_ID");
                return report;
            }
        }
    }
    report.Fresh = fresh.size();
    for (const auto node : sample.Nodes) {
        if (!fresh.contains(node)) {
            report.Missing.push_back(node);
        }
    }
    if (frozen.empty() || frozen.size() != sample.Nodes.size() || sample.Threshold != frozen.size() / 2 + 1) {
        report.Reasons.push_back("INVALID_FROZEN_SAMPLE");
        return report;
    }
    const auto pile = view.Piles.find(sample.Pile);
    report.Eligible = IsQualified(qualification) && pile != view.Piles.end() && IsSynchronized(pile->second) && fresh.size() >= sample.Threshold;
    std::set<TString> origins;
    for (const auto& [node, observation] : fresh) {
        (void)node;
        origins.insert(observation->Origin);
    }
    if (qualification.Source != ESourceKind::GlobalBsc) {
        report.Eligible = report.Eligible && !origins.empty() && !origins.contains(TString());
        report.Origins = origins;
        if (origins.size() == 1) {
            report.Origin = *origins.begin();
        }
    }
    report.AdminFingerprint = CanonicalFingerprint(view);
    for (const auto& [target, state] : view.Piles) {
        (void)state;
        size_t failed = 0;
        size_t good = 0;
        for (const auto& [node, observation] : fresh) {
            (void)node;
            const auto found = observation->Targets.find(target);
            if (found != observation->Targets.end() && found->second.Complete) {
                failed += found->second.Value == EAssessment::Failed;
                good += found->second.Value == EAssessment::NoFailureReported;
            }
        }
        auto& assessment = report.Targets[target];
        if (failed >= sample.Threshold || good >= sample.Threshold) {
            assessment.Value = failed >= sample.Threshold ? EAssessment::Failed : EAssessment::NoFailureReported;
            assessment.Complete = true;
            report.TargetNumerators[target] = assessment.Value == EAssessment::Failed ? failed : good;
        } else {
            assessment.Reasons.push_back("NO_FIXED_SAMPLE_MAJORITY");
        }
    }
    return report;
}

TAdminSelection SelectAdminView(
    const std::vector<TFrozenSample>& samples,
    const std::vector<TAdminReply>& replies,
    const std::set<TString>& confirmedMembership,
    ui64 durableHighWater,
    TInstant now,
    TDuration ttl)
{
    TAdminSelection result;
    std::map<ui32, const TClusterView*> byNode;
    std::map<ui64, std::set<TString>> states;
    std::map<ui64, std::set<TString>> full;
    ui64 highest = durableHighWater;
    for (const auto& reply : replies) {
        if (!reply.NodeId || reply.View.ReceivedAt > now || now - reply.View.ReceivedAt >= ttl) {
            continue;
        }
        std::set<TString> names;
        for (const auto& [name, state] : reply.View.Piles) {
            (void)state;
            names.insert(name);
        }
        if (names != confirmedMembership) {
            return {std::nullopt, "MEMBERSHIP_RECONCILIATION_REQUIRED"};
        }
        if (!ValidateClusterView(reply.View).empty()) {
            return {std::nullopt, "INVALID_ADMIN_VIEW"};
        }
        if (!byNode.emplace(reply.NodeId, &reply.View).second) {
            return {std::nullopt, "DUPLICATE_RESPONDER_ID"};
        }
        highest = std::max(highest, reply.View.Generation);
        states[reply.View.Generation].insert(CanonicalNamedState(reply.View));
        full[reply.View.Generation].insert(CanonicalFingerprint(reply.View));
    }
    for (const auto& [generation, variants] : states) {
        if (variants.size() > 1) {
            return {std::nullopt, "STATE_CONFLICT"};
        }
        if (full[generation].size() > 1) {
            return {std::nullopt, "LIFECYCLE_METADATA_RESAMPLE"};
        }
    }
    for (const auto& sample : samples) {
        if (sample.Nodes.empty() || sample.Threshold != sample.Nodes.size() / 2 + 1) {
            continue;
        }
        std::map<TString, std::vector<const TClusterView*>> groups;
        std::set<ui32> seen;
        for (const auto node : sample.Nodes) {
            const auto found = byNode.find(node);
            if (found != byNode.end() && seen.insert(node).second && found->second->Generation >= durableHighWater) {
                groups[CanonicalFingerprint(*found->second)].push_back(found->second);
            }
        }
        for (const auto& [fingerprint, group] : groups) {
            (void)fingerprint;
            if (group.size() >= sample.Threshold) {
                std::vector<TInstant> received;
                for (const auto* view : group) {
                    received.push_back(view->ReceivedAt);
                }
                std::sort(received.begin(), received.end(), std::greater<TInstant>());
                const auto quorumAt = received[sample.Threshold - 1];
                if (!result.View || result.View->Generation < group.front()->Generation
                    || (result.View->Generation == group.front()->Generation && result.View->ReceivedAt < quorumAt))
                {
                    result.View = *group.front();
                    // This view is justified by the newest necessary q, never
                    // by the freshest single responder or a recomputation time.
                    result.View->ReceivedAt = quorumAt;
                }
            }
        }
    }
    if (!result.View) {
        result.Blocker = "NO_CORROBORATED_ADMIN_VIEW";
    } else if (result.View->Generation < highest) {
        result.Blocker = "UNCORROBORATED_HIGHER_GENERATION";
    }
    return result;
}

bool TConfirmation::CompleteRound(TDecision& decision, TInstant now, const TEvidenceTiming& timing) {
    const auto key = CanonicalDecision(decision);
    if (decision.Kind != EDecisionKind::Failover || decision.RoundStartedAt > now
        || now + timing.DispatchMargin > decision.EvidenceExpiry)
    {
        Reset();
        decision.Confirmed = false;
        return false;
    }
    if (decision.RoundId == RoundId && !Key.empty()) {
        decision.Confirmed = false;
        return false;
    }
    decision.Confirmed = !Key.empty() && Key == key && decision.RoundId == RoundId + 1
        && decision.RoundStartedAt >= StartedAt && decision.RoundStartedAt - StartedAt <= timing.ConfirmationStartGap;
    Key = key;
    RoundId = decision.RoundId;
    StartedAt = decision.RoundStartedAt;
    return decision.Confirmed;
}

void TConfirmation::Reset() {
    Key.clear();
    RoundId = 0;
    StartedAt = {};
}

} // namespace NBridgeSkipper
