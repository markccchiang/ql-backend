cmake_minimum_required(VERSION 3.16)

project(qlservice CXX)

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
#     cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/.local
#
find_package(QuantLib REQUIRED)

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
