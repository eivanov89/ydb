PY3TEST()

SIZE(MEDIUM)

FORK_SUBTESTS()
# Each process-backed test keeps the medium timeout; avoid cumulative chunks
# (the three-reply-loss case alone measured 53 seconds).
SPLIT_FACTOR(13)

TEST_SRCS(
    test_acceptance.py
    test_report.py
)

DEPENDS(
    ydb/apps/bridge_skipper
    ydb/apps/bridge_skipper/ydbd_mock
)

PEERDIR(
    library/python/testing/yatest_common
    ydb/apps/bridge_skipper/acceptance/lib
)

END()
