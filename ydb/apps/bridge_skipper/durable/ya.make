LIBRARY()

SRCS(
    codec.cpp
    durable.cpp
    store.cpp
    workflow.cpp
)

PEERDIR(
    contrib/libs/openssl
    contrib/libs/sqlite3
    library/cpp/json
    ydb/apps/bridge_skipper/domain
)

END()

RECURSE_FOR_TESTS(ut)
