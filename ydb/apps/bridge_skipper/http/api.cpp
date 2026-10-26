#include "internal.h"

#include <library/cpp/monlib/dynamic_counters/encode.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>
#include <library/cpp/string_utils/base64/base64.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <util/string/cast.h>
#include <util/string/hex.h>

#include <algorithm>
#include <cctype>

namespace NBridgeSkipper {
namespace {

TString Sign(TStringBuf key, TStringBuf payload) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    Require(HMAC(EVP_sha256(), key.data(), key.size(), reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), hash, &length) && length == 32, "Signing unavailable", 503, "SIGNING_UNAVAILABLE");
    return HexEncode(hash, length);
}

TString RequestHash(const TApiRequest& request, const TApiJson& body) {
    return DurableHash(request.Method + "\n" + request.Target + "\n" + CanonicalApiJson(body));
}

TApiJson Block(TString code) {
    auto value = Object();
    value["code"] = code;
    value["reason"] = BoundedDiagnostic(code);
    return value;
}

std::map<TString, TString> Query(TStringBuf query) {
    std::map<TString, TString> fields;
    while (!query.empty()) {
        const size_t end = query.find('&');
        const auto pair = query.SubStr(0, end);
        const auto equals = pair.find('=');
        Require(equals != TStringBuf::npos, "Query fields require values");
        const TString key(pair.SubStr(0, equals));
        const TString value(pair.SubStr(equals + 1));
        Require(!key.empty() && !value.Contains('%') && !key.Contains('%') && fields.emplace(key, value).second, "Invalid or duplicate query field");
        if (end == TStringBuf::npos) {
            break;
        }
        query.Skip(end + 1);
        Require(!query.empty(), "Trailing query separator");
    }
    return fields;
}

size_t Limit(const std::map<TString, TString>& query, size_t fallback, size_t maximum) {
    const auto it = query.find("limit");
    const ui64 value = it == query.end() ? fallback : Decimal(it->second);
    Require(value > 0 && value <= maximum, "Pagination limit out of range");
    return value;
}

ui64 After(const std::map<TString, TString>& query) {
    const auto it = query.find("after");
    return it == query.end() ? 0 : Decimal(it->second);
}

void QueryKeys(const std::map<TString, TString>& query, std::initializer_list<TStringBuf> keys) {
    for (const auto& [key, value] : query) {
        Y_UNUSED(value);
        Require(std::find(keys.begin(), keys.end(), key) != keys.end(), "Unknown query field");
    }
}

TClusterView PlanningView(const TControllerSnapshot& snapshot) {
    if (snapshot.Cluster) {
        return *snapshot.Cluster;
    }
    TClusterView view;
    return view;
}

struct TActionPlan {
    TString Action;
    TApiJson Arguments;
    TPlanRequest Request;
    TWorkflowPlan Plan;
    std::optional<TString> DecisionId;
    std::optional<TString> ResumeOperation;
    ui64 WorkflowRevision = 0;
    size_t NextStep = 0;
    TApiJson Acknowledgements = Array();
    TApiJson Blockers = Array();
    bool Dispatchable = true;
};

TString DecisionEvidenceHash(const TDecision& decision) {
    auto evidence = Object();
    evidence["decision"] = CanonicalDecision(decision);
    evidence["expires_us"] = ToString(decision.EvidenceExpiry.MicroSeconds());
    evidence["request_ids"] = Array();
    for (const auto& id : decision.EvidenceRequestIds) {
        evidence["request_ids"].AppendValue(id);
    }
    evidence["endpoint"] = Null();
    if (decision.Endpoint) {
        auto endpoint = Object();
        endpoint["uri"] = decision.Endpoint->Endpoint;
        endpoint["node_id"] = ToString(decision.Endpoint->NodeId);
        endpoint["pile"] = decision.Endpoint->Pile ? TApiJson(*decision.Endpoint->Pile) : Null();
        endpoint["revision"] = ToString(decision.Endpoint->Revision);
        evidence["endpoint"] = std::move(endpoint);
    }
    return DurableHash(CanonicalApiJson(evidence));
}

