# Third-party license notices

This directory is the auditable notice set for the cvforwin 1.0.0 release. The
release package copies this directory verbatim, and the `package-verify` target
fails when the directory is missing or contains no `LICENSE`/`NOTICE`/`COPYING`
named, non-empty file.

All components below are managed in vcpkg manifest mode. The dependency graph is
pinned by `vcpkg.json` and `vcpkg-configuration.json` (immutable builtin
baseline `a1cae005c39be7b18ba319fced856b68d7276271`, registry
`https://github.com/microsoft/vcpkg`). The license texts were copied from the
vcpkg install trees (`vcpkg_installed/<triplet>/share/<port>/copyright`) for
that baseline.

## Component mapping

| Component | Version | License (SPDX) | Role | Linkage in the DLL | Notice file |
| --- | --- | --- | --- | --- | --- |
| OpenCV (`opencv4`, features `fs`, `jpeg`, `png`, `thread`) | 4.12.0 | Apache-2.0 | Internal image model and algorithm operations (`core`, `imgproc`, `imgcodecs`) | Static | `OpenCV-LICENSE.txt` |
| nlohmann-json | 3.12.0 | MIT | Strict JSON parsing for configuration, recipes, and bounded algorithm I/O | Static | `nlohmann-json-LICENSE.txt` |
| spdlog | 1.17.0 | MIT | Rolling file logging | Static | `spdlog-LICENSE.txt` |
| fmt (spdlog's formatting backend) | 12.2.0 | MIT | Formatting compiled into spdlog | Static | `fmt-LICENSE.txt` |
| libjpeg-turbo (OpenCV `jpeg` feature) | 3.2.0 | BSD-style (IJG + BSD-3-Clause) | JPEG codec statically linked into OpenCV `imgcodecs` | Static | `libjpeg-turbo-LICENSE.txt` |
| libpng (OpenCV `png` feature) | 1.6.58 | libpng-2.0 | PNG codec statically linked into OpenCV `imgcodecs` | Static | `libpng-LICENSE.txt` |
| zlib (OpenCV + libpng) | 1.3.2 | Zlib | Deflate compression statically linked into OpenCV | Static | `zlib-LICENSE.txt` |
| Catch2 v3 | 3.16.0 | BSL-1.0 | Test-only framework (developer and independent suites) | Test binaries only; never redistributed | `Catch2-LICENSE.txt` |

## Notes

- The release ships one project runtime DLL, `cvforwin.dll`. Every component
  above is statically linked into it (or, for Catch2, only into test
  executables), so no third-party DLL must be redistributed. Windows system
  DLLs (kernel32, Media Foundation `mfplat`/`mf`/`mfreadwrite`/`mfuuid`,
  `ole32`, and the like) are operating-system components and are not covered by
  these notices.
- vcpkg build-time helper ports (`vcpkg-cmake`, `vcpkg-cmake-config`,
  `vcpkg-get-python-packages`) are not redistributed and carry no notice here.
- The Daheng SDK and other vendor binaries are intentionally absent from the
  baseline. Adding one requires an approved R3 revision and a corresponding
  notice update.
- The release symbols archive (PDB) is a separate CI artifact and is not part of
  the license-bearing package.
- `LICENSES/README.md` is the mapping source; the `*.txt` files are verbatim
  copies of the upstream license texts collected by vcpkg.
- Project-authored example assets are not third-party components and carry no
  notice here. `config/examples/assets/tmpl.asymmetric.png` (the CVF-106
  `template.match` example template) is a deterministic 12x8 grayscale image
  generated from an in-project formula; it is dedicated to the project under
  the same terms as the surrounding source and embeds no third-party pixels or
  metadata.
