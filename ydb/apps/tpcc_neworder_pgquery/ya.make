SUBSCRIBER(g:kikimr)

PROGRAM()

SET(USE_VANILLA_PROTOC "yes")

CFLAGS(
    -Wno-unknown-warning-option
    -Wno-unused-parameter
    -Wno-unused-private-field
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-ignored-qualifiers
    -Wno-misleading-indentation
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-unused-but-set-variable
)

SRCS(
   main.cpp
)

PEERDIR(
    library/cpp/getopt
    library/cpp/histogram/hdr
    library/cpp/string_utils/url
    ydb/public/sdk/cpp/client/ydb_table

    contrib/libs/libpg_query
)

END()
