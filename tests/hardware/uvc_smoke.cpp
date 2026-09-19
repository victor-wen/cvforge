/*
 * CVF-007 opt-in UVC (Media Foundation) hardware smoke suite -- owner: test-engineer.
 * Hardened by CVF-107 (stress cycles, orientation, color, forced timeout,
 * release evidence).
 *
 * This file is authored BEFORE the Windows UVC backend exists. WSL has no Windows
 * and no camera, so author-phase RED evidence is structural (this translation unit
 * is empty off Windows) and execution evidence is deferred to a designated
 * Windows 10/11 x64 station. No compile/run output is claimed from Linux.
 *
 * ---------------------------------------------------------------------------
 * OPERATOR NOTE (README -- the authoritative copy is docs/hardware-uvc.md)
 * ---------------------------------------------------------------------------
 * Prerequisites
 *   - Windows 10 or Windows 11 x64 with MSVC v143 (Visual Studio 2022) and
 *     CMake >= 3.28.
 *   - A UVC camera reachable through Media Foundation (driverless class driver).
 *   - A build configured with the opt-in CMake option CVFORWIN_BUILD_HARDWARE_TESTS
 *     (default OFF). The target is cvf_hw_uvc_smoke. It is opt-in, is NEVER
 *     registered with CTest, and is NEVER part of mandatory CI, because hosted
 *     CI has no physical camera.
 *
 * Discover the identity first
 *   Run:  cvf_hw_uvc_smoke --list
 *   The listing prints every enumerated UVC descriptor (device_path, vendor_id,
 *   product_id, friendly_name) and exits. There is no first-device fallback: the
 *   suite refuses to run without an explicitly configured selector, and a missing
 *   selector is a CONFIGURATION FAILURE (nonzero exit), not a pass.
 *
 * Required selector (exactly one device; never the first enumerated device)
 *   CVF_HW_UVC_DEVICE_PATH   preferred; the Media Foundation symbolic link (exact)
 *   CVF_HW_UVC_VID          four lowercase hex digits (exact)
 *   CVF_HW_UVC_PID          four lowercase hex digits (exact)
 *   CVF_HW_UVC_NAME         friendly name (exact)
 *
 * Capture settings and cycles
 *   CVF_HW_WIDTH / CVF_HW_HEIGHT   configured capture size (default 640 / 480)
 *   CVF_HW_FPS                     configured frame rate, 0 = backend default
 *   CVF_HW_CYCLES                  repeated-capture count (default 50, min 1,
 *                                  max 100000)
 *
 * Stress mode (>= 1000 consecutive captures plus mandatory orientation/color)
 *   CVF_HW_STRESS=1                enable stress mode
 *   Stress mode is also implied when CVF_HW_CYCLES >= 1000. In stress mode:
 *     - CVF_HW_CYCLES must be >= 1000, otherwise the run FAILS;
 *     - the orientation and color checks are MANDATORY: if the operator did not
 *       set up the documented target, the check is reported as a FAILURE, never a
 *       silent skip;
 *     - a skipped forced-timeout path is a FAILURE;
 *     - the zero-device boundary run is rejected.
 *   CVF_HW_REQUIRE_ORIENTATION=1   force mandatory orientation outside stress
 *   CVF_HW_REQUIRE_COLOR=1         force mandatory color outside stress
 *   CVF_HW_REQUIRE_TIMEOUT=1       force mandatory timeout evidence outside stress
 *
 * Orientation procedure (documented target CVF-ORIENT-1)
 *   Target: a portrait card whose TOP third is bright white and whose BOTTOM
 *   third is matte black (vertically asymmetric). Fill the central half of the
 *   frame, upright in the camera's visual field, evenly lit.
 *   CVF_HW_ORIENTATION_TEST=1              run the orientation check
 *   CVF_HW_ORIENTATION_MIN_CONTRAST        luma contrast threshold (default 40/255)
 *   CVF_HW_ORIENTATION_SYMMETRIC_CONTROL=1 negative control: a vertically
 *                                          symmetric card must be REJECTED as
 *                                          orientation evidence (proves the check
 *                                          cannot be satisfied by a symmetric
 *                                          image). NOT a substitute for the
 *                                          positive target: when orientation is
 *                                          required, CVF_HW_ORIENTATION_TEST
 *                                          must still be set or the check FAILS.
 *                                          The control is captured and evaluated
 *                                          separately from the positive target
 *                                          and needs its own run with a
 *                                          vertically symmetric card.
 *   The check measures the mean luma of the central top band and bottom band.
 *   It PASSES only when the absolute contrast is at least the threshold AND the
 *   bright band is at the visual top (no vertical flip). A symmetric image has
 *   near-zero contrast and therefore FAILS.
 *
 * Color procedure (documented target CVF-COLOR-1)
 *   Target: a matte saturated swatch (red, green, or blue) filling the central
 *   half of the frame under the station's normal lighting.
 *   CVF_HW_COLOR_TEST=1                    run the color check
 *   CVF_HW_COLOR_EXPECT                    red | green | blue | neutral (default red)
 *   CVF_HW_COLOR_MIN_DOMINANCE             expected-channel lead (default 30/255)
 *   CVF_HW_COLOR_MIN_LEVEL                 expected-channel minimum (default 60/255)
 *   CVF_HW_COLOR_MAX_SPREAD                neutral control max spread (default 30/255)
 *   CVF_HW_COLOR_REF_B / REF_G / REF_R     optional measured reference (0..255)
 *   CVF_HW_COLOR_TOLERANCE                 per-channel tolerance (default 40/255)
 *   PASS requires the expected channel to lead the others by MIN_DOMINANCE and to
 *   reach MIN_LEVEL; when all three REF_* values are configured, every channel
 *   must also be within TOLERANCE of its reference. A neutral card PASSES only
 *   the neutral control (channel spread <= MAX_SPREAD); a saturated card in
 *   neutral mode FAILS, and vice versa. Measured values and thresholds are
 *   printed for the release record. The neutral control is NOT a substitute for
 *   the positive target: when the colour check is mandatory, the run fails
 *   unless a saturated target (red, green, or blue) is actually tested.
 *
 * Forced timeout procedure
 *   Always exercised while the device is open, deterministically, with no
 *   sleeping: an already-expired deadline and a zero timeout must both fail with
 *   timeout/capture_timed_out, the bounded capture_with_one_retry helper must
 *   return that timeout unchanged (no retry, no camera_io conversion), and the
 *   device must remain usable afterwards.
 *
 * Manual unplug/replug step (never automatic)
 *   Run with CVF_HW_UNPLUG_TEST=1. The suite then:
 *     1. opens the camera, reconnects once (bounded) to prove reconnect works,
 *        then prompts you to unplug it and press Enter;
 *     2. expects the next capture to fail with camera_io and the bounded
 *        capture_with_one_retry attempt to fail within its deadline;
 *     3. prompts you to replug into the SAME USB port and press Enter;
 *     4. expects one bounded reconnect + recapture to recover.
 *   With CVF_HW_UNPLUG_TEST unset the case is reported as skipped. The at-most-one
 *   retry rule is pinned deterministically by the hardware-free CVF-002 suite; this
 *   hardware case observes the bounded recovery behavior end to end.
 *
 * Release-evidence record
 *   The suite prints a delimited CVF-107 RELEASE EVIDENCE block containing device
 *   identity, cycle count, Windows version, camera driver/firmware (best effort),
 *   stress/requirement flags, check counts, and the process exit code.
 *     CVF_HW_EVIDENCE_FILE     also write the block to this file (operator path)
 *     CVF_HW_WINDOWS_VERSION   override/record the exact Windows version
 *     CVF_HW_CAMERA_DRIVER     record the camera driver description (best effort)
 *     CVF_HW_CAMERA_FIRMWARE   record the camera firmware version (best effort)
 *
 * Zero-camera boundary
 *   CVF_HW_EXPECT_ZERO_DEVICES=1   camera-less station run: asserts enumerate is
 *                                  empty and resolution fails with camera_not_found
 *                                  (not allowed in stress mode)
 *
 * Exit code: 0 = every executed check passed (non-required skips allowed),
 * 1 = at least one failure (including a missing selector or a mandatory check
 * that could not be executed).
 *
 * Interface assumption (recorded in .ai/reports/CVF-007-test-red.yaml):
 * src/camera/uvc_windows/uvc_backend.h declares a default-constructible
 * cvforwin::camera::UvcCameraBackend implementing cvforwin::camera::ICameraBackend
 * and reporting backend_key() == "uvc". The static assertions below pin that
 * assumption at compile time.
 * ---------------------------------------------------------------------------
 */

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
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

