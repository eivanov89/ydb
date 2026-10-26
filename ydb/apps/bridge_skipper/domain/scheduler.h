#pragma once

#include "types.h"

namespace NBridgeSkipper {

struct TSchedulerConfig {
    size_t SampleNodesPerPile = 3;
    size_t PerPileHealthConcurrency = 3;
    size_t GlobalAdminConcurrency = 16;
    TDuration RoundInterval = TDuration::Seconds(15);
    TDuration RoundDeadline = TDuration::Seconds(12);
    TDuration BracketAdminTimeout = TDuration::Seconds(1);
    TDuration HealthOperationTimeout = TDuration::Seconds(8);
    TDuration HealthClientTimeout = TDuration::Seconds(9);
    TDuration ObservationTtl = TDuration::Seconds(20);
    TDuration AdmissionBudget = TDuration::Seconds(5);
    TDuration DispatchMargin = TDuration::Seconds(2);
    TDuration SchedulerTolerance = TDuration::Seconds(5);
    TDuration ConfirmationStartGap = TDuration::Seconds(20);
};

TString ValidateSchedulerConfig(const TSchedulerConfig& config);

struct TRoundReservation {
    ui64 Id = 0;
    TInstant StartedAt;
    TInstant Deadline;
    std::vector<TFrozenSample> Samples;
    std::vector<TNode> Launch;
    TString Blocker;
};

// Reservations represent whole chains and survive deadline/cancel until SDK
// settlement. The caller is responsible for issuing no second method while a
// chain has that method in flight.
class TBoundedScheduler {
public:
    explicit TBoundedScheduler(TSchedulerConfig config);
    std::optional<TRoundReservation> Start(const std::vector<TNode>& inventory);
    bool CloseDue();
    bool Settle(ui64 roundId, ui32 nodeId);
    bool AcquireAdmin(const TString& endpoint, bool operation);
    bool ReleaseAdmin(const TString& endpoint);
    size_t InFlightChains() const;
    size_t InFlightAdmin() const;
    const std::optional<TRoundReservation>& CurrentRound() const;

private:
    struct TChain {
        ui64 RoundId;
        TString Pile;
        TString Endpoint;
    };

    TSchedulerConfig Config;
    ui64 NextRoundId = 1;
    TInstant NextStart;
    bool Scheduled = false;
    std::optional<TRoundReservation> Current;
    std::map<TString, size_t> Rotation;
    std::map<ui32, TChain> Chains;
    std::map<TString, bool> Admin;
};

// Exclude duplicate IDs/conflicting bindings, preserving explicit empty names.
std::vector<TNode> VerifiedInventory(const std::vector<TNode>& nodes);

// A confirmed responder binding may resolve an unlabeled host only with an
// explicitly qualified supported-version implicit-mapping fixture.
TString VerifyBinding(TNode& node, ui32 responderId, const std::optional<TString>& responderPile,
    const std::set<TString>& confirmedPiles, bool implicitMappingQualified);

} // namespace NBridgeSkipper
