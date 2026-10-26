#include "policy.h"
#include "scheduler.h"

#include <ydb/apps/bridge_skipper/domain/clock.h>

#include <algorithm>

namespace NBridgeSkipper {
namespace {

EAssessment Assessment(const std::map<TString, TAssessment>& targets, const TString& name) {
    const auto found = targets.find(name);
    return found == targets.end() || !found->second.Complete ? EAssessment::Unknown : found->second.Value;
}

bool NodeEligible(const TNode& node, const TString& pile, const TPolicyInput& input, TInstant now) {
    return node.Pile && *node.Pile == pile && node.NodeId && node.DirectEndpoint && !node.Endpoint.empty()
        && (node.Binding == EBinding::Explicit || node.Binding == EBinding::ImplicitVerified)
        && node.AdminFingerprint == input.Epoch.AdminFingerprint && node.AdminObservedAt <= now
        && now - node.AdminObservedAt < input.Timing.ObservationTtl;
}

} // namespace

TDecision EvaluatePolicy(const TPolicyInput& input) {
    const auto now = TSkipperClock::Now();
    TDecision decision;
    decision.Id = input.DecisionId;
    decision.Epoch = input.Epoch;
    decision.Profile = input.Qualification.Profile;
    decision.Source = input.Qualification.Source;
    decision.RoundId = input.RoundId;
    decision.RoundStartedAt = input.RoundStartedAt;
    decision.EvidenceExpiry = TInstant::Max();
    const auto block = [&](const TString& reason) {
        decision.Kind = EDecisionKind::Blocked;
        decision.Reason = reason;
        decision.Update.clear();
        return decision;
    };
    if (const auto error = ValidateClusterView(input.View); !error.empty()) {
        return block(error);
    }
    if (CanonicalFingerprint(input.View) != input.Epoch.AdminFingerprint) {
        return block("STALE_ADMIN_EPOCH");
    }
    if (!IsQualified(input.Qualification)) {
        return block("UNQUALIFIED_EVIDENCE_PROFILE");
    }
    if (input.RoundStartedAt > now || input.RoundDeadline < input.RoundStartedAt) {
        return block("INVALID_ROUND_TIME");
    }
    for (const auto& [name, state] : input.View.Piles) {
        if (state == EPileState::Primary) {
            decision.Primary = name;
        }
    }
    if (!decision.Primary) {
        return block("NO_PRIMARY");
    }
    std::vector<const TObservation*> accepted;
    const auto inventory = VerifiedInventory(input.Nodes);
    std::set<ui32> identities;
    for (const auto& observation : input.Observations) {
        if (observation.RoundId != input.RoundId || !ValidateObservation(observation, input.Epoch, input.ProcessId,
            input.Qualification, now, input.RoundDeadline, input.Timing).empty())
        {
            continue;
        }
        bool sampled = false;
        for (const auto& sample : input.Samples) {
            sampled = sampled || (sample.Pile == *observation.ResponderPile
                && std::find(sample.Nodes.begin(), sample.Nodes.end(), observation.NodeId) != sample.Nodes.end());
        }
        if (sampled) {
            if (!identities.insert(observation.NodeId).second) {
                return block("DUPLICATE_RESPONDER_ID");
            }
            accepted.push_back(&observation);
        }
    }
    std::map<TString, TPileReport> reports;
    for (const auto& sample : input.Samples) {
        if (!reports.emplace(sample.Pile, AggregatePile(sample, input.View, accepted, input.Qualification, now, input.Timing)).second) {
            return block("DUPLICATE_PILE_SAMPLE");
        }
    }
    std::map<TString, TAssessment> global;
    std::set<TString> failed;
    std::vector<const TObservation*> justification;
    std::vector<const TPileReport*> reporters;
    if (input.Qualification.Source == ESourceKind::GlobalBsc) {
        std::map<TString, EAssessment> determinate;
        for (const auto* observation : accepted) {
            for (const auto& [target, assessment] : observation->Targets) {
                if (!input.View.Piles.contains(target) || !assessment.Complete || assessment.Value == EAssessment::Unknown) {
                    continue;
                }
                const auto [it, inserted] = determinate.emplace(target, assessment.Value);
                if (!inserted && it->second != assessment.Value) {
                    return block("CONTRADICTORY_GLOBAL_ASSESSMENT");
                }
                global[target] = assessment;
                if (assessment.Value == EAssessment::Failed && IsActive(input.View.Piles.at(target))) {
                    failed.insert(target);
                }
            }
        }
        if (accepted.empty()) {
            return block("NO_REPORTERS");
        }
    } else {
        std::map<TString, size_t> origins;
        for (const auto& [pile, report] : reports) {
            (void)pile;
            if (report.Eligible) {
                for (const auto& origin : report.Origins) {
                    ++origins[origin];
                }
            }
        }
        for (const auto& [pile, report] : reports) {
            (void)pile;
            const bool independent = std::all_of(report.Origins.begin(), report.Origins.end(), [&](const auto& origin) {
                return origins[origin] == 1;
            });
            if (report.Eligible && independent) {
                reporters.push_back(&report);
            }
        }
        if (reporters.empty()) {
            return block("NO_REPORTERS");
        }
        for (const auto& [target, state] : input.View.Piles) {
            size_t votes = 0;
            for (const auto* reporter : reporters) {
                votes += Assessment(reporter->Targets, target) == EAssessment::Failed;
            }
            if (IsActive(state) && votes > reporters.size() / 2) {
                failed.insert(target);
            }
        }
    }

    bool mutualPreference = false;
    if (input.Qualification.Source != ESourceKind::GlobalBsc && reporters.size() == 2) {
        size_t active = 0;
        for (const auto& [name, state] : input.View.Piles) {
            (void)name;
            active += IsActive(state);
        }
        const auto* a = reporters[0];
        const auto* b = reporters[1];
        mutualPreference = active == 2 && (a->Sample.Pile == *decision.Primary || b->Sample.Pile == *decision.Primary)
            && Assessment(a->Targets, a->Sample.Pile) == EAssessment::NoFailureReported
            && Assessment(b->Targets, b->Sample.Pile) == EAssessment::NoFailureReported
            && Assessment(a->Targets, b->Sample.Pile) == EAssessment::Failed
            && Assessment(b->Targets, a->Sample.Pile) == EAssessment::Failed;
        if (mutualPreference) {
            decision.Candidate = decision.Primary;
            failed = {a->Sample.Pile == *decision.Primary ? b->Sample.Pile : a->Sample.Pile};
        }
    }

    const auto candidateHealthy = [&](const TString& candidate) {
        if (failed.contains(candidate)) {
            return false;
        }
        if (input.Qualification.Source == ESourceKind::GlobalBsc) {
            return Assessment(global, candidate) == EAssessment::NoFailureReported;
        }
        for (const auto* reporter : reporters) {
            if (reporter->Sample.Pile == candidate) {
                return Assessment(reporter->Targets, candidate) == EAssessment::NoFailureReported;
            }
        }
        return false;
    };
    const auto completeGlobalVector = [&](const TString& candidate) {
        std::vector<const TObservation*> best;
        for (const auto& sample : input.Samples) {
            if (sample.Nodes.empty() || sample.Threshold != sample.Nodes.size() / 2 + 1) {
                continue;
            }
            std::vector<const TObservation*> complete;
            for (const auto* observation : accepted) {
                if (*observation->ResponderPile != sample.Pile
                    || Assessment(observation->Targets, candidate) != EAssessment::NoFailureReported)
                {
                    continue;
                }
                bool covers = true;
                for (const auto& target : failed) {
                    covers = covers && Assessment(observation->Targets, target) == EAssessment::Failed;
                }
                if (covers) {
                    complete.push_back(observation);
                }
            }
            if (complete.size() >= sample.Threshold) {
                std::sort(complete.begin(), complete.end(), [](const auto* a, const auto* b) {
                    return a->HealthStartedAt != b->HealthStartedAt ? a->HealthStartedAt > b->HealthStartedAt : a->NodeId < b->NodeId;
                });
                complete.resize(sample.Threshold);
                if (best.empty() || complete.back()->HealthStartedAt > best.back()->HealthStartedAt) {
                    best = std::move(complete);
                }
            }
        }
        return best;
    };
    std::vector<TString> candidates;
    candidates.push_back(*decision.Primary);
    for (const auto& [name, state] : input.View.Piles) {
        if (name != *decision.Primary && IsSynchronized(state)) {
            candidates.push_back(name);
        }
    }
    for (const auto& candidate : candidates) {
        if (decision.Candidate || !candidateHealthy(candidate)) {
            continue;
        }
        const bool endpoint = std::any_of(inventory.begin(), inventory.end(), [&](const auto& node) {
            return NodeEligible(node, candidate, input, now);
        });
        if (endpoint) {
            if (input.Qualification.Source == ESourceKind::GlobalBsc) {
                auto vector = completeGlobalVector(candidate);
                if (vector.empty()) {
                    continue;
                }
                justification = std::move(vector);
            }
            decision.Candidate = candidate;
        }
    }
    if (!decision.Candidate) {
        if (input.Qualification.Source == ESourceKind::GlobalBsc && std::any_of(candidates.begin(), candidates.end(), candidateHealthy)) {
            return block("NO_COMPLETE_CORROBORATED_GLOBAL_VECTOR");
        }
        return block("NO_ELIGIBLE_CANDIDATE");
    }
    if (input.Qualification.Source == ESourceKind::GlobalBsc) {
        // One fixed sample must provide the complete candidate + all-target
        // vector. Per-target majorities cannot be stitched together.
        decision.Targets = failed;
        if (!justification.empty()) {
            decision.GlobalAssessment[*decision.Candidate] = justification.front()->Targets.at(*decision.Candidate);
            for (const auto& target : failed) {
                decision.GlobalAssessment[target] = justification.front()->Targets.at(target);
            }
        }
    } else {
        const auto& candidateReport = reports.at(*decision.Candidate);
        for (const auto& target : failed) {
            if (mutualPreference || Assessment(candidateReport.Targets, target) == EAssessment::Failed) {
                decision.Targets.insert(target);
            }
        }
        std::set<TString> included;
        const auto matching = [&](const TPileReport& reporter, const std::optional<TString>& target, EAssessment value) {
            std::vector<const TObservation*> selected;
            for (const auto* observation : accepted) {
                if (*observation->ResponderPile == reporter.Sample.Pile
                    && (!target || Assessment(observation->Targets, *target) == value))
                {
                    selected.push_back(observation);
                }
            }
            std::sort(selected.begin(), selected.end(), [](const auto* a, const auto* b) {
                return a->HealthStartedAt != b->HealthStartedAt ? a->HealthStartedAt > b->HealthStartedAt : a->NodeId < b->NodeId;
            });
            selected.resize(std::min(selected.size(), reporter.Sample.Threshold));
            return selected;
        };
        const auto include = [&](const std::vector<const TObservation*>& selected) {
            for (const auto* observation : selected) {
                if (included.insert(observation->RequestId).second) {
                    justification.push_back(observation);
                }
            }
        };
        // Every reporter remains in the frozen R denominator, including
        // UNKNOWN rows. Preserve its freshest q eligibility observations.
        for (const auto* reporter : reporters) {
            include(matching(*reporter, std::nullopt, EAssessment::Unknown));
        }
        include(matching(candidateReport, decision.Candidate, EAssessment::NoFailureReported));
        if (mutualPreference) {
            for (const auto* reporter : reporters) {
                include(matching(*reporter, reporter->Sample.Pile, EAssessment::NoFailureReported));
                const auto& other = reporter->Sample.Pile == reporters.front()->Sample.Pile ? reporters.back()->Sample.Pile : reporters.front()->Sample.Pile;
                include(matching(*reporter, other, EAssessment::Failed));
            }
        } else {
            for (const auto& target : decision.Targets) {
                include(matching(candidateReport, target, EAssessment::Failed));
                std::vector<std::vector<const TObservation*>> votes;
                for (const auto* reporter : reporters) {
                    if (Assessment(reporter->Targets, target) == EAssessment::Failed) {
                        auto selected = matching(*reporter, target, EAssessment::Failed);
                        if (selected.size() == reporter->Sample.Threshold) {
                            votes.push_back(std::move(selected));
                        }
                    }
                }
                std::sort(votes.begin(), votes.end(), [](const auto& a, const auto& b) {
                    return a.back()->HealthStartedAt != b.back()->HealthStartedAt ? a.back()->HealthStartedAt > b.back()->HealthStartedAt
                        : a.front()->NodeId < b.front()->NodeId;
                });
                if (votes.size() <= reporters.size() / 2) {
                    return block("INSUFFICIENT_FRESH_LOCAL_JUSTIFICATION");
                }
                for (size_t index = 0; index <= reporters.size() / 2; ++index) {
                    include(votes[index]);
                }
            }
        }
    }
    for (const auto& target : decision.Targets) {
        if (input.View.Piles.at(target) == EPileState::NotSynchronized
            && (!input.Qualification.CoversResynchronizing || input.Qualification.Source == ESourceKind::GlobalBsc))
        {
            return block("RESYNC_FAILURE_EVIDENCE_UNSUPPORTED");
        }
    }
    if (decision.Targets.empty()) {
        decision.Kind = EDecisionKind::Stable;
        decision.Reason = "NO_APPROVED_TARGETS";
        return decision;
    }
    if (!input.PlacementAcknowledged && (input.Qualification.Source == ESourceKind::GlobalBsc || reporters.size() == 1)) {
        return block("PLACEMENT_ACKNOWLEDGEMENT_REQUIRED");
    }
    for (const auto& target : decision.Targets) {
        decision.Update[target] = EPileState::Disconnected;
    }
    if (decision.Targets.contains(*decision.Primary)) {
        decision.Update[*decision.Candidate] = EPileState::Primary;
        for (const auto& [name, state] : input.View.Piles) {
            if (state == EPileState::Promoted && name != *decision.Candidate && !decision.Targets.contains(name)) {
                decision.Update[name] = EPileState::Synchronized;
            }
        }
    }
    if (const auto error = ValidateExactUpdate(input.View, decision.Update); !error.empty()) {
        return block(error);
    }
    for (const auto& node : inventory) {
        if (NodeEligible(node, *decision.Candidate, input, now) && (!decision.Endpoint || node.NodeId < decision.Endpoint->NodeId)) {
            decision.Endpoint = node;
        }
    }
    if (!decision.Endpoint) {
        return block("NO_DIRECT_CANDIDATE_ENDPOINT");
    }
    for (const auto* observation : justification) {
        decision.EvidenceExpiry = std::min(decision.EvidenceExpiry, observation->HealthStartedAt + input.Timing.ObservationTtl);
        decision.EvidenceRequestIds.push_back(observation->RequestId);
        decision.Evidence.push_back(*observation);
    }
    std::sort(decision.EvidenceRequestIds.begin(), decision.EvidenceRequestIds.end());
    if (now + input.Timing.DispatchMargin > decision.EvidenceExpiry) {
        return block("HANDOFF_EVIDENCE_EXPIRED");
    }
    decision.Kind = EDecisionKind::Failover;
    decision.Reason = mutualPreference ? "TWO_ACTIVE_LOCAL_PRIMARY_PREFERENCE" : "QUALIFIED_FAILURE_EVIDENCE";
    return decision;
}

TString ValidatePreflight(
    const TDecision& decision,
    const TClusterView& directView,
    const TNode& directNode,
    const TDispatchReadiness& readiness,
    TInstant now,
    TDuration dispatchMargin)
{
    if (decision.Kind != EDecisionKind::Failover || !decision.Confirmed) {
        return "UNCONFIRMED_DECISION";
    }
    if (!readiness.Auto || !readiness.AuthorityReady || !readiness.OwnershipAcknowledged || !readiness.DurableStorage
        || readiness.UnresolvedAttempt || readiness.LocalStop || !readiness.BackgroundGuardsPermit)
    {
        return "DISPATCH_NOT_READY";
    }
    if (readiness.Epoch != decision.Epoch || CanonicalFingerprint(directView) != decision.Epoch.AdminFingerprint) {
        return "PREFLIGHT_FINGERPRINT_MISMATCH";
    }
    if (!decision.Endpoint || !directNode.DirectEndpoint || !directNode.NodeId
        || directNode.NodeId != decision.Endpoint->NodeId || directNode.Endpoint != decision.Endpoint->Endpoint
        || directNode.Revision != decision.Endpoint->Revision || directNode.Pile != decision.Candidate
        || directView.Endpoint != directNode.Endpoint || directNode.Binding == EBinding::Conflict
        || directNode.Binding == EBinding::ImplicitUnresolved)
    {
        return "PREFLIGHT_ENDPOINT_REPLACED";
    }
    if (directView.ReceivedAt > now || directView.ReceivedAt < decision.RoundStartedAt) {
        return "STALE_PREFLIGHT";
    }
    if (now + dispatchMargin > decision.EvidenceExpiry) {
        return "HANDOFF_EVIDENCE_EXPIRED";
    }
    return ValidateExactUpdate(directView, decision.Update);
}

} // namespace NBridgeSkipper
