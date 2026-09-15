# Fail fast when the linker PDB expected by package-symbols is missing.
#
# The package-symbols custom target runs this script with `cmake -P` before
# copying $<TARGET_PDB_FILE:cvforwin> into the symbols archive. `cmake -E
# copy_if_different` reports only "Error copying file", which hid the real
# cause of CI round 4 (run 34965200127): link.exe writes no PDB when /debug is
# absent, and CMake's Windows-MSVC platform module links /debug for Debug and
# RelWithDebInfo only (Release gets /INCREMENTAL:NO alone). The Release link
# therefore carries an explicit /debug and a pinned PDB output directory in the
# root CMakeLists.txt (cvforwin, WIN32/MSVC branch).
#
# Inputs (passed with -D, generator expressions already resolved by the build
# tool):
#   CVF_PDB    - resolved path of the expected linker PDB (required)
#   CVF_TARGET - target name for the diagnostic (default "cvforwin")
#   CVF_CONFIG - configuration being built for the diagnostic (default "unknown")

if(NOT DEFINED CVF_PDB OR CVF_PDB STREQUAL "")
    message(FATAL_ERROR
        "package-symbols: CVF_PDB is unset, so the linker PDB path is unknown. "
        "Check the $<TARGET_PDB_FILE:...> generator expression in cmake/CvfPackage.cmake.")
endif()

if(NOT EXISTS "${CVF_PDB}")
    set(_cvf_target "cvforwin")
    if(DEFINED CVF_TARGET AND NOT CVF_TARGET STREQUAL "")
        set(_cvf_target "${CVF_TARGET}")
    endif()
    set(_cvf_config "unknown")
    if(DEFINED CVF_CONFIG AND NOT CVF_CONFIG STREQUAL "")
        set(_cvf_config "${CVF_CONFIG}")
    endif()
    message(FATAL_ERROR
        "package-symbols: target '${_cvf_target}' produced no linker PDB at "
        "'${CVF_PDB}' in configuration '${_cvf_config}'. On MSVC the linker writes a PDB "
        "only when /debug is on the link line, and CMake adds it for Debug and "
        "RelWithDebInfo only. Release must link /debug explicitly and keep the linker PDB "
        "directory in sync with $<TARGET_PDB_FILE:${_cvf_target}> "
        "(see the WIN32/MSVC block in CMakeLists.txt).")
endif()

message(STATUS "package-symbols: linker PDB found at '${CVF_PDB}'")
