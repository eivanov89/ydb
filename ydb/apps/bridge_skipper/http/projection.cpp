#include "internal.h"

#include <util/string/cast.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>
#include <library/cpp/svnversion/svnversion.h>
#include <ydb/public/sdk/cpp/src/version.h>

#include <algorithm>

namespace NBridgeSkipper {
namespace {

template<size_t N> TString EnumName(ui32 value, const char* const (&names)[N]) {
    return value < N ? TString(names[value]) : TString("UNKNOWN");
}

TApiJson Assessment(const TAssessment& assessment) {
    auto value = Object();
    value["verdict"] = assessment.Value == EAssessment::Failed ? "FAILED" : (assessment.Value == EAssessment::NoFailureReported ? "NO_FAILURE_REPORTED" : "UNKNOWN");
    value["complete"] = assessment.Complete;
    value["reasons"] = Array();
    for (size_t i = 0; i < std::min<size_t>(20, assessment.Reasons.size()); ++i) {
        value["reasons"].AppendValue(BoundedDiagnostic(assessment.Reasons[i]));
    }
    value["diagnostics_truncated"] = assessment.Reasons.size() > 20;
    return value;
}

TApiJson Blocker(TStringBuf code) {
    auto value = Object();
    value["code"] = TString(code);
    value["reason"] = BoundedDiagnostic(code);
    return value;
}

TApiJson Action(TString action, bool enabled, TString reason) {
    auto value = Object();
    value["action"] = std::move(action);
    value["enabled"] = enabled;
    value["code"] = enabled ? TString() : reason;
    value["reason"] = enabled ? TString() : BoundedDiagnostic(reason);
    return value;
}

bool AdmissionReady(const TControllerSnapshot& snapshot, const TLocalStopReceipt& stop, const TReadLiveness& live) {
    return snapshot.Applied && snapshot.MutationReady && !snapshot.UnsupportedCluster && !live.Stalled && !stop.Stopped && snapshot.Applied->RequestedMode != ERequestedMode::Observe && snapshot.Applied->Ownership && !snapshot.Applied->StateConflict && !snapshot.Applied->UnresolvedAttempt;
}

TApiJson Steps(const TWorkflowPlan& plan) {
    auto result = Array();
    for (const auto& step : plan.Steps) {
        auto value = Object();
        value["method"] = MethodName(step.Method);
        value["updates"] = NamedStates(step.Update);
        value["target"] = step.Target ? TApiJson(*step.Target) : Null();
        value["quorum_piles"] = Array();
        result.AppendValue(std::move(value));
    }
    return result;
}

} // namespace

TString ModeName(ERequestedMode mode) {
    static const char* const names[] = {"Manual", "Auto", "Observe"};
    return EnumName(static_cast<ui32>(mode), names);
}

TString KindName(EOperationKind kind) {
    static const char* const names[] = {"failover", "switchover", "takedown", "primary_maintenance", "rejoin", "convert", "add_pile", "remove_pile"};
    return EnumName(static_cast<ui32>(kind), names);
}

TString StatusName(EOperationStatus status) {
    static const char* const names[] = {"queued", "executing", "waiting_background", "paused", "reconciling", "needs_attention", "succeeded", "rejected", "cancelled", "preempted", "externally_resolved"};
    return EnumName(static_cast<ui32>(status), names);
}

TString MethodName(EBridgeMethod method) {
    static const char* const names[] = {"UpdateClusterState", "ConvertToBridge", "AddPile", "RemovePile"};
    return EnumName(static_cast<ui32>(method), names);
}

TString PileStateName(EPileState state) {
    static const char* const names[] = {"UNKNOWN", "PRIMARY", "PROMOTED", "SYNCHRONIZED", "NOT_SYNCHRONIZED", "SUSPENDED", "DISCONNECTED", "INACTIVE"};
    return EnumName(static_cast<ui32>(state), names);
}

TString SourceName(ESourceKind source) {
    static const char* const names[] = {"UNSUPPORTED", "GLOBAL_BSC", "LOCAL_WHITEBOARD", "LOCAL_NODEWARDEN"};
    return EnumName(static_cast<ui32>(source), names);
}

TApiJson NamedStates(const TNamedPileStates& states) {
    auto result = Array();
    for (const auto& [name, state] : states) {
        auto value = Object();
        value["name"] = name;
        value["state"] = PileStateName(state);
        result.AppendValue(std::move(value));
    }
    return result;
}

TApiJson ProjectOperation(const TOperation& operation, const TDurableState& state) {
    auto value = Object();
    value["id"] = operation.Id;
    value["request_id"] = operation.RequestId;
    value["kind"] = KindName(operation.Plan.Kind);
    value["source"] = operation.Plan.Automatic ? "automatic" : "manual";
    value["status"] = StatusName(operation.Status);
    value["revision"] = ToString(operation.Revision);
    value["state_index"] = ToString(state.AppliedIndex);
    value["next_step"] = static_cast<ui32>(operation.NextStep);
    value["created_at"] = operation.CreatedAt;
    value["updated_at"] = operation.UpdatedAt;
    TInstant created;
    TInstant completed;
    if (TInstant::TryParseIso8601(operation.CreatedAt, created)) {
        const auto until = TInstant::TryParseIso8601(operation.CompletedAt, completed) ? completed : TSkipperClock::Now();
        value["elapsed_ms"] = AgeMs(until, created);
    } else {
        value["elapsed_ms"] = Null();
    }
    value["summary"] = KindName(operation.Plan.Kind);
    value["plan"] = Object();
    value["plan"]["target"] = operation.Plan.Target ? TApiJson(*operation.Plan.Target) : Null();
    value["plan"]["successor"] = operation.Plan.Successor ? TApiJson(*operation.Plan.Successor) : Null();
    value["steps"] = Steps(operation.Plan);
    value["attempts"] = Array();
    for (const auto& id : operation.Attempts) {
        const auto& attempt = state.Attempts.at(id);
        auto entry = Object();
        entry["id"] = id;
        entry["method"] = MethodName(attempt.Method);
        entry["step"] = static_cast<ui32>(attempt.Step);
        entry["endpoint"] = attempt.Endpoint;
        entry["node_id"] = ToString(attempt.NodeId);
        entry["intent_index"] = ToString(attempt.IntentIndex);
        entry["designated_member"] = attempt.DesignatedMember;
        entry["designated_process"] = attempt.DesignatedProcess;
        entry["arguments_hash"] = attempt.ArgumentsHash;
        entry["updates"] = NamedStates(attempt.Update);
        entry["target"] = attempt.Target ? TApiJson(*attempt.Target) : Null();
        static const char* const dispositions[] = {"intent_committed", "never_dispatched", "completed_success", "completed_rejection", "completed_with_background", "accepted_by_observation", "ambiguous", "externally_resolved"};
        entry["disposition"] = EnumName(static_cast<ui32>(attempt.Disposition), dispositions);
        static const char* const rpc[] = {"pending", "success", "rejection", "background", "unknown"};
        entry["rpc_outcome"] = EnumName(static_cast<ui32>(attempt.RpcDisposition), rpc);
        entry["rpc_detail"] = BoundedDiagnostic(attempt.RpcDetail);
        entry["accepted"] = attempt.Accepted;
        entry["acceptance_proof"] = BoundedDiagnostic(attempt.Proof);
        entry["acceptance_assumptions"] = attempt.Disposition == EAttemptDisposition::AcceptedByObservation ? "Exclusive writer history and qualified distinguishing marker; no server request-ID proof" : TString();
        value["attempts"].AppendValue(std::move(entry));
    }
    value["background_work"] = Array();
    value["guards"] = Array();
    for (const auto& work : operation.Background) {
        auto entry = Object();
        entry["attempt_id"] = work.AttemptId;
        entry["kind"] = KindName(work.Kind);
        entry["target"] = work.Target ? TApiJson(*work.Target) : Null();
        entry["complete"] = work.Complete;
        entry["superseded"] = work.Superseded;
        entry["outcome"] = BoundedDiagnostic(work.Outcome);
        value["background_work"].AppendValue(entry);
        if (!work.Complete) {
            value["guards"].AppendValue(std::move(entry));
        }
    }
    value["preempted_by"] = operation.PreemptedBy ? TApiJson(*operation.PreemptedBy) : Null();
    value["preempts"] = Array();
    for (const auto& [id, other] : state.Operations) {
        if (other.PreemptedBy == operation.Id) {
            value["preempts"].AppendValue(id);
        }
    }
    value["outcome"] = BoundedDiagnostic(operation.Outcome);
    value["observed_states"] = Array();
    for (const auto& observed : operation.ObservedStates) {
        auto entry = Object();
        entry["generation"] = ToString(observed.Generation);
        entry["state_fingerprint"] = CanonicalFingerprint(observed);
        entry["piles"] = NamedStates(observed.Piles);
        value["observed_states"].AppendValue(std::move(entry));
    }
    value["audit"] = Array();
    for (const auto& event : operation.Audit) {
        auto entry = Object();
        entry["command_id"] = event.CommandId;
        entry["index"] = ToString(event.Index);
        entry["time"] = event.Time;
        entry["code"] = event.Code;
        entry["reason"] = BoundedDiagnostic(event.Reason);
        entry["caller_id"] = "local";
        value["audit"].AppendValue(std::move(entry));
    }
    const auto cancelError = CancelError(state, operation);
    value["cancel_available"] = cancelError.empty();
    value["cancel_reason"] = cancelError;
    value["resume_available"] = operation.Status == EOperationStatus::Paused && !state.UnresolvedAttempt && std::all_of(operation.Background.begin(), operation.Background.end(), [](const auto& work) {
        return work.Complete;
    });
    value["resume_reason"] = operation.Status == EOperationStatus::Preempted ? "Preempted workflows cannot resume" : "Prior invocation and postcondition must be resolved";
    value["resolve_available"] = state.UnresolvedAttempt && state.Attempts.at(*state.UnresolvedAttempt).OperationId == operation.Id;
    return value;
}

TApiJson ProjectState(const TControllerSnapshot& snapshot, const TConfig& config, const TLocalStopReceipt& stop) {
    const auto live = ReadLiveness(snapshot);
    const auto& displayCluster = snapshot.UnsupportedCluster ? snapshot.UnsupportedCluster : snapshot.Cluster;
    const TDurableState empty;
    const auto& state = snapshot.Applied ? *snapshot.Applied : empty;
    const bool ready = AdmissionReady(snapshot, stop, live);
    auto value = ApiObject();
    value["incarnation_id"] = snapshot.IncarnationId;
    value["build"] = GetProgramCommitId();
    value["versions"] = Object();
    value["versions"]["api"] = 1;
    value["versions"]["state_schema"] = state.Schema;
    value["versions"]["sdk"] = NYdb::YDB_SDK_VERSION;
    value["snapshot_revision"] = ToString(snapshot.SnapshotRevision);
    value["topology_revision"] = ToString(snapshot.TopologyRevision);
    value["controller_revision"] = ToString(state.ControllerRevision);
    value["policy_revision"] = snapshot.PolicyRevision;
    value["generated_at"] = snapshot.GeneratedAt.ToString();
    value["read_status"] = Object();
    value["read_status"]["served_at"] = live.Now.ToString();
    value["read_status"]["snapshot_age_ms"] = live.AgeMs;
    value["read_status"]["controller_stalled"] = live.Stalled;
    auto controller = Object();
    controller["requested_mode"] = ModeName(state.RequestedMode);
    controller["effective_state"] = stop.Stopped ? "stopped" : (live.Stalled ? "blocked" : snapshot.EffectiveState);
    controller["autonomy_ready"] = snapshot.AutonomyReady && ready && !live.EvidenceExpired;
    controller["authority"] = Object();
    controller["authority"]["backend"] = "local";
    controller["authority"]["role"] = "standalone";
    controller["authority"]["ready"] = snapshot.AuthorityReady && !live.Stalled;
    controller["authority"]["applied_index"] = ToString(state.AppliedIndex);
    controller["authority"]["committed_index"] = ToString(state.AppliedIndex);
    controller["local_stop"] = Object();
    controller["local_stop"]["latched"] = stop.Stopped;
    controller["local_stop"]["revision"] = ToString(stop.Revision);
    controller["local_stop"]["marker_durable"] = stop.Durable;
    controller["ownership_acknowledgement"] = Object();
    controller["ownership_acknowledgement"]["active"] = state.Ownership.has_value();
    controller["ownership_acknowledgement"]["caller_id"] = "local";
    controller["ownership_acknowledgement"]["independent_failure_domain"] = state.Ownership && state.Ownership->IndependentFailureDomain;
    controller["handover"] = Object();
    controller["handover"]["complete"] = HandoverComplete(state);
    controller["blockers"] = Array();
    for (const auto& reason : snapshot.Blockers) {
        controller["blockers"].AppendValue(Blocker(reason));
    }
    if (live.Stalled) {
        controller["blockers"].AppendValue(Blocker("CONTROLLER_STALLED"));
    }
    if (live.EvidenceExpired) {
        controller["blockers"].AppendValue(Blocker("EVIDENCE_EXPIRED"));
    }
    value["controller"] = std::move(controller);
    auto cluster = Object();
    cluster["cluster_id"] = snapshot.ClusterId;
    cluster["generation"] = displayCluster ? TApiJson(ToString(displayCluster->Generation)) : Null();
    cluster["high_watermark"] = ToString(state.AcceptedGenerationHighWater);
    cluster["state_fingerprint"] = displayCluster ? TApiJson(CanonicalFingerprint(*displayCluster)) : Null();
    cluster["bridged"] = displayCluster && displayCluster->Converted;
    cluster["bridge_state"] = displayCluster ? (displayCluster->Converted ? "KNOWN" : "NOT_FOUND") : "UNKNOWN";
    cluster["conversion_in_progress"] = displayCluster && displayCluster->ConversionInProgress;
    cluster["pending_pile_operation"] = Null();
    if (displayCluster && !displayCluster->Pending.empty()) {
        const auto& pending = displayCluster->Pending.front();
        auto entry = Object();
        entry["pile_name"] = pending.Pile ? TApiJson(*pending.Pile) : Null();
        entry["kind"] = pending.Kind;
        entry["error_reason"] = BoundedDiagnostic(pending.Error);
        cluster["pending_pile_operation"] = std::move(entry);
    }
    cluster["topology_status"] = snapshot.ConfigStale ? "stale" : (snapshot.TopologyConfirmed ? "confirmed" : "tentative");
    value["cluster"] = std::move(cluster);
    std::set<TString> piles(snapshot.Topology.ConfiguredPiles.begin(), snapshot.Topology.ConfiguredPiles.end());
    if (displayCluster) {
        for (const auto& [name, state] : displayCluster->Piles) {
            Y_UNUSED(state);
            piles.insert(name);
        }
    }
    value["piles"] = Array();
    for (const auto& name : piles) {
        auto pile = Object();
        const auto current = displayCluster && displayCluster->Piles.contains(name) ? displayCluster->Piles.at(name) : EPileState::Unspecified;
        pile["name"] = name;
        pile["state"] = PileStateName(current);
        pile["admin_view"] = PileStateName(current);
        pile["observed_generation"] = displayCluster ? TApiJson(ToString(displayCluster->Generation)) : Null();
        pile["profile"] = TString(EvidenceProfileName(config.EvidenceProfile));
        pile["self_assessment"] = "UNKNOWN";
        pile["target_assessment"] = "UNKNOWN";
        pile["sample_count"] = 0;
        pile["fresh_count"] = 0;
        pile["threshold"] = 0;
        pile["evidence_age_ms"] = Null();
        pile["nodes"] = Array();
        pile["coverage"] = "UNKNOWN";
        pile["reachability"] = "UNKNOWN";
        pile["target_assessment"] = Object();
        pile["target_assessment"]["reporters"] = Array();
        for (const auto& report : snapshot.Reports) {
            const auto assessment = report.Targets.find(name);
            if (assessment != report.Targets.end()) {
                auto target = Object();
                target["reporter"] = report.Sample.Pile;
                target["assessment"] = live.Stalled || live.EvidenceExpired ? TApiJson("UNKNOWN") : Assessment(assessment->second);
                pile["target_assessment"]["reporters"].AppendValue(std::move(target));
            }
        }
        if (snapshot.Decision) {
            const auto global = snapshot.Decision->GlobalAssessment.find(name);
            if (global != snapshot.Decision->GlobalAssessment.end() && !live.Stalled && !live.EvidenceExpired) {
                pile["target_assessment"]["corroborated_global"] = Assessment(global->second);
            }
            std::optional<ui64> oldestAge;
            for (const auto& observation : snapshot.Decision->Evidence) {
                if (observation.ResponderPile == name) {
                    oldestAge = std::max(oldestAge.value_or(0), AgeMs(live.Now, observation.HealthStartedAt));
                }
            }
            if (oldestAge) {
                pile["evidence_age_ms"] = *oldestAge;
            }
        }
        for (const auto& report : snapshot.Reports) {
            if (report.Sample.Pile == name) {
                pile["sample_count"] = static_cast<ui32>(report.Sample.Nodes.size());
                pile["fresh_count"] = live.Stalled || live.EvidenceExpired ? 0 : static_cast<ui32>(report.Fresh);
                pile["threshold"] = static_cast<ui32>(report.Sample.Threshold);
                pile["coverage"] = Object();
                pile["coverage"]["eligible"] = report.Eligible && !live.Stalled && !live.EvidenceExpired;
                pile["coverage"]["targets"] = Object();
                for (const auto& [target, assessment] : report.Targets) {
                    pile["coverage"]["targets"][target] = assessment.Complete && !live.Stalled && !live.EvidenceExpired;
                }
                const auto self = report.Targets.find(name);
                if (self != report.Targets.end() && !live.Stalled && !live.EvidenceExpired) {
                    pile["self_assessment"] = Assessment(self->second);
                }
            }
        }
        size_t responding = 0;
        size_t observedNodes = 0;
        for (const auto& node : snapshot.Topology.Nodes) {
            if (node.Pile == name) {
                auto entry = Object();
                entry["node_id"] = ToString(node.NodeId);
                entry["endpoint"] = node.Endpoint;
                entry["host"] = node.Host;
                entry["identity"] = node.LogicalId;
                static const char* const binding[] = {"explicit", "implicit-verified", "implicit-unresolved", "conflict"};
                entry["binding"] = EnumName(static_cast<ui32>(node.Binding), binding);
                entry["admin_age_ms"] = node.AdminObservedAt ? TApiJson(AgeMs(live.Now, node.AdminObservedAt)) : Null();
                entry["reachability"] = "UNKNOWN";
                const bool recentAdmin = node.AdminObservedAt && live.Now >= node.AdminObservedAt && live.Now - node.AdminObservedAt <= config.Polling.ObservationTtl;
                bool replied = recentAdmin;
                entry["issues"] = Array();
                size_t total = 0;
                for (const auto& diagnostic : snapshot.Diagnostics) {
                    if (diagnostic.Endpoint == node.Endpoint) {
                        entry["health_age_ms"] = AgeMs(live.Now, diagnostic.CheckedAt);
                        entry["status"] = diagnostic.Status;
                        entry["latency_ms"] = diagnostic.Latency ? TApiJson(diagnostic.Latency->MicroSeconds() / 1000.0) : Null();
                        entry["rpc_success"] = diagnostic.Success;
                        entry["profile"] = diagnostic.Profile;
                        entry["source_kind"] = SourceName(diagnostic.Source);
                        entry["identity_verified"] = diagnostic.IdentityVerified;
                        entry["origin"] = BoundedDiagnostic(diagnostic.Origin);
                        entry["vantage"] = BoundedDiagnostic(diagnostic.Vantage);
                        entry["source_snapshot"] = diagnostic.SnapshotId.empty() ? Null() : TApiJson(BoundedDiagnostic(diagnostic.SnapshotId));
                        const bool freshDiagnostic = diagnostic.CheckedAt && live.Now >= diagnostic.CheckedAt && live.Now - diagnostic.CheckedAt <= config.Polling.ObservationTtl;
                        replied = replied || (freshDiagnostic && (diagnostic.IdentityVerified || diagnostic.Success));
                        entry["reachability"] = freshDiagnostic ? (replied ? "RESPONDED" : "NO_SUCCESSFUL_REPLY") : "STALE";
                        ++observedNodes;
                        for (const auto& reason : diagnostic.Reasons) {
                            if (++total <= 20) {
                                entry["issues"].AppendValue(BoundedDiagnostic(reason));
                            }
                        }
                    }
                }
                entry["diagnostics_truncated"] = total > 20;
                if (replied) {
                    ++responding;
                    entry["reachability"] = "RESPONDED";
                }
                pile["nodes"].AppendValue(std::move(entry));
            }
        }
        if (responding) {
            pile["reachability"] = "RESPONDERS_PRESENT";
        } else if (observedNodes) {
            pile["reachability"] = "NO_RECENT_SUCCESSFUL_REPLY";
        }
        pile["actions"] = Array();
        for (const auto kind : {EOperationKind::Failover, EOperationKind::Switchover, EOperationKind::Takedown, EOperationKind::PrimaryMaintenance, EOperationKind::Rejoin, EOperationKind::AddPile, EOperationKind::RemovePile}) {
            bool legal = false;
            switch (kind) {
                case EOperationKind::Failover: legal = IsSynchronized(current) || current == EPileState::NotSynchronized; break;
                case EOperationKind::Switchover: legal = current == EPileState::Synchronized; break;
                case EOperationKind::Takedown: legal = current == EPileState::Synchronized; break;
                case EOperationKind::PrimaryMaintenance: legal = current == EPileState::Primary; break;
                case EOperationKind::Rejoin: legal = current == EPileState::Disconnected; break;
                case EOperationKind::AddPile: legal = current == EPileState::Inactive; break;
                case EOperationKind::RemovePile: legal = current == EPileState::Disconnected; break;
                case EOperationKind::Convert: break;
            }
            pile["actions"].AppendValue(Action(KindName(kind), legal && ready, legal ? "CONTROLLER_NOT_READY" : "PILE_STATE_PREREQUISITE"));
        }
        value["piles"].AppendValue(std::move(pile));
    }
    auto evidence = Object();
    evidence["epoch_id"] = DurableHash(snapshot.Epoch.AdminFingerprint + snapshot.Epoch.PolicyRevision + ToString(snapshot.Epoch.TopologyRevision));
    evidence["profile"] = TString(EvidenceProfileName(config.EvidenceProfile));
    evidence["qualified"] = IsProductionAutoQualified(config.EvidenceProfile);
    const auto source = config.EvidenceProfile == EEvidenceProfile::GlobalBscV1 ? ESourceKind::GlobalBsc : (config.EvidenceProfile == EEvidenceProfile::StaticWhiteboardV1 ? ESourceKind::LocalWhiteboard : ESourceKind::LocalNodewarden);
    evidence["source_kind"] = SourceName(source);
    evidence["vantage"] = Object();
    evidence["vantage"]["kind"] = source == ESourceKind::GlobalBsc ? "shared" : "local";
    evidence["vantage"]["origin"] = source == ESourceKind::GlobalBsc ? "bsc" : "UNKNOWN";
    evidence["vantage"]["source_snapshot"] = Null();
    evidence["reporters"] = Array();
    evidence["matrix"] = source == ESourceKind::GlobalBsc ? Null() : Array();
    evidence["global_assessment"] = source == ESourceKind::GlobalBsc ? Object() : Null();
    if (source == ESourceKind::GlobalBsc) {
        evidence["global_assessment"]["status"] = "UNKNOWN";
        evidence["global_assessment"]["reason"] = "Per-sample diagnostics below do not establish a same-quorum whole vector";
    }
    evidence["contradictory_diagnostics"] = Array();
    evidence["provenance"] = Array();
    if (source == ESourceKind::GlobalBsc && snapshot.Decision && !snapshot.Decision->GlobalAssessment.empty() && !live.EvidenceExpired && !live.Stalled) {
        evidence["global_assessment"]["status"] = "CORROBORATED_POLICY_VECTOR";
        evidence["global_assessment"]["reason"] = "One candidate-and-all-target vector supported by the policy's required same-quorum observations";
        evidence["global_assessment"]["targets"] = Object();
        for (const auto& [target, assessment] : snapshot.Decision->GlobalAssessment) {
            evidence["global_assessment"]["targets"][target] = Assessment(assessment);
        }
    }
    if (snapshot.Decision) {
        for (const auto& observation : snapshot.Decision->Evidence) {
            auto item = Object();
            item["request_id"] = observation.RequestId;
            item["node_id"] = ToString(observation.NodeId);
            item["responder_pile"] = observation.ResponderPile ? TApiJson(*observation.ResponderPile) : Null();
            item["profile"] = observation.Profile;
            item["source_kind"] = SourceName(observation.Source);
            item["origin"] = BoundedDiagnostic(observation.Origin);
            item["vantage"] = BoundedDiagnostic(observation.Vantage);
            item["source_snapshot"] = observation.SnapshotId.empty() ? Null() : TApiJson(BoundedDiagnostic(observation.SnapshotId));
            item["started_at"] = observation.HealthStartedAt.ToString();
            item["finished_at"] = observation.FinishedAt.ToString();
            item["age_ms"] = AgeMs(live.Now, observation.HealthStartedAt);
            item["before_fingerprint"] = observation.BeforeFingerprint;
            item["after_fingerprint"] = observation.AfterFingerprint;
            item["identity_verified"] = observation.IdentityVerified;
            item["direct_local_origin"] = observation.DirectLocalOrigin;
            evidence["provenance"].AppendValue(std::move(item));
        }
    }
    for (const auto& report : snapshot.Reports) {
        auto entry = Object();
        entry["pile"] = report.Sample.Pile;
        entry["origin"] = report.Origin;
        entry["sample_count"] = static_cast<ui32>(report.Sample.Nodes.size());
        entry["fresh_count"] = live.EvidenceExpired ? 0 : static_cast<ui32>(report.Fresh);
        entry["threshold"] = static_cast<ui32>(report.Sample.Threshold);
        entry["targets"] = Object();
        for (const auto& [target, assessment] : report.Targets) {
            entry["targets"][target] = Assessment(assessment);
        }
        evidence["reporters"].AppendValue(std::move(entry));
        for (const auto& [target, assessment] : report.Targets) {
            if (source == ESourceKind::GlobalBsc) {
                // Individual target results cannot be stitched into an
                // authoritative whole-vector GLOBAL_BSC assessment.
                Y_UNUSED(target);
                Y_UNUSED(assessment);
            } else {
                auto cell = Object();
                cell["reporter"] = report.Sample.Pile;
                cell["target"] = target;
                cell["verdict"] = live.EvidenceExpired ? TApiJson("UNKNOWN") : Assessment(assessment);
                cell["origin"] = report.Origin;
                cell["denominator"] = static_cast<ui32>(report.Sample.Nodes.size());
                cell["threshold"] = static_cast<ui32>(report.Sample.Threshold);
                const auto numerator = report.TargetNumerators.find(target);
                cell["numerator"] = numerator == report.TargetNumerators.end() ? Null() : TApiJson(static_cast<ui32>(numerator->second));
                evidence["matrix"].AppendValue(std::move(cell));
            }
        }
    }
    evidence["capability_messages"] = Array();
    if (!IsProductionAutoQualified(config.EvidenceProfile)) {
        evidence["capability_messages"].AppendValue("Production Auto profile is not qualified; manual operations and diagnostics remain available");
    }
    value["evidence"] = std::move(evidence);
    value["decision"] = Null();
    if (snapshot.Decision) {
        const auto& decision = *snapshot.Decision;
        auto entry = Object();
        entry["decision_id"] = decision.Id;
        entry["kind"] = decision.Kind == EDecisionKind::Failover ? "failover" : (decision.Kind == EDecisionKind::Stable ? "stable" : "blocked");
        entry["reason_code"] = decision.Reason;
        entry["target_rule"] = decision.Reason == "TWO_ACTIVE_LOCAL_PRIMARY_PREFERENCE" ? "P-06 exactly-two-active independently-local primary preference" : (decision.Source == ESourceKind::GlobalBsc ? "P-04 same-quorum whole-vector GLOBAL_BSC" : "P-02/P-04 strict local reporter and target majorities");
        size_t active = 0;
        if (displayCluster) {
            for (const auto& [name, state] : displayCluster->Piles) {
                Y_UNUSED(name);
                active += IsActive(state);
            }
        }
        entry["active_pile_count"] = static_cast<ui32>(active);
        entry["policy_conditions"] = Array();
        entry["policy_conditions"].AppendValue("Exclusive administration acknowledgement is an operational assumption");
        entry["policy_conditions"].AppendValue("GLOBAL_BSC and sole-reporter paths additionally require independent placement acceptance");
        entry["policy_conditions"].AppendValue("Profile recognition and local/mock tests do not qualify production Auto");
        entry["confirmation"] = Object();
        entry["confirmation"]["required_rounds"] = 2;
        entry["confirmation"]["confirmed"] = decision.Confirmed && !live.EvidenceExpired && !live.Stalled;
        entry["confirmation"]["latest_round_id"] = ToString(decision.RoundId);
        entry["confirmation"]["latest_round_started_at"] = decision.RoundStartedAt.ToString();
        entry["confirmation"]["rule"] = "Two distinct qualified rounds ordered by their start instants; current controller owns their retained confirmation history";
        entry["candidate"] = decision.Candidate ? TApiJson(*decision.Candidate) : Null();
        entry["intended_roles"] = NamedStates(decision.Update);
        entry["confirmed"] = decision.Confirmed && !live.EvidenceExpired;
        entry["evidence_expires_at"] = decision.EvidenceExpiry.ToString();
        value["decision"] = std::move(entry);
    }
    value["ordinary_workflow"] = state.OrdinaryOwner ? ProjectOperation(state.Operations.at(*state.OrdinaryOwner), state) : Null();
    value["unresolved_attempt"] = state.UnresolvedAttempt ? TApiJson(*state.UnresolvedAttempt) : Null();
    value["background_work"] = Array();
    for (const auto& [id, operation] : state.Operations) {
        for (const auto& work : operation.Background) {
            if (!work.Complete) {
                auto entry = Object();
                entry["operation_id"] = id;
                entry["kind"] = KindName(work.Kind);
                entry["target"] = work.Target ? TApiJson(*work.Target) : Null();
                value["background_work"].AppendValue(std::move(entry));
            }
        }
    }
    value["capabilities"] = Object();
    value["capabilities"]["actions"] = Array();
    value["capabilities"]["actions"].AppendValue(Action("convert", displayCluster && !displayCluster->Converted && snapshot.Topology.ConfiguredPiles.size() == 1 && ready, "CONVERT_PREREQUISITES"));
    value["capabilities"]["lab"] = false;
    value["settings"] = Object();
    value["settings"]["round_interval_seconds"] = config.Polling.RoundInterval.Seconds();
    value["settings"]["sample_nodes_per_pile"] = config.Polling.SampleNodesPerPile;
    if (config.YdbAuth.TokenFile && config.YdbAuth.AllowPlaintextToken && !config.YdbEndpoints.empty() && config.YdbEndpoints.front().Scheme == "grpc") {
        value["security"] = Object();
        value["security"]["plaintext_token_diagnostic"] = "Operator explicitly enabled plaintext YDB token transmission";
    }
    return value;
}

} // namespace NBridgeSkipper
