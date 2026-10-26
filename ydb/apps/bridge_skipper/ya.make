PROGRAM(bridge_skipper)

SRCS(main.cpp)

PEERDIR(
    library/cpp/json
    library/cpp/svnversion
    library/cpp/time_provider
    ydb/apps/bridge_skipper/config
    ydb/apps/bridge_skipper/http
    ydb/apps/bridge_skipper/runtime
    ydb/library/actors/core
    ydb/library/actors/http
    ydb/library/actors/interconnect
)

END()

RECURSE(
    acceptance
    config
    domain
    durable
    http
    runtime
    ydbd_mock
)
