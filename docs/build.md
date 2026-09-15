# Building cvforwin

Dependencies are managed in vcpkg manifest mode. `vcpkg.json` plus
`vcpkg-configuration.json` pin the dependency graph to the immutable baseline
`a1cae005c39be7b18ba319fced856b68d7276271`; floating refs are not used.

## Prerequisites

Common:

- CMake 3.28 or newer.
- `VCPKG_ROOT` pointing at a vcpkg checkout compatible with the pinned baseline
  (`export VCPKG_ROOT=$HOME/vcpkg` in WSL; the windows-2022 CI image ships
  `C:\vcpkg`).
- Network access on the first configure so vcpkg can fetch the baseline ports.

WSL (GCC 12+, Ninja, clang-format, clang-tidy):

```
cmake --preset wsl-gcc-debug
cmake --build --preset wsl-gcc-debug --parallel
ctest --preset wsl-gcc-debug --output-on-failure
cmake --build --preset wsl-gcc-debug --target format-check
cmake --build --preset wsl-gcc-debug --target abi-check
cmake --preset wsl-clang-analysis
cmake --build --preset wsl-clang-analysis --target static-analysis --parallel
```

The sanitizer preset mirrors the debug chain:

```
cmake --preset wsl-gcc-asan
cmake --build --preset wsl-gcc-asan --parallel
ctest --preset wsl-gcc-asan --output-on-failure
```

`cmake --workflow --preset wsl-quality` runs the canonical WSL chain in one
invocation.

Windows (Visual Studio 2022, MSVC v143, x64 tools):

| Preset | Purpose | Test backends |
| --- | --- | --- |
| `windows-msvc-release` | release build, tests, ABI check, package, package-verify | OFF; the release package is `uvc`-only |
| `windows-msvc-tests` | full test-enabled suite (camera/ABI/lifecycle) | ON; produces the variant the clean package consumer needs |
| `windows-ci` (workflow) | canonical release workflow | OFF |
| `windows-tests-quality` (workflow) | test-enabled workflow + package variant | ON |

```
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release --config Release --parallel
ctest --preset windows-msvc-release --output-on-failure
cmake --build --preset windows-msvc-release --config Release --target abi-check
cmake --build --preset windows-msvc-release --config Release --target package
cmake --build --preset windows-msvc-release --config Release --target package-verify
```

GCC/Clang builds are authoritative for portable compilation and unit behaviour
only; Windows MSVC is authoritative for the DLL, exports, runtime dependencies,
and packaging.

## Test backends

`CVFORWIN_BUILD_TEST_BACKENDS` defaults ON for Debug-style portable presets and
OFF for release configurations. Test-enabled builds additionally accept the
deterministic `file` and `synthetic` camera backends so the camera, ABI, and
lifecycle suites run without hardware. Release packages never contain them.

The C host example is part of the default build on every platform
(`cvf_c_host_example`, output under `<build>/examples/c_host/`); see
[usage.md](usage.md) for running it against the synthetic configuration.

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `CVFORWIN_BUILD_SHARED` | ON | build the shared library / DLL |
| `CVFORWIN_BUILD_TESTS` | ON | build unit, ABI, and camera suites |
| `CVFORWIN_BUILD_TEST_BACKENDS` | preset-dependent | build the file/synthetic/fault camera backends |
| `CVFORWIN_WARNINGS_AS_ERRORS` | ON | `-Werror` / `/WX` for project targets |
| `CVFORWIN_ENABLE_CLANG_TIDY` | OFF | run clang-tidy at build time |
| `CVFORWIN_ENABLE_SANITIZERS` | OFF | Address/UB sanitizers (portable builds only) |
| `CVFORWIN_BUILD_HARDWARE_TESTS` | OFF | opt-in Windows-only UVC hardware smoke suite, never a CI gate |
