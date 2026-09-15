# Developing cvforwin

This manual is for contributors and integrators who work on this repository:
module structure, presets and quality gates, test policy, extension guides for
inspection algorithms and camera backends, error-code conventions, diagnostics
and artifact behaviour, packaging/CI, and the guardrails a change must respect.

It complements the sibling documents and deliberately does not repeat them:

| Document | Read it for |
| --- | --- |
| [usage.md](usage.md) | Public C API lifecycle, configuration and recipe schema, example host |
| [build.md](build.md) | Prerequisites in brief, preset summary, build options |
| [packaging.md](packaging.md) | Package layout, verification steps, release flow, symbols |

## 1. Overview and architecture

cvforwin is one Windows x64 visual-inspection DLL with a frozen C ABI v1. A
native C host calls `cvf_initialize` once, `cvf_inspect` once per cycle, and
`cvf_shutdown`; the DLL owns camera acquisition, recipe selection, OpenCV
processing, diagnostics, result encoding, and optional image persistence.

### Module map and dependency direction

Dependencies flow one way: `public_c_api -> runtime_orchestrator -> modules ->
core_model`. Leaf modules never depend back on the public API or the runtime.

```
public_c_api                        include/cvforwin/cvf_api.h, src/c_api/**
  |
  v
runtime_orchestrator                src/runtime/**
  |-> camera_contract               src/camera/**
  |     |-> uvc_windows_backend     src/camera/uvc_windows/**   (WIN32 only)
  |     |-> test_camera_backends    src/camera/test_backends/** (test-enabled only)
  |-> recipe_store                  src/recipes/**
  |-> inspection_contract_and_registry  src/inspection/**
  |     |-> compiled_algorithms     src/algorithms/**
  |-> diagnostics_and_artifacts     src/diagnostics/**, src/artifacts/**
  |-> core_model                    src/core/** (also reached by every module)
```

