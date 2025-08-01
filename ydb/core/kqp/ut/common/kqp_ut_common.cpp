#include "kqp_ut_common.h"

#include <ydb/core/base/backtrace.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/provider/yql_kikimr_results.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/proto/accessor.h>

#include <yql/essentials/core/yql_data_provider.h>
#include <yql/essentials/utils/backtrace/backtrace.h>
#include <yql/essentials/public/udf/udf_helpers.h>
#include <yql/essentials/public/udf/udf_value_builder.h>
#include <yql/essentials/minikql/invoke_builtins/mkql_builtins.h>
#include <yql/essentials/utils/yql_panic.h>

#include <library/cpp/testing/common/env.h>

namespace NKikimr {
namespace NKqp {

using namespace NYdb::NTable;

const TString EXPECTED_EIGHTSHARD_VALUE1 = R"(
[
    [[1];[101u];["Value1"]];
    [[2];[201u];["Value1"]];
    [[3];[301u];["Value1"]];
    [[1];[401u];["Value1"]];
    [[2];[501u];["Value1"]];
    [[3];[601u];["Value1"]];
    [[1];[701u];["Value1"]];
    [[2];[801u];["Value1"]]
])";

SIMPLE_UDF(TTestFilter, bool(i64)) {
    Y_UNUSED(valueBuilder);
    const i64 arg = args[0].Get<i64>();

    return NUdf::TUnboxedValuePod(arg >= 0);
}

SIMPLE_UDF(TTestFilterTerminate, bool(i64, i64)) {
    Y_UNUSED(valueBuilder);
    const i64 arg1 = args[0].Get<i64>();
    const i64 arg2 = args[1].Get<i64>();

    if (arg1 < arg2) {
        UdfTerminate("Bad filter value.");
    }

    return NUdf::TUnboxedValuePod(true);
}

SIMPLE_UDF(TRandString, char*(ui32)) {
    Y_UNUSED(valueBuilder);
    const ui32 size = args[0].Get<ui32>();

    auto str = valueBuilder->NewStringNotFilled(size);
    auto strRef = str.AsStringRef();

    for (ui32 i = 0; i < size; ++i) {
        *(strRef.Data() + i) = '0' + RandomNumber<ui32>() % 10;
    }

    return str;
}

SIMPLE_MODULE(TTestUdfsModule, TTestFilter, TTestFilterTerminate, TRandString);
NYql::NUdf::TUniquePtr<NYql::NUdf::IUdfModule> CreateJson2Module();
NYql::NUdf::TUniquePtr<NYql::NUdf::IUdfModule> CreateRe2Module();
NYql::NUdf::TUniquePtr<NYql::NUdf::IUdfModule> CreateStringModule();
NYql::NUdf::TUniquePtr<NYql::NUdf::IUdfModule> CreateDateTime2Module();
NYql::NUdf::TUniquePtr<NYql::NUdf::IUdfModule> CreateMathModule();
NYql::NUdf::TUniquePtr<NYql::NUdf::IUdfModule> CreateUnicodeModule();
NYql::NUdf::TUniquePtr<NYql::NUdf::IUdfModule> CreateDigestModule();

NMiniKQL::IFunctionRegistry* UdfFrFactory(const NScheme::TTypeRegistry& typeRegistry) {
    Y_UNUSED(typeRegistry);
    auto funcRegistry = NMiniKQL::CreateFunctionRegistry(NMiniKQL::CreateBuiltinRegistry())->Clone();
    funcRegistry->AddModule("", "TestUdfs", new TTestUdfsModule());
    funcRegistry->AddModule("", "Json2", CreateJson2Module());
    funcRegistry->AddModule("", "Re2", CreateRe2Module());
    funcRegistry->AddModule("", "String", CreateStringModule());
    funcRegistry->AddModule("", "DateTime", CreateDateTime2Module());
    funcRegistry->AddModule("", "Math", CreateMathModule());
    funcRegistry->AddModule("", "Unicode", CreateUnicodeModule());
    funcRegistry->AddModule("", "Digest", CreateDigestModule());

    NKikimr::NMiniKQL::FillStaticModules(*funcRegistry);
    return funcRegistry.Release();
}

TVector<NKikimrKqp::TKqpSetting> SyntaxV1Settings() {
    auto setting = NKikimrKqp::TKqpSetting();
    setting.SetName("_KqpYqlSyntaxVersion");
    setting.SetValue("1");
    return {setting};
}

TKikimrRunner::TKikimrRunner(const TKikimrSettings& settings) {
    EnableYDBBacktraceFormat();

    auto mbusPort = PortManager.GetPort();
    auto grpcPort = PortManager.GetPort();

    Cerr << "Trying to start YDB, gRPC: " << grpcPort << ", MsgBus: " << mbusPort << Endl;

    TVector<NKikimrKqp::TKqpSetting> effectiveKqpSettings;

    bool enableSpilling = false;
    if (settings.AppConfig.GetTableServiceConfig().GetSpillingServiceConfig().GetLocalFileConfig().GetEnable()) {
        NKikimrKqp::TKqpSetting setting;
        setting.SetName("_KqpEnableSpilling");
        setting.SetValue("true");
        effectiveKqpSettings.push_back(setting);
        enableSpilling = true;
    }

    effectiveKqpSettings.insert(effectiveKqpSettings.end(), settings.KqpSettings.begin(), settings.KqpSettings.end());

    NKikimrProto::TAuthConfig authConfig;
    authConfig.SetUseBuiltinDomain(true);
    ServerSettings.Reset(MakeHolder<Tests::TServerSettings>(mbusPort, authConfig, settings.PQConfig));
    ServerSettings->SetDomainName(settings.DomainRoot);
    ServerSettings->SetKqpSettings(effectiveKqpSettings);

    NKikimrConfig::TAppConfig appConfig = settings.AppConfig;
    appConfig.MutableColumnShardConfig()->SetDisabledOnSchemeShard(false);
    appConfig.MutableTableServiceConfig()->SetEnableRowsDuplicationCheck(true);
    ServerSettings->SetAppConfig(appConfig);
    ServerSettings->SetFeatureFlags(settings.FeatureFlags);
    ServerSettings->SetNodeCount(settings.NodeCount);
    ServerSettings->SetEnableKqpSpilling(enableSpilling);
    ServerSettings->SetEnableDataColumnForIndexTable(true);
    ServerSettings->SetKeepSnapshotTimeout(settings.KeepSnapshotTimeout);
    ServerSettings->SetFrFactory(&UdfFrFactory);
    ServerSettings->SetEnableNotNullColumns(true);
    ServerSettings->SetEnableMoveIndex(true);
    ServerSettings->SetUseRealThreads(settings.UseRealThreads);
    ServerSettings->SetEnableTablePgTypes(true);
    ServerSettings->SetEnablePgSyntax(true);
    ServerSettings->S3ActorsFactory = settings.S3ActorsFactory;
    ServerSettings->Controls = settings.Controls;
    ServerSettings->SetEnableForceFollowers(settings.EnableForceFollowers);

    if (!settings.FeatureFlags.HasEnableOlapCompression()) {
        ServerSettings->SetEnableOlapCompression(true);
    }

    if (settings.Storage) {
        ServerSettings->SetCustomDiskParams(*settings.Storage);
        ServerSettings->SetEnableMockOnSingleNode(false);
    }

    if (settings.LogStream)
        ServerSettings->SetLogBackend(new TStreamLogBackend(settings.LogStream));

    if (settings.FederatedQuerySetupFactory) {
        ServerSettings->SetFederatedQuerySetupFactory(settings.FederatedQuerySetupFactory);
    }

    Server.Reset(MakeHolder<Tests::TServer>(*ServerSettings));

    if (settings.GrpcServerOptions) {
        auto options = settings.GrpcServerOptions;
        options->SetPort(grpcPort);
        options->SetHost("localhost");
        Server->EnableGRpc(*options);
    } else {
        Server->EnableGRpc(grpcPort);
    }

    RunCall([this, domain = settings.DomainRoot] {
        this->Server->SetupDefaultProfiles();
        return true;
    });

    Client.Reset(MakeHolder<Tests::TClient>(*ServerSettings));

    Endpoint = "localhost:" + ToString(grpcPort);

    DriverConfig = NYdb::TDriverConfig()
        .SetEndpoint(Endpoint)
        .SetDatabase("/" + settings.DomainRoot)
        .SetDiscoveryMode(NYdb::EDiscoveryMode::Async)
        .SetAuthToken(settings.AuthToken);
    Driver.Reset(MakeHolder<NYdb::TDriver>(DriverConfig));

    CountersRoot = settings.CountersRoot;

    Initialize(settings);
}

TKikimrRunner::TKikimrRunner(const TVector<NKikimrKqp::TKqpSetting>& kqpSettings, const TString& authToken,
    const TString& domainRoot, ui32 nodeCount)
    : TKikimrRunner(TKikimrSettings()
        .SetKqpSettings(kqpSettings)
        .SetAuthToken(authToken)
        .SetDomainRoot(domainRoot)
        .SetNodeCount(nodeCount)) {}

TKikimrRunner::TKikimrRunner(const NKikimrConfig::TAppConfig& appConfig, const TString& authToken,
    const TString& domainRoot, ui32 nodeCount)
    : TKikimrRunner(TKikimrSettings(appConfig)
        .SetAuthToken(authToken)
        .SetDomainRoot(domainRoot)
        .SetNodeCount(nodeCount)) {}

TKikimrRunner::TKikimrRunner(const NKikimrConfig::TAppConfig& appConfig,
    const TVector<NKikimrKqp::TKqpSetting>& kqpSettings, const TString& authToken, const TString& domainRoot,
    ui32 nodeCount)
    : TKikimrRunner(TKikimrSettings(appConfig)
        .SetKqpSettings(kqpSettings)
        .SetAuthToken(authToken)
        .SetDomainRoot(domainRoot)
        .SetNodeCount(nodeCount)) {}

TKikimrRunner::TKikimrRunner(const NKikimrConfig::TFeatureFlags& featureFlags, const TString& authToken,
    const TString& domainRoot, ui32 nodeCount)
    : TKikimrRunner(TKikimrSettings()
        .SetFeatureFlags(featureFlags)
        .SetAuthToken(authToken)
        .SetDomainRoot(domainRoot)
        .SetNodeCount(nodeCount)) {}

TKikimrRunner::TKikimrRunner(const TString& authToken, const TString& domainRoot, ui32 nodeCount)
    : TKikimrRunner(TKikimrSettings()
        .SetAuthToken(authToken)
        .SetDomainRoot(domainRoot)
        .SetNodeCount(nodeCount)) {}

TKikimrRunner::TKikimrRunner(const NFake::TStorage& storage)
    : TKikimrRunner(TKikimrSettings()
        .SetStorage(storage)) {}

void TKikimrRunner::CreateSampleTables() {
    Client->CreateTable("/Root", R"(
        Name: "TwoShard"
        Columns { Name: "Key", Type: "Uint32" }
        Columns { Name: "Value1", Type: "String" }
        Columns { Name: "Value2", Type: "Int32" }
        KeyColumnNames: ["Key"]
        UniformPartitionsCount: 2
    )");

    Client->CreateTable("/Root", R"(
        Name: "EightShard"
        Columns { Name: "Key", Type: "Uint64" }
        Columns { Name: "Text", Type: "String" }
        Columns { Name: "Data", Type: "Int32" }
        KeyColumnNames: ["Key"],
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 100 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 200 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 300 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 400 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 500 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 600 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 700 } } } }
    )");

    Client->CreateTable("/Root", R"(
        Name: "Logs"
        Columns { Name: "App", Type: "Utf8" }
        Columns { Name: "Message", Type: "Utf8" }
        Columns { Name: "Ts", Type: "Int64" }
        Columns { Name: "Host", Type: "Utf8" }
        KeyColumnNames: ["App", "Ts", "Host"],
        SplitBoundary { KeyPrefix { Tuple { Optional { Text: "a" } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Text: "b" } } } }
    )");

    Client->CreateTable("/Root", R"(
        Name: "BatchUpload"
        Columns {
            Name: "Key1"
            Type: "Uint32"
        }
        Columns {
            Name: "Key2"
            Type: "String"
        }
        Columns {
            Name: "Value1"
            Type: "Int64"
        }
        Columns {
            Name: "Value2"
            Type: "Double"
        }
        Columns {
            Name: "Blob1"
            Type: "String"
        }
        Columns {
            Name: "Blob2"
            Type: "String"
        }
        KeyColumnNames: ["Key1", "Key2"]
        UniformPartitionsCount: 10
    )");

    // TODO: Reuse driver (YDB-626)
    NYdb::TDriver driver(NYdb::TDriverConfig().SetEndpoint(Endpoint).SetDatabase("/Root"));
    NYdb::NTable::TTableClient client(driver);
    auto session = client.CreateSession().GetValueSync().GetSession();

    AssertSuccessResult(session.ExecuteSchemeQuery(R"(
        --!syntax_v1

        CREATE TABLE `KeyValue` (
            Key Uint64,
            Value String,
            PRIMARY KEY (Key)
        );

        CREATE TABLE `KeyValue2` (
            Key String,
            Value String,
            PRIMARY KEY (Key)
        );

        CREATE TABLE `KeyValueLargePartition` (
            Key Uint64,
            Value String,
            PRIMARY KEY (Key)
        );

        CREATE TABLE `Test` (
            Group Uint32,
            Name String,
            Amount Uint64,
            Comment String,
            PRIMARY KEY (Group, Name)
        );

        CREATE TABLE `Join1` (
            Key Int32,
            Fk21 Uint32,
            Fk22 String,
            Value String,
            PRIMARY KEY (Key)
        )
        WITH (
            PARTITION_AT_KEYS = (5)
        );

        CREATE TABLE `Join2` (
            Key1 Uint32,
            Key2 String,
            Name String,
            Value2 String,
            PRIMARY KEY (Key1, Key2)
        )
        WITH (
            PARTITION_AT_KEYS = (105)
        );

        CREATE TABLE `ReorderKey` (
            Col1 Uint32,
            Col2 Uint64,
            Col3 Int64,
            Col4 Int64,
            PRIMARY KEY (Col2, Col1, Col3)
        )
        WITH (
            AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 2,
            PARTITION_AT_KEYS = (2, 3)
        );

        CREATE TABLE `ReorderOptionalKey` (
            k1 Uint64 NOT NULL,
            k2 Uint64,
            v1 Uint64,
            v2 String,
            id Int64,
            PRIMARY KEY (k2, k1)
        )
        WITH (
            AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 3,
            PARTITION_AT_KEYS = ((2, 2), (4), (5, 6), (8))
        );
    )").GetValueSync());

    AssertSuccessResult(session.ExecuteDataQuery(R"(

        REPLACE INTO `KeyValueLargePartition` (Key, Value) VALUES
            (101u, "Value1"),
            (102u, "Value2"),
            (103u, "Value3"),
            (201u, "Value1"),
            (202u, "Value2"),
            (203u, "Value3"),
            (301u, "Value1"),
            (302u, "Value2"),
            (303u, "Value3"),
            (401u, "Value1"),
            (402u, "Value2"),
            (403u, "Value3"),
            (501u, "Value1"),
            (502u, "Value2"),
            (503u, "Value3"),
            (601u, "Value1"),
            (602u, "Value2"),
            (603u, "Value3"),
            (701u, "Value1"),
            (702u, "Value2"),
            (703u, "Value3"),
            (801u, "Value1"),
            (802u, "Value2"),
            (803u, "Value3");

        REPLACE INTO `TwoShard` (Key, Value1, Value2) VALUES
            (1u, "One", -1),
            (2u, "Two", 0),
            (3u, "Three", 1),
            (4000000001u, "BigOne", -1),
            (4000000002u, "BigTwo", 0),
            (4000000003u, "BigThree", 1);

        REPLACE INTO `EightShard` (Key, Text, Data) VALUES
            (101u, "Value1",  1),
            (201u, "Value1",  2),
            (301u, "Value1",  3),
            (401u, "Value1",  1),
            (501u, "Value1",  2),
            (601u, "Value1",  3),
            (701u, "Value1",  1),
            (801u, "Value1",  2),
            (102u, "Value2",  3),
            (202u, "Value2",  1),
            (302u, "Value2",  2),
            (402u, "Value2",  3),
            (502u, "Value2",  1),
            (602u, "Value2",  2),
            (702u, "Value2",  3),
            (802u, "Value2",  1),
            (103u, "Value3",  2),
            (203u, "Value3",  3),
            (303u, "Value3",  1),
            (403u, "Value3",  2),
            (503u, "Value3",  3),
            (603u, "Value3",  1),
            (703u, "Value3",  2),
            (803u, "Value3",  3);

        REPLACE INTO `KeyValue` (Key, Value) VALUES
            (1u, "One"),
            (2u, "Two");

        REPLACE INTO `KeyValue2` (Key, Value) VALUES
            ("1", "One"),
            ("2", "Two");

        REPLACE INTO `Test` (Group, Name, Amount, Comment) VALUES
            (1u, "Anna", 3500ul, "None"),
            (1u, "Paul", 300ul, "None"),
            (2u, "Tony", 7200ul, "None");

        REPLACE INTO `Logs` (App, Ts, Host, Message) VALUES
            ("apache", 0, "front-42", " GET /index.html HTTP/1.1"),
            ("nginx", 1, "nginx-10", "GET /index.html HTTP/1.1"),
            ("nginx", 2, "nginx-23", "PUT /form HTTP/1.1"),
            ("nginx", 3, "nginx-23", "GET /cat.jpg HTTP/1.1"),
            ("kikimr-db", 1, "kikimr-db-10", "Write Data"),
            ("kikimr-db", 2, "kikimr-db-21", "Read Data"),
            ("kikimr-db", 3, "kikimr-db-21", "Stream Read Data"),
            ("kikimr-db", 4, "kikimr-db-53", "Discover"),
            ("ydb", 0, "ydb-1000", "some very very very very long string");

        REPLACE INTO `Join1` (Key, Fk21, Fk22, Value) VALUES
            (1, 101, "One", "Value1"),
            (2, 102, "Two", "Value1"),
            (3, 103, "One", "Value2"),
            (4, 104, "Two", "Value2"),
            (5, 105, "One", "Value3"),
            (6, 106, "Two", "Value3"),
            (7, 107, "One", "Value4"),
            (8, 108, "One", "Value5"),
            (9, 101, "Two", "Value1");

        REPLACE INTO `Join2` (Key1, Key2, Name, Value2) VALUES
            (101, "One",   "Name1", "Value21"),
            (101, "Two",   "Name1", "Value22"),
            (101, "Three", "Name3", "Value23"),
            (102, "One",   "Name2", "Value24"),
            (103, "One",   "Name1", "Value25"),
            (104, "One",   "Name3", "Value26"),
            (105, "One",   "Name2", "Value27"),
            (105, "Two",   "Name4", "Value28"),
            (106, "One",   "Name3", "Value29"),
            (108, "One",    NULL,   "Value31");

        REPLACE INTO `ReorderKey` (Col1, Col2, Col3, Col4) VALUES
            (0, 1, 0, 3),
            (1, 1, 0, 1),
            (1, 1, 1, 0),
            (1, 1, 2, 1),
            (2, 1, 0, 2),
            (1, 2, 0, 1),
            (1, 2, 1, 0),
            (2, 2, 0, 1),
            (3, 2, 1, 5),
            (0, 3, 0, 1),
            (1, 3, 3, 0),
            (2, 3, 0, 1),
            (0, 3, 2, 4),
            (1, 3, 1, 1),
            (2, 3, 1, 2),
            (3, 3, 0, 1);

        REPLACE INTO `ReorderOptionalKey` (k1, k2, v1, v2, id) VALUES
            (0, NULL, 0, "0NULL", 0),
            (1, NULL, 1, "1NULL", 2),
            (1, 0, 1, "10", 0),
            (1, 1, 2, "11", 1),
            (1, 2, 3, "12", 2),
            (1, 3, 4, "13", 0),
            (1, 4, 5, "14", 1),
            (2, NULL, 2, "2NULL", 1),
            (2, 0, 2, "20", 2),
            (2, 1, 3, "21", 0),
            (2, 2, 4, "22", 1),
            (2, 3, 5, "23", 2),
            (2, 4, 6, "24", 0),
            (3, NULL, 3, "3NULL", 0),
            (3, 0, 3, "30", 1),
            (3, 1, 4, "31", 2),
            (3, 2, 5, "32", 0),
            (3, 3, 6, "33", 1),
            (3, 4, 7, "34", 2),
            (4, NULL, 4, "4NULL", 2),
            (4, 0, 4, "40", 0),
            (4, 1, 5, "41", 1),
            (4, 2, 6, "42", 2),
            (4, 3, 7, "43", 0),
            (4, 4, 8, "44", 1),
            (5, NULL, 5, "5NULL", 1),
            (5, 0, 5, "50", 2),
            (5, 1, 6, "51", 0),
            (5, 2, 7, "52", 1),
            (5, 3, 8, "53", 2),
            (5, 4, 9, "54", 0),
            (6, NULL, 6, "6NULL", 0),
            (6, 0, 6, "60", 1),
            (6, 1, 7, "61", 2),
            (6, 2, 8, "62", 0),
            (6, 3, 9, "63", 1),
            (6, 4, 10, "64", 2),
            (6, 5, 11, "65", 0),
            (7, NULL, 7, "7NULL", 2),
            (7, 0, 7, "70", 0),
            (7, 1, 8, "71", 1),
            (7, 2, 9, "72", 2),
            (7, 3, 10, "73", 0),
            (7, 4, 11, "74", 1),
            (8, NULL, 8, "8NULL", 1),
            (8, 0, 8, "80", 2),
            (8, 1, 9, "81", 0),
            (8, 2, 10, "82", 1),
            (8, 3, 11, "83", 2),
            (8, 4, 12, "84", 0),
            (9, NULL, 9, "9NULL", 0),
            (9, 0, 9, "90", 1),
            (9, 1, 10, "91", 2),
            (9, 2, 11, "92", 0),
            (9, 3, 12, "93", 1),
            (9, 4, 13, "94", 2);
    )", TTxControl::BeginTx(TTxSettings::SerializableRW()).CommitTx()).GetValueSync());

}

static TMaybe<NActors::NLog::EPriority> ParseLogLevel(const TString& level) {
    static const THashMap<TString, NActors::NLog::EPriority> levels = {
        { "TRACE", NActors::NLog::PRI_TRACE },
        { "DEBUG", NActors::NLog::PRI_DEBUG },
        { "INFO", NActors::NLog::PRI_INFO },
        { "NOTICE", NActors::NLog::PRI_NOTICE },
        { "WARN", NActors::NLog::PRI_WARN },
        { "ERROR", NActors::NLog::PRI_ERROR },
        { "CRIT", NActors::NLog::PRI_CRIT },
        { "ALERT", NActors::NLog::PRI_ALERT },
        { "EMERG", NActors::NLog::PRI_EMERG },
    };

    TString l = level;
    l.to_upper();
    const auto levelIt = levels.find(l);
    if (levelIt != levels.end()) {
        return levelIt->second;
    } else {
        Cerr << "Failed to parse test log level [" << level << "]" << Endl;
        return Nothing();
    }
}

void TKikimrRunner::SetupLogLevelFromTestParam(NKikimrServices::EServiceKikimr service) {
    if (const TString paramForService = GetTestParam(TStringBuilder() << "KQP_LOG_" << NKikimrServices::EServiceKikimr_Name(service))) {
        if (const TMaybe<NActors::NLog::EPriority> level = ParseLogLevel(paramForService)) {
            Server->GetRuntime()->SetLogPriority(service, *level);
            return;
        }
    }
    if (const TString commonParam = GetTestParam("KQP_LOG")) {
        if (const TMaybe<NActors::NLog::EPriority> level = ParseLogLevel(commonParam)) {
            Server->GetRuntime()->SetLogPriority(service, *level);
        }
    }
}

void TKikimrRunner::Initialize(const TKikimrSettings& settings) {
    // You can enable logging for these services in test using test option:
    // `--test-param KQP_LOG=<level>`
    // or `--test-param KQP_LOG_<service>=<level>`
    // For example:
    // --test-param KQP_LOG=TRACE
    // --test-param KQP_LOG_FLAT_TX_SCHEMESHARD=debug
    SetupLogLevelFromTestParam(NKikimrServices::FLAT_TX_SCHEMESHARD);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_YQL);
    SetupLogLevelFromTestParam(NKikimrServices::TX_DATASHARD);
    SetupLogLevelFromTestParam(NKikimrServices::TX_COORDINATOR);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_COMPUTE);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_TASKS_RUNNER);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_EXECUTER);
    SetupLogLevelFromTestParam(NKikimrServices::TX_PROXY_SCHEME_CACHE);
    SetupLogLevelFromTestParam(NKikimrServices::TX_PROXY);
    SetupLogLevelFromTestParam(NKikimrServices::SCHEME_BOARD_REPLICA);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_WORKER);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_SESSION);
    SetupLogLevelFromTestParam(NKikimrServices::TABLET_EXECUTOR);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_SLOW_LOG);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_PROXY);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_COMPILE_SERVICE);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_COMPILE_ACTOR);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_COMPILE_REQUEST);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_GATEWAY);
    SetupLogLevelFromTestParam(NKikimrServices::RPC_REQUEST);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_RESOURCE_MANAGER);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_NODE);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_BLOBS_STORAGE);
    SetupLogLevelFromTestParam(NKikimrServices::KQP_WORKLOAD_SERVICE);
    SetupLogLevelFromTestParam(NKikimrServices::TX_COLUMNSHARD);
    SetupLogLevelFromTestParam(NKikimrServices::TX_COLUMNSHARD_SCAN);
    SetupLogLevelFromTestParam(NKikimrServices::LOCAL_PGWIRE);
    SetupLogLevelFromTestParam(NKikimrServices::SSA_GRAPH_EXECUTION);


    RunCall([this, domain = settings.DomainRoot]{
        this->Client->InitRootScheme(domain);
        return true;
    });

    if (settings.AuthToken) {
        this->Client->GrantConnect(settings.AuthToken);
    }

    if (settings.WithSampleTables) {
        RunCall([this] {
            this->CreateSampleTables();
            return true;
        });
    }
}

