# cvforwin static-analysis target (clang-tidy over project sources).
#
# The canonical command is
#   cmake --preset wsl-clang-analysis && cmake --build --preset wsl-clang-analysis --target static-analysis --parallel
# The preset configures a build tree with CMAKE_EXPORT_COMPILE_COMMANDS so this
# target can hand clang-tidy a compilation database. Any finding is an error.

include_guard(GLOBAL)

function(cvforwin_add_static_analysis_target)
    find_program(CVFORWIN_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    get_filename_component(tidy_dir "${CVFORWIN_CLANG_TIDY_EXE}" DIRECTORY)
    find_program(CVFORWIN_RUN_CLANG_TIDY_EXE NAMES run-clang-tidy run-clang-tidy.py HINTS "${tidy_dir}" NO_DEFAULT_PATH)

    set(script "${CMAKE_CURRENT_BINARY_DIR}/cmake/CvfStaticAnalysis.cmake")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CvfStaticAnalysis.cmake.in" "${script}" @ONLY)

    add_custom_target(static-analysis
        COMMAND "${CMAKE_COMMAND}"
            -D "CVF_CLANG_TIDY=${CVFORWIN_CLANG_TIDY_EXE}"
            -D "CVF_RUN_CLANG_TIDY=${CVFORWIN_RUN_CLANG_TIDY_EXE}"
            -D "CVF_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -D "CVF_BUILD_DIR=${CMAKE_BINARY_DIR}"
            -P "${script}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Running clang-tidy over project sources (warnings are errors)"
        VERBATIM
    )
endfunction()
