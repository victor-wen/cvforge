/*
 * CVF-007 opt-in UVC (Media Foundation) hardware smoke suite -- owner: test-engineer.
 *
 * This file is authored BEFORE the Windows UVC backend exists. WSL has no Windows
 * and no camera, so author-phase RED evidence is structural (this translation unit
 * is empty off Windows) and execution evidence is deferred to a designated
 * Windows 10/11 x64 station. No compile/run output is claimed from Linux.
 *
 * ---------------------------------------------------------------------------
 * OPERATOR NOTE (README)
 * ---------------------------------------------------------------------------
 * Prerequisites
 *   - Windows 10 or Windows 11 x64 with MSVC v143 (Visual Studio 2022) and
 *     CMake >= 3.28.
 *   - A UVC camera reachable through Media Foundation (driverless class driver).
 *   - A build configured with the opt-in CMake option CVFORWIN_BUILD_HARDWARE_TESTS
 *     (default OFF). The developer wires that option and the WIN32-only target;
 *     this suite is opt-in and is NEVER part of mandatory CI, because hosted CI
 *     has no physical camera.
 *
 * Discover the identity first
 *   Run:  uvc_smoke --list
 *   The listing prints every enumerated UVC descriptor (device_path, vendor_id,
 *   product_id, friendly_name) and exits. There is no first-device fallback: the
 *   suite refuses to run without an explicitly configured selector.
 *
 * Run
 *   Configure at least one selector variable (exact, case-sensitive):
 *     CVF_HW_UVC_DEVICE_PATH   preferred; the Media Foundation symbolic link
 *     CVF_HW_UVC_VID           four lowercase hex digits
 *     CVF_HW_UVC_PID           four lowercase hex digits
 *     CVF_HW_UVC_NAME          friendly name
 *   Optional settings:
 *     CVF_HW_WIDTH / CVF_HW_HEIGHT   configured capture size (default 640 / 480)
 *     CVF_HW_FPS                     configured frame rate, 0 = backend default
 *     CVF_HW_CYCLES                  repeated-capture count (default 50)
 *     CVF_HW_EXPECT_ZERO_DEVICES=1   camera-less station run: asserts enumerate is
 *                                    empty and resolution fails with camera_not_found
 *                                    (H1/H2 only; used for the zero-device boundary)
 *   Then run the executable from an interactive console. A missing selector is a
 *   deliberate configuration failure, never a silent fallback.
 *
 * Manual unplug/replug step (H6, never automatic)
 *   Run with CVF_HW_UNPLUG_TEST=1. The suite then:
 *     1. opens the camera and prompts you to unplug it, then press Enter;
 *     2. expects the next capture to fail with camera_io and the bounded
 *        capture_with_one_retry attempt to fail within its deadline;
 *     3. prompts you to replug into the SAME USB port and press Enter;
 *     4. expects one bounded reconnect + recapture to recover.
 *   With CVF_HW_UNPLUG_TEST unset the case is reported as skipped. The at-most-one
 *   retry rule is pinned deterministically by the hardware-free CVF-002 suite; this
 *   hardware case observes the bounded recovery behavior end to end.
 *
 * Exit code: 0 = every executed check passed (skips allowed), 1 = at least one
 * failure.
 *
 * Interface assumption (recorded in .ai/reports/CVF-007-test-red.yaml):
 * src/camera/uvc_windows/uvc_backend.h declares a default-constructible
 * cvforwin::camera::UvcCameraBackend implementing cvforwin::camera::ICameraBackend
 * and reporting backend_key() == "uvc". The static assertions below pin that
 * assumption at compile time; the RED report's assumptions section is the
 * coordination point if the planned spelling differs.
 * ---------------------------------------------------------------------------
 */

#if defined(_WIN32)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

#include "camera/camera_backend.h"
#include "camera/captured_frame.h"
#include "camera/uvc_windows/uvc_backend.h"
#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"

