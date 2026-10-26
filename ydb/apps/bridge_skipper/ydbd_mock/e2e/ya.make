PY3TEST()

SIZE(LARGE)

TAG(ya:fat)

FORK_SUBTESTS()
SPLIT_FACTOR(4)

TEST_SRCS(
    test_cli.py
    test_demo_compat.py
    test_launcher.py
    test_supervisor.py
)

PY_SRCS(
    TOP_LEVEL
    harness.py
)

DATA(
    arcadia/ydb/apps/bridge_skipper/ydbd_mock/scripts
    arcadia/ydb/apps/bridge_skipper_demo
)

DEPENDS(
    ydb/apps/bridge_skipper
    ydb/apps/bridge_skipper/ydbd_mock
)

PEERDIR(
    library/python/testing/yatest_common
    ydb/apps/bridge_skipper/ydbd_mock/lib
)

END()
