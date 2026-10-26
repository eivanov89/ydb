#include "durable.h"

#include <algorithm>
#include <limits>

namespace NBridgeSkipper {
namespace {

bool PendingMatches(const TClusterView& view, EBridgeMethod method, const std::optional<TString>& target) {
    const TString kind = method == EBridgeMethod::AddPile ? "ADD" : "REMOVE";
    for (const auto& pending : view.Pending) {
        if (pending.Kind == kind && pending.Pile == target) {
            return true;
        }
    }
    return false;
}

bool StateIs(const TClusterView& view, const std::optional<TString>& target, EPileState state) {
    return target && view.Piles.contains(*target) && view.Piles.at(*target) == state;
}

bool MatchesUpdate(const TClusterView& view, const TExactUpdate& update) {
    for (const auto& [name, state] : update) {
        if (!StateIs(view, name, state)) {
            return false;
        }
    }
    return true;
}

bool BackgroundDone(const TOperation& operation, const TBackgroundObligation& background, const TClusterView& view, bool healthy) {
    switch (background.Kind) {
        case EOperationKind::Failover:
            return MatchesUpdate(view, background.IntendedUpdate);
        case EOperationKind::Switchover:
            return StateIs(view, background.Target, EPileState::Primary) && !std::any_of(view.Piles.begin(), view.Piles.end(), [](const auto& item) {
                return item.second == EPileState::Promoted;
            });
        case EOperationKind::PrimaryMaintenance:
            if (background.IntendedUpdate.contains(background.Target.value_or(TString())) && background.IntendedUpdate.at(*background.Target) == EPileState::Suspended) {
                return StateIs(view, background.Target, EPileState::Disconnected);
            }
            return StateIs(view, operation.Plan.Successor, EPileState::Primary) && StateIs(view, operation.Plan.Target, EPileState::Synchronized) && !std::any_of(view.Piles.begin(), view.Piles.end(), [](const auto& item) {
                return item.second == EPileState::Promoted;
            });
        case EOperationKind::Takedown:
            return StateIs(view, background.Target, EPileState::Disconnected);
        case EOperationKind::Rejoin:
            return StateIs(view, background.Target, EPileState::Synchronized) && healthy;
        case EOperationKind::Convert:
            return view.Converted && !view.ConversionInProgress;
        case EOperationKind::AddPile:
            return view.Pending.empty() && StateIs(view, background.Target, EPileState::Disconnected);
        case EOperationKind::RemovePile:
            return view.Pending.empty() && StateIs(view, background.Target, EPileState::Inactive);
    }
    return false;
}

bool SupersedesBackground(const TMutationAttempt& emergency, const TBackgroundObligation& work, const TClusterView& view) {
    if (!emergency.Accepted || emergency.Method != EBridgeMethod::UpdateClusterState || !work.Target || !emergency.Update.contains(*work.Target) || !MatchesUpdate(view, emergency.Update)) {
        return false;
    }
    const auto replacement = emergency.Update.at(*work.Target);
    if (work.Kind == EOperationKind::Rejoin) {
        return replacement == EPileState::Disconnected;
    }
    if (work.Kind == EOperationKind::Switchover || work.Kind == EOperationKind::PrimaryMaintenance) {
        return work.IntendedUpdate.contains(*work.Target) && work.IntendedUpdate.at(*work.Target) == EPileState::Promoted && replacement != EPileState::Promoted;
    }
    // Membership/conversion work remains an obligation even when a disjoint
    // emergency completes. It cannot be cancelled by an UpdateClusterState.
    return false;
}

TString ConflictGuard(const TDurableState& state, const TWorkflowPlan& plan) {
    for (const auto& [id, operation] : state.Operations) {
        Y_UNUSED(id);
        for (const auto& work : operation.Background) {
            if (work.Complete) {
                continue;
            }
            if (work.Kind == EOperationKind::Convert) {
                return "CONVERSION_PENDING";
            }
            if (work.Kind == EOperationKind::AddPile || work.Kind == EOperationKind::RemovePile) {
                for (const auto& step : plan.Steps) {
                    if (step.Method == EBridgeMethod::AddPile || step.Method == EBridgeMethod::RemovePile || !work.Target || step.Update.contains(*work.Target)) {
                        return "PENDING_TARGET_GUARD";
                    }
                }
            }
            if (work.Kind == EOperationKind::Switchover || work.Kind == EOperationKind::PrimaryMaintenance) {
                if (plan.Kind == EOperationKind::Failover && work.Target) {
                    const auto& update = plan.Steps.front().Update;
                    const bool replacesPrimary = std::any_of(update.begin(), update.end(), [](const auto& item) {
                        return item.second == EPileState::Primary;
                    });
                    if (replacesPrimary && !update.contains(*work.Target)) {
                        return "PROMOTED_ROLE_MUST_BE_EXPLICITLY_SUPERSEDED";
                    }
                }
            }
        }
    }
    return {};
}

void AddBackground(TDurableState& state, TMutationAttempt& attempt) {
    auto& operation = state.Operations.at(attempt.OperationId);
    for (const auto& work : operation.Background) {
        if (work.AttemptId == attempt.Id) {
            return;
        }
    }
    operation.Background.push_back({attempt.Id, operation.Plan.Kind, attempt.Target, attempt.Update, false, false, {}});
    if (!IsTerminal(operation.Status)) {
        // Acceptance advances bookkeeping, not the operator's paused workflow.
        if (operation.Status != EOperationStatus::Paused) {
            operation.Status = EOperationStatus::WaitingBackground;
        }
        ++operation.Revision;
    }
}

void ReleaseSlot(TDurableState& state, const TString& id) {
    if (state.UnresolvedAttempt == id) {
        state.UnresolvedAttempt.reset();
    }
}

} // namespace

bool IsAttemptClosed(const TMutationAttempt& attempt) {
    return attempt.Accepted || attempt.Disposition == EAttemptDisposition::NeverDispatched || attempt.Disposition == EAttemptDisposition::CompletedRejection || attempt.Disposition == EAttemptDisposition::ExternallyResolved;
}

bool IsTerminal(EOperationStatus status) {
    return status == EOperationStatus::Succeeded || status == EOperationStatus::Rejected || status == EOperationStatus::Cancelled || status == EOperationStatus::Preempted || status == EOperationStatus::ExternallyResolved;
}

namespace {

bool NoOpenAttempt(const TDurableState& state, const TOperation& operation) {
    return operation.Status != EOperationStatus::Executing && std::all_of(operation.Attempts.begin(), operation.Attempts.end(), [&](const auto& id) {
        return IsAttemptClosed(state.Attempts.at(id));
    });
}

} // namespace

TString CancelError(const TDurableState& state, const TOperation& operation) {
    if (!NoOpenAttempt(state, operation)) {
        return "ALREADY_DISPATCHED: dispatch may have occurred";
    }
    if (IsTerminal(operation.Status)) {
        return "OPERATION_TERMINAL";
    }
    const bool settled = std::all_of(operation.Background.begin(), operation.Background.end(), [](const auto& work) {
        return work.Complete;
    });
    return settled ? TString() : TString("BACKGROUND_WORK_PENDING");
}

bool HandoverComplete(const TDurableState& state) {
    if (state.UnresolvedAttempt) {
        return false;
    }
    for (const auto& [id, operation] : state.Operations) {
        Y_UNUSED(id);
        for (const auto& work : operation.Background) {
            if (!work.Complete) {
                return false;
            }
        }
    }
    return true;
}

TString ValidateAcceptance(const TDurableState& state, const TMutationAttempt& attempt, const TAcceptanceProof& proof) {
    if (!state.ExclusiveHistory || !proof.CompleteExclusiveHistory || !proof.NoAlternativeProducer || !proof.NoReplayPathQualified || !proof.MarkerQualified || !proof.Fresh || !proof.IdentityVerified || !proof.OrderedAfterSend || !proof.Coherent || proof.Provenance.empty()) {
        return "UNQUALIFIED_ACCEPTANCE_HISTORY";
    }
    if (proof.TopologyRevision != attempt.TopologyRevision || proof.Observed.Generation < attempt.Baseline.Generation || proof.Observed.Generation < state.AcceptedGenerationHighWater) {
        return "STALE_OR_CHANGED_ACCEPTANCE_BINDING";
    }
    if (state.UnresolvedAttempt && *state.UnresolvedAttempt != attempt.Id) {
        return "OLDER_UNPROVEN_SEND";
    }
    if (!ValidateClusterView(proof.Observed).empty()) {
        return "INCOHERENT_ACCEPTANCE_VIEW";
    }
    const auto& before = attempt.Baseline;
    const auto& after = proof.Observed;
    bool marker = false;
    if (attempt.Method == EBridgeMethod::AddPile || attempt.Method == EBridgeMethod::RemovePile) {
        const bool add = attempt.Method == EBridgeMethod::AddPile;
        if (!before.Pending.empty() || !StateIs(before, attempt.Target, add ? EPileState::Inactive : EPileState::Disconnected)) {
            return "LIFECYCLE_BASELINE_NOT_DISTINGUISHING";
        }
        marker = PendingMatches(after, attempt.Method, attempt.Target) || (proof.CertifiedAutomaticFollowup && after.Pending.empty() && StateIs(after, attempt.Target, add ? EPileState::Disconnected : EPileState::Inactive));
    } else if (attempt.Method == EBridgeMethod::Convert) {
        marker = !before.Converted && after.Converted && before.Piles == after.Piles;
    } else {
        if (after.Generation <= before.Generation) {
            return "UPDATE_GENERATION_DID_NOT_ADVANCE";
        }
        auto expected = before;
        bool changed = false;
        for (const auto& [name, wanted] : attempt.Update) {
            if (!before.Piles.contains(name)) {
                return "UNKNOWN_BASELINE_TARGET";
            }
            const auto old = before.Piles.at(name);
            changed = changed || old != wanted;
            auto actual = after.Piles.find(name);
            if (actual == after.Piles.end()) {
                return "MISSING_ACCEPTANCE_TARGET";
            }
            bool legal = actual->second == wanted;
            if (proof.CertifiedAutomaticFollowup) {
                legal = legal || (wanted == EPileState::Promoted && actual->second == EPileState::Primary) || (wanted == EPileState::Suspended && actual->second == EPileState::Disconnected) || (wanted == EPileState::NotSynchronized && actual->second == EPileState::Synchronized);
            }
            if (!legal || (wanted == EPileState::Promoted && old != EPileState::Synchronized) || (wanted == EPileState::Suspended && old != EPileState::Synchronized) || (wanted == EPileState::NotSynchronized && old != EPileState::Disconnected) || (wanted == EPileState::Disconnected && !IsSynchronized(old) && old != EPileState::NotSynchronized)) {
                return "MARKER_NOT_REACHABLE_BY_EXACT_REQUEST";
            }
            expected.Piles[name] = wanted;
        }
        if (!changed || MatchesUpdate(before, attempt.Update)) {
            return "PRIOR_PRESENT_OR_NOOP_MARKER";
        }
        // Unmentioned stored states are immutable except the certified primary
        // role follow-up of this exact promotion.
        for (const auto& [name, old] : before.Piles) {
            if (!attempt.Update.contains(name)) {
                const auto actual = after.Piles.find(name);
                const bool promotionDemotion = proof.CertifiedAutomaticFollowup && old == EPileState::Primary && actual != after.Piles.end() && actual->second == EPileState::Synchronized && std::any_of(attempt.Update.begin(), attempt.Update.end(), [](const auto& item) {
                return item.second == EPileState::Promoted;
            });
                if (actual == after.Piles.end() || (actual->second != old && !promotionDemotion)) {
                    return "UNEXPLAINED_ACCEPTANCE_TRANSITION";
                }
            }
        }
        for (const auto& [name, actual] : after.Piles) {
            if (!before.Piles.contains(name) || (actual == EPileState::Promoted && before.Piles.at(name) != EPileState::Promoted && (!attempt.Update.contains(name) || attempt.Update.at(name) != EPileState::Promoted))) {
                return "UNEXPLAINED_ROLE_OR_MEMBERSHIP";
            }
        }
        marker = true;
    }
    return marker ? TString() : TString("NO_DISTINGUISHING_ACCEPTANCE_MARKER");
}

TApplyResult Apply(const TDurableState& original, const TDurableCommand& command) {
    TApplyResult applied{original, {}, false};
    const auto payloadHash = DurableHash(SerializeCommand(command));
    auto duplicate = [&](const TStoredReceipt& receipt, TStringBuf hash, TStringBuf reused) {
        applied.Duplicate = true;
        applied.Result = receipt.PayloadHash == hash ? receipt.Result : TCommandResult{false, TString(reused), {}, original.AppliedIndex, original.ControllerRevision};
    };
    if (const auto it = original.Commands.find(command.Id); it != original.Commands.end()) {
        duplicate(it->second, payloadHash, "COMMAND_ID_REUSED");
        return applied;
    }
    if (command.RequestId) {
        if (const auto it = original.Requests.find(*command.RequestId); it != original.Requests.end()) {
            duplicate(it->second, command.RequestHash, "REQUEST_ID_REUSED");
            return applied;
        }
    }
    if (command.Id.empty() || command.Schema != 1 || static_cast<ui32>(command.Kind) > static_cast<ui32>(ECommandKind::AdvanceGeneration) || static_cast<ui32>(command.Mode) > static_cast<ui32>(ERequestedMode::Observe) || (command.RequestId && (command.RequestId->empty() || command.RequestHash.empty())) || original.AppliedIndex >= static_cast<ui64>(std::numeric_limits<i64>::max())) {
        applied.Result = {false, "INVALID_COMMAND_OR_INDEX_EXHAUSTED", {}, original.AppliedIndex, original.ControllerRevision};
        return applied;
    }
    auto& state = applied.State;
    auto finish = [&](bool ok, TString code) {
        if (ok && state.RequestedMode == ERequestedMode::Observe && HandoverComplete(state)) {
            state.ExclusiveHistory = false;
        }
        state.AppliedIndex = original.AppliedIndex + 1;
        state.ControllerRevision = original.ControllerRevision + (ok ? 1 : 0);
        applied.Result = {ok, std::move(code), command.OperationId, state.AppliedIndex, state.ControllerRevision};
        for (auto& [operationId, operation] : state.Operations) {
            const auto prior = original.Operations.find(operationId);
            const bool changed = prior == original.Operations.end() || prior->second.Revision != operation.Revision;
            if (operationId == command.OperationId || changed) {
                operation.UpdatedAt = command.AuditTime;
                operation.Audit.push_back({command.Id, state.AppliedIndex, command.AuditTime, applied.Result.Code, command.Reason});
                if (IsTerminal(operation.Status) && operation.CompletedAt.empty()) {
                    operation.CompletedAt = command.AuditTime;
                }
            }
        }
        state.Commands.emplace(command.Id, TStoredReceipt{payloadHash, applied.Result});
        if (command.RequestId) {
            state.Requests.emplace(*command.RequestId, TStoredReceipt{command.RequestHash, applied.Result});
        }
        return applied;
    };
    if ((command.Kind == ECommandKind::SetMode || command.Kind == ECommandKind::AdmitOperation || command.Kind == ECommandKind::Resume || command.Kind == ECommandKind::Cancel || command.Kind == ECommandKind::ResolveExternal) && command.ExpectedControllerRevision != state.ControllerRevision) {
        return finish(false, "STALE_CONTROLLER_REVISION");
    }
    auto dispatchError = [&]() -> TString {
        if (state.RequestedMode == ERequestedMode::Observe) {
            return "OBSERVE_MODE";
        }
        if (!state.Ownership || !state.Ownership->ExclusiveAdministration) {
            return "OWNERSHIP_ACKNOWLEDGEMENT_REQUIRED";
        }
        if (state.StateConflict) {
            return "STATE_CONFLICT";
        }
        if (state.UnresolvedAttempt) {
            return "UNRESOLVED_ATTEMPT";
        }
        if (!command.Ready || command.LocalStopped || !command.Fresh || !command.Coherent) {
            return "ADMISSION_NOT_READY";
        }
        return {};
    };
    switch (command.Kind) {
        case ECommandKind::SetMode: {
            if (command.Reason.empty()) {
                return finish(false, "REASON_REQUIRED");
            }
            if (command.Mode == ERequestedMode::Observe) {
                if (command.Ownership) {
                    return finish(false, "OBSERVE_REQUIRES_NULL_ACKNOWLEDGEMENT");
                }
                state.Ownership.reset();
                // Retain the exclusive interval while admitted work hands over.
                // Observe revokes permission for new work immediately.
                state.ExclusiveHistory = state.ExclusiveHistory && !HandoverComplete(state);
                for (auto& [id, operation] : state.Operations) {
                    Y_UNUSED(id);
                    if (operation.Status == EOperationStatus::Queued && operation.Attempts.empty()) {
                        operation.Status = EOperationStatus::Cancelled;
                        ++operation.Revision;
                    } else if (!IsTerminal(operation.Status)) {
                        operation.Status = EOperationStatus::Paused;
                        ++operation.Revision;
                    }
                }
            } else {
                const bool required = !state.Ownership || state.RequestedMode == ERequestedMode::Observe || (command.Mode == ERequestedMode::Auto && state.RequestedMode != ERequestedMode::Auto);
                if (required && !command.Ownership) {
                    return finish(false, "OWNERSHIP_ACKNOWLEDGEMENT_REQUIRED");
                }
                if (command.Ownership) {
                    if (!command.Ownership->ExclusiveAdministration || command.Ownership->DeploymentPlanReference.empty()) {
                        return finish(false, "INVALID_OWNERSHIP_ACKNOWLEDGEMENT");
                    }
                    state.Ownership = command.Ownership;
                    state.Ownership->ClusterId = state.ClusterId;
                    state.Ownership->CallerId = "local";
                    state.Ownership->Reason = command.Reason;
                    state.Ownership->AuditTime = command.AuditTime;
                    state.Ownership->Revision = state.ControllerRevision + 1;
                    // A new acknowledgement cannot repair an unknown interval.
                    state.ExclusiveHistory = state.ExclusiveHistory || HandoverComplete(state);
                }
            }
            state.RequestedMode = command.Mode;
            return finish(true, "MODE_CHANGED");
        }
        case ECommandKind::AdmitOperation: {
            const auto error = dispatchError();
            if (!error.empty()) {
                return finish(false, error);
            }
            if (command.OperationId.empty() || state.Operations.contains(command.OperationId) || command.Plan.Steps.empty() || static_cast<ui32>(command.Plan.Kind) > static_cast<ui32>(EOperationKind::RemovePile)) {
                return finish(false, "INVALID_OPERATION");
            }
            if (!command.Plan.PrerequisitesAcknowledged) {
                return finish(false, "WORKFLOW_PREREQUISITES_REQUIRED");
            }
            if (command.Plan.Automatic && (state.RequestedMode != ERequestedMode::Auto || command.Plan.Kind != EOperationKind::Failover || state.SuppressedAutomaticPlans.contains(command.Plan.SuppressionKey))) {
                return finish(false, "AUTOMATIC_PLAN_SUPPRESSED_OR_MODE");
            }
            const auto guard = ConflictGuard(state, command.Plan);
            if (!guard.empty()) {
                return finish(false, guard);
            }
            if (state.OrdinaryOwner) {
                auto& old = state.Operations.at(*state.OrdinaryOwner);
                if (!IsTerminal(old.Status)) {
                    if (command.Plan.Kind != EOperationKind::Failover) {
                        return finish(false, "ORDINARY_WORKFLOW_OWNED");
                    }
                    old.Status = EOperationStatus::Preempted;
                    old.PreemptedBy = command.OperationId;
                    ++old.Revision;
                }
            }
            TOperation operation;
            operation.Id = command.OperationId;
            operation.RequestId = command.RequestId.value_or(TString());
            operation.Plan = command.Plan;
            operation.CreatedAt = command.AuditTime;
            operation.CreatedIndex = state.AppliedIndex + 1;
            state.Operations.emplace(operation.Id, operation);
            state.OrdinaryOwner = operation.Id;
            return finish(true, "OPERATION_ADMITTED");
        }
        case ECommandKind::AdmitAttempt: {
            const auto error = dispatchError();
            if (!error.empty()) {
                return finish(false, error);
            }
            auto found = state.Operations.find(command.OperationId);
            if (found == state.Operations.end()) {
                return finish(false, "OPERATION_NOT_FOUND");
            }
            auto& operation = found->second;
            const auto& attempt = command.Attempt;
            if (operation.Status != EOperationStatus::Queued || operation.Revision != command.ExpectedWorkflowRevision || state.OrdinaryOwner != command.OperationId || attempt.Step != operation.NextStep || attempt.Step >= operation.Plan.Steps.size()) {
                return finish(false, "STALE_WORKFLOW_STEP");
            }
            const auto& step = operation.Plan.Steps[attempt.Step];
            if (attempt.Id.empty() || state.Attempts.contains(attempt.Id) || attempt.OperationId != operation.Id || attempt.Method != step.Method || attempt.Update != step.Update || attempt.Target != step.Target || attempt.Endpoint.empty() || attempt.Endpoint != attempt.Baseline.Endpoint || !attempt.NodeId || attempt.DesignatedMember != state.MemberId || attempt.DesignatedProcess.empty() || attempt.ArgumentsHash != MutationArgumentsHash(attempt) || (!step.ExpectedFingerprint.empty() && CanonicalFingerprint(attempt.Baseline) != step.ExpectedFingerprint) || attempt.Baseline.Generation < state.AcceptedGenerationHighWater || !ValidateClusterView(attempt.Baseline).empty()) {
                return finish(false, "IMMUTABLE_ATTEMPT_OR_PREFLIGHT_MISMATCH");
            }
            if (attempt.Method == EBridgeMethod::UpdateClusterState && !ValidateExactUpdate(attempt.Baseline, attempt.Update).empty()) {
                return finish(false, "ILLEGAL_EXACT_UPDATE");
            }
            if (operation.Plan.Kind == EOperationKind::PrimaryMaintenance && attempt.Step == 1 && (!StateIs(attempt.Baseline, operation.Plan.Successor, EPileState::Primary) || !StateIs(attempt.Baseline, operation.Plan.Target, EPileState::Synchronized))) {
                return finish(false, "MAINTENANCE_PRIOR_POSTCONDITION_CHANGED");
            }
            const auto guard = ConflictGuard(state, operation.Plan);
            if (!guard.empty()) {
                return finish(false, guard);
            }
            auto stored = attempt;
            stored.WorkflowRevision = operation.Revision;
            stored.IntentIndex = state.AppliedIndex + 1;
            stored.Disposition = EAttemptDisposition::IntentCommitted;
            stored.RpcDisposition = ERpcDisposition::Pending;
            stored.Accepted = false;
            state.Attempts.emplace(stored.Id, stored);
            state.UnresolvedAttempt = stored.Id;
            operation.Attempts.push_back(stored.Id);
            operation.Status = EOperationStatus::Executing;
            ++operation.Revision;
            return finish(true, "INTENT_COMMITTED");
        }
        case ECommandKind::NeverDispatched: {
            auto found = state.Attempts.find(command.Attempt.Id);
            if (found == state.Attempts.end() || IsAttemptClosed(found->second) || command.GatewayProof.empty() || command.Attempt.DesignatedMember != found->second.DesignatedMember || command.Attempt.DesignatedProcess != found->second.DesignatedProcess) {
                return finish(false, "INVALID_GATEWAY_NEGATIVE_PROOF");
            }
            auto& attempt = found->second;
            attempt.Disposition = EAttemptDisposition::NeverDispatched;
            attempt.Proof = command.GatewayProof;
            ReleaseSlot(state, attempt.Id);
            auto& operation = state.Operations.at(attempt.OperationId);
            if (!IsTerminal(operation.Status)) {
                // Automatic work reschedules under fresh evidence; an operator
                // explicitly resumes manual or Observe-paused work.
                const bool reschedule = operation.Plan.Automatic && operation.Status == EOperationStatus::Executing && state.RequestedMode == ERequestedMode::Auto;
                operation.Status = reschedule ? EOperationStatus::Queued : EOperationStatus::Paused;
            }
            ++operation.Revision;
            return finish(true, "NEVER_DISPATCHED");
        }
        case ECommandKind::RpcOutcome: {
            const auto& outcome = command.Outcome;
            auto found = state.Attempts.find(outcome.AttemptId);
            if (found == state.Attempts.end() || found->second.DesignatedMember != outcome.DesignatedMember || found->second.DesignatedProcess != outcome.DesignatedProcess) {
                return finish(false, "ATTEMPT_CORRELATION_MISMATCH");
            }
            auto& attempt = found->second;
            if (attempt.RpcDisposition != ERpcDisposition::Pending) {
                return finish(false, "RPC_ALREADY_TERMINAL");
            }
            const bool unsupported = attempt.Method == EBridgeMethod::UpdateClusterState && outcome.Status == "INTERNAL_ERROR" && outcome.IssueCode == "DEFAULT_ERROR" && outcome.Issue == "Unsupported pile state";
            const TString prefix = attempt.Method == EBridgeMethod::AddPile ? "Pile is added to the cluster, but its parts of storage groups can't be created (this is retried until it succeeds): " : "Pile is being removed from the cluster, but its parts of storage groups can't be deleted (this is retried until it succeeds): ";
            const bool background = (attempt.Method == EBridgeMethod::AddPile || attempt.Method == EBridgeMethod::RemovePile) && outcome.PinnedLifecycleErrorQualified && outcome.Status == "GENERIC_ERROR" && outcome.IssueCode == "DEFAULT_ERROR" && outcome.Issue.StartsWith(prefix) && outcome.Issue.size() > prefix.size() && outcome.Observed && ValidateClusterView(*outcome.Observed).empty() && outcome.Observed->Generation >= attempt.Baseline.Generation && PendingMatches(*outcome.Observed, attempt.Method, attempt.Target);
            const bool rejection = unsupported || outcome.GenerationMismatchQualified;
            attempt.RpcDisposition = outcome.Success ? ERpcDisposition::Success : (rejection ? ERpcDisposition::Rejection : (background ? ERpcDisposition::Background : ERpcDisposition::Unknown));
            attempt.RpcDetail = outcome.Status + ":" + outcome.IssueCode + ":" + outcome.Issue;
            if ((attempt.Accepted && rejection) || attempt.Disposition == EAttemptDisposition::NeverDispatched) {
                state.StateConflict = true;
                auto& operation = state.Operations.at(attempt.OperationId);
                if (!IsTerminal(operation.Status)) {
                    operation.Status = EOperationStatus::NeedsAttention;
                }
                return finish(true, "STATE_CONFLICT");
            }
            if (attempt.Accepted) {
                return finish(true, "INFORMATIONAL_LATE_RPC_OUTCOME");
            }
            if (outcome.Success || background) {
                attempt.Accepted = true;
                attempt.Disposition = background ? EAttemptDisposition::CompletedWithBackground : EAttemptDisposition::CompletedSuccess;
                ReleaseSlot(state, attempt.Id);
                AddBackground(state, attempt);
                if (background) {
                    auto& operation = state.Operations.at(attempt.OperationId);
                    if (!IsTerminal(operation.Status)) {
                        if (operation.Status != EOperationStatus::Paused) {
                            operation.Status = EOperationStatus::NeedsAttention;
                        }
                        operation.Outcome = outcome.Issue;
                    }
                }
            } else if (rejection) {
                attempt.Disposition = EAttemptDisposition::CompletedRejection;
                ReleaseSlot(state, attempt.Id);
                auto& operation = state.Operations.at(attempt.OperationId);
                if (!IsTerminal(operation.Status)) {
                    operation.Status = EOperationStatus::Rejected;
                    ++operation.Revision;
                }
                if (operation.Plan.Automatic && !operation.Plan.SuppressionKey.empty()) {
                    state.SuppressedAutomaticPlans.insert(operation.Plan.SuppressionKey);
                }
            } else {
                attempt.Disposition = EAttemptDisposition::Ambiguous;
                auto& operation = state.Operations.at(attempt.OperationId);
                if (!IsTerminal(operation.Status)) {
                    if (operation.Status != EOperationStatus::Paused) {
                        operation.Status = EOperationStatus::NeedsAttention;
                    }
                    ++operation.Revision;
                }
            }
            return finish(true, "RPC_OUTCOME_RECORDED");
        }
        case ECommandKind::AcceptByObservation: {
            auto found = state.Attempts.find(command.Attempt.Id);
            if (found == state.Attempts.end() || IsAttemptClosed(found->second)) {
                return finish(false, "NO_UNRESOLVED_MATCHING_ATTEMPT");
            }
            auto& attempt = found->second;
            const auto error = ValidateAcceptance(state, attempt, command.Acceptance);
            if (!error.empty()) {
                return finish(false, error);
            }
            attempt.Accepted = true;
            attempt.Disposition = EAttemptDisposition::AcceptedByObservation;
            attempt.Proof = command.Acceptance.Provenance;
            state.AcceptedGenerationHighWater = std::max(state.AcceptedGenerationHighWater, command.Acceptance.Observed.Generation);
            ReleaseSlot(state, attempt.Id);
            AddBackground(state, attempt);
            return finish(true, "ACCEPTED_BY_OBSERVATION");
        }
        case ECommandKind::Reconcile: {
            if (!command.Coherent || !command.Fresh || !state.ExclusiveHistory || !ValidateClusterView(command.Observed).empty() || command.Observed.Generation < state.AcceptedGenerationHighWater) {
                return finish(false, "RECONCILIATION_NOT_COHERENT_OR_EXCLUSIVE");
            }
            state.AcceptedGenerationHighWater = std::max(state.AcceptedGenerationHighWater, command.Observed.Generation);
            for (auto& [id, operation] : state.Operations) {
                Y_UNUSED(id);
                bool changed = false;
                for (auto& work : operation.Background) {
                    if (work.Complete) {
                        continue;
                    }
                    if (BackgroundDone(operation, work, command.Observed, work.Target && command.FreshHealthyPiles.contains(*work.Target))) {
                        work.Complete = true;
                        work.Outcome = "observed_postcondition";
                        changed = true;
                    } else if (operation.Status == EOperationStatus::Preempted && operation.PreemptedBy) {
                        const auto& emergency = state.Operations.at(*operation.PreemptedBy);
                        bool superseded = false;
                        for (const auto& attemptId : emergency.Attempts) {
                            superseded = superseded || (emergency.Plan.Kind == EOperationKind::Failover && SupersedesBackground(state.Attempts.at(attemptId), work, command.Observed));
                        }
                        if (superseded) {
                            work.Complete = true;
                            work.Superseded = true;
                            work.Outcome = "superseded_original_postcondition_unmet";
                            changed = true;
                        }
                    }
                }
                if (changed) {
                    ++operation.Revision;
                    operation.ObservedStates.push_back(command.Observed);
                }
                if (!IsTerminal(operation.Status) && NoOpenAttempt(state, operation) && !operation.Background.empty() && std::all_of(operation.Background.begin(), operation.Background.end(), [](const auto& work) {
                    return work.Complete;
                })) {
                    operation.NextStep = operation.Background.size();
                    if (operation.NextStep == operation.Plan.Steps.size()) {
                        operation.Status = EOperationStatus::Succeeded;
                    } else if (operation.Status != EOperationStatus::Paused) {
                        operation.Status = state.RequestedMode == ERequestedMode::Observe ? EOperationStatus::Paused : EOperationStatus::Queued;
                    }
                }
            }
            return finish(true, "RECONCILED");
        }
        case ECommandKind::Cancel:
        case ECommandKind::Resume: {
            auto found = state.Operations.find(command.OperationId);
            if (found == state.Operations.end()) {
                return finish(false, "OPERATION_NOT_FOUND");
            }
            auto& operation = found->second;
            if (command.Kind == ECommandKind::Cancel) {
                const auto error = CancelError(state, operation);
                if (!error.empty()) {
                    return finish(false, error);
                }
                operation.Status = EOperationStatus::Cancelled;
            } else {
                const auto error = dispatchError();
                if (!error.empty()) {
                    return finish(false, error);
                }
                if (operation.Status != EOperationStatus::Paused || operation.Revision != command.ExpectedWorkflowRevision || operation.NextStep >= operation.Plan.Steps.size()) {
                    return finish(false, "STALE_OR_UNRESUMABLE_WORKFLOW");
                }
                for (const auto& work : operation.Background) {
                    if (!work.Complete) {
                        return finish(false, "PRIOR_POSTCONDITION_UNMET");
                    }
                }
                operation.Status = EOperationStatus::Queued;
                state.OrdinaryOwner = operation.Id;
            }
            ++operation.Revision;
            return finish(true, command.Kind == ECommandKind::Cancel ? "CANCELLED" : "RESUMED");
        }
        case ECommandKind::ResolveExternal: {
            if (command.Reason.empty() || !command.AcknowledgeUnfencedRecovery || !command.ExecutorStopped || !command.ServerWorkReconciled || !command.Coherent || !command.Fresh || !ValidateClusterView(command.Observed).empty() || command.Observed.Generation < state.AcceptedGenerationHighWater) {
                return finish(false, "UNFENCED_RECOVERY_ATTESTATIONS_REQUIRED");
            }
            auto found = state.Operations.find(command.OperationId);
            if (found == state.Operations.end()) {
                return finish(false, "OPERATION_NOT_FOUND");
            }
            auto& operation = found->second;
            for (const auto& id : operation.Attempts) {
                auto& attempt = state.Attempts.at(id);
                if (!IsAttemptClosed(attempt)) {
                    attempt.Disposition = EAttemptDisposition::ExternallyResolved;
                    attempt.Proof = command.Reason + ":" + CanonicalFingerprint(command.Observed);
                    ReleaseSlot(state, id);
                }
            }
            for (auto& work : operation.Background) {
                if (!work.Complete) {
                    work.Complete = true;
                    work.Superseded = true;
                    work.Outcome = "externally_reconciled:" + command.Reason;
                }
            }
            operation.Status = EOperationStatus::ExternallyResolved;
            operation.Outcome = command.Reason;
            ++operation.Revision;
            state.RequestedMode = ERequestedMode::Manual;
            state.AcceptedGenerationHighWater = std::max(state.AcceptedGenerationHighWater, command.Observed.Generation);
            // Recovery ends the old interval; only a fresh acknowledgement may
            // establish exclusivity for subsequent work, not prove the old RPC.
            state.Ownership.reset();
            state.ExclusiveHistory = false;
            return finish(true, "EXTERNALLY_RESOLVED");
        }
        case ECommandKind::AdvanceGeneration:
            if (!command.Coherent || !command.Fresh || command.Observed.Generation < state.AcceptedGenerationHighWater) {
                return finish(false, "GENERATION_HIGH_WATER_REGRESSION");
            }
            state.AcceptedGenerationHighWater = command.Observed.Generation;
            return finish(true, "HIGH_WATER_ADVANCED");
    }
    return finish(false, "UNSUPPORTED_COMMAND");
}

} // namespace NBridgeSkipper
