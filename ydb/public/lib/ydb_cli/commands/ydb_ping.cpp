#include "ydb_ping.h"

#include <ydb/public/sdk/cpp/client/ydb_ping/client.h>
#include <ydb/public/sdk/cpp/client/ydb_query/client.h>

#include <library/cpp/time_provider/monotonic.h>

namespace NYdb::NConsoleClient {

constexpr int DEFAULT_COUNT = 100;

using namespace NKikimr::NOperationId;

TCommandPing::TCommandPing()
    : TYdbCommand("ping", {}, "ping YDB")
    , Count(DEFAULT_COUNT)
{}

void TCommandPing::Config(TConfig& config) {
    TYdbCommand::Config(config);
    config.Opts->AddLongOption('c', "count", "stop after <count> replies").RequiredArgument("COUNT").StoreResult(&Count);
    config.Opts->AddLongOption(
        't', "type", "ping type: kqp, kqpsys, plainkqp, plaingrpc, grpcproxy").RequiredArgument("TYPE")
            .StoreResult(&PingTypeStr);
}

void TCommandPing::Parse(TConfig& config) {
    TClientCommand::Parse(config);
}

int TCommandPing::Run(TConfig& config) {
    if (PingTypeStr == "kqp") {
        PingType = EPingType::Kqp;
    } else if (PingTypeStr == "kqpsys") {
        PingType = EPingType::KqpSys;
    } else if (PingTypeStr == "plainkqp") {
        PingType = EPingType::PlainKqp;
    } else if (PingTypeStr == "plaingrpc") {
        PingType = EPingType::PlainGrpc;
    } else if (PingTypeStr == "grpcproxy") {
        PingType = EPingType::GrpcProxy;
    } else {
        std::cerr << "Unknown ping type: " << PingTypeStr << std::endl;
        return EXIT_FAILURE;
    }

    return RunCommand(config);
}

int TCommandPing::RunCommand(TConfig& config) {
    TDriver driver = CreateDriver(config);
    NQuery::TQueryClient queryClient(driver);
    NPing::TPingClient pingClient(driver);
    SetInterruptHandlers();

    std::vector<int> durations;
    durations.reserve(Count);
    size_t failedCount = 0;

    TString query;
    if (PingType == EPingType::KqpSys) {
        query = "SELECT * FROM `.sys/nodes` LIMIT 100;";
    } else {
        query = "SELECT 1;";
    }

    for (int i = 0; i < Count && !IsInterrupted(); ++i) {
        bool isOk;
        auto start = NMonotonic::TMonotonic::Now();

        switch (PingType) {
        case EPingType::PlainGrpc:
            isOk = PingPlainGrpc(pingClient);
            break;
        case EPingType::PlainKqp:
            isOk = PingPlainKqp(pingClient);
            break;
        case EPingType::GrpcProxy:
            isOk = PingGrpcProxy(pingClient);
            break;
        case EPingType::Kqp:
            [[fallthrough]];
        case EPingType::KqpSys:
            isOk = PingKqp(queryClient, query);
            break;
        default:
            std::cerr << "Unknown ping type" << std::endl;
            return EXIT_FAILURE;
        }

        auto end = NMonotonic::TMonotonic::Now();
        auto deltaUs = (end - start).MicroSeconds();

        std::cout << i << (isOk ? " ok" : " failed") << " in " << deltaUs << " us" << std::endl;

        if (!isOk) {
            ++failedCount;
        }

        durations.push_back(deltaUs);

        Sleep(DelayBetween);
    }

    std::sort(durations.begin(), durations.end());

    std::cout << std::endl;
    std::cout << "Total: " << durations.size() << ", failed: " << failedCount << std::endl;
    const auto& percentiles = {0.5, 0.9, 0.95, 0.99};

    for (double percentile: percentiles) {
        size_t index = (size_t)(durations.size() * percentile);
        std::cout << (int)(percentile * 100) << "%: "
            << durations[index] << " us" << std::endl;
    }

    return 0;
}

bool TCommandPing::PingPlainGrpc(NPing::TPingClient& client) {
    auto asyncResult = client.PlainGrpcPing(NPing::TPlainGrpcPingSettings());
    asyncResult.GetValueSync();

    return true;
}

bool TCommandPing::PingPlainKqp(NPing::TPingClient& client) {
    auto asyncResult = client.KqpProxyPing(NPing::TKqpProxyPingSettings());
    auto result = asyncResult.GetValueSync();

    if (result.IsSuccess()) {
        return true;
    }

    return false;
}

bool TCommandPing::PingGrpcProxy(NPing::TPingClient& client) {
    auto asyncResult = client.GrpcProxyPing(NPing::TGrpcProxyPingSettings());
    auto result = asyncResult.GetValueSync();

    if (result.IsSuccess()) {
        return true;
    }

    return false;
}

bool TCommandPing::PingKqp(NQuery::TQueryClient& client, const TString& query) {
    // Single stream execution
    NQuery::TExecuteQuerySettings settings;

    // Execute query
    settings.ExecMode(NQuery::EExecMode::Execute);
    settings.StatsMode(NQuery::EStatsMode::None);

    settings.Syntax(NQuery::ESyntax::YqlV1);

    // Execute query without parameters
    auto asyncResult = client.StreamExecuteQuery(
        query,
        NQuery::TTxControl::NoTx(),
        settings
    );

    auto result = asyncResult.GetValueSync();
    while (!IsInterrupted()) {
        auto streamPart = result.ReadNext().GetValueSync();
        if (!streamPart.IsSuccess()) {
            if (streamPart.EOS()) {
                return false;
            }
            return false;
        }

        if (streamPart.HasResultSet()) {
            return true;
        }
    }

    return false;
}

} // NYdb::NConsoleClient
