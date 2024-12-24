UNITTEST_FOR(ydb/core/kqp)

FORK_SUBTESTS()

SPLIT_FACTOR(5)
SIZE(MEDIUM)

SRCS(
    fast_query_ut.cpp
)

PEERDIR(
    ydb/core/kqp
    library/cpp/testing/common
    ydb/core/testlib
    ydb/core/kqp/ut/common
    yql/essentials/parser/pg_wrapper
    yql/essentials/sql/pg
)

YQL_LAST_ABI_VERSION()

END()