TActionPlan PlanAction(const TApiJson& body, const TControllerSnapshot& snapshot, const TLocalStopReceipt& stop) {
    TActionPlan result;
    result.Action = String(body, "action", 64);
    result.Arguments = body;
    const auto& action = result.Action;
    if (action == "apply_decision") {
        Fields(body, {"action", "decision_id"});
        const auto decisionId = String(body, "decision_id", 36);
        Require(IsRequestUuid(decisionId), "decision_id must be a UUID");
        Require(snapshot.Decision && snapshot.Decision->Id == decisionId, "Decision changed; request a fresh preview", 409, "STALE_PLAN");
        result.DecisionId = decisionId;
        result.Request.Kind = EOperationKind::Failover;
        result.Request.FailedTargets = snapshot.Decision->Targets;
        if (snapshot.Cluster) {
            for (const auto& [name, state] : snapshot.Cluster->Piles) {
                if (state == EPileState::Primary && snapshot.Decision->Targets.contains(name)) {
                    result.Request.Successor = snapshot.Decision->Candidate;
                }
            }
        }
        // Manual application of a qualified displayed decision still uses all
        // controller decision/freshness checks; it does not enable Auto.
        result.Request.Automatic = false;
        if (snapshot.Decision->Kind != EDecisionKind::Failover || !snapshot.Decision->Confirmed || !snapshot.Decision->Endpoint) {
            result.Blockers.AppendValue(Block("DECISION_NOT_DISPATCHABLE"));
        }
    } else if (action == "resume_workflow") {
        Fields(body, {"action", "operation_id"});
        const auto id = String(body, "operation_id", 36);
        Require(IsRequestUuid(id), "operation_id must be a UUID");
        Require(snapshot.Applied && snapshot.Applied->Operations.contains(id), "Unknown operation", 404, "OPERATION_NOT_FOUND");
        const auto& operation = snapshot.Applied->Operations.at(id);
        result.ResumeOperation = id;
        result.Plan = operation.Plan;
        result.WorkflowRevision = operation.Revision;
        result.NextStep = operation.NextStep;
        if (operation.Status != EOperationStatus::Paused || operation.NextStep >= operation.Plan.Steps.size() || !std::all_of(operation.Background.begin(), operation.Background.end(), [](const auto& work) {
            return work.Complete;
        })) {
            result.Blockers.AppendValue(Block("WORKFLOW_NOT_RESUMABLE"));
        }
        if (operation.Plan.EvidenceBound) {
            // Rebind only the evidence for the original remaining arguments.
            // A replacement decision cannot replan or redirect this operation.
            bool matches = false;
            if (operation.NextStep < operation.Plan.Steps.size() && snapshot.Decision) {
                const auto& step = operation.Plan.Steps[operation.NextStep];
                const auto& decision = *snapshot.Decision;
                const bool candidateMatches = decision.Candidate && snapshot.Cluster && snapshot.Cluster->Piles.contains(*decision.Candidate)
                    && IsSynchronized(snapshot.Cluster->Piles.at(*decision.Candidate))
                    && (!step.Update.contains(*decision.Candidate) || step.Update.at(*decision.Candidate) != EPileState::Disconnected)
                    && (!operation.Plan.Successor || decision.Candidate == operation.Plan.Successor);
                matches = operation.Plan.Kind == EOperationKind::Failover && step.Method == EBridgeMethod::UpdateClusterState
                    && decision.Kind == EDecisionKind::Failover && decision.Confirmed && decision.Endpoint
                    && decision.Epoch == snapshot.Epoch && decision.Update == step.Update && candidateMatches
                    && (step.ExpectedFingerprint.empty() || (snapshot.Cluster && step.ExpectedFingerprint == CanonicalFingerprint(*snapshot.Cluster)));
                if (matches) {
                    result.DecisionId = decision.Id;
                }
            }
            if (!matches) {
                result.Blockers.AppendValue(Block("MATCHING_CONFIRMED_DECISION_REQUIRED"));
            }
        }
    } else if (action == "failover") {
        Fields(body, {"action", "targets", "successor"});
        Require(body["targets"].IsArray() && !body["targets"].GetArray().empty() && body["targets"].GetArray().size() <= 1024, "Nonempty bounded targets required");
        for (const auto& target : body["targets"].GetArray()) {
            Require(target.IsString() && target.GetString().size() <= 128 && result.Request.FailedTargets.insert(target.GetString()).second, "Invalid or duplicate exact target");
        }
        result.Request.Kind = EOperationKind::Failover;
        result.Request.Successor = NullableString(body, "successor");
        // Set order is semantic; canonicalize it before signing/recomputation.
        result.Arguments["targets"] = Array();
        for (const auto& target : result.Request.FailedTargets) {
            result.Arguments["targets"].AppendValue(target);
        }
    } else if (action == "convert") {
        Fields(body, {"action"});
        result.Request.Kind = EOperationKind::Convert;
    } else {
        const std::map<TString, EOperationKind> kinds = {{"switchover", EOperationKind::Switchover}, {"takedown", EOperationKind::Takedown}, {"primary_maintenance", EOperationKind::PrimaryMaintenance}, {"rejoin", EOperationKind::Rejoin}, {"add_pile", EOperationKind::AddPile}, {"remove_pile", EOperationKind::RemovePile}};
        Require(kinds.contains(action), "Unknown action");
        result.Request.Kind = kinds.at(action);
        if (action == "primary_maintenance") {
            Fields(body, {"action", "target", "successor"});
            result.Request.Successor = String(body, "successor", 128);
        } else {
            Fields(body, {"action", "target"});
        }
        const auto target = String(body, "target", 128);
        if (action == "switchover") {
            result.Request.Successor = target;
        } else {
            result.Request.Target = target;
        }
    }
    if (!result.ResumeOperation) {
        const auto planned = PlanWorkflow(PlanningView(snapshot), result.Request);
        if (!planned.Plan) {
            result.Blockers.AppendValue(Block(planned.Error));
        } else {
            result.Plan = *planned.Plan;
            for (const auto& required : planned.RequiredAcknowledgements) {
                result.Acknowledgements.AppendValue(required);
            }
        }
    }
    const auto live = ReadLiveness(snapshot);
    if (!snapshot.Applied || !snapshot.MutationReady || live.Stalled || snapshot.UnsupportedCluster) {
        result.Blockers.AppendValue(Block("MUTATION_NOT_READY"));
    }
    if (stop.Stopped) {
        result.Blockers.AppendValue(Block("LOCAL_STOP"));
    }
    if (snapshot.Applied) {
        const auto& state = *snapshot.Applied;
        if (state.RequestedMode == ERequestedMode::Observe) {
            result.Blockers.AppendValue(Block("OBSERVE_MODE"));
        }
        if (!state.Ownership) {
            result.Blockers.AppendValue(Block("OWNERSHIP_ACKNOWLEDGEMENT_REQUIRED"));
        }
        if (state.UnresolvedAttempt) {
            result.Blockers.AppendValue(Block("UNRESOLVED_ATTEMPT"));
        }
        if (state.StateConflict) {
            result.Blockers.AppendValue(Block("STATE_CONFLICT"));
        }
        if (state.OrdinaryOwner && !IsTerminal(state.Operations.at(*state.OrdinaryOwner).Status) && !result.ResumeOperation && result.Request.Kind != EOperationKind::Failover) {
            result.Blockers.AppendValue(Block("ORDINARY_WORKFLOW_OWNED"));
        }
    }
    if (result.DecisionId && live.EvidenceExpired) {
        result.Blockers.AppendValue(Block("EVIDENCE_EXPIRED"));
    }
    const auto workflowKind = result.ResumeOperation ? result.Plan.Kind : result.Request.Kind;
    if (snapshot.ConfigStale && (workflowKind == EOperationKind::Convert || workflowKind == EOperationKind::AddPile || workflowKind == EOperationKind::RemovePile || workflowKind == EOperationKind::Takedown || workflowKind == EOperationKind::PrimaryMaintenance)) {
        result.Blockers.AppendValue(Block("CONFIG_STALE"));
    }
    result.Dispatchable = result.Blockers.GetArray().empty() && !result.Plan.Steps.empty();
    return result;
}

