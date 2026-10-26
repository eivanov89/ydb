#include <ydb/apps/bridge_skipper/config/config.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/yexception.h>
#include <util/stream/file.h>
#include <util/string/builder.h>
#include <util/system/tempfile.h>

namespace NBridgeSkipper {
namespace {

TString Minimal(TStringBuf extra = {}) {
    return TStringBuilder() << "schema_version: 1\ncluster_id: test-bridge\nydb_endpoints: [grpcs://node.example:2135]\n" << extra;
}

void Reject(TStringBuf yaml, TStringBuf error) {
    UNIT_ASSERT_EXCEPTION_CONTAINS(ParseConfig(yaml), yexception, error);
}

} // namespace

Y_UNIT_TEST_SUITE(TSkipperConfiguration) {
    Y_UNIT_TEST(DefaultsMatchDocumentedBudgets) {
        const auto config = ParseConfig(Minimal());
        UNIT_ASSERT_VALUES_EQUAL(config.ClusterId, "test-bridge");
        UNIT_ASSERT_VALUES_EQUAL(config.AuthorityBackend, "local");
        UNIT_ASSERT_VALUES_EQUAL(config.Http.Listen, "127.0.0.1:8080");
        UNIT_ASSERT_VALUES_EQUAL(config.Polling.RoundInterval, TDuration::Seconds(15));
        UNIT_ASSERT_VALUES_EQUAL(config.Polling.RoundDeadline, TDuration::Seconds(12));
        UNIT_ASSERT_VALUES_EQUAL(config.Polling.ObservationTtl, TDuration::Seconds(20));
        UNIT_ASSERT_VALUES_EQUAL(config.Polling.GlobalAdminConcurrency, 16);
        UNIT_ASSERT_VALUES_EQUAL(config.Operations.MembershipClientTimeout, TDuration::Seconds(600));
        UNIT_ASSERT(!config.Operations.AttentionAfter.at("rejoin"));
        UNIT_ASSERT(!config.YdbAuth.TokenFile);
        UNIT_ASSERT(!config.YdbAuth.AllowPlaintextToken);
    }

    Y_UNIT_TEST(StrictNestedOverridesAndOptionalNulls) {
        const auto config = ParseConfig(Minimal(
            "controller: {authority_backend: local}\n"
            "evidence: {profile: static_whiteboard_v1}\n"
            "ydb_auth: {ca_file: null, client_cert_file: cert.pem, client_key_file: key.pem, token_file: null}\n"
            "http: {listen: '[::1]:8081'}\n"
            "polling: {sample_nodes_per_pile: 101, per_pile_health_concurrency: 1, global_admin_concurrency: 3}\n"
            "operations:\n  mutation_client_timeout_seconds: 240\n  attention_after_seconds: {rejoin: 1200, failover: null}\n"
            "logging: {level: debug, display_ring_lines: 2000}\n"));
        UNIT_ASSERT(config.EvidenceProfile == EEvidenceProfile::StaticWhiteboardV1);
        UNIT_ASSERT_VALUES_EQUAL(*config.YdbAuth.ClientCertFile, "cert.pem");
        UNIT_ASSERT(!config.YdbAuth.CaFile);
        UNIT_ASSERT_VALUES_EQUAL(config.Polling.SampleNodesPerPile, 101);
        UNIT_ASSERT_VALUES_EQUAL(config.Polling.PerPileHealthConcurrency, 1);
        UNIT_ASSERT_VALUES_EQUAL(*config.Operations.AttentionAfter.at("rejoin"), TDuration::Seconds(1200));
        UNIT_ASSERT(!config.Operations.AttentionAfter.at("failover"));
        UNIT_ASSERT_VALUES_EQUAL(config.Logging.DisplayRingLines, 2000);
    }

    Y_UNIT_TEST(RejectsLegacyHaLabAndHttpAuthenticationKeys) {
        for (const auto extra : {
                "member_id: own-identity\n", "cluster_name: other\n", "seed_endpoints: []\n",
                "default_node_port: 2135\n", "node_endpoint_map: {}\n", "lab: {}\n",
                "implicit_pile_name: ''\n", "candidate_priority: []\n",
                "controller: {group_id: other}\n", "controller: {peers: []}\n",
                "http: {principals: []}\n", "http: {bearer_token: secret}\n", "http: {public_origin: example}\n",
                "polling: {global_health_concurrency: 12}\n", "polling: {grpc_max_receive_message_bytes: 100}\n",
                "polling: {round_interval_ms: 15000}\n", "polling: {confirmation_rounds: 1}\n"}) {
            Reject(Minimal(extra), "unknown key");
        }
        Reject(Minimal("controller: {authority_backend: raft}\n"), "local only");
    }

    Y_UNIT_TEST(RejectsMissingIdentityAndUnsupportedSchema) {
        Reject("schema_version: 1\nydb_endpoints: [grpc://node:2135]\n", "cluster_id is required");
        Reject("schema_version: 1\ncluster_id: ''\nydb_endpoints: [grpc://node:2135]\n", "cluster_id must be nonempty");
        Reject("schema_version: 1\ncluster_id: cluster\n", "ydb_endpoints is required");
        Reject("schema_version: 1\ncluster_id: cluster\nydb_endpoints: []\n", "must not be empty");
        Reject("schema_version: 2\ncluster_id: cluster\nydb_endpoints: [grpc://node:1]\n", "unsupported schema_version");
        Reject("cluster_id: cluster\nydb_endpoints: [grpc://node:1]\n", "schema_version is required");
    }

    Y_UNIT_TEST(RejectsDuplicateKeysAndMalformedShapes) {
        Reject(Minimal("cluster_id: duplicate\n"), "duplicate key");
        Reject(Minimal("polling: {admin_interval_seconds: 1, admin_interval_seconds: 2}\n"), "duplicate key");
        Reject(Minimal("http: null\n"), "must be a mapping");
        Reject(Minimal("polling: []\n"), "must be a mapping");
        Reject(Minimal("evidence: {}\n"), "evidence.profile is required");
        Reject(Minimal("logging: {level: []}\n"), "must be a scalar");
        Reject(Minimal("operations: {attention_after_seconds: {unknown: 1}}\n"), "unknown key");
        Reject(Minimal() + "---\n" + Minimal(), "exactly one YAML document");
        Reject("{bad: [yaml", "invalid YAML");
        Reject("[]", "must be a mapping");
    }

    Y_UNIT_TEST(EndpointParsingPreservesExactUriAndIpv6Brackets) {
        const auto endpoint = ParseEndpoint("grpcs://[2001:db8::1]:2135");
        UNIT_ASSERT_VALUES_EQUAL(endpoint.Uri, "grpcs://[2001:db8::1]:2135");
        UNIT_ASSERT_VALUES_EQUAL(endpoint.Host, "[2001:db8::1]");
        UNIT_ASSERT_VALUES_EQUAL(endpoint.Scheme, "grpcs");
        UNIT_ASSERT_VALUES_EQUAL(endpoint.Port, 2135);
        UNIT_ASSERT_VALUES_EQUAL(ParseEndpoint("grpc://node.example:1").Port, 1);
        UNIT_ASSERT_VALUES_EQUAL(ParseEndpoint("grpc://127.0.0.1:65535").Port, 65535);
    }

    Y_UNIT_TEST(RejectsAmbiguousOrUnsafeEndpoints) {
        for (const auto uri : {"node:2135", "http://node:2135", "grpc://node", "grpc://:2135", "grpc://node:",
                "grpc://node:0", "grpc://node:65536", "grpc://node:+1", "grpc://node:1.5", "grpc://user@node:2135",
                "grpc://node:2135/", "grpc://node:2135?x=1", "grpc://node:2135#fragment", "grpc://node:2135\\path",
                "grpc://2001:db8::1:2135", "grpc://[bad-ip]:2135", "grpc://[::1]", "grpc://node name:2135",
                "grpc://node%20name:2135", "grpc://[::1]:1:2"}) {
            UNIT_ASSERT_EXCEPTION(ParseEndpoint(uri), yexception);
        }
        Reject("schema_version: 1\ncluster_id: cluster\nydb_endpoints: [grpc://node:1, grpcs://node:2]\n", "mixed grpc/grpcs");
    }

    Y_UNIT_TEST(PlaintextTokenRequiresExplicitBooleanOptIn) {
        const TString base = "schema_version: 1\ncluster_id: cluster\nydb_endpoints: [grpc://node:1]\n";
        Reject(base + "ydb_auth: {token_file: token}\n", "plaintext token requires explicit");
        Reject(base + "ydb_auth: {token_file: token, allow_plaintext_token: false}\n", "plaintext token requires explicit");
        Reject(base + "ydb_auth: {token_file: token, allow_plaintext_token: yes}\n", "must be true or false");
        const auto config = ParseConfig(base + "ydb_auth: {token_file: token, allow_plaintext_token: true}\n");
        UNIT_ASSERT(config.YdbAuth.AllowPlaintextToken);
        UNIT_ASSERT_NO_EXCEPTION(ParseConfig(Minimal("ydb_auth: {token_file: token}\n")));
    }

    Y_UNIT_TEST(CertificateAndKeyMustBePaired) {
        Reject(Minimal("ydb_auth: {client_cert_file: cert}\n"), "must be supplied together");
        Reject(Minimal("ydb_auth: {client_key_file: key}\n"), "must be supplied together");
        Reject(Minimal("ydb_auth: {ca_file: ''}\n"), "nonempty file path");
    }

    Y_UNIT_TEST(EveryTimingConstraintHasBoundaryCoverage) {
        Reject(Minimal("polling: {round_deadline_seconds: 11}\n"), "fit strictly inside");
        Reject(Minimal("polling: {round_interval_seconds: 11}\n"), "must not exceed");
        Reject(Minimal("polling: {health_operation_timeout_seconds: 9}\n"), "operation < client");
        Reject(Minimal("polling: {observation_ttl_seconds: 18}\n"), "must cover round deadline");
        Reject(Minimal("polling: {confirmation_start_gap_seconds: 19}\n"), "must cover round interval");
        UNIT_ASSERT_NO_EXCEPTION(ParseConfig(Minimal("polling: {round_interval_seconds: 12, observation_ttl_seconds: 19, confirmation_start_gap_seconds: 17}\n")));
        UNIT_ASSERT_NO_EXCEPTION(ParseConfig(Minimal("polling: {round_deadline_seconds: 13}\n")));
    }

    Y_UNIT_TEST(RejectsInvalidNumericDurationsAndLimits) {
        for (const auto value : {"0", "-1", "1.5", "null", "true", "+1", "18446744073709551616", "18446744073709551615"}) {
            UNIT_ASSERT_EXCEPTION(ParseConfig(Minimal(TStringBuilder() << "polling: {admin_interval_seconds: " << value << "}\n")), yexception);
        }
        Reject(Minimal("polling: {global_admin_concurrency: 2}\n"), "at least 3");
        Reject(Minimal("polling: {sample_nodes_per_pile: 102}\n"), "1..101");
        Reject(Minimal("polling: {sample_nodes_per_pile: 0}\n"), "positive decimal integer");
        Reject(Minimal("polling: {per_pile_health_concurrency: 0}\n"), "positive decimal integer");
        Reject(Minimal("polling: {global_admin_concurrency: 4294967296}\n"), "too large");
        Reject(Minimal("operations: {attention_after_seconds: {rejoin: 0}}\n"), "positive decimal integer");
        Reject(Minimal("logging: {display_ring_lines: 0}\n"), "positive decimal integer");
        Reject(Minimal("logging: {level: unknown}\n"), "unsupported logging.level");
    }

    Y_UNIT_TEST(DirectTypedConfigCannotBypassValidation) {
        auto config = ParseConfig(Minimal());
        config.Polling.AdminInterval = TDuration::MilliSeconds(500);
        UNIT_ASSERT_EXCEPTION(ValidateConfig(config), yexception);
        config = ParseConfig(Minimal());
        config.YdbEndpoints.front().Host = "different-node";
        UNIT_ASSERT_EXCEPTION(ValidateConfig(config), yexception);
        config = ParseConfig(Minimal());
        config.Operations.AttentionAfter.clear();
        UNIT_ASSERT_EXCEPTION(ValidateConfig(config), yexception);
    }

    Y_UNIT_TEST(ProfilesAreRecognizedWithoutCertifyingProductionAuto) {
        for (const auto name : {"global_bsc_v1", "static_whiteboard_v1", "nodewarden_local_v1"}) {
            const auto config = ParseConfig(Minimal(TStringBuilder() << "evidence: {profile: " << name << "}\n"));
            UNIT_ASSERT_VALUES_EQUAL(EvidenceProfileName(config.EvidenceProfile), name);
            UNIT_ASSERT(!IsProductionAutoQualified(config.EvidenceProfile));
        }
        Reject(Minimal("evidence: {profile: convenient_fallback}\n"), "unknown evidence.profile");
    }

    Y_UNIT_TEST(HttpBindSyntaxAndAccessClassification) {
        for (const auto listen : {"127.0.0.1:8080", "127.9.8.7:1", "[::1]:8080", "[0:0:0:0:0:0:0:1]:8080", "localhost:8080"}) {
            UNIT_ASSERT(IsLoopbackHttpListen(listen));
        }
        for (const auto listen : {"0.0.0.0:8080", "[::]:8080", "192.0.2.1:8080", "host.example:8080"}) {
            UNIT_ASSERT(!IsLoopbackHttpListen(listen));
            UNIT_ASSERT_NO_EXCEPTION(ParseConfig(Minimal(TStringBuilder() << "http: {listen: '" << listen << "'}\n")));
        }
        for (const auto listen : {"::1:8080", "127.0.0.1", "127.0.0.1:0", "http://localhost:8080", "[bad]:8080"}) {
            UNIT_ASSERT_EXCEPTION(IsLoopbackHttpListen(listen), yexception);
        }
    }

    Y_UNIT_TEST(ConfigInputBoundAndFileLoader) {
        UNIT_ASSERT_EXCEPTION_CONTAINS(ParseConfig(TString(1024 * 1024 + 1, 'x')), yexception, "size limit");
        TTempFileHandle file;
        TFileOutput(file.Name()).Write(Minimal());
        UNIT_ASSERT_VALUES_EQUAL(LoadConfig(file.Name()).ClusterId, "test-bridge");
    }

    Y_UNIT_TEST(LoadsCredentialsOnceAndRedactsReadErrors) {
        TTempFileHandle token;
        TFileOutput(token.Name()).Write("secret-token\r\n");
        auto config = ParseConfig(Minimal());
        config.YdbAuth.TokenFile = token.Name();
        const auto loaded = LoadCredentials(config);
        UNIT_ASSERT_VALUES_EQUAL(*loaded.Token, "secret-token");
        TFileOutput(token.Name()).Write("replacement");
        UNIT_ASSERT_VALUES_EQUAL(*loaded.Token, "secret-token");
        config.YdbAuth.TokenFile = "/does/not/exist/secret-token-file";
        try {
            LoadCredentials(config);
            UNIT_FAIL("missing credential file was accepted");
        } catch (const yexception& e) {
            const TStringBuf message(e.what());
            UNIT_ASSERT(message.Contains("cannot load token credential file"));
            UNIT_ASSERT(!message.Contains("/does/not/exist"));
            UNIT_ASSERT(!message.Contains("secret-token-file"));
        }
    }

    Y_UNIT_TEST(RejectsEmptyMultilineAndOversizedCredentials) {
        TTempFileHandle file;
        auto config = ParseConfig(Minimal());
        config.YdbAuth.TokenFile = file.Name();
        UNIT_ASSERT_EXCEPTION(LoadCredentials(config), yexception);
        TFileOutput(file.Name()).Write("one\ntwo\n");
        UNIT_ASSERT_EXCEPTION_CONTAINS(LoadCredentials(config), yexception, "one nonempty line");
        TFileOutput(file.Name()).Write(TString(16 * 1024 * 1024 + 1, 'x'));
        UNIT_ASSERT_EXCEPTION_CONTAINS(LoadCredentials(config), yexception, "cannot load token credential file");
    }

    Y_UNIT_TEST(PlaintextOptInAppearsInStartupResult) {
        auto config = ParseConfig("schema_version: 1\ncluster_id: c\nydb_endpoints: [grpc://node:2135]\nydb_auth: {allow_plaintext_token: true}\n");
        UNIT_ASSERT(LoadCredentials(config).PlaintextTokenOptIn);
    }

    Y_UNIT_TEST(LoadsEveryCredentialWithoutRetainingPaths) {
        TTempFileHandle ca;
        TTempFileHandle certificate;
        TTempFileHandle key;
        TFileOutput(ca.Name()).Write("ca-material");
        TFileOutput(certificate.Name()).Write("certificate-material");
        TFileOutput(key.Name()).Write("key-material");
        auto config = ParseConfig(Minimal());
        config.YdbAuth.CaFile = ca.Name();
        config.YdbAuth.ClientCertFile = certificate.Name();
        config.YdbAuth.ClientKeyFile = key.Name();
        const auto credentials = LoadCredentials(config);
        UNIT_ASSERT_VALUES_EQUAL(*credentials.Ca, "ca-material");
        UNIT_ASSERT_VALUES_EQUAL(*credentials.ClientCert, "certificate-material");
        UNIT_ASSERT_VALUES_EQUAL(*credentials.ClientKey, "key-material");
        UNIT_ASSERT(!credentials.Token);
        UNIT_ASSERT(!credentials.PlaintextTokenOptIn);
    }

    Y_UNIT_TEST(ExplicitEntrypointArgumentsAndCheckConfig) {
        const auto args = ParseEntrypointArguments({"--config", "config.yaml", "--state-dir=/state", "--member-id", "member-a", "--check-config"});
        UNIT_ASSERT(args.Action == EEntrypointAction::CheckConfig);
        UNIT_ASSERT_VALUES_EQUAL(args.ConfigFile, "config.yaml");
        UNIT_ASSERT_VALUES_EQUAL(args.StateDir, "/state");
        UNIT_ASSERT_VALUES_EQUAL(args.MemberId, "member-a");
        UNIT_ASSERT_NO_EXCEPTION(ValidateStartup(ParseConfig(Minimal()), args));
        UNIT_ASSERT(ParseEntrypointArguments({"--help"}).Action == EEntrypointAction::Help);
        UNIT_ASSERT(ParseEntrypointArguments({"--version"}).Action == EEntrypointAction::Version);
        UNIT_ASSERT(EntrypointHelp().Contains("every route and mutation"));
    }

    Y_UNIT_TEST(RejectsUnknownDuplicateAndIncompleteArguments) {
        for (const TVector<TString>& args : {
                TVector<TString>{}, {"--check-config"}, {"--config", "c", "--state-dir", "s"},
                {"--config", "c", "--member-id", "m"}, {"--token", "secret"},
                {"--config", "--member-id", "m"}, {"--config="}, {"--help=true"},
                {"--help", "--version"}, {"--help", "--help"}, {"--member-id", "a", "--member-id=b"}}) {
            UNIT_ASSERT_EXCEPTION(ParseEntrypointArguments(args), yexception);
        }
    }
}

} // namespace NBridgeSkipper
