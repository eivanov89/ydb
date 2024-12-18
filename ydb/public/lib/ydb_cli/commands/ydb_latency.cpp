#include "ydb_latency.h"

#include "ydb_ping.h"

#include <ydb/public/sdk/cpp/client/ydb_debug/client.h>
#include <ydb/public/sdk/cpp/client/ydb_query/client.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

namespace NYdb::NConsoleClient {

namespace {

constexpr int DEFAULT_WARMUP_SECONDS = 1;
constexpr int DEFAULT_INTERVAL_SECONDS = 5;
constexpr int DEFAULT_MAX_INFLIGHT = 128;
constexpr TCommandLatency::EFormat DEFAULT_FORMAT = TCommandLatency::EFormat::Plain;
constexpr TCommandPing::EPingType DEFAULT_RUN_TYPE = TCommandPing::EPingType::MaxPingType;

// 1-16, 32, 64, ...
constexpr int INCREMENT_UNTIL_THREAD_COUNT = 16;

const TString QUERY = "SELECT 1;";

// factory returns callable, which makes requests to the DB
using TRequestMaker = std::function<bool()>;
using TCallableFactory = std::function<TRequestMaker()>;

struct TResult {
    const char* Name = nullptr;
    int ThreadCount = 0;
    int LatencyUs = 0;
    int Throughput = 0;
};

struct alignas(64) TEvaluateResult {
    ui64 OkCount = 0;
    ui64 ErrorCount = 0;
};

TEvaluateResult Evaluate(ui64 warmupSeconds, ui64 intervalSeconds, int threadCount, TCallableFactory TCallableFactory) {
    std::atomic<bool> startMeasure{false};
    std::atomic<bool> stop{false};

    auto timer = std::thread([&startMeasure, &stop, warmupSeconds, intervalSeconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(warmupSeconds));
        startMeasure.store(true, std::memory_order_relaxed);

        std::this_thread::sleep_for(std::chrono::seconds(intervalSeconds));
        stop.store(true, std::memory_order_relaxed);
    });

