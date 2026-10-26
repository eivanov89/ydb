#include "internal.h"
#include <ydb/apps/bridge_skipper/runtime/gateway.h>
#include <algorithm>

namespace NBridgeSkipper {

void UpdateHttpMetrics(const TControllerSnapshot& snapshot, const TConfig& config,
    const TLocalStopReceipt& stop, size_t pendingStorage,
    const TIntrusivePtr<NMonitoring::TDynamicCounters>& counters)
{
    const auto live = ReadLiveness(snapshot);
    *counters->GetCounter("skipper_controller_progress_age_milliseconds") = live.AgeMs;
    *counters->GetCounter("skipper_controller_stalled") = live.Stalled;
    *counters->GetCounter("skipper_authority_ready") = snapshot.AuthorityReady && !live.Stalled;
    *counters->GetCounter("skipper_autonomy_ready") = snapshot.AutonomyReady && !live.Stalled && !live.EvidenceExpired && !stop.Stopped;
    *counters->GetCounter("skipper_local_stop") = stop.Stopped;
    *counters->GetCounter("skipper_health_inflight") = snapshot.InFlightHealth;
    *counters->GetCounter("skipper_read_inflight") = snapshot.InFlightReads;
    *counters->GetCounter("skipper_admission_depth") = snapshot.PendingCommands;
    *counters->GetCounter("skipper_health_limit") = TGatewayBudget::HealthLimit;
    *counters->GetCounter("skipper_admin_limit") = config.Polling.GlobalAdminConcurrency;
    *counters->GetCounter("skipper_storage_pending") = pendingStorage;
    *counters->GetCounter("skipper_pending_lifecycle_errors") = snapshot.Cluster ? std::count_if(snapshot.Cluster->Pending.begin(), snapshot.Cluster->Pending.end(), [](const auto& pending) {
        return !pending.Error.empty();
    }) : 0;
    *counters->GetCounter("skipper_evidence_expired") = live.EvidenceExpired;
    *counters->GetCounter("skipper_read_skipped") = snapshot.SkippedReads;
    *counters->GetCounter("skipper_blockers") = snapshot.Blockers.size() + live.Stalled + live.EvidenceExpired;
    *counters->GetCounter("skipper_round_closed_total", true) = snapshot.ClosedRounds;
    *counters->GetCounter("skipper_round_deadline_total", true) = snapshot.DeadlineRounds;
    *counters->GetCounter("skipper_admission_budget_overruns_total", true) = snapshot.AdmissionBudgetOverruns;
    *counters->GetCounter("skipper_handoff_freshness_misses_total", true) = snapshot.FreshnessMisses;
    *counters->GetCounter("skipper_round_sampled_nodes") = snapshot.LastRoundSampledNodes;
    *counters->GetCounter("skipper_round_fresh_nodes") = snapshot.LastRoundFreshNodes;
    *counters->GetCounter("skipper_handoff_slack_observed") = snapshot.LastHandoffSlack.has_value();
    if (snapshot.LastHandoffSlack) {
        *counters->GetCounter("skipper_handoff_slack_microseconds") = snapshot.LastHandoffSlack->MicroSeconds();
    }
    if (snapshot.LastRoundCloseToHandoff) {
        *counters->GetCounter("skipper_round_close_to_handoff_microseconds") = snapshot.LastRoundCloseToHandoff->MicroSeconds();
    }
    ui64 otherRejects = 0;
    for (const auto& [reason, count] : snapshot.EvidenceRejects) {
        bool known = false;
        for (const TStringBuf fixed : {"STALE_ADMIN_EPOCH", "UNQUALIFIED_EVIDENCE_PROFILE", "INVALID_ROUND_TIME", "NO_PRIMARY",
                "DUPLICATE_RESPONDER_ID", "DUPLICATE_PILE_SAMPLE", "CONTRADICTORY_GLOBAL_ASSESSMENT", "NO_REPORTERS",
                "NO_COMPLETE_CORROBORATED_GLOBAL_VECTOR", "NO_ELIGIBLE_CANDIDATE", "INSUFFICIENT_FRESH_LOCAL_JUSTIFICATION",
                "RESYNC_FAILURE_EVIDENCE_UNSUPPORTED", "PLACEMENT_ACKNOWLEDGEMENT_REQUIRED", "NO_DIRECT_CANDIDATE_ENDPOINT",
                "HANDOFF_EVIDENCE_EXPIRED"}) {
            known = known || reason == fixed;
        }
        if (known) {
            *counters->GetSubgroup("reason", reason)->GetCounter("skipper_evidence_rejected_total", true) = count;
        } else {
            otherRejects += count;
        }
    }
    *counters->GetSubgroup("reason", "OTHER")->GetCounter("skipper_evidence_rejected_total", true) = otherRejects;
    size_t background = 0;
    size_t longRunning = 0;
    if (snapshot.Applied) {
        const auto& state = *snapshot.Applied;
        *counters->GetCounter("skipper_unresolved_operation") = state.UnresolvedAttempt.has_value();
        *counters->GetCounter("skipper_ordinary_workflow") = state.OrdinaryOwner && !IsTerminal(state.Operations.at(*state.OrdinaryOwner).Status);
        for (const auto& [id, operation] : state.Operations) {
            Y_UNUSED(id);
            for (const auto& work : operation.Background) {
                background += !work.Complete;
            }
            const auto threshold = config.Operations.AttentionAfter.find(KindName(operation.Plan.Kind));
            TInstant created;
            if (threshold != config.Operations.AttentionAfter.end() && threshold->second && operation.CompletedAt.empty() && TInstant::TryParseIso8601(operation.CreatedAt, created) && live.Now > created && live.Now - created > *threshold->second) {
                ++longRunning;
            }
        }
        for (const auto mode : {ERequestedMode::Manual, ERequestedMode::Auto, ERequestedMode::Observe}) {
            *counters->GetSubgroup("mode", ModeName(mode))->GetCounter("skipper_requested_mode") = state.RequestedMode == mode;
        }
    }
    *counters->GetCounter("skipper_background_work") = background;
    *counters->GetCounter("skipper_long_running_work") = longRunning;
}

} // namespace NBridgeSkipper