namespace cvf107 {

namespace cam = cvforwin::camera;
namespace core = cvforwin::core;

using HwBackend = cam::UvcCameraBackend;

static_assert(std::is_base_of_v<cam::ICameraBackend, HwBackend>,
              "CVF-107 assumption: UvcCameraBackend must implement cvforwin::camera::ICameraBackend");
static_assert(std::is_default_constructible_v<HwBackend>,
              "CVF-107 assumption: UvcCameraBackend must be default-constructible");

constexpr std::uint32_t kOpenTimeoutMs = 8000;
constexpr std::uint32_t kCaptureTimeoutMs = 3000;
constexpr std::uint32_t kReplugTimeoutMs = 8000;
constexpr std::uint32_t kDefaultWidth = 640;
constexpr std::uint32_t kDefaultHeight = 480;
constexpr std::uint32_t kDefaultCycles = 50;
constexpr std::uint32_t kMinCycles = 1;
constexpr std::uint32_t kMaxCycles = 100000;
constexpr std::uint32_t kStressMinCycles = 1000;
constexpr std::int64_t kDeadlineSlackMs = 2000;
constexpr double kOrientationMinContrast = 40.0;
constexpr double kColorMinDominance = 30.0;
constexpr double kColorMinLevel = 60.0;
constexpr double kColorMaxNeutralSpread = 30.0;
constexpr double kColorDefaultTolerance = 40.0;

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

    int passed() const noexcept
    {
        return passed_;
    }

    int failed() const noexcept
    {
        return failed_;
    }

    int skipped() const noexcept
    {
        return skipped_;
    }

