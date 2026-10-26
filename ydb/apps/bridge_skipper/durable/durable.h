#pragma once

#include <ydb/apps/bridge_skipper/domain/types.h>

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace NBridgeSkipper {

enum class ERequestedMode { Manual, Auto, Observe };
enum class EOperationKind { Failover, Switchover, Takedown, PrimaryMaintenance, Rejoin, Convert, AddPile, RemovePile };
enum class EOperationStatus { Queued, Executing, WaitingBackground, Paused, Reconciling, NeedsAttention, Succeeded, Rejected, Cancelled, Preempted, ExternallyResolved };
enum class EBridgeMethod { UpdateClusterState, Convert, AddPile, RemovePile };
enum class EAttemptDisposition { IntentCommitted, NeverDispatched, CompletedSuccess, CompletedRejection, CompletedWithBackground, AcceptedByObservation, Ambiguous, ExternallyResolved };
enum class ERpcDisposition { Pending, Success, Rejection, Background, Unknown };

struct TOwnershipAcknowledgement {
    bool ExclusiveAdministration = false;
    bool IndependentFailureDomain = false;
    TString DeploymentPlanReference;
    TString ClusterId;
    TString CallerId = "local";
    TString Reason;
    TString AuditTime;
    ui64 Revision = 0;
};

struct TWorkflowStep {
    EBridgeMethod Method = EBridgeMethod::UpdateClusterState;
    TExactUpdate Update;
    std::optional<TString> Target;
    TClusterView Baseline;
    TString ExpectedFingerprint;
};

struct TWorkflowPlan {
    EOperationKind Kind = EOperationKind::Failover;
    bool Automatic = false;
    bool PrerequisitesAcknowledged = false;
    bool EvidenceBound = false;
    std::optional<TString> Target;
    std::optional<TString> Successor;
    std::set<TString> FailedTargets;
    std::vector<TWorkflowStep> Steps;
    TString ImmutableArguments;
    TString SuppressionKey;
};

struct TPlanRequest {
    EOperationKind Kind = EOperationKind::Failover;
    std::optional<TString> Target;
    std::optional<TString> Successor;
    std::set<TString> FailedTargets;
    bool Automatic = false;
    bool ExternalPrerequisitesAcknowledged = false;
    bool DeletionAcknowledged = false;
    std::optional<TString> TypedPileName;
    bool EmptyNameAcknowledged = false;
    bool ResynchronizingTargetQualified = false;
};

struct TPlanResult {
    std::optional<TWorkflowPlan> Plan;
    TString Error;
    std::vector<TString> RequiredAcknowledgements;
};

// Narrow vectors only. No observations, actor objects, SDK objects or clocks.
TPlanResult PlanWorkflow(const TClusterView& view, const TPlanRequest& request);

struct TMutationAttempt {
    TString Id;
    TString OperationId;
    size_t Step = 0;
    ui64 WorkflowRevision = 0;
    EBridgeMethod Method = EBridgeMethod::UpdateClusterState;
    TExactUpdate Update;
    std::optional<TString> Target;
    TString Endpoint;
    ui32 NodeId = 0;
    TString ArgumentsHash;
    TString DesignatedMember;
    TString DesignatedProcess;
    ui64 IntentIndex = 0;
    TClusterView Baseline;
    ui64 TopologyRevision = 0;
    EAttemptDisposition Disposition = EAttemptDisposition::IntentCommitted;
    ERpcDisposition RpcDisposition = ERpcDisposition::Pending;
    bool Accepted = false;
    TString Proof;
    TString RpcDetail;
};

struct TBackgroundObligation {
    TString AttemptId;
    EOperationKind Kind = EOperationKind::Failover;
    std::optional<TString> Target;
    TExactUpdate IntendedUpdate;
    bool Complete = false;
    bool Superseded = false;
    TString Outcome;
};

struct TOperation {
    TString Id;
    TString RequestId;
    TWorkflowPlan Plan;
    EOperationStatus Status = EOperationStatus::Queued;
    ui64 Revision = 1;
    size_t NextStep = 0;
    std::vector<TString> Attempts;
    std::vector<TBackgroundObligation> Background;
    std::optional<TString> PreemptedBy;
    TString Outcome;
    ui64 CreatedIndex = 0;
    TString CreatedAt;
    TString UpdatedAt;
    TString CompletedAt;
    struct TAuditEvent {
        TString CommandId;
        ui64 Index = 0;
        TString Time;
        TString Code;
        TString Reason;
    };
    std::vector<TAuditEvent> Audit;
    std::vector<TClusterView> ObservedStates;
};

struct TCommandResult {
    bool Ok = false;
    TString Code;
    TString OperationId;
    ui64 Index = 0;
    ui64 ControllerRevision = 0;

    bool operator==(const TCommandResult&) const = default;
};

struct TStoredReceipt {
    TString PayloadHash;
    TCommandResult Result;
};

struct TDurableState {
    ui32 Schema = 1;
    TString ClusterId;
    TString MemberId;
    TString Backend = "local";
    TString MembershipIncarnation;
    ui64 AppliedIndex = 0;
    ui64 ControllerRevision = 0;
    ui64 AcceptedGenerationHighWater = 0;
    ERequestedMode RequestedMode = ERequestedMode::Manual;
    std::optional<TOwnershipAcknowledgement> Ownership;
    bool ExclusiveHistory = false;
    bool StateConflict = false;
    std::optional<TString> UnresolvedAttempt;
    std::optional<TString> OrdinaryOwner;
    std::map<TString, TOperation> Operations;
    std::map<TString, TMutationAttempt> Attempts;
    std::set<TString> SuppressedAutomaticPlans;
    std::map<TString, TStoredReceipt> Commands;
    std::map<TString, TStoredReceipt> Requests;
};

// Proof inputs are established by the gateway/controller from pinned fixtures.
// No bare desired-state similarity is an acceptance proof.
struct TAcceptanceProof {
    TClusterView Observed;
    ui64 TopologyRevision = 0;
    bool Fresh = false;
    bool IdentityVerified = false;
    bool OrderedAfterSend = false;
    bool Coherent = false;
    bool NoReplayPathQualified = false;
    bool MarkerQualified = false;
    bool CompleteExclusiveHistory = false;
    bool NoAlternativeProducer = false;
    bool CertifiedAutomaticFollowup = false;
    TString Provenance;
};

struct TRpcOutcome {
    TString AttemptId;
    TString DesignatedMember;
    TString DesignatedProcess;
    bool Success = false;
    TString Status;
    TString IssueCode;
    TString Issue;
    bool GenerationMismatchQualified = false;
    bool PinnedLifecycleErrorQualified = false;
    std::optional<TClusterView> Observed;
};

enum class ECommandKind { SetMode, AdmitOperation, AdmitAttempt, NeverDispatched, RpcOutcome, AcceptByObservation, Reconcile, Cancel, Resume, ResolveExternal, AdvanceGeneration };

struct TDurableCommand {
    ui32 Schema = 1;
    TString Id;
    std::optional<TString> RequestId;
    // Canonical HTTP action/body hash; duplicate lookup precedes revisions.
    TString RequestHash;
    TString AuditTime;
    TString Reason;
    ui64 ExpectedControllerRevision = 0;
    ECommandKind Kind = ECommandKind::SetMode;
    ERequestedMode Mode = ERequestedMode::Manual;
    std::optional<TOwnershipAcknowledgement> Ownership;
    TString OperationId;
    ui64 ExpectedWorkflowRevision = 0;
    TWorkflowPlan Plan;
    TMutationAttempt Attempt;
    TRpcOutcome Outcome;
    TAcceptanceProof Acceptance;
    TClusterView Observed;
    // Local facts are explicit admission inputs, never restored freshness.
    bool Ready = false;
    bool LocalStopped = false;
    bool Fresh = false;
    bool Coherent = false;
    // Complete fresh self-health for each exact pile, bound to Observed's
    // coherent fingerprint; one pile's health never completes another rejoin.
    std::set<TString> FreshHealthyPiles;
    bool ExecutorStopped = false;
    bool ServerWorkReconciled = false;
    bool AcknowledgeUnfencedRecovery = false;
    TString GatewayProof;
};

struct TApplyResult {
    TDurableState State;
    TCommandResult Result;
    bool Duplicate = false;
};

TApplyResult Apply(const TDurableState& state, const TDurableCommand& command);
TString ValidateAcceptance(const TDurableState& state, const TMutationAttempt& attempt, const TAcceptanceProof& proof);
bool IsAttemptClosed(const TMutationAttempt& attempt);
bool IsTerminal(EOperationStatus status);
// Empty when the next step has no intent and every earlier step is settled.
TString CancelError(const TDurableState& state, const TOperation& operation);
bool HandoverComplete(const TDurableState& state);
TString SerializeCommand(const TDurableCommand& command);
constexpr size_t MaxDurableRecordBytes = 256ULL * 1024 * 1024;

TDurableCommand DeserializeCommand(TStringBuf data);
TString SerializeState(const TDurableState& state);
TDurableState DeserializeState(TStringBuf data);
TString SerializeResult(const TCommandResult& result);
TCommandResult DeserializeResult(TStringBuf data);
TString DurableHash(TStringBuf data);
TString MutationArgumentsHash(const TMutationAttempt& attempt);

} // namespace NBridgeSkipper
