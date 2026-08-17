# YcppGTest.cmake — locate or fetch GoogleTest.
#
# Prefers a system gtest (find_package CONFIG QUIET) to keep the build
# offline-capable. Falls back to FetchContent only when nothing is found.
# This is the ONLY external dependency in ycpp, and only when tests are on.

include_guard(GLOBAL)

find_package(GTest CONFIG QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    # Match the MSVC runtime of our own code (dynamic CRT).
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    # Do NOT let the fetched GoogleTest add its own install rules. Without
    # this, `cmake --install` on a tests-enabled ycpp build drops gtest's
    # headers, four static archives, lib/cmake/GTest/ and four .pc files into
    # the caller's prefix — ycpp would be shipping somebody else's library
    # under its own install, and could silently shadow a system GoogleTest.
    # gtest is a test-only dependency; it has no business in ycpp's package.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)
endif()
