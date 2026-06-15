# YcppCompileOptions.cmake — reusable compile option helper.
#
# Every ycpp::* target calls ycpp_apply_hardening() to enforce the
# warnings-as-errors / explicit-everything policy portably across
# MSVC, clang, and gcc.
#
# Policy:
#  - Warnings:        /W4 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
#  - Exceptions:      ycpp code never throws (every public fn is noexcept).
#                     We do NOT pass /EHsc-none — MSVC stdlib headers
#                     (<atomic>, <chrono>) require unwind semantics at
#                     the ABI level. Discipline is enforced by `noexcept`
#                     on every function, not by the compiler flag.
#  - RTTI:            disabled for PRIVATE (compiled) targets; INTERFACE
#                     targets leave RTTI to the consumer. Mirrors bolt.
#  - -Werror:         on if YCPP_WARNINGS_AS_ERRORS=ON.

include_guard(GLOBAL)

function(ycpp_apply_hardening tgt)
    get_target_property(_type ${tgt} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        set(_scope INTERFACE)
    else()
        set(_scope PRIVATE)
    endif()

    if(MSVC)
        target_compile_options(${tgt} ${_scope}
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /EHsc
            /utf-8
            /wd4324    # structure padded due to alignas — deliberate (cache-line padding)
            /DNOMINMAX
            /D_CRT_SECURE_NO_WARNINGS
        )
        if(NOT _scope STREQUAL "INTERFACE")
            target_compile_options(${tgt} PRIVATE /GR-)
        endif()
        if(YCPP_WARNINGS_AS_ERRORS)
            target_compile_options(${tgt} ${_scope} /WX)
        endif()
    else()
        target_compile_options(${tgt} ${_scope}
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wno-unused-parameter
        )
        if(NOT _scope STREQUAL "INTERFACE")
            target_compile_options(${tgt} PRIVATE -fno-exceptions -fno-rtti)
        endif()
        if(YCPP_WARNINGS_AS_ERRORS)
            target_compile_options(${tgt} ${_scope} -Werror)
        endif()
    endif()
endfunction()