TString EffectsHash(const TActionPlan& action) {
    auto effect = Object();
    effect["kind"] = KindName(action.Plan.Kind);
    effect["steps"] = Array();
    for (size_t index = action.NextStep; index < action.Plan.Steps.size(); ++index) {
        const auto& step = action.Plan.Steps[index];
        auto value = Object();
        value["method"] = MethodName(step.Method);
        value["updates"] = NamedStates(step.Update);
        value["target"] = step.Target ? TApiJson(*step.Target) : Null();
        effect["steps"].AppendValue(std::move(value));
    }
    effect["workflow_revision"] = ToString(action.WorkflowRevision);
    effect["next_step"] = static_cast<ui32>(action.NextStep);
    return DurableHash(CanonicalApiJson(effect));
}

TApiJson Bindings(const TControllerSnapshot& snapshot) {
    auto value = Object();
    value["version"] = 1;
    value["incarnation_id"] = snapshot.IncarnationId;
    value["readiness_generation"] = ToString(snapshot.Epoch.ReadinessGeneration);
    value["topology_revision"] = ToString(snapshot.TopologyRevision);
    value["controller_revision"] = ToString(snapshot.Applied ? snapshot.Applied->ControllerRevision : 0);
    value["policy_revision"] = snapshot.PolicyRevision;
    value["state_fingerprint"] = snapshot.Cluster ? CanonicalFingerprint(*snapshot.Cluster) : CanonicalFingerprint(PlanningView(snapshot));
    return value;
}

void ValidateAcknowledgements(const TApiJson& object, TActionPlan& plan) {
    Fields(object, {"confirmed", "typed_pile_name", "empty_name", "deletion"});
    Require(object["confirmed"].IsArray() && object["confirmed"].GetArray().size() <= 32, "Acknowledgement list invalid");
    std::set<TString> supplied;
    for (const auto& value : object["confirmed"].GetArray()) {
        Require(value.IsString() && supplied.insert(value.GetString()).second, "Duplicate or invalid acknowledgement");
    }
    std::set<TString> required;
    for (const auto& value : plan.Acknowledgements.GetArray()) {
        required.insert(value.GetString());
    }
    Require(supplied == required, "Confirm every required acknowledgement", 422, "ACKNOWLEDGEMENTS_REQUIRED");
    const auto typed = NullableString(object, "typed_pile_name");
    const bool empty = Boolean(object, "empty_name");
    const bool deletion = Boolean(object, "deletion");
    if (plan.Request.Kind == EOperationKind::RemovePile && !plan.ResumeOperation) {
        Require(plan.Request.Target && typed == plan.Request.Target && deletion && (!plan.Request.Target->empty() || empty), "Exact typed pile name, deletion acknowledgement and explicit empty-name acknowledgement required", 422, "EXACT_DELETION_ACKNOWLEDGEMENT_REQUIRED");
        Require(plan.Request.Target->empty() || !empty, "Empty-name acknowledgement only applies to the empty pile");
    } else {
        Require(!typed && !empty && !deletion, "Deletion fields must be null/false for this action");
    }
    plan.Request.ExternalPrerequisitesAcknowledged = true;
    plan.Request.TypedPileName = typed;
    plan.Request.EmptyNameAcknowledged = empty;
    plan.Request.DeletionAcknowledged = deletion;
}

