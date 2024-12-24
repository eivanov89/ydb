LIBRARY()

SRCS(
    kqp_compile_actor.cpp
    kqp_compile_service.cpp
    kqp_compile_computation_pattern_service.cpp
    kqp_fast_query.cpp
)

PEERDIR(
    ydb/core/actorlib_impl
    ydb/core/base
    ydb/core/kqp/common/simple
    ydb/core/kqp/federated_query
    ydb/core/kqp/host
    ydb/core/ydb_convert
    yql/essentials/parser/pg_wrapper
    yql/essentials/sql/pg
)

ADDINCL(
    yql/essentials/parser/pg_wrapper/postgresql/src/include
)

YQL_LAST_ABI_VERSION()

GENERATE_ENUM_SERIALIZATION(kqp_fast_query.h)

END()
