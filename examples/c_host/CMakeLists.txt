# cvforwin native C host example (template).
#
# Builds a strict C11 executable that consumes only the public header through
# the cvforwin target. It is part of the default build on every platform so the
# C ABI surface is compiled by a plain C compiler in WSL and MSVC alike. It is
# not a CTest test: the portable lifecycle evidence is the recorded example run
# against a test-enabled build (examples/c_host/run_example.sh).

add_executable(cvf_c_host_example main.c)

target_link_libraries(cvf_c_host_example PRIVATE cvforwin)

set_target_properties(cvf_c_host_example PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/c_host"
)
# Multi-config generators append a per-config subdirectory; pin the standard
# configurations so the executable path stays predictable in every build.
foreach(cvf_example_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(cvf_c_host_example PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${cvf_example_config}" "${CMAKE_BINARY_DIR}/examples/c_host")
endforeach()

cvforwin_apply_test_compile_options(cvf_c_host_example)
cvforwin_apply_sanitizers(cvf_c_host_example)
