# UVC hardware stress suite (CVF-107)

Status: opt-in, operator-run release evidence. **Not** a CI gate.

`tests/hardware/uvc_smoke.cpp` builds the Windows-only executable
`cvf_hw_uvc_smoke`. It is compiled only when **both** `WIN32` and
`CVFORWIN_BUILD_HARDWARE_TESTS=ON` hold. It is **never registered with CTest**
and is **never added to a hosted CI workflow**; hosted runners have no physical
UVC camera. The portable WSL suite is unaffected because the whole translation
unit is inside `#if defined(_WIN32)`.

## Build

Use the contract's canonical `windows-msvc-tests` preset. The preset already
selects the MSVC v143 x64 generator, the `build/windows-msvc-tests` directory,
the vcpkg toolchain/triplet, and the warning-as-error policy; only the opt-in
hardware option is added on the configure line.

```powershell
cmake --preset windows-msvc-tests -DCVFORWIN_BUILD_HARDWARE_TESTS=ON
cmake --build --preset windows-msvc-tests --config Release --target cvf_hw_uvc_smoke --parallel
```

The binary is `build\windows-msvc-tests\Release\cvf_hw_uvc_smoke.exe`.

## 1. Discover the camera identity

```powershell
build\windows-msvc-tests\Release\cvf_hw_uvc_smoke.exe --list
```

The listing prints every enumerated UVC descriptor
(`backend_key`, `device_path`, `vendor_id`, `product_id`, `friendly_name`) and
exits.

On the Windows Media Foundation backend this suite targets, `enumerate()` leaves
`vendor_id` and `product_id` empty and populates only `device_path` and
`friendly_name`, so VID/PID matching can never succeed there. Select the camera
by `device_path` or, second, by `friendly_name`.

## 2. Select exactly one device (required)

There is **no first-device fallback**. A missing selector is a configuration
failure and the process exits nonzero.

| Variable | Meaning |
| --- | --- |
| `CVF_HW_UVC_DEVICE_PATH` | Exact Media Foundation symbolic-link device path. **Works on this platform; preferred.** |
| `CVF_HW_UVC_NAME` | Exact friendly name. **Works on this platform.** |
| `CVF_HW_UVC_VID` | Four lowercase hex digits; use with `CVF_HW_UVC_PID`. Matches only on a platform/backend that populates enumerated `vendor_id`. |
| `CVF_HW_UVC_PID` | Four lowercase hex digits. Matches only on a platform/backend that populates enumerated `product_id`. |

At least one must be set. If several are set they are combined, and the selector
must still resolve exactly one device. On Windows, use `CVF_HW_UVC_DEVICE_PATH`
or `CVF_HW_UVC_NAME`; VID/PID remain accepted inputs but only match a backend
that reports those fields.

## 3. Capture settings

| Variable | Default | Meaning |
| --- | --- | --- |
| `CVF_HW_WIDTH` | `640` | Configured capture width. |
| `CVF_HW_HEIGHT` | `480` | Configured capture height. |
| `CVF_HW_FPS` | `0` (backend default) | Configured frame rate. |
| `CVF_HW_CYCLES` | `50` | Consecutive captures; min `1`, max `100000`. |

Each capture in the loop is validated for configured dimensions, `bgr8` pixel
format / `CV_8UC3` type, a continuous buffer, and a strictly increasing
`sequence`. At `CVF_HW_CYCLES=1` (the documented minimum) the sequence check is
not applicable and is not asserted.

## 4. Stress mode (>= 1000 consecutive captures)

| Variable | Meaning |
| --- | --- |
| `CVF_HW_STRESS=1` | Enable stress mode. Also implied when `CVF_HW_CYCLES >= 1000`. |
| `CVF_HW_REQUIRE_ORIENTATION=1` | Make the orientation check mandatory outside stress mode. |
| `CVF_HW_REQUIRE_COLOR=1` | Make the color check mandatory outside stress mode. |
| `CVF_HW_REQUIRE_TIMEOUT=1` | Make forced-timeout evidence mandatory outside stress mode. |

In stress mode:

* `CVF_HW_CYCLES` **must** be `>= 1000`, otherwise the run fails before the loop;
* a missing orientation target is a **failure**, never a skip;
* a missing color target is a **failure**, never a skip;
* a skipped forced-timeout path is a **failure**;
* `CVF_HW_EXPECT_ZERO_DEVICES=1` is rejected.

The requirement flags are forced on in stress mode and cannot be disabled by an
environment value there. The requirement and control flags actually in effect are
recorded in the release-evidence block (section 10) so an archived log proves
which flags were set, and each mandatory-check failure message names them.

## 5. Orientation procedure (target CVF-ORIENT-1)

Target: a portrait card whose **top third is bright white** and whose **bottom
third is matte black** (vertically asymmetric). Fill the central half of the
frame, upright in the camera's visual field, under even lighting.

