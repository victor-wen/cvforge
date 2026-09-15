# CVF-008 clean package-consumer kit - independent package checks.
#
# Driver for the windows-2022 package-consumer job. It verifies the installed
# package produced by the developer and then runs the clean consumer from
# tests/package/consumer against it, using only the installed package.
#
# Invocation (from the repository root, in any shell; Windows only):
#
#   cmake -DCVF_PACKAGE_ROOT=C:/path/to/installed/package -P tests/package/package_checks.cmake
#
# Optional cache variables:
#   CVF_WORK_DIR            scratch root (default <cwd>/cvf-package-checks)
#   CVF_CONSUMER_CONFIG     absolute consumer config root (default
#                           tests/package/consumer/config, the synthetic
#                           test-enabled package configuration)
#   CVF_CONSUMER_GENERATOR  CMake generator for the consumer build
#   CVF_DUMPBIN             explicit dumpbin.exe path when it is not on PATH
#
# The synthetic backend is accepted only by a test-enabled package variant
# (CVFORWIN_BUILD_TEST_BACKENDS=ON), so the CI job must install that variant
# for the consumer run. Release packages are "uvc"-only.
#
# Checks, each failing with a clear message and a non-zero exit:
#   1. required package files and directories exist (DLL, import library,
#      header, example configuration/recipes, docs, notices);
#   2. bin/ contains no runtime DLL other than cvforwin.dll;
#   3. cvforwin.dll exports exactly the five approved cvf_* symbols (dumpbin);
#   4. every dependent DLL of cvforwin.dll is a Windows system DLL (dumpbin);
#   5. the clean consumer configures, builds, and runs with exit 0, printing
#      its ABI/initialize/inspect/reload/shutdown report.
#
# Negative behavior is self-checked on every run: the script re-invokes itself
# with a relative root, a missing root, and an empty root and requires all
# three probes to fail with the expected message. Those probes run before any
# Windows-only step, so they are also meaningful on a non-Windows author host.
#
# Exit codes: 0 = all checks passed, 1 = at least one check failed.
#
# Authorship: test-engineer, change CVF-008. Authored from the approved
# .ai/project-contract.yaml and .ai/test-briefs/CVF-008.yaml only; no
# production source was consulted.

cmake_minimum_required(VERSION 3.28)

function(cvf_note text)
    message(STATUS "package checks: ${text}")
endfunction()

function(cvf_fail text)
    message(SEND_ERROR "package checks FAILED: ${text}")
    set_property(GLOBAL PROPERTY CVF_CHECKS_FAILED TRUE)
endfunction()

# --- arguments --------------------------------------------------------------

if(NOT DEFINED CVF_PACKAGE_ROOT OR "${CVF_PACKAGE_ROOT}" STREQUAL "")
    message(FATAL_ERROR
        "package checks: CVF_PACKAGE_ROOT is required. Invoke as:\n"
        "  cmake -DCVF_PACKAGE_ROOT=<absolute installed package root> "
        "-P tests/package/package_checks.cmake")
endif()
if(NOT IS_ABSOLUTE "${CVF_PACKAGE_ROOT}")
    message(FATAL_ERROR
        "package checks: CVF_PACKAGE_ROOT must be an absolute path; got '${CVF_PACKAGE_ROOT}'")
endif()
if(NOT IS_DIRECTORY "${CVF_PACKAGE_ROOT}")
    message(FATAL_ERROR
        "package checks: CVF_PACKAGE_ROOT does not exist or is not a directory: "
        "'${CVF_PACKAGE_ROOT}'")
endif()

set(cvf_work_root "${CVF_WORK_DIR}")
if("${cvf_work_root}" STREQUAL "")
    set(cvf_work_root "${CMAKE_CURRENT_BINARY_DIR}/cvf-package-checks")
endif()
file(MAKE_DIRECTORY "${cvf_work_root}")

set(cvf_consumer_source "${CMAKE_CURRENT_LIST_DIR}/consumer")
set(cvf_consumer_config "${CVF_CONSUMER_CONFIG}")
if("${cvf_consumer_config}" STREQUAL "")
    set(cvf_consumer_config "${cvf_consumer_source}/config")
endif()
set(cvf_consumer_output "${cvf_work_root}/consumer-output")
file(MAKE_DIRECTORY "${cvf_consumer_output}")

if(NOT IS_DIRECTORY "${cvf_consumer_source}")
    message(FATAL_ERROR
        "package checks: consumer project is missing at '${cvf_consumer_source}'")
endif()
if(NOT IS_ABSOLUTE "${cvf_consumer_config}")
    message(FATAL_ERROR
        "package checks: CVF_CONSUMER_CONFIG must be an absolute path; got '${cvf_consumer_config}'")
