LIBRARY()

PEERDIR(
    library/cpp/testing/unittest
    ydb/core/base
    ydb/core/blockstore/core
    ydb/core/cms/console
    ydb/core/engine
    ydb/core/engine/minikql
    ydb/core/filestore/core
    ydb/core/metering
    ydb/core/persqueue/ut/common
    ydb/core/protos
    ydb/core/scheme
    ydb/core/tablet_flat
    ydb/core/testlib
    ydb/core/tx
    ydb/core/tx/datashard
    ydb/core/tx/schemeshard
    ydb/core/tx/tx_allocator
    ydb/core/tx/tx_proxy
    ydb/public/lib/scheme_types
    yql/essentials/public/issue
    ydb/public/lib/deprecated/kicli
    ydb/public/sdk/cpp/src/client/driver
    ydb/public/sdk/cpp/src/client/table
)

SRCS(
    export_reboots_common.cpp
    failing_mtpq.cpp
    helpers.cpp
    helpers.h
    ls_checks.cpp
    ls_checks.h
    shred_helpers.cpp
    test_env.cpp
    test_env.h
    test_with_reboots.h
)

YQL_LAST_ABI_VERSION()

END()
