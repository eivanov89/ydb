#pragma once

#include <ydb/apps/bridge_skipper/runtime/controller.h>
#include <ydb/apps/bridge_skipper/runtime/resources.h>
#include <library/cpp/json/json_value.h>
#include <library/cpp/monlib/dynamic_counters/counters.h>

#include <deque>
#include <functional>

namespace NBridgeSkipper {

using TApiJson = NJson::TJsonValue;

struct TApiRequest {
    TString Method;
    TString Target;
    TString ContentType;
    TString Body;
};

struct TApiResponse {
    unsigned Status = 200;
    TString ContentType = "application/json; charset=utf-8";
    TString Body;
    std::map<TString, TString> Headers;
};

struct TApiAction {
    TApiResponse Response;
    std::optional<TControllerSubmission> Submission;
    bool ReadAudit = false;
    bool ReadLocalStopAudit = false;
    ui64 AuditAfter = 0;
    size_t AuditLimit = 100;
};

struct TApiOptions {
    TConfig Config;
    TString ProcessId;
    // Empty chooses 32 cryptographically random process-local bytes.
    TString SigningKey;
    std::function<TString()> NextId;
    std::function<std::optional<TLocalStopReceipt>(const TString&)> LookupLocalReceipt;
    std::function<TLocalStopReceipt(const TString&, const TString&, const TString&)> Stop;
    std::function<TLocalStopReceipt(const TString&, const TString&, ui64, const TString&, const TString&)> Release;
    std::function<TRuntimeLogPage(ui64, size_t, bool)> ReadLogs;
};

struct TLogLine {
    ui64 Sequence = 0;
    TString Json;
};

// Bounded process ring, exact cursors and restart/overflow signaling. Log calls
// accept structured bounded fields; never feed SDK payloads or credential data.
class TDiagnosticRing {
public:
    explicit TDiagnosticRing(size_t capacity = 1000);
    void Append(TApiJson fields);
    TApiJson Read(ui64 after, size_t limit, bool reset) const;

private:
    size_t Capacity;
    ui64 Next = 1;
    std::deque<TLogLine> Lines;
};

// All handlers are synchronous CPU-only pure request/projection work. Mutations
// return a submission for the actor adapter; no store/SDK waits happen here.
class THttpApi {
public:
    explicit THttpApi(TApiOptions options);
    ~THttpApi();
    TApiAction Handle(const TApiRequest& request, const TControllerSnapshot& snapshot,
        const TLocalStopReceipt& stop);
    TApiResponse Receipt(const TProposalResult& result) const;
    TApiResponse Unknown(TStringBuf requestId) const;
    void BeginAdmission(TString requestId, TString canonicalHash);
    void EndAdmission(TStringBuf requestId);
    TDiagnosticRing& Logs();
    TIntrusivePtr<NMonitoring::TDynamicCounters> Counters() const;

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
};

TApiJson ParseStrictJson(TStringBuf body);
TString CanonicalApiJson(const TApiJson& value);
bool IsRequestUuid(TStringBuf value);
TApiResponse ApiError(unsigned status, TString code, TString message,
    bool retryable = false, std::optional<TString> operationId = {});
TApiJson ProjectState(const TControllerSnapshot& snapshot, const TConfig& config,
    const TLocalStopReceipt& stop);
TApiJson ProjectOperation(const TOperation& operation, const TDurableState& state);
void UpdateHttpMetrics(const TControllerSnapshot& snapshot, const TConfig& config,
    const TLocalStopReceipt& stop, size_t pendingStorage,
    const TIntrusivePtr<NMonitoring::TDynamicCounters>& counters);
TApiResponse ReadTimeoutResponse();

// Register this actor in the HTTP pool. It registers only the explicit static,
// API, metrics and allowlisted monitor routes on the supplied NHttp proxy.
NActors::IActor* CreateHttpActor(const TConfig& config,
    std::shared_ptr<TRuntimeResources> resources, NActors::TActorId controller,
    NActors::TActorId httpProxy);

} // namespace NBridgeSkipper
