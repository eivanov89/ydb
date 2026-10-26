PY3_PROGRAM(skipper_acceptance)

PY_MAIN(ydb.apps.bridge_skipper.acceptance.main:entry)

PY_SRCS(
    __init__.py
    main.py
)

PEERDIR(ydb/apps/bridge_skipper/acceptance/lib)

END()

RECURSE_FOR_TESTS(tests)
