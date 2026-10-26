LIBRARY()

SRCS(
    actor.cpp
    api.cpp
    projection.cpp
    metrics.cpp
    schema.cpp
)

PEERDIR(
    contrib/libs/openssl
    library/cpp/json
    library/cpp/monlib/dynamic_counters
    library/cpp/string_utils/base64
    library/cpp/time_provider
    library/cpp/svnversion
    ydb/apps/bridge_skipper/config
    ydb/apps/bridge_skipper/domain
    ydb/apps/bridge_skipper/durable
    ydb/apps/bridge_skipper/runtime
    ydb/library/actors/core
    ydb/library/actors/http
)

END()

RECURSE_FOR_TESTS(ut)
