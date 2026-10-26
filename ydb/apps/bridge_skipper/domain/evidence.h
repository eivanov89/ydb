#pragma once

#include "types.h"

namespace NBridgeSkipper {

struct TEvidenceTiming {
    TDuration ObservationTtl = TDuration::Seconds(20);
    TDuration BeforeMaxAge = TDuration::Seconds(1);
    TDuration DispatchMargin = TDuration::Seconds(2);
    TDuration ConfirmationStartGap = TDuration::Seconds(20);
};

// Adapters must preserve message presence; a present empty pile name is valid.
// Inventory is release-pinned, validated physical group identity, never a pool name.
struct TPhysicalGroup {
    TString Id;
    std::optional<TString> Pile;
    bool Covered = false;
    bool Failed = false;
    bool UnknownShape = false;
};

struct TDiagnosticIssue {
    TString Id;
    TString Type;
    bool Red = false;
    std::optional<TString> Pile;
    std::vector<TString> Reasons;
};

struct TDiagnosticInput {
    bool RpcSuccess = false;
    bool SupportedOverallStatus = false;
    bool Truncated = false;
    bool MandatoryShapeUnknown = false;
    bool MissingStorageData = false;
    std::map<TString, TString> ExpectedGroups;
    std::vector<TPhysicalGroup> Groups;
    std::vector<TDiagnosticIssue> Issues;
};

bool IsRecognizedProfile(const TString& profile);
bool IsQualified(const TProfileQualification& qualification);
std::map<TString, TAssessment> ParseDiagnostic(
    const TClusterView& view,
    const TProfileQualification& qualification,
    const TDiagnosticInput& input);

TString ValidateObservation(
    const TObservation& observation,
    const TEpoch& epoch,
    const TString& processId,
    const TProfileQualification& qualification,
    TInstant now,
    TInstant roundDeadline,
    const TEvidenceTiming& timing = {});

TPileReport AggregatePile(
    const TFrozenSample& sample,
    const TClusterView& view,
    const std::vector<const TObservation*>& observations,
    const TProfileQualification& qualification,
    TInstant now,
    const TEvidenceTiming& timing = {});

struct TAdminReply {
    ui32 NodeId = 0;
    TClusterView View;
};

struct TAdminSelection {
    std::optional<TClusterView> View;
    TString Blocker;
};

TAdminSelection SelectAdminView(
    const std::vector<TFrozenSample>& samples,
    const std::vector<TAdminReply>& replies,
    const std::set<TString>& confirmedMembership,
    ui64 durableHighWater,
    TInstant now,
    TDuration ttl);

class TConfirmation {
public:
    bool CompleteRound(TDecision& decision, TInstant now, const TEvidenceTiming& timing = {});
    void Reset();

private:
    TString Key;
    ui64 RoundId = 0;
    TInstant StartedAt;
};

} // namespace NBridgeSkipper
