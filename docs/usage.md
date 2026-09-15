# Using cvforwin

`include/cvforwin/cvf_api.h` is the complete C ABI v1 contract. It is C11,
self-contained (`<stdint.h>` and `<stddef.h>` only), and callable from C and
C++20. All buffers are caller-owned; the only DLL-managed value is the opaque
`cvf_context`.

## Lifecycle

```
cvf_get_abi_version()                       -> CVF_ABI_VERSION_V1 (1)
cvf_initialize(options, &context, &error)   -> CVF_STATUS_OK on success
cvf_inspect(context, &request, &result)     -> once per normal cycle
cvf_reload_recipes(context, &error)         -> atomic, all-or-nothing
cvf_shutdown(context)                       -> releases the context
```

Rules that matter to a host:

- `struct_size` must equal `sizeof` the v1 structure and `abi_version` must be
  `CVF_ABI_VERSION_V1`; reserved fields must be zero.
- `config_root_utf8` and `output_root_utf8` are absolute directories.
  `output_root` receives `logs/` and `captures/`.
- One live context and one configured camera exist per process; calls are
  serialized inside the context. The host quiesces calls before shutdown.
- Every text buffer carries `capacity`, `bytes_written` (excluding the trailing
  NUL) and `bytes_required` (including the NUL). The v1 required capacities are
  `CVF_RESULT_JSON_REQUIRED_CAPACITY` (65536), `CVF_ERROR_MESSAGE_REQUIRED_CAPACITY`
  (1024) and `CVF_IMAGE_PATH_REQUIRED_CAPACITY` (4096).
- `status` and `verdict` are independent: a non-OK status is never reported as
  FAIL; it is always `CVF_VERDICT_NOT_EVALUATED`.
- `request.timeout_ms = 0` selects the documented 5000 ms default; a non-zero
  value overrides it for the whole cycle (queueing, capture, one
  reconnect/recapture, algorithm, encoding, persistence).

## Configuration schema (`<config_root>/cvforwin.json`)

`schema_version` is `1`. Unknown keys, duplicate keys, and invalid UTF-8 are
rejected at `cvf_initialize`.

| Key | Values |
| --- | --- |
| `schema_version` | `1` |
| `camera.backend` | `uvc` (release); `file`/`synthetic` are accepted only by test-enabled builds (`CVFORWIN_BUILD_TEST_BACKENDS=ON`) |
| `camera.device_path` | stable device path, or omit and use the VID/PID pair |
| `camera.vendor_id`, `camera.product_id` | four hexadecimal characters each |
| `camera.friendly_name` | optional identity hint |
| `base_capture.width`, `base_capture.height` | 1..16384 |
| `base_capture.frame_rate` | > 0.0 .. 1000.0 |
| `base_capture.pixel_format` | `any`, `mono8`, `bgr8`, `rgb8` |
| `logging.level` | `trace`, `debug`, `info`, `warn`, `error`, `critical` |
| `logging.max_file_bytes` | 1024 .. 1073741824 |
| `logging.max_files` | 1 .. 1000 |
| `retention.max_age_days` | 1 .. 3650 |
| `retention.max_total_bytes` | 1048576 .. 1099511627776 |

A valid deployment template is `config/examples/cvforwin.json`. The DLL never
selects the first enumerated camera: the selector must resolve exactly one
device or initialization fails.

## Recipe schema (`<config_root>/recipes/*.json`)

| Key | Values |
| --- | --- |
| `schema_version` | `1` |
| `recipe_id` | `[A-Za-z0-9._-]{1,128}`, unique in the recipe set |
| `algorithm` | compiled registry key, `[a-z0-9._-]{1,64}` (the example key is `example.threshold`) |
| `parameters` | algorithm-specific object, validated before activation |
| `capture.width`, `.height`, `.frame_rate`, `.pixel_format` | optional bounded capture overrides |
| `capture.settle_frames` | required, 0..1000 |
| `artifacts.save_policy` | `always`, `fail_or_error`, `never` (default behaviour is save on FAIL or technical error) |
| `artifacts.required` | boolean; `true` turns an image-save failure into `CVF_STATUS_REQUIRED_ARTIFACT_ERROR` |

The example algorithm `example.threshold` accepts `threshold` (integer 0..255)
and `min_pass_ratio` (0.0..1.0); it reports `white_pixels`, `total_pixels`, and
`pass_ratio` in `output_json`, and returns PASS when
`pass_ratio >= min_pass_ratio`. See `config/examples/recipes/`.

`cvf_reload_recipes` validates the complete candidate set and swaps the snapshot
atomically; on any error the previous set stays active.

## Example host

`examples/c_host/main.c` is a strict C11 template that uses only
`<cvforwin/cvf_api.h>`:

```
cvf_c_host_example <absolute config root> <absolute output root> [cycles]
```

It queries the ABI version, initializes with file and callback logging, runs one
inspect per cycle (printing status, verdict, elapsed time, an output-JSON
excerpt, and the image path), reloads recipes, and shuts down. Any failure
returns a non-zero exit code. A local run against the test-enabled build uses
the synthetic configuration in `examples/c_host/config`:

```
cmake --build --preset wsl-gcc-debug
examples/c_host/run_example.sh
```

A deployment host points the same executable at a `uvc` configuration such as
`config/examples/cvforwin.json` from the release package.
