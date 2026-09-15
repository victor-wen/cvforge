# CVF-008 clean package-consumer kit - standalone consumer project.
#
# Configure it only against an installed cvforwin package:
#
#   cmake -S tests/package/consumer -B <build> -DCVF_PACKAGE_ROOT=<absolute package root>
#   cmake --build <build> --config Release
#   <build>/bin/cvf_package_consumer.exe <absolute config root> <absolute output root>
#
# CVF_PACKAGE_ROOT must be an absolute path to the installed package that
# contains bin/cvforwin.dll, lib/cvforwin.lib, and include/cvforwin/cvf_api.h.
# The consumer includes only <cvforwin/cvf_api.h>, links only the installed
# import library, and deliberately does not read the main build tree, the
# source tree, or any project CMake module. The executable is always placed at
# <build>/bin independently of the generator configuration.
#
# At run time the installed DLL must be reachable: the normal driver,
# tests/package/package_checks.cmake, prepends <CVF_PACKAGE_ROOT>/bin to PATH.
# For a manual run, do the same (or run the executable next to cvforwin.dll).

cmake_minimum_required(VERSION 3.28)

project(cvf_package_consumer LANGUAGES C)

if(NOT DEFINED CVF_PACKAGE_ROOT OR "${CVF_PACKAGE_ROOT}" STREQUAL "")
    message(FATAL_ERROR
        "CVF_PACKAGE_ROOT is required: pass -DCVF_PACKAGE_ROOT=<absolute installed package root>")
endif()
if(NOT IS_ABSOLUTE "${CVF_PACKAGE_ROOT}")
    message(FATAL_ERROR "CVF_PACKAGE_ROOT must be an absolute path; got '${CVF_PACKAGE_ROOT}'")
endif()
if(NOT IS_DIRECTORY "${CVF_PACKAGE_ROOT}")
    message(FATAL_ERROR
        "CVF_PACKAGE_ROOT does not exist or is not a directory: '${CVF_PACKAGE_ROOT}'")
endif()
if(NOT EXISTS "${CVF_PACKAGE_ROOT}/include/cvforwin/cvf_api.h")
    message(FATAL_ERROR
        "CVF_PACKAGE_ROOT does not look like an installed cvforwin package: "
        "'${CVF_PACKAGE_ROOT}/include/cvforwin/cvf_api.h' is missing")
endif()
if(NOT EXISTS "${CVF_PACKAGE_ROOT}/lib/cvforwin.lib")
    message(FATAL_ERROR
        "CVF_PACKAGE_ROOT does not look like an installed cvforwin package: "
        "'${CVF_PACKAGE_ROOT}/lib/cvforwin.lib' is missing")
endif()

add_executable(cvf_package_consumer main.c)

target_include_directories(cvf_package_consumer PRIVATE "${CVF_PACKAGE_ROOT}/include")
target_link_libraries(cvf_package_consumer PRIVATE "${CVF_PACKAGE_ROOT}/lib/cvforwin.lib")

set_target_properties(cvf_package_consumer PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cvf_consumer_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(cvf_package_consumer PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${cvf_consumer_config}" "${CMAKE_BINARY_DIR}/bin")
endforeach()

if(MSVC)
    target_compile_options(cvf_package_consumer PRIVATE /W4 /WX)
else()
    target_compile_options(cvf_package_consumer PRIVATE -Wall -Wextra -Werror)
endif()