namespace cvf007 {

namespace cam = cvforwin::camera;
namespace core = cvforwin::core;

using HwBackend = cam::UvcCameraBackend;

static_assert(std::is_base_of_v<cam::ICameraBackend, HwBackend>,
              "CVF-007 assumption: UvcCameraBackend must implement cvforwin::camera::ICameraBackend");
static_assert(std::is_default_constructible_v<HwBackend>,
              "CVF-007 assumption: UvcCameraBackend must be default-constructible");

constexpr std::uint32_t kOpenTimeoutMs = 8000;
constexpr std::uint32_t kCaptureTimeoutMs = 3000;
constexpr std::uint32_t kReplugTimeoutMs = 8000;
constexpr std::uint32_t kDefaultWidth = 640;
constexpr std::uint32_t kDefaultHeight = 480;
constexpr std::uint32_t kDefaultCycles = 50;
constexpr std::int64_t kDeadlineSlackMs = 2000;

// ---------------------------------------------------------------------------
// Minimal self-checking harness (no test framework so a station can run the
// executable standalone).
// ---------------------------------------------------------------------------

class Report {
public:
    void info(const std::string& message)
    {
        std::cout << "[info] " << message << '\n';
    }

    void check(bool condition, const std::string& what)
    {
        if (condition) {
            ++passed_;
            std::cout << "[pass] " << what << '\n';
        } else {
            ++failed_;
            failures_.push_back(what);
            std::cout << "[fail] " << what << '\n';
        }
    }

    void skip(const std::string& what, const std::string& why)
    {
        ++skipped_;
        std::cout << "[skip] " << what << " -- " << why << '\n';
    }

