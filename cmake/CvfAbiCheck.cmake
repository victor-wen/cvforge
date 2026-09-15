# cvforwin export-surface check.
#
# The authoritative export evidence is produced on Windows against the built
# DLL. The same check runs locally in WSL against the ELF shared library so
# export drift is caught before CI. Both paths compare the symbol names against
# src/c_api/cvforwin.def, the single source of truth for the allowlist.
#
# Linux:  nm -D --defined-only <lib>  -> filter cvf_*
# Windows: dumpbin /exports <dll>     -> filter cvf_*

include_guard(GLOBAL)

function(cvforwin_expected_exports out_var def_file)
    file(STRINGS "${def_file}" def_lines)
    set(names "")
    foreach(line IN LISTS def_lines)
        string(STRIP "${line}" stripped)
        if(stripped STREQUAL "" OR stripped MATCHES "^;")
            continue()
        endif()
        if(stripped MATCHES "^EXPORTS")
            continue()
        endif()
        string(REGEX REPLACE "[ \t@].*$" "" name "${stripped}")
        if(name STREQUAL "")
            continue()
        endif()
        list(APPEND names "${name}")
    endforeach()
    list(SORT names)
    set(${out_var} "${names}" PARENT_SCOPE)
endfunction()

function(cvforwin_add_abi_check_target library_target)
    set(def_file "${CMAKE_CURRENT_SOURCE_DIR}/src/c_api/cvforwin.def")
    cvforwin_expected_exports(expected "${def_file}")
    list(LENGTH expected expected_count)
    if(NOT expected_count EQUAL 5)
        message(FATAL_ERROR "cvforwin.def must list exactly five exports, found ${expected_count}: ${expected}")
    endif()

    set(script "${CMAKE_CURRENT_BINARY_DIR}/cmake/CvfAbiCheck.cmake")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CvfAbiCheck.cmake.in" "${script}" @ONLY)

    if(WIN32)
        unset(dumpbin)
        if(CMAKE_LINKER)
            get_filename_component(linker_dir "${CMAKE_LINKER}" DIRECTORY)
            find_program(CVFORWIN_DUMPBIN_EXE dumpbin HINTS "${linker_dir}" NO_DEFAULT_PATH)
        endif()
        if(NOT CVFORWIN_DUMPBIN_EXE)
            find_program(CVFORWIN_DUMPBIN_EXE dumpbin REQUIRED)
        endif()
    else()
        find_program(CVFORWIN_NM_EXE NAMES nm llvm-nm REQUIRED)
    endif()

    add_custom_target(abi-check
        COMMAND "${CMAKE_COMMAND}"
            -D "CVF_LIBRARY=$<TARGET_FILE:${library_target}>"
            -D "CVF_EXPECTED=${expected}"
            -D "CVF_NM=${CVFORWIN_NM_EXE}"
            -D "CVF_DUMPBIN=${CVFORWIN_DUMPBIN_EXE}"
            -P "${script}"
        DEPENDS "${library_target}"
        COMMENT "Checking that only the five v1 cvf_* symbols are exported"
        VERBATIM
    )
endfunction()