TString ReformatYson(const TString& yson) {
    TStringStream ysonInput(yson);
    TStringStream output;
    NYson::ReformatYsonStream(&ysonInput, &output, NYson::EYsonFormat::Text);
    return output.Str();
}

void CompareYson(const TString& expected, const TString& actual, const TString& message) {
    UNIT_ASSERT_VALUES_EQUAL_C(ReformatYson(expected), ReformatYson(actual), message);
}

void CompareYson(const TString& expected, const NKikimrMiniKQL::TResult& actual, const TString& message) {
    TStringStream ysonStream;
    NYson::TYsonWriter writer(&ysonStream, NYson::EYsonFormat::Text);
    NYql::IDataProvider::TFillSettings fillSettings;
    bool truncated;
    KikimrResultToYson(ysonStream, writer, actual, {}, fillSettings, truncated);
    UNIT_ASSERT(!truncated);

    CompareYson(expected, ysonStream.Str(), message);
}

bool HasIssue(const NYql::TIssues& issues, ui32 code,
    std::function<bool(const NYql::TIssue& issue)> predicate)
{
    bool hasIssue = false;

    for (auto& issue : issues) {
        WalkThroughIssues(issue, false, [code, predicate, &hasIssue] (const NYql::TIssue& issue, int level) {
            Y_UNUSED(level);
            if (issue.GetCode() == code) {
                bool issueMatch = predicate
                    ? predicate(issue)
                    : true;

                hasIssue = hasIssue || issueMatch;
            }
        });
    }

    return hasIssue;
}

