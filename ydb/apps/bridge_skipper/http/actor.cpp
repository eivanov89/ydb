#include "http.h"
#include "page.h"

#include <ydb/apps/bridge_skipper/runtime/events.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/http/http_proxy.h>
#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>
#include <util/string/cast.h>
#include <library/cpp/cgiparam/cgiparam.h>

namespace NBridgeSkipper {

void TCommandAdmission::Open(NActors::TActorId controller) {
    std::lock_guard lock(Mutex);
    Controller = controller;
}

void TCommandAdmission::Close() {
    std::lock_guard lock(Mutex);
    Controller.reset();
}

std::optional<NActors::TActorId> TCommandAdmission::Read() const {
    std::lock_guard lock(Mutex);
    return Controller;
}

namespace {

THttpResponse Error(unsigned status, TStringBuf code) {
    NJson::TJsonValue json(NJson::JSON_MAP);
    json["error"] = TString(code);
    return {status, "application/json; charset=utf-8", NJson::WriteJson(json, false, true)};
}

TStringBuf StatusText(unsigned status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Error";
    }
}

void Respond(NActors::TActorSystem* system, NActors::TActorId recipient, ui64 cookie,
    const NHttp::THttpIncomingRequestPtr& request, const THttpResponse& response)
{
    NHttp::THeadersBuilder headers;
    headers.Set("Content-Type", response.ContentType);
    headers.Set("Cache-Control", "no-store");
    if (response.Status == 405) {
        const auto path = request->URL.Before('?');
        headers.Set("Allow", path == "/api/mode" || path == "/api/plans" || path == "/api/operations" ? "POST" : "GET, HEAD");
    }
    auto outgoing = request->CreateResponse(::ToString(response.Status), TString(StatusText(response.Status)), headers,
        request->Method == "HEAD" ? TString() : response.Body);
    system->Send(new NActors::IEventHandle(recipient, {}, new NHttp::TEvHttpProxy::TEvHttpOutgoingResponse(std::move(outgoing)), 0, cookie));
}

// One actor owns the HTTP request until response or deadline. The controller may
// finish after the deadline; its late reply is intentionally harmless.
class TCommandRequestActor final : public NActors::TActorBootstrapped<TCommandRequestActor> {
public:
    TCommandRequestActor(NHttp::THttpIncomingRequestPtr request, NActors::TActorId sender, ui64 cookie,
        std::shared_ptr<TCommandAdmission> admission, TControllerCommand command)
        : Request(std::move(request))
        , Sender(sender)
        , Cookie(cookie)
        , Admission(std::move(admission))
        , Command(std::move(command)) {
    }

    void Bootstrap() {
        Become(&TThis::StateWork);
        const auto controller = Admission ? Admission->Read() : std::nullopt;
        if (!controller) {
            Finish(Error(503, "CONTROLLER_UNAVAILABLE"));
            return;
        }
        Controller = *controller;
        Send(Controller, new TEvSkipper::TEvCommand(std::move(Command)), NActors::IEventHandle::FlagTrackDelivery, 1);
        Schedule(TDuration::Seconds(15), new NActors::TEvents::TEvWakeup());
    }

private:
    void Finish(const THttpResponse& response) {
        Respond(NActors::TActivationContext::ActorSystem(), Sender, Cookie, Request, response);
        PassAway();
    }

    void Reply(TEvSkipper::TEvCommandResult::TPtr& ev) {
        if (ev->Sender != Controller || ev->Cookie != 1) {
            return;
        }
        Finish({ev->Get()->Result.Status, "application/json; charset=utf-8", NJson::WriteJson(ev->Get()->Result.Body, false, true)});
    }

    void Undelivered(NActors::TEvents::TEvUndelivered::TPtr& ev) {
        if (ev->Cookie == 1) {
            Finish(Error(503, "CONTROLLER_UNAVAILABLE"));
        }
    }

    void Timeout() {
        Finish(Error(504, "COMMAND_OUTCOME_UNKNOWN"));
    }

    STRICT_STFUNC(StateWork,
        hFunc(TEvSkipper::TEvCommandResult, Reply);
        hFunc(NActors::TEvents::TEvUndelivered, Undelivered);
        cFunc(NActors::TEvents::TEvWakeup::EventType, Timeout);
    )

