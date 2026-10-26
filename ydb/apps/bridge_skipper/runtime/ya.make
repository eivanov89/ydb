LIBRARY()

SRCS(
    controller.cpp
    gateway.cpp
    resources.cpp
    log_writer.cpp
)

PEERDIR(
    library/cpp/json
    library/cpp/monlib/dynamic_counters
    library/cpp/time_provider
    ydb/apps/bridge_skipper/config
    ydb/apps/bridge_skipper/domain
    ydb/apps/bridge_skipper/durable
    ydb/library/actors/core
    ydb/public/api/protos
    ydb/public/sdk/cpp/src/client/config
    ydb/public/sdk/cpp/src/client/discovery
    ydb/public/sdk/cpp/src/client/draft
    ydb/public/sdk/cpp/src/client/monitoring
    ydb/public/sdk/cpp/src/client/proto
)

END()

RECURSE_FOR_TESTS(ut)
