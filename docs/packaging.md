# Packaging and release

Packages are produced only on the pinned windows-2022 runner
(`.github/workflows/windows.yml`). The `package` and `package-verify` targets
exist on other hosts but refuse with a non-zero exit so a portable invocation
can never be mistaken for release evidence.

## Package layout

```
cvforwin-1.1.0/
  bin/cvforwin.dll              # the one project runtime DLL
  lib/cvforwin.lib              # import library
  include/cvforwin/cvf_api.h    # frozen public C ABI v1 header
  config/examples/              # deployment template: cvforwin.json, recipes/*.json, assets/*
  docs/                         # usage, build, and packaging documentation
  LICENSES/                     # third-party notices and the component mapping
```

The staged directory is also archived as `cvforwin-1.1.0.zip`; only that ZIP and
the separate `cvforwin-1.1.0-symbols.zip` (release PDB) are CI artifacts. The
PDB never enters the package.

## What the targets do

- `package` stages the tree above from the build outputs for the requested
  configuration and creates the ZIP. Script-mode execution expands no generator
  expressions: the configuration and resolved output paths are passed in.
- `package-verify` checks the staged package and fails on:
  - missing DLL, import library, header, example configuration/recipes, docs,
    or notices;
  - a `bin/` runtime DLL other than `cvforwin.dll`;
  - a packaged DLL or import library that does not match the built output;
  - an export set other than the exactly five names in `src/c_api/cvforwin.def`;
  - a direct import of `cvforwin.dll` that is not a Windows system DLL
    (dependency allowlist);
  - a missing upload ZIP.
- `package-symbols` copies the release PDB into
  `cvforwin-1.1.0-symbols.zip` (see `<build>/symbols/`).

The release requirement is one runtime DLL with no non-system runtime DLL
dependency; the MSVC runtime is statically linked and OpenCV, nlohmann-json,
spdlog, and codec libraries are static. `LICENSES/README.md` maps every
statically linked third-party component to its license and notice file.

## Clean package consumer

`tests/package/` is an independent kit maintained by the test owner. It
verifies an installed package (contents, exactly five exports, notices, runtime
dependency allowlist) and then configures, builds, and runs a C11 consumer
against it using only the installed header and import library:

```
cmake -DCVF_PACKAGE_ROOT=<absolute installed package root> -P tests/package/package_checks.cmake
```

The consumer uses the synthetic backend, so CI points `CVF_PACKAGE_ROOT` at the
test-enabled package variant built by the `windows-msvc-tests` preset
(`build/windows-msvc-tests/package/cvforwin-1.1.0`). Release packages stay
`uvc`-only.

## Release flow

1. `cmake --workflow --preset windows-ci` (release): configure, build, ctest,
   abi-check, package, package-verify, symbols archive.
2. `cmake --workflow --preset windows-tests-quality`: test-enabled
   configure/build/ctest (camera/ABI/lifecycle), abi-check, package, and
   package-verify of the test-enabled variant.
3. `tests/package/package_checks.cmake` against that variant.
4. Upload the verified release ZIP and the symbols ZIP only.

Hardware evidence is separate: the opt-in UVC smoke suite
(`CVFORWIN_BUILD_HARDWARE_TESTS=ON`) runs manually on a designated Windows
10/11 station and is required before production FCT deployment, never as a
hosted-CI gate.
