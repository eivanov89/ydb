#include "config.h"

#include <yaml-cpp/yaml.h>

#include <util/generic/hash_set.h>
#include <util/generic/yexception.h>
#include <util/network/socket.h>
#include <util/stream/file.h>
#include <util/string/cast.h>
#include <util/string/builder.h>

#include <initializer_list>
#include <limits>

namespace NBridgeSkipper {
namespace {

constexpr size_t MaxConfigBytes = 1024 * 1024;
constexpr size_t MaxCredentialBytes = 16 * 1024 * 1024;

void Require(bool condition, TStringBuf message) {
    if (!condition) {
        throw yexception() << message;
    }
}

TString Scalar(const YAML::Node& node, TStringBuf path) {
    Require(node.IsScalar(), TStringBuilder() << path << " must be a scalar");
    return TString(node.Scalar());
}

void Map(const YAML::Node& node, TStringBuf path, std::initializer_list<TStringBuf> allowed) {
    Require(node.IsMap(), TStringBuilder() << path << " must be a mapping");
    THashSet<TString> seen;
    for (const auto& entry : node) {
        const auto key = Scalar(entry.first, path);
        Require(seen.insert(key).second, TStringBuilder() << path << ": duplicate key " << key);
        bool known = false;
        for (const auto item : allowed) {
            known = known || key == item;
        }
        Require(known, TStringBuilder() << path << ": unknown key " << key);
    }
}

ui64 Unsigned(TStringBuf text, TStringBuf path) {
    Require(!text.empty(), TStringBuilder() << path << " must be a positive decimal integer");
    for (const char c : text) {
        Require(c >= '0' && c <= '9', TStringBuilder() << path << " must be a positive decimal integer");
    }
    ui64 result = 0;
    Require(TryFromString(text, result) && result > 0, TStringBuilder() << path << " must be a positive decimal integer");
    return result;
}

ui32 UInt32(const YAML::Node& node, TStringBuf path) {
    const auto value = Unsigned(Scalar(node, path), path);
    Require(value <= std::numeric_limits<ui32>::max(), TStringBuilder() << path << " is too large");
    return value;
}

TDuration Duration(const YAML::Node& node, TStringBuf path) {
    const auto seconds = Unsigned(Scalar(node, path), path);
    Require(seconds <= std::numeric_limits<ui64>::max() / 1000000, TStringBuilder() << path << " is too large");
    return TDuration::Seconds(seconds);
}

bool Boolean(const YAML::Node& node, TStringBuf path) {
    const auto value = Scalar(node, path);
    Require(value == "true" || value == "false", TStringBuilder() << path << " must be true or false");
    return value == "true";
}

TMaybe<TString> OptionalPath(const YAML::Node& node, TStringBuf path) {
    if (!node || node.IsNull()) {
        return Nothing();
    }
    const auto value = Scalar(node, path);
    Require(!value.empty() && value.find('\0') == TString::npos, TStringBuilder() << path << " must be a nonempty file path or null");
    return value;
}

struct THostPort {
    TString Host;
    ui16 Port;
};

THostPort HostPort(TStringBuf address) {
    Require(!address.empty(), "host and port are required");
    for (const char c : address) {
        Require(static_cast<unsigned char>(c) > 32 && static_cast<unsigned char>(c) < 127, "host:port contains whitespace or invalid characters");
    }
    Require(address.find_first_of("/@?#\\%") == TStringBuf::npos, "host:port cannot contain userinfo, path, query, fragment or escapes");
    TStringBuf host;
    TStringBuf port;
    if (address.StartsWith('[')) {
        const auto close = address.find(']');
        Require(close != TStringBuf::npos && close + 1 < address.size() && address[close + 1] == ':', "IPv6 address requires [host]:port");
        host = address.substr(0, close + 1);
        port = address.substr(close + 2);
        in6_addr parsed;
        const TString ip(address.substr(1, close - 1));
        Require(inet_pton(AF_INET6, ip.c_str(), &parsed) == 1, "invalid IPv6 address");
    } else {
        const auto colon = address.find(':');
        Require(colon != TStringBuf::npos && address.find(':', colon + 1) == TStringBuf::npos, "endpoint requires host:port; IPv6 requires brackets");
        host = address.substr(0, colon);
        port = address.substr(colon + 1);
        Require(!host.empty(), "endpoint host is required");
        for (const char c : host) {
            Require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_', "invalid endpoint host");
        }
    }
    const auto number = Unsigned(port, "port");
    Require(number <= 65535, "port must be in 1..65535");
    return {TString(host), static_cast<ui16>(number)};
}

TString ReadBounded(const TString& path, size_t maxBytes, TStringBuf kind) {
    // Read one byte beyond the bound, including for streams whose size is unknown.
    TFileInput input(path);
    TString result = TString::Uninitialized(maxBytes + 1);
    const size_t size = input.Load(result.Detach(), result.size());
    Require(size <= maxBytes, TStringBuilder() << kind << " exceeds size limit");
    result.resize(size);
    return result;
}

TMaybe<TString> ReadCredential(const TMaybe<TString>& path, TStringBuf kind) {
    if (!path) {
        return Nothing();
    }
    TString value;
    try {
        value = ReadBounded(*path, MaxCredentialBytes, kind);
    } catch (const yexception&) {
        // Do not copy file content, path, or lower-level exception into logs.
        throw yexception() << "cannot load " << kind << " credential file";
    }
    Require(!value.empty(), TStringBuilder() << kind << " credential file is empty");
    Require(value.find('\0') == TString::npos, TStringBuilder() << kind << " credential file contains NUL");
    return value;
}

void ValidateDuration(TDuration value, TStringBuf name) {
    Require(value > TDuration::Zero() && value.MicroSeconds() % 1000000 == 0, TStringBuilder() << name << " must be positive whole seconds");
}

} // namespace

TStringBuf EvidenceProfileName(EEvidenceProfile profile) {
    switch (profile) {
        case EEvidenceProfile::GlobalBscV1:
            return "global_bsc_v1";
        case EEvidenceProfile::StaticWhiteboardV1:
            return "static_whiteboard_v1";
        case EEvidenceProfile::NodewardenLocalV1:
            return "nodewarden_local_v1";
    }
    throw yexception() << "unknown evidence profile";
}

bool IsProductionAutoQualified(EEvidenceProfile profile) {
    EvidenceProfileName(profile);
    return false;
}

TEndpoint ParseEndpoint(TStringBuf uri) {
    const auto separator = uri.find("://");
    Require(separator != TStringBuf::npos, "endpoint requires explicit grpc:// or grpcs:// scheme");
    const auto scheme = uri.substr(0, separator);
    Require(scheme == "grpc" || scheme == "grpcs", "endpoint scheme must be grpc or grpcs");
    const auto parsed = HostPort(uri.substr(separator + 3));
    return {TString(uri), TString(scheme), parsed.Host, parsed.Port};
}

bool IsLoopbackHttpListen(TStringBuf listen) {
    const auto parsed = HostPort(listen);
    if (parsed.Host == "localhost") {
        return true;
    }
    if (parsed.Host.StartsWith('[')) {
        in6_addr ip;
        const TString host(TStringBuf(parsed.Host).substr(1, parsed.Host.size() - 2));
        return inet_pton(AF_INET6, host.c_str(), &ip) == 1 && IN6_IS_ADDR_LOOPBACK(&ip);
    }
    in_addr ip;
    return inet_pton(AF_INET, parsed.Host.c_str(), &ip) == 1 && (ntohl(ip.s_addr) >> 24) == 127;
}

TConfig ParseConfig(TStringBuf yaml) {
    Require(yaml.size() <= MaxConfigBytes, "config exceeds size limit");
    TConfig result;
    try {
        const auto documents = YAML::LoadAll(std::string(yaml.data(), yaml.size()));
        Require(documents.size() == 1, "config must contain exactly one YAML document");
        const auto root = documents.front();
        Map(root, "config", {"schema_version", "cluster_id", "controller", "ydb_endpoints", "evidence", "ydb_auth", "http", "polling", "operations", "logging"});
        Require(root["schema_version"].IsDefined(), "schema_version is required");
        Require(root["cluster_id"].IsDefined(), "cluster_id is required");
        Require(root["ydb_endpoints"].IsDefined(), "ydb_endpoints is required");
        result.SchemaVersion = UInt32(root["schema_version"], "schema_version");
        result.ClusterId = Scalar(root["cluster_id"], "cluster_id");
        const auto endpoints = root["ydb_endpoints"];
        Require(endpoints.IsSequence(), "ydb_endpoints must be a sequence");
        for (const auto& endpoint : endpoints) {
            result.YdbEndpoints.push_back(ParseEndpoint(Scalar(endpoint, "ydb_endpoints[]")));
        }
        if (const auto node = root["controller"]) {
            Map(node, "controller", {"authority_backend"});
            if (node["authority_backend"]) {
                result.AuthorityBackend = Scalar(node["authority_backend"], "controller.authority_backend");
            }
        }
        if (const auto node = root["evidence"]) {
            Map(node, "evidence", {"profile"});
            Require(node["profile"].IsDefined(), "evidence.profile is required");
            const auto profile = Scalar(node["profile"], "evidence.profile");
            bool found = false;
            for (const auto candidate : {EEvidenceProfile::GlobalBscV1, EEvidenceProfile::StaticWhiteboardV1, EEvidenceProfile::NodewardenLocalV1}) {
                if (EvidenceProfileName(candidate) == profile) {
                    result.EvidenceProfile = candidate;
                    found = true;
                }
            }
            Require(found, "unknown evidence.profile");
        }
        if (const auto node = root["ydb_auth"]) {
            Map(node, "ydb_auth", {"ca_file", "client_cert_file", "client_key_file", "token_file", "allow_plaintext_token"});
            result.YdbAuth.CaFile = OptionalPath(node["ca_file"], "ydb_auth.ca_file");
            result.YdbAuth.ClientCertFile = OptionalPath(node["client_cert_file"], "ydb_auth.client_cert_file");
            result.YdbAuth.ClientKeyFile = OptionalPath(node["client_key_file"], "ydb_auth.client_key_file");
            result.YdbAuth.TokenFile = OptionalPath(node["token_file"], "ydb_auth.token_file");
            if (node["allow_plaintext_token"]) {
                result.YdbAuth.AllowPlaintextToken = Boolean(node["allow_plaintext_token"], "ydb_auth.allow_plaintext_token");
            }
        }
        if (const auto node = root["http"]) {
            Map(node, "http", {"listen"});
            if (node["listen"]) {
                result.Http.Listen = Scalar(node["listen"], "http.listen");
            }
        }
        if (const auto node = root["polling"]) {
            Map(node, "polling", {"round_interval_seconds", "round_deadline_seconds", "bracket_admin_timeout_seconds", "health_operation_timeout_seconds", "health_client_timeout_seconds", "admin_interval_seconds", "global_admin_concurrency", "config_interval_seconds", "read_rpc_timeout_seconds", "observation_ttl_seconds", "admission_budget_seconds", "dispatch_margin_seconds", "scheduler_tolerance_seconds", "confirmation_start_gap_seconds", "sample_nodes_per_pile", "per_pile_health_concurrency"});
            auto& p = result.Polling;
            const std::initializer_list<std::pair<TStringBuf, TDuration*>> durations = {
                {"round_interval_seconds", &p.RoundInterval}, {"round_deadline_seconds", &p.RoundDeadline},
                {"bracket_admin_timeout_seconds", &p.BracketAdminTimeout}, {"health_operation_timeout_seconds", &p.HealthOperationTimeout},
                {"health_client_timeout_seconds", &p.HealthClientTimeout}, {"admin_interval_seconds", &p.AdminInterval},
                {"config_interval_seconds", &p.ConfigInterval}, {"read_rpc_timeout_seconds", &p.ReadRpcTimeout},
                {"observation_ttl_seconds", &p.ObservationTtl}, {"admission_budget_seconds", &p.AdmissionBudget},
                {"dispatch_margin_seconds", &p.DispatchMargin}, {"scheduler_tolerance_seconds", &p.SchedulerTolerance},
                {"confirmation_start_gap_seconds", &p.ConfirmationStartGap},
            };
            for (const auto& [key, target] : durations) {
                if (node[std::string(key)]) {
                    *target = Duration(node[std::string(key)], key);
                }
            }
            for (const auto& [key, target] : {std::pair{"global_admin_concurrency", &p.GlobalAdminConcurrency}, {"sample_nodes_per_pile", &p.SampleNodesPerPile}, {"per_pile_health_concurrency", &p.PerPileHealthConcurrency}}) {
                if (node[key]) {
                    *target = UInt32(node[key], key);
                }
            }
        }
        if (const auto node = root["operations"]) {
            Map(node, "operations", {"mutation_client_timeout_seconds", "membership_client_timeout_seconds", "attention_after_seconds"});
            if (node["mutation_client_timeout_seconds"]) {
                result.Operations.MutationClientTimeout = Duration(node["mutation_client_timeout_seconds"], "operations.mutation_client_timeout_seconds");
            }
            if (node["membership_client_timeout_seconds"]) {
                result.Operations.MembershipClientTimeout = Duration(node["membership_client_timeout_seconds"], "operations.membership_client_timeout_seconds");
            }
            if (const auto attention = node["attention_after_seconds"]) {
                Map(attention, "operations.attention_after_seconds", {"failover", "switchover", "takedown", "primary_maintenance", "rejoin", "convert", "add_pile", "remove_pile"});
                for (const auto& entry : attention) {
                    const auto key = Scalar(entry.first, "attention_after_seconds");
                    result.Operations.AttentionAfter[key] = entry.second.IsNull() ? TMaybe<TDuration>(Nothing()) : TMaybe<TDuration>(Duration(entry.second, key));
                }
            }
        }
        if (const auto node = root["logging"]) {
            Map(node, "logging", {"level", "display_ring_lines"});
            if (node["level"]) {
                result.Logging.Level = Scalar(node["level"], "logging.level");
            }
            if (node["display_ring_lines"]) {
                result.Logging.DisplayRingLines = UInt32(node["display_ring_lines"], "logging.display_ring_lines");
            }
        }
    } catch (const YAML::Exception&) {
        // yaml-cpp exception messages can include source scalar/credential text.
        throw yexception() << "invalid YAML configuration";
    }
    ValidateConfig(result);
    return result;
}

TConfig LoadConfig(const TString& path) {
    return ParseConfig(ReadBounded(path, MaxConfigBytes, "config"));
}

void ValidateConfig(const TConfig& config) {
    Require(config.SchemaVersion == 1, "unsupported schema_version");
    Require(!config.ClusterId.empty() && config.ClusterId.find('\0') == TString::npos, "cluster_id must be nonempty and contain no NUL");
    Require(config.AuthorityBackend == "local", "v1 supports controller.authority_backend: local only");
    EvidenceProfileName(config.EvidenceProfile);
    Require(!config.YdbEndpoints.empty(), "ydb_endpoints must not be empty");
    TString scheme;
    for (const auto& endpoint : config.YdbEndpoints) {
        const auto parsed = ParseEndpoint(endpoint.Uri);
        Require(parsed.Scheme == endpoint.Scheme && parsed.Host == endpoint.Host && parsed.Port == endpoint.Port, "endpoint fields disagree with URI");
        Require(scheme.empty() || scheme == parsed.Scheme, "mixed grpc/grpcs endpoint schemes are forbidden");
        scheme = parsed.Scheme;
    }
    const auto& auth = config.YdbAuth;
    for (const auto* path : {&auth.CaFile, &auth.ClientCertFile, &auth.ClientKeyFile, &auth.TokenFile}) {
        Require(!*path || (!(*path)->empty() && (*path)->find('\0') == TString::npos), "credential paths must be nonempty and contain no NUL");
    }
    Require(auth.ClientCertFile.Defined() == auth.ClientKeyFile.Defined(), "client_cert_file and client_key_file must be supplied together");
    Require(scheme != "grpc" || !auth.TokenFile || auth.AllowPlaintextToken, "plaintext token requires explicit ydb_auth.allow_plaintext_token: true");
    HostPort(config.Http.Listen);
    const auto& p = config.Polling;
    for (const auto duration : {p.RoundInterval, p.RoundDeadline, p.BracketAdminTimeout, p.HealthOperationTimeout, p.HealthClientTimeout, p.AdminInterval, p.ConfigInterval, p.ReadRpcTimeout, p.ObservationTtl, p.AdmissionBudget, p.DispatchMargin, p.SchedulerTolerance, p.ConfirmationStartGap, config.Operations.MutationClientTimeout, config.Operations.MembershipClientTimeout}) {
        ValidateDuration(duration, "duration");
    }
    // Compare seconds to avoid overflow in TDuration microsecond arithmetic.
    Require(2 * p.BracketAdminTimeout.Seconds() + p.HealthClientTimeout.Seconds() < p.RoundDeadline.Seconds(), "bracket chain must fit strictly inside round_deadline_seconds");
    Require(p.RoundDeadline <= p.RoundInterval, "round_deadline_seconds must not exceed round_interval_seconds");
    Require(p.HealthOperationTimeout < p.HealthClientTimeout && p.HealthClientTimeout < p.ObservationTtl, "health timeouts must satisfy operation < client < observation TTL");
    Require(p.ObservationTtl.Seconds() >= p.RoundDeadline.Seconds() + p.AdmissionBudget.Seconds() + p.DispatchMargin.Seconds(), "observation_ttl_seconds must cover round deadline, admission budget and dispatch margin");
    Require(p.ConfirmationStartGap.Seconds() >= p.RoundInterval.Seconds() + p.SchedulerTolerance.Seconds(), "confirmation_start_gap_seconds must cover round interval and scheduler tolerance");
    Require(p.SampleNodesPerPile >= 1 && p.SampleNodesPerPile <= 101, "sample_nodes_per_pile must be in 1..101");
    Require(p.PerPileHealthConcurrency > 0, "per_pile_health_concurrency must be positive");
    Require(p.GlobalAdminConcurrency >= 3, "global_admin_concurrency must be at least 3");
    // A sample larger than health capacity is valid diagnostics configuration;
    // runtime reports CONFIG_BUDGET and blocks Auto without shrinking its sample.
    const TOperationsConfig defaults;
    Require(config.Operations.AttentionAfter.size() == defaults.AttentionAfter.size(), "attention_after_seconds must retain the known operation kinds");
    for (const auto& [key, value] : config.Operations.AttentionAfter) {
        Require(defaults.AttentionAfter.contains(key), "unknown attention_after_seconds operation kind");
        if (value) {
            ValidateDuration(*value, key);
        }
    }
    Require(config.Logging.Level == "trace" || config.Logging.Level == "debug" || config.Logging.Level == "info" || config.Logging.Level == "warn" || config.Logging.Level == "error" || config.Logging.Level == "fatal", "unsupported logging.level");
    Require(config.Logging.DisplayRingLines > 0, "logging.display_ring_lines must be positive");
}

TLoadedCredentials LoadCredentials(const TConfig& config) {
    ValidateConfig(config);
    TLoadedCredentials result;
    result.Ca = ReadCredential(config.YdbAuth.CaFile, "CA");
    result.ClientCert = ReadCredential(config.YdbAuth.ClientCertFile, "client certificate");
    result.ClientKey = ReadCredential(config.YdbAuth.ClientKeyFile, "client key");
    result.Token = ReadCredential(config.YdbAuth.TokenFile, "token");
    if (result.Token) {
        // Token files commonly end in a newline. Never transmit that delimiter.
        while (!result.Token->empty() && (result.Token->back() == '\n' || result.Token->back() == '\r')) {
            result.Token->pop_back();
        }
        Require(!result.Token->empty() && result.Token->find_first_of("\r\n") == TString::npos, "token credential must contain one nonempty line");
    }
    result.PlaintextTokenOptIn = config.YdbEndpoints.front().Scheme == "grpc" && config.YdbAuth.AllowPlaintextToken;
    return result;
}

TEntrypointArguments ParseEntrypointArguments(const TVector<TString>& arguments) {
    TEntrypointArguments result;
    THashSet<TString> seen;
    for (size_t i = 0; i < arguments.size(); ++i) {
        TStringBuf argument(arguments[i]);
        const auto equal = argument.find('=');
        const TString key(argument.substr(0, equal));
        Require(seen.insert(key).second, TStringBuilder() << "duplicate argument " << key);
        if (key == "--help" || key == "--version" || key == "--check-config") {
            Require(equal == TStringBuf::npos, TStringBuilder() << key << " does not take a value");
            Require(result.Action == EEntrypointAction::Run, "only one entrypoint action is allowed");
            result.Action = key == "--help" ? EEntrypointAction::Help : key == "--version" ? EEntrypointAction::Version : EEntrypointAction::CheckConfig;
        } else {
            Require(key == "--config" || key == "--state-dir" || key == "--member-id", TStringBuilder() << "unknown argument " << key);
            TString value;
            if (equal != TStringBuf::npos) {
                value = argument.substr(equal + 1);
            } else {
                Require(i + 1 < arguments.size() && !TStringBuf(arguments[i + 1]).StartsWith("--"), TStringBuilder() << key << " requires a value");
                value = arguments[++i];
            }
            Require(!value.empty() && value.find('\0') == TString::npos, TStringBuilder() << key << " requires a nonempty value");
            if (key == "--config") {
                result.ConfigFile = value;
            } else if (key == "--state-dir") {
                result.StateDir = value;
            } else {
                result.MemberId = value;
            }
        }
    }
    if (result.Action == EEntrypointAction::Run || result.Action == EEntrypointAction::CheckConfig) {
        Require(!result.ConfigFile.empty(), "--config FILE is required");
        Require(!result.StateDir.empty(), "--state-dir DIR is required");
        Require(!result.MemberId.empty(), "--member-id ID is required");
    }
    return result;
}

TStringBuf EntrypointHelp() {
    return "bridge_skipper --config FILE --state-dir DIR --member-id ID [--check-config]\n"
           "  --config FILE    Shared standalone YAML configuration (loaded once)\n"
           "  --state-dir DIR  Local durable store directory\n"
           "  --member-id ID   Nonempty identity of this process member\n"
           "  --check-config   Validate configuration, files and bind syntax; no listeners or RPCs\n"
           "  --version        Print build, API and schema versions\n"
           "  --help           Show this help\n"
           "Any client reaching http.listen is authorized for every route and mutation.\n"
           "No evidence profile is qualified for production Auto.\n";
}

TLoadedCredentials ValidateStartup(const TConfig& config, const TEntrypointArguments& arguments) {
    Require(arguments.Action == EEntrypointAction::Run || arguments.Action == EEntrypointAction::CheckConfig, "startup validation requires Run or CheckConfig action");
    for (const auto* value : {&arguments.ConfigFile, &arguments.StateDir, &arguments.MemberId}) {
        Require(!value->empty() && value->find('\0') == TString::npos, "startup requires nonempty config, state directory and member identity without NUL");
    }
    return LoadCredentials(config);
}

} // namespace NBridgeSkipper