bool HasIssue(const NYdb::NIssue::TIssues& issues, ui32 code,
    std::function<bool(const NYdb::NIssue::TIssue& issue)> predicate)
{
    bool hasIssue = false;

    for (auto& issue : issues) {
        NYdb::NIssue::WalkThroughIssues(issue, false, [code, predicate, &hasIssue] (const NYdb::NIssue::TIssue& issue, int level) {
            Y_UNUSED(level);
            if (issue.GetCode() == code) {
                bool issueMatch = predicate
                    ? predicate(issue)
                    : true;

                hasIssue = hasIssue || issueMatch;
            }
        });
    }

    return hasIssue;
}

void PrintQueryStats(const TDataQueryResult& result) {
    if (!result.GetStats().has_value()) {
        return;
    }

    auto stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());

    Cerr << "------- query stats -----------" << Endl;
    for (const auto& qp : stats.query_phases()) {
        Cerr << "-- phase" << Endl
             << "     duration: " << qp.duration_us() << Endl
             << "     access:   " << Endl;
        for (const auto& ta : qp.table_access()) {
            Cerr << "       name:    " << ta.name() << Endl
                 << "       reads:   " << ta.reads().rows() << Endl
                 << "       updates: " << ta.updates().rows() << Endl
                 << "       deletes: " << ta.deletes().rows() << Endl;
        }
    }
}

