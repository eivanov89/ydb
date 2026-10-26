PY3TEST()

SIZE(MEDIUM)

FORK_SUBTESTS()
# One process per scenario retains the full MEDIUM timeout and the required
# >=20-second BSC warmup. Do not combine cases into cumulative chunks.
SPLIT_FACTOR(10)

TEST_SRCS(
    test_e2e.py
)

DEPENDS(
    ydb/apps/bridge_skipper
)

PEERDIR(
    library/python/testing/yatest_common
    ydb/apps/bridge_skipper/acceptance/lib
)

END()
