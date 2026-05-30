# CompilerWarnings.cmake
# Provides hft_set_warnings(<target>) — applies a strict, portable warning set.
# Warnings are intentionally NOT errors at the skeleton stage; flip to -Werror later.

function(hft_set_warnings target)
    if(NOT HFT_ENABLE_WARNINGS)
        return()
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Wunused
            -Wdouble-promotion)
    endif()

    if(HFT_NATIVE_ARCH AND NOT MSVC)
        target_compile_options(${target} PRIVATE -march=native)
    endif()
endfunction()