void AssertTableStats(const Ydb::TableStats::QueryStats& stats, TStringBuf table,
    const TExpectedTableStats& expectedStats)
{
    ui64 actualReads = 0;
    ui64 actualUpdates = 0;
    ui64 actualDeletes = 0;

    for (const auto& phase : stats.query_phases()) {
        for (const auto& access : phase.table_access()) {
            if (access.name() == table) {
                actualReads += access.reads().rows();
                actualUpdates += access.updates().rows();
                actualDeletes += access.deletes().rows();
            }
        }
    }

    if (expectedStats.ExpectedReads) {
        UNIT_ASSERT_EQUAL_C(*expectedStats.ExpectedReads, actualReads, "table: " << table
            << ", reads expected " << *expectedStats.ExpectedReads << ", actual " << actualReads);
    }

    if (expectedStats.ExpectedUpdates) {
        UNIT_ASSERT_EQUAL_C(*expectedStats.ExpectedUpdates, actualUpdates, "table: " << table
            << ", updates expected " << *expectedStats.ExpectedUpdates << ", actual " << actualUpdates);
    }

    if (expectedStats.ExpectedDeletes) {
        UNIT_ASSERT_EQUAL_C(*expectedStats.ExpectedDeletes, actualDeletes, "table: " << table
            << ", deletes expected " << *expectedStats.ExpectedDeletes << ", actual " << actualDeletes);
    }
}

void AssertTableStats(const TDataQueryResult& result, TStringBuf table, const TExpectedTableStats& expectedStats) {
    auto stats = NYdb::TProtoAccessor::GetProto(*result.GetStats());
    return AssertTableStats(stats, table, expectedStats);
}

TDataQueryResult ExecQueryAndTestResult(TSession& session, const TString& query, const NYdb::TParams& params,
    const TString& expectedYson)
{
    NYdb::NTable::TExecDataQuerySettings settings;
    settings.CollectQueryStats(ECollectQueryStatsMode::Profile);

    TDataQueryResult result = session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx(), params, settings)
            .ExtractValueSync();
    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
    if (!result.GetIssues().Empty()) {
        Cerr << result.GetIssues().ToString() << Endl;
    }

    CompareYson(expectedYson, FormatResultSetYson(result.GetResultSet(0)));

    return result;
}

NYdb::NQuery::TExecuteQueryResult ExecQueryAndTestEmpty(NYdb::NQuery::TSession& session, const TString& query) {
    auto result = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx())
        .ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    CompareYson("[[0u]]", FormatResultSetYson(result.GetResultSet(0)));
    return result;
}

void FillProfile(NYdb::NQuery::TExecuteQueryPart& streamPart, NYson::TYsonWriter& writer, TVector<TString>* profiles,
    ui32 profileIndex)
{
    Y_UNUSED(streamPart);
    Y_UNUSED(writer);
    Y_UNUSED(profiles);
    Y_UNUSED(profileIndex);
}

void FillProfile(NYdb::NTable::TScanQueryPart& streamPart, NYson::TYsonWriter& writer, TVector<TString>* profiles,
    ui32 profileIndex)
{
    Y_UNUSED(streamPart);
    Y_UNUSED(writer);
    Y_UNUSED(profiles);
    Y_UNUSED(profileIndex);
}

void CreateLargeTable(TKikimrRunner& kikimr, ui32 rowsPerShard, ui32 keyTextSize,
    ui32 dataTextSize, ui32 batchSizeRows, ui32 fillShardsCount, ui32 largeTableKeysPerShard)
{
    kikimr.GetTestClient().CreateTable("/Root", R"(
        Name: "LargeTable"
        Columns { Name: "Key", Type: "Uint64" }
        Columns { Name: "KeyText", Type: "String" }
        Columns { Name: "Data", Type: "Int64" }
        Columns { Name: "DataText", Type: "String" }
        KeyColumnNames: ["Key", "KeyText"],
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 1000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 2000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 3000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 4000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 5000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 6000000 } } } }
        SplitBoundary { KeyPrefix { Tuple { Optional { Uint64: 7000000 } } } }
    )");

    auto client = kikimr.GetTableClient();

    for (ui32 shardIdx = 0; shardIdx < fillShardsCount; ++shardIdx) {
        ui32 rowIndex = 0;
        while (rowIndex < rowsPerShard) {

            auto rowsBuilder = NYdb::TValueBuilder();
            rowsBuilder.BeginList();
            for (ui32 i = 0; i < batchSizeRows; ++i) {
                rowsBuilder.AddListItem()
                    .BeginStruct()
                    .AddMember("Key")
                        .OptionalUint64(shardIdx * largeTableKeysPerShard + rowIndex)
                    .AddMember("KeyText")
                        .OptionalString(TString(keyTextSize, '0' + (i + shardIdx) % 10))
                    .AddMember("Data")
                        .OptionalInt64(rowIndex)
                    .AddMember("DataText")
                        .OptionalString(TString(dataTextSize, '0' + (i + shardIdx + 1) % 10))
                    .EndStruct();

                ++rowIndex;
                if (rowIndex == rowsPerShard) {
                    break;
                }
            }
            rowsBuilder.EndList();

            auto result = client.BulkUpsert("/Root/LargeTable", rowsBuilder.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
        }
    }
}

void CreateManyShardsTable(TKikimrRunner& kikimr, ui32 totalRows, ui32 shards, ui32 batchSizeRows)
{
    kikimr.GetTestClient().CreateTable("/Root", R"(
        Name: "ManyShardsTable"
        Columns { Name: "Key", Type: "Uint32" }
        Columns { Name: "Data", Type: "Int32" }
        KeyColumnNames: ["Key"]
        UniformPartitionsCount:
    )" + std::to_string(shards));

    auto client = kikimr.GetTableClient();

    for (ui32 rows = 0; rows < totalRows; rows += batchSizeRows) {
        auto rowsBuilder = NYdb::TValueBuilder();
        rowsBuilder.BeginList();
        for (ui32 i = 0; i < batchSizeRows && rows + i < totalRows; ++i) {
            rowsBuilder.AddListItem()
                .BeginStruct()
                .AddMember("Key")
                    .OptionalUint32((std::numeric_limits<ui32>::max() / totalRows) * (rows + i))
                .AddMember("Data")
                    .OptionalInt32(i)
                .EndStruct();
        }
        rowsBuilder.EndList();

        auto result = client.BulkUpsert("/Root/ManyShardsTable", rowsBuilder.Build()).ExtractValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }
}

void PrintResultSet(const NYdb::TResultSet& resultSet, NYson::TYsonWriter& writer) {
    auto columns = resultSet.GetColumnsMeta();

    NYdb::TResultSetParser parser(resultSet);
    while (parser.TryNextRow()) {
        writer.OnListItem();
        writer.OnBeginList();
        for (ui32 i = 0; i < columns.size(); ++i) {
            writer.OnListItem();
            FormatValueYson(parser.GetValue(i), writer);
        }
        writer.OnEndList();
    }
}

bool IsTimeoutError(NYdb::EStatus status) {
    return status == NYdb::EStatus::CLIENT_DEADLINE_EXCEEDED || status == NYdb::EStatus::TIMEOUT || status == NYdb::EStatus::CANCELLED;
}

