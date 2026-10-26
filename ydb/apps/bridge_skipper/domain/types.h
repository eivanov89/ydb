#pragma once

#include <util/datetime/base.h>
#include <util/generic/string.h>
#include <util/system/types.h>

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace NBridgeSkipper {

// Public wire values are retained, including unknown future enum values.
enum class EPileState : ui32 {
    Unspecified = 0,
    Primary = 1,
    Promoted = 2,
    Synchronized = 3,
    NotSynchronized = 4,
    Suspended = 5,
    Disconnected = 6,
    Inactive = 7,
};

using TNamedPileStates = std::map<TString, EPileState>;
using TExactUpdate = TNamedPileStates;

struct TPendingOperation {
    TString Kind;
    // Presence is distinct from the valid empty pile name.
    std::optional<TString> Pile;
    TString Error;
    TString Progress;

    bool operator==(const TPendingOperation&) const = default;
};

struct TClusterView {
    ui64 Generation = 0;
    std::vector<TString> PileOrder;
    TNamedPileStates Piles;
    bool Converted = false;
    bool ConversionInProgress = false;
    std::vector<TPendingOperation> Pending;
    // Canonical lossless additional public metadata, not a generation proxy.
    std::map<TString, TString> Metadata;
    TString Endpoint;
    TInstant ReceivedAt;
};

enum class EBinding { Explicit, ImplicitVerified, ImplicitUnresolved, Conflict };

struct TNode {
    TString LogicalId;
    std::optional<TString> Pile;
    EBinding Binding = EBinding::ImplicitUnresolved;
    TString Host;
    TString Endpoint;
    ui32 NodeId = 0;
    ui32 InventoryNodeId = 0; // configured expectation, never voting identity
    bool DirectEndpoint = false;
    TString Source;
    ui64 Revision = 0;
    TString AdminFingerprint;
    TInstant AdminObservedAt;
};

enum class ESourceKind { Unsupported, GlobalBsc, LocalWhiteboard, LocalNodewarden };
enum class EAssessment { Unknown, NoFailureReported, Failed };

struct TAssessment {
    EAssessment Value = EAssessment::Unknown;
    bool Complete = false;
    std::vector<TString> Reasons;
};

struct TProfileQualification {
    TString Profile;
    TString SupportedBuild;
    TString FixtureRecord;
    ESourceKind Source = ESourceKind::Unsupported;
    bool Qualified = false;
    bool CapabilityPresent = false;
    bool IndependentLocalOrigins = false;
    bool CoversResynchronizing = false;
};

struct TEpoch {
    ui64 TopologyRevision = 0;
    TString AdminFingerprint;
    TString PolicyRevision;
    ui64 ReadinessGeneration = 0;

    bool operator==(const TEpoch&) const = default;
};

struct TObservation {
    TString RequestId;
    TString ProcessId;
    ui64 RoundId = 0;
    TEpoch Epoch;
    ui32 NodeId = 0;
    std::optional<TString> ResponderPile;
    TInstant BeforeAt;
    TInstant HealthStartedAt;
    TInstant FinishedAt;
    TString BeforeFingerprint;
    TString AfterFingerprint;
    TString Profile;
    ESourceKind Source = ESourceKind::Unsupported;
    TString Vantage;
    TString Origin;
    TString SnapshotId;
    bool IdentityVerified = false;
    bool DirectLocalOrigin = false;
    std::map<TString, TAssessment> Targets;
    std::vector<TString> Reasons;
};

struct TFrozenSample {
    TString Pile;
    std::vector<ui32> Nodes;
    size_t Threshold = 0;
};

struct TPileReport {
    TFrozenSample Sample;
    size_t Received = 0;
    size_t Fresh = 0;
    TString AdminFingerprint;
    bool Eligible = false;
    TString Origin;
    std::set<TString> Origins;
    std::map<TString, TAssessment> Targets;
    std::map<TString, size_t> TargetNumerators;
    std::vector<ui32> Missing;
    std::vector<TString> Reasons;
};

enum class EDecisionKind { Stable, Blocked, Failover };

struct TDecision {
    TString Id;
    EDecisionKind Kind = EDecisionKind::Blocked;
    TString Reason;
    std::optional<TString> Candidate;
    std::optional<TString> Primary;
    std::set<TString> Targets;
    TExactUpdate Update;
    TEpoch Epoch;
    TString Profile;
    ESourceKind Source = ESourceKind::Unsupported;
    ui64 RoundId = 0;
    TInstant RoundStartedAt;
    TInstant EvidenceExpiry;
    std::optional<TNode> Endpoint;
    std::vector<TString> EvidenceRequestIds;
    // Exactly the observations required by the selected fixed-sample proofs.
    // GlobalAssessment is one whole corroborated vector, never per-row merging.
    std::vector<TObservation> Evidence;
    std::map<TString, TAssessment> GlobalAssessment;
    bool Confirmed = false;
};

bool IsKnownState(EPileState state);
bool IsSynchronized(EPileState state);
bool IsActive(EPileState state);
TString CanonicalFingerprint(const TClusterView& view);
TString DomainHash(const TString& value);
TString CanonicalNamedState(const TClusterView& view);
TString CanonicalDecision(const TDecision& decision);
TString ValidateClusterView(const TClusterView& view);
TString ValidateExactUpdate(const TClusterView& view, const TExactUpdate& update);

} // namespace NBridgeSkipper
