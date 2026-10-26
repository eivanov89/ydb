#include "internal.h"

#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/http/http_proxy.h>
#include <library/cpp/monlib/dynamic_counters/encode.h>
#include <util/generic/guid.h>
#include <util/string/cast.h>

namespace NBridgeSkipper {
namespace {

struct TEvHttpDeadline : NActors::TEventLocal<TEvHttpDeadline, EventSpaceBegin(NActors::TEvents::ES_PRIVATE) + 0x7500> {
    TString Id;

    explicit TEvHttpDeadline(TString id)
        : Id(std::move(id))
    {
    }
};

TApiOptions Options(const TConfig& config, const std::shared_ptr<TRuntimeResources>& resources, NActors::TActorId controller) {
    TApiOptions options;
    options.Config = config;
    options.ProcessId = resources->ProcessId();
    options.NextId = []() {
        return TGUID::Create().AsUuidString();
    };
    options.LookupLocalReceipt = [resources](const TString& id) {
        return resources->LookupLocalReceipt(id);
    };
    options.Stop = [resources, controller](const TString& id, const TString& hash, const TString& reason) {
        return resources->StopLocal(id, controller, hash, reason);
    };
    options.Release = [resources, controller](const TString& id, const TString& process, ui64 revision, const TString& hash, const TString& reason) {
        return resources->ReleaseLocal(id, process, revision, controller, hash, reason);
    };
    options.ReadLogs = [resources](ui64 after, size_t limit, bool reset) {
        return resources->ReadLogs(after, limit, reset);
    };
    return options;
}

class THttpActor final : public NActors::TActorBootstrapped<THttpActor> {
    struct TWaiter {
        NActors::TActorId Sender;
        NHttp::THttpIncomingRequestPtr Request;
        TString RequestId;
        TString RequestHash;
        bool Audit = false;
        bool LocalStopAudit = false;
        ui64 After = 0;
        size_t Limit = 0;
        std::shared_ptr<void> Credit;
    };

public:
    THttpActor(const TConfig& config, std::shared_ptr<TRuntimeResources> resources, NActors::TActorId controller, NActors::TActorId proxy)
        : Config(config)
        , Resources(std::move(resources))
        , Controller(controller)
        , Proxy(proxy)
        , Api(Options(config, Resources, controller))
    {
    }

    void Bootstrap() {
        for (const TStringBuf path : {"/", "/api/", "/metrics", "/mon/"}) {
            Send(Proxy, new NHttp::TEvHttpProxy::TEvRegisterHandler(TString(path), SelfId()));
        }
        Become(&TThis::StateWork);
    }

private:
    void Respond(TWaiter& waiter, TApiResponse response) {
        if (!waiter.Request) {
            return;
        }
        NHttp::THeadersBuilder headers;
        headers.Set("Content-Type", response.ContentType);
        for (const auto& [key, value] : response.Headers) {
            headers.Set(key, value);
        }
        const TString status = ToString(response.Status);
        const TString message = response.Status < 300 ? "OK" : "Error";
        auto outgoing = waiter.Request->CreateResponse(status, message, headers, response.Body);
        Send(waiter.Sender, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(std::move(outgoing)));
        waiter.Request.Reset();
    }

    TControllerSnapshot Snapshot() const {
        if (const auto snapshot = Resources->ReadSnapshot()) {
            return *snapshot;
        }
        TControllerSnapshot snapshot;
        snapshot.Applied = std::make_shared<const TDurableState>(Resources->InitialState());
        snapshot.ClusterId = Config.ClusterId;
        snapshot.IncarnationId = Resources->ProcessId();
        snapshot.Blockers = {"CONTROLLER_STARTING"};
        return snapshot;
    }

    TApiResponse Metrics(const TControllerSnapshot& snapshot) {
        auto counters = Resources->Counters;
        UpdateHttpMetrics(snapshot, Config, Resources->StopLatch->Current(), Resources->PendingStorage(), counters);
        TStringStream body;
        {
            auto encoder = NMonitoring::CreateEncoder(&body, NMonitoring::EFormat::PROMETHEUS);
            counters->Accept({}, {}, *encoder);
        }
        TApiResponse response;
        response.ContentType = "text/plain; version=0.0.4; charset=utf-8";
        response.Body = body.Str();
        response.Headers = JsonResponse(ApiObject()).Headers;
        return response;
    }