endif()
if(NOT IS_DIRECTORY "${cvf_consumer_config}")
    message(FATAL_ERROR
        "package checks: CVF_CONSUMER_CONFIG is not a directory: '${cvf_consumer_config}'")
endif()

cvf_note("verifying package root '${CVF_PACKAGE_ROOT}'")

# --- 1. required package layout --------------------------------------------

set(cvf_required_files
    "bin/cvforwin.dll"
    "lib/cvforwin.lib"
    "include/cvforwin/cvf_api.h"
    "config/examples/cvforwin.json")
foreach(cvf_relative IN LISTS cvf_required_files)
    if(NOT EXISTS "${CVF_PACKAGE_ROOT}/${cvf_relative}")
        cvf_fail("missing required package file: ${cvf_relative}")
    endif()
endforeach()

file(GLOB_RECURSE cvf_example_recipes LIST_DIRECTORIES FALSE
     "${CVF_PACKAGE_ROOT}/config/examples/recipes/*.json")
if(cvf_example_recipes STREQUAL "")
    cvf_fail("missing example recipes: expected at least one *.json below config/examples/recipes/")
endif()

file(GLOB_RECURSE cvf_doc_files LIST_DIRECTORIES FALSE "${CVF_PACKAGE_ROOT}/docs/*")
if(cvf_doc_files STREQUAL "")
    cvf_fail("missing package documentation: expected at least one file below docs/")
endif()

file(GLOB_RECURSE cvf_license_files LIST_DIRECTORIES FALSE "${CVF_PACKAGE_ROOT}/LICENSES/*")
if(cvf_license_files STREQUAL "")
    cvf_fail("missing notices: expected at least one file below LICENSES/")
else()
    set(cvf_has_named_notice FALSE)
    foreach(cvf_license IN LISTS cvf_license_files)
        get_filename_component(cvf_license_name "${cvf_license}" NAME)
        string(TOLOWER "${cvf_license_name}" cvf_license_lower)
        if(cvf_license_lower MATCHES "license|notice|copying")
            set(cvf_has_named_notice TRUE)
        endif()
    endforeach()
    if(NOT cvf_has_named_notice)
        cvf_fail("LICENSES/ has no LICENSE/NOTICE/COPYING-named notice file")
    endif()
endif()

# --- 2. shipped runtime DLLs -----------------------------------------------

file(GLOB cvf_bin_dlls LIST_DIRECTORIES FALSE "${CVF_PACKAGE_ROOT}/bin/*.[dD][lL][lL]")
foreach(cvf_dll IN LISTS cvf_bin_dlls)
    get_filename_component(cvf_dll_name "${cvf_dll}" NAME)
    string(TOLOWER "${cvf_dll_name}" cvf_dll_lower)
    if(NOT cvf_dll_lower STREQUAL "cvforwin.dll")
        cvf_fail("unexpected runtime DLL in the package bin/ directory: ${cvf_dll_name}")
    endif()
endforeach()

# --- 3. resolve dumpbin for the export and dependency checks ----------------

set(cvf_dumpbin "")
if(NOT "${CVF_DUMPBIN}" STREQUAL "")
    set(cvf_dumpbin "${CVF_DUMPBIN}")
else()
    find_program(cvf_dumpbin_found NAMES dumpbin dumpbin.exe)
    if(cvf_dumpbin_found)
        set(cvf_dumpbin "${cvf_dumpbin_found}")
    endif()
