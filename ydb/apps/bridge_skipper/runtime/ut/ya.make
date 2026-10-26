UNITTEST()

SRCS(
    controller_ut.cpp
    gateway_ut.cpp
    resources_ut.cpp
)

PEERDIR(
    ydb/apps/bridge_skipper/runtime
    ydb/apps/bridge_skipper/http
    ydb/library/actors/testlib
)

END()
