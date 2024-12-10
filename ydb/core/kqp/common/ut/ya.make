UNITTEST_FOR(ydb/core/kqp/common)

FORK_SUBTESTS()

SIZE(MEDIUM)

SRCS(
    kqp_shared_multitimer_ut.cpp
)

PEERDIR(
    ydb/core/kqp/run_script_actor
    ydb/core/kqp/proxy_service
    ydb/core/kqp/ut/common
    ydb/core/kqp/workload_service/ut/common
    yql/essentials/sql/pg_dummy
    ydb/public/sdk/cpp/client/ydb_query
    ydb/public/sdk/cpp/client/ydb_driver
    ydb/services/ydb
)

YQL_LAST_ABI_VERSION()

END()
