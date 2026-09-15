# cvforwin package, package-symbols, and package-verify targets.
#
# The package target assembles a ZIP with the DLL, import library, public
# header, example configuration and recipes, usage documentation, and license
# notices; package-verify checks the staged contents, the exact five-symbol
# export set, the notices, and the runtime DLL dependency allowlist. The
# authoritative run is the windows-2022 CI job. On portable hosts the targets
# exist but fail loudly with the reason, so a WSL invocation can never be
# mistaken for release evidence.
#
# F3 (CVF-008): the scripted bodies run in `cmake -P` script mode, where
# generator expressions are never expanded. CvfPackage.cmake therefore passes
# the concrete configuration together with the generator-expression-resolved
# output paths, and the scripts locate build outputs through that passed
# configuration. No script body contains a literal generator expression.
#
# QE-1 (CVF-008): the portable refusal messages contain no shell
# metacharacters, so the refusal exits non-zero through `cmake -E false`
# instead of a shell syntax error.

include_guard(GLOBAL)

# Defines the portable refusal target: print one clear line, then fail with a
# non-zero exit through cmake -E false. No parentheses or other shell
# metacharacters may appear in the message (QE-1).
function(cvforwin_add_refusal_target target_name)
    add_custom_target(${target_name}
        COMMAND "${CMAKE_COMMAND}" -E echo
            "${target_name} is Windows-only - run the windows-msvc-release preset on windows-2022 CI"
        COMMAND "${CMAKE_COMMAND}" -E false
        COMMENT "${target_name} is Windows-only; refusing to produce portable release evidence"
        VERBATIM
    )
endfunction()

function(cvforwin_add_package_targets)
    if(NOT WIN32)
        cvforwin_add_refusal_target(package)
        cvforwin_add_refusal_target(package-verify)
        return()
    endif()

    # dumpbin is needed by package-verify. It usually sits next to the linker
    # that CMake already resolved; the verify script additionally falls back to
    # vswhere and then PATH, and fails when it cannot find it.
    unset(CVFORWIN_PACKAGE_DUMPBIN_EXE CACHE)
    if(CMAKE_LINKER)
        get_filename_component(linker_dir "${CMAKE_LINKER}" DIRECTORY)
        find_program(CVFORWIN_PACKAGE_DUMPBIN_EXE dumpbin HINTS "${linker_dir}")
    else()
        find_program(CVFORWIN_PACKAGE_DUMPBIN_EXE dumpbin)
    endif()
    if(NOT CVFORWIN_PACKAGE_DUMPBIN_EXE)
        set(CVFORWIN_PACKAGE_DUMPBIN_EXE "")
    endif()

    set(script "${CMAKE_CURRENT_BINARY_DIR}/cmake/CvfPackage.cmake")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CvfPackage.cmake.in" "${script}" @ONLY)

    add_custom_target(package
        COMMAND "${CMAKE_COMMAND}"
            -D "CVF_BUILD_DIR=${CMAKE_BINARY_DIR}"
            -D "CVF_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -D "CVF_CONFIG=$<CONFIG>"
            -D "CVF_PACKAGE_DIR=${CMAKE_BINARY_DIR}/package"
            -D "CVF_DLL=$<TARGET_FILE:cvforwin>"
            -D "CVF_IMPORT_LIB=$<TARGET_LINKER_FILE:cvforwin>"
            -P "${script}"
        DEPENDS cvforwin cvforwin_core
        COMMENT "Assembling the release package"
        VERBATIM
    )

    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CvfPackageVerify.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake/CvfPackageVerify.cmake" @ONLY)

    add_custom_target(package-verify
        COMMAND "${CMAKE_COMMAND}"
            -D "CVF_BUILD_DIR=${CMAKE_BINARY_DIR}"
            -D "CVF_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            -D "CVF_CONFIG=$<CONFIG>"
            -D "CVF_PACKAGE_DIR=${CMAKE_BINARY_DIR}/package"
            -D "CVF_DLL=$<TARGET_FILE:cvforwin>"
            -D "CVF_IMPORT_LIB=$<TARGET_LINKER_FILE:cvforwin>"
            -D "CVF_DUMPBIN=${CVFORWIN_PACKAGE_DUMPBIN_EXE}"
            -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/CvfPackageVerify.cmake"
        DEPENDS package
        COMMENT "Verifying package contents, exports, notices, and runtime DLL dependencies"
        VERBATIM
    )

    # The release symbols PDB travels in its own archive and never enters the
    # package ZIP. The root CMakeLists.txt (WIN32/MSVC) links Release with
    # /debug and pins the linker PDB to bin/<config>, so $<TARGET_PDB_FILE>
    # names the file link.exe actually writes; CMake's platform module alone
    # would link /debug only for Debug and RelWithDebInfo (CI round 4, run
    # 34965200127). The cmake -P check below fails with the missing path,
    # configuration, and /debug requirement before the copy, instead of the
    # bare "Error copying file" from copy_if_different.
    #
    # CI round 3 (run 34960459232): no WORKING_DIRECTORY may point at a
    # directory that the target itself creates. Build tools change into the
    # working directory before running the first command, so the batch failed
    # before `cmake -E make_directory` could run (MSBuild: "The system cannot
    # find the path specified."; Ninja: "cd: can't cd to .../symbols"). The
    # directory is created at configure time, every command runs from the
    # always-existing build root, and the tar step uses `cmake -E chdir` into
    # the symbols directory so the archive keeps its root-level cvforwin.pdb
    # entry (absolute tar paths would record symbols/cvforwin.pdb instead).
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/symbols")
    add_custom_target(package-symbols
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/symbols"
        COMMAND "${CMAKE_COMMAND}"
            -D "CVF_PDB=$<TARGET_PDB_FILE:cvforwin>"
            -D "CVF_TARGET=cvforwin"
            -D "CVF_CONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CvfCheckPdb.cmake"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_PDB_FILE:cvforwin>" "${CMAKE_BINARY_DIR}/symbols/"
        COMMAND "${CMAKE_COMMAND}" -E chdir "${CMAKE_BINARY_DIR}/symbols"
            "${CMAKE_COMMAND}" -E tar cf
                "cvforwin-${PROJECT_VERSION}-symbols.zip" --format=zip
                "cvforwin.pdb"
        DEPENDS cvforwin
        COMMENT "Archiving the release PDB separately from the package"
        VERBATIM
    )
endfunction()
