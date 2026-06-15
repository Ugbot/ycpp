# ycpp — a Tiger-Style, dependency-free C++20 Yjs CRDT runtime.
#
# Standalone build (toplevel): tests + benchmarks ON by default.
# Submoduled build (consumer):  everything OFF; consumer pulls ycpp::ycpp
#                               (or ycpp::headers) into its own graph.
#
# No external runtime dependencies. GoogleTest is FetchContent'd ONLY when
# YCPP_BUILD_TESTS=ON.

cmake_minimum_required(VERSION 3.21)

project(ycpp
    VERSION     0.0.1
    DESCRIPTION "Tiger-Style, dependency-free C++20 Yjs CRDT runtime"
    LANGUAGES   CXX
)

# --- toplevel detection ------------------------------------------------------
if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(YCPP_IS_TOPLEVEL ON)
else()
    set(YCPP_IS_TOPLEVEL OFF)
endif()

option(YCPP_BUILD_TESTS         "Build ycpp tests"               ${YCPP_IS_TOPLEVEL})
option(YCPP_BUILD_BENCHMARKS    "Build ycpp benchmarks"          OFF)
option(YCPP_BUILD_INTEROP       "Build Yjs interop CLI"          ${YCPP_IS_TOPLEVEL})
option(YCPP_WARNINGS_AS_ERRORS  "Treat compile warnings as errors" ${YCPP_IS_TOPLEVEL})

# --- language standard -------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(YcppCompileOptions)

# --- ycpp::headers ── pure-header INTERFACE target -----------------------------
add_library(ycpp_headers INTERFACE)
add_library(ycpp::headers ALIAS ycpp_headers)
target_include_directories(ycpp_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_compile_features(ycpp_headers INTERFACE cxx_std_20)
ycpp_apply_hardening(ycpp_headers)

# --- ycpp::ycpp ── compiled STATIC library (filled out from W2 onwards) --------
# W1 ships an empty .cpp so the static target exists from the start; later
# waves add per-module TUs (struct store, integration, encoders, types …).
add_library(ycpp_static STATIC
    src/ycpp_placeholder.cpp
)
add_library(ycpp::ycpp ALIAS ycpp_static)
target_link_libraries(ycpp_static PUBLIC ycpp::headers)
target_compile_features(ycpp_static PUBLIC cxx_std_20)
ycpp_apply_hardening(ycpp_static)
set_target_properties(ycpp_static PROPERTIES
    OUTPUT_NAME ycpp
    POSITION_INDEPENDENT_CODE ON
)

# --- tests + benchmarks ------------------------------------------------------
if(YCPP_BUILD_TESTS)
    enable_testing()
    include(YcppGTest)
    add_subdirectory(tests)
endif()

if(YCPP_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(YCPP_BUILD_INTEROP)
    add_subdirectory(tools/yjs_interop)
endif()

# --- summary line ------------------------------------------------------------
if(YCPP_IS_TOPLEVEL)
    message(STATUS "ycpp ${PROJECT_VERSION} — toplevel")
    message(STATUS "  tests:      ${YCPP_BUILD_TESTS}")
    message(STATUS "  benchmarks: ${YCPP_BUILD_BENCHMARKS}")
    message(STATUS "  -Werror:    ${YCPP_WARNINGS_AS_ERRORS}")
endif()