    void Handle(NHttp::TEvHttpProxy::TEvHttpIncomingRequest::TPtr& event) {
        TWaiter waiter;
        waiter.Sender = event->Sender;
        waiter.Request = event->Get()->Request;
        TApiRequest request;
        request.Method = waiter.Request->Method;
        request.Target = waiter.Request->URL;
        request.ContentType = waiter.Request->ContentType;
        if (waiter.Request->Body.size() > 1024 * 1024) {
            Respond(waiter, ApiError(413, "BODY_TOO_LARGE", "Body exceeds 1 MiB"));
            return;
        }
        request.Body = waiter.Request->Body;
        const auto snapshot = Snapshot();
        if (request.Method == "GET" && request.Target == "/metrics") {
            Respond(waiter, Metrics(snapshot));
            return;
        }
        auto action = Api.Handle(request, snapshot, Resources->StopLatch->Current());
        if (action.ReadAudit || action.ReadLocalStopAudit) {
            if (AuditJobs >= 16) {
                Respond(waiter, ApiError(503, "BUSY", "Audit read capacity exhausted", true));
                return;
            }
            TStorageRequest storage;
            storage.Id = TGUID::Create().AsUuidString();
            storage.Kind = action.ReadLocalStopAudit ? EStorageKind::LocalStopAudit : EStorageKind::Audit;
            storage.After = action.AuditAfter;
            storage.Limit = action.AuditLimit;
            if (!Resources->SubmitStore(storage, SelfId())) {
                Respond(waiter, ApiError(503, "BUSY", "Storage read capacity exhausted", true));
                return;
            }
            waiter.Audit = true;
            waiter.LocalStopAudit = action.ReadLocalStopAudit;
            waiter.After = action.AuditAfter;
            waiter.Limit = action.AuditLimit;
            ++AuditJobs;
            Waiters.emplace(storage.Id, std::move(waiter));
            Schedule(TDuration::Seconds(10), new TEvHttpDeadline(storage.Id));
            return;
        }
        if (!action.Submission) {
            Respond(waiter, std::move(action.Response));
            return;
        }
        auto& submission = *action.Submission;
        waiter.RequestId = submission.Command.RequestId.value_or(TString());
        waiter.RequestHash = submission.Command.RequestHash;
        if (!Resources->AcquireSubmission()) {
            ++*Resources->Counters->GetCounter("skipper_admission_rejected_total", true);
            Respond(waiter, ApiError(503, "BUSY", "Command admission capacity exhausted", true));
            return;
        }
        waiter.Credit = std::shared_ptr<void>(Resources.get(), [resources = Resources](void*) {
            resources->ReleaseSubmission();
        });
        Api.BeginAdmission(waiter.RequestId, waiter.RequestHash);
        auto* commandEvent = new TEvController::TEvSubmit();
        commandEvent->Submission = std::move(submission);
        commandEvent->AdmissionCredit = waiter.Credit;
        const auto id = commandEvent->Submission.Command.Id;
        Waiters.emplace(id, std::move(waiter));
        Send(Controller, commandEvent);
        Schedule(TDuration::Seconds(10), new TEvHttpDeadline(id));
    }

    void Handle(TEvController::TEvResult::TPtr& event) {
        const auto& reply = event->Get()->Reply;
        const auto found = Waiters.find(reply.Id);
        if (found == Waiters.end()) {
            return;
        }
        auto& waiter = found->second;
        if (!reply.Final || reply.Result.Status == EProposalStatus::Unknown) {
            Respond(waiter, Api.Unknown(waiter.RequestId));
        } else {
            Respond(waiter, Api.Receipt(reply.Result));
            Resources->Log("command_result", {{"request_id", waiter.RequestId}, {"command_id", reply.Id},
                {"operation_id", reply.Result.Result.OperationId}, {"status", reply.Result.Result.Code}});
        }
        if (reply.Final) {
            Api.EndAdmission(waiter.RequestId);
            Waiters.erase(found);
        }
    }

