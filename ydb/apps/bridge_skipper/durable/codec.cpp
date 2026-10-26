#include "durable.h"

#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>

#include <openssl/sha.h>
#include <util/generic/yexception.h>
#include <util/string/cast.h>
#include <util/string/hex.h>
#include <util/stream/str.h>

#include <tuple>
#include <type_traits>

namespace NBridgeSkipper {
namespace {
using J = NJson::TJsonValue;

template<class T, class Enable = void> struct TCodec;

// Durable struct layouts: every member is required and no other key is allowed.
template<class T> struct TFields {};

template<class T> struct TCodec<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    static J Write(T value) {
        return J(ToString(value));
    }

    static T Read(const J& value) {
        Y_ENSURE(value.IsString(), "integer must be decimal text");
        return FromString<T>(value.GetString());
    }
};

template<> struct TCodec<bool> {
    static J Write(bool value) {
        return J(value);
    }

    static bool Read(const J& value) {
        Y_ENSURE(value.IsBoolean(), "boolean required");
        return value.GetBoolean();
    }
};

template<> struct TCodec<TString> {
    static J Write(const TString& value) {
        return J(value);
    }

    static TString Read(const J& value) {
        Y_ENSURE(value.IsString(), "string required");
        return value.GetString();
    }
};

template<class T> struct TCodec<T, std::enable_if_t<std::is_enum_v<T>>> {
    static J Write(T value) {
        return TCodec<ui32>::Write(static_cast<ui32>(value));
    }

    static T Read(const J& value) {
        return static_cast<T>(TCodec<ui32>::Read(value));
    }
};

template<class T> struct TCodec<std::optional<T>> {
    static J Write(const std::optional<T>& value) {
        return value ? TCodec<T>::Write(*value) : J(NJson::JSON_NULL);
    }

    static std::optional<T> Read(const J& value) {
        return value.IsNull() ? std::nullopt : std::optional<T>(TCodec<T>::Read(value));
    }
};

template<class T> struct TCodec<std::vector<T>> {
    static J Write(const std::vector<T>& values) {
        J out(NJson::JSON_ARRAY);
        for (const auto& value : values) {
            out.AppendValue(TCodec<T>::Write(value));
        }
        return out;
    }

    static std::vector<T> Read(const J& value) {
        Y_ENSURE(value.IsArray(), "array required");
        std::vector<T> out;
        for (const auto& item : value.GetArray()) {
            out.push_back(TCodec<T>::Read(item));
        }
        return out;
    }
};

template<class T> struct TCodec<std::set<T>> {
    static J Write(const std::set<T>& values) {
        return TCodec<std::vector<T>>::Write({values.begin(), values.end()});
    }

    static std::set<T> Read(const J& value) {
        auto values = TCodec<std::vector<T>>::Read(value);
        std::set<T> out(values.begin(), values.end());
        Y_ENSURE(out.size() == values.size(), "duplicate set member");
        return out;
    }
};

template<class T> struct TCodec<std::map<TString, T>> {
    static J Write(const std::map<TString, T>& values) {
        J out(NJson::JSON_MAP);
        for (const auto& [key, value] : values) {
            out[key] = TCodec<T>::Write(value);
        }
        return out;
    }

    static std::map<TString, T> Read(const J& value) {
        Y_ENSURE(value.IsMap(), "map required");
        std::map<TString, T> out;
        for (const auto& [key, item] : value.GetMap()) {
            out.emplace(key, TCodec<T>::Read(item));
        }
        return out;
    }
};

const J& Required(const J& object, TStringBuf key) {
    Y_ENSURE(object.IsMap() && object.Has(key), "missing durable field " << key);
    return object[key];
}

template<class T, class M> struct TField {
    const char* Name;
    M T::* Member;
};

template<class T, class M> constexpr TField<T, M> Field(const char* name, M T::* member) {
    return {name, member};
}

#define SKIPPER_FIELD(name) Field(#name, &TSelf::name)

template<> struct TFields<TPendingOperation> {
    using TSelf = TPendingOperation;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Kind),
        SKIPPER_FIELD(Pile),
        SKIPPER_FIELD(Error),
        SKIPPER_FIELD(Progress));
};

