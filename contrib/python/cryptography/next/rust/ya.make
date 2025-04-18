# Generated by devtools/yamaker/ym2

PY3_LIBRARY()

LICENSE(
    Apache-2.0 AND
    BSD-3-Clause
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

SUBSCRIBER(
    g:cpp-contrib
    g:devtools-contrib
)

VERSION(41.0.6)

ORIGINAL_SOURCE(https://github.com/pyca/cryptography/archive/refs/tags/41.0.6.tar.gz)

IF (FALSE)
ELSEIF (OS_LINUX AND ARCH_X86_64 AND NOT MUSL)
    COPY_FILE(a/x86_64-unknown-linux-gnu/release/libcryptography_rust.a libcryptography_rust.a)
ELSEIF (OS_LINUX AND ARCH_X86_64 AND MUSL)
    COPY_FILE(a/x86_64-unknown-linux-musl/release/libcryptography_rust.a libcryptography_rust.a)
ELSE()
    MESSAGE(FATAL_ERROR "We don't have cryptography_rust for this platform")
ENDIF()

PY_REGISTER(
    _rust
    _openssl
)

NO_COMPILER_WARNINGS()

NO_UTIL()

END()