    void Handle(TEvRuntime::TEvStorageResult::TPtr& event) {
        const auto& result = event->Get()->Result;
        const auto found = Waiters.find(result.Id);
        if (found == Waiters.end() || !found->second.Audit) {
            return;
        }
        auto& waiter = found->second;
        if (!result.Success) {
            Respond(waiter, ApiError(503, "PERSISTENCE_ERROR", "Audit unavailable", true));
        } else {
            auto value = ApiObject();
            value["items"] = Array();
            ui64 next = waiter.After;
            if (waiter.LocalStopAudit) {
                try {
                    for (const auto& line : result.LocalStopAudit) {
                        auto record = ParseStrictJson(line);
                        for (auto& [key, field] : record.GetMapSafe()) {
                            Y_UNUSED(key);
                            if (field.IsString()) {
                                field = BoundedDiagnostic(field.GetString());
                            }
                        }
                        value["items"].AppendValue(std::move(record));
                    }
                } catch (...) {
                    Respond(waiter, ApiError(503, "PERSISTENCE_ERROR", "Emergency stop audit is corrupt or unsupported"));
                    --AuditJobs;
                    Waiters.erase(found);
                    return;
                }
                value["chronology"] = "separate_process_emergency_stop_journal";
                value["truncated"] = result.LocalStopAuditTruncated;
                value["command_audit"] = "/api/audit";
                Respond(waiter, JsonResponse(std::move(value)));
                --AuditJobs;
                Waiters.erase(found);
                return;
            }
            for (const auto& entry : result.Audit) {
                auto record = Object();
                record["sequence"] = ToString(entry.Index);
                record["command_id"] = entry.Command.Id;
                record["request_id"] = entry.Command.RequestId ? TApiJson(*entry.Command.RequestId) : Null();
                record["operation_id"] = entry.Command.OperationId.empty() ? Null() : TApiJson(entry.Command.OperationId);
                record["kind"] = ToString(static_cast<ui32>(entry.Command.Kind));
                record["caller_id"] = "local";
                record["time"] = entry.Command.AuditTime;
                record["reason"] = BoundedDiagnostic(entry.Command.Reason);
                record["result"] = ReceiptJson(entry.Result);
                record["mode"] = entry.Command.Kind == ECommandKind::SetMode ? TApiJson(ModeName(entry.Command.Mode)) : Null();
                record["ownership_acknowledged"] = entry.Command.Ownership && entry.Command.Ownership->ExclusiveAdministration;
                value["items"].AppendValue(std::move(record));
                next = entry.Index;
            }
            value["next"] = ToString(next);
            value["has_more"] = result.HasMore;
            value["local_stop_audit"] = Object();
            value["local_stop_audit"]["url"] = "/mon/local-stop";
            value["local_stop_audit"]["chronology"] = "separate emergency journal; indexes are not comparable with command indexes";
            Respond(waiter, JsonResponse(std::move(value)));
        }
        --AuditJobs;
        Waiters.erase(found);
    }

    void Handle(TEvHttpDeadline::TPtr& event) {
        const auto found = Waiters.find(event->Get()->Id);
        if (found != Waiters.end()) {
            Respond(found->second, found->second.Audit ? ReadTimeoutResponse() : Api.Unknown(found->second.RequestId));
            // Keep credit and correlation after waiter timeout. Only final
            // durable completion releases authority/storage admission capacity.
        }
    }

    void Shutdown() {
        for (auto& [id, waiter] : Waiters) {
            Y_UNUSED(id);
            Respond(waiter, Api.Unknown(waiter.RequestId));
        }
        Waiters.clear();
        // Controller/storage producers retain their own credit/gate references.
        PassAway();
    }

    STRICT_STFUNC(StateWork,
        hFunc(NHttp::TEvHttpProxy::TEvHttpIncomingRequest, Handle);
        hFunc(TEvController::TEvResult, Handle);
        hFunc(TEvRuntime::TEvStorageResult, Handle);
        hFunc(TEvHttpDeadline, Handle);
        cFunc(NActors::TEvents::TEvPoison::EventType, Shutdown);
    )

    TConfig Config;
    std::shared_ptr<TRuntimeResources> Resources;
    NActors::TActorId Controller;
    NActors::TActorId Proxy;
    THttpApi Api;
    std::map<TString, TWaiter> Waiters;
    size_t AuditJobs = 0;
};

} // namespace

NActors::IActor* CreateHttpActor(const TConfig& config, std::shared_ptr<TRuntimeResources> resources,
    NActors::TActorId controller, NActors::TActorId httpProxy)
{
    return new THttpActor(config, std::move(resources), controller, httpProxy);
}

} // namespace NBridgeSkipper