| Variable | Default | Meaning |
| --- | --- | --- |
| `CVF_HW_ORIENTATION_TEST=1` | unset | Run the orientation check. |
| `CVF_HW_ORIENTATION_MIN_CONTRAST` | `40` (of 255) | Minimum absolute top/bottom mean-luma contrast. |
| `CVF_HW_ORIENTATION_SYMMETRIC_CONTROL=1` | unset | Negative control: a vertically symmetric card must be rejected. |

The check samples the mean luma (BT.601: `0.114 B + 0.587 G + 0.299 R`) of a
central top band and a central bottom band. It passes only when:

1. the absolute contrast is at least `CVF_HW_ORIENTATION_MIN_CONTRAST`, **and**
2. the bright band is at the visual top (so a vertical flip fails).

A vertically symmetric image has near-zero contrast and therefore **fails** the
first condition; it can never be accepted as orientation evidence. The symmetric
control (`CVF_HW_ORIENTATION_SYMMETRIC_CONTROL=1`) asserts exactly that, and is
the documented negative check for this procedure. It is **not** a substitute for
the positive asymmetric target: when the orientation check is required
(`CVF_HW_STRESS=1` or `CVF_HW_REQUIRE_ORIENTATION=1`) and
`CVF_HW_ORIENTATION_TEST` is unset, the run **fails** even when the symmetric
control is set.

The positive asymmetric target and the symmetric control are **separate,
non-required checks**: each is evaluated on its own separately captured frame, so
they are never required to hold on one frame, and a failure in one does not
prevent the other from being evaluated. Because the asymmetric card and the
vertically symmetric card are physically different targets, run the symmetric
control in its own run with a vertically symmetric card in view.

## 6. Color procedure (target CVF-COLOR-1)

Target: a matte saturated swatch (red, green, or blue) filling the central half
of the frame under the station's normal lighting. A uniform gray card is the
documented `neutral` target.

| Variable | Default | Meaning |
| --- | --- | --- |
| `CVF_HW_COLOR_TEST=1` | unset | Run the color check. |
| `CVF_HW_COLOR_EXPECT` | `red` | `red`, `green`, `blue`, or `neutral`. |
| `CVF_HW_COLOR_MIN_DOMINANCE` | `30` (of 255) | Expected channel must lead the other two by at least this. |
| `CVF_HW_COLOR_MIN_LEVEL` | `60` (of 255) | Expected channel mean must be at least this. |
| `CVF_HW_COLOR_MAX_SPREAD` | `30` (of 255) | Neutral control: max-min channel spread allowed. |
| `CVF_HW_COLOR_REF_B` / `_G` / `_R` | unset | Reference mean of each channel (0..255). All three or none. |
| `CVF_HW_COLOR_TOLERANCE` | `40` (of 255) | Per-channel tolerance when all three references are set. |

The check computes the mean BGR of the central half of the frame. For `red`,
`green`, or `blue` it passes when the expected channel leads the others by
`CVF_HW_COLOR_MIN_DOMINANCE` and reaches `CVF_HW_COLOR_MIN_LEVEL`; when all three
`REF_*` variables are set it additionally requires every channel to be within
`CVF_HW_COLOR_TOLERANCE` of its reference — pass inside tolerance, fail outside.
For `neutral` it passes only when the channel spread is at most
`CVF_HW_COLOR_MAX_SPREAD`. Measured values and thresholds are printed.

The `neutral` target is the spread **control**, not the positive target. It is
never a substitute for the saturated CVF-COLOR-1 swatch: when the colour check is
required (`CVF_HW_STRESS=1` or `CVF_HW_REQUIRE_COLOR=1`) the run fails closed
unless a saturated target (`red`, `green`, or `blue`) is actually tested, and it
also fails when `CVF_HW_COLOR_TEST` is unset regardless of
`CVF_HW_COLOR_EXPECT`.

## 7. Forced timeout procedure

Always exercised when the device is open and never silently skipped:

* a capture with `Deadline::immediate()` must fail `timeout`/`capture_timed_out`;
* a capture with `timeout_ms == 0` must fail `timeout`/`capture_timed_out`;
* `capture_with_one_retry` with an expired deadline must return that timeout
  unchanged (no retry, no `camera_io` conversion);
* the device must still capture successfully afterwards.

In stress mode (or with `CVF_HW_REQUIRE_TIMEOUT=1`) all of the above must pass.

## 8. Reconnect and the manual unplug/replug step

The bounded-reconnect / unplug-replug case (H6) runs **only** when
`CVF_HW_UNPLUG_TEST=1`; with the variable unset the whole case is skipped
(reported as a skip, not executed). When it runs, the suite exercises a bounded
reconnect to the attached device and pins the deadline. The manual procedure is
environment-gated and never automatic:

```powershell
$env:CVF_HW_UNPLUG_TEST = "1"
```

Run in an interactive console. The suite opens and reconnects once, then:

