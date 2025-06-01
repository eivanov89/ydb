PROGRAM(grpc_ping_client)

IF(BUILD_TYPE == RELEASE)
#    STRIP()
ENDIF()

SRCS(
    grpc_ping_client.cpp
)

ADDINCL(
    contrib/libs/grpc
    contrib/libs/grpc/include
)

PEERDIR(
    ydb/public/api/grpc
    ydb/public/api/protos
    contrib/libs/grpc
)

IF (NOT USE_SSE4 AND NOT OPENSOURCE)
    # contrib/libs/glibasm can not be built without SSE4
    # Replace it with contrib/libs/asmlib which can be built this way.
    DISABLE(USE_ASMLIB)
    PEERDIR(
        contrib/libs/asmlib
    )
ENDIF()

END()
