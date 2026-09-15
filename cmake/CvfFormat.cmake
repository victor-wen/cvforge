# cvforwin format-check target.
#
# Runs clang-format in check mode over every project-owned C/C++ source. The
# independent test suites authored and hash-pinned by the test owner under
# tests/abi (CVF-001, D1 precedent) and tests/camera (CVF-002) are deliberately
# excluded: they are recorded in .ai/test-ownership.yaml and reformatting them
# would be an independence violation. They are still compiled by CTest with
# -Wall -Wextra -Werror, so style cannot hide a real diagnostic.
#
# Failures are reported as a diff summary plus a nonzero exit code so the
# canonical command "cmake --build --preset wsl-gcc-debug --target format-check"
# cannot silently succeed on drift.

include_guard(GLOBAL)

function(cvforwin_add_format_check_target)
    find_program(CVFORWIN_CLANG_FORMAT_EXE NAMES clang-format REQUIRED)
    message(STATUS "cvforwin: format-check uses ${CVFORWIN_CLANG_FORMAT_EXE}")

    set(sources "")
    file(GLOB_RECURSE core_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/c_host/*.c"
    )
    list(APPEND sources ${core_sources})

    set(script "${CMAKE_CURRENT_BINARY_DIR}/cmake/CvfFormatCheck.cmake")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CvfFormatCheck.cmake.in" "${script}" @ONLY)

    add_custom_target(format-check
        COMMAND "${CMAKE_COMMAND}"
            -D "CVF_CLANG_FORMAT=${CVFORWIN_CLANG_FORMAT_EXE}"
            -D "CVF_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -D "CVF_FILES=${sources}"
            -P "${script}"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Checking clang-format conformance (tests/abi and tests/camera excluded: test-owner files)"
        VERBATIM
    )
endfunction()