1. prompts you to physically unplug the camera from the USB port and press Enter;
2. expects the next capture to fail `camera_io`, and the bounded
   `capture_with_one_retry` to fail within its deadline;
3. prompts you to replug into the **same** USB port and press Enter;
4. expects one bounded reconnect + recapture to recover a valid BGR8 frame.

The at-most-one-retry rule is pinned deterministically by the hardware-free
CVF-002 suite; this hardware case observes the bounded recovery end to end. With
`CVF_HW_UNPLUG_TEST` unset the case is reported as skipped.

## 9. Zero-camera boundary

```powershell
$env:CVF_HW_EXPECT_ZERO_DEVICES = "1"
```

Asserts that enumeration is empty and that resolution fails with
`camera_not_found`. Not allowed in stress mode.

## 10. Release-evidence record

The suite prints, and optionally writes, a delimited block:

```
=== CVF-107 RELEASE EVIDENCE BEGIN ===
CVF-107 UVC hardware stress evidence
backend_key=uvc
device_path=...
vendor_id=...
product_id=...
friendly_name=...
selector_kind=CVF_HW_UVC_DEVICE_PATH
configured_width=640
configured_height=480
configured_fps=0
configured_cycles=1000
stress_mode=yes
require_orientation=yes
require_color=yes
require_timeout=yes
orientation_test=yes
orientation_min_contrast=40
orientation_symmetric_control=no
color_test=yes
color_expect=red
color_min_dominance=30
color_min_level=60
color_max_spread=30
color_reference_configured=no
color_tolerance=40
unplug_test=no
expect_zero_devices=no
windows_version=Windows 10.0 build 19045
camera_driver=...
camera_firmware=...
process_exit_code=0
checks_passed=...
checks_failed=...
checks_skipped=...
=== CVF-107 RELEASE EVIDENCE END ===
```

| Variable | Meaning |
| --- | --- |
| `CVF_HW_EVIDENCE_FILE` | If set, also write the evidence block to this path. |
| `CVF_HW_WINDOWS_VERSION` | Override/record the exact Windows version string. |
| `CVF_HW_CAMERA_DRIVER` | Record the camera driver description (best effort). |
| `CVF_HW_CAMERA_FIRMWARE` | Record the camera firmware version (best effort). |

Windows version is detected from `RtlGetVersion` (accurate, unlike the
manifested `GetVersionEx`) and falls back to the `OS` environment variable plus
the operator override. Driver/firmware are best-effort operator entries.

The requirement and control flags actually in effect are recorded in the block
(stress mode; `require_orientation`/`require_color`/`require_timeout`; the
orientation test/control flags and contrast threshold; the colour test/expect
flag and dominance/level/spread/tolerance thresholds plus whether references are
configured; the unplug and zero-device probe flags). The mandatory orientation
and colour failure messages also name the specific `CVF_HW_*` flags that were
set, so each archived log is self-evidencing.

### Reading the console output

* `[info]` lines are context (identity, measured values, thresholds, progress).
* `[pass]` / `[fail]` / `[skip]` are individual checks.
* The final `CVF-107 UVC hardware smoke summary` line gives check counts.
* `RESULT: PASS` / `RESULT: FAIL` and `process_exit_code` are the gate.
* Archive the whole block (and `CVF_HW_EVIDENCE_FILE`) as the release record.

Exit code: `0` = every executed check passed (non-required skips allowed);
`1` = at least one failure, including a missing selector or a mandatory check
that could not be executed.

## 11. Operator commands (release run)

```powershell
$exe = "build\windows-msvc-tests\Release\cvf_hw_uvc_smoke.exe"

# 1. identify
& $exe --list

# 2. stress run with the CVF-ORIENT-1 and CVF-COLOR-1 targets in view
$env:CVF_HW_UVC_DEVICE_PATH = "<symbolic link from --list>"
$env:CVF_HW_WIDTH  = "640"
$env:CVF_HW_HEIGHT = "480"
$env:CVF_HW_CYCLES = "1000"
$env:CVF_HW_ORIENTATION_TEST = "1"
$env:CVF_HW_COLOR_TEST = "1"
$env:CVF_HW_COLOR_EXPECT = "red"
$env:CVF_HW_EVIDENCE_FILE = "cvf107-evidence-$(Get-Date -Format yyyyMMdd-HHmmss).txt"
& $exe
"exit code: $LASTEXITCODE"

# 3. reconnect run (interactive; keep the targets in view)
$env:CVF_HW_UNPLUG_TEST = "1"
& $exe
```

## 12. What a Linux host can and cannot prove

A Linux/WSL host can prove that this translation unit is syntactically sound
under a forced `-D_WIN32` probe and that the portable suite stays green, but it
**cannot** compile or execute the Windows binary and **cannot** produce hardware
evidence. Windows build/run evidence must come from a designated Windows 10/11
station. Until then the hardware result is `UNVERIFIED`, not `PASS`.
