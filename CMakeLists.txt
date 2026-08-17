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

# Install rules exist only when ycpp is the thing being built and shipped.
# A consumer that add_subdirectory()s ycpp gets targets, not install rules —
# otherwise `cmake --install` on the consumer would scatter ycpp's headers and
# archive into the consumer's prefix as an unasked-for side effect.
option(YCPP_INSTALL "Generate ycpp install + export rules" ${YCPP_IS_TOPLEVEL})

# A scikit-build-core wheel build must ship the Python extension module and
# nothing else; a static archive + headers dropped into the wheel's data dir
# would be dead weight at best. SKBUILD is set by scikit-build{,-core}.
if(SKBUILD)
    set(YCPP_INSTALL OFF)
endif()

# --- language standard -------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(YcppCompileOptions)

# GNUInstallDirs is included unconditionally (not under YCPP_INSTALL) because
# CMAKE_INSTALL_INCLUDEDIR is referenced in ycpp_headers' INSTALL_INTERFACE
# below, which must be correct before any install() rule runs. It only defines
# cache variables; it has no other effect on a consumer's build.
include(GNUInstallDirs)

# --- ycpp::headers ── pure-header INTERFACE target -----------------------------
add_library(ycpp_headers INTERFACE)
add_library(ycpp::headers ALIAS ycpp_headers)
target_include_directories(ycpp_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features(ycpp_headers INTERFACE cxx_std_20)

# ycpp_apply_hardening() is deliberately NOT called on this target: it is a
# no-op on INTERFACE libraries, because every flag it adds would land on a
# CONSUMER's translation units rather than on ours (see cmake/
# YcppCompileOptions.cmake). What follows is the short list of flags the
# HEADERS genuinely need in order to compile correctly downstream — each one
# justified, none of them diagnostic policy:
#
#   NOMINMAX  — ycpp_id.h writes `std::numeric_limits<uint64_t>::max()`
#               unparenthesised. If a consumer has already included
#               <windows.h> without NOMINMAX, `max(` expands as a macro and
#               the header fails to parse. Required for correctness.
#   /utf-8    — every public header carries UTF-8 bytes (in comments: ≥, ─,
#               …). Without /utf-8, MSVC decodes them in the system ANSI
#               codepage, where a lead byte can swallow the following
#               character — a real miscompile hazard, not just a warning.
#
# Deliberately NOT propagated: /permissive- (only makes a consumer stricter;
# our headers parse either way), /Zc:__cplusplus (no header tests
# __cplusplus), /Zc:preprocessor (no variadic macros), /EHsc + /GR- +
# -fno-exceptions + -fno-rtti (those are a consumer's ABI choice to make).
if(MSVC)
    target_compile_definitions(ycpp_headers INTERFACE NOMINMAX)
    target_compile_options(ycpp_headers INTERFACE /utf-8)
endif()

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

# --- one vocabulary in BOTH resolution modes ---------------------------------
# The in-tree spellings are the ALIASes above: ycpp::headers and ycpp::ycpp.
# install(EXPORT ... NAMESPACE ycpp::) does NOT know about aliases — it builds
# imported-target names from NAMESPACE + the target's EXPORT_NAME, which
# defaults to the raw target name. Without these two properties an installed
# ycpp would hand consumers `ycpp::ycpp_headers` / `ycpp::ycpp_static`, so a
# CMakeLists written against add_subdirectory() would fail to configure against
# find_package() and vice versa. Setting EXPORT_NAME makes the two modes agree
# exactly, which is the whole point of shipping a package.
set_target_properties(ycpp_headers PROPERTIES EXPORT_NAME headers)
set_target_properties(ycpp_static  PROPERTIES EXPORT_NAME ycpp)

# --- tests + benchmarks ------------------------------------------------------
if(YCPP_BUILD_TESTS)
    enable_testing()
    include(YcppGTest)
    add_subdirectory(tests)
endif()

# benchmarks/ is not in the tree yet (the perf gates against y-crdt are a later
# wave). Guard on EXISTS so -DYCPP_BUILD_BENCHMARKS=ON configures cleanly today
# and picks the directory up automatically the moment it lands, rather than
# hard-failing on a directory that was never committed.
if(YCPP_BUILD_BENCHMARKS)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt")
        add_subdirectory(benchmarks)
    else()
        message(STATUS "ycpp: YCPP_BUILD_BENCHMARKS=ON but benchmarks/ is absent — skipping")
    endif()
endif()

if(YCPP_BUILD_INTEROP)
    add_subdirectory(tools/yjs_interop)
endif()

# --- install + export --------------------------------------------------------
# Makes ycpp consumable via find_package(ycpp) with the SAME target spellings
# an add_subdirectory() consumer sees: ycpp::ycpp and ycpp::headers.
if(YCPP_INSTALL)
    include(CMakePackageConfigHelpers)

    set(YCPP_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/ycpp")

    # Both targets go into ONE export set. ycpp_static links ycpp::headers
    # PUBLIC, so ycpp_headers MUST be exported alongside it or install(EXPORT)
    # errors out with "requires target ycpp_headers that is not in any export
    # set" — and a consumer would be missing the include path regardless.
    install(TARGETS ycpp_headers ycpp_static
        EXPORT  ycpp-targets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )

    # Headers are shipped as the whole ycpp/ directory: they are top-level
    # under include/ and prefixed (ycpp_arena.h, ycpp_doc.h), so a consumer's
    # #include <ycpp/ycpp.h> resolves unambiguously.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/ycpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    install(EXPORT ycpp-targets
        FILE        ycpp-targets.cmake
        NAMESPACE   ycpp::
        DESTINATION "${YCPP_CMAKE_INSTALL_DIR}"
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ycpp-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ycpp-config.cmake"
        INSTALL_DESTINATION "${YCPP_CMAKE_INSTALL_DIR}"
    )

    # SameMajorVersion: ycpp is pre-1.0, so this is the honest contract today —
    # 0.x carries no compatibility promise beyond the exact major. Revisit when
    # the wire format is frozen and 1.0 ships.
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ycpp-config-version.cmake"
        VERSION       ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ycpp-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ycpp-config-version.cmake"
        DESTINATION "${YCPP_CMAKE_INSTALL_DIR}"
    )
endif()

# --- summary line ------------------------------------------------------------
if(YCPP_IS_TOPLEVEL)
    message(STATUS "ycpp ${PROJECT_VERSION} — toplevel")
    message(STATUS "  tests:      ${YCPP_BUILD_TESTS}")
    message(STATUS "  benchmarks: ${YCPP_BUILD_BENCHMARKS}")
    message(STATUS "  -Werror:    ${YCPP_WARNINGS_AS_ERRORS}")
    message(STATUS "  install:    ${YCPP_INSTALL}")
endif()
