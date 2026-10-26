#pragma once

#include "evidence.h"

namespace NBridgeSkipper {

struct TPolicyInput {
    TString DecisionId;
    TString ProcessId;
    ui64 RoundId = 0;
    TInstant RoundStartedAt;
    TInstant RoundDeadline;
    TClusterView View;
    TEpoch Epoch;
    TProfileQualification Qualification;
    std::vector<TFrozenSample> Samples;
    std::vector<TObservation> Observations;
    std::vector<TNode> Nodes;
    bool PlacementAcknowledged = false;
    TEvidenceTiming Timing;
};

TDecision EvaluatePolicy(const TPolicyInput& input);

struct TDispatchReadiness {
    bool Auto = false;
    bool AuthorityReady = false;
    bool OwnershipAcknowledged = false;
    bool DurableStorage = false;
    bool UnresolvedAttempt = false;
    bool LocalStop = false;
    bool BackgroundGuardsPermit = false;
    TEpoch Epoch;
};

TString ValidatePreflight(
    const TDecision& decision,
    const TClusterView& directView,
    const TNode& directNode,
    const TDispatchReadiness& readiness,
    TInstant now,
    TDuration dispatchMargin = TDuration::Seconds(2));

} // namespace NBridgeSkipper