template<> struct TFields<TClusterView> {
    using TSelf = TClusterView;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Generation),
        SKIPPER_FIELD(PileOrder),
        SKIPPER_FIELD(Piles),
        SKIPPER_FIELD(Converted),
        SKIPPER_FIELD(ConversionInProgress),
        SKIPPER_FIELD(Pending),
        SKIPPER_FIELD(Metadata),
        SKIPPER_FIELD(Endpoint));
};

template<> struct TFields<TOwnershipAcknowledgement> {
    using TSelf = TOwnershipAcknowledgement;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(ExclusiveAdministration),
        SKIPPER_FIELD(IndependentFailureDomain),
        SKIPPER_FIELD(DeploymentPlanReference),
        SKIPPER_FIELD(ClusterId),
        SKIPPER_FIELD(CallerId),
        SKIPPER_FIELD(Reason),
        SKIPPER_FIELD(AuditTime),
        SKIPPER_FIELD(Revision));
};

template<> struct TFields<TWorkflowStep> {
    using TSelf = TWorkflowStep;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Method),
        SKIPPER_FIELD(Update),
        SKIPPER_FIELD(Target),
        SKIPPER_FIELD(Baseline),
        SKIPPER_FIELD(ExpectedFingerprint));
};

template<> struct TFields<TWorkflowPlan> {
    using TSelf = TWorkflowPlan;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Kind),
        SKIPPER_FIELD(Automatic),
        SKIPPER_FIELD(PrerequisitesAcknowledged),
        SKIPPER_FIELD(EvidenceBound),
        SKIPPER_FIELD(Target),
        SKIPPER_FIELD(Successor),
        SKIPPER_FIELD(FailedTargets),
        SKIPPER_FIELD(Steps),
        SKIPPER_FIELD(ImmutableArguments),
        SKIPPER_FIELD(SuppressionKey));
};

template<> struct TFields<TMutationAttempt> {
    using TSelf = TMutationAttempt;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Id),
        SKIPPER_FIELD(OperationId),
        SKIPPER_FIELD(Step),
        SKIPPER_FIELD(WorkflowRevision),
        SKIPPER_FIELD(Method),
        SKIPPER_FIELD(Update),
        SKIPPER_FIELD(Target),
        SKIPPER_FIELD(Endpoint),
        SKIPPER_FIELD(NodeId),
        SKIPPER_FIELD(ArgumentsHash),
        SKIPPER_FIELD(DesignatedMember),
        SKIPPER_FIELD(DesignatedProcess),
        SKIPPER_FIELD(IntentIndex),
        SKIPPER_FIELD(Baseline),
        SKIPPER_FIELD(TopologyRevision),
        SKIPPER_FIELD(Disposition),
        SKIPPER_FIELD(RpcDisposition),
        SKIPPER_FIELD(Accepted),
        SKIPPER_FIELD(Proof),
        SKIPPER_FIELD(RpcDetail));
};

template<> struct TFields<TBackgroundObligation> {
    using TSelf = TBackgroundObligation;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(AttemptId),
        SKIPPER_FIELD(Kind),
        SKIPPER_FIELD(Target),
        SKIPPER_FIELD(IntendedUpdate),
        SKIPPER_FIELD(Complete),
        SKIPPER_FIELD(Superseded),
        SKIPPER_FIELD(Outcome));
};

template<> struct TFields<TOperation::TAuditEvent> {
    using TSelf = TOperation::TAuditEvent;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(CommandId),
        SKIPPER_FIELD(Index),
        SKIPPER_FIELD(Time),
        SKIPPER_FIELD(Code),
        SKIPPER_FIELD(Reason));
};

template<> struct TFields<TOperation> {
    using TSelf = TOperation;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Id),
        SKIPPER_FIELD(RequestId),
        SKIPPER_FIELD(Plan),
        SKIPPER_FIELD(Status),
        SKIPPER_FIELD(Revision),
        SKIPPER_FIELD(NextStep),
        SKIPPER_FIELD(Attempts),
        SKIPPER_FIELD(Background),
        SKIPPER_FIELD(PreemptedBy),
        SKIPPER_FIELD(Outcome),
        SKIPPER_FIELD(CreatedIndex),
        SKIPPER_FIELD(CreatedAt),
        SKIPPER_FIELD(UpdatedAt),
        SKIPPER_FIELD(CompletedAt),
        SKIPPER_FIELD(Audit),
        SKIPPER_FIELD(ObservedStates));
};