// IssueMessageSubString - uses only in case if !streamPart.IsSuccess()
template<typename TIterator>
TString StreamResultToYsonImpl(TIterator& it, TVector<TString>* profiles, bool throwOnTimeout = false, const NYdb::EStatus& opStatus = NYdb::EStatus::SUCCESS, const TString& issueMessageSubString = "") {
    if (!it.IsSuccess()) {
        UNIT_ASSERT_VALUES_EQUAL_C(it.GetStatus(), opStatus, it.GetIssues().ToString());
        UNIT_ASSERT_STRING_CONTAINS_C(it.GetIssues().ToString(), issueMessageSubString,
            TStringBuilder() << "Issue should contain '" << issueMessageSubString << "'. " << it.GetIssues().ToString());
        return {};
    }

    TStringStream out;
    NYson::TYsonWriter writer(&out, NYson::EYsonFormat::Text, ::NYson::EYsonType::Node, true);
    writer.OnBeginList();

    ui32 profileIndex = 0;

    for (;;) {
        auto streamPart = it.ReadNext().GetValueSync();
        if (!streamPart.IsSuccess()) {
            if (opStatus != NYdb::EStatus::SUCCESS) {
                UNIT_ASSERT_VALUES_EQUAL_C(streamPart.GetStatus(), opStatus, streamPart.GetIssues().ToString());
                UNIT_ASSERT_STRING_CONTAINS_C(streamPart.GetIssues().ToString(), issueMessageSubString,
                    TStringBuilder() << "Issue should contain '" << issueMessageSubString << "'. " << streamPart.GetIssues().ToString());
                break;
            }
            if (throwOnTimeout && IsTimeoutError(streamPart.GetStatus())) {
                throw TStreamReadError(streamPart.GetStatus());
            }
            UNIT_ASSERT_C(streamPart.EOS(), streamPart.GetIssues().ToString());
            break;
        }

        if (streamPart.HasResultSet()) {
            auto resultSet = streamPart.ExtractResultSet();
            PrintResultSet(resultSet, writer);
        }

        FillProfile(streamPart, writer, profiles, profileIndex);
        profileIndex++;
    }

    writer.OnEndList();

    return out.Str();
}

TString StreamResultToYson(NYdb::NQuery::TExecuteQueryIterator& it, bool throwOnTimeout, const NYdb::EStatus& opStatus, const TString& issueMessageSubString) {
    return StreamResultToYsonImpl(it, nullptr, throwOnTimeout, opStatus, issueMessageSubString);
}

TString StreamResultToYson(NYdb::NTable::TScanQueryPartIterator& it, bool throwOnTimeout, const NYdb::EStatus& opStatus, const TString& issueMessageSubString) {
    return StreamResultToYsonImpl(it, nullptr, throwOnTimeout, opStatus, issueMessageSubString);
}

TString StreamResultToYson(NYdb::NTable::TTablePartIterator& it, bool throwOnTimeout, const NYdb::EStatus& opStatus) {
    if (!it.IsSuccess()) {
        UNIT_ASSERT_VALUES_EQUAL_C(it.GetStatus(), opStatus, it.GetIssues().ToString());
        return {};
    }

    TStringStream out;
    NYson::TYsonWriter writer(&out, NYson::EYsonFormat::Text, ::NYson::EYsonType::Node, true);
    writer.OnBeginList();

    for (;;) {
        auto streamPart = it.ReadNext().GetValueSync();
        if (!streamPart.IsSuccess()) {
            if (opStatus != NYdb::EStatus::SUCCESS) {
                UNIT_ASSERT_VALUES_EQUAL_C(streamPart.GetStatus(), opStatus, streamPart.GetIssues().ToString());
                break;
            }
            if (throwOnTimeout && IsTimeoutError(streamPart.GetStatus())) {
                throw TStreamReadError(streamPart.GetStatus());
            }
            UNIT_ASSERT_C(streamPart.EOS(), streamPart.GetIssues().ToString());
            break;
        }

        auto resultSet = streamPart.ExtractPart();
        PrintResultSet(resultSet, writer);
    }

    writer.OnEndList();

    return out.Str();
}

TString StreamResultToYson(NYdb::NScripting::TYqlResultPartIterator& it, bool throwOnTimeout, const NYdb::EStatus& opStatus) {
    if (!it.IsSuccess()) {
        UNIT_ASSERT_VALUES_EQUAL_C(it.GetStatus(), opStatus, it.GetIssues().ToString());
        return {};
    }

    TStringStream out;
    NYson::TYsonWriter writer(&out, NYson::EYsonFormat::Text, ::NYson::EYsonType::Node, true);
    writer.OnBeginList();

    ui32 currentIndex = 0;
    writer.OnListItem();
    writer.OnBeginList();

    for (;;) {
        auto streamPart = it.ReadNext().GetValueSync();
        if (!streamPart.IsSuccess()) {
            if (opStatus != NYdb::EStatus::SUCCESS) {
                UNIT_ASSERT_VALUES_EQUAL_C(streamPart.GetStatus(), opStatus, streamPart.GetIssues().ToString());
                break;
            }
            if (throwOnTimeout && IsTimeoutError(streamPart.GetStatus())) {
                throw TStreamReadError(streamPart.GetStatus());
            }
            UNIT_ASSERT_C(streamPart.EOS(), streamPart.GetIssues().ToString());
            break;
        }

        if (streamPart.HasPartialResult()) {
            const auto& partialResult = streamPart.GetPartialResult();

            ui32 resultSetIndex = partialResult.GetResultSetIndex();
            if (currentIndex != resultSetIndex) {
                currentIndex = resultSetIndex;
                writer.OnEndList();
                writer.OnListItem();
                writer.OnBeginList();
            }

            PrintResultSet(partialResult.GetResultSet(), writer);
        }
    }

    writer.OnEndList();
    writer.OnEndList();

    return out.Str();
}

static void FillPlan(const NYdb::NTable::TScanQueryPart& streamPart, TCollectedStreamResult& res) {
    if (streamPart.HasQueryStats() ) {
        res.QueryStats = NYdb::TProtoAccessor::GetProto(streamPart.GetQueryStats());

        auto plan = res.QueryStats->query_plan();
        if (!plan.empty()) {
            res.PlanJson = plan;
        }

        auto ast = res.QueryStats->query_ast();
        if (!ast.empty()) {
            res.Ast = ast;
        }
    }
}

static void FillPlan(const NYdb::NScripting::TYqlResultPart& streamPart, TCollectedStreamResult& res) {
    if (streamPart.HasQueryStats() ) {
        res.QueryStats = NYdb::TProtoAccessor::GetProto(streamPart.GetQueryStats());

        auto plan = res.QueryStats->query_plan();
        if (!plan.empty()) {
            res.PlanJson = plan;
        }

        auto ast = res.QueryStats->query_ast();
        if (!ast.empty()) {
            res.Ast = ast;
        }
    }
}

static void FillPlan(const NYdb::NQuery::TExecuteQueryPart& streamPart, TCollectedStreamResult& res) {
    if (streamPart.GetStats() ) {
        res.QueryStats = NYdb::TProtoAccessor::GetProto(*streamPart.GetStats());

        auto plan = res.QueryStats->query_plan();
        if (!plan.empty()) {
            res.PlanJson = plan;
        }

        auto ast = res.QueryStats->query_ast();
        if (!ast.empty()) {
            res.Ast = ast;
        }
    }
}

template<typename TIterator>
TCollectedStreamResult CollectStreamResultImpl(TIterator& it) {
    TCollectedStreamResult res;

    TStringStream out;
    NYson::TYsonWriter resultSetWriter(&out, NYson::EYsonFormat::Text, ::NYson::EYsonType::Node, true);
    resultSetWriter.OnBeginList();

    for (;;) {
        auto streamPart = it.ReadNext().GetValueSync();
        if (!streamPart.IsSuccess()) {
            UNIT_ASSERT_C(streamPart.EOS(), streamPart.GetIssues().ToString());
            const auto& meta = streamPart.GetResponseMetadata();
            auto mit = meta.find("x-ydb-consumed-units");
            if (mit != meta.end()) {
                res.ConsumedRuFromHeader = std::stol(mit->second);
            }
            UNIT_ASSERT_EQUAL_C(res.ConsumedRuFromHeader, streamPart.GetConsumedRu(),
                "Request unit values from headers and TStatus are differ");
            break;
        }

        if constexpr (std::is_same_v<TIterator, NYdb::NTable::TScanQueryPartIterator>) {
            UNIT_ASSERT_C(streamPart.HasResultSet() || streamPart.HasQueryStats(),
                "Unexpected empty scan query response.");

            if (streamPart.HasResultSet()) {
                auto resultSet = streamPart.ExtractResultSet();
                PrintResultSet(resultSet, resultSetWriter);
                res.RowsCount += resultSet.RowsCount();
            }
        }

        if constexpr (std::is_same_v<TIterator, NYdb::NQuery::TExecuteQueryIterator>) {
            if (streamPart.HasResultSet()) {
                auto resultSet = streamPart.ExtractResultSet();
                PrintResultSet(resultSet, resultSetWriter);
                res.RowsCount += resultSet.RowsCount();
            }
        }

        if constexpr (std::is_same_v<TIterator, NYdb::NScripting::TYqlResultPartIterator>) {
            if (streamPart.HasPartialResult()) {
                const auto& partialResult = streamPart.GetPartialResult();
                const auto& resultSet = partialResult.GetResultSet();
                PrintResultSet(resultSet, resultSetWriter);
                res.RowsCount += resultSet.RowsCount();
            }
        }

        if constexpr (std::is_same_v<TIterator, NYdb::NTable::TScanQueryPartIterator>
                || std::is_same_v<TIterator, NYdb::NScripting::TYqlResultPartIterator>
                || std::is_same_v<TIterator, NYdb::NQuery::TExecuteQueryIterator>) {
            FillPlan(streamPart, res);
        } else {
            if (streamPart.HasPlan()) {
                res.PlanJson = streamPart.ExtractPlan();
            }
        }
    }

    resultSetWriter.OnEndList();

    res.ResultSetYson = out.Str();
    return res;
}

