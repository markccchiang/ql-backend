cmake_minimum_required(VERSION 3.16)

# C is enabled for uSockets, the C library under uWebSockets, which is built
# from the submodule rather than linked as a prebuilt archive.
project(ql-backend C CXX)

# C++17 is QuantLib's baseline (QuantLib AGENTS.md §1); staying on it keeps a
# worker binary compilable against the same library the rest of the stack uses.
if (NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
#
# QuantLib is found through the package config it installs, so the include and
# link paths come from the install tree rather than being guessed here. Point
# CMAKE_PREFIX_PATH at a non-standard prefix, e.g.
#
#     cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/.local/quantlib-sessions
#
# That prefix has to be a QuantLib built with QL_ENABLE_SESSIONS (DESIGN §2) and
# without OpenMP (DESIGN §2.2), which nothing in this build can check: the wrong
# one links and prices, and only test/smoke_sessions.py notices. Vendoring turns
# both of those from environmental facts into build settings.

option(QLSERVICE_VENDOR_QUANTLIB
    "Build QuantLib from third_party/QuantLib instead of finding an installed one" OFF)

set(QLSERVICE_QUANTLIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QuantLib")

if (QLSERVICE_VENDOR_QUANTLIB)

    if (NOT EXISTS "${QLSERVICE_QUANTLIB}/ql/quantlib.hpp")
        message(FATAL_ERROR
            "QLSERVICE_VENDOR_QUANTLIB is ON but third_party/QuantLib is empty. "
            "Run: git submodule update --init --checkout third_party/QuantLib")
    endif()

    # The two settings this repo cannot be built correctly without. FORCE
    # because QuantLib declares them with option(), which would otherwise keep
    # whatever is already in the cache.
    set(QL_ENABLE_SESSIONS ON CACHE BOOL "" FORCE)   # DESIGN §2
    set(QL_ENABLE_OPENMP OFF CACHE BOOL "" FORCE)    # DESIGN §2.2

    # Both default ON, and the test suite pulls in Boost.Test. With them off
    # the only target QuantLib adds is the library itself.
    set(QL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(QL_BUILD_TEST_SUITE OFF CACHE BOOL "" FORCE)
    set(QL_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(QL_INSTALL_TEST_SUITE OFF CACHE BOOL "" FORCE)
    set(QL_INSTALL_BENCHMARK OFF CACHE BOOL "" FORCE)

    # QuantLib's CMakeLists has no top-level guard, so its install() rules
    # become part of this project's install as well. Harmless here — nothing
    # installs qlservice — and preferable to EXCLUDE_FROM_ALL, which leaves
    # install rules on an excluded directory.
    add_subdirectory("${QLSERVICE_QUANTLIB}")

    # In-tree the target is ql_library; the QuantLib:: namespace is applied by
    # install(EXPORT ...) and so exists only for an installed QuantLib. The
    # alias lets the rest of this file name the dependency one way.
    add_library(QuantLib::QuantLib ALIAS ql_library)

    # An imported QuantLib arrives with system includes, an in-tree one does
    # not, and QuantLib's headers do not compile clean under this project's
    # -Wall -Wextra -Wpedantic. Without this the vendored build is 15 warnings
    # louder than the installed one and QLSERVICE_WERROR fails on someone
    # else's code. (add_subdirectory(SYSTEM) would do the same but needs 3.25.)
    set_target_properties(ql_library PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
            "$<TARGET_PROPERTY:ql_library,INTERFACE_INCLUDE_DIRECTORIES>")

else()

    find_package(QuantLib REQUIRED)

endif()

# CONFIG mode: protobuf's own package config carries the abseil dependencies
# that libprotobuf now needs, which the deprecated FindProtobuf module does not.
find_package(Protobuf CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Generated protobuf sources
# ---------------------------------------------------------------------------
#
# The generated headers are included as "quantlib/v1/envelope.pb.h", so the
# output root — not the package directory — goes on the include path and the
# proto's own package path is preserved underneath it.
set(QLSERVICE_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${QLSERVICE_PROTO_OUT}")

add_library(qlservice_proto OBJECT
    proto/quantlib/v1/conventions.proto
    proto/quantlib/v1/envelope.proto

    # v2 is the generalized schema (DESIGN 6.3). No handler serves it yet;
    # it is built so that protoc and the C++ compiler check it, which is the
    # only verification this repository has.
    proto/quantlib/v2/market.proto
    proto/quantlib/v2/instrument.proto
    proto/quantlib/v2/engine.proto
    proto/quantlib/v2/results.proto
    proto/quantlib/v2/envelope.proto
)

protobuf_generate(
    TARGET qlservice_proto
    IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/proto"
    PROTOC_OUT_DIR "${QLSERVICE_PROTO_OUT}"
)

target_include_directories(qlservice_proto PUBLIC "${QLSERVICE_PROTO_OUT}")
target_link_libraries(qlservice_proto PUBLIC protobuf::libprotobuf)

# Generated code is not ours to lint, and protobuf 34 emits deprecation
# warnings from its own headers under -Wall.
target_compile_options(qlservice_proto PRIVATE -w)

# ---------------------------------------------------------------------------
# The service library
# ---------------------------------------------------------------------------
#
# A library rather than an executable: the gateway that drives it, the process
# host that forks workers, and the worker main() are not written yet
# (README §1.1). Building this compiles every translation unit that is.

add_library(qlservice STATIC
    src/conventions/registry.cpp
    src/session/session.cpp
    src/session/supervisor.cpp
    src/session/worker.cpp
)

target_include_directories(qlservice PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(qlservice PUBLIC qlservice_proto QuantLib::QuantLib)

target_compile_options(qlservice PRIVATE
    -Wall -Wextra -Wpedantic
    # Every proto3 enum reserves 0 as *_UNSPECIFIED and the registry rejects
    # it, so a switch that omits it is deliberate rather than an oversight.
    -Wno-switch
)

option(QLSERVICE_WERROR "Treat warnings as errors" OFF)
if (QLSERVICE_WERROR)
    target_compile_options(qlservice PRIVATE -Werror)
endif()

# ---------------------------------------------------------------------------
# uWebSockets and uSockets
# ---------------------------------------------------------------------------
#
# Vendored as a submodule at third_party/uWebSockets, with uSockets nested
# inside it, so a clone plus
#
#     git submodule update --init --recursive
#
# is everything the gateway needs. uWebSockets installs nothing and has no
# package config: the C++ layer is headers only and the C library underneath it
# is built from source here.

set(QLSERVICE_UWS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/uWebSockets")

if (EXISTS "${QLSERVICE_UWS}/uSockets/src/libusockets.h")

    # What uSockets' own Makefile compiles, minus the translation units that
    # are empty in this configuration. Globbed rather than listed so a version
    # bump does not silently drop a new file.
    file(GLOB QLSERVICE_USOCKETS_SOURCES CONFIGURE_DEPENDS
        "${QLSERVICE_UWS}/uSockets/src/*.c"
        "${QLSERVICE_UWS}/uSockets/src/eventing/*.c"
        "${QLSERVICE_UWS}/uSockets/src/crypto/*.c"
    )

    # Each of these compiles to nothing here and ranlib warns about the empty
    # archive members: openssl.c is behind LIBUS_NO_SSL, quic.c needs
    # LIBUS_USE_QUIC, and gcd.c and libuv.c are the eventing backends this
    # build does not select — epoll_kqueue.c is the one that has content on
    # both macOS and Linux. src/crypto/sni_tree.cpp (SSL) and src/io_uring
    # (Linux) are outside the globs above for the same reason.
    list(REMOVE_ITEM QLSERVICE_USOCKETS_SOURCES
        "${QLSERVICE_UWS}/uSockets/src/crypto/openssl.c"
        "${QLSERVICE_UWS}/uSockets/src/quic.c"
        "${QLSERVICE_UWS}/uSockets/src/eventing/gcd.c"
        "${QLSERVICE_UWS}/uSockets/src/eventing/libuv.c"
    )

    add_library(usockets STATIC ${QLSERVICE_USOCKETS_SOURCES})

    set_target_properties(usockets PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

    # PUBLIC, and it matters: LIBUS_NO_SSL changes the layout of structs that
    # uWebSockets' headers also see, so the library and everything including
    # libusockets.h have to agree on it.
    target_compile_definitions(usockets PUBLIC LIBUS_NO_SSL)

    target_include_directories(usockets SYSTEM PUBLIC "${QLSERVICE_UWS}/uSockets/src")

    # Third-party code, not ours to lint.
    target_compile_options(usockets PRIVATE -w)

    # -----------------------------------------------------------------------
    # The gateway executable
    # -----------------------------------------------------------------------

    add_executable(ql-backend
        src/app/main.cpp
        src/gateway/gateway.cpp
        src/gateway/threadhost.cpp
    )

    target_include_directories(ql-backend SYSTEM PRIVATE "${QLSERVICE_UWS}/src")

    # No permessage-deflate: the frames are already compact protobuf, and it
    # drops the zlib dependency.
    target_compile_definitions(ql-backend PRIVATE UWS_NO_ZLIB)

    target_link_libraries(ql-backend PRIVATE qlservice usockets)

    # -Wpedantic is left off here alone: uWebSockets' templates are
    # instantiated in this target and are not written to it.
    target_compile_options(ql-backend PRIVATE -Wall -Wextra -Wno-switch)

else()

    message(STATUS
        "third_party/uWebSockets is empty, so the ql-backend target is skipped. "
        "Run: git submodule update --init --recursive")

endif()