template<> struct TFields<TCommandResult> {
    using TSelf = TCommandResult;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Ok),
        SKIPPER_FIELD(Code),
        SKIPPER_FIELD(OperationId),
        SKIPPER_FIELD(Index),
        SKIPPER_FIELD(ControllerRevision));
};

template<> struct TFields<TStoredReceipt> {
    using TSelf = TStoredReceipt;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(PayloadHash),
        SKIPPER_FIELD(Result));
};

template<> struct TFields<TDurableState> {
    using TSelf = TDurableState;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Schema),
        SKIPPER_FIELD(ClusterId),
        SKIPPER_FIELD(MemberId),
        SKIPPER_FIELD(Backend),
        SKIPPER_FIELD(MembershipIncarnation),
        SKIPPER_FIELD(AppliedIndex),
        SKIPPER_FIELD(ControllerRevision),
        SKIPPER_FIELD(AcceptedGenerationHighWater),
        SKIPPER_FIELD(RequestedMode),
        SKIPPER_FIELD(Ownership),
        SKIPPER_FIELD(ExclusiveHistory),
        SKIPPER_FIELD(StateConflict),
        SKIPPER_FIELD(UnresolvedAttempt),
        SKIPPER_FIELD(OrdinaryOwner),
        SKIPPER_FIELD(Operations),
        SKIPPER_FIELD(Attempts),
        SKIPPER_FIELD(SuppressedAutomaticPlans),
        SKIPPER_FIELD(Commands),
        SKIPPER_FIELD(Requests));
};

template<> struct TFields<TAcceptanceProof> {
    using TSelf = TAcceptanceProof;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Observed),
        SKIPPER_FIELD(TopologyRevision),
        SKIPPER_FIELD(Fresh),
        SKIPPER_FIELD(IdentityVerified),
        SKIPPER_FIELD(OrderedAfterSend),
        SKIPPER_FIELD(Coherent),
        SKIPPER_FIELD(NoReplayPathQualified),
        SKIPPER_FIELD(MarkerQualified),
        SKIPPER_FIELD(CompleteExclusiveHistory),
        SKIPPER_FIELD(NoAlternativeProducer),
        SKIPPER_FIELD(CertifiedAutomaticFollowup),
        SKIPPER_FIELD(Provenance));
};

template<> struct TFields<TRpcOutcome> {
    using TSelf = TRpcOutcome;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(AttemptId),
        SKIPPER_FIELD(DesignatedMember),
        SKIPPER_FIELD(DesignatedProcess),
        SKIPPER_FIELD(Success),
        SKIPPER_FIELD(Status),
        SKIPPER_FIELD(IssueCode),
        SKIPPER_FIELD(Issue),
        SKIPPER_FIELD(GenerationMismatchQualified),
        SKIPPER_FIELD(PinnedLifecycleErrorQualified),
        SKIPPER_FIELD(Observed));
};

template<> struct TFields<TDurableCommand> {
    using TSelf = TDurableCommand;
    static constexpr auto Value = std::make_tuple(
        SKIPPER_FIELD(Schema),
        SKIPPER_FIELD(Id),
        SKIPPER_FIELD(RequestId),
        SKIPPER_FIELD(RequestHash),
        SKIPPER_FIELD(AuditTime),
        SKIPPER_FIELD(Reason),
        SKIPPER_FIELD(ExpectedControllerRevision),
        SKIPPER_FIELD(Kind),
        SKIPPER_FIELD(Mode),
        SKIPPER_FIELD(Ownership),
        SKIPPER_FIELD(OperationId),
        SKIPPER_FIELD(ExpectedWorkflowRevision),
        SKIPPER_FIELD(Plan),
        SKIPPER_FIELD(Attempt),
        SKIPPER_FIELD(Outcome),
        SKIPPER_FIELD(Acceptance),
        SKIPPER_FIELD(Observed),
        SKIPPER_FIELD(Ready),
        SKIPPER_FIELD(LocalStopped),
        SKIPPER_FIELD(Fresh),
        SKIPPER_FIELD(Coherent),
        SKIPPER_FIELD(FreshHealthyPiles),
        SKIPPER_FIELD(ExecutorStopped),
        SKIPPER_FIELD(ServerWorkReconciled),
        SKIPPER_FIELD(AcknowledgeUnfencedRecovery),
        SKIPPER_FIELD(GatewayProof));
};
#undef SKIPPER_FIELD