template<typename TIterator>
TCollectedStreamResult CollectStreamResult(TIterator& it) {
    return CollectStreamResultImpl(it);
}

template TCollectedStreamResult CollectStreamResult(NYdb::NTable::TScanQueryPartIterator& it);
template TCollectedStreamResult CollectStreamResult(NYdb::NScripting::TYqlResultPartIterator& it);
template TCollectedStreamResult CollectStreamResult(NYdb::NQuery::TExecuteQueryIterator& it);

TString ReadTableToYson(NYdb::NTable::TSession session, const TString& table) {
    TReadTableSettings settings;
    settings.Ordered(true);
    auto it = session.ReadTable(table, settings).GetValueSync();
    UNIT_ASSERT(it.IsSuccess());
    return StreamResultToYson(it);
}

TString ReadTablePartToYson(NYdb::NTable::TSession session, const TString& table) {
    TReadTableSettings settings;
    settings.Ordered(true);
    auto it = session.ReadTable(table, settings).GetValueSync();
    UNIT_ASSERT(it.IsSuccess());

    TReadTableResultPart streamPart = it.ReadNext().GetValueSync();
    if (!streamPart.IsSuccess()) {
        streamPart.GetIssues().PrintTo(Cerr);
        if (streamPart.EOS()) {
            return {};
        }
        UNIT_ASSERT_C(false, "Status: " << streamPart.GetStatus());
    }
    return NYdb::FormatResultSetYson(streamPart.ExtractPart());
}

bool ValidatePlanNodeIds(const NJson::TJsonValue& plan) {
    ui32 planNodeId = 0;
    ui32 count = 0;

    do {
        count = CountPlanNodesByKv(plan, "PlanNodeId", std::to_string(++planNodeId));
        if (count > 1) {
            return false;
        }
    } while (count > 0);

    return true;
}

ui32 CountPlanNodesByKv(const NJson::TJsonValue& plan, const TString& key, const TString& value) {
    ui32 result = 0;

    if (plan.IsArray()) {
        for (const auto &node: plan.GetArray()) {
            result += CountPlanNodesByKv(node, key, value);
        }
        return result;
    }

    UNIT_ASSERT(plan.IsMap());

    auto map = plan.GetMap();
    if (map.contains(key) && map.at(key).GetStringRobust() == value) {
        return 1;
    }

    if (map.contains("Plans")) {
        for (const auto &node: map["Plans"].GetArraySafe()) {
            result += CountPlanNodesByKv(node, key, value);
        }
    }

    if (map.contains("Plan")) {
        result += CountPlanNodesByKv(map.at("Plan"), key, value);
    }

    if (map.contains("Operators")) {
        for (const auto &node : map["Operators"].GetArraySafe()) {
            result += CountPlanNodesByKv(node, key, value);
        }
    }

    return result;
}

NJson::TJsonValue FindPlanNodeByKv(const NJson::TJsonValue& plan, const TString& key, const TString& value) {
    if (plan.IsArray()) {
        for (const auto &node: plan.GetArray()) {
            auto stage = FindPlanNodeByKv(node, key, value);
            if (stage.IsDefined()) {
                return stage;
            }
        }
    } else if (plan.IsMap()) {
        auto map = plan.GetMap();
        if (map.contains(key) && map.at(key).GetStringRobust() == value) {
            return plan;
        }
        if (map.contains("Plans")) {
            for (const auto &node: map["Plans"].GetArraySafe()) {
                auto stage = FindPlanNodeByKv(node, key, value);
                if (stage.IsDefined()) {
                    return stage;
                }
            }
        } else if (map.contains("Plan")) {
            auto stage = FindPlanNodeByKv(map.at("Plan"), key, value);
            if (stage.IsDefined()) {
                return stage;
            }
        }

        if (map.contains("Operators")) {
            for (const auto &node : map["Operators"].GetArraySafe()) {
                auto op = FindPlanNodeByKv(node, key, value);
                if (op.IsDefined()) {
                    return op;
                }
            }
        }

        if (map.contains("queries")) {
            for (const auto &node : map["queries"].GetArraySafe()) {
                auto op = FindPlanNodeByKv(node, key, value);
                if (op.IsDefined()) {
                    return op;
                }
            }
        }
    } else {
        Y_ASSERT(false);
    }

    return NJson::TJsonValue();
}

void FindPlanNodesImpl(const NJson::TJsonValue& node, const TString& key, std::vector<NJson::TJsonValue>& results) {
    if (node.IsArray()) {
        for (const auto& item: node.GetArray()) {
            FindPlanNodesImpl(item, key, results);
        }
    }

    if (!node.IsMap()) {
        return;
    }

    if (auto* valueNode = node.GetValueByPath(key)) {
        results.push_back(*valueNode);
    }

    for (const auto& [_, value]: node.GetMap()) {
        FindPlanNodesImpl(value, key, results);
    }
}

void FindPlanStagesImpl(const NJson::TJsonValue& node, std::vector<NJson::TJsonValue>& stages) {
    if (node.IsArray()) {
        for (const auto& item: node.GetArray()) {
            FindPlanStagesImpl(item, stages);
        }
    }

    if (!node.IsMap()) {
        return;
    }

    auto map = node.GetMap();
    // TODO: Use explicit PlanNodeType for stages
    if (map.contains("Node Type") && !map.contains("PlanNodeType")) {
        stages.push_back(node);
    }

    for (const auto& [_, value]: map) {
        FindPlanStagesImpl(value, stages);
    }
}

std::vector<NJson::TJsonValue> FindPlanNodes(const NJson::TJsonValue& plan, const TString& key) {
    std::vector<NJson::TJsonValue> results;
    FindPlanNodesImpl(plan, key, results);
    return results;
}

std::vector<NJson::TJsonValue> FindPlanStages(const NJson::TJsonValue& plan) {
    std::vector<NJson::TJsonValue> stages;
    FindPlanStagesImpl(plan.GetMapSafe().at("Plan"), stages);
    return stages;
}

void CreateSampleTablesWithIndex(TSession& session, bool populateTables, bool withPgTypes) {
    auto res = session.ExecuteSchemeQuery(R"(
        --!syntax_v1
        CREATE TABLE `/Root/SecondaryKeys` (
            Key Int32,
            Fk Int32,
            Value String,
            PRIMARY KEY (Key),
            INDEX Index GLOBAL ON (Fk)
        );
        CREATE TABLE `/Root/SecondaryComplexKeys` (
            Key Int32,
            Fk1 Int32,
            Fk2 String,
            Value String,
            PRIMARY KEY (Key),
            INDEX Index GLOBAL ON (Fk1, Fk2)
        );
        CREATE TABLE `/Root/SecondaryWithDataColumns` (
            Key String,
            Index2 String,
            Value String,
            ExtPayload String,
            PRIMARY KEY (Key),
            INDEX Index GLOBAL ON (Index2)
            COVER (Value)
        );

    )").GetValueSync();
    UNIT_ASSERT_C(res.IsSuccess(), res.GetIssues().ToString());

    if (withPgTypes) {
        auto res = session.ExecuteSchemeQuery(R"(
            --!syntax_v1
            CREATE TABLE `/Root/SecondaryPgTypeKeys` (
                Key pgint4,
                Fk pgint4,
                Value String,
                PRIMARY KEY (Key),
                INDEX Index GLOBAL ON (Fk)
            );

        )").GetValueSync();
        UNIT_ASSERT_C(res.IsSuccess(), res.GetIssues().ToString());
    }

    if (!populateTables)
        return;

    auto result = session.ExecuteDataQuery(R"(

        REPLACE INTO `KeyValue` (Key, Value) VALUES
            (3u,   "Three"),
            (4u,   "Four"),
            (10u,  "Ten"),
            (NULL, "Null Value");

        REPLACE INTO `Test` (Group, Name, Amount, Comment) VALUES
            (1u, "Jack",     100500ul, "Just Jack"),
            (3u, "Harry",    5600ul,   "Not Potter"),
            (3u, "Joshua",   8202ul,   "Very popular name in GB"),
            (3u, "Muhammad", 887773ul, "Also very popular name in GB"),
            (4u, "Hugo",     77,       "Boss");

        REPLACE INTO `/Root/SecondaryKeys` (Key, Fk, Value) VALUES
            (1,    1,    "Payload1"),
            (2,    2,    "Payload2"),
            (5,    5,    "Payload5"),
            (NULL, 6,    "Payload6"),
            (7,    NULL, "Payload7"),
            (NULL, NULL, "Payload8");

        REPLACE INTO `/Root/SecondaryComplexKeys` (Key, Fk1, Fk2, Value) VALUES
            (1,    1,    "Fk1", "Payload1"),
            (2,    2,    "Fk2", "Payload2"),
            (5,    5,    "Fk5", "Payload5"),
            (NULL, 6,    "Fk6", "Payload6"),
            (7,    NULL, "Fk7", "Payload7"),
            (NULL, NULL, NULL,  "Payload8");

        REPLACE INTO `/Root/SecondaryWithDataColumns` (Key, Index2, Value) VALUES
            ("Primary1", "Secondary1", "Value1");

    )", TTxControl::BeginTx().CommitTx()).GetValueSync();

    if (withPgTypes) {
        auto result = session.ExecuteDataQuery(R"(

            REPLACE INTO `/Root/SecondaryPgTypeKeys` (Key, Fk, Value) VALUES
                (1pi,  1pi,  "Payload1"),
                (2pi,  2pi,  "Payload2"),
                (5pi,  5pi,  "Payload5"),
                (NULL, 6pi,  "Payload6"),
                (7pi,  NULL, "Payload7");

        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
    }

    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
}

void InitRoot(Tests::TServer::TPtr server, TActorId sender) {
    server->SetupRootStoragePools(sender);
}

void Grant(NYdb::NTable::TSession& adminSession, const char* permissions, const char* path, const char* user) {
    auto grantQuery = Sprintf(R"(
            GRANT %s ON `%s` TO `%s`;
        )",
        permissions, path, user
    );
    auto result = adminSession.ExecuteSchemeQuery(grantQuery).ExtractValueSync();
    UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
};

