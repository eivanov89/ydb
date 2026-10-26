#pragma once

#include "http.h"

#include <util/generic/yexception.h>
#include <util/stream/str.h>

namespace NBridgeSkipper {

struct TApiException : yexception {
    unsigned Status = 400;
    TString Code = "MALFORMED_REQUEST";

    TApiException(unsigned status, TString code, TStringBuf message)
        : Status(status)
        , Code(std::move(code))
    {
        *this << message;
    }
};

void Require(bool condition, TStringBuf message, unsigned status = 400, TString code = "MALFORMED_REQUEST");
void Fields(const TApiJson& value, std::initializer_list<TStringBuf> fields);
TString String(const TApiJson& value, TStringBuf field, size_t max = 4096);
bool Boolean(const TApiJson& value, TStringBuf field);
std::optional<TString> NullableString(const TApiJson& value, TStringBuf field);
ui64 Decimal(TStringBuf value);
TString BoundedDiagnostic(TStringBuf value, size_t max = 512);
TApiJson Array();
TApiJson Object();
TApiJson Null();
TApiJson ApiObject();
TApiResponse JsonResponse(TApiJson value, unsigned status = 200);
TString ModeName(ERequestedMode mode);
TString KindName(EOperationKind kind);
TString StatusName(EOperationStatus status);
TString MethodName(EBridgeMethod method);
TString PileStateName(EPileState state);
TString SourceName(ESourceKind source);
TApiJson NamedStates(const TNamedPileStates& states);
TApiJson ReceiptJson(const TCommandResult& result);
TApiJson StopJson(const TLocalStopReceipt& stop);
ui64 AgeMs(TInstant now, TInstant then);

struct TReadLiveness {
    TInstant Now;
    ui64 AgeMs = 0;
    bool Stalled = false;
    bool EvidenceExpired = false;
};

TReadLiveness ReadLiveness(const TControllerSnapshot& snapshot);

} // namespace NBridgeSkipper