template<class T> struct TCodec<T, std::void_t<decltype(TFields<T>::Value)>> {
    static J Write(const T& value) {
        J out(NJson::JSON_MAP);
        std::apply([&](const auto&... field) {
            ((out[field.Name] = TCodec<std::remove_cvref_t<decltype(value.*field.Member)>>::Write(value.*field.Member)), ...);
        }, TFields<T>::Value);
        return out;
    }

    static T Read(const J& object) {
        Y_ENSURE(object.IsMap() && object.GetMap().size() == std::tuple_size_v<decltype(TFields<T>::Value)>, "durable object field mismatch");
        T value;
        std::apply([&](const auto&... field) {
            ((value.*field.Member = TCodec<std::remove_cvref_t<decltype(value.*field.Member)>>::Read(Required(object, field.Name))), ...);
        }, TFields<T>::Value);
        return value;
    }
};

template<class T> TString Encode(const T& value) {
    NJson::TJsonWriterConfig config;
    config.SortKeys = true;
    TStringStream out;
    const auto json = TCodec<T>::Write(value);
    NJson::WriteJson(&out, &json, config);
    return out.Str();
}

template<class T> T Decode(TStringBuf data) {
    Y_ENSURE(data.size() <= MaxDurableRecordBytes, "durable record too large");
    J value;
    NJson::TJsonReaderConfig config;
    config.MaxDepth = 64;
    NJson::ReadJsonTree(data, &config, &value, true);
    T out = TCodec<T>::Read(value);
    Y_ENSURE(Encode(out) == data, "noncanonical durable record");
    return out;
}

} // namespace

TString DurableHash(TStringBuf data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    return HexEncode(hash, sizeof(hash));
}

TString SerializeCommand(const TDurableCommand& value) {
    return Encode(value);
}

TDurableCommand DeserializeCommand(TStringBuf data) {
    auto value = Decode<TDurableCommand>(data);
    Y_ENSURE(value.Schema == 1 && static_cast<ui32>(value.Kind) <= static_cast<ui32>(ECommandKind::AdvanceGeneration), "unsupported command schema/kind");
    return value;
}

TString SerializeState(const TDurableState& value) {
    return Encode(value);
}

TDurableState DeserializeState(TStringBuf data) {
    auto value = Decode<TDurableState>(data);
    Y_ENSURE(value.Schema == 1 && static_cast<ui32>(value.RequestedMode) <= static_cast<ui32>(ERequestedMode::Observe), "unsupported state schema/mode");
    return value;
}

TString SerializeResult(const TCommandResult& value) {
    return Encode(value);
}

TCommandResult DeserializeResult(TStringBuf data) {
    return Decode<TCommandResult>(data);
}

TString MutationArgumentsHash(const TMutationAttempt& value) {
    J args(NJson::JSON_MAP);
    args["method"] = TCodec<EBridgeMethod>::Write(value.Method);
    args["update"] = TCodec<TExactUpdate>::Write(value.Update);
    args["target"] = TCodec<std::optional<TString>>::Write(value.Target);
    args["preflight_generation"] = TCodec<ui64>::Write(value.Baseline.Generation);
    args["quorum_piles"] = J(NJson::JSON_ARRAY);
    NJson::TJsonWriterConfig config;
    config.SortKeys = true;
    TStringStream out;
    NJson::WriteJson(&out, &args, config);
    return DurableHash(out.Str());
}

} // namespace NBridgeSkipper
