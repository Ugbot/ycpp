# YcppCompileOptions.cmake — reusable compile option helper.
#
# Every COMPILED ycpp target (static lib, tests, tools) calls
# ycpp_apply_hardening() to enforce the warnings-as-errors /
# explicit-everything policy portably across MSVC, clang, and gcc.
#
# Policy:
#  - Warnings:        /W4 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
#  - Exceptions:      ycpp code never throws (every public fn is noexcept).
#                     We do NOT pass /EHsc-none — MSVC stdlib headers
#                     (<atomic>, <chrono>) require unwind semantics at
#                     the ABI level. Discipline is enforced by `noexcept`
#                     on every function, not by the compiler flag.
#  - RTTI:            disabled for compiled targets; consumers keep theirs.
#  - -Werror:         on if YCPP_WARNINGS_AS_ERRORS=ON.
#
# ---------------------------------------------------------------------------
# SCOPE IS ALWAYS PRIVATE. This function is a no-op on INTERFACE libraries.
#
# ycpp is a shipped artifact: `ycpp::headers` is consumed by third parties
# (and, in-tree, by bolt::ybolt). Previously this helper selected INTERFACE
# scope for INTERFACE libraries, which pushed ycpp's ENTIRE diagnostic
# policy — /W4, -Wall -Wextra -Wpedantic -Wshadow -Wconversion, and (because
# YCPP_WARNINGS_AS_ERRORS defaults ON when ycpp is top-level) /WX and -Werror
# — onto every consumer's own translation units. A downstream project would
# then fail to build on OUR warning policy, over ITS own code. That is a
# defect, not a feature: a library may state what it REQUIRES to be compiled
# correctly, never how strictly its consumers must be judged.
#
# The distinction drawn here:
#   * diagnostic flags  (-W*, /W*, -Werror, /WX, /permissive-)  -> compiled
#     targets only. ycpp's own sources are still built under the full policy,
#     so nothing about the library's own quality bar is relaxed.
#   * correctness flags (source encoding, NOMINMAX, language standard) ->
#     these ARE required for the headers to compile correctly in a consumer,
#     so they are declared explicitly as INTERFACE on ycpp_headers at the
#     definition site in CMakeLists.txt, where the justification for each one
#     is visible. See the comment there.
# ---------------------------------------------------------------------------

include_guard(GLOBAL)

function(ycpp_apply_hardening tgt)
    get_target_property(_type ${tgt} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        # Nothing to harden: an INTERFACE library compiles no sources of its
        # own, so every flag we could add here would land on a CONSUMER.
        # See the header comment above.
        return()
    endif()

    if(MSVC)
        target_compile_options(${tgt} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /EHsc
            /utf-8
            /wd4324    # structure padded due to alignas — deliberate (cache-line padding)
            /DNOMINMAX
            /D_CRT_SECURE_NO_WARNINGS
            /GR-
        )
        if(YCPP_WARNINGS_AS_ERRORS)
            target_compile_options(${tgt} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${tgt} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wno-unused-parameter
            -fno-exceptions
            -fno-rtti
        )
        if(YCPP_WARNINGS_AS_ERRORS)
            target_compile_options(${tgt} PRIVATE -Werror)
        endif()
    endif()
endfunction()