TApiJson PreviewDisplay(const TActionPlan& action, const TControllerSnapshot& snapshot) {
    auto value = ApiObject();
    value["incarnation_id"] = snapshot.IncarnationId;
    value["dispatchable"] = action.Dispatchable;
    value["bindings"] = Bindings(snapshot);
    if (action.DecisionId) {
        value["bindings"]["decision_id"] = *action.DecisionId;
    }
    value["steps"] = Array();
    value["effects"] = Array();
    value["states"] = Object();
    auto states = PlanningView(snapshot).Piles;
    value["states"]["current"] = NamedStates(states);
    value["states"]["intermediate"] = Array();
    for (size_t index = action.NextStep; index < action.Plan.Steps.size(); ++index) {
        const auto& step = action.Plan.Steps[index];
        auto entry = Object();
        entry["method"] = MethodName(step.Method);
        entry["updates"] = NamedStates(step.Update);
        entry["target"] = step.Target ? TApiJson(*step.Target) : Null();
        entry["quorum_piles"] = Array();
        value["steps"].AppendValue(entry);
        value["effects"].AppendValue(std::move(entry));
        for (const auto& [name, state] : step.Update) {
            states[name] = state;
        }
        value["states"]["intermediate"].AppendValue(NamedStates(states));
        for (const auto& [name, requested] : step.Update) {
            if (requested == EPileState::Promoted) {
                for (auto& [other, role] : states) {
                    Y_UNUSED(other);
                    if (role == EPileState::Primary) {
                        role = EPileState::Synchronized;
                    }
                }
                states[name] = EPileState::Primary;
            } else if (requested == EPileState::Suspended) {
                states[name] = EPileState::Disconnected;
            } else if (requested == EPileState::NotSynchronized && action.Plan.Kind == EOperationKind::Rejoin) {
                states[name] = EPileState::Synchronized;
            }
        }
        if (step.Target && step.Method == EBridgeMethod::AddPile) {
            states[*step.Target] = EPileState::Disconnected;
        } else if (step.Target && step.Method == EBridgeMethod::RemovePile) {
            states[*step.Target] = EPileState::Inactive;
        }
    }
    value["states"]["terminal"] = NamedStates(states);
    value["blockers"] = action.Blockers;
    value["required_acknowledgements"] = action.Acknowledgements;
    value["prerequisites"] = Array();
    auto prerequisite = Object();
    prerequisite["kind"] = "external_attestation";
    prerequisite["message"] = "Operator verifies provision, version, resources, repair and application-drain prerequisites as applicable; YDB remains final validator";
    value["prerequisites"].AppendValue(std::move(prerequisite));
    value["evidence_age_ms"] = action.DecisionId && snapshot.Decision ? TApiJson(AgeMs(TSkipperClock::Now(), snapshot.Decision->RoundStartedAt)) : Null();
    return value;
}

unsigned ResultStatus(TStringBuf code) {
    if (code.Contains("PERSISTENCE") || code.Contains("UNKNOWN") || code.Contains("NOT_READY")) {
        return 503;
    }
    if (code.Contains("PREREQUISITE") || code.Contains("ACKNOWLEDGEMENT")) {
        return 422;
    }
    return 409;
}

} // namespace

struct THttpApi::TImpl {
    TApiOptions Options;
    TDiagnosticRing Ring;
    TIntrusivePtr<NMonitoring::TDynamicCounters> Metrics = MakeIntrusive<NMonitoring::TDynamicCounters>();
    std::map<TString, TString> Pending;

    explicit TImpl(TApiOptions options)
        : Options(std::move(options))
        , Ring(Options.Config.Logging.DisplayRingLines)
    {
        if (Options.SigningKey.empty()) {
            Options.SigningKey.resize(32);
            Y_ENSURE(RAND_bytes(reinterpret_cast<unsigned char*>(Options.SigningKey.Detach()), Options.SigningKey.size()) == 1, "Cannot initialize preview signing key");
        }
        Y_ENSURE(Options.SigningKey.size() >= 32 && Options.NextId, "HTTP signer requires key and ID provider");
    }

    TString Envelope(TApiJson payload) const {
        const auto bytes = CanonicalApiJson(payload);
        const auto encoded = Base64Encode(bytes) + "." + Sign(Options.SigningKey, bytes);
        Require(encoded.size() <= 65536, "Preview exceeds 64 KiB", 413, "PREVIEW_TOO_LARGE");
        return encoded;
    }

    TApiJson Verify(TStringBuf envelope) const {
        Require(envelope.size() <= 65536, "Preview exceeds 64 KiB", 413, "PREVIEW_TOO_LARGE");
        const auto separator = envelope.find('.');
        Require(separator != TStringBuf::npos && envelope.find('.', separator + 1) == TStringBuf::npos, "Invalid signed preview", 409, "STALE_PLAN");
        TString bytes;
        try {
            bytes = Base64Decode(envelope.SubStr(0, separator));
        } catch (...) {
            throw TApiException(409, "STALE_PLAN", "Invalid signed preview");
        }
        const auto expected = Sign(Options.SigningKey, bytes);
        const auto signature = envelope.SubStr(separator + 1);
        Require(signature.size() == expected.size() && CRYPTO_memcmp(signature.data(), expected.data(), expected.size()) == 0, "Preview invalid or from a previous process; request a fresh preview", 409, "STALE_PLAN");
        const auto value = ParseStrictJson(bytes);
        Require(CanonicalApiJson(value) == bytes, "Noncanonical preview", 409, "STALE_PLAN");
        return value;
    }
};