    NHttp::THttpIncomingRequestPtr Request;
    NActors::TActorId Sender;
    ui64 Cookie;
    std::shared_ptr<TCommandAdmission> Admission;
    TControllerCommand Command;
    NActors::TActorId Controller;
};

class THttpActor final : public NActors::TActorBootstrapped<THttpActor> {
public:
    THttpActor(std::shared_ptr<TSnapshotStore> snapshots, NActors::TActorId proxy, std::shared_ptr<TCommandAdmission> admission)
        : Snapshots(std::move(snapshots))
        , Proxy(proxy)
        , Admission(std::move(admission)) {
    }

    void Bootstrap() {
        Send(Proxy, new NHttp::TEvHttpProxy::TEvRegisterHandler("/", SelfId()));
        Become(&TThis::StateWork);
    }

private:
    void Handle(NHttp::TEvHttpProxy::TEvHttpIncomingRequest::TPtr& event) {
        const auto& request = event->Get()->Request;
        const TStringBuf path = request->URL.Before('?');
        if (request->Method == "POST" && (path == "/api/mode" || path == "/api/plans" || path == "/api/operations")) {
            THttpResponse error;
            auto command = ParseCommand(path, request->ContentType, request->Body, error);
            if (!command) {
                Respond(NActors::TActivationContext::ActorSystem(), event->Sender, event->Cookie, request, error);
                return;
            }
            Register(new TCommandRequestActor(request, event->Sender, event->Cookie, Admission, std::move(*command)));
            return;
        }
        const auto snapshot = Snapshots->Read();
        Y_ENSURE(snapshot, "HTTP request before the seed snapshot");
        const auto response = RouteHttpRequest(request->Method, request->URL, *snapshot,
            NActors::TActivationContext::Monotonic(), NActors::TActivationContext::Now());
        Respond(NActors::TActivationContext::ActorSystem(), event->Sender, event->Cookie, request, response);
    }

    void Ready(TEvSkipper::TEvReady::TPtr& ev) {
        Send(ev->Sender, new TEvSkipper::TEvReadyAck(), 0, ev->Cookie);
    }

    STRICT_STFUNC(StateWork,
        hFunc(NHttp::TEvHttpProxy::TEvHttpIncomingRequest, Handle);
        hFunc(TEvSkipper::TEvReady, Ready);
        cFunc(NActors::TEvents::TEvPoison::EventType, PassAway);
    )

