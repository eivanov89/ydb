#include <ydb/apps/bridge_skipper/http/http.h>
#include <ydb/apps/bridge_skipper/runtime/events.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <ydb/library/actors/http/http_proxy.h>
#include <library/cpp/json/json_reader.h>
#include <library/cpp/testing/unittest/registar.h>

#include <cstring>

namespace NBridgeSkipper {
namespace {

struct THttpFixture {
    NActors::TTestActorRuntimeBase Runtime;
    std::shared_ptr<TSnapshotStore> Store = std::make_shared<TSnapshotStore>();
    std::shared_ptr<TCommandAdmission> Admission = std::make_shared<TCommandAdmission>();
    NActors::TActorId Http;
    NActors::TActorId Sender;
    NActors::TActorId Controller;

    THttpFixture() {
        Runtime.Initialize();
        Store->Publish(std::make_shared<TSkipperSnapshot>());
        const auto proxy = Runtime.AllocateEdgeActor();
        Sender = Runtime.AllocateEdgeActor();
        Controller = Runtime.AllocateEdgeActor();
        Http = Runtime.Register(CreateHttpActor(Store, proxy, Admission));
        Runtime.GrabEdgeEvent<NHttp::TEvHttpProxy::TEvRegisterHandler>(proxy);
    }

    void Post(TStringBuf path, TStringBuf body, TStringBuf type = "application/json") {
        const TString wire = "POST " + TString(path) + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: "
            + TString(type) + "\r\nContent-Length: " + ::ToString(body.size()) + "\r\n\r\n" + TString(body);
        NHttp::THttpIncomingRequestPtr request = new NHttp::THttpIncomingRequest();
        request->EnsureEnoughSpaceAvailable(wire.size());
        memcpy(request->Pos(), wire.data(), wire.size());
        request->Advance(wire.size());
        Runtime.Send(new NActors::IEventHandle(Http, Sender,
            new NHttp::TEvHttpProxy::TEvHttpIncomingRequest(request), 0, 39), 0, true);
    }