endif()
if("${cvf_dumpbin}" STREQUAL "" AND WIN32)
    # A plain runner shell does not inherit the VS developer environment, so
    # fall back to the installed toolset through vswhere.
    set(cvf_vswhere "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
    if(EXISTS "${cvf_vswhere}")
        execute_process(
            COMMAND "${cvf_vswhere}" -latest -products "*"
                    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                    -property installationPath
            OUTPUT_VARIABLE cvf_vs_path
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(EXISTS "${cvf_vs_path}")
            file(GLOB cvf_dumpbin_candidates
                 "${cvf_vs_path}/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe")
            if(NOT cvf_dumpbin_candidates STREQUAL "")
                list(SORT cvf_dumpbin_candidates)
                list(REVERSE cvf_dumpbin_candidates)
                list(GET cvf_dumpbin_candidates 0 cvf_dumpbin)
            endif()
        endif()
    endif()
endif()
if("${cvf_dumpbin}" STREQUAL "" OR NOT EXISTS "${cvf_dumpbin}")
    cvf_fail("dumpbin was not found on PATH or through vswhere; the export set and runtime "
             "dependencies cannot be verified (set -DCVF_DUMPBIN=<path> to override)")
else()
    cvf_note("using dumpbin '${cvf_dumpbin}'")
endif()

# --- 4. exact export set ----------------------------------------------------

set(cvf_expected_exports
    cvf_get_abi_version
    cvf_initialize
    cvf_inspect
    cvf_reload_recipes
    cvf_shutdown)

if(NOT "${cvf_dumpbin}" STREQUAL "" AND EXISTS "${cvf_dumpbin}")
    execute_process(
        COMMAND "${cvf_dumpbin}" /nologo /exports "${CVF_PACKAGE_ROOT}/bin/cvforwin.dll"
        RESULT_VARIABLE cvf_exports_result
        OUTPUT_VARIABLE cvf_exports_output
        ERROR_VARIABLE cvf_exports_error)
    if(NOT cvf_exports_result EQUAL 0)
        cvf_fail("dumpbin /exports failed with exit ${cvf_exports_result}: ${cvf_exports_error}")
    else()
        string(REGEX MATCHALL "[ \t]+[0-9]+[ \t]+[0-9]+[ \t]+[0-9A-Fa-f]+[ \t]+[A-Za-z_][A-Za-z0-9_]*"
               cvf_export_rows "${cvf_exports_output}")
        set(cvf_actual_exports "")
        foreach(cvf_row IN LISTS cvf_export_rows)
            string(REGEX REPLACE ".*[ \t]" "" cvf_export_name "${cvf_row}")
            list(APPEND cvf_actual_exports "${cvf_export_name}")
        endforeach()
        list(REMOVE_DUPLICATES cvf_actual_exports)
        list(SORT cvf_actual_exports)
        list(SORT cvf_expected_exports)
        if(NOT "${cvf_actual_exports}" STREQUAL "${cvf_expected_exports}")
            cvf_fail("export set mismatch: expected [${cvf_expected_exports}], "
                     "observed [${cvf_actual_exports}]")
        else()
            cvf_note("export set verified: [${cvf_actual_exports}]")
        endif()
    endif()
endif()

# --- 5. runtime dependency allowlist ----------------------------------------

set(cvf_allowed_system_dll_prefixes
    advapi32 api-ms-win- bcrypt cfgmgr32 comdlg32 combase crypt32 d3d11 dbghelp dwmapi
    ext-ms-win- gdi32 gdiplus kernel32 mf mfplat mfreadwrite mfuuid ncrypt ntdll ole32
    oleaut32 opengl32 powrprof propsys rpcrt4 secur32 setupapi shell32 shlwapi user32
    version winmm ws2_32)

if(NOT "${cvf_dumpbin}" STREQUAL "" AND EXISTS "${cvf_dumpbin}")
    execute_process(
        COMMAND "${cvf_dumpbin}" /nologo /dependents "${CVF_PACKAGE_ROOT}/bin/cvforwin.dll"
        RESULT_VARIABLE cvf_dependents_result
        OUTPUT_VARIABLE cvf_dependents_output
        ERROR_VARIABLE cvf_dependents_error)
    if(NOT cvf_dependents_result EQUAL 0)
        cvf_fail("dumpbin /dependents failed with exit ${cvf_dependents_result}: "
                 "${cvf_dependents_error}")
    else()
        string(FIND "${cvf_dependents_output}" "dependencies:" cvf_dependencies_at)
        if(cvf_dependencies_at LESS 0)
            cvf_fail("dumpbin /dependents output has no dependencies section")
        else()
            string(SUBSTRING "${cvf_dependents_output}" ${cvf_dependencies_at} -1
                   cvf_dependencies_text)
            string(REGEX MATCHALL "[A-Za-z0-9_.-]+\\.[dD][lL][lL]"
                   cvf_dependent_tokens "${cvf_dependencies_text}")
            foreach(cvf_token IN LISTS cvf_dependent_tokens)
                string(TOLOWER "${cvf_token}" cvf_dependent_lower)
                if(cvf_dependent_lower STREQUAL "cvforwin.dll")
                    continue()
                endif()
                set(cvf_dependent_ok FALSE)
                foreach(cvf_prefix IN LISTS cvf_allowed_system_dll_prefixes)
                    string(FIND "${cvf_dependent_lower}" "${cvf_prefix}" cvf_prefix_at)
                    if(cvf_prefix_at EQUAL 0)
                        set(cvf_dependent_ok TRUE)
                        break()
                    endif()
                endforeach()
                if(NOT cvf_dependent_ok)
                    cvf_fail("non-system runtime dependency in cvforwin.dll imports: "
                             "${cvf_token} (not in the Windows system allowlist)")
                endif()
            endforeach()
            if(NOT cvf_dependent_tokens STREQUAL "")
                cvf_note("runtime dependencies checked: [${cvf_dependent_tokens}]")
            else()
                cvf_fail("dumpbin /dependents reported no dependent DLLs")
            endif()
        endif()
    endif()
endif()

# --- 6. clean consumer configure / build / run ------------------------------

set(cvf_consumer_build "${cvf_work_root}/consumer-build")
set(cvf_consumer_exe "${cvf_consumer_build}/bin/cvf_package_consumer.exe")

set(cvf_consumer_configure_args
    -S "${cvf_consumer_source}"
    -B "${cvf_consumer_build}"
    "-DCVF_PACKAGE_ROOT=${CVF_PACKAGE_ROOT}"
    -DCMAKE_BUILD_TYPE=Release)
if(NOT "${CVF_CONSUMER_GENERATOR}" STREQUAL "")
    list(APPEND cvf_consumer_configure_args -G "${CVF_CONSUMER_GENERATOR}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${cvf_consumer_configure_args}
    RESULT_VARIABLE cvf_configure_result
    OUTPUT_VARIABLE cvf_configure_output
    ERROR_VARIABLE cvf_configure_error)
if(NOT cvf_configure_result EQUAL 0)
    cvf_fail("consumer configure failed with exit ${cvf_configure_result}:\n"
             "${cvf_configure_output}${cvf_configure_error}")
else()
    cvf_note("consumer configure succeeded against the installed package")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${cvf_consumer_build}" --config Release
        RESULT_VARIABLE cvf_build_result
        OUTPUT_VARIABLE cvf_build_output
        ERROR_VARIABLE cvf_build_error)
    if(NOT cvf_build_result EQUAL 0)
        cvf_fail("consumer build failed with exit ${cvf_build_result}:\n"
                 "${cvf_build_output}${cvf_build_error}")
    else()
        cvf_note("consumer build succeeded")

        if(NOT EXISTS "${cvf_consumer_exe}")
            cvf_fail("consumer executable not found at ${cvf_consumer_exe}")
        else()
            # Prepend the package bin directory so the installed cvforwin.dll is
            # the DLL the consumer loads.
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E env --modify "PATH=prepend:${CVF_PACKAGE_ROOT}/bin"
                        "${cvf_consumer_exe}" "${cvf_consumer_config}" "${cvf_consumer_output}"
                RESULT_VARIABLE cvf_run_result
                OUTPUT_VARIABLE cvf_run_output
                ERROR_VARIABLE cvf_run_error)
            if(NOT cvf_run_result EQUAL 0)
                cvf_fail("consumer run failed with exit ${cvf_run_result}:\n"
                         "${cvf_run_output}${cvf_run_error}")
            else()
                cvf_note("consumer run succeeded; report:\n${cvf_run_output}")
            endif()
        endif()
    endif()