THolder<NSchemeCache::TSchemeCacheNavigate> Navigate(TTestActorRuntime& runtime, const TActorId& sender,
                                                     const TString& path, NSchemeCache::TSchemeCacheNavigate::EOp op)
{
    using TNavigate = NSchemeCache::TSchemeCacheNavigate;
    using TEvRequest = TEvTxProxySchemeCache::TEvNavigateKeySet;
    using TEvResponse = TEvTxProxySchemeCache::TEvNavigateKeySetResult;

    auto request = MakeHolder<TNavigate>();
    auto& entry = request->ResultSet.emplace_back();
    entry.Path = SplitPath(path);
    entry.RequestType = TNavigate::TEntry::ERequestType::ByPath;
    entry.Operation = op;
    entry.ShowPrivatePath = true;
    runtime.Send(MakeSchemeCacheID(), sender, new TEvRequest(request.Release()));

    auto ev = runtime.GrabEdgeEventRethrow<TEvResponse>(sender);
    UNIT_ASSERT(ev);
    UNIT_ASSERT(ev->Get());

    auto* response = ev->Get()->Request.Release();
    UNIT_ASSERT(response);
    UNIT_ASSERT_VALUES_EQUAL(response->ResultSet.size(), 1);

    return THolder(response);
}

 NKikimrScheme::TEvDescribeSchemeResult DescribeTable(Tests::TServer* server,
                                                        TActorId sender,
                                                        const TString &path)
{
    auto &runtime = *server->GetRuntime();
    TAutoPtr<IEventHandle> handle;

    auto request = MakeHolder<TEvTxUserProxy::TEvNavigate>();
    request->Record.MutableDescribePath()->SetPath(path);
    request->Record.MutableDescribePath()->MutableOptions()->SetShowPrivateTable(true);
    runtime.Send(new IEventHandle(MakeTxProxyID(), sender, request.Release()));
    auto reply = runtime.GrabEdgeEventRethrow<NSchemeShard::TEvSchemeShard::TEvDescribeSchemeResult>(handle);

    return *reply->MutableRecord();
}

TVector<ui64> GetTableShards(Tests::TServer* server,
                            TActorId sender,
                            const TString &path)
{
    TVector<ui64> shards;
    auto lsResult = DescribeTable(server, sender, path);
    for (auto &part : lsResult.GetPathDescription().GetTablePartitions())
        shards.push_back(part.GetDatashardId());

    return shards;
}

TVector<ui64> GetColumnTableShards(Tests::TServer* server,
                                   TActorId sender,
                                   const TString &path)
{
    TVector<ui64> shards;
    auto lsResult = DescribeTable(server, sender, path);
    for (auto &part : lsResult.GetPathDescription().GetColumnTableDescription().GetSharding().GetColumnShards())
        shards.push_back(part);

    return shards;
}

TVector<ui64> GetTableShards(Tests::TServer::TPtr server,
                                TActorId sender,
                                const TString &path) {
    return GetTableShards(server.Get(), sender, path);
 }

void WaitForZeroSessions(const NKqp::TKqpCounters& counters) {
    int count = 60;
    while (counters.GetActiveSessionActors()->Val() != 0 && count) {
        count--;
        Sleep(TDuration::Seconds(1));
    }

    UNIT_ASSERT_C(count, "Unable to wait for proper active session count, it looks like cancelation doesn`t work");
}

void WaitForZeroReadIterators(Tests::TServer& server, const TString& path) {
    int iterators = 0;
    static const TString counterName = "DataShard/ReadIteratorsCount";

    for (int i = 0; i < 10; i++, Sleep(TDuration::Seconds(1))) {
        TTestActorRuntime* runtime = server.GetRuntime();
        auto sender = runtime->AllocateEdgeActor();
        auto shards = GetTableShards(&server, sender, path);
        UNIT_ASSERT_C(shards.size() > 0, "Table: " << path << " has no shards");
        iterators = 0;
        for (auto x : shards) {
            runtime->SendToPipe(
                x,
                sender,
                new TEvTablet::TEvGetCounters,
                0,
                GetPipeConfigWithRetries());

            auto ev = runtime->GrabEdgeEvent<TEvTablet::TEvGetCountersResponse>(sender);
            UNIT_ASSERT(ev);

            const NKikimrTabletBase::TEvGetCountersResponse& resp = ev->Get()->Record;
            for (const auto& counter : resp.GetTabletCounters().GetAppCounters().GetSimpleCounters()) {
                if (counterName != counter.GetName()) {
                    continue;
                }

                iterators += counter.GetValue();
            }
        }
        if (iterators == 0) {
            break;
        }
    }

    UNIT_ASSERT_C(iterators == 0, "Unable to wait for proper read iterator count, it looks like cancelation doesn`t work (" << iterators << ")");
}

int GetCumulativeCounterValue(Tests::TServer& server, const TString& path, const TString& counterName) {
    int result = 0;

    TTestActorRuntime* runtime = server.GetRuntime();
    auto sender = runtime->AllocateEdgeActor();
    auto shards = GetTableShards(&server, sender, path);
    UNIT_ASSERT_C(shards.size() > 0, "Table: " << path << " has no shards");

    for (auto x : shards) {
        runtime->SendToPipe(
            x,
            sender,
            new TEvTablet::TEvGetCounters,
            0,
            GetPipeConfigWithRetries());

        auto ev = runtime->GrabEdgeEvent<TEvTablet::TEvGetCountersResponse>(sender);
        UNIT_ASSERT(ev);

        const NKikimrTabletBase::TEvGetCountersResponse& resp = ev->Get()->Record;
        for (const auto& counter : resp.GetTabletCounters().GetAppCounters().GetCumulativeCounters()) {
            if (counter.GetName() == counterName) {
                result += counter.GetValue();
            }
        }
    }

    return result;
}

