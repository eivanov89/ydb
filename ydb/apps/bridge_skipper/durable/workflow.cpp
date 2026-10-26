#include "durable.h"

#include <util/string/cast.h>

namespace NBridgeSkipper {
namespace {

std::optional<TString> Primary(const TClusterView& view) {
    for (const auto& [name, state] : view.Piles) {
        if (state == EPileState::Primary) {
            return name;
        }
    }
    return std::nullopt;
}

TString RoleMapError(const TClusterView& view, const TExactUpdate& update) {
    auto result = view;
    for (const auto& [name, state] : update) {
        result.Piles[name] = state;
    }
    size_t primaryCount = 0;
    for (const auto& [name, state] : result.Piles) {
        Y_UNUSED(name);
        if (state == EPileState::Primary) {
            ++primaryCount;
        }
    }
    return primaryCount == 1 ? TString() : TString("EXACTLY_ONE_PRIMARY_REQUIRED");
}

} // namespace

TPlanResult PlanWorkflow(const TClusterView& view, const TPlanRequest& request) {
    auto fail = [](TString error) {
        return TPlanResult{std::nullopt, std::move(error), {}};
    };
    if (request.Automatic && request.Kind != EOperationKind::Failover) {
        return fail("AUTOMATIC_WORKFLOW_UNSUPPORTED");
    }
    TWorkflowPlan plan;
    plan.Kind = request.Kind;
    plan.Target = request.Target;
    plan.Successor = request.Successor;
    plan.FailedTargets = request.FailedTargets;
    plan.Automatic = request.Automatic;
    plan.PrerequisitesAcknowledged = request.ExternalPrerequisitesAcknowledged;
    std::vector<TString> acknowledgements;
    if (!request.ExternalPrerequisitesAcknowledged) {
        acknowledgements.push_back("EXTERNAL_PREREQUISITES_REQUIRED");
    }
    const auto primary = Primary(view);
    auto stateOf = [&](const std::optional<TString>& pile) {
        if (!pile || !view.Piles.contains(*pile)) {
            return EPileState::Unspecified;
        }
        return view.Piles.at(*pile);
    };
    auto addStep = [&](EBridgeMethod method, TExactUpdate update, std::optional<TString> target) {
        TWorkflowStep step;
        step.Method = method;
        step.Update = std::move(update);
        step.Target = std::move(target);
        step.Baseline = view;
        step.ExpectedFingerprint = CanonicalFingerprint(view);
        plan.Steps.push_back(std::move(step));
    };
    if (request.Kind == EOperationKind::Convert) {
        if (view.Converted || view.ConversionInProgress || !view.Pending.empty() || view.Piles.size() != 1) {
            return fail("CONVERT_REQUIRES_ATTESTED_SINGLE_PILE_PREBRIDGE");
        }
        addStep(EBridgeMethod::Convert, {}, view.Piles.begin()->first);
    } else {
        const auto error = ValidateClusterView(view);
        if (!error.empty() || !view.Converted) {
            return fail("COHERENT_BRIDGE_STATE_REQUIRED");
        }
        if (view.ConversionInProgress) {
            return fail("CONVERSION_PENDING");
        }
        switch (request.Kind) {
            case EOperationKind::Failover: {
                if (request.FailedTargets.empty()) {
                    return fail("NONEMPTY_TARGETS_REQUIRED");
                }
                TExactUpdate update;
                for (const auto& target : request.FailedTargets) {
                    if (!view.Piles.contains(target)) {
                        return fail("UNKNOWN_TARGET");
                    }
                    const auto state = view.Piles.at(target);
                    if (state == EPileState::NotSynchronized && request.Automatic && !request.ResynchronizingTargetQualified) {
                        return fail("RESYNCHRONIZING_TARGET_UNSUPPORTED");
                    }
                    if (!IsSynchronized(state) && state != EPileState::NotSynchronized) {
                        return fail(state == EPileState::Suspended ? "SUSPENDED_TARGET_UNSUPPORTED" : "TARGET_NOT_MUTABLE");
                    }
                    update[target] = EPileState::Disconnected;
                }
                if (primary && request.FailedTargets.contains(*primary)) {
                    if (!request.Successor || request.FailedTargets.contains(*request.Successor) || !IsSynchronized(stateOf(request.Successor))) {
                        return fail("SYNCHRONIZED_SUCCESSOR_REQUIRED");
                    }
                    update[*request.Successor] = EPileState::Primary;
                } else if (request.Successor && request.Successor != primary) {
                    return fail("PRIMARY_CHANGE_REQUIRES_FAILED_PRIMARY");
                }
                if (primary && request.FailedTargets.contains(*primary)) {
                    for (const auto& [name, state] : view.Piles) {
                        if (state == EPileState::Promoted && !update.contains(name)) {
                            update[name] = EPileState::Synchronized;
                        }
                    }
                }
                for (const auto& pending : view.Pending) {
                    if (!pending.Pile || update.contains(*pending.Pile)) {
                        return fail("PENDING_TARGET_GUARD");
                    }
                }
                const auto roleError = RoleMapError(view, update);
                if (!roleError.empty()) {
                    return fail(roleError);
                }
                addStep(EBridgeMethod::UpdateClusterState, std::move(update), std::nullopt);
                break;
            }
            case EOperationKind::Switchover:
            case EOperationKind::PrimaryMaintenance: {
                if (request.Kind == EOperationKind::PrimaryMaintenance && request.Target != primary) {
                    return fail("MAINTENANCE_TARGET_MUST_BE_CURRENT_PRIMARY");
                }
                if (!view.Pending.empty() || !primary || !request.Successor || *request.Successor == *primary || stateOf(request.Successor) != EPileState::Synchronized) {
                    return fail("DISTINCT_SYNCHRONIZED_SUCCESSOR_REQUIRED");
                }
                TExactUpdate update{{*request.Successor, EPileState::Promoted}};
                for (const auto& [name, state] : view.Piles) {
                    if (state == EPileState::Promoted) {
                        update[name] = EPileState::Synchronized;
                    }
                }
                addStep(EBridgeMethod::UpdateClusterState, std::move(update), request.Successor);
                if (request.Kind == EOperationKind::PrimaryMaintenance) {
                    plan.Target = primary;
                    addStep(EBridgeMethod::UpdateClusterState, {{*primary, EPileState::Suspended}}, primary);
                    // Its baseline/fingerprint is rebound only after the first
                    // observed postcondition and fresh same-node preflight.
                    plan.Steps.back().ExpectedFingerprint.clear();
                }
                break;
            }
            case EOperationKind::Takedown:
                if (!view.Pending.empty() || !request.Target || stateOf(request.Target) != EPileState::Synchronized) {
                    return fail("TAKEDOWN_REQUIRES_SYNCHRONIZED_NONPRIMARY");
                }
                addStep(EBridgeMethod::UpdateClusterState, {{*request.Target, EPileState::Suspended}}, request.Target);
                break;
            case EOperationKind::Rejoin:
                if (!view.Pending.empty() || !request.Target || stateOf(request.Target) != EPileState::Disconnected) {
                    return fail("REJOIN_REQUIRES_DISCONNECTED");
                }
                addStep(EBridgeMethod::UpdateClusterState, {{*request.Target, EPileState::NotSynchronized}}, request.Target);
                break;
            case EOperationKind::AddPile:
            case EOperationKind::RemovePile: {
                if (!view.Pending.empty() || !request.Target) {
                    return fail("MEMBERSHIP_OPERATION_PENDING_OR_TARGET_MISSING");
                }
                const bool add = request.Kind == EOperationKind::AddPile;
                if (stateOf(request.Target) != (add ? EPileState::Inactive : EPileState::Disconnected)) {
                    return fail("LIFECYCLE_TARGET_STATE_INVALID");
                }
                if (!add && (!request.DeletionAcknowledged || request.TypedPileName != request.Target || (request.Target->empty() && !request.EmptyNameAcknowledged))) {
                    plan.PrerequisitesAcknowledged = false;
                    acknowledgements.push_back("EXACT_DELETION_ACKNOWLEDGEMENT_REQUIRED");
                }
                addStep(add ? EBridgeMethod::AddPile : EBridgeMethod::RemovePile, {}, request.Target);
                break;
            }
            case EOperationKind::Convert:
                break;
        }
    }
    TString immutable;
    for (const auto& step : plan.Steps) {
        TMutationAttempt attempt;
        attempt.Method = step.Method;
        attempt.Update = step.Update;
        attempt.Target = step.Target;
        attempt.Baseline = step.Baseline;
        immutable += MutationArgumentsHash(attempt);
    }
    plan.ImmutableArguments = DurableHash(immutable);
    return {std::move(plan), {}, std::move(acknowledgements)};
}

} // namespace NBridgeSkipper
