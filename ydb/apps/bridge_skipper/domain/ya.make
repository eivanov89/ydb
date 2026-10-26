LIBRARY()

SRCS(
    clock.cpp
    types.cpp
    evidence.cpp
    policy.cpp
    scheduler.cpp
    topology.cpp
)

PEERDIR(
    library/cpp/time_provider
    contrib/libs/yaml-cpp
    contrib/libs/openssl
    ydb/apps/bridge_skipper/config
)

END()

RECURSE_FOR_TESTS(
    ut
)
