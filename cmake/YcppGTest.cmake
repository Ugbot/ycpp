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
    FetchContent_MakeAvailable(googletest)
endif()