    TString Response(TStringBuf status) {
        const auto response = Runtime.GrabEdgeEvent<NHttp::TEvHttpProxy::TEvHttpOutgoingResponse>(Sender);
        UNIT_ASSERT_VALUES_EQUAL(response->Cookie, 39);
        UNIT_ASSERT_VALUES_EQUAL(response->Get()->Response->Status, status);
        UNIT_ASSERT_STRING_CONTAINS(response->Get()->Response->Headers, "Cache-Control: no-store");
        UNIT_ASSERT(!response->Get()->Response->Headers.Contains("Access-Control-Allow-Origin"));
        return TString(response->Get()->Response->Body);
    }
};

const TString ModeBody = R"({"mode":"observe","expected_boot_id":"boot","expected_mode_revision":"1","request_id":"off"})";

} // namespace

Y_UNIT_TEST_SUITE(THttpCommands) {
    Y_UNIT_TEST(LogSearchMatchesTheExportedRecordEncoding) {
        TSkipperSnapshot snapshot;
        snapshot.Diagnostics = std::make_shared<TDiagnosticStore>("boot");
        snapshot.Diagnostics->Log("info", "message", TInstant::Seconds(1));
        const auto exported = RouteHttpRequest("GET", "/api/logs/export", snapshot,
            TMonotonic::Seconds(1), TInstant::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(exported.Status, 200);
        const TStringBuf record = TStringBuf(exported.Body).After('\n').Before('\n');
        // A compact exported prefix spans timestamp and cursor fields. The
        // browser searches the same record received from the API.
        const auto end = record.find(",\"level\"");
        UNIT_ASSERT(end != TStringBuf::npos);
        const TString query(record.SubStr(0, end));
        const auto filtered = snapshot.Diagnostics->ReadLogs({}, {}, query);
        UNIT_ASSERT_VALUES_EQUAL(filtered["rows"].GetArray().size(), 1);
    }

    Y_UNIT_TEST(ParserRejectsUnsupportedActionsBodiesAndNonDecimalRevisions) {
        THttpResponse error;
        UNIT_ASSERT(ParseCommand("/api/mode", "application/json; charset=utf-8", ModeBody, error));
        for (const TStringBuf body : {R"({"action":"remove","snapshot_revision":"b"})",
                R"({"action":"rejoin","snapshot_revision":"b"})", "[]", "{}", "{broken"})
        {
            UNIT_ASSERT(!ParseCommand("/api/plans", "application/json", body, error));
            UNIT_ASSERT_VALUES_EQUAL(error.Status, 400);
        }
        const auto emptyPile = ParseCommand("/api/plans", "application/json",
            R"({"action":"rejoin","target_pile":"","snapshot_revision":"b"})", error);
        UNIT_ASSERT(emptyPile && emptyPile->Target && emptyPile->Target->empty());
        UNIT_ASSERT(!ParseCommand("/api/mode", "text/plain", ModeBody, error));
        UNIT_ASSERT(!ParseCommand("/api/mode", "application/json", TString(65537, ' '), error));
        UNIT_ASSERT_VALUES_EQUAL(error.Status, 413);
        UNIT_ASSERT(!ParseCommand("/api/mode", "application/json",
            R"({"mode":"auto","expected_boot_id":"b","expected_mode_revision":1,"request_id":"x","reviewed_snapshot_revision":"b"})", error));
    }

    Y_UNIT_TEST(AdmissionAndSyntaxFailureNeverForwardCommands) {
        THttpFixture f;
        f.Post("/api/mode", ModeBody);
        UNIT_ASSERT_STRING_CONTAINS(f.Response("503"), "CONTROLLER_UNAVAILABLE");
        f.Admission->Open(f.Controller);
        f.Post("/api/plans", R"({"action":"remove","snapshot_revision":"b"})");
        UNIT_ASSERT_STRING_CONTAINS(f.Response("400"), "INVALID_COMMAND");
        f.Admission->Close();
        f.Post("/api/mode", ModeBody);
        UNIT_ASSERT_STRING_CONTAINS(f.Response("503"), "CONTROLLER_UNAVAILABLE");
    }

    Y_UNIT_TEST(ControllerRepliesPreserveCorrelationAndInternalErrorDetails) {
        THttpFixture f;
        f.Admission->Open(f.Controller);
        f.Post("/api/mode", ModeBody);
        const auto command = f.Runtime.GrabEdgeEvent<TEvSkipper::TEvCommand>(f.Controller);
        UNIT_ASSERT(command->Get()->Command.Mode == EMode::Observe);
        UNIT_ASSERT_VALUES_EQUAL(command->Cookie, 1);
        TCommandResult result;
        result.Status = 409;
        result.Body["error"] = "REJECTED_AT_GENERATION";
        result.Body["status"] = "INTERNAL_ERROR";
        f.Runtime.Send(new NActors::IEventHandle(command->Sender, f.Controller,
            new TEvSkipper::TEvCommandResult(result), 0, command->Cookie), 0, true);
        UNIT_ASSERT_STRING_CONTAINS(f.Response("409"), "INTERNAL_ERROR");
    }

    Y_UNIT_TEST(UndeliveredAndTimeoutHaveDistinctOutcomeContracts) {
        THttpFixture f;
        f.Admission->Open(f.Controller);
        f.Post("/api/mode", ModeBody);
        const auto command = f.Runtime.GrabEdgeEvent<TEvSkipper::TEvCommand>(f.Controller);
        f.Runtime.Send(new NActors::IEventHandle(command->Sender, {}, new NActors::TEvents::TEvWakeup()), 0, true);
        UNIT_ASSERT_STRING_CONTAINS(f.Response("504"), "COMMAND_OUTCOME_UNKNOWN");
        // Timeout ends the HTTP actor only, leaving the original command valid.
        TCommandResult result;
        f.Runtime.Send(new NActors::IEventHandle(command->Sender, f.Controller,
            new TEvSkipper::TEvCommandResult(result), 0, command->Cookie), 0, true);
        const auto missing = f.Runtime.Register(CreateHttpActor(f.Store, f.Runtime.AllocateEdgeActor(), {}));
        Y_UNUSED(missing);
        f.Admission->Open(NActors::TActorId(0, "missing"));
        f.Post("/api/mode", ModeBody);
        UNIT_ASSERT_STRING_CONTAINS(f.Response("503"), "CONTROLLER_UNAVAILABLE");
    }
}

} // namespace NBridgeSkipper