endif()

# --- 7. negative self-checks ------------------------------------------------

function(cvf_negative_probe probe_name expected_text)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${ARGN} -DCVF_SKIP_NEGATIVE_PROBES=ON
                -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE cvf_probe_result
        OUTPUT_VARIABLE cvf_probe_output
        ERROR_VARIABLE cvf_probe_error)
    set(cvf_probe_text "${cvf_probe_output}${cvf_probe_error}")
    if(cvf_probe_result EQUAL 0)
        cvf_fail("negative probe '${probe_name}' unexpectedly succeeded")
    else()
        string(FIND "${cvf_probe_text}" "${expected_text}" cvf_probe_at)
        if(cvf_probe_at LESS 0)
            cvf_fail("negative probe '${probe_name}' failed without the expected message "
                     "'${expected_text}'")
        else()
            cvf_note("negative probe '${probe_name}' failed as expected "
                     "(exit ${cvf_probe_result})")
        endif()
    endif()
endfunction()

get_property(cvf_checks_failed GLOBAL PROPERTY CVF_CHECKS_FAILED)
if(NOT CVF_SKIP_NEGATIVE_PROBES)
    if(cvf_checks_failed)
        cvf_note("negative probes skipped because earlier checks already failed")
    else()
        set(cvf_empty_package "${cvf_work_root}/empty-package")
        file(MAKE_DIRECTORY "${cvf_empty_package}")
        cvf_negative_probe("relative root" "must be an absolute path"
            "-DCVF_PACKAGE_ROOT=relative/package")
        cvf_negative_probe("missing root" "does not exist or is not a directory"
            "-DCVF_PACKAGE_ROOT=${cvf_work_root}/no-such-package")
        cvf_negative_probe("empty root" "missing required package file"
            "-DCVF_PACKAGE_ROOT=${cvf_empty_package}")
    endif()
endif()

# --- summary ----------------------------------------------------------------

get_property(cvf_checks_failed GLOBAL PROPERTY CVF_CHECKS_FAILED)
if(cvf_checks_failed)
    message(FATAL_ERROR "package checks: one or more checks failed for '${CVF_PACKAGE_ROOT}'")
endif()
cvf_note("all checks passed for '${CVF_PACKAGE_ROOT}'")