| Module | Paths (CMake target) | Responsibility in one line |
| --- | --- | --- |
| `core_model` | `src/core/**` (`cvforwin_core`) | Internal status, verdict, warning, deadline, identifier, result, and text/ABI-validation types with no platform or ABI leakage. |
| `camera_contract` | `src/camera/camera_backend.*`, `src/camera/captured_frame.h` (`cvforwin_camera`) | Backend-neutral descriptor/selector/settings values, `ICameraBackend`, unique identity resolution, and bounded capture-with-one-retry. |
| `uvc_windows_backend` | `src/camera/uvc_windows/**` (`cvforwin_uvc_windows`) | Windows Media Foundation UVC enumeration, exactly-one identity resolution, open/configure/capture/reconnect/close; Media Foundation system libraries only. |
| `test_camera_backends` | `src/camera/test_backends/**` (`cvforwin_test_backends`) | Deterministic file and synthetic frames plus fault injection; built only when `CVFORWIN_BUILD_TEST_BACKENDS=ON`. |
| `recipe_store` | `src/recipes/**` (`cvforwin_recipes`) | Strict versioned parsing of the global configuration and recipe documents, immutable catalog, atomic snapshot replacement. |
| `inspection_contract_and_registry` | `src/inspection/**` (`cvforwin_inspection`) | `IInspectionAlgorithm`, `AlgorithmRequest`/`AlgorithmResult`, deadline semantics, keyed registry, and dispatch containment. |
| `compiled_algorithms` | `src/algorithms/**` (`cvforwin_algorithms`) | OpenCV algorithms behind the contract, strict parameter validation, bounded structured results, and the registration point. |
| `diagnostics_and_artifacts` | `src/diagnostics/**`, `src/artifacts/**` (`cvforwin_diagnostics`, `cvforwin_artifacts`) | Rolling log sink and optional callback, managed PNG persistence and retention, optional-failure degradation to warning bits. |
| `runtime_orchestrator` | `src/runtime/**` (`cvforwin_runtime`) | Owns the single process context, camera session, recipe snapshot, registry, diagnostics, and the timed serialization gate; runs one inspection under one deadline. |
| `public_c_api` | `include/cvforwin/cvf_api.h`, `src/c_api/**` (`cvforwin`) | Frozen v1 C ABI, exactly five exports, argument validation before side effects, and exception containment. |
| `build_test_and_delivery` | `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `vcpkg-configuration.json`, `cmake/**`, `tests/**`, `examples/c_host/**`, `.github/workflows/**` | Presets, quality gates, package/symbols targets, and the windows-2022 CI workflows. |

Dependency rules that a change must preserve:

- Only `runtime_orchestrator` constructs concrete backends, recipes, registry,
  diagnostics, and artifacts, and it initializes them in dependency order with
  full rollback on failure.
- Algorithms receive an owned frame, validated parameters, optional input JSON,
  and the remaining deadline only. They never open cameras, select recipes, save
  files, call host callbacks, or touch the C ABI.
- Camera backends acquire frames only; they never evaluate product verdicts or
  know recipe algorithms.
- Configuration models are internal types and are never aliases of public ABI
  structures.
- Windows-only includes and link libraries stay inside `uvc_windows_backend`
  and the Windows delivery checks. Portable translation units must compile
  without Windows headers.

## 2. Repository layout

Two levels; `build/` is generated output and never edited.

```
cvforwin/
  .ai/                     project contract, test-ownership manifest, reports
  .github/workflows/       windows-ci workflow definition
  cmake/                   helper modules: options, format, abi, analysis, package
  config/examples/         deployment template: cvforwin.json + recipes/*.json
  docs/                    README, usage, build, packaging, development
  examples/c_host/         native C11 host template, config, run_example.sh
  include/cvforwin/        cvf_api.h - the frozen public C ABI v1 header
  src/
    algorithms/            compiled algorithms and registry wiring
    artifacts/             managed capture persistence and retention
    c_api/                 exported entry points, context bridge, cvforwin.def
    camera/                camera contract
      test_backends/       file, synthetic, and fault-injection support
      uvc_windows/         Media Foundation UVC backend (WIN32 only)
    core/                  internal value types and utilities
    diagnostics/           rolling log sink and callback fan-out
    inspection/            algorithm contract, registry, dispatch
    recipes/               global config, recipes, immutable catalog
    runtime/               context orchestrator (single context)
  tests/
    abi/                   independent C ABI and header tests
    camera/                independent camera-contract tests
    unit/                  developer unit tests + test-owner CVF suites
    package/               independent clean package-consumer kit
    hardware/              opt-in Windows-only UVC smoke suite
    fixtures/              test-owner runtime fixtures
  LICENSES/                third-party notices and component mapping
  CMakeLists.txt CMakePresets.json vcpkg.json vcpkg-configuration.json
  .clang-format .clang-tidy
```

## 3. Prerequisites

Common:

- CMake 3.28 or newer (`CMakePresets.json` uses preset schema 6 and
  `cmake_minimum_required(VERSION 3.28)`).
- vcpkg in manifest mode with `VCPKG_ROOT` exported. `vcpkg.json` and
  `vcpkg-configuration.json` pin the dependency graph to the immutable baseline
  `a1cae005c39be7b18ba319fced856b68d7276271`; floating refs are forbidden.
  Verify the checkout with:

  ```sh
  git -C "$VCPKG_ROOT" rev-parse HEAD
  # must print a1cae005c39be7b18ba319fced856b68d7276271
  ```

- Network access on the first configure so vcpkg can fetch the baseline ports
  (later rounds reuse the vcpkg binary cache; see section 11).

WSL (the portable development host):

- GCC 12 or newer (`gcc`, `g++`).
- Ninja.
- clang-format and clang-tidy. Both are found with `REQUIRED` while configuring
  the format-check/static-analysis targets, so a missing tool fails configure.
- `nm` (binutils) for the portable `abi-check` target.

Windows (the authoritative DLL host):

- Visual Studio 2022 with the MSVC v143 toolset and x64 tools.
- A vcpkg checkout with `VCPKG_ROOT` set (the windows-2022 CI image ships
  `C:\vcpkg`).
- `dumpbin` next to the linker or on `PATH` for the Windows `abi-check` and
  `package-verify` targets.

## 4. Build and test

All entry points are CMake presets. The lists below are exactly what
`cmake --list-presets`, `--list-presets=build`, `--list-presets=test`, and
`--list-presets=workflow` report in this repository.

### Configure presets

| Preset | Purpose |
| --- | --- |
| `wsl-gcc-debug` | Canonical portable Debug build; GCC + Ninja, tests and test backends ON, warnings-as-errors ON |
| `wsl-gcc-asan` | Same as above plus AddressSanitizer + UndefinedBehaviorSanitizer |
| `wsl-clang-analysis` | Clang build tree for `static-analysis` (compile database + clang-tidy) |
| `windows-msvc-release` | MSVC v143 Release x64, static CRT, test backends OFF, tests ON |
| `windows-msvc-tests` | MSVC v143 x64 test-enabled (camera/ABI/lifecycle suites), test backends ON |

```sh
cmake --preset wsl-gcc-debug
cmake --preset wsl-gcc-asan
cmake --preset wsl-clang-analysis
cmake --preset windows-msvc-release
cmake --preset windows-msvc-tests
```

### WSL build, test, and gate commands

```sh
cmake --preset wsl-gcc-debug
cmake --build --preset wsl-gcc-debug --parallel
ctest --preset wsl-gcc-debug --output-on-failure

cmake --build --preset wsl-gcc-debug-format-check
cmake --build --preset wsl-gcc-debug-abi-check
cmake --build --preset wsl-gcc-debug-static-analysis

cmake --preset wsl-gcc-asan
cmake --build --preset wsl-gcc-asan --parallel
ctest --preset wsl-gcc-asan --output-on-failure

cmake --preset wsl-clang-analysis
cmake --build --preset wsl-clang-analysis --parallel
cmake --build --preset wsl-clang-analysis --target static-analysis --parallel
# equivalent dedicated build preset:
cmake --build --preset wsl-clang-analysis-static-analysis
```

What each gate checks:

| Gate | What it verifies |
| --- | --- |
| `ctest` (`wsl-gcc-debug`) | Full portable test set: ABI, camera contract, recipes, algorithms, diagnostics/artifacts, runtime, developer tests |
| `wsl-gcc-debug-format-check` | clang-format in check mode over `src/`, `include/`, `tests/unit/`, and `examples/c_host/`; test-owner trees (`tests/abi`, `tests/camera`, `tests/package`, `tests/hardware`) are deliberately not reformatted |
| `wsl-gcc-debug-abi-check` | Exactly the five `cvf_*` symbols are exported (Linux: `nm -D`); the allowlist source of truth is `src/c_api/cvforwin.def` |
| `wsl-gcc-debug-static-analysis` / `wsl-clang-analysis-static-analysis` | clang-tidy over project sources using `compile_commands.json`; every finding is an error |
| `wsl-gcc-asan` + `ctest --preset wsl-gcc-asan` | Sanitizer build and test run (ASan + UBSan) |

The canonical single-command WSL chain is the workflow preset:

```sh
cmake --workflow --preset wsl-quality
```

It configures/ builds/ tests `wsl-gcc-debug` and then runs the format-check,
abi-check, and static-analysis targets against that one build tree.

### Windows build, test, and package commands

Release (the UVC-only variant behind the package):

```sh
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release --config Release --parallel
ctest --preset windows-msvc-release --output-on-failure
cmake --build --preset windows-msvc-release-abi-check
cmake --build --preset windows-msvc-release-package
cmake --build --preset windows-msvc-release-package-verify
cmake --build --preset windows-msvc-release-symbols
cmake --workflow --preset windows-ci
```

Test-enabled (the variant the clean package consumer uses; accepts `file` and
`synthetic` camera backends):

```sh
cmake --preset windows-msvc-tests
cmake --build --preset windows-msvc-tests --config Release --parallel
ctest --preset windows-msvc-tests --output-on-failure
cmake --build --preset windows-msvc-tests-abi-check
cmake --build --preset windows-msvc-tests-package
cmake --build --preset windows-msvc-tests-package-verify
cmake --workflow --preset windows-tests-quality
```

| Windows gate | What it verifies |
| --- | --- |
| `windows-msvc-release` + `ctest` | Release compile with `/W4 /WX`, test suites that need no test backends |
| `windows-msvc-tests` + `ctest` | Camera/ABI/lifecycle suites with `CVFORWIN_BUILD_TEST_BACKENDS=ON` |
| `*-abi-check` | Exactly five `cvf_*` exports from the built DLL (`dumpbin`) |
| `*-package` | Stages `build/<preset>/package/cvforwin-1.0.0/` and builds the ZIP; Windows-only, refuses non-zero on portable hosts |
| `*-package-verify` | Package contents, five exports, system-only runtime DLL dependency allowlist, notices, upload ZIP |
| `windows-msvc-release-symbols` | Copies the release PDB into a separate symbols ZIP; the PDB never enters the package |
| `windows-ci` (workflow) | Release chain: configure, build, ctest, abi-check, package, package-verify, symbols |
| `windows-tests-quality` (workflow) | Test-enabled chain: configure, build, ctest, abi-check, package, package-verify |

GCC/Clang builds are authoritative for portable compilation and unit behaviour
only; Windows MSVC is authoritative for the DLL, exports, runtime dependencies,
and packaging. See [build.md](build.md) for the build options and
[packaging.md](packaging.md) for the package/release flow.

## 5. Test suites and policy

### Where tests live

| Suite | Location | Covers |
| --- | --- | --- |
| ABI | `tests/abi/` | `test_c11_surface.c` (C11 header surface), `test_abi_runtime.c` (struct_size/abi_version/reserved/capacity/context validation), `test_cpp20_include.cpp` (C++20 inclusion), `test_header_isolation.c` (self-containment with `-nostdinc`, GCC/Clang only), `cvf006_lifecycle.c` (end-to-end C lifecycle over the synthetic backend) |
| Camera | `tests/camera/` | Identity resolution (exactly one match, no first-device fallback), file and synthetic backends, fault injection, bounded one-reconnect/one-recapture |
| Unit | `tests/unit/` | Developer tests (core, recipes, algorithms, diagnostics/artifacts, runtime, UVC factory) plus the test-owner CVF-003/004/005/006 suites for registry/dispatch, config/recipe/catalog, diagnostics/capture policy/retention, and runtime deadline/reconnect/save-policy/rollback/serialization |
| Package | `tests/package/` | Independent clean-consumer kit: `package_checks.cmake` verifies an installed package and configures/builds/runs a C11 consumer using only the installed header and import library |
| Hardware | `tests/hardware/` | Opt-in `uvc_smoke.cpp`; Windows-only, never registered with CTest, never a CI gate |
| Fixtures | `tests/fixtures/runtime/` | Test-owner runtime fixture config and recipes consumed by the CVF-006 suites |

`tests/camera` and the CVF-006 suites are registered only when
`CVFORWIN_BUILD_TEST_BACKENDS=ON` (the `wsl-*` and `windows-msvc-tests`
presets). The release preset runs the remaining suites without test backends.

### Independence rules

- Test author and production implementer are different roles. Every
  test-owner-authored file is recorded with a sha256 in
  `.ai/test-ownership.yaml` (sections CVF-001 through CVF-008) and is
  hash-pinned.
- Never edit a hash-pinned file, and never "fix" a test to match an
  implementation. A hash mismatch at verification time is an independence
  violation. This includes `tests/abi/**`, `tests/camera/**`,
  `tests/unit/cvf00*_*.{cpp,h}`, `tests/fixtures/runtime/**`,
  `tests/package/**`, and `tests/hardware/**` entries listed in the manifest.
- To check whether a file is pinned and unmodified:

  ```sh
  grep -n "tests/abi/test_c11_surface.c" .ai/test-ownership.yaml
  sha256sum tests/abi/test_c11_surface.c
  # compare the printed hash with the recorded sha256 value
  ```

- Production files under `src/`, `include/`, and the CMake/delivery files may
  only be changed by the implementer; test-owner files may only be changed by
  the test owner (and only with a recorded manifest amendment).

### Running a subset

`ctest -R` filters by test-name regular expression; `-N` lists without running:

```sh
ctest --preset wsl-gcc-debug -N                      # list every registered test
ctest --preset wsl-gcc-debug -R "cvf\.abi\." --output-on-failure
ctest --preset wsl-gcc-debug -R "CVF-006" --output-on-failure
ctest --preset wsl-gcc-debug -R "example\.threshold" --output-on-failure
```

Catch2 test cases can also be selected by passing a name pattern to the test
executable. The pattern matches the test-case name, so run it against the
target that owns those cases:

```sh
build/wsl-gcc-debug/cvf_unit_tests "example.threshold*"   # developer unit tests
build/wsl-gcc-debug/cvf003_tests "CVF-003*"               # independent CVF-003 suite
```

The `example.threshold` cases live in `tests/unit/test_inspection_contract.cpp`
on the `cvf_unit_tests` target; the independent CVF-003 cases are named
`CVF-003 ...`.

## 6. Adding an inspection algorithm

1. Implement the contract from `src/inspection/algorithm.h`:

   ```cpp
   class MyAlgorithm final : public cvforwin::inspection::IInspectionAlgorithm {
   public:
       std::string_view key() const noexcept override;            // [a-z0-9._-]{1,64}, unique
       core::Result<void> validate_parameters(const nlohmann::json&) const override;
       core::Result<inspection::AlgorithmResult> inspect(const inspection::AlgorithmRequest&) override;
   };
   ```

   Put the header/source in `src/algorithms/` and add the source to the
   `cvforwin_algorithms` target in `CMakeLists.txt`.

2. Validate parameters strictly in `validate_parameters`: reject non-objects,
   wrong types, out-of-range values, and unknown keys with
   `algorithm_parameters_invalid` (broad status `config_error`); reject unknown
   `input_json` keys with `algorithm_input_invalid`.

3. Produce bounded output. `measurements` plus `defects` must serialize to at
   most 65536 bytes (`k_max_result_json_bytes` in `src/inspection/registry.h`);
   dispatch otherwise fails with `algorithm_output_too_large`.

4. Honor the deadline. Check `request.deadline` before expensive work and
   between stages; an exceeded deadline is `algorithm_deadline_exceeded` (broad
   status `timeout`). The runtime passes the remaining end-to-end budget, so
   the algorithm must never sleep past it.

5. Register the algorithm in `src/algorithms/compiled_algorithms.cpp`:

   ```cpp
   return registry.add(std::make_unique<MyAlgorithm>());
   ```

   Registration is compile-time only; there is no runtime plug-in loading.
   Duplicate keys are rejected at registration.

6. Add a recipe document so the algorithm can be selected. Recipes live under
   `<config_root>/recipes/` and must carry `schema_version: 1`, a unique
   `recipe_id`, `algorithm` equal to the registry key, and a `parameters`
   object. Start from `config/examples/recipes/` and see
   [usage.md](usage.md) for the full schema.

7. Add tests. Developer unit tests are Catch2 v3 sources under `tests/unit/`
   registered on the `cvf_unit_tests` target in `CMakeLists.txt` (or a
   dedicated developer target). Cover parameter validation, deterministic
   output on a synthesized frame, and deadline behaviour. Synthesize the config
   and recipe documents in a per-test temporary directory (see
   `tests/unit/cvf006_test_support.h`); do not modify the hash-pinned fixtures.

Rules that always apply: an algorithm never opens a camera, selects a recipe,
persists a file, invokes a host callback, or touches the public ABI. Dispatch
catches every escaped exception (`algorithm_exception`), so an algorithm must
still be written not to leak exceptions.

## 7. Adding a camera backend

1. Implement `camera::ICameraBackend` from `src/camera/camera_backend.h`
   (`backend_key`, `enumerate`, `open`, `capture`, `reconnect`, `close`) and add
   the sources to the appropriate CMake target.

2. Follow the descriptor and identity rules:

   - `enumerate()` returns `CameraDescriptor` values (backend key, device path,
     vendor/product id, friendly name). A field is either a real value or
     empty; matching is exact and case-sensitive.
   - Identity resolution must go through
     `camera::resolve_identity(candidates, selector)`: an empty selector fails
     with `selector_empty`, zero matches with `camera_not_found`, and two or
     more with `camera_identity_ambiguous`. There is never a
     first-device fallback.

3. Implement capture and reconnect against the remaining deadline and return an
   owned BGR8 `CapturedFrame`. Use `camera::capture_with_one_retry` for the
   bounded contract: one capture, and on a `camera_io` failure at most one
   reconnect plus one recapture; other failures are returned unchanged.

4. Wire the backend into the runtime factory. Config-driven backend selection
   lives in `build_backend()` in `src/runtime/context.cpp`; add the new
   `camera.backend` key there and the matching build gating. The release
   configuration accepts `uvc`; `file` and `synthetic` exist only when
   `CVFORWIN_BUILD_TEST_BACKENDS=ON`.

5. Keep real SDK code Windows-only. Vendor/SDK includes, link libraries, and
   `#if defined(_WIN32)` guards belong to the backend module; Windows system
   libraries only (Media Foundation for UVC). Portable builds must neither
   compile nor link the Windows backend. A new vendor SDK is an out-of-scope
   architecture change, not a routine backend addition.

6. Test the backend contract through the deterministic file and synthetic
   backends and the shared fault injection in
   `src/camera/test_backends/test_backend_support.h`
   (`inject_failure`, `inject_delay`, call counters). Hosted CI has no camera,
   so mandatory tests must stay hardware-free.

## 8. Configuration and recipes

Every deployment has one config root containing `cvforwin.json` and a
`recipes/` directory. The full schema, defaults, and bounds are in
[usage.md](usage.md); the important structural rules are:

- `schema_version` is `1` for both the global config and every recipe; any
  other value is `config_schema_version` or `recipe_schema_version`.
- Unknown keys, duplicate keys (including nested objects), invalid UTF-8, wrong
  types, and out-of-range values are rejected before anything is activated
  (codes 1600-1607 for config, 1610-1623 for recipes).
- `config_root` and `output_root` must be absolute directories;
  `output_root/logs/` receives logs and `output_root/captures/` receives
  persisted images.
- `camera.backend` is `uvc` in the release package; `file` and `synthetic` are
  accepted only by test-enabled builds.
- Recipes are validated as a complete candidate set; `cvf_reload_recipes`
  swaps the snapshot atomically and keeps the previous set on any error
  (`runtime_reload_failed`, broad status `config_error`).

## 9. Error codes, statuses, and warnings

Every internal failure is a `core::Failure` coupling a broad public `Status`
with a stable `ErrorCode` (`src/core/error.h`) and a bounded message. Error
codes are never zero for a failure.

| Range | Subsystem |
| --- | --- |
| 1000-1013 | ABI and argument validation (pointers, struct_size, abi_version, reserved, flags, text, paths, request) |
| 1100-1101 | Context lifecycle |
| 1200-1204 | Initialization and required subsystems |
| 1300-1301 | Internal exceptions/unexpected states |
| 1400-1409 | Camera |
| 1500-1508 | Algorithms |
| 1600-1607 / 1610-1623 | Global configuration / recipes |
| 1700-1701 | Diagnostics |
| 1800-1803 | Artifacts |
| 1900-1904 | Runtime orchestrator |

Mapping rule: `core::status_for()` in `src/core/error.cpp` is the single
authority and maps each code individually (the mapping is by code, not a
mechanical range function). Notable patterns:

- 1000-1013 map to `invalid_argument`, except `abi_version_mismatch` (1005)
  which maps to `abi_mismatch`.
- Camera: not found/ambiguous/descriptor mismatch (1400, 1401, 1409) map to
  `camera_not_found`; open/capture/frame failures (1402-1404, 1406) to
  `camera_io`; capture/deadline timeouts (1405, 1407) to `timeout`;
  `selector_empty` (1408) to `invalid_argument`.
- Algorithms: 1500, 1501, 1504, 1506, 1507 map to `algorithm_error`;
  `algorithm_parameters_invalid` (1503) to `config_error`;
  `algorithm_key_invalid`/`algorithm_input_invalid` (1502, 1508) to
  `invalid_argument`; `algorithm_deadline_exceeded` (1505) to `timeout`.
- Recipes: the 1600-1623 family maps to `config_error`, except
  `recipe_not_found` (1622) which maps to `recipe_not_found`.
- Artifacts: `artifacts_root_error` (1800) to `config_error`; encode/write/
  retention (1801-1803) to `internal_error`.
- Runtime: `runtime_queue_timeout` (1900) to `timeout`,
  `runtime_result_too_large` (1901) to `buffer_too_small`,
  `runtime_required_artifact_failed` (1902) to `required_artifact_error`,
  `runtime_reload_failed` (1903) to `config_error`, `runtime_context_closed`
  (1904) to `invalid_context`.
- A non-OK status always reports `CVF_VERDICT_NOT_EVALUATED`; a technical error
  is never reported as `CVF_VERDICT_FAIL`.

Warning bits occupy the low two bits of the public `warning_flags` field
(`src/core/warnings.h`):

| Bit | Name | Meaning |
| --- | --- | --- |
| `1u << 0` | `warning_log_sink_failed` | An optional log sink could not be opened or written; diagnostics degraded without failing the call |
| `1u << 1` | `warning_image_save_failed` | An optional image save failed but the inspection result stands |

Optional versus required artifacts: an optional sink or save failure only sets
the warning bit and never changes status or verdict. When a recipe sets
`artifacts.required: true`, an image-save failure becomes
`CVF_STATUS_REQUIRED_ARTIFACT_ERROR` (error code 1902) with verdict
`NOT_EVALUATED`; the failure is no longer contained.

## 10. Diagnostics and artifact behaviour

- Logging: one size-rotating file sink at `<output_root>/logs/cvforwin.log`
  (`logging.level`, `logging.max_file_bytes`, `logging.max_files`) plus an
  optional synchronous C callback when
  `CVF_INIT_FLAG_CALLBACK_LOGGING` is set and a callback is supplied. A file
  sink that cannot be opened degrades to callback-only operation and sets
  warning bit 0. Messages are truncated to a UTF-8-safe prefix of at most 4096
  bytes.
- Capture policy: per-recipe `artifacts.save_policy` is `always`,
  `fail_or_error`, or `never`. `fail_or_error` saves the frame when execution
  failed or the verdict is FAIL (the default deployment template uses it).
- Persistence: frames are encoded as PNG and written as direct children of
  `<output_root>/captures/` with sanitized, bounded names; identifiers can
  never escape the managed root.
- Retention: `retention.max_age_days` and `retention.max_total_bytes` drive a
  non-recursive age-then-size cleanup over regular files only, oldest first;
  links and other entry types are never followed.
- Warning bits are reported per call in `cvf_inspection_result_v1.warning_flags`
  (see section 9).

## 11. Packaging and CI

The `package` and `package-verify` targets are Windows-only by design: on
portable hosts they are defined as refusal targets that fail with a non-zero
exit, so a WSL invocation can never be mistaken for release evidence. The
`package-symbols` target is Windows-only too and is not defined on portable
hosts at all, so a WSL build of that target fails immediately as an unknown
target.

- `package` stages `build/<preset>/package/cvforwin-1.0.0/` (DLL, import
  library, public header, example config/recipes, `docs/`, `LICENSES/`) and
  creates `cvforwin-1.0.0.zip`.
- `package-verify` checks the staged tree: required files, no second runtime
  DLL, exactly five exports, a system-only direct-import allowlist, and the
  upload ZIP.
- `package-symbols` archives the release PDB separately as
  `cvforwin-1.0.0-symbols.zip`; the PDB never enters the package.

CI runs two jobs on the pinned `windows-2022` runner
(`.github/workflows/windows.yml`, workflow name `windows-ci`):

| Job | What it does | Uploaded artifacts |
| --- | --- | --- |
| `windows-release` | `cmake --workflow --preset windows-ci`: release configure/build/ctest, abi-check, package, package-verify, symbols | `cvforwin-1.0.0-package` (`build/windows-msvc-release/package/cvforwin-1.0.0.zip`) and `cvforwin-1.0.0-symbols` (`build/windows-msvc-release/symbols/cvforwin-1.0.0-symbols.zip`) |
| `windows-tests-consumer` | `cmake --workflow --preset windows-tests-quality` (test-enabled camera/ABI/lifecycle suites, package, package-verify), then `cmake -DCVF_PACKAGE_ROOT=... -P tests/package/package_checks.cmake` against the installed test-enabled package | none (the test-enabled package and consumer scratch tree are never uploaded) |

Both jobs use a job-level `VCPKG_DEFAULT_BINARY_CACHE` plus one
`actions/cache@v4` step keyed by the hashes of `vcpkg.json` and
`vcpkg-configuration.json`, so built ports are restored instead of rebuilt.

The uploaded release package is `uvc`-only. Test-enabled builds (local or the
`windows-tests-consumer` job) additionally accept the deterministic `file` and
`synthetic` backends, which is what lets the camera, ABI, and lifecycle suites
run without hardware. Real UVC hardware evidence is opt-in
(`CVFORWIN_BUILD_HARDWARE_TESTS=ON`) and runs manually on a designated Windows
10/11 station; it is never a hosted-CI gate. See [packaging.md](packaging.md)
for the release flow.

## 12. Conventions and guardrails

- Warnings are errors. `CVFORWIN_WARNINGS_AS_ERRORS` defaults ON: project
  targets compile with `/W4 /permissive- /utf-8` and `/WX` on MSVC or
  `-Wall -Wextra -Werror` on GCC/Clang.
- Formatting is enforced by `format-check` against `.clang-format`
  (LLVM-derived, 4-space indent, `ColumnLimit: 0`, function braces on their own
  line). Static analysis is enforced by `static-analysis` against `.clang-tidy`
  with `WarningsAsErrors: '*'` over the project sources.
- The C ABI is frozen:
  - `include/cvforwin/cvf_api.h` is self-contained C11 (`<stdint.h>` and
    `<stddef.h>` only), includable from C++20 via `extern "C"`.
  - Exactly five exports: `cvf_get_abi_version`, `cvf_initialize`,
    `cvf_reload_recipes`, `cvf_inspect`, `cvf_shutdown`
    (`src/c_api/cvforwin.def` is the authoritative Windows allowlist).
  - No C++ types, OpenCV types, vendor types, or Windows types in exported
    declarations; no exception may cross the boundary.
  - `struct_size` must equal the exact v1 size, `abi_version` must be
    `CVF_ABI_VERSION_V1`, reserved fields must be zero, flags may not contain
    unknown bits.
  - Text buffers follow the `capacity` / `bytes_written` (excluding NUL) /
    `bytes_required` (including NUL) rule; a NULL pointer is legal only with
    capacity 0. Every entry point validates arguments before any observable
    side effect.
  - `status` and `verdict` are independent; a technical error is always
    `CVF_VERDICT_NOT_EVALUATED`.
- Module dependencies follow the direction in section 1; do not add an edge
  from a leaf module back to the public API or runtime, and do not let internal
  config types alias public ABI structures.
- Deadline propagation: one end-to-end deadline covers queue wait, capture,
  the optional reconnect/recapture, algorithm dispatch, encoding, and required
  persistence. Pass and check `core::Deadline` at every stage; a request with
  `timeout_ms = 0` uses the documented 5000 ms default.
- Test ownership: never edit hash-pinned test-owner files (section 5); add
  developer tests in separate files.
- Build artifacts under `build/` are generated and never edited or committed.

## 13. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| Configure fails resolving the vcpkg toolchain | `VCPKG_ROOT` is unset or points at a missing checkout. Export it (`export VCPKG_ROOT="$HOME/vcpkg"` in WSL; `C:\vcpkg` on the CI image) and re-run the preset. |
| vcpkg baseline error during configure | The vcpkg checkout HEAD differs from the pinned baseline. `git -C "$VCPKG_ROOT" rev-parse HEAD` must print `a1cae005c39be7b18ba319fced856b68d7276271`; fetch/check out that commit instead of changing the pin in `vcpkg.json`/`vcpkg-configuration.json`. |
| Configure fails finding `clang-format` or `clang-tidy` | Both are found with `REQUIRED` while configuring. Install them (and `nm`/binutils for `abi-check`) or use a build tree whose configure step already succeeded. |
| `package` or `package-verify` fail on WSL | Expected: both are Windows-only refusal targets and exit non-zero by design on portable hosts. Run them from the Windows presets or rely on CI. |
| `package-symbols` fails on WSL as an unknown target | Expected: unlike `package`/`package-verify`, the symbols target is not defined on portable hosts. Run it only from the Windows presets. |
| Camera/lifecycle tests are not found | They require `CVFORWIN_BUILD_TEST_BACKENDS=ON`. Use `wsl-gcc-debug`/`wsl-gcc-asan` or `windows-msvc-tests`; the release preset intentionally does not register them. |
| WSL GCC build is green but the Windows CI build fails | MSVC-only diagnostics that GCC accepts, typically under `/W4 /WX`: narrowing conversions in braced initialization (C2397, promoted to C2220), constant truncations/casts (C4310-class), deprecated CRT calls such as `fopen` (C4996 to C2220), and `override` on a COM-derived destructor (C3668). Fix the source with explicit `static_cast` conversions, the scoped `_CRT_SECURE_NO_WARNINGS` opt-out where already justified, or drop the invalid `override`; do not weaken warnings project-wide. The authoritative check is the Windows presets in CI. |
| Where are the CI logs and artifacts? | GitHub Actions runs of the `windows-ci` workflow (push, pull_request, workflow_dispatch): logs per job (`windows-release`, `windows-tests-consumer`) and the artifacts `cvforwin-1.0.0-package` and `cvforwin-1.0.0-symbols`. Locally, test logs are under `build/<preset>/Testing/Temporary/` and package/symbols outputs under `build/windows-msvc-*/{package,symbols}/`. |