    int finish() const
    {
        std::cout << "\nCVF-107 UVC hardware smoke summary: passed=" << passed_ << " failed=" << failed_
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
        if (parsed == 0 || parsed > kMaxCycles) {
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

bool env_double(const char* name, double& out)
{
    const std::string value = env_text(name);
    if (value.empty()) {
        return false;
    }
    try {
        out = std::stod(value);
        return true;
    } catch (...) {
        return false;
    }
}

std::uint32_t configured_cycles()
{
    const std::uint32_t value = env_u32("CVF_HW_CYCLES", kDefaultCycles);
    if (value < kMinCycles) {
        return kMinCycles;
    }
    if (value > kMaxCycles) {
        return kMaxCycles;
    }
    return value;
}

bool stress_requested()
{
    return env_flag("CVF_HW_STRESS");
}

bool stress_mode(std::uint32_t cycles)
{
    return stress_requested() || cycles >= kStressMinCycles;
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
// Release-evidence record (identity, cycles, OS, driver/firmware, exit code).
// ---------------------------------------------------------------------------

struct EvidenceRecord {
    std::string backend_key;
    std::string device_path;
    std::string vendor_id;
    std::string product_id;
    std::string friendly_name;
    std::string selector_kind;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double frame_rate = 0.0;
    std::uint32_t cycles = 0;
    bool stress = false;
    bool require_orientation = false;
    bool require_color = false;
    bool require_timeout = false;
    bool orientation_test = false;
    double orientation_min_contrast = 0.0;
    bool orientation_symmetric_control = false;
    bool color_test = false;
    std::string color_expect;
    double color_min_dominance = 0.0;
    double color_min_level = 0.0;
    double color_max_spread = 0.0;
    double color_tolerance = 0.0;
    bool color_reference_configured = false;
    bool unplug_test = false;
    bool expect_zero_devices = false;
    std::string windows_version;
    std::string camera_driver;
    std::string camera_firmware;
};

EvidenceRecord g_evidence;

struct CvfOsVersionInfo {
    unsigned long dwOSVersionInfoSize = 0;
    unsigned long dwMajorVersion = 0;
    unsigned long dwMinorVersion = 0;
    unsigned long dwBuildNumber = 0;
    unsigned long dwPlatformId = 0;
    wchar_t szCSDVersion[128] = {};
};

using RtlGetVersionFn = LONG(WINAPI*)(CvfOsVersionInfo*);

std::string detect_windows_version()
{
    const std::string override_value = env_text("CVF_HW_WINDOWS_VERSION");
    if (!override_value.empty()) {
        return override_value;
    }

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        const auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtl_get_version != nullptr) {
            CvfOsVersionInfo info;
            if (rtl_get_version(&info) == 0) {
                return "Windows " + std::to_string(info.dwMajorVersion) + "." +
                       std::to_string(info.dwMinorVersion) + " build " + std::to_string(info.dwBuildNumber);
            }
        }
    }

    const std::string os_env = env_text("OS");
    if (!os_env.empty()) {
        return os_env + " (build unknown; set CVF_HW_WINDOWS_VERSION for exact evidence)";
    }
    return "unknown (set CVF_HW_WINDOWS_VERSION for exact evidence)";
}

std::string evidence_text(int exit_code)
{
    std::ostringstream out;
    out << "CVF-107 UVC hardware stress evidence\n";
    out << "backend_key=" << g_evidence.backend_key << "\n";
    out << "device_path=" << g_evidence.device_path << "\n";
    out << "vendor_id=" << g_evidence.vendor_id << "\n";
    out << "product_id=" << g_evidence.product_id << "\n";
    out << "friendly_name=" << g_evidence.friendly_name << "\n";
    out << "selector_kind=" << g_evidence.selector_kind << "\n";
    out << "configured_width=" << g_evidence.width << "\n";
    out << "configured_height=" << g_evidence.height << "\n";
    out << "configured_fps=" << g_evidence.frame_rate << "\n";
    out << "configured_cycles=" << g_evidence.cycles << "\n";
    out << "stress_mode=" << (g_evidence.stress ? "yes" : "no") << "\n";
    out << "require_orientation=" << (g_evidence.require_orientation ? "yes" : "no") << "\n";
    out << "require_color=" << (g_evidence.require_color ? "yes" : "no") << "\n";
    out << "require_timeout=" << (g_evidence.require_timeout ? "yes" : "no") << "\n";
    out << "orientation_test=" << (g_evidence.orientation_test ? "yes" : "no") << "\n";
    out << "orientation_min_contrast=" << g_evidence.orientation_min_contrast << "\n";
    out << "orientation_symmetric_control=" << (g_evidence.orientation_symmetric_control ? "yes" : "no") << "\n";
    out << "color_test=" << (g_evidence.color_test ? "yes" : "no") << "\n";
    out << "color_expect=" << g_evidence.color_expect << "\n";
    out << "color_min_dominance=" << g_evidence.color_min_dominance << "\n";
    out << "color_min_level=" << g_evidence.color_min_level << "\n";
    out << "color_max_spread=" << g_evidence.color_max_spread << "\n";
    out << "color_reference_configured=" << (g_evidence.color_reference_configured ? "yes" : "no") << "\n";
    out << "color_tolerance=" << g_evidence.color_tolerance << "\n";
    out << "unplug_test=" << (g_evidence.unplug_test ? "yes" : "no") << "\n";
    out << "expect_zero_devices=" << (g_evidence.expect_zero_devices ? "yes" : "no") << "\n";
    out << "windows_version=" << g_evidence.windows_version << "\n";
    out << "camera_driver=" << g_evidence.camera_driver << "\n";
    out << "camera_firmware=" << g_evidence.camera_firmware << "\n";
    out << "process_exit_code=" << exit_code << "\n";
    return out.str();
}

void write_evidence_file(const std::string& text)
{
    const std::string path = env_text("CVF_HW_EVIDENCE_FILE");
    if (path.empty()) {
        return;
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        std::cout << "[warn] cannot write CVF_HW_EVIDENCE_FILE=\"" << path << "\"\n";
        return;
    }
    file << text;
    file.flush();
    if (file) {
        std::cout << "[info] release evidence written to \"" << path << "\"\n";
    } else {
        std::cout << "[warn] failed while writing CVF_HW_EVIDENCE_FILE=\"" << path << "\"\n";
    }
}

void emit_release_evidence(const Report& report, int exit_code)
{
    std::ostringstream out;
    out << "=== CVF-107 RELEASE EVIDENCE BEGIN ===\n";
    out << evidence_text(exit_code);
    out << "checks_passed=" << report.passed() << "\n";
    out << "checks_failed=" << report.failed() << "\n";
    out << "checks_skipped=" << report.skipped() << "\n";
    out << "=== CVF-107 RELEASE EVIDENCE END ===\n";
    std::cout << '\n'
              << out.str();
    write_evidence_file(out.str());
}

// ---------------------------------------------------------------------------
// Frame sampling helpers (orientation and color).
// ---------------------------------------------------------------------------

core::Result<cam::CapturedFrame> capture_one(const cam::CameraDescriptor& descriptor,
                                             const cam::CameraSettings& settings)
{
    HwBackend backend;
    auto opened = backend.open(descriptor, settings, open_deadline());
    if (!opened.has_value()) {
        std::cout << "  open failure: " << failure_text(opened.failure()) << '\n';
        return core::Result<cam::CapturedFrame>(opened.failure());
    }
    auto captured = backend.capture(capture_deadline());
    backend.close();
    return captured;
}

double mean_luma(const cv::Mat& bgr, const cv::Rect& roi)
{
    const cv::Scalar mean = cv::mean(bgr(roi));
    return 0.114 * mean[0] + 0.587 * mean[1] + 0.299 * mean[2];
}

struct BandLuma {
    bool valid = false;
    double top = 0.0;
    double bottom = 0.0;
};

BandLuma measure_orientation(const cv::Mat& bgr)
{
    BandLuma result;
    const int width = bgr.cols;
    const int height = bgr.rows;
    const int band_width = width / 2;
    const int band_height = height / 4;
    const int left = width / 4;
    if (band_width < 4 || band_height < 2 || height < 12) {
        return result;
    }
    const int top_y = height / 8;
    const int bottom_y = height - height / 8 - band_height;
    if (bottom_y <= top_y + band_height) {
        return result;
    }
    result.top = mean_luma(bgr, cv::Rect(left, top_y, band_width, band_height));
    result.bottom = mean_luma(bgr, cv::Rect(left, bottom_y, band_width, band_height));
    result.valid = true;
    return result;
}

struct ColorMeasurement {
    bool valid = false;
    double blue = 0.0;
    double green = 0.0;
    double red = 0.0;
};

ColorMeasurement measure_color(const cv::Mat& bgr)
{
    ColorMeasurement result;
    const int width = bgr.cols;
    const int height = bgr.rows;
    const int roi_width = width / 2;
    const int roi_height = height / 2;
    if (roi_width < 4 || roi_height < 4) {
        return result;
    }
    const cv::Scalar mean = cv::mean(bgr(cv::Rect(width / 4, height / 4, roi_width, roi_height)));
    result.blue = mean[0];
    result.green = mean[1];
    result.red = mean[2];
    result.valid = true;
    return result;
}

enum class ColorTarget { red,
                         green,
                         blue,
                         neutral };

std::string lower_copy(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

bool parse_color_target(const std::string& text, ColorTarget& out)
{
    const std::string lowered = lower_copy(text);
    if (lowered == "red") {
        out = ColorTarget::red;
    } else if (lowered == "green") {
        out = ColorTarget::green;
    } else if (lowered == "blue") {
        out = ColorTarget::blue;
    } else if (lowered == "neutral") {
        out = ColorTarget::neutral;
    } else {
        return false;
    }
    return true;
}

const char* color_target_name(ColorTarget target)
{
    switch (target) {
    case ColorTarget::red:
        return "red";
    case ColorTarget::green:
        return "green";
    case ColorTarget::blue:
        return "blue";
    case ColorTarget::neutral:
    default:
        return "neutral";
    }
}

double channel_value(const ColorMeasurement& measurement, int channel)
{
    switch (channel) {
    case 0:
        return measurement.blue;
    case 1:
        return measurement.green;
    default:
        return measurement.red;
    }
}

const char* channel_name(int channel)
{
    switch (channel) {
    case 0:
        return "B";
    case 1:
        return "G";
    default:
        return "R";
    }
}

std::string format_double(double value)
{
    std::ostringstream out;
    out.precision(2);
    out << std::fixed << value;
    return out.str();
}

const char* yes_no(bool value)
{
    return value ? "yes" : "no";
}

// B4A-03: describe the specific CVF_HW_* requirement/control flags actually set,
// so every mandatory-check failure message is self-evidencing in an archive.
std::string orientation_flag_state(bool orientation_test, bool symmetric_control)
{
    return std::string("flags in effect: stress_mode=") + yes_no(stress_mode(configured_cycles())) +
           " CVF_HW_STRESS=" + yes_no(env_flag("CVF_HW_STRESS")) +
           " CVF_HW_CYCLES=" + std::to_string(configured_cycles()) +
           " CVF_HW_REQUIRE_ORIENTATION=" + yes_no(env_flag("CVF_HW_REQUIRE_ORIENTATION")) +
           " CVF_HW_ORIENTATION_TEST=" + yes_no(orientation_test) +
           " CVF_HW_ORIENTATION_SYMMETRIC_CONTROL=" + yes_no(symmetric_control) +
           " CVF_HW_ORIENTATION_MIN_CONTRAST=" +
           format_double(env_f64("CVF_HW_ORIENTATION_MIN_CONTRAST", kOrientationMinContrast));
}

std::string color_flag_state(bool color_test, const std::string& color_expect)
{
    return std::string("flags in effect: stress_mode=") + yes_no(stress_mode(configured_cycles())) +
           " CVF_HW_STRESS=" + yes_no(env_flag("CVF_HW_STRESS")) +
           " CVF_HW_CYCLES=" + std::to_string(configured_cycles()) +
           " CVF_HW_REQUIRE_COLOR=" + yes_no(env_flag("CVF_HW_REQUIRE_COLOR")) +
           " CVF_HW_COLOR_TEST=" + yes_no(color_test) +
           " CVF_HW_COLOR_EXPECT=" + (color_expect.empty() ? std::string("unset (default red)") : color_expect);
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

    // A closed backend instance is single-use by contract: close() releases the
    // device and the same instance is not reopened. Re-activation within a live
    // session is reconnect()'s job, not close()-then-open() on one instance. What
    // must hold is that the descriptor stays usable, so a FRESH backend instance
    // must be able to open it again and capture from it.
    HwBackend reopened_backend;
    auto reopened = reopened_backend.open(descriptor, settings, open_deadline());
    report.check(reopened.has_value(),
                 "H3 a fresh backend instance can reopen the same descriptor after close");
    if (reopened.has_value()) {
        auto recaptured = reopened_backend.capture(capture_deadline());
        report.check(recaptured.has_value(),
                     "H3 a capture from the fresh backend instance succeeds (descriptor stays usable)");
    }
    reopened_backend.close();

    return result;
}

// ---------------------------------------------------------------------------
// H5: forced timeout path (deterministic, explicit, never silent).
// ---------------------------------------------------------------------------

struct TimeoutResult {
    bool exercised = false;
    bool passed = false;
};

TimeoutResult run_forced_timeout_case(Report& report, const cam::CameraDescriptor& descriptor,
                                      const cam::CameraSettings& settings)
{
    TimeoutResult result;
    HwBackend backend;
    auto opened = backend.open(descriptor, settings, open_deadline());
    report.check(opened.has_value(), "H5 open before the forced-timeout probe succeeds");
    if (!opened.has_value()) {
        std::cout << "  open failure: " << failure_text(opened.failure()) << '\n';
        return result;
    }
    result.exercised = true;

    auto expired = backend.capture(core::Deadline::immediate());
    result.passed = !expired.has_value() && expired.failure().status == core::Status::timeout &&
                    expired.failure().code == core::ErrorCode::capture_timed_out;
    report.check(result.passed, "H5a capture with an already-expired deadline -> timeout/capture_timed_out");
    if (!expired.has_value()) {
        std::cout << "  observed: " << failure_text(expired.failure()) << '\n';
    }

    auto zero_timeout = backend.capture(core::Deadline::from_timeout_ms(0));
    result.passed = result.passed && !zero_timeout.has_value() &&
                    zero_timeout.failure().status == core::Status::timeout &&
                    zero_timeout.failure().code == core::ErrorCode::capture_timed_out;
    report.check(!zero_timeout.has_value() && zero_timeout.failure().status == core::Status::timeout &&
                     zero_timeout.failure().code == core::ErrorCode::capture_timed_out,
                 "H5b capture with timeout_ms == 0 -> timeout/capture_timed_out");
    if (!zero_timeout.has_value()) {
        std::cout << "  observed: " << failure_text(zero_timeout.failure()) << '\n';
    }

    auto bounded = cam::capture_with_one_retry(backend, settings, core::Deadline::immediate());
    result.passed = result.passed && !bounded.has_value() && bounded.failure().status == core::Status::timeout;
    report.check(!bounded.has_value() && bounded.failure().status == core::Status::timeout,
                 "H5c bounded capture_with_one_retry returns the timeout unchanged (no retry, no camera_io)");
    if (!bounded.has_value()) {
        std::cout << "  observed: " << failure_text(bounded.failure()) << '\n';
    }

    auto recovered = backend.capture(capture_deadline());
    result.passed = result.passed && recovered.has_value();
    report.check(recovered.has_value(), "H5d the device stays usable after the forced-timeout probes");
    backend.close();

    return result;
}

// ---------------------------------------------------------------------------
// H7: orientation check against the documented asymmetric target.
// ---------------------------------------------------------------------------

void run_orientation_case(Report& report, const cam::CameraDescriptor& descriptor,
                          const cam::CameraSettings& settings, bool required)
{
    const bool orientation_test = env_flag("CVF_HW_ORIENTATION_TEST");
    const bool symmetric_control = env_flag("CVF_HW_ORIENTATION_SYMMETRIC_CONTROL");
    const double min_contrast = env_f64("CVF_HW_ORIENTATION_MIN_CONTRAST", kOrientationMinContrast);

    // The symmetric negative control is NOT a substitute for the positive
    // asymmetric target. A mandatory orientation check therefore fails when the
    // documented asymmetric CVF-ORIENT-1 target is not requested, even if the
    // operator set CVF_HW_ORIENTATION_SYMMETRIC_CONTROL: a vertically symmetric
    // card can never evidence that the bright TOP band is at the visual top.
    if (required && !orientation_test) {
        report.check(false,
                     "H7 orientation check is MANDATORY but CVF_HW_ORIENTATION_TEST is not set with the documented "
                     "asymmetric CVF-ORIENT-1 target in view (the CVF_HW_ORIENTATION_SYMMETRIC_CONTROL negative "
                     "control is not a substitute for the positive target; " +
                         orientation_flag_state(orientation_test, symmetric_control) + ")");
        return;
    }

    if (!orientation_test && !symmetric_control) {
        report.skip("H7 orientation target", "set CVF_HW_ORIENTATION_TEST=1 with the CVF-ORIENT-1 target in view");
        return;
    }

    // B4A-02: the positive checks are evaluated only against the positive
    // asymmetric target that is in view at positive-check time.
    if (orientation_test) {
        auto captured = capture_one(descriptor, settings);
        if (!captured.has_value()) {
            report.check(false, "H7 capture the orientation target frame");
        } else {
            const BandLuma bands = measure_orientation(captured.value().pixels);
            if (!bands.valid) {
                report.check(false, "H7 the captured frame is large enough for the top/bottom orientation bands");
            } else {
                report.info("H7 orientation: top_luma=" + format_double(bands.top) +
                            " bottom_luma=" + format_double(bands.bottom) +
                            " contrast=" + format_double(bands.top - bands.bottom));
                report.check(std::fabs(bands.top - bands.bottom) >= min_contrast,
                             "H7 orientation evidence present: top/bottom luma contrast >= " +
                                 format_double(min_contrast));
                report.check(bands.top - bands.bottom >= min_contrast,
                             "H7 orientation correct: the documented bright TOP band is at the visual top (no "
                             "vertical flip)");
            }
        }
    }

    // B4A-02: the negative control is captured and evaluated separately at
    // control time, so the positive asymmetric target and the vertically
    // symmetric control are never required to hold on one frame. A failure in
    // one independent check does not prevent the other from being evaluated when
    // the operator has set up only one of the two targets.
    if (symmetric_control) {
        auto control_captured = capture_one(descriptor, settings);
        if (!control_captured.has_value()) {
            report.check(false, "H7 capture the vertically symmetric control frame");
        } else {
            const BandLuma control_bands = measure_orientation(control_captured.value().pixels);
            if (!control_bands.valid) {
                report.check(false, "H7 the control frame is large enough for the top/bottom orientation bands");
            } else {
                const double control_contrast = std::fabs(control_bands.top - control_bands.bottom);
                report.info("H7 control orientation: top_luma=" + format_double(control_bands.top) +
                            " bottom_luma=" + format_double(control_bands.bottom) +
                            " contrast=" + format_double(control_bands.top - control_bands.bottom));
                report.check(control_contrast < min_contrast,
                             "H7 negative control: a vertically symmetric target is NOT accepted as orientation "
                             "evidence (contrast " +
                                 format_double(control_contrast) + " < " + format_double(min_contrast) + ")");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// H8: color check against the documented target and tolerances.
// ---------------------------------------------------------------------------

void run_color_case(Report& report, const cam::CameraDescriptor& descriptor, const cam::CameraSettings& settings,
                    bool required)
{
    const bool color_test = env_flag("CVF_HW_COLOR_TEST");
    const std::string requested_expect = env_text("CVF_HW_COLOR_EXPECT");

    // B4A-01: the neutral spread control is NOT a substitute for the positive
    // saturated CVF-COLOR-1 swatch. A mandatory colour check fails closed when
    // the documented saturated target is not requested, even when the operator
    // asks for the neutral control.
    if (required && !color_test) {
        report.check(false,
                     "H8 color check is MANDATORY but CVF_HW_COLOR_TEST is not set with the documented saturated "
                     "CVF-COLOR-1 target in view (the neutral spread control is not a substitute for the positive "
                     "target; " +
                         color_flag_state(color_test, requested_expect) + ")");
        return;
    }

    if (!color_test) {
        report.skip("H8 color target", "set CVF_HW_COLOR_TEST=1 with the CVF-COLOR-1 target in view");
        return;
    }

    ColorTarget target = ColorTarget::red;
    const std::string target_text = requested_expect;
    if (!target_text.empty() && !parse_color_target(target_text, target)) {
        report.check(false, "H8 CVF_HW_COLOR_EXPECT must be red|green|blue|neutral (got \"" + target_text + "\")");
        return;
    }

    // B4A-01: a mandatory colour check must exercise the saturated positive
    // target; the neutral spread control can never satisfy it.
    if (required && target == ColorTarget::neutral) {
        report.check(false,
                     "H8 color check is MANDATORY but CVF_HW_COLOR_EXPECT=neutral requests only the neutral "
                     "spread control, which is not a substitute for the positive saturated CVF-COLOR-1 target; "
                     "set CVF_HW_COLOR_EXPECT=red|green|blue with the saturated swatch in view (" +
                         color_flag_state(color_test, requested_expect) + ")");
        return;
    }

    auto captured = capture_one(descriptor, settings);
    if (!captured.has_value()) {
        report.check(false, "H8 capture the color target frame");
        return;
    }

    const ColorMeasurement measurement = measure_color(captured.value().pixels);
    if (!measurement.valid) {
        report.check(false, "H8 the captured frame is large enough for the central color ROI");
        return;
    }
    report.info("H8 color: expected=" + std::string(color_target_name(target)) +
                " measured BGR=(" + format_double(measurement.blue) + ", " + format_double(measurement.green) +
                ", " + format_double(measurement.red) + ")");

    const double min_dominance = env_f64("CVF_HW_COLOR_MIN_DOMINANCE", kColorMinDominance);
    const double min_level = env_f64("CVF_HW_COLOR_MIN_LEVEL", kColorMinLevel);
    const double max_neutral_spread = env_f64("CVF_HW_COLOR_MAX_SPREAD", kColorMaxNeutralSpread);

    if (target == ColorTarget::neutral) {
        // B4A-01: the neutral spread control is a non-required control and is
        // never a substitute for the positive saturated target (a mandatory
        // neutral request already failed closed above).
        const double highest = std::max(measurement.blue, std::max(measurement.green, measurement.red));
        const double lowest = std::min(measurement.blue, std::min(measurement.green, measurement.red));
        const double spread = highest - lowest;
        report.check(spread <= max_neutral_spread,
                     "H8 neutral control: channel spread " + format_double(spread) + " <= " +
                         format_double(max_neutral_spread));
        return;
    }

    const int expected_channel = target == ColorTarget::blue ? 0 : (target == ColorTarget::green ? 1 : 2);
    const double expected_value = channel_value(measurement, expected_channel);
    const double other_highest = std::max(channel_value(measurement, (expected_channel + 1) % 3),
                                          channel_value(measurement, (expected_channel + 2) % 3));
    const double dominance = expected_value - other_highest;
    report.info("H8 dominance(" + std::string(channel_name(expected_channel)) + ")=" + format_double(dominance) +
                " min_dominance=" + format_double(min_dominance) + " value=" + format_double(expected_value) +
                " min_level=" + format_double(min_level));
    report.check(dominance >= min_dominance && expected_value >= min_level,
                 "H8 color target " + std::string(color_target_name(target)) + ": dominance >= " +
                     format_double(min_dominance) + " and level >= " + format_double(min_level));

    double ref_blue = 0.0;
    double ref_green = 0.0;
    double ref_red = 0.0;
    const bool has_blue = env_double("CVF_HW_COLOR_REF_B", ref_blue);
    const bool has_green = env_double("CVF_HW_COLOR_REF_G", ref_green);
    const bool has_red = env_double("CVF_HW_COLOR_REF_R", ref_red);
    if (has_blue && has_green && has_red) {
        const double tolerance = env_f64("CVF_HW_COLOR_TOLERANCE", kColorDefaultTolerance);
        const bool blue_ok = std::fabs(measurement.blue - ref_blue) <= tolerance;
        const bool green_ok = std::fabs(measurement.green - ref_green) <= tolerance;
        const bool red_ok = std::fabs(measurement.red - ref_red) <= tolerance;
        report.info("H8 reference BGR=(" + format_double(ref_blue) + ", " + format_double(ref_green) + ", " +
                    format_double(ref_red) + ") tolerance=" + format_double(tolerance));
        report.check(blue_ok && green_ok && red_ok,
                     "H8 color target within per-channel tolerance " + format_double(tolerance) +
                         " (B " + format_double(measurement.blue) + ", G " + format_double(measurement.green) +
                         ", R " + format_double(measurement.red) + ")");
    } else {
        report.info("H8 no CVF_HW_COLOR_REF_B/G/R configured; dominance check only (set all three for a "
                    "per-channel tolerance check)");
    }
}

// ---------------------------------------------------------------------------
// H4: repeated capture cycles without resource exhaustion (stress >= 1000).
// ---------------------------------------------------------------------------

void run_repeated_cycles_case(Report& report, const cam::CameraDescriptor& descriptor,
                              const cam::CameraSettings& settings, bool stress)
{
    const std::uint32_t cycles = configured_cycles();

    if (stress_requested() && cycles < kStressMinCycles) {
        report.check(false, "CVF_HW_STRESS=1 requires CVF_HW_CYCLES >= " + std::to_string(kStressMinCycles) +
                                " (got " + std::to_string(cycles) + "); refusing a misleading short stress run");
        return;
    }

    report.check(cycles >= kMinCycles && cycles <= kMaxCycles,
                 "H4 configured cycle count is within [" + std::to_string(kMinCycles) + ", " +
                     std::to_string(kMaxCycles) + "] (got " + std::to_string(cycles) + ")");
    report.info("H4 repeated-capture case: cycles=" + std::to_string(cycles) +
                " stress=" + std::string(stress ? "yes" : "no"));

    HwBackend backend;
    auto opened = backend.open(descriptor, settings, open_deadline());
    report.check(opened.has_value(), "H4 open before the repeated-capture case succeeds");
    if (!opened.has_value()) {
        std::cout << "  open failure: " << failure_text(opened.failure()) << '\n';
        return;
    }

    int valid = 0;
    int invalid = 0;
    int dimension_violations = 0;
    int type_violations = 0;
    int continuity_violations = 0;
    int sequence_violations = 0;
    bool have_previous = false;
    bool sequence_strictly_increasing = true;
    std::uint64_t previous_sequence = 0;
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;

    for (std::uint32_t index = 0; index < cycles; ++index) {
        auto captured = backend.capture(capture_deadline());
        bool ok = captured.has_value();
        if (ok) {
            const cam::CapturedFrame& frame = captured.value();
            const bool dimensions_ok = frame.metadata.width == settings.width &&
                                       frame.metadata.height == settings.height &&
                                       frame.pixels.cols == static_cast<int>(settings.width) &&
                                       frame.pixels.rows == static_cast<int>(settings.height);
            const bool type_ok = frame.metadata.pixel_format == cam::PixelFormat::bgr8 &&
                                 frame.pixels.type() == CV_8UC3 && !frame.pixels.empty();
            const bool continuity_ok = frame.pixels.isContinuous();
            const bool sequence_ok = !have_previous || frame.metadata.sequence > previous_sequence;
            if (!dimensions_ok) {
                ++dimension_violations;
            }
            if (!type_ok) {
                ++type_violations;
            }
            if (!continuity_ok) {
                ++continuity_violations;
            }
            if (!sequence_ok) {
                ++sequence_violations;
            }
            sequence_strictly_increasing = sequence_strictly_increasing && sequence_ok;
            ok = dimensions_ok && type_ok && continuity_ok && sequence_ok;
            if (have_previous) {
                last_sequence = frame.metadata.sequence;
            } else {
                first_sequence = frame.metadata.sequence;
                last_sequence = frame.metadata.sequence;
            }
            previous_sequence = frame.metadata.sequence;
            have_previous = true;
        }
        if (ok) {
            ++valid;
        } else {
            ++invalid;
        }
        if (stress && cycles >= 100 && (index + 1) % 100 == 0) {
            report.info("H4 stress progress: " + std::to_string(index + 1) + "/" + std::to_string(cycles) +
                        " valid=" + std::to_string(valid) + " invalid=" + std::to_string(invalid));
        }
    }

    report.info("H4 repeated captures: valid=" + std::to_string(valid) + " invalid=" + std::to_string(invalid) +
                " of " + std::to_string(cycles) + " dimension_violations=" + std::to_string(dimension_violations) +
                " type_violations=" + std::to_string(type_violations) +
                " continuity_violations=" + std::to_string(continuity_violations) +
                " sequence_violations=" + std::to_string(sequence_violations));
    report.check(invalid == 0 && valid == static_cast<int>(cycles),
                 "H4 " + std::to_string(cycles) + " repeated captures all return valid frames");
    if (cycles > 1) {
        report.check(dimension_violations == 0 && type_violations == 0 && continuity_violations == 0 &&
                         sequence_violations == 0 && sequence_strictly_increasing &&
                         last_sequence > first_sequence,
                     "H4 every capture has configured dimensions, BGR8 type, a continuous buffer, and a strictly "
                     "increasing sequence");
    }

    auto final_capture = backend.capture(capture_deadline());
    report.check(final_capture.has_value(), "H4 the device remains usable after the repeated-capture loop");

    auto still_enumerated = backend.enumerate(open_deadline());
    report.check(still_enumerated.has_value() && !still_enumerated.value().empty(),
                 "H4 the device is still enumerated after the repeated-capture loop");

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
// H6: bounded reconnect plus manual, env-gated unplug/replug (never automatic).
// ---------------------------------------------------------------------------

void run_manual_replug_case(Report& report, const cam::CameraDescriptor& descriptor,
                            const cam::CameraSettings& settings)
{
    if (!env_flag("CVF_HW_UNPLUG_TEST")) {
        report.skip("H6 bounded reconnect / manual unplug-replug",
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

    auto reconnected = backend.reconnect(settings, core::Deadline::from_timeout_ms(kReplugTimeoutMs));
    report.check(reconnected.has_value(), "H6 bounded reconnect to an attached device succeeds");
    if (reconnected.has_value()) {
        auto recaptured = backend.capture(capture_deadline());
        report.check(recaptured.has_value(), "H6 capture after the bounded reconnect succeeds");
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

std::string selector_kind(const cam::CameraSelector& selector)
{
    if (!selector.device_path.empty()) {
        return "CVF_HW_UVC_DEVICE_PATH";
    }
    if (!selector.vendor_id.empty() && !selector.product_id.empty()) {
        return "CVF_HW_UVC_VID/PID";
    }
    if (!selector.vendor_id.empty() || !selector.product_id.empty()) {
        return "CVF_HW_UVC_VID or CVF_HW_UVC_PID (incomplete pair)";
    }
    if (!selector.friendly_name.empty()) {
        return "CVF_HW_UVC_NAME";
    }
    return "none";
}

void run_suite(Report& report, const std::vector<cam::CameraDescriptor>& devices)
{
    const std::uint32_t cycles = configured_cycles();
    const bool stress = stress_mode(cycles);
    const bool require_orientation = stress || env_flag("CVF_HW_REQUIRE_ORIENTATION");
    const bool require_color = stress || env_flag("CVF_HW_REQUIRE_COLOR");
    const bool require_timeout = stress || env_flag("CVF_HW_REQUIRE_TIMEOUT");

    g_evidence.cycles = cycles;
    g_evidence.stress = stress;
    g_evidence.require_orientation = require_orientation;
    g_evidence.require_color = require_color;
    g_evidence.require_timeout = require_timeout;
    g_evidence.orientation_test = env_flag("CVF_HW_ORIENTATION_TEST");
    g_evidence.orientation_min_contrast = env_f64("CVF_HW_ORIENTATION_MIN_CONTRAST", kOrientationMinContrast);
    g_evidence.orientation_symmetric_control = env_flag("CVF_HW_ORIENTATION_SYMMETRIC_CONTROL");
    g_evidence.color_test = env_flag("CVF_HW_COLOR_TEST");
    {
        const std::string color_expect = env_text("CVF_HW_COLOR_EXPECT");
        g_evidence.color_expect =
            color_expect.empty() ? std::string("red (default; CVF_HW_COLOR_EXPECT unset)") : color_expect;
    }
    g_evidence.color_min_dominance = env_f64("CVF_HW_COLOR_MIN_DOMINANCE", kColorMinDominance);
    g_evidence.color_min_level = env_f64("CVF_HW_COLOR_MIN_LEVEL", kColorMinLevel);
    g_evidence.color_max_spread = env_f64("CVF_HW_COLOR_MAX_SPREAD", kColorMaxNeutralSpread);
    g_evidence.color_tolerance = env_f64("CVF_HW_COLOR_TOLERANCE", kColorDefaultTolerance);
    {
        double color_reference = 0.0;
        g_evidence.color_reference_configured = env_double("CVF_HW_COLOR_REF_B", color_reference) &&
                                                env_double("CVF_HW_COLOR_REF_G", color_reference) &&
                                                env_double("CVF_HW_COLOR_REF_R", color_reference);
    }
    g_evidence.unplug_test = env_flag("CVF_HW_UNPLUG_TEST");
    g_evidence.expect_zero_devices = env_flag("CVF_HW_EXPECT_ZERO_DEVICES");
    g_evidence.windows_version = detect_windows_version();
    g_evidence.camera_driver = env_text("CVF_HW_CAMERA_DRIVER");
    g_evidence.camera_firmware = env_text("CVF_HW_CAMERA_FIRMWARE");
    if (g_evidence.camera_driver.empty()) {
        g_evidence.camera_driver = "unknown (best effort; set CVF_HW_CAMERA_DRIVER)";
    }
    if (g_evidence.camera_firmware.empty()) {
        g_evidence.camera_firmware = "unknown (best effort; set CVF_HW_CAMERA_FIRMWARE)";
    }

    report.info("CVF-107 configuration: cycles=" + std::to_string(cycles) +
                " stress=" + std::string(stress ? "yes" : "no") +
                " require_orientation=" + std::string(require_orientation ? "yes" : "no") +
                " require_color=" + std::string(require_color ? "yes" : "no") +
                " require_timeout=" + std::string(require_timeout ? "yes" : "no"));
    report.info("CVF-107 host: windows_version=" + g_evidence.windows_version);

    if (env_flag("CVF_HW_EXPECT_ZERO_DEVICES")) {
        if (stress) {
            report.check(false, "CVF_HW_EXPECT_ZERO_DEVICES=1 is not allowed in stress mode (stress requires a camera)");
        }
        report.check(devices.empty(), "boundary: zero cameras attached -> enumerate is empty");
        cam::CameraSelector probe;
        probe.device_path = "cvf-hw-zero-device-probe";
        auto none = cam::resolve_identity(devices, probe);
        report.check(!none.has_value() && none.failure().status == core::Status::camera_not_found &&
                         none.failure().code == core::ErrorCode::camera_not_found,
                     "boundary: zero devices -> resolution fails with camera_not_found");
        report.skip("H3-H8 hardware cases", "CVF_HW_EXPECT_ZERO_DEVICES=1 (station has no camera attached)");
        return;
    }

    if (devices.empty()) {
        report.check(false,
                     "H1 at least one UVC device is enumerated (attach a camera, or set "
                     "CVF_HW_EXPECT_ZERO_DEVICES=1 for the camera-less boundary run)");
        report.skip("H2-H8", "no UVC device enumerated");
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
                 "H2 operator selector configured via CVF_HW_UVC_DEVICE_PATH/VID/PID/NAME (a missing selector is a "
                 "configuration failure, never a first-device fallback)");
    if (!selector_configured) {
        report.info("run --list to print device identities, then set the selector environment variables");
        report.skip("H2-H8", "no explicit selector configured");
        return;
    }

    auto resolved_result = cam::resolve_identity(devices, selector);
    report.check(resolved_result.has_value(), "H2 the configured selector resolves exactly one device");
    if (!resolved_result.has_value()) {
        std::cout << "  observed: " << failure_text(resolved_result.failure()) << '\n';
        report.skip("H3-H8", "configured selector did not resolve a unique device");
        return;
    }
    const cam::CameraDescriptor resolved = std::move(resolved_result).value();

    g_evidence.backend_key = resolved.backend_key;
    g_evidence.device_path = resolved.device_path;
    g_evidence.vendor_id = resolved.vendor_id;
    g_evidence.product_id = resolved.product_id;
    g_evidence.friendly_name = resolved.friendly_name;
    g_evidence.selector_kind = selector_kind(selector);

    report.info("H2 resolved device:");
    print_descriptor(resolved);

    run_resolution_cases(report, devices, resolved);

    cam::CameraSettings settings;
    settings.width = env_u32("CVF_HW_WIDTH", kDefaultWidth);
    settings.height = env_u32("CVF_HW_HEIGHT", kDefaultHeight);
    settings.frame_rate = env_f64("CVF_HW_FPS", 0.0);
    settings.preferred_format = cam::PixelFormat::bgr8;
    g_evidence.width = settings.width;
    g_evidence.height = settings.height;
    g_evidence.frame_rate = settings.frame_rate;
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

    const TimeoutResult timeout = run_forced_timeout_case(report, resolved, settings);
    if (require_timeout) {
        report.check(timeout.exercised && timeout.passed,
                     "H5 the forced-timeout path is exercised and passes (mandatory in stress mode)");
    }

    run_orientation_case(report, resolved, settings, require_orientation);
    run_color_case(report, resolved, settings, require_color);
    run_repeated_cycles_case(report, resolved, settings, stress);
    run_negative_cases(report, settings);
    run_manual_replug_case(report, resolved, settings);
}

void print_usage()
{
    std::cout << "CVF-107 UVC hardware stress suite (opt-in, never a CI gate)\n"
              << "usage: cvf_hw_uvc_smoke [--list] [--help]\n"
              << "  --list   enumerate UVC devices, print their identities, and exit\n"
              << "environment:\n"
              << "  CVF_HW_UVC_DEVICE_PATH / CVF_HW_UVC_VID / CVF_HW_UVC_PID / CVF_HW_UVC_NAME (selector)\n"
              << "  CVF_HW_WIDTH CVF_HW_HEIGHT CVF_HW_FPS CVF_HW_CYCLES\n"
              << "  CVF_HW_STRESS=1 CVF_HW_REQUIRE_ORIENTATION=1 CVF_HW_REQUIRE_COLOR=1 CVF_HW_REQUIRE_TIMEOUT=1\n"
              << "  CVF_HW_ORIENTATION_TEST=1 CVF_HW_ORIENTATION_MIN_CONTRAST"
                 " CVF_HW_ORIENTATION_SYMMETRIC_CONTROL=1\n"
              << "  CVF_HW_COLOR_TEST=1 CVF_HW_COLOR_EXPECT=red|green|blue|neutral"
                 " CVF_HW_COLOR_MIN_DOMINANCE CVF_HW_COLOR_MIN_LEVEL\n"
              << "  CVF_HW_COLOR_MAX_SPREAD CVF_HW_COLOR_REF_B CVF_HW_COLOR_REF_G CVF_HW_COLOR_REF_R"
                 " CVF_HW_COLOR_TOLERANCE\n"
              << "  CVF_HW_EXPECT_ZERO_DEVICES=1 CVF_HW_UNPLUG_TEST=1\n"
              << "  CVF_HW_EVIDENCE_FILE CVF_HW_WINDOWS_VERSION CVF_HW_CAMERA_DRIVER CVF_HW_CAMERA_FIRMWARE\n";
}

}  // namespace cvf107

int main(int argc, char** argv)
{
    cvf107::Report report;
    try {
        bool list_mode = false;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--list") {
                list_mode = true;
            } else if (argument == "--help" || argument == "-h") {
                cvf107::print_usage();
                return 0;
            }
        }

        cvf107::HwBackend backend;
        const std::vector<cvforwin::camera::CameraDescriptor> devices = cvf107::check_enumeration(report, backend);
        if (list_mode) {
            if (devices.empty()) {
                report.info("no UVC devices enumerated");
            }
            report.info("set CVF_HW_UVC_DEVICE_PATH or CVF_HW_UVC_VID/PID/NAME from the identities above");
            const int list_code = report.finish();
            cvf107::emit_release_evidence(report, list_code);
            return list_code;
        }

        cvf107::run_suite(report, devices);
    } catch (const std::exception& error) {
        report.check(false, std::string("unexpected exception escaped the smoke suite: ") + error.what());
    } catch (...) {
        report.check(false, "unexpected non-standard exception escaped the smoke suite");
    }

    const int exit_code = report.finish();
    cvf107::emit_release_evidence(report, exit_code);
    return exit_code;
}

#endif  // defined(_WIN32)
