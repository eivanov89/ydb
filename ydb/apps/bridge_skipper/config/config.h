#pragma once

#include <util/datetime/base.h>
#include <util/generic/hash.h>
#include <util/generic/maybe.h>
#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>

namespace NBridgeSkipper {

enum class EEvidenceProfile {
    GlobalBscV1,
    StaticWhiteboardV1,
    NodewardenLocalV1,
};

TStringBuf EvidenceProfileName(EEvidenceProfile profile);

// Recognition selects a parser. It never certifies production Auto support.
bool IsProductionAutoQualified(EEvidenceProfile profile);

struct TEndpoint {
    TString Uri;
    TString Scheme;
    TString Host; // IPv6 brackets are retained, as in Uri.
    ui16 Port = 0;
};

TEndpoint ParseEndpoint(TStringBuf uri);

struct TYdbAuthConfig {
    TMaybe<TString> CaFile;
    TMaybe<TString> ClientCertFile;
    TMaybe<TString> ClientKeyFile;
    TMaybe<TString> TokenFile;
    bool AllowPlaintextToken = false;
};

struct THttpConfig {
    TString Listen = "127.0.0.1:8080";
};

struct TPollingConfig {
    TDuration RoundInterval = TDuration::Seconds(15);
    TDuration RoundDeadline = TDuration::Seconds(12);
    TDuration BracketAdminTimeout = TDuration::Seconds(1);
    TDuration HealthOperationTimeout = TDuration::Seconds(8);
    TDuration HealthClientTimeout = TDuration::Seconds(9);
    TDuration AdminInterval = TDuration::Seconds(1);
    ui32 GlobalAdminConcurrency = 16;
    TDuration ConfigInterval = TDuration::Seconds(30);
    TDuration ReadRpcTimeout = TDuration::Seconds(10);
    TDuration ObservationTtl = TDuration::Seconds(20);
    TDuration AdmissionBudget = TDuration::Seconds(5);
    TDuration DispatchMargin = TDuration::Seconds(2);
    TDuration SchedulerTolerance = TDuration::Seconds(5);
    TDuration ConfirmationStartGap = TDuration::Seconds(20);
    ui32 SampleNodesPerPile = 3;
    ui32 PerPileHealthConcurrency = 3;
};

struct TOperationsConfig {
    TDuration MutationClientTimeout = TDuration::Seconds(120);
    TDuration MembershipClientTimeout = TDuration::Seconds(600);
    THashMap<TString, TMaybe<TDuration>> AttentionAfter = {
        {"failover", TDuration::Seconds(300)},
        {"switchover", TDuration::Seconds(300)},
        {"takedown", TDuration::Seconds(300)},
        {"primary_maintenance", TDuration::Seconds(600)},
        {"rejoin", Nothing()},
        {"convert", TDuration::Seconds(3600)},
        {"add_pile", TDuration::Seconds(3600)},
        {"remove_pile", TDuration::Seconds(3600)},
    };
};

struct TLoggingConfig {
    TString Level = "info";
    ui32 DisplayRingLines = 1000;
};

struct TConfig {
    ui32 SchemaVersion = 1;
    TString ClusterId;
    TString AuthorityBackend = "local";
    TVector<TEndpoint> YdbEndpoints;
    EEvidenceProfile EvidenceProfile = EEvidenceProfile::GlobalBscV1;
    TYdbAuthConfig YdbAuth;
    THttpConfig Http;
    TPollingConfig Polling;
    TOperationsConfig Operations;
    TLoggingConfig Logging;
};

// These functions throw yexception on invalid input. Parsing validates all fields.
TConfig ParseConfig(TStringBuf yaml);
TConfig LoadConfig(const TString& path);
void ValidateConfig(const TConfig& config);

// Startup-only secrets: never include this object in durable state or diagnostics.
struct TLoadedCredentials {
    TMaybe<TString> Ca;
    TMaybe<TString> ClientCert;
    TMaybe<TString> ClientKey;
    TMaybe<TString> Token;
    bool PlaintextTokenOptIn = false;
};

TLoadedCredentials LoadCredentials(const TConfig& config);
bool IsLoopbackHttpListen(TStringBuf listen);

enum class EEntrypointAction {
    Run,
    CheckConfig,
    Version,
    Help,
};

struct TEntrypointArguments {
    EEntrypointAction Action = EEntrypointAction::Run;
    TString ConfigFile;
    TString StateDir;
    TString MemberId;
};

// Arguments exclude argv[0]. Help/version need no runtime configuration.
TEntrypointArguments ParseEntrypointArguments(const TVector<TString>& arguments);
TStringBuf EntrypointHelp();

// Performs the same pure config validation and file reads for Run/CheckConfig.
// Does not open listeners, contact YDB, or create/change the state directory.
TLoadedCredentials ValidateStartup(const TConfig& config, const TEntrypointArguments& arguments);

} // namespace NBridgeSkipper