    std::vector<TEvaluateResult> results(threadCount);
    std::vector<std::thread> threads;

    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back([i, &results, &startMeasure, &stop, &TCallableFactory]() {
            TRequestMaker requester;
            try {
                requester = TCallableFactory();
            } catch (yexception ex) {
                std::cerr << "Failed to create request maker: " << ex.what() << std::endl;
                return;
            }

                while (!startMeasure.load(std::memory_order_relaxed)) {
                    try {
                        requester();
                    } catch (...) {
                        continue;
                    }
                }

                while (!stop.load(std::memory_order_relaxed)) {
                    try {
                        if (requester()) {
                            ++results[i].OkCount;
                        } else {
                            ++results[i].ErrorCount;
                        }
                    } catch (...) {
                        ++results[i].ErrorCount;
                    }
                }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    if (timer.joinable()) {
        timer.join();
    }

    TEvaluateResult total;
    for (const auto& result: results) {
        total.OkCount += result.OkCount;
        total.ErrorCount += result.ErrorCount;
    }

    return total;
}

} // anonymous

TCommandLatency::TCommandLatency()
    : TYdbCommand("latency", {}, "check basic latency")
    , IntervalSeconds(DEFAULT_INTERVAL_SECONDS)
    , MaxInflight(DEFAULT_MAX_INFLIGHT)
    , Format(DEFAULT_FORMAT)
    , RunType(DEFAULT_RUN_TYPE)
{}

void TCommandLatency::Config(TConfig& config) {
    TYdbCommand::Config(config);

    config.Opts->AddLongOption(
        'i', "interval", TStringBuilder() << "<interval> s for each latency type, default " << DEFAULT_INTERVAL_SECONDS)
            .RequiredArgument("INTERVAL").StoreResult(&IntervalSeconds);
    config.Opts->AddLongOption(
        'm', "max-inflight", TStringBuilder() << "max inflight, default " << DEFAULT_MAX_INFLIGHT)
            .RequiredArgument("INT").StoreResult(&MaxInflight);
    config.Opts->AddLongOption(
        'f', "format", TStringBuilder() << "output format, default " << DEFAULT_FORMAT)
            .RequiredArgument("STRING").StoreResult(&Format);
    config.Opts->AddLongOption('t', "type", "Only only specified ping type, otherwise all")
        .OptionalArgument("STRING").StoreResult(&RunType);
}

void TCommandLatency::Parse(TConfig& config) {
    TClientCommand::Parse(config);
}

int TCommandLatency::Run(TConfig& config) {
    TDriver driver = CreateDriver(config);

    SetInterruptHandlers();

    std::vector<TResult> results;
    auto debugClient = std::make_shared<NDebug::TDebugClient>(driver);
    auto queryClient = std::make_shared<NQuery::TQueryClient>(driver);

    auto plainGrpcPingFactory = [debugClient] () {
        return [debugClient] () {
            return TCommandPing::PingPlainGrpc(*debugClient);
        };
    };

    auto grpcPingFactory = [debugClient] () {
        return [debugClient] () {
            return TCommandPing::PingGrpcProxy(*debugClient);
        };
    };

    auto plainKqpPingFactory = [debugClient] () {
        return [debugClient] () {
            return TCommandPing::PingPlainKqp(*debugClient);
        };
    };

    auto schemeCachePingFactory = [debugClient] () {
        return [debugClient] () {
            return TCommandPing::PingSchemeCache(*debugClient);
        };
    };

    auto txProxyPingFactory = [debugClient] () {
        return [debugClient] () {
            return TCommandPing::PingTxProxy(*debugClient);
        };
    };

    auto select1Factory = [queryClient] () {
        auto session = std::make_shared<NQuery::TSession>(queryClient->GetSession().GetValueSync().GetSession());
        return [session] () {
            return TCommandPing::PingKqpSelect1(*session, QUERY);
        };
    };

    std::vector<std::pair<const char*, TCallableFactory>> runTasks;

    switch (RunType) {
    case TCommandPing::EPingType::PlainGrpc:
        runTasks = {
            { "PlainGrpc", plainGrpcPingFactory},
        };
        break;
    case TCommandPing::EPingType::GrpcProxy:
        runTasks = {
            { "GrpcProxy", grpcPingFactory},
        };
        break;
    case TCommandPing::EPingType::PlainKqp:
        runTasks = {
            { "PlainKqp", plainKqpPingFactory},
        };
        break;
    case TCommandPing::EPingType::Select1:
        runTasks = {
            { "Select1", select1Factory},
        };
        break;
    case TCommandPing::EPingType::SchemeCache:
        runTasks = {
            { "SchemeCache", schemeCachePingFactory},
        };
        break;
    case TCommandPing::EPingType::TxProxy:
        runTasks = {
            { "TxProxy", txProxyPingFactory},
        };
        break;
    case TCommandPing::EPingType::MaxPingType:
        runTasks = {
            { "PlainGrpc", plainGrpcPingFactory},
            { "GrpcProxy", grpcPingFactory},
            { "PlainKqp", plainKqpPingFactory},
            { "SchemeCache", schemeCachePingFactory},
            { "TxProxy", txProxyPingFactory},
            { "Select1", select1Factory},
        };
        break;
    }

    for (const auto& [name, factory]: runTasks) {
        for (int threadCount = 1; threadCount <= MaxInflight && !IsInterrupted(); ) {
            auto result = Evaluate(DEFAULT_WARMUP_SECONDS, IntervalSeconds, threadCount, factory);

            if (result.ErrorCount) {
                auto totalRequests = result.ErrorCount + result.OkCount;
                double errorsPercent = 100.0 * result.ErrorCount / totalRequests;
                if (errorsPercent >= 1) {
                    std::cerr << "Skipping " << name << ", threads=" << threadCount
                        << ": error rate=" << errorsPercent << "%" << std::endl;
                    continue;
                }
            }

            ui64 throughput = result.OkCount / IntervalSeconds;
            ui64 throughputPerThread = throughput / threadCount;
            ui64 latencyUsec = 1'000'000 / throughputPerThread;

            results.emplace_back(name, threadCount, latencyUsec, throughput);

            std::cout << name << " threads=" << threadCount
                << ", throughput: " << throughput
                << ", per thread: " << throughputPerThread
                << ", avg latency usec: " << latencyUsec
                << ", ok: " << result.OkCount
                << ", error: " << result.ErrorCount << std::endl;

            if (threadCount < INCREMENT_UNTIL_THREAD_COUNT) {
                ++threadCount;
            } else {
                threadCount *= 2;
            }
        }
    }

    if (Format == EFormat::Plain) {
        return 0;
    }

    THashMap<const char*, std::vector<int>> latencies;
    THashMap<const char*, std::vector<int>> throughputs;
    for (const auto& result: results) {
        latencies[result.Name].push_back(result.LatencyUs);
        throughputs[result.Name].push_back(result.Throughput);
    }

    if (Format == EFormat::CSV) {
        const int maxThreadsMeasured = results.back().ThreadCount;

        std::cout << std::endl;
        std::cout << "Latencies" << std::endl;
        std::cout << "Name";
        for (int i = 1; i <= maxThreadsMeasured;) {
            std::cout << "," << i;
            if (i < INCREMENT_UNTIL_THREAD_COUNT) {
                ++i;
            } else {
                i *= 2;
            }
        }
        std::cout << std::endl;
        for (const auto& [name, vec]: latencies) {
            std::cout << name;
            for (auto value: vec) {
                std::cout << "," << value;
            }
            std::cout << std::endl;
        }

        std::cout << std::endl;
        std::cout << "Througputs" << std::endl;
        std::cout << "Name";
        for (int i = 1; i <= maxThreadsMeasured;) {
            std::cout << "," << i;
            if (i < INCREMENT_UNTIL_THREAD_COUNT) {
                ++i;
            } else {
                i *= 2;
            }
        }
        std::cout << std::endl;
        for (const auto& [name, vec]: throughputs) {
            std::cout << name;
            for (auto value: vec) {
                std::cout << "," << value;
            }
            std::cout << std::endl;
        }
    }

    return 0;
}

} // NYdb::NConsoleClient