THttpApi::THttpApi(TApiOptions options)
    : Impl(std::make_unique<TImpl>(std::move(options)))
{
}

THttpApi::~THttpApi() = default;

TApiResponse THttpApi::Receipt(const TProposalResult& result) const {
    if (result.Status == EProposalStatus::Unknown) {
        return Unknown({});
    }
    if (!result.Result.Ok) {
        const auto code = result.Result.Code.empty() ? result.Error : result.Result.Code;
        return ApiError(ResultStatus(code), code, code, false, result.Result.OperationId.empty() ? std::nullopt : std::optional<TString>(result.Result.OperationId));
    }
    return JsonResponse(ReceiptJson(result.Result), result.Result.OperationId.empty() ? 200 : 202);
}

TApiResponse THttpApi::Unknown(TStringBuf requestId) const {
    auto value = ApiObject();
    value["error"] = Object();
    value["error"]["code"] = "ADMISSION_UNKNOWN";
    value["error"]["message"] = "Durability is unknown; look up or replay the same request ID and unchanged body. Do not create a new request ID";
    value["error"]["retryable"] = true;
    value["error"]["details"] = Object();
    value["error"]["details"]["request_id"] = TString(requestId);
    value["error"]["operation_id"] = Null();
    return JsonResponse(std::move(value), 503);
}

