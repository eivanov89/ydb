#pragma once

#include <ydb/apps/bridge_skipper/durable/store.h>
#include <ydb/library/actors/core/event_local.h>
#include <ydb/library/actors/core/events.h>

#include <functional>
#include <memory>

namespace Ydb::Monitoring {
class SelfCheckResult;
}

namespace NBridgeSkipper {

enum class EReadMethod { Admin, Health, Config, Discovery };

struct TReadRequest {
    TString Id;
    TString ProcessId;
    TEpoch Epoch;
    TString Endpoint;
    EReadMethod Method = EReadMethod::Admin;
    TDuration Timeout = TDuration::Seconds(10);
    TDuration OperationTimeout = TDuration::Seconds(8);
    TInstant Deadline;
    bool OperationPriority = false;
    bool Bracket = false;
    bool IdentityOnly = false;
};

struct TDiscoveredEndpoint {
    TString Endpoint;
    ui32 NodeId = 0;
    // SDK Discovery's scalar pile cannot establish field presence or binding.
    TString SupplementaryPile;
};

struct TReadResult {
    TReadRequest Request;
    bool Success = false;
    bool Busy = false;
    TString Status;
    TString IssueCode;
    TString Issue;
    std::optional<TClusterView> Cluster;
    std::shared_ptr<const Ydb::Monitoring::SelfCheckResult> Health;
    std::vector<TString> Configs;
    std::vector<TDiscoveredEndpoint> Discovered;
    TInstant StartedAt;
    TInstant FinishedAt;
    ui64 ResponseBytes = 0;
};

// A live permission is only constructed following this process's commit reply.
// It is never serialized, restored from a snapshot, or recreated on restart.
struct TDispatchPermission {
    TString Id;
    TString PreflightRequestId;
    TString ProcessId;
    TString ArgumentsHash;
    ui64 IntentIndex = 0;
    TInstant ExpiresAt;
    TInstant EvidenceExpiry;
    bool Automatic = false;
    TEpoch Epoch;
    ui64 ConfirmationGeneration = 0;
};

struct TDispatchRequest {
    TMutationAttempt Attempt;
    TDispatchPermission Permission;
};

struct TMutationResult {
    bool Terminal = true;
    TString AttemptId;
    TString ProcessId;
    bool HandedOff = false;
    bool NeverDispatched = false;
    TString GatewayProof;
    TRpcOutcome Outcome;
    TInstant HandoffAt;
    TInstant FinishedAt;
    TDuration EvidenceSlack;
};

enum class EStorageKind { Commit, ResolveUnknown, LookupCommand, LookupRequest, Checkpoint, Audit, LocalStopAudit };

struct TStorageRequest {
    TString Id;
    EStorageKind Kind = EStorageKind::Commit;
    std::shared_ptr<const TDurableState> State;
    TDurableCommand Command;
    TString LookupId;
    ui64 After = 0;
    size_t Limit = 50;
};

struct TStorageResult {
    TString Id;
    EStorageKind Kind = EStorageKind::Commit;
    TProposalResult Proposal;
    std::optional<TCommandResult> Command;
    std::optional<TStoredReceipt> Receipt;
    bool Success = false;
    TString Error;
    TStoreStats Stats;
    std::vector<TDurableAuditEntry> Audit;
    bool HasMore = false;
    std::vector<TString> LocalStopAudit;
    bool LocalStopAuditTruncated = false;
};

struct TEvRuntime {
    enum EEvents {
        EvGatewayRead = EventSpaceBegin(NActors::TEvents::ES_PRIVATE) + 0x7200,
        EvGatewayReadResult,
        EvDispatchAttempt,
        EvMutationResult,
        EvCloseAttempt,
        EvGatewayQuiesce,
        EvGatewayQuiesced,
        EvSdkReadComplete,
        EvSdkMutationComplete,
        EvStorageResult = EventSpaceBegin(NActors::TEvents::ES_PRIVATE) + 0x7300,
        EvLocalStopChanged,
        EvEnd,
    };

    struct TEvGatewayRead : NActors::TEventLocal<TEvGatewayRead, EvGatewayRead> {
        TReadRequest Request;
        explicit TEvGatewayRead(TReadRequest request)
            : Request(std::move(request))
        {
        }
    };

    struct TEvGatewayReadResult : NActors::TEventLocal<TEvGatewayReadResult, EvGatewayReadResult> {
        TReadResult Result;
        explicit TEvGatewayReadResult(TReadResult result)
            : Result(std::move(result))
        {
        }
    };

    struct TEvDispatchAttempt : NActors::TEventLocal<TEvDispatchAttempt, EvDispatchAttempt> {
        TDispatchRequest Request;
        explicit TEvDispatchAttempt(TDispatchRequest request)
            : Request(std::move(request))
        {
        }
    };

    struct TEvMutationResult : NActors::TEventLocal<TEvMutationResult, EvMutationResult> {
        TMutationResult Result;
        explicit TEvMutationResult(TMutationResult result)
            : Result(std::move(result))
        {
        }
    };

    struct TEvCloseAttempt : NActors::TEventLocal<TEvCloseAttempt, EvCloseAttempt> {
        TString AttemptId;
        TString ProcessId;
    };

    struct TEvGatewayQuiesce : NActors::TEventLocal<TEvGatewayQuiesce, EvGatewayQuiesce> {
    };

    struct TEvGatewayQuiesced : NActors::TEventLocal<TEvGatewayQuiesced, EvGatewayQuiesced> {
        size_t Reads = 0;
        size_t Mutations = 0;
    };

    struct TEvStorageResult : NActors::TEventLocal<TEvStorageResult, EvStorageResult> {
        TStorageResult Result;
        explicit TEvStorageResult(TStorageResult result)
            : Result(std::move(result))
        {
        }
    };

    struct TEvLocalStopChanged : NActors::TEventLocal<TEvLocalStopChanged, EvLocalStopChanged> {
        ui64 Revision = 0;
    };
};

} // namespace NBridgeSkipper
