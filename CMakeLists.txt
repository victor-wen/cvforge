cmake_minimum_required(VERSION 3.28)

project(cvforwin
    VERSION 1.0.0
    DESCRIPTION "Windows x64 visual-inspection DLL with a native C ABI v1"
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CvfOptions)
include(CvfFormat)
include(CvfAbiCheck)
include(CvfAnalysis)
include(CvfPackage)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

message(STATUS "cvforwin: compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "cvforwin: build type ${CMAKE_BUILD_TYPE}")

# ---------------------------------------------------------------------------
# cvforwin_core - internal status/validation model (no ABI, no OpenCV)
# ---------------------------------------------------------------------------

add_library(cvforwin_core STATIC
    src/core/error.cpp
    src/core/status.cpp
    src/core/text.cpp
    src/core/deadline.cpp
    src/core/abi_validation.cpp
)
add_library(cvforwin::core ALIAS cvforwin_core)

target_include_directories(cvforwin_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
set_target_properties(cvforwin_core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
cvforwin_apply_project_compile_options(cvforwin_core)
cvforwin_apply_sanitizers(cvforwin_core)

# ---------------------------------------------------------------------------
# cvforwin_camera - backend-neutral camera contract (camera_contract module)
# ---------------------------------------------------------------------------
#
# Descriptor/selector/settings values, ICameraBackend, captured frames with
# owned BGR8 pixels, unique identity resolution, and the bounded
# one-reconnect/one-recapture helper. No vendor type, product verdict, or
# public C ABI structure enters this boundary.

find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgcodecs imgproc)

add_library(cvforwin_camera STATIC
    src/camera/camera_backend.cpp
)
add_library(cvforwin::camera ALIAS cvforwin_camera)

target_include_directories(cvforwin_camera
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(cvforwin_camera
    PUBLIC
        cvforwin_core
        opencv_core
)
set_target_properties(cvforwin_camera PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
cvforwin_apply_project_compile_options(cvforwin_camera)
cvforwin_apply_sanitizers(cvforwin_camera)

# ---------------------------------------------------------------------------
# cvforwin_uvc_windows - Media Foundation UVC backend (Windows-only)
# ---------------------------------------------------------------------------
#
# uvc_windows_backend module: Media Foundation enumeration with stable identity
# attributes, exactly-one resolution through the shared camera contract, and
# open/configure/capture/reconnect/close with deadline handling and BGR8
# normalization. The sources compile only under if(WIN32) and link Media
# Foundation system libraries only; no vendor SDK. Portable builds neither
# compile nor link this module.

if(WIN32)
    add_library(cvforwin_uvc_windows STATIC
        src/camera/uvc_windows/uvc_backend.cpp
        src/camera/uvc_windows/uvc_frame_convert.cpp
    )
    add_library(cvforwin::uvc_windows ALIAS cvforwin_uvc_windows)

    target_include_directories(cvforwin_uvc_windows
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(cvforwin_uvc_windows
        PUBLIC
            cvforwin_camera
        PRIVATE
            opencv_imgproc
            mfplat
            mf
            mfreadwrite
            mfuuid
            ole32
    )
    set_target_properties(cvforwin_uvc_windows PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_project_compile_options(cvforwin_uvc_windows)
    cvforwin_apply_sanitizers(cvforwin_uvc_windows)
endif()

# ---------------------------------------------------------------------------
# cvforwin_inspection - algorithm authoring contract and compiled registry
# ---------------------------------------------------------------------------
#
# inspection_contract_and_registry module: IInspectionAlgorithm,
# AlgorithmRequest/AlgorithmResult, AlgorithmRegistry, and dispatch
# containment. It depends on core_model, camera_contract, and the nlohmann-json
# value model that the frozen request/result types expose by reference; no
# camera backend, C ABI structure, or runtime plug-in loading.

find_package(nlohmann_json CONFIG REQUIRED)

add_library(cvforwin_inspection STATIC
    src/inspection/registry.cpp
)
add_library(cvforwin::inspection ALIAS cvforwin_inspection)

target_include_directories(cvforwin_inspection
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(cvforwin_inspection
    PUBLIC
        cvforwin_core
        cvforwin_camera
        nlohmann_json::nlohmann_json
)
set_target_properties(cvforwin_inspection PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
cvforwin_apply_project_compile_options(cvforwin_inspection)
cvforwin_apply_sanitizers(cvforwin_inspection)

# ---------------------------------------------------------------------------
# cvforwin_algorithms - compiled OpenCV algorithms behind IInspectionAlgorithm
# ---------------------------------------------------------------------------
#
# compiled_algorithms module: isolated OpenCV-based algorithms with strict
# parameter validation and bounded structured results. It depends on the
# inspection registry, approved OpenCV components (imgproc), and nlohmann-json
# only; algorithms never reach a camera backend, the C ABI, files, or global
# mutable state.

add_library(cvforwin_algorithms STATIC
    src/algorithms/compiled_algorithms.cpp
    src/algorithms/example_threshold.cpp
)
add_library(cvforwin::algorithms ALIAS cvforwin_algorithms)

target_include_directories(cvforwin_algorithms
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(cvforwin_algorithms
    PUBLIC
        cvforwin_inspection
        nlohmann_json::nlohmann_json
    PRIVATE
        opencv_imgproc
)
set_target_properties(cvforwin_algorithms PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
cvforwin_apply_project_compile_options(cvforwin_algorithms)
cvforwin_apply_sanitizers(cvforwin_algorithms)

# ---------------------------------------------------------------------------
# cvforwin_recipes - strict global configuration, recipes, and immutable catalog
# ---------------------------------------------------------------------------
#
# recipe_store module: strict, duplicate-key-safe parsing of cvforwin.json and
# recipes/*.json, algorithm-aware parameter validation through the compiled
# registry contract, an immutable catalog indexed by recipe_id, and the atomic
# holder. It depends on core_model, nlohmann-json, and the inspection registry
# validation contract; it never reaches a camera implementation or the public
# C ABI.

add_library(cvforwin_recipes STATIC
    src/recipes/config.cpp
    src/recipes/recipe.cpp
    src/recipes/recipe_catalog.cpp
)
add_library(cvforwin::recipes ALIAS cvforwin_recipes)

target_include_directories(cvforwin_recipes
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(cvforwin_recipes
    PUBLIC
        cvforwin_core
        cvforwin_inspection
        nlohmann_json::nlohmann_json
)
set_target_properties(cvforwin_recipes PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
cvforwin_apply_project_compile_options(cvforwin_recipes)
cvforwin_apply_sanitizers(cvforwin_recipes)

if(CVFORWIN_BUILD_TEST_BACKENDS)
    # The backend schema additionally accepts "file"/"synthetic" only in
    # test-enabled builds; release builds stay "uvc"-only.
    target_compile_definitions(cvforwin_recipes PRIVATE CVFORWIN_TEST_BACKENDS_ENABLED=1)
endif()

# ---------------------------------------------------------------------------
# cvforwin_diagnostics - rotating logs, callback fan-out, warning containment
# ---------------------------------------------------------------------------
#
# diagnostics_and_artifacts module (diagnostics half): a spdlog size-rotating
# file sink below an absolute log directory plus an optional synchronous C
# callback. Contained sink failures become internal warning flags; log() and
# the create-side effects never throw into the caller. It depends on core_model
# and spdlog only, and never reaches a recipe, a camera, or the public C ABI.

find_package(spdlog CONFIG REQUIRED)

add_library(cvforwin_diagnostics STATIC
    src/diagnostics/diagnostics.cpp
)
add_library(cvforwin::diagnostics ALIAS cvforwin_diagnostics)

target_include_directories(cvforwin_diagnostics
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(cvforwin_diagnostics
    PUBLIC
        cvforwin_core
    PRIVATE
        spdlog::spdlog
)
set_target_properties(cvforwin_diagnostics PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
cvforwin_apply_project_compile_options(cvforwin_diagnostics)
cvforwin_apply_sanitizers(cvforwin_diagnostics)

# ---------------------------------------------------------------------------
# cvforwin_artifacts - managed capture persistence and bounded retention
# ---------------------------------------------------------------------------
#
# diagnostics_and_artifacts module (artifacts half): sanitized in-root PNG
# persistence with distinct encode/write failures and a non-recursive,
# symlink-safe age-then-size retention scan. It depends on core_model and
# OpenCV imgcodecs; it never reaches a camera, a recipe, or the public C ABI.

add_library(cvforwin_artifacts STATIC
    src/artifacts/capture_store.cpp
)
add_library(cvforwin::artifacts ALIAS cvforwin_artifacts)

target_include_directories(cvforwin_artifacts
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(cvforwin_artifacts
    PUBLIC
        cvforwin_core
        opencv_core
    PRIVATE
        opencv_imgcodecs
)
set_target_properties(cvforwin_artifacts PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
cvforwin_apply_project_compile_options(cvforwin_artifacts)
cvforwin_apply_sanitizers(cvforwin_artifacts)

# ---------------------------------------------------------------------------
# cvforwin_test_backends - deterministic, hardware-free test camera backends
# ---------------------------------------------------------------------------
#
# test_camera_backends module: built only when CVFORWIN_BUILD_TEST_BACKENDS is
# ON (dev presets) and never linked into the shipped DLL. They decode files and
# synthesize frames, and they inject disconnect, capture-error, and bounded
# delay faults.

if(CVFORWIN_BUILD_TEST_BACKENDS)
    add_library(cvforwin_test_backends STATIC
        src/camera/test_backends/test_backend_support.cpp
        src/camera/test_backends/file_camera_backend.cpp
        src/camera/test_backends/synthetic_camera_backend.cpp
    )

    target_include_directories(cvforwin_test_backends
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(cvforwin_test_backends
        PUBLIC
            cvforwin_camera
        PRIVATE
            opencv_imgcodecs
            opencv_imgproc
    )
    set_target_properties(cvforwin_test_backends PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_project_compile_options(cvforwin_test_backends)
    cvforwin_apply_sanitizers(cvforwin_test_backends)
endif()

# ---------------------------------------------------------------------------
# cvforwin_runtime - runtime orchestrator
# ---------------------------------------------------------------------------
#
# runtime_orchestrator module: the single process context (camera session,
# immutable recipe snapshot, registry, diagnostics, artifacts, serialization
# gate), rollback-safe initialization, the one-deadline inspection cycle,
# atomic reload, and orderly shutdown. It depends on every approved internal
# module and never assumes public ABI layout.

add_library(cvforwin_runtime STATIC
    src/runtime/context.cpp
)
add_library(cvforwin::runtime ALIAS cvforwin_runtime)

target_include_directories(cvforwin_runtime
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(cvforwin_runtime
    PUBLIC
        cvforwin_core
        cvforwin_camera
        cvforwin_inspection
        cvforwin_recipes
        cvforwin_algorithms
        cvforwin_diagnostics
        cvforwin_artifacts
        nlohmann_json::nlohmann_json
)
if(CVFORWIN_BUILD_TEST_BACKENDS)
    target_link_libraries(cvforwin_runtime PRIVATE cvforwin_test_backends)
    target_compile_definitions(cvforwin_runtime PRIVATE CVFORWIN_TEST_BACKENDS_ENABLED=1)
endif()
if(WIN32)
    # The runtime factory selects the UVC backend for config backend "uvc" on
    # Windows builds; the static link also propagates the Media Foundation
    # system libraries to the DLL. Portable builds fail that config instead.
    target_link_libraries(cvforwin_runtime PRIVATE cvforwin_uvc_windows)
endif()
set_target_properties(cvforwin_runtime PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
cvforwin_apply_project_compile_options(cvforwin_runtime)
cvforwin_apply_sanitizers(cvforwin_runtime)

# ---------------------------------------------------------------------------
# cvforwin - the public shared library / Windows DLL
# ---------------------------------------------------------------------------

set(cvforwin_sources
    src/c_api/cvf_context.cpp
    src/c_api/cvf_entry_points.cpp
)

add_library(cvforwin SHARED ${cvforwin_sources})
add_library(cvforwin::shared ALIAS cvforwin)

target_include_directories(cvforwin
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(cvforwin PRIVATE cvforwin_core cvforwin_runtime)

target_compile_definitions(cvforwin PRIVATE CVF_BUILD_SHARED=1)
set_target_properties(cvforwin PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(WIN32)
    # Exact export allowlist. The .def file is the Windows source of truth and
    # additions/removals are R3 changes.
    target_sources(cvforwin PRIVATE src/c_api/cvforwin.def)
    set_target_properties(cvforwin PROPERTIES
        PREFIX ""
        OUTPUT_NAME "cvforwin"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
    )

    if(MSVC)
        # CI round 4 (run 34965200127): CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        # ProgramDatabase adds the compiler /Zi only, and CMake's Windows-MSVC
        # platform module puts /debug on the link line for Debug and
        # RelWithDebInfo, while Release gets /INCREMENTAL:NO alone. link.exe
        # without /debug writes no PDB, so package-symbols had no file to
        # archive. Link /debug explicitly for Release and pin the linker PDB
        # next to the DLL, which is where $<TARGET_PDB_FILE:cvforwin> reports it
        # (bin/<config> under a multi-config generator).
        #
        # COMPILE_PDB_OUTPUT_DIRECTORY is deliberately left at its default:
        # pinning it here too would resolve the compiler /Fd file and the linker
        # /PDB file to the same bin/<config>/cvforwin.pdb path.
        target_link_options(cvforwin PRIVATE "$<$<CONFIG:Release>:/debug>")
        set_target_properties(cvforwin PROPERTIES
            PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
    endif()
else()
    # Portable builds hide everything except the five explicitly exported
    # symbols, so the WSL export check is meaningful.
    set_target_properties(cvforwin PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(cvforwin PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/cvforwin.map")
    endif()
endif()

cvforwin_apply_project_compile_options(cvforwin)
cvforwin_apply_sanitizers(cvforwin)

# ---------------------------------------------------------------------------
# Install-rule scaffold (used by the Windows package job)
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS cvforwin cvforwin_core
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY include/cvforwin DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# ---------------------------------------------------------------------------
# Native C host example (template consumer of the public C ABI)
# ---------------------------------------------------------------------------
#
# examples/c_host builds a strict C11 executable that includes only
# <cvforwin/cvf_api.h> and links the cvforwin target. It is built on every
# platform so a plain C compiler validates the public surface, and it is run
# against a test-enabled build by examples/c_host/run_example.sh.

add_subdirectory(examples/c_host)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
#
# Test targets are defined here (not in a tests/CMakeLists.txt) so that the
# change scope stays limited to tests/abi/**, tests/unit/**, and tests/camera/**.
# The tests/abi and tests/camera sources are authored and hash-pinned by the
# test owner; this file only registers them with CTest and never modifies them.

if(CVFORWIN_BUILD_TESTS)
    enable_testing()

    find_package(Threads REQUIRED)

    # --- independent C ABI tests (test-engineer owned) ---------------------

    add_executable(cvf_abi_c11_surface tests/abi/test_c11_surface.c)
    target_link_libraries(cvf_abi_c11_surface PRIVATE cvforwin)
    set_target_properties(cvf_abi_c11_surface PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    cvforwin_apply_test_compile_options(cvf_abi_c11_surface)
    cvforwin_apply_sanitizers(cvf_abi_c11_surface)
    cvforwin_place_shared_library(cvf_abi_c11_surface)
    add_test(NAME cvf.abi.c11_surface COMMAND cvf_abi_c11_surface)

    add_executable(cvf_abi_runtime tests/abi/test_abi_runtime.c)
    target_link_libraries(cvf_abi_runtime PRIVATE cvforwin)
    set_target_properties(cvf_abi_runtime PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    cvforwin_apply_test_compile_options(cvf_abi_runtime)
    cvforwin_apply_sanitizers(cvf_abi_runtime)
    cvforwin_place_shared_library(cvf_abi_runtime)
    add_test(NAME cvf.abi.runtime COMMAND cvf_abi_runtime)

    add_executable(cvf_abi_cpp20_include tests/abi/test_cpp20_include.cpp)
    target_link_libraries(cvf_abi_cpp20_include PRIVATE cvforwin)
    set_target_properties(cvf_abi_cpp20_include PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_test_compile_options(cvf_abi_cpp20_include)
    cvforwin_apply_sanitizers(cvf_abi_cpp20_include)
    cvforwin_place_shared_library(cvf_abi_cpp20_include)
    add_test(NAME cvf.abi.cpp20_include COMMAND cvf_abi_cpp20_include)

    if(MSVC)
        # The header-isolation check uses -nostdinc with the test-owner stub
        # headers, which are GCC/Clang freestanding stubs. The authoritative
        # run is the GNU/Clang configuration; MSVC still compiles the header in
        # C11 mode through the other three ABI tests.
        message(STATUS "cvforwin: header-isolation -nostdinc check is enabled for GCC/Clang only")
    else()
        add_executable(cvf_abi_header_isolation tests/abi/test_header_isolation.c)
        target_include_directories(cvf_abi_header_isolation PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/abi/isolation_stub"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_compile_options(cvf_abi_header_isolation PRIVATE -nostdinc)
        set_target_properties(cvf_abi_header_isolation PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF
        )
        cvforwin_apply_test_compile_options(cvf_abi_header_isolation)
        cvforwin_apply_sanitizers(cvf_abi_header_isolation)
        add_test(NAME cvf.abi.header_isolation COMMAND cvf_abi_header_isolation)
    endif()

    # --- developer unit tests (Catch2 v3) ----------------------------------

    find_package(Catch2 3 CONFIG REQUIRED)

    add_executable(cvf_unit_tests
        tests/unit/test_c_api_result_hygiene.cpp
        tests/unit/test_core_status.cpp
        tests/unit/test_core_text.cpp
        tests/unit/test_core_abi_validation.cpp
        tests/unit/test_core_result_deadline.cpp
        tests/unit/test_core_camera_error_codes.cpp
        tests/unit/test_inspection_error_codes.cpp
        tests/unit/test_inspection_contract.cpp
        tests/unit/test_recipes_store.cpp
        tests/unit/test_diagnostics_artifacts.cpp
    )
    target_include_directories(cvf_unit_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(cvf_unit_tests PRIVATE
        cvforwin_core
        cvforwin_recipes
        cvforwin_algorithms
        cvforwin_diagnostics
        cvforwin_artifacts
        cvforwin
        Catch2::Catch2WithMain
    )
    cvforwin_apply_project_compile_options(cvf_unit_tests)
    cvforwin_apply_sanitizers(cvf_unit_tests)
    # Must precede catch_discover_tests below: its POST_BUILD discovery step
    # already runs the executable and therefore needs the DLL next to it.
    cvforwin_place_shared_library(cvf_unit_tests)

    include(Catch OPTIONAL RESULT_VARIABLE cvforwin_catch_module)
    if(cvforwin_catch_module STREQUAL "NOTFOUND")
        message(STATUS "cvforwin: Catch.cmake not found; registering the unit test executable as one CTest test")
        add_test(NAME cvf.unit.core COMMAND cvf_unit_tests)
    else()
        catch_discover_tests(cvf_unit_tests)
    endif()

    # --- independent camera-contract tests (test-engineer owned) -----------
    #
    # The tests/camera suite drives the camera contract through the file and
    # synthetic test backends, so it is registered only when those backends are
    # built. Its sources stay untouched here; only the build target is added.

    if(CVFORWIN_BUILD_TEST_BACKENDS)
        add_executable(cvf_camera_tests
            tests/camera/test_core_result_deadline.cpp
            tests/camera/test_camera_identity_resolution.cpp
            tests/camera/test_file_camera_backend.cpp
            tests/camera/test_synthetic_camera_backend.cpp
            tests/camera/test_camera_fault_injection.cpp
            tests/camera/test_capture_with_one_retry.cpp
        )
        target_include_directories(cvf_camera_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/camera"
        )
        target_link_libraries(cvf_camera_tests PRIVATE
            cvforwin_test_backends
            Catch2::Catch2WithMain
        )
        set_target_properties(cvf_camera_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        cvforwin_apply_test_compile_options(cvf_camera_tests)
        cvforwin_apply_sanitizers(cvf_camera_tests)
        catch_discover_tests(cvf_camera_tests)
    else()
        message(STATUS "cvforwin: tests/camera is not registered because CVFORWIN_BUILD_TEST_BACKENDS is OFF")
    endif()

    # --- independent CVF-003 inspection tests (test-engineer owned) --------
    #
    # The cvf003_* sources are authored and hash-pinned by the test owner; this
    # file only builds and registers them with CTest. They link the compiled
    # inspection registry and example algorithm by target, never by direct
    # production .cpp inclusion.

    add_executable(cvf003_tests
        tests/unit/cvf003_registry_test.cpp
        tests/unit/cvf003_dispatch_test.cpp
        tests/unit/cvf003_example_threshold_test.cpp
    )
    target_include_directories(cvf003_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(cvf003_tests PRIVATE
        cvforwin_algorithms
        Catch2::Catch2WithMain
    )
    set_target_properties(cvf003_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_test_compile_options(cvf003_tests)
    cvforwin_apply_sanitizers(cvf003_tests)
    catch_discover_tests(cvf003_tests)

    # --- independent CVF-004 recipe-store tests (test-engineer owned) ------
    #
    # The cvf004_* sources are authored and hash-pinned by the test owner; this
    # file only builds and registers them with CTest. They link the recipe
    # store and the compiled algorithms (example.threshold and the registry)
    # and need Threads::Threads because the holder test drives std::thread
    # readers.

    add_executable(cvf004_tests
        tests/unit/cvf004_config_test.cpp
        tests/unit/cvf004_recipe_test.cpp
        tests/unit/cvf004_catalog_test.cpp
        tests/unit/cvf004_holder_test.cpp
        tests/unit/cvf004_error_codes_test.cpp
    )
    target_include_directories(cvf004_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(cvf004_tests PRIVATE
        cvforwin_recipes
        cvforwin_algorithms
        Threads::Threads
        Catch2::Catch2WithMain
    )
    set_target_properties(cvf004_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_test_compile_options(cvf004_tests)
    cvforwin_apply_sanitizers(cvf004_tests)
    catch_discover_tests(cvf004_tests)

    if(CVFORWIN_BUILD_TEST_BACKENDS)
        # The backend schema is build-mode aware: test-enabled builds accept
        # "file"/"synthetic" in addition to "uvc".
        target_compile_definitions(cvf004_tests PRIVATE CVFORWIN_TEST_BACKENDS_ENABLED=1)
    endif()

    # --- independent CVF-005 diagnostics/artifact tests (test-engineer owned)
    #
    # The cvf005_* sources are authored and hash-pinned by the test owner; this
    # file only builds and registers them with CTest. They link the diagnostics
    # and artifacts libraries plus OpenCV core/imgcodecs and Catch2.

    add_executable(cvf005_tests
        tests/unit/cvf005_error_codes_test.cpp
        tests/unit/cvf005_diagnostics_config_test.cpp
        tests/unit/cvf005_diagnostics_logging_test.cpp
        tests/unit/cvf005_capture_store_test.cpp
        tests/unit/cvf005_save_policy_test.cpp
        tests/unit/cvf005_retention_test.cpp
    )
    target_include_directories(cvf005_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(cvf005_tests PRIVATE
        cvforwin_diagnostics
        cvforwin_artifacts
        opencv_core
        opencv_imgcodecs
        Catch2::Catch2WithMain
    )
    set_target_properties(cvf005_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_test_compile_options(cvf005_tests)
    cvforwin_apply_sanitizers(cvf005_tests)
    catch_discover_tests(cvf005_tests)

    # --- independent CVF-006 runtime lifecycle tests (test-engineer owned) --
    #
    # The C lifecycle test drives the public C ABI through the deterministic
    # test backends, so it needs the test-backend build, POSIX threads, and the
    # fixture/scratch compile definitions. The C++ suite drives the internal
    # runtime orchestrator directly and links the runtime library, the test
    # backends, OpenCV imgcodecs, Catch2, and Threads. Both source sets are
    # authored and hash-pinned by the test owner; this file only registers them.

    if(CVFORWIN_BUILD_TEST_BACKENDS)
        add_executable(cvf006_lifecycle tests/abi/cvf006_lifecycle.c)
        target_link_libraries(cvf006_lifecycle PRIVATE
            cvforwin
            Threads::Threads
        )
        target_compile_definitions(cvf006_lifecycle PRIVATE
            CVF_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/runtime"
            CVF_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}/cvf006_tmp"
        )
        if(MSVC)
            # The lifecycle test intentionally uses portable C file APIs such
            # as fopen; MSVC flags those with C4996 and /WX promotes it to
            # C2220. The opt-out is scoped to this C ABI test target only.
            target_compile_definitions(cvf006_lifecycle PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        set_target_properties(cvf006_lifecycle PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF
        )
        cvforwin_apply_test_compile_options(cvf006_lifecycle)
        cvforwin_apply_sanitizers(cvf006_lifecycle)
        cvforwin_place_shared_library(cvf006_lifecycle)
        add_test(NAME cvf006.lifecycle COMMAND cvf006_lifecycle)

        add_executable(cvf006_tests
            tests/unit/cvf006_deadline_test.cpp
            tests/unit/cvf006_reconnect_test.cpp
            tests/unit/cvf006_save_policy_test.cpp
            tests/unit/cvf006_degraded_log_test.cpp
            tests/unit/cvf006_rollback_test.cpp
            tests/unit/cvf006_serialization_test.cpp
        )
        target_include_directories(cvf006_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(cvf006_tests PRIVATE
            cvforwin_runtime
            cvforwin_test_backends
            opencv_imgcodecs
            Threads::Threads
            Catch2::Catch2WithMain
        )
        target_compile_definitions(cvf006_tests PRIVATE CVFORWIN_TEST_BACKENDS_ENABLED=1)
        set_target_properties(cvf006_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        cvforwin_apply_test_compile_options(cvf006_tests)
        cvforwin_apply_sanitizers(cvf006_tests)
        catch_discover_tests(cvf006_tests)

        add_executable(cvf006_developer_tests
            tests/unit/test_runtime_lifecycle.cpp
        )
        target_include_directories(cvf006_developer_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(cvf006_developer_tests PRIVATE
            cvforwin_runtime
            cvforwin_test_backends
            Threads::Threads
            Catch2::Catch2WithMain
        )
        target_compile_definitions(cvf006_developer_tests PRIVATE CVFORWIN_TEST_BACKENDS_ENABLED=1)
        set_target_properties(cvf006_developer_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        cvforwin_apply_test_compile_options(cvf006_developer_tests)
        cvforwin_apply_sanitizers(cvf006_developer_tests)
        catch_discover_tests(cvf006_developer_tests)
    else()
        message(STATUS "cvforwin: CVF-006 lifecycle tests are not registered because CVFORWIN_BUILD_TEST_BACKENDS is OFF")
    endif()

    # --- developer-owned CVF-007 UVC factory regression --------------------
    #
    # Built and registered by the presets. On portable builds the case pins the
    # "uvc" factory failure; on Windows the same file additionally pins the UVC
    # backend interface through the real uvc_backend.h.

    add_executable(cvf007_developer_tests
        tests/unit/test_uvc_runtime_factory.cpp
    )
    target_include_directories(cvf007_developer_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    )
    target_link_libraries(cvf007_developer_tests PRIVATE
        cvforwin_runtime
        Catch2::Catch2WithMain
    )
    if(WIN32)
        target_link_libraries(cvf007_developer_tests PRIVATE cvforwin_uvc_windows)
    endif()
    set_target_properties(cvf007_developer_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_project_compile_options(cvf007_developer_tests)
    cvforwin_apply_sanitizers(cvf007_developer_tests)
    catch_discover_tests(cvf007_developer_tests)

    # --- developer-owned CVF-101 portable locked-buffer helper tests --------
    #
    # tests/unit/uvc_frame_math_test.cpp exercises the portable helpers extracted
    # from the Windows-only seam src/camera/uvc_windows/uvc_frame_convert.cpp:
    # the overflow-safe row arithmetic and the full-range YUV conversion. The
    # helpers live in uvc_frame_math.h, which has no Windows API, Media
    # Foundation, or OpenCV dependency, so this target builds and runs on the
    # portable Linux host and covers the seam's pure logic there. The Windows
    # component target cvf101_windows_uvc_tests remains owned by the test owner.

    add_executable(uvc_frame_math_developer_tests
        tests/unit/uvc_frame_math_test.cpp
    )
    target_include_directories(uvc_frame_math_developer_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(uvc_frame_math_developer_tests PRIVATE
        Catch2::Catch2WithMain
    )
    set_target_properties(uvc_frame_math_developer_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_project_compile_options(uvc_frame_math_developer_tests)
    cvforwin_apply_sanitizers(uvc_frame_math_developer_tests)
    catch_discover_tests(uvc_frame_math_developer_tests)

    # --- developer-owned CVF-102 UVC command-worker tests -------------------
    #
    # tests/unit/uvc_command_worker_test.cpp exercises the portable
    # src/camera/uvc_windows/uvc_command_worker.h header: the single-thread
    # command marshaller that the Windows UVC backend uses to own its COM/Media
    # Foundation worker. The header has no Windows API, Media Foundation, or
    # OpenCV dependency, so this target builds and runs on the portable Linux
    # host. The Windows-only worker/apartment behavior is covered by the
    # Windows-gated developer test below and by the independent
    # cvf102_windows_uvc_tests target; neither is faked here.
    #
    # tests/unit/uvc_worker_snapshot_test.cpp is an empty TU off Windows and
    # drives the real UvcCameraBackend::worker_snapshot() seam on Windows.

    add_executable(uvc_command_worker_developer_tests
        tests/unit/uvc_command_worker_test.cpp
    )
    target_include_directories(uvc_command_worker_developer_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(uvc_command_worker_developer_tests PRIVATE
        cvforwin_core
        Threads::Threads
        Catch2::Catch2WithMain
    )
    set_target_properties(uvc_command_worker_developer_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_project_compile_options(uvc_command_worker_developer_tests)
    cvforwin_apply_sanitizers(uvc_command_worker_developer_tests)
    catch_discover_tests(uvc_command_worker_developer_tests)

    add_executable(uvc_worker_snapshot_developer_tests
        tests/unit/uvc_worker_snapshot_test.cpp
    )
    target_include_directories(uvc_worker_snapshot_developer_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit"
    )
    target_link_libraries(uvc_worker_snapshot_developer_tests PRIVATE
        Catch2::Catch2WithMain
    )
    if(WIN32)
        target_link_libraries(uvc_worker_snapshot_developer_tests PRIVATE cvforwin_uvc_windows)
    endif()
    set_target_properties(uvc_worker_snapshot_developer_tests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    cvforwin_apply_project_compile_options(uvc_worker_snapshot_developer_tests)
    cvforwin_apply_sanitizers(uvc_worker_snapshot_developer_tests)
    catch_discover_tests(uvc_worker_snapshot_developer_tests)

    # --- independent CVF-101 Windows UVC 2D-buffer component tests -----------
    #
    # tests/windows/uvc/cvf101_uvc_frame_convert_test.cpp is authored and
    # hash-pinned by the test owner. It includes only the frozen seam header
    # src/camera/uvc_windows/uvc_frame_convert.h and drives it with synthetic
    # in-memory locked buffers; no camera, Media Foundation object, or device is
    # used, and no production .cpp is read. catch_discover_tests registers the
    # cases with CTest via add_test, so the windows-tests-quality workflow runs
    # them. The gate is WIN32 + CVFORWIN_BUILD_TESTS + CVFORWIN_BUILD_TEST_BACKENDS:
    # the release preset sets CVFORWIN_BUILD_TESTS=ON but keeps the test backends
    # OFF, so this target is never registered in the release or any hosted
    # no-camera preset. This file only builds and registers the owner's source;
    # it never modifies it.

    if(WIN32 AND CVFORWIN_BUILD_TESTS AND CVFORWIN_BUILD_TEST_BACKENDS)
        add_executable(cvf101_windows_uvc_tests
            tests/windows/uvc/cvf101_uvc_frame_convert_test.cpp
        )
        target_include_directories(cvf101_windows_uvc_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(cvf101_windows_uvc_tests PRIVATE
            cvforwin_uvc_windows
            opencv_core
            Catch2::Catch2WithMain
        )
        set_target_properties(cvf101_windows_uvc_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        cvforwin_apply_test_compile_options(cvf101_windows_uvc_tests)
        cvforwin_apply_sanitizers(cvf101_windows_uvc_tests)
        catch_discover_tests(cvf101_windows_uvc_tests)
    endif()

    # --- independent CVF-102 Windows UVC worker/lifetime component tests ------
    #
    # tests/windows/uvc/cvf102_uvc_worker_lifecycle_test.cpp is authored and
    # hash-pinned by the test owner. It includes only frozen headers
    # (camera/camera_backend.h, camera/uvc_windows/uvc_backend.h, core/*.h) and
    # drives the frozen UvcCameraBackend::worker_snapshot() seam plus the
    # ICameraBackend contract with absent/ambiguous selectors; no camera,
    # Media Foundation object, or device is used, and no production .cpp is
    # read. catch_discover_tests registers the cases with CTest via add_test, so
    # the windows-tests-quality workflow runs them. The gate is WIN32 +
    # CVFORWIN_BUILD_TESTS + CVFORWIN_BUILD_TEST_BACKENDS: the release preset
    # sets CVFORWIN_BUILD_TESTS=ON but keeps the test backends OFF, so this
    # target is never registered in the release or any hosted no-camera preset.
    # This file only builds and registers the owner's source; it never modifies
    # it.

    if(WIN32 AND CVFORWIN_BUILD_TESTS AND CVFORWIN_BUILD_TEST_BACKENDS)
        add_executable(cvf102_windows_uvc_tests
            tests/windows/uvc/cvf102_uvc_worker_lifecycle_test.cpp
        )
        target_include_directories(cvf102_windows_uvc_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(cvf102_windows_uvc_tests PRIVATE
            cvforwin_uvc_windows
            opencv_core
            Catch2::Catch2WithMain
        )
        set_target_properties(cvf102_windows_uvc_tests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        cvforwin_apply_test_compile_options(cvf102_windows_uvc_tests)
        cvforwin_apply_sanitizers(cvf102_windows_uvc_tests)
        catch_discover_tests(cvf102_windows_uvc_tests)
    endif()
endif()

# ---------------------------------------------------------------------------
# Opt-in UVC hardware smoke suite (Windows-only, never a CI gate)
# ---------------------------------------------------------------------------
#
# tests/hardware/uvc_smoke.cpp is authored and hash-pinned by the test owner
# and is built only when CVFORWIN_BUILD_HARDWARE_TESTS=ON on Windows. It is
# never registered with CTest and never added to a workflow: hosted CI has no
# camera, and the suite runs manually on a designated Windows station. The
# earlier branch reports the option on other platforms instead of silently
# creating a target that cannot compile.

if(CVFORWIN_BUILD_HARDWARE_TESTS)
    if(WIN32)
        add_executable(cvf_hw_uvc_smoke tests/hardware/uvc_smoke.cpp)
        target_include_directories(cvf_hw_uvc_smoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
        )
        target_link_libraries(cvf_hw_uvc_smoke PRIVATE
            cvforwin_uvc_windows
            opencv_core
        )
        set_target_properties(cvf_hw_uvc_smoke PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        cvforwin_apply_test_compile_options(cvf_hw_uvc_smoke)
        cvforwin_apply_sanitizers(cvf_hw_uvc_smoke)
    else()
        message(STATUS "cvforwin: CVFORWIN_BUILD_HARDWARE_TESTS is ignored on non-Windows builds")
    endif()
endif()

# ---------------------------------------------------------------------------
# Canonical quality targets (always available, even without tests enabled)
# ---------------------------------------------------------------------------

cvforwin_add_format_check_target()
cvforwin_add_abi_check_target(cvforwin)
cvforwin_add_static_analysis_target()
cvforwin_add_package_targets()
