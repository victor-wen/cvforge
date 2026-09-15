# cvforwin build options and policies.
#
# Included by the root CMakeLists.txt. Every option here has a default that is
# safe for a fresh checkout; presets and CI may override them.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# Project-wide compile policies
# ---------------------------------------------------------------------------

# C++20 for portable code, C11 for the public header's consumers.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The project does not use C++20 modules. Disabling module dependency scanning
# keeps compile commands free of compiler-specific .modmap response files, which
# clang-tidy cannot resolve when reading the compilation database.
set(CMAKE_C_SCAN_FOR_MODULES OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Portable builds must not leak local machine paths (invariant: no hard-coded
# paths). Keep the build deterministic for caching and review stability.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # ProgramDatabase selects the compiler /Zi only; it does not put /debug on
    # the link line, and CMake's Windows-MSVC platform module links /debug for
    # Debug and RelWithDebInfo only. The Release target that package-symbols
    # archives therefore gets an explicit /debug link option and a pinned
    # linker PDB directory in the root CMakeLists.txt (cvforwin, WIN32/MSVC
    # branch); the PDB is never copied into the package ZIP. Requires CMake
    # 3.25+ (the project requires 3.28).
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
endif()

# ---------------------------------------------------------------------------
# cvforwin options
# ---------------------------------------------------------------------------

option(CVFORWIN_BUILD_SHARED "Build the cvforwin shared library / DLL" ON)
option(CVFORWIN_BUILD_TESTS "Build the C++ unit and C ABI test executables" ON)
# Opt-in Windows-only UVC hardware smoke suite (tests/hardware/uvc_smoke.cpp).
# It is never registered with CTest and never part of the mandatory workflows:
# hosted CI has no physical camera, and a designated Windows station runs it
# manually. Ignored on non-Windows builds, where Media Foundation is absent.
option(CVFORWIN_BUILD_HARDWARE_TESTS "Build the opt-in Windows-only UVC hardware smoke suite" OFF)
option(CVFORWIN_WARNINGS_AS_ERRORS "Treat compiler warnings in project targets as errors" ON)
option(CVFORWIN_ENABLE_CLANG_TIDY "Run clang-tidy on project targets at build time" OFF)
option(CVFORWIN_ENABLE_SANITIZERS "Build portable targets with Address/UB sanitizers" OFF)

# The deterministic test camera backends are a development and CI aid. They
# default to ON for the Debug dev presets (wsl-gcc-debug, wsl-gcc-asan,
# wsl-clang-analysis, and a single-config build without an explicit type) and
# to OFF for release configurations, including the multi-config
# windows-msvc-release preset whose CMAKE_BUILD_TYPE is empty at configure time.
# An explicit -DCVFORWIN_BUILD_TEST_BACKENDS=ON/OFF always wins.
set(CVFORWIN_TEST_BACKENDS_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES))
    set(CVFORWIN_TEST_BACKENDS_DEFAULT ON)
endif()
option(CVFORWIN_BUILD_TEST_BACKENDS "Build the deterministic hardware-free test camera backends" ${CVFORWIN_TEST_BACKENDS_DEFAULT})

# The independent ABI tests are supplied by the test owner as source files under
# tests/abi. They are never compiled with the project warning policy because the
# test owner controls that surface; they still must compile as strict C11/C++20.
set(CVFORWIN_ABI_TEST_SOURCES "" CACHE STRING "Explicit tests/abi sources registered with CTest")

# ---------------------------------------------------------------------------
# Helper: create a project target with the canonical warning policy
# ---------------------------------------------------------------------------

function(cvforwin_apply_project_compile_options target)
    if(CVFORWIN_ENABLE_CLANG_TIDY)
        find_program(CVFORWIN_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
        set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CVFORWIN_CLANG_TIDY_EXE}")
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(CVFORWIN_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
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
            -Woverloaded-virtual
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough
            -Wnull-dereference
        )
        if(CVFORWIN_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Helper: warning policy for independently authored test sources
# ---------------------------------------------------------------------------

# The test owner documents -std=c11 -Wall -Wextra -Werror (C11) and
# -std=c++20 -Wall -Wextra -Werror (C++20) as the contract for tests/abi. Use
# exactly that set so an independent test file can never fail because of an
# extra project-specific warning the owner did not agree to.
function(cvforwin_apply_test_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(CVFORWIN_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        if(CVFORWIN_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Helper: place the shared library next to a consumer executable (Windows)
# ---------------------------------------------------------------------------

# The public DLL is emitted into the project's per-configuration bin directory
# (RUNTIME_OUTPUT_DIRECTORY in the root CMakeLists) while executables default to
# the generator's per-configuration directory. Windows resolves a DLL from the
# directory of the loading executable first, so without this copy every test
# executable and example that links the shared library fails to start with
# STATUS_DLL_NOT_FOUND (0xc0000135). Copy the DLL next to the consumer after
# linking. The DLL's published location is deliberately unchanged: the abi-check
# and package/package-verify targets consume it there. On non-Windows builds the
# loader finds the shared object through the build rpath, so this is a no-op.
function(cvforwin_place_shared_library target)
    if(NOT WIN32)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:cvforwin>"
                "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying cvforwin.dll next to ${target}"
        VERBATIM
    )
endfunction()

# ---------------------------------------------------------------------------
# Helper: apply the canonical sanitizer configuration
# ---------------------------------------------------------------------------

function(cvforwin_apply_sanitizers target)
    if(NOT CVFORWIN_ENABLE_SANITIZERS)
        return()
    endif()
    if(MSVC)
        message(FATAL_ERROR "CVFORWIN_ENABLE_SANITIZERS is only supported for portable (GCC/Clang) builds")
    endif()
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endfunction()
