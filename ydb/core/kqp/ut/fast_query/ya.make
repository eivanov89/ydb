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

    # not needed, but linked fails otherwise
    ydb/core/kqp/ut/common
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

END()