void CheckTableReads(NYdb::NTable::TSession& session, const TString& tableName, bool checkFollower, bool readsExpected) {
    for (size_t attempt = 0; attempt < 30; ++attempt)
    {
        Cerr << "... SELECT from partition_stats for " << tableName << " , attempt " << attempt << Endl;

        const TString selectPartitionStats(Q_(Sprintf(R"(
            SELECT *
            FROM `/Root/.sys/partition_stats`
            WHERE FollowerId %s 0 AND (RowReads != 0 OR RangeReadRows != 0) AND Path = '%s'
        )", (checkFollower ? "!=" : "="), tableName.c_str())));

        auto result = session.ExecuteDataQuery(selectPartitionStats, TTxControl::BeginTx().CommitTx()).ExtractValueSync();
        AssertSuccessResult(result);
        Cerr << selectPartitionStats << Endl;

        auto rs = result.GetResultSet(0);
        if (readsExpected) {
            if (rs.RowsCount() != 0)
                return;
            Sleep(TDuration::Seconds(5));
        } else {
            if (rs.RowsCount() == 0)
                return;
            Y_FAIL("!readsExpected, but there are read stats for %s", tableName.c_str());
        }
    }
    Y_FAIL("readsExpected, but there is timeout waiting for read stats from %s", tableName.c_str());
}

TTableId ResolveTableId(Tests::TServer* server, TActorId sender, const TString& path) {
    auto response = Navigate(*server->GetRuntime(), sender, path, NSchemeCache::TSchemeCacheNavigate::EOp::OpUnknown);
    return response->ResultSet.at(0).TableId;
}

NKikimrTxDataShard::TEvCompactTableResult CompactTable(
    Tests::TServer* server, ui64 shardId, const TTableId& tableId, bool compactBorrowed)
{
    TTestActorRuntime* runtime = server->GetRuntime();
    auto sender = runtime->AllocateEdgeActor();
    auto request = MakeHolder<TEvDataShard::TEvCompactTable>(tableId.PathId);
    request->Record.SetCompactBorrowed(compactBorrowed);
    runtime->SendToPipe(shardId, sender, request.Release(), 0, GetPipeConfigWithRetries());

    auto ev = runtime->GrabEdgeEventRethrow<TEvDataShard::TEvCompactTableResult>(sender);
    return ev->Get()->Record;
}

void WaitForCompaction(Tests::TServer* server, const TString& path, bool compactBorrowed) {
    TTestActorRuntime* runtime = server->GetRuntime();
    auto sender = runtime->AllocateEdgeActor();
    auto shards = GetTableShards(server, sender, path);
    auto tableId = ResolveTableId(server, sender, path);
    for (auto shard : shards) {
        CompactTable(server, shard, tableId, compactBorrowed);
    }
}

NJson::TJsonValue SimplifyPlan(NJson::TJsonValue& opt, const TGetPlanParams& params) {
    if (!opt.IsMap()) {
        return {};
    }
    const auto& [_, nodeType] = *opt.GetMapSafe().find("Node Type");
    bool isShuffle = nodeType.GetStringSafe().find("HashShuffle") != TString::npos;

    if (isShuffle) {
        NJson::TJsonValue op;
        op["Name"] = "HashShuffle";
        opt["Operators"].AppendValue(std::move(op));
    }

    if (auto ops = opt.GetMapSafe().find("Operators"); ops != opt.GetMapSafe().end()) {
        auto opName = ops->second.GetArraySafe()[0].GetMapSafe().at("Name").GetStringSafe();
        if (
            opName.find("Join") != TString::npos ||
            opName.find("Union") != TString::npos ||
            (opName.find("Filter") != TString::npos && params.IncludeFilters) ||
            (opName.find("HashShuffle") != TString::npos && params.IncludeShuffles)
        ) {
            NJson::TJsonValue newChildren;

            for (auto c : opt.GetMapSafe().at("Plans").GetArraySafe()) {
                newChildren.AppendValue(SimplifyPlan(c, params));
            }

            opt["Plans"] = newChildren;
            return opt;
        }
        else if (opName.find("Table") != TString::npos) {
            return opt;
        }
    }

    if (opt.IsMap() && opt.GetMapSafe().contains("Plans")) {
        auto firstPlan = opt.GetMapSafe().at("Plans").GetArraySafe()[0];
        return SimplifyPlan(firstPlan, params);
    }

    return {};
}

bool JoinOrderAndAlgosMatch(const NJson::TJsonValue& opt, const NJson::TJsonValue& ref) {
    auto op = opt.GetMapSafe().at("Operators").GetArraySafe()[0];
    if (op.GetMapSafe().at("Name").GetStringSafe() != ref.GetMapSafe().at("op_name").GetStringSafe()) {
        return false;
    }

    auto refMap = ref.GetMapSafe();
    if (auto args = refMap.find("args"); args != refMap.end()){
        if (!opt.GetMapSafe().contains("Plans")){
            return false;
        }
        auto subplans = opt.GetMapSafe().at("Plans").GetArraySafe();
        if (args->second.GetArraySafe().size() != subplans.size()) {
            return false;
        }
        for (size_t i=0; i<subplans.size(); i++) {
            if (!JoinOrderAndAlgosMatch(subplans[i],args->second.GetArraySafe()[i])) {
                return false;
            }
        }
        return true;
    } else {
        if (!op.GetMapSafe().contains("Table")) {
            return false;
        }
        return op.GetMapSafe().at("Table").GetStringSafe() == refMap.at("table").GetStringSafe();
    }
}

bool JoinOrderAndAlgosMatch(const TString& optimized, const TString& reference){
    NJson::TJsonValue optRoot;
    NJson::ReadJsonTree(optimized, &optRoot, true);
    optRoot = SimplifyPlan(optRoot.GetMapSafe().at("SimplifiedPlan"), {});

    NJson::TJsonValue refRoot;
    NJson::ReadJsonTree(reference, &refRoot, true);

    return JoinOrderAndAlgosMatch(optRoot, refRoot);
}

/* Temporary solution to canonize tests */
NJson::TJsonValue GetDetailedJoinOrderImpl(const NJson::TJsonValue& opt, const TGetPlanParams& params) {
    NJson::TJsonValue res;

    if (!opt.GetMapSafe().contains("Plans") && !params.IncludeTables) {
        return res;
    }

    auto op = opt.GetMapSafe().at("Operators").GetArraySafe()[0];
    res["op_name"] = op.GetMapSafe().at("Name").GetStringSafe();
    if (params.IncludeOptimizerEstimation && op.GetMapSafe().contains("E-Rows")) {
        res["e-size"] = op.GetMapSafe().at("E-Rows").GetStringSafe();
    }


    if (!opt.GetMapSafe().contains("Plans")) {
        res["table"] = op.GetMapSafe().at("Table").GetStringSafe();
        return res;
    }

    auto subplans = opt.GetMapSafe().at("Plans").GetArraySafe();
    for (size_t i = 0; i < subplans.size(); ++i) {
        res["args"].AppendValue(GetDetailedJoinOrderImpl(subplans[i], params));
    }
    return res;
}

NJson::TJsonValue GetDetailedJoinOrder(const TString& deserializedPlan, const TGetPlanParams& params) {
    NJson::TJsonValue optRoot;
    NJson::ReadJsonTree(deserializedPlan, &optRoot, true);
    optRoot = SimplifyPlan(optRoot.GetMapSafe().at("SimplifiedPlan"), params);
    return GetDetailedJoinOrderImpl(optRoot, params);
}

NJson::TJsonValue GetJoinOrderImpl(const NJson::TJsonValue& opt) {
    if (!opt.IsMap()) {
        return {};
    }

    if (!opt.GetMapSafe().contains("Plans")) {
        auto op = opt.GetMapSafe().at("Operators").GetArraySafe()[0];
        return op.GetMapSafe().at("Table").GetStringSafe();
    }

    NJson::TJsonValue res;

    auto subplans = opt.GetMapSafe().at("Plans").GetArraySafe();
    for (size_t i = 0; i < subplans.size(); ++i) {
        res.AppendValue(GetJoinOrderImpl(subplans[i]));
    }

    return res;
}

NJson::TJsonValue GetJoinOrder(const TString& deserializedPlan) {
    NJson::TJsonValue optRoot;
    NJson::ReadJsonTree(deserializedPlan, &optRoot, true);
    optRoot = SimplifyPlan(optRoot.GetMapSafe().at("SimplifiedPlan"), {});
    return GetJoinOrderImpl(optRoot);
}

NJson::TJsonValue GetJoinOrderFromDetailedJoinOrderImpl(const NJson::TJsonValue& opt) {
    if (!opt.IsMap()) {
        return {};
    }

    if (!opt.GetMapSafe().contains("table")) {
        NJson::TJsonValue res;
        auto args = opt.GetMapSafe().at("args").GetArraySafe();
        for (size_t i = 0; i < args.size(); ++i) {
            res.AppendValue(GetJoinOrderFromDetailedJoinOrderImpl(args[i]));
        }
        return res;
    }

    return opt.GetMapSafe().at("table");
}

NJson::TJsonValue GetJoinOrderFromDetailedJoinOrder(const TString& deserializedDetailedJoinOrder) {
    NJson::TJsonValue optRoot;
    NJson::ReadJsonTree(deserializedDetailedJoinOrder, &optRoot, true);
    return GetJoinOrderFromDetailedJoinOrderImpl(optRoot);
}

TTestExtEnv::TTestExtEnv(TTestExtEnv::TEnvSettings envSettings) {
    auto mbusPort = PortManager.GetPort();
    auto grpcPort = PortManager.GetPort();

    Settings = new Tests::TServerSettings(mbusPort);
    EnvSettings = envSettings;

    Settings->SetDomainName("Root");
    Settings->SetNodeCount(EnvSettings.StaticNodeCount);
    Settings->SetDynamicNodeCount(EnvSettings.DynamicNodeCount);
    Settings->SetUseRealThreads(EnvSettings.UseRealThreads);
    Settings->AddStoragePoolType(EnvSettings.PoolName);
    Settings->SetFeatureFlags(EnvSettings.FeatureFlags);

    Server = new Tests::TServer(*Settings);
    Server->EnableGRpc(grpcPort);

    auto sender = Server->GetRuntime()->AllocateEdgeActor();
    Server->SetupRootStoragePools(sender);

    Client = MakeHolder<Tests::TClient>(*Settings);

    Tenants = MakeHolder<Tests::TTenants>(Server);

    Endpoint = "localhost:" + ToString(grpcPort);
    DriverConfig = NYdb::TDriverConfig().SetEndpoint(Endpoint);
    Driver = MakeHolder<NYdb::TDriver>(DriverConfig);
}

TTestExtEnv::~TTestExtEnv() {
    Driver->Stop(true);
}

void TTestExtEnv::CreateDatabase(const TString& databaseName) {
    auto fullDbName = "/Root/" + databaseName;

    Ydb::Cms::CreateDatabaseRequest request;
    request.set_path(fullDbName);

    auto* resources = request.mutable_resources();
    auto* storage = resources->add_storage_units();
    storage->set_unit_kind(EnvSettings.PoolName);
    storage->set_count(1);

    Tenants->CreateTenant(request, EnvSettings.DynamicNodeCount);
}

Tests::TServer& TTestExtEnv::GetServer() const {
    return *Server.Get();
}

Tests::TClient& TTestExtEnv::GetClient() const {
    return *Client;
}

void CheckOwner(TSession& session, const TString& path, const TString& name) {
    TDescribeTableResult describe = session.DescribeTable(path).GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL(describe.GetStatus(), NYdb::EStatus::SUCCESS);
    auto tableDesc = describe.GetTableDescription();
    const auto& currentOwner = tableDesc.GetOwner();
    UNIT_ASSERT_VALUES_EQUAL_C(name, currentOwner, "name is not currentOwner");
}

} // namspace NKqp
} // namespace NKikimr