    int finish() const
    {
        std::cout << "\nCVF-007 UVC hardware smoke summary: passed=" << passed_ << " failed=" << failed_
                  << " skipped=" << skipped_ << '\n';
        for (const std::string& failure : failures_) {
            std::cout << "FAILED: " << failure << '\n';
        }
        if (failed_ == 0) {
            std::cout << "RESULT: PASS (opt-in hardware evidence; not a CI gate)\n";
            return 0;
        }
        std::cout << "RESULT: FAIL\n";
        return 1;
    }

private:
    int passed_ = 0;
    int failed_ = 0;
    int skipped_ = 0;
    std::vector<std::string> failures_;
};

// ---------------------------------------------------------------------------
// Environment and small helpers.
// ---------------------------------------------------------------------------

std::string env_text(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

bool env_flag(const char* name)
{
    const std::string value = env_text(name);
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "on";
}

std::uint32_t env_u32(const char* name, std::uint32_t fallback)
{
    const std::string value = env_text(name);
    if (value.empty()) {
        return fallback;
    }
    try {
        const unsigned long parsed = std::stoul(value);
        if (parsed == 0 || parsed > 100000) {
            return fallback;
        }
        return static_cast<std::uint32_t>(parsed);
    } catch (...) {
        return fallback;
    }
}

double env_f64(const char* name, double fallback)
{
    const std::string value = env_text(name);
    if (value.empty()) {
        return fallback;
    }
    try {
        const double parsed = std::stod(value);
        return parsed > 0.0 ? parsed : fallback;
    } catch (...) {
        return fallback;
    }
}

core::Deadline open_deadline()
{
    return core::Deadline::from_timeout_ms(kOpenTimeoutMs);
}

core::Deadline capture_deadline()
{
    return core::Deadline::from_timeout_ms(kCaptureTimeoutMs);
}

std::string failure_text(const core::Failure& failure)
{
    return std::string(core::status_name(failure.status)) + "/" + std::string(core::error_code_name(failure.code)) +
           " message=\"" + failure.message + "\"";
}

bool is_lower_hex4(const std::string& value)
{
    if (value.size() != 4) {
        return false;
    }
    for (const char character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

bool descriptors_equal(const cam::CameraDescriptor& left, const cam::CameraDescriptor& right)
{
    return left.backend_key == right.backend_key && left.device_path == right.device_path &&
           left.vendor_id == right.vendor_id && left.product_id == right.product_id &&
           left.friendly_name == right.friendly_name;
}

void print_descriptor(const cam::CameraDescriptor& descriptor)
{
    std::cout << "  backend_key=\"" << descriptor.backend_key << "\" device_path=\"" << descriptor.device_path
              << "\" vendor_id=\"" << descriptor.vendor_id << "\" product_id=\"" << descriptor.product_id
              << "\" friendly_name=\"" << descriptor.friendly_name << "\"\n";
}

bool wait_for_enter()
{
    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cout << "[warn] stdin is not interactive; the manual procedure needs an operator console\n";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// H1: enumeration contract.
// ---------------------------------------------------------------------------

std::vector<cam::CameraDescriptor> check_enumeration(Report& report, HwBackend& backend)
{
    report.check(backend.backend_key() == "uvc", "H1 backend_key() reports \"uvc\"");

    auto enumerated = backend.enumerate(open_deadline());
    report.check(enumerated.has_value(), "H1 enumerate succeeds");
    if (!enumerated.has_value()) {
        std::cout << "  enumerate failure: " << failure_text(enumerated.failure()) << '\n';
        return {};
    }

    std::vector<cam::CameraDescriptor> devices = std::move(enumerated).value();
    report.info("H1 enumerated " + std::to_string(devices.size()) + " descriptor(s)");
    for (const cam::CameraDescriptor& device : devices) {
        print_descriptor(device);
    }

    bool keys_ok = true;
    bool paths_ok = true;
    bool code_formats_ok = true;
    bool vendor_seen = false;
    bool product_seen = false;
    bool friendly_seen = false;
    for (const cam::CameraDescriptor& device : devices) {
        keys_ok = keys_ok && device.backend_key == "uvc";
        paths_ok = paths_ok && !device.device_path.empty();
        code_formats_ok = code_formats_ok && (device.vendor_id.empty() || is_lower_hex4(device.vendor_id)) &&
                          (device.product_id.empty() || is_lower_hex4(device.product_id));
        vendor_seen = vendor_seen || !device.vendor_id.empty();
        product_seen = product_seen || !device.product_id.empty();
        friendly_seen = friendly_seen || !device.friendly_name.empty();
    }

    if (!devices.empty()) {
        report.check(keys_ok, "H1 every enumerated descriptor has backend_key \"uvc\"");
        report.check(paths_ok, "H1 every enumerated descriptor has a non-empty device_path");
        report.check(code_formats_ok, "H1 vendor_id/product_id are empty or 4 lowercase hex (descriptor contract)");
        report.info(std::string("H1 exposed identity fields: vendor_id=") + (vendor_seen ? "yes" : "no") +
                    " product_id=" + (product_seen ? "yes" : "no") +
                    " friendly_name=" + (friendly_seen ? "yes" : "no"));
    }

    return devices;
}

// ---------------------------------------------------------------------------
// H2: unique resolution, no first-device fallback.
// ---------------------------------------------------------------------------

struct AmbiguousCase {
    std::vector<cam::CameraDescriptor> candidates;
    cam::CameraSelector selector;
};

/*
 * Multiple-match probe built from a synthetic twin pair. It deliberately does not
 * read any real enumerated descriptor, so no test path ever selects the first
 * enumerated device; the resolver is backend-neutral and the real candidate set
 * is used unchanged by the zero-match and empty-selector cases above.
 */
AmbiguousCase make_ambiguous_case()
{
    cam::CameraDescriptor base;
    base.backend_key = "uvc";
    base.device_path = "cvf-hw-synthetic-a";
    base.vendor_id = "cafe";
    base.product_id = "beef";
    base.friendly_name = "CVF HW Synthetic";

    cam::CameraDescriptor twin = base;
    twin.device_path += "#cvf-hw-twin";

    cam::CameraSelector selector;
    selector.vendor_id = base.vendor_id;

    return AmbiguousCase{{base, twin}, selector};
}

void run_resolution_cases(Report& report, const std::vector<cam::CameraDescriptor>& devices,
                          const cam::CameraDescriptor& resolved)
{
    cam::CameraSelector empty_selector;
    auto empty_result = cam::resolve_identity(devices, empty_selector);
    report.check(!empty_result.has_value() && empty_result.failure().status == core::Status::invalid_argument &&
                     empty_result.failure().code == core::ErrorCode::selector_empty,
                 "H2 negative: empty selector is rejected (selector_empty), never a first-device fallback");

    cam::CameraSelector missing_selector;
    missing_selector.device_path = resolved.device_path + "-cvf-hw-no-such-device";
    auto missing_result = cam::resolve_identity(devices, missing_selector);
    report.check(!missing_result.has_value() && missing_result.failure().status == core::Status::camera_not_found &&
                     missing_result.failure().code == core::ErrorCode::camera_not_found,
                 "H2 negative: selector matching no device -> camera_not_found/camera_not_found");
    if (!missing_result.has_value()) {
        std::cout << "  observed: " << failure_text(missing_result.failure()) << '\n';
    }

    const AmbiguousCase ambiguous = make_ambiguous_case();
    auto ambiguous_result = cam::resolve_identity(ambiguous.candidates, ambiguous.selector);
    report.check(!ambiguous_result.has_value() &&
                     ambiguous_result.failure().status == core::Status::camera_not_found &&
                     ambiguous_result.failure().code == core::ErrorCode::camera_identity_ambiguous,
                 "H2 negative: selector matching multiple devices -> camera_not_found/camera_identity_ambiguous");
    if (!ambiguous_result.has_value()) {
        std::cout << "  observed: " << failure_text(ambiguous_result.failure()) << '\n';
    }

    if (!resolved.friendly_name.empty()) {
        cam::CameraSelector name_selector;
        name_selector.friendly_name = resolved.friendly_name;
        auto named_result = cam::resolve_identity(devices, name_selector);
        if (named_result.has_value()) {
            report.check(descriptors_equal(named_result.value(), resolved),
                         "H2 boundary: friendly_name-only selector resolves the same descriptor");
        } else {
            const core::Failure& failure = named_result.failure();
            report.check(failure.code == core::ErrorCode::camera_identity_ambiguous ||
                             failure.code == core::ErrorCode::camera_not_found,
                         "H2 boundary: friendly_name-only selector fails per rules (ambiguous or not found)");
        }

        cam::CameraSelector suffixed_selector;
        suffixed_selector.friendly_name = resolved.friendly_name + " cvf-hw-no-such-suffix";
        auto suffixed_result = cam::resolve_identity(devices, suffixed_selector);
        report.check(!suffixed_result.has_value() &&
                         suffixed_result.failure().code == core::ErrorCode::camera_not_found,
                     "H2 boundary: friendly_name matching is exact (suffixed name is camera_not_found)");
    } else {
        report.skip("H2 boundary: friendly_name-only selector on hardware",
                    "the enumerated device exposes no friendly_name");
    }
}

// ---------------------------------------------------------------------------
// H3: open/configure/capture/close ordering and frame ownership.
// ---------------------------------------------------------------------------

struct SessionResult {
    bool opened = false;
    bool captured = false;
    cv::Mat held_pixels;
};

SessionResult run_open_capture_close_session(Report& report, const cam::CameraDescriptor& descriptor,
                                             const cam::CameraSettings& settings)
{
    SessionResult result;
    HwBackend backend;

    auto opened = backend.open(descriptor, settings, open_deadline());
    report.check(opened.has_value(), "H3 open with the resolved descriptor and configured settings succeeds");
    if (!opened.has_value()) {
        std::cout << "  open failure: " << failure_text(opened.failure()) << '\n';
        return result;
    }
    result.opened = true;

    bool have_previous = false;
    std::uint64_t previous_sequence = 0;
    for (int index = 0; index < 3; ++index) {
        auto captured = backend.capture(capture_deadline());
        if (!captured.has_value()) {
            report.check(false, "H3 capture #" + std::to_string(index) + " succeeds");
            std::cout << "  capture failure: " << failure_text(captured.failure()) << '\n';
            continue;
        }
        const cam::CapturedFrame& frame = captured.value();
        const bool dimensions_ok = frame.metadata.width == settings.width && frame.metadata.height == settings.height;
        const bool format_ok = frame.metadata.pixel_format == cam::PixelFormat::bgr8;
        const bool pixels_ok = frame.pixels.type() == CV_8UC3 && !frame.pixels.empty() &&
                               frame.pixels.isContinuous() &&
                               frame.pixels.cols == static_cast<int>(settings.width) &&
                               frame.pixels.rows == static_cast<int>(settings.height);
        const bool sequence_ok = !have_previous || frame.metadata.sequence > previous_sequence;
        report.check(dimensions_ok && format_ok && pixels_ok && sequence_ok,
                     "H3 capture #" + std::to_string(index) +
                         " returns an owned BGR8 frame with configured dimensions and an incrementing sequence");
        if (!dimensions_ok || !format_ok || !pixels_ok || !sequence_ok) {
            std::cout << "  capture #" << index << " observed: sequence=" << frame.metadata.sequence
                      << " " << frame.metadata.width << "x" << frame.metadata.height
                      << " format=" << static_cast<std::uint32_t>(frame.metadata.pixel_format)
                      << " type=" << frame.pixels.type() << '\n';
        }
        if (index == 0) {
            result.held_pixels = frame.pixels;
        }
        previous_sequence = frame.metadata.sequence;
        have_previous = true;
        result.captured = true;
    }

    if (result.captured) {
        report.check(!result.held_pixels.empty() && result.held_pixels.data != nullptr,
                     "H3 the captured frame owns its pixel buffer");
    }

    backend.close();
    backend.close();
    auto after_close = backend.capture(capture_deadline());
    report.check(!after_close.has_value() && after_close.failure().status == core::Status::camera_io,
                 "H3 capture after two idempotent close() calls fails with camera_io (no crash)");
    if (!after_close.has_value()) {
        std::cout << "  capture-after-close observed: " << failure_text(after_close.failure()) << '\n';
    }

    auto reopened = backend.open(descriptor, settings, open_deadline());
    report.check(reopened.has_value(), "H3 the backend can reopen the same descriptor after close");
    if (reopened.has_value()) {
        auto recaptured = backend.capture(capture_deadline());
        report.check(recaptured.has_value(), "H3 a capture after reopen succeeds (device remains usable)");
    }
    backend.close();

    return result;
}

// ---------------------------------------------------------------------------
// H4: repeated capture cycles without resource exhaustion.
// ---------------------------------------------------------------------------

void run_repeated_cycles_case(Report& report, const cam::CameraDescriptor& descriptor,
                              const cam::CameraSettings& settings)
{
    const std::uint32_t cycles = env_u32("CVF_HW_CYCLES", kDefaultCycles);
    HwBackend backend;

    auto opened = backend.open(descriptor, settings, open_deadline());
    report.check(opened.has_value(), "H4 open before the repeated-capture case succeeds");
    if (!opened.has_value()) {
        std::cout << "  open failure: " << failure_text(opened.failure()) << '\n';
        return;
    }

    int valid = 0;
    int invalid = 0;
    bool have_previous = false;
    std::uint64_t previous_sequence = 0;
    for (std::uint32_t index = 0; index < cycles; ++index) {
        auto captured = backend.capture(capture_deadline());
        bool ok = captured.has_value();
        if (ok) {
            const cam::CapturedFrame& frame = captured.value();
            ok = frame.metadata.width == settings.width && frame.metadata.height == settings.height &&
                 frame.metadata.pixel_format == cam::PixelFormat::bgr8 && frame.pixels.type() == CV_8UC3 &&
                 !frame.pixels.empty() && frame.pixels.isContinuous();
            if (ok && have_previous) {
                ok = frame.metadata.sequence > previous_sequence;
            }
            if (ok) {
                previous_sequence = frame.metadata.sequence;
                have_previous = true;
            }
        }
        if (ok) {
            ++valid;
        } else {
            ++invalid;
        }
    }
    report.info("H4 repeated captures: valid=" + std::to_string(valid) + " invalid=" + std::to_string(invalid) +
                " of " + std::to_string(cycles));
    report.check(invalid == 0 && valid == static_cast<int>(cycles),
                 "H4 " + std::to_string(cycles) + " repeated captures all return valid frames");

    auto final_capture = backend.capture(capture_deadline());
    report.check(final_capture.has_value(), "H4 the device remains usable after the repeated-capture loop");

    auto still_enumerated = backend.enumerate(open_deadline());
    report.check(still_enumerated.has_value() && !still_enumerated.value().empty(),
                 "H4 the device is still enumerated after the repeated-capture loop");

    backend.close();
}

// ---------------------------------------------------------------------------
// H5: already-expired deadline.
// ---------------------------------------------------------------------------

void run_expired_deadline_case(Report& report, const cam::CameraDescriptor& descriptor,
                               const cam::CameraSettings& settings)
{
    HwBackend backend;
    auto opened = backend.open(descriptor, settings, open_deadline());
    report.check(opened.has_value(), "H5 open before the expired-deadline probe succeeds");
    if (!opened.has_value()) {
        std::cout << "  open failure: " << failure_text(opened.failure()) << '\n';
        return;
    }

    auto timed_out = backend.capture(core::Deadline::immediate());
    report.check(!timed_out.has_value() && timed_out.failure().status == core::Status::timeout &&
                     timed_out.failure().code == core::ErrorCode::capture_timed_out,
                 "H5 capture with an already-expired deadline -> timeout/capture_timed_out");
    if (!timed_out.has_value()) {
        std::cout << "  observed: " << failure_text(timed_out.failure()) << '\n';
    }

    auto recovered = backend.capture(capture_deadline());
    report.check(recovered.has_value(), "H5 the device stays usable after an expired-deadline capture");
    backend.close();
}

// ---------------------------------------------------------------------------
// Negative cases: stale descriptor, capture before open.
// ---------------------------------------------------------------------------

void run_negative_cases(Report& report, const cam::CameraSettings& settings)
{
    HwBackend backend;

    auto before_open = backend.capture(capture_deadline());
    report.check(!before_open.has_value() && before_open.failure().status == core::Status::camera_io,
                 "negative: capture before open fails with camera_io (no crash)");
    if (!before_open.has_value()) {
        std::cout << "  capture-before-open observed: " << failure_text(before_open.failure()) << '\n';
    }

    cam::CameraDescriptor stale;
    stale.backend_key = "uvc";
    stale.device_path = "cvf-hw-stale-descriptor";
    stale.vendor_id = "dead";
    stale.product_id = "beef";
    stale.friendly_name = "CVF HW Stale";
    auto opened = backend.open(stale, settings, open_deadline());
    report.check(!opened.has_value() && opened.failure().status == core::Status::camera_not_found,
                 "negative: open before resolve / with a stale descriptor fails with camera_not_found (no crash)");
    if (!opened.has_value()) {
        std::cout << "  stale-descriptor open observed: " << failure_text(opened.failure()) << '\n';
    }

    backend.close();
    report.info("negative: close on a never-opened backend returned without crashing");
}

// ---------------------------------------------------------------------------
// H6: manual, env-gated unplug/replug procedure (never automatic).
// ---------------------------------------------------------------------------

void run_manual_replug_case(Report& report, const cam::CameraDescriptor& descriptor,
                            const cam::CameraSettings& settings)
{
    if (!env_flag("CVF_HW_UNPLUG_TEST")) {
        report.skip("H6 manual unplug/replug",
                    "set CVF_HW_UNPLUG_TEST=1 on an interactive station to run the operator procedure");
        return;
    }

    HwBackend backend;
    auto opened = backend.open(descriptor, settings, open_deadline());
    report.check(opened.has_value(), "H6 open before the manual replug procedure succeeds");
    if (!opened.has_value()) {
        std::cout << "  open failure: " << failure_text(opened.failure()) << '\n';
        return;
    }

    std::cout << "\n=== MANUAL STEP 1 of 2 ===\n"
              << "Physically UNPLUG the UVC camera now (replug it into the SAME USB port later).\n"
              << "Then press Enter to continue..." << std::flush;
    if (!wait_for_enter()) {
        report.check(false, "H6 an interactive operator console is available for the manual procedure");
        backend.close();
        return;
    }

    auto after_unplug = backend.capture(core::Deadline::from_timeout_ms(kReplugTimeoutMs));
    report.check(!after_unplug.has_value() && after_unplug.failure().status == core::Status::camera_io,
                 "H6 first capture after unplug fails with camera_io");
    if (!after_unplug.has_value()) {
        std::cout << "  observed: " << failure_text(after_unplug.failure()) << '\n';
    }

    const auto retry_started = std::chrono::steady_clock::now();
    auto still_absent =
        cam::capture_with_one_retry(backend, settings, core::Deadline::from_timeout_ms(kReplugTimeoutMs));
    const std::int64_t elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - retry_started)
            .count();
    report.check(!still_absent.has_value() && still_absent.failure().status == core::Status::camera_io,
                 "H6 bounded capture_with_one_retry fails with camera_io while the device is absent");
    report.check(elapsed_ms >= 0 && elapsed_ms <= static_cast<std::int64_t>(kReplugTimeoutMs) + kDeadlineSlackMs,
                 "H6 the failed bounded retry returns within the deadline (elapsed=" + std::to_string(elapsed_ms) +
                     " ms)");

    std::cout << "\n=== MANUAL STEP 2 of 2 ===\n"
              << "Replug the camera into the SAME USB port and wait for Windows to re-enumerate it\n"
              << "(a few seconds), then press Enter to continue..." << std::flush;
    if (!wait_for_enter()) {
        report.check(false, "H6 an interactive operator console is available for the manual procedure");
        backend.close();
        return;
    }

    auto recovered = cam::capture_with_one_retry(backend, settings, core::Deadline::from_timeout_ms(kReplugTimeoutMs));
    report.check(recovered.has_value(), "H6 bounded reconnect + recapture recovers after replug");
    if (!recovered.has_value()) {
        std::cout << "  recovery failure: " << failure_text(recovered.failure()) << '\n';
    } else {
        const cam::CapturedFrame& frame = recovered.value();
        report.check(frame.metadata.pixel_format == cam::PixelFormat::bgr8 && frame.pixels.type() == CV_8UC3 &&
                         !frame.pixels.empty(),
                     "H6 the recovered frame is a valid BGR8 frame");
        std::cout << "  recovered: sequence=" << frame.metadata.sequence << " " << frame.metadata.width << "x"
                  << frame.metadata.height << '\n';
    }

    backend.close();
}

// ---------------------------------------------------------------------------
// Suite driver.
// ---------------------------------------------------------------------------

void run_suite(Report& report, const std::vector<cam::CameraDescriptor>& devices)
{
    if (env_flag("CVF_HW_EXPECT_ZERO_DEVICES")) {
        report.check(devices.empty(), "boundary: zero cameras attached -> enumerate is empty");
        cam::CameraSelector probe;
        probe.device_path = "cvf-hw-zero-device-probe";
        auto none = cam::resolve_identity(devices, probe);
        report.check(!none.has_value() && none.failure().status == core::Status::camera_not_found &&
                         none.failure().code == core::ErrorCode::camera_not_found,
                     "boundary: zero devices -> resolution fails with camera_not_found");
        report.skip("H3-H6 hardware cases", "CVF_HW_EXPECT_ZERO_DEVICES=1 (station has no camera attached)");
        return;
    }

    if (devices.empty()) {
        report.check(false,
                     "H1 at least one UVC device is enumerated (attach a camera, or set "
                     "CVF_HW_EXPECT_ZERO_DEVICES=1 for the camera-less boundary run)");
        report.skip("H2-H6", "no UVC device enumerated");
        return;
    }

    cam::CameraSelector selector;
    selector.device_path = env_text("CVF_HW_UVC_DEVICE_PATH");
    selector.vendor_id = env_text("CVF_HW_UVC_VID");
    selector.product_id = env_text("CVF_HW_UVC_PID");
    selector.friendly_name = env_text("CVF_HW_UVC_NAME");
    const bool selector_configured = !(selector.device_path.empty() && selector.vendor_id.empty() &&
                                       selector.product_id.empty() && selector.friendly_name.empty());
    report.check(selector_configured,
                 "H2 operator selector configured via CVF_HW_UVC_DEVICE_PATH/VID/PID/NAME (no first-device fallback)");
    if (!selector_configured) {
        report.info("run --list to print device identities, then set the selector environment variables");
        report.skip("H2-H6", "no explicit selector configured");
        return;
    }

    auto resolved_result = cam::resolve_identity(devices, selector);
    report.check(resolved_result.has_value(), "H2 the configured selector resolves exactly one device");
    if (!resolved_result.has_value()) {
        std::cout << "  observed: " << failure_text(resolved_result.failure()) << '\n';
        report.skip("H3-H6", "configured selector did not resolve a unique device");
        return;
    }
    const cam::CameraDescriptor resolved = std::move(resolved_result).value();
    report.info("H2 resolved device:");
    print_descriptor(resolved);

    run_resolution_cases(report, devices, resolved);

    cam::CameraSettings settings;
    settings.width = env_u32("CVF_HW_WIDTH", kDefaultWidth);
    settings.height = env_u32("CVF_HW_HEIGHT", kDefaultHeight);
    settings.frame_rate = env_f64("CVF_HW_FPS", 0.0);
    settings.preferred_format = cam::PixelFormat::bgr8;
    report.info("H3 configured settings: " + std::to_string(settings.width) + "x" +
                std::to_string(settings.height) + " fps=" + std::to_string(settings.frame_rate) + " format=bgr8");

    const SessionResult session = run_open_capture_close_session(report, resolved, settings);
    if (session.captured) {
        report.check(!session.held_pixels.empty() && session.held_pixels.data != nullptr &&
                         session.held_pixels.type() == CV_8UC3 &&
                         session.held_pixels.cols == static_cast<int>(settings.width) &&
                         session.held_pixels.rows == static_cast<int>(settings.height),
                     "H3 the captured pixels stay valid after close and backend destruction");
    }

    run_expired_deadline_case(report, resolved, settings);
    run_repeated_cycles_case(report, resolved, settings);
    run_negative_cases(report, settings);
    run_manual_replug_case(report, resolved, settings);
}

void print_usage()
{
    std::cout << "CVF-007 UVC hardware smoke suite\n"
              << "usage: uvc_smoke [--list] [--help]\n"
              << "  --list   enumerate UVC devices, print their identities, and exit\n"
              << "environment: CVF_HW_UVC_DEVICE_PATH / CVF_HW_UVC_VID / CVF_HW_UVC_PID / CVF_HW_UVC_NAME\n"
              << "             CVF_HW_WIDTH CVF_HW_HEIGHT CVF_HW_FPS CVF_HW_CYCLES\n"
              << "             CVF_HW_EXPECT_ZERO_DEVICES=1  CVF_HW_UNPLUG_TEST=1\n";
}

}  // namespace cvf007

int main(int argc, char** argv)
{
    cvf007::Report report;
    try {
        bool list_mode = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--list") {
                list_mode = true;
            } else if (argument == "--help" || argument == "-h") {
                cvf007::print_usage();
                return 0;
            }
        }

        cvf007::HwBackend backend;
        const std::vector<cvforwin::camera::CameraDescriptor> devices = cvf007::check_enumeration(report, backend);
        if (list_mode) {
            if (devices.empty()) {
                report.info("no UVC devices enumerated");
            }
            report.info("set CVF_HW_UVC_DEVICE_PATH or CVF_HW_UVC_VID/PID/NAME from the identities above");
            return report.finish();
        }

        cvf007::run_suite(report, devices);
    } catch (const std::exception& error) {
        report.check(false, std::string("unexpected exception escaped the smoke suite: ") + error.what());
    } catch (...) {
        report.check(false, "unexpected non-standard exception escaped the smoke suite");
    }
    return report.finish();
}

#endif  // defined(_WIN32)