    std::shared_ptr<TSnapshotStore> Snapshots;
    NActors::TActorId Proxy;
    std::shared_ptr<TCommandAdmission> Admission;
};

bool Decimal(TStringBuf value, ui64& result) {
    if (value.empty()) {
        return false;
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return TryFromString(value, result);
}

// Query parameters only filter retained diagnostics; never reach a command path.
TString Query(TStringBuf target, TStringBuf key) {
    const TCgiParameters parameters(TString(target.After('?')));
    return parameters.Get(key);
}

} // namespace

std::optional<TControllerCommand> ParseCommand(TStringBuf path, TStringBuf contentType, TStringBuf body, THttpResponse& error) {
    error = Error(400, "INVALID_COMMAND");
    if (body.size() > 65536) {
        error = Error(413, "BODY_TOO_LARGE");
        return {};
    }
    if (contentType.Before(';') != "application/json") {
        error = Error(400, "JSON_REQUIRED");
        return {};
    }
    NJson::TJsonValue json;
    if (!NJson::ReadJsonTree(body, &json) || !json.IsMap()) {
        return {};
    }
    TControllerCommand command;
    const auto string = [&](TStringBuf key, TString& out, bool nonempty = true) {
        const auto& value = json[TString(key)];
        if (!value.IsString() || (nonempty && value.GetString().empty()) || value.GetString().Contains('\0')) {
            return false;
        }
        out = value.GetString();
        return true;
    };
    std::set<TString> allowed;
    if (path == "/api/mode") {
        command.Kind = ECommandKind::Mode;
        allowed = {"mode", "request_id", "expected_boot_id", "expected_mode_revision", "reviewed_snapshot_revision"};
        TString mode, revision;
        if (!string("mode", mode) || !string("request_id", command.RequestId) || !string("expected_boot_id", command.ExpectedBootId)
            || !string("expected_mode_revision", revision) || !Decimal(revision, command.ExpectedModeRevision))
        {
            return {};
        }
        if (mode != "observe" && mode != "manual" && mode != "auto") {
            return {};
        }
        command.Mode = mode == "observe" ? EMode::Observe : mode == "manual" ? EMode::Manual : EMode::Auto;
        if ((command.Mode == EMode::Auto || json.Has("reviewed_snapshot_revision")) && !string("reviewed_snapshot_revision", command.SnapshotRevision)) {
            return {};
        }
    } else if (path == "/api/plans") {
        command.Kind = ECommandKind::CreatePlan;
        allowed = {"action", "snapshot_revision", "target_pile"};
        TString action;
        if (!string("action", action) || !string("snapshot_revision", command.SnapshotRevision)) {
            return {};
        }
        bool known = false;
        for (const auto kind : {EOperationKind::Failover, EOperationKind::Switchover, EOperationKind::TakeOffline, EOperationKind::Rejoin}) {
            if (action == ToString(kind)) {
                command.Action = kind;
                known = true;
            }
        }
        if (!known) {
            return {};
        }
        if (command.Action != EOperationKind::Failover || json.Has("target_pile")) {
            TString target;
            if (!string("target_pile", target, false)) {
                return {};
            }
            command.Target = std::move(target);
        }
    } else if (path == "/api/operations") {
        command.Kind = ECommandKind::Execute;
        allowed = {"request_id", "plan_id"};
        if (!string("request_id", command.RequestId) || !string("plan_id", command.PlanId)) {
            return {};
        }
    } else {
        error = Error(404, "NOT_FOUND");
        return {};
    }
    for (const auto& [key, value] : json.GetMap()) {
        (void)value;
        if (!allowed.contains(key)) {
            return {};
        }
    }
    return command;
}

THttpResponse RouteHttpRequest(TStringBuf method, TStringBuf target, const TSkipperSnapshot& snapshot, TMonotonic now, TInstant wallNow) {
    const bool head = method == "HEAD";
    if (method != "GET" && !head) {
        return Error(405, "METHOD_NOT_ALLOWED");
    }
    const auto path = target.Before('?');
    THttpResponse response;
    if (path == "/api/state") {
        response.Body = RenderStateJson(snapshot, now, wallNow);
    } else if (path == "/") {
        response.ContentType = "text/html; charset=utf-8";
        response.Body = RenderPage(snapshot, now, wallNow);
    } else if (path == "/metrics") {
        response = {200, "text/plain; version=0.0.4; charset=utf-8", RenderMetrics(snapshot, now)};
    } else if (path == "/api/mode" || path == "/api/plans" || path == "/api/operations") {
        response = Error(405, "METHOD_NOT_ALLOWED");
    } else if ((path == "/api/history" || path == "/api/logs" || path == "/api/logs/export") && snapshot.Diagnostics) {
        const auto rows = path == "/api/history" ? snapshot.Diagnostics->ReadHistory(Query(target, "after"))
            : snapshot.Diagnostics->ReadLogs(Query(target, "after"), Query(target, "level"), Query(target, "search"), Query(target, "pile"));
        response.Body = NJson::WriteJson(rows, false, true);
        if (path == "/api/logs/export") {
            response.ContentType = "application/x-ndjson; charset=utf-8";
            auto metadata = rows;
            metadata.EraseValue("rows");
            response.Body = NJson::WriteJson(metadata, false, true) + "\n";
            for (const auto& row : rows["rows"].GetArray()) {
                response.Body += NJson::WriteJson(row, false, true) + "\n";
            }
        }
    } else if (path.StartsWith("/api/operations/") && snapshot.Diagnostics) {
        const auto operation = snapshot.Diagnostics->ReadOperation(TString(path.SubStr(16)));
        response = operation ? THttpResponse{200, "application/json; charset=utf-8", NJson::WriteJson(*operation, false, true)} : Error(404, "OPERATION_UNAVAILABLE");
    } else {
        response = Error(404, "NOT_FOUND");
    }
    if (head) {
        response.Body.clear();
    }
    return response;
}

NActors::IActor* CreateHttpActor(std::shared_ptr<TSnapshotStore> snapshots, NActors::TActorId httpProxy,
    std::shared_ptr<TCommandAdmission> admission)
{
    return new THttpActor(std::move(snapshots), httpProxy, std::move(admission));
}

} // namespace NBridgeSkipper
