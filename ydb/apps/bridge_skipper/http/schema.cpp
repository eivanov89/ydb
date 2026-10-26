#include "internal.h"

#include <library/cpp/json/json_reader.h>
#include <library/cpp/json/json_writer.h>
#include <util/charset/utf8.h>
#include <util/string/cast.h>
#include <ydb/apps/bridge_skipper/domain/clock.h>

#include <algorithm>
#include <cctype>
#include <limits>

namespace NBridgeSkipper {
namespace {

class TStrictCallbacks : public NJson::TJsonCallbacks {
public:
    bool OnOpenMap() override {
        Stack.push_back(std::set<TString>());
        return Stack.size() <= 32;
    }

    bool OnCloseMap() override {
        Stack.pop_back();
        return true;
    }

    bool OnOpenArray() override {
        Stack.push_back(std::nullopt);
        return Stack.size() <= 32;
    }

    bool OnCloseArray() override {
        Stack.pop_back();
        return true;
    }

    bool OnMapKey(const TStringBuf& key) override {
        return !Stack.empty() && Stack.back() && Stack.back()->insert(TString(key)).second;
    }

    bool OnMapKeyNoCopy(const TStringBuf& key) override {
        return OnMapKey(key);
    }

private:
    std::vector<std::optional<std::set<TString>>> Stack;
};

} // namespace

void Require(bool condition, TStringBuf message, unsigned status, TString code) {
    if (!condition) {
        throw TApiException(status, std::move(code), message);
    }
}

TApiJson ParseStrictJson(TStringBuf body) {
    Require(body.size() <= 1024 * 1024, "JSON body exceeds 1 MiB", 413, "BODY_TOO_LARGE");
    Require(IsUtf(body), "Invalid UTF-8");
    TStrictCallbacks callbacks;
    NJson::TJsonReaderConfig config;
    config.MaxDepth = 32;
    const TString bytes(body);
    TStringInput stream(bytes);
    Require(NJson::ReadJson(&stream, &config, &callbacks), "Invalid JSON, excessive nesting or duplicate key");
    TApiJson result;
    Require(NJson::ReadJsonTree(body, &config, &result), "Invalid JSON");
    Require(result.IsMap(), "JSON object required");
    return result;
}

TString CanonicalApiJson(const TApiJson& value) {
    return NJson::WriteJson(&value, false, true, true);
}

void Fields(const TApiJson& value, std::initializer_list<TStringBuf> fields) {
    Require(value.IsMap() && value.GetMap().size() == fields.size(), "Missing or unknown fields");
    for (const auto field : fields) {
        Require(value.Has(field), "Missing or unknown fields");
    }
}

TString String(const TApiJson& value, TStringBuf field, size_t max) {
    Require(value.Has(field) && value[field].IsString() && value[field].GetString().size() <= max, "Expected bounded string field");
    return value[field].GetString();
}

bool Boolean(const TApiJson& value, TStringBuf field) {
    Require(value.Has(field) && value[field].IsBoolean(), "Expected boolean field");
    return value[field].GetBoolean();
}

std::optional<TString> NullableString(const TApiJson& value, TStringBuf field) {
    Require(value.Has(field), "Missing nullable field");
    if (value[field].IsNull()) {
        return std::nullopt;
    }
    return String(value, field, 128);
}

bool IsRequestUuid(TStringBuf value) {
    if (value.size() != 36) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') {
                return false;
            }
        } else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f') || (value[i] >= 'A' && value[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

ui64 Decimal(TStringBuf value) {
    Require(!value.empty() && (value.size() == 1 || value[0] != '0'), "Canonical decimal string required");
    ui64 result = 0;
    for (const auto ch : value) {
        Require(ch >= '0' && ch <= '9' && result <= (std::numeric_limits<ui64>::max() - (ch - '0')) / 10, "Decimal value out of range");
        result = result * 10 + ch - '0';
    }
    return result;
}

TString BoundedDiagnostic(TStringBuf value, size_t max) {
    // SDK error messages can include credentials, request metadata and paths.
    // Expose a stable redacted summary, never an unbounded raw payload.
    TString lower(value);
    char* const lowerData = lower.Detach();
    std::transform(lowerData, lowerData + lower.size(), lowerData, [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    for (const TStringBuf sensitive : {"authorization", "bearer ", "token", "private key", "-----begin", "password", "secret", "/users/", "/home/", ".pem", ".key"}) {
        if (lower.Contains(sensitive)) {
            return "[redacted diagnostic]";
        }
    }
    TString out(value.SubStr(0, std::min(value.size(), max)));
    while (!out.empty() && !IsUtf(out)) {
        out.pop_back();
    }
    if (value.size() > max) {
        out += "…";
    }
    return out;
}

TApiJson Array() {
    return TApiJson(NJson::JSON_ARRAY);
}

TApiJson Object() {
    return TApiJson(NJson::JSON_MAP);
}

TApiJson Null() {
    return TApiJson(NJson::JSON_NULL);
}

TApiJson ApiObject() {
    auto value = Object();
    value["api_version"] = 1;
    return value;
}

TApiResponse JsonResponse(TApiJson value, unsigned status) {
    TApiResponse response;
    response.Status = status;
    response.Body = CanonicalApiJson(value);
    response.Headers["Cache-Control"] = "no-store";
    response.Headers["Content-Security-Policy"] = "default-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'";
    response.Headers["X-Content-Type-Options"] = "nosniff";
    return response;
}

TApiResponse ApiError(unsigned status, TString code, TString message, bool retryable, std::optional<TString> operationId) {
    auto value = ApiObject();
    auto error = Object();
    error["code"] = std::move(code);
    error["message"] = BoundedDiagnostic(message);
    error["retryable"] = retryable;
    error["details"] = Object();
    error["operation_id"] = operationId ? TApiJson(*operationId) : Null();
    value["error"] = std::move(error);
    return JsonResponse(std::move(value), status);
}

TApiJson ReceiptJson(const TCommandResult& result) {
    auto value = ApiObject();
    value["status"] = result.Ok ? "committed" : "rejected";
    value["code"] = result.Code;
    value["operation_id"] = result.OperationId.empty() ? Null() : TApiJson(result.OperationId);
    value["index"] = ToString(result.Index);
    value["controller_revision"] = ToString(result.ControllerRevision);
    return value;
}

TApiJson StopJson(const TLocalStopReceipt& stop) {
    auto value = ApiObject();
    value["request_id"] = stop.RequestId;
    value["incarnation_id"] = stop.ProcessId;
    value["stop_revision"] = ToString(stop.Revision);
    value["latched"] = stop.Stopped;
    value["receipt_durability"] = stop.Durable ? "durable" : "volatile";
    value["marker_durable"] = stop.Durable;
    value["persistence_pending"] = stop.Pending;
    value["message"] = "Queued or in-flight YDB work may still execute";
    value["error"] = stop.Error.empty() ? Null() : TApiJson(BoundedDiagnostic(stop.Error));
    return value;
}

ui64 AgeMs(TInstant now, TInstant then) {
    return now >= then ? (now - then).MilliSeconds() : 0;
}

TReadLiveness ReadLiveness(const TControllerSnapshot& snapshot) {
    TReadLiveness live;
    live.Now = TSkipperClock::Now();
    live.AgeMs = AgeMs(live.Now, snapshot.GeneratedAt);
    live.Stalled = !snapshot.GeneratedAt || live.AgeMs > 5000;
    live.EvidenceExpired = snapshot.Decision && snapshot.Decision->Kind == EDecisionKind::Failover && live.Now >= snapshot.Decision->EvidenceExpiry;
    return live;
}

} // namespace NBridgeSkipper
