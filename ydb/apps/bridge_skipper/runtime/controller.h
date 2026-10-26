#pragma once

#include "events.h"

#include <ydb/apps/bridge_skipper/config/config.h>
#include <ydb/apps/bridge_skipper/domain/policy.h>
#include <ydb/apps/bridge_skipper/domain/scheduler.h>
#include <ydb/apps/bridge_skipper/domain/topology.h>
#include <ydb/library/actors/core/actor.h>

#include <deque>
#include <functional>
#include <memory>

namespace NBridgeSkipper {

class TRuntimeResources;

struct TDiagnosticSummary {
    TString Endpoint;
    ui32 NodeId = 0;
    std::optional<TString> Pile;
    TString Profile;
    ESourceKind Source = ESourceKind::Unsupported;
    TString Status;
    TInstant StartedAt;
    TInstant CheckedAt;
    std::optional<TDuration> Latency;
    bool Success = false;
    bool IdentityVerified = false;
    bool DirectLocalOrigin = false;
    TString Origin;
    TString Vantage;
    TString SnapshotId;
    std::vector<TString> Reasons;
    std::map<TString, TAssessment> Targets;
};

// Immutable publication; HTTP adds served-at/age using its own execution pool.
// Applied points at one installed durable prefix, never an uncommitted candidate.
struct TControllerSnapshot {
    TString ClusterId;
    TString IncarnationId;
    ui64 SnapshotRevision = 0;
    ui64 TopologyRevision = 0;
    ui64 ControllerRevision = 0;
    TString PolicyRevision;
    TInstant GeneratedAt;
    std::shared_ptr<const TDurableState> Applied;
    std::optional<TClusterView> Cluster;
    std::optional<TClusterView> UnsupportedCluster;
    TTopology Topology;
    TEpoch Epoch;
    std::optional<TDecision> Decision;
    std::vector<TPileReport> Reports;
    std::vector<TDiagnosticSummary> Diagnostics;
    std::vector<TString> Blockers;
    TString EffectiveState = "observing";
    bool AuthorityReady = false;
    bool AutonomyReady = false;
    bool MutationReady = false;
    // Dispatch permits the installed attempt's own unresolved slot. Admission
    // readiness above deliberately excludes every unresolved slot.
    bool DispatchReady = false;
    bool AutomaticDispatchReady = false;
    TInstant AdminValidUntil;
    TInstant ConfigValidUntil;
    ui64 ConfirmationGeneration = 0;
    bool ConfigStale = true;
    bool TopologyConfirmed = false;
    bool HandoverComplete = false;
    bool LocalStopped = false;
    ui64 LocalStopRevision = 0;
    size_t InFlightReads = 0;
    size_t InFlightHealth = 0;
    size_t SkippedReads = 0;
    size_t PendingCommands = 0;
    bool AdmissionUnknown = false;
    ui64 ClosedRounds = 0;
    ui64 DeadlineRounds = 0;
    ui64 AdmissionBudgetOverruns = 0;
    ui64 FreshnessMisses = 0;
    size_t LastRoundSampledNodes = 0;
    size_t LastRoundFreshNodes = 0;
    std::map<TString, ui64> EvidenceRejects;
    std::optional<TDuration> LastHandoffSlack;
    std::optional<TDuration> LastRoundCloseToHandoff;
};

struct TControllerSubmission {
    TDurableCommand Command;
    std::optional<TPlanRequest> PlanRequest;
    TString IncarnationId;
    ui64 TopologyRevision = 0;
    TString StateFingerprint;
    std::optional<TString> DecisionId;
};

struct TControllerCommandReply {
    TString Id;
    TProposalResult Result;
    bool Final = true;
};

struct TControllerEffects {
    struct THandoffSample {
        TString AttemptId;
        TDuration RoundCloseToHandoff;
        TDuration EvidenceSlack;
    };

    std::vector<TReadRequest> Reads;
    std::vector<TStorageRequest> Stores;
    std::vector<TDispatchRequest> Dispatches;
    std::vector<TString> CloseAttempts;
    std::vector<TControllerCommandReply> Replies;
    std::vector<THandoffSample> Handoffs;
};

struct TControllerOptions {
    TConfig Config;
    TDurableState Restored;
    TString MemberId;
    TString ProcessId;
    std::function<TString()> NextId;
    // Production initialization leaves these gates empty until supported-build
    // recordings qualify them. Tests inject explicitly identified fixtures.
    TProfileQualification EvidenceQualification;
    std::function<std::map<TString, TAssessment>(const TReadResult&, const TClusterView&)> EvidenceDecoder;
    std::function<void(TObservation&, const TReadResult&)> ProvenanceDecoder;
    bool NoReplayPathQualified = false;
    bool AcceptanceMarkersQualified = false;
    bool AutomaticFollowupQualified = false;
    TString AcceptanceProvenance;
};

// Single-owner deterministic controller state. No thread, actor address, SDK
// handle, filesystem operation, random generator or blocking wait lives here.
class TControllerCore {
public:
    explicit TControllerCore(TControllerOptions options);
    ~TControllerCore();
    TControllerCore(const TControllerCore&) = delete;
    TControllerCore& operator=(const TControllerCore&) = delete;
    void Tick();
    void Submit(TControllerSubmission submission);
    void OnRead(TReadResult result);
    void OnStorage(TStorageResult result);
    void OnMutation(TMutationResult result);
    void SetLocalStop(bool stopped, ui64 revision);
    void Shutdown();
    TControllerEffects DrainEffects();
    std::shared_ptr<const TControllerSnapshot> Snapshot() const;

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
};

struct TEvController {
    enum EEvents {
        EvSubmit = EventSpaceBegin(NActors::TEvents::ES_PRIVATE) + 0x7400,
        EvResult,
        EvWakeup,
        EvEnd,
    };

    struct TEvSubmit : NActors::TEventLocal<TEvSubmit, EvSubmit> {
        TControllerSubmission Submission;
        // The HTTP owner releases the reserved submission credit on final
        // completion, even when its original response waiter has disconnected.
        std::shared_ptr<void> AdmissionCredit;
    };

    struct TEvResult : NActors::TEventLocal<TEvResult, EvResult> {
        TControllerCommandReply Reply;
    };

    struct TEvWakeup : NActors::TEventLocal<TEvWakeup, EvWakeup> {
        ui64 Generation = 0;
    };
};

NActors::IActor* CreateControllerActor(const TConfig& config,
    std::shared_ptr<TRuntimeResources> resources, NActors::TActorId gateway);

} // namespace NBridgeSkipper