TApiAction THttpApi::Handle(const TApiRequest& request, const TControllerSnapshot& snapshot, const TLocalStopReceipt& stop) {
    TApiAction action;
    try {
        Require(request.Target.size() <= 2048, "Request target too long");
        const auto question = request.Target.find('?');
        const TString path = request.Target.substr(0, question);
        const auto query = question == TString::npos ? std::map<TString, TString>() : Query(TStringBuf(request.Target).SubStr(question + 1));
        if (request.Method == "GET") {
            Require(request.Body.empty(), "GET body unsupported");
            if (path == "/api/state") {
                Require(query.empty(), "Unknown query field");
                action.Response = JsonResponse(ProjectState(snapshot, Impl->Options.Config, stop));
                return action;
            }
            const TDurableState empty;
            const auto& state = snapshot.Applied ? *snapshot.Applied : empty;
            if (path.StartsWith("/api/requests/")) {
                Require(query.empty(), "Unknown query field");
                const TString id = path.substr(14);
                Require(IsRequestUuid(id), "request_id must be a UUID");
                if (Impl->Options.LookupLocalReceipt) {
                    if (const auto receipt = Impl->Options.LookupLocalReceipt(id)) {
                        action.Response = JsonResponse(StopJson(*receipt));
                        return action;
                    }
                }
                const auto receipt = state.Requests.find(id);
                if (receipt == state.Requests.end() && Impl->Pending.contains(id)) {
                    action.Response = Unknown(id);
                    return action;
                }
                Require(receipt != state.Requests.end(), "Receipt not found; absence does not prove rejection. Replay the same ID and unchanged body", 404, "REQUEST_NOT_FOUND");
                action.Response = JsonResponse(ReceiptJson(receipt->second.Result));
                return action;
            }
            if (path == "/api/operations") {
                QueryKeys(query, {"after", "limit"});
                const ui64 after = After(query);
                const size_t limit = Limit(query, 50, 200);
                std::vector<std::pair<ui64, const TOperation*>> operations;
                for (const auto& [id, operation] : state.Operations) {
                    Y_UNUSED(id);
                    const ui64 created = operation.CreatedIndex;
                    if (created > after) {
                        operations.emplace_back(created, &operation);
                    }
                }
                std::sort(operations.begin(), operations.end(), [](const auto& lhs, const auto& rhs) {
                    return lhs.first < rhs.first;
                });
                auto value = ApiObject();
                value["items"] = Array();
                ui64 next = after;
                for (size_t i = 0; i < std::min(limit, operations.size()); ++i) {
                    value["items"].AppendValue(ProjectOperation(*operations[i].second, state));
                    next = operations[i].first;
                }
                value["next"] = ToString(next);
                value["has_more"] = operations.size() > limit;
                value["state_index"] = ToString(state.AppliedIndex);
                action.Response = JsonResponse(std::move(value));
                return action;
            }
            if (path.StartsWith("/api/operations/")) {
                Require(query.empty(), "Unknown query field");
                const TString id = path.substr(16);
                Require(IsRequestUuid(id), "operation_id must be a UUID");
                Require(state.Operations.contains(id), "Unknown operation", 404, "OPERATION_NOT_FOUND");
                auto value = ApiObject();
                value["operation"] = ProjectOperation(state.Operations.at(id), state);
                action.Response = JsonResponse(std::move(value));
                return action;
            }
            if (path == "/api/audit") {
                QueryKeys(query, {"after", "limit"});
                action.ReadAudit = true;
                action.AuditAfter = After(query);
                action.AuditLimit = Limit(query, 100, 1000);
                return action;
            }
            if (path == "/api/logs") {
                QueryKeys(query, {"after", "limit", "incarnation_id"});
                const bool reset = !query.contains("incarnation_id") || query.at("incarnation_id") != snapshot.IncarnationId;
                const auto after = After(query);
                const auto limit = Limit(query, 200, 1000);
                auto value = Object();
                if (Impl->Options.ReadLogs) {
                    const auto page = Impl->Options.ReadLogs(after, limit, reset);
                    value["lines"] = Array();
                    for (const auto& line : page.Lines) {
                        value["lines"].AppendValue(ParseStrictJson(line));
                    }
                    value["next"] = ToString(page.Next);
                    value["oldest"] = ToString(page.Oldest);
                    value["reset"] = reset || page.Reset;
                    value["overflow"] = page.Lost;
                    value["has_more"] = page.HasMore;
                } else {
                    value = Impl->Ring.Read(after, limit, reset);
                }
                value["api_version"] = 1;
                value["incarnation_id"] = snapshot.IncarnationId;
                action.Response = JsonResponse(std::move(value));
                return action;
            }
            if (path == "/mon/local-stop") {
                QueryKeys(query, {"limit"});
                action.ReadLocalStopAudit = true;
                action.AuditLimit = Limit(query, 50, 100);
                return action;
            }
            if (path == "/mon/" || path == "/mon/controller" || path == "/mon/storage") {
                Require(query.empty(), "Unknown query field");
                auto value = ApiObject();
                value["summary"] = path == "/mon/storage" ? "Storage counters available at /metrics; journal contents available at /api/audit" : "Standalone Skipper read-only diagnostics";
                value["state"] = ProjectState(snapshot, Impl->Options.Config, stop);
                action.Response = JsonResponse(std::move(value));
                return action;
            }
            throw TApiException(404, "NOT_FOUND", "Unknown route");
        }
        Require(request.Method == "POST", "Unsupported route/method", 404, "NOT_FOUND");
        Require(query.empty(), "Mutation query fields unsupported");
        const auto semicolon = request.ContentType.find(';');
        auto media = request.ContentType.substr(0, semicolon);
        char* const mediaData = media.Detach();
        std::transform(mediaData, mediaData + media.size(), mediaData, [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        Require(media == "application/json", "Content-Type application/json required", 415, "CONTENT_TYPE_REQUIRED");
        const auto body = ParseStrictJson(request.Body);
        if (path == "/api/plans") {
            const auto planned = PlanAction(body, snapshot, stop);
            auto payload = Bindings(snapshot);
            payload["action"] = planned.Arguments;
            payload["effects_hash"] = EffectsHash(planned);
            payload["required_acknowledgements"] = planned.Acknowledgements;
            auto expires = TSkipperClock::Now() + TDuration::Seconds(120);
            if (planned.DecisionId && snapshot.Decision) {
                expires = std::min(expires, snapshot.Decision->EvidenceExpiry);
                payload["decision_id"] = *planned.DecisionId;
                payload["evidence_hash"] = DecisionEvidenceHash(*snapshot.Decision);
            } else {
                payload["decision_id"] = Null();
                payload["evidence_hash"] = Null();
            }
            payload["expires_us"] = ToString(expires.MicroSeconds());
            auto display = PreviewDisplay(planned, snapshot);
            display["preview"] = Impl->Envelope(std::move(payload));
            display["expires_at"] = expires.ToString();
            action.Response = JsonResponse(std::move(display));
            return action;
        }
        // UUID and canonical immutable-body hash are checked before preview
        // verification and all revision/expiry/readiness checks.
        const auto requestId = String(body, "request_id", 36);
        Require(IsRequestUuid(requestId), "request_id must be a UUID");
        const auto bodyHash = RequestHash(request, body);
        if (snapshot.Applied) {
            const auto old = snapshot.Applied->Requests.find(requestId);
            if (old != snapshot.Applied->Requests.end()) {
                Require(old->second.PayloadHash == bodyHash, "Request ID already used with a different body", 409, "REQUEST_ID_REUSED");
                TProposalResult receipt;
                receipt.Status = EProposalStatus::Committed;
                receipt.Result = old->second.Result;
                action.Response = Receipt(receipt);
                return action;
            }
        }
        if (Impl->Options.LookupLocalReceipt) {
            if (const auto old = Impl->Options.LookupLocalReceipt(requestId)) {
                Require(old->PayloadHash == bodyHash, "Request ID already used with a different body", 409, "REQUEST_ID_REUSED");
                action.Response = JsonResponse(StopJson(*old), old->Pending ? 202 : 200);
                return action;
            }
        }
        if (const auto pending = Impl->Pending.find(requestId); pending != Impl->Pending.end()) {
            Require(pending->second == bodyHash, "Request ID pending with a different body", 409, "REQUEST_ID_REUSED");
            action.Response = Unknown(requestId);
            return action;
        }
        if (path == "/api/local-stop") {
            Fields(body, {"request_id", "reason"});
            const auto reason = String(body, "reason", 2048);
            Require(!reason.empty(), "Reason required");
            Require(static_cast<bool>(Impl->Options.Stop), "Stop unavailable", 503, "STOP_UNAVAILABLE");
            action.Response = JsonResponse(StopJson(Impl->Options.Stop(requestId, bodyHash, reason)), 200);
            return action;
        }
        if (path == "/api/local-stop/release") {
            Fields(body, {"request_id", "incarnation_id", "stop_revision", "reason"});
            const auto process = String(body, "incarnation_id", 36);
            Require(IsRequestUuid(process), "incarnation_id must be a UUID");
            const auto revision = Decimal(String(body, "stop_revision", 20));
            const auto reason = String(body, "reason", 2048);
            Require(!reason.empty(), "Reason required");
            Require(static_cast<bool>(Impl->Options.Release), "Stop release unavailable", 503, "STOP_UNAVAILABLE");
            const auto receipt = Impl->Options.Release(requestId, process, revision, bodyHash, reason);
            if (!receipt.Error.empty()) {
                action.Response = ApiError(receipt.Error == "STALE_LOCAL_STOP" ? 409 : 503, receipt.Error, receipt.Error);
            } else {
                action.Response = JsonResponse(StopJson(receipt), receipt.Pending ? 202 : 200);
            }
            return action;
        }
        Require(snapshot.Applied != nullptr, "Durable state unavailable", 503, "AUTHORITY_NOT_READY");
        TControllerSubmission submission;
        auto& command = submission.Command;
        command.Id = Impl->Options.NextId();
        command.RequestId = requestId;
        command.RequestHash = bodyHash;
        command.AuditTime = TSkipperClock::Now().ToString();
        command.ExpectedControllerRevision = snapshot.Applied->ControllerRevision;
        submission.IncarnationId = snapshot.IncarnationId;
        submission.TopologyRevision = snapshot.TopologyRevision;
        submission.StateFingerprint = snapshot.Cluster ? CanonicalFingerprint(*snapshot.Cluster) : CanonicalFingerprint(PlanningView(snapshot));
        if (path == "/api/operations" || (path.StartsWith("/api/operations/") && path.EndsWith("/resume"))) {
            Fields(body, {"preview", "request_id", "acknowledgements"});
            const auto signedPlan = Impl->Verify(String(body, "preview", 65536));
            Require(Decimal(String(signedPlan, "expires_us", 20)) > TSkipperClock::Now().MicroSeconds(), "Preview expired; request a fresh preview", 409, "STALE_PLAN");
            const auto currentBindings = Bindings(snapshot);
            for (const auto& [key, value] : currentBindings.GetMap()) {
                Require(signedPlan.Has(key) && CanonicalApiJson(signedPlan[key]) == CanonicalApiJson(value), "Preview bindings changed; request a fresh preview", 409, "STALE_PLAN");
            }
            auto planned = PlanAction(signedPlan["action"], snapshot, stop);
            Require(signedPlan["effects_hash"].GetString() == EffectsHash(planned) && CanonicalApiJson(signedPlan["required_acknowledgements"]) == CanonicalApiJson(planned.Acknowledgements), "Plan effects changed; request a fresh preview", 409, "STALE_PLAN");
            if (!signedPlan["decision_id"].IsNull()) {
                Require(planned.DecisionId && snapshot.Decision
                    && String(signedPlan, "decision_id", 36) == *planned.DecisionId
                    && String(signedPlan, "evidence_hash", 64) == DecisionEvidenceHash(*snapshot.Decision),
                    "Decision evidence changed; request a fresh preview", 409, "STALE_PLAN");
            } else {
                Require(!planned.DecisionId, "Decision binding changed; request a fresh preview", 409, "STALE_PLAN");
            }
            Require(planned.Dispatchable, "Preview has unmet prerequisites; inspect blockers and request a fresh preview", 422, "PLAN_BLOCKED");
            ValidateAcknowledgements(body["acknowledgements"], planned);
            submission.DecisionId = planned.DecisionId;
            if (planned.ResumeOperation) {
                Require(path == "/api/operations/" + *planned.ResumeOperation + "/resume", "Resume must target its existing operation", 409, "RESUME_ROUTE_MISMATCH");
                command.Kind = ECommandKind::Resume;
                command.OperationId = *planned.ResumeOperation;
                command.ExpectedWorkflowRevision = planned.WorkflowRevision;
            } else {
                Require(path == "/api/operations", "Non-resume preview cannot use resume endpoint", 409, "RESUME_ROUTE_MISMATCH");
                command.Kind = ECommandKind::AdmitOperation;
                command.OperationId = Impl->Options.NextId();
                submission.PlanRequest = planned.Request;
            }
        } else if (path == "/api/mode") {
            Fields(body, {"mode", "controller_revision", "request_id", "reason", "ownership_acknowledgement"});
            const auto mode = String(body, "mode", 16);
            Require(mode == "Manual" || mode == "Auto" || mode == "Observe", "Unknown requested mode");
            command.Kind = ECommandKind::SetMode;
            command.Mode = mode == "Manual" ? ERequestedMode::Manual : (mode == "Auto" ? ERequestedMode::Auto : ERequestedMode::Observe);
            command.ExpectedControllerRevision = Decimal(String(body, "controller_revision", 20));
            command.Reason = String(body, "reason", 2048);
            Require(!command.Reason.empty(), "Reason required");
            if (!body["ownership_acknowledgement"].IsNull()) {
                const auto& ack = body["ownership_acknowledgement"];
                Fields(ack, {"exclusive_administration", "independent_failure_domain", "deployment_plan_reference"});
                TOwnershipAcknowledgement ownership;
                ownership.ExclusiveAdministration = Boolean(ack, "exclusive_administration");
                ownership.IndependentFailureDomain = Boolean(ack, "independent_failure_domain");
                ownership.DeploymentPlanReference = String(ack, "deployment_plan_reference", 2048);
                command.Ownership = std::move(ownership);
            }
        } else if (path.StartsWith("/api/operations/") && (path.EndsWith("/cancel") || path.EndsWith("/resolve"))) {
            const auto lastSlash = path.rfind('/');
            const TString id = path.substr(16, lastSlash - 16);
            Require(IsRequestUuid(id), "operation_id must be a UUID");
            command.OperationId = id;
            if (path.EndsWith("/cancel")) {
                Fields(body, {"request_id"});
                command.Kind = ECommandKind::Cancel;
            } else {
                Fields(body, {"request_id", "resolution", "state_fingerprint", "reason", "acknowledge_unfenced_recovery", "executor_stopped", "server_work_reconciled"});
                Require(String(body, "resolution", 64) == "resolve_external", "Unsupported resolution");
                Require(snapshot.Cluster && String(body, "state_fingerprint", 64) == CanonicalFingerprint(*snapshot.Cluster), "Adopted fingerprint changed", 409, "STALE_PLAN");
                command.Kind = ECommandKind::ResolveExternal;
                command.Reason = String(body, "reason", 2048);
                Require(!command.Reason.empty(), "Reason required");
                command.AcknowledgeUnfencedRecovery = Boolean(body, "acknowledge_unfenced_recovery");
                command.ExecutorStopped = Boolean(body, "executor_stopped");
                command.ServerWorkReconciled = Boolean(body, "server_work_reconciled");
                command.Observed = *snapshot.Cluster;
            }
        } else {
            throw TApiException(404, "NOT_FOUND", "Unknown route");
        }
        action.Submission = std::move(submission);
        return action;
    } catch (const TApiException& error) {
        action.Response = ApiError(error.Status, error.Code, error.what());
        return action;
    } catch (...) {
        action.Response = ApiError(400, "MALFORMED_REQUEST", "Malformed request");
        return action;
    }
}

TApiResponse ReadTimeoutResponse() {
    return ApiError(503, "AUDIT_READ_PENDING", "Read-only audit storage work is still pending; repeat the same cursor", true);
}

TDiagnosticRing& THttpApi::Logs() {
    return Impl->Ring;
}

void THttpApi::BeginAdmission(TString requestId, TString canonicalHash) {
    Impl->Pending.emplace(std::move(requestId), std::move(canonicalHash));
}

void THttpApi::EndAdmission(TStringBuf requestId) {
    Impl->Pending.erase(TString(requestId));
}

TIntrusivePtr<NMonitoring::TDynamicCounters> THttpApi::Counters() const {
    return Impl->Metrics;
}

TDiagnosticRing::TDiagnosticRing(size_t capacity)
    : Capacity(std::min<size_t>(10000, std::max<size_t>(1, capacity)))
{
}

void TDiagnosticRing::Append(TApiJson fields) {
    Require(fields.IsMap() && fields.GetMap().size() <= 16, "Bounded structured log required");
    auto value = Object();
    value["sequence"] = ToString(Next);
    value["time"] = TSkipperClock::Now().ToString();
    for (const auto& [key, field] : fields.GetMap()) {
        Require(key.size() <= 64 && (field.IsString() || field.IsBoolean()), "Log fields must be bounded scalars");
        value[key] = field.IsString() ? TApiJson(BoundedDiagnostic(field.GetString())) : field;
    }
    Lines.push_back({Next++, CanonicalApiJson(value)});
    while (Lines.size() > Capacity) {
        Lines.pop_front();
    }
}

TApiJson TDiagnosticRing::Read(ui64 after, size_t limit, bool reset) const {
    auto value = Object();
    value["lines"] = Array();
    const ui64 oldest = Lines.empty() ? Next : Lines.front().Sequence;
    const bool pastEnd = after >= Next;
    value["reset"] = reset || pastEnd;
    value["overflow"] = !reset && after < oldest - 1;
    value["oldest"] = ToString(oldest);
    ui64 next = reset || pastEnd ? oldest - 1 : after;
    for (const auto& line : Lines) {
        if (line.Sequence > next && value["lines"].GetArray().size() < limit) {
            value["lines"].AppendValue(ParseStrictJson(line.Json));
            next = line.Sequence;
        }
    }
    value["next"] = ToString(next);
    return value;
}

} // namespace NBridgeSkipper
