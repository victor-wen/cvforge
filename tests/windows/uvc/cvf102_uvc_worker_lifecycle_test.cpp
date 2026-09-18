/*
 * CVF-102 independent black-box Windows component tests for the frozen UVC
 * worker / COM+Media Foundation lifetime seam.
 *
 * Coverage: brief observable behaviors B1..B6, the boundary_cases, and the
 * negative_cases. Every case drives the frozen seam
 * UvcCameraBackend::worker_snapshot() and the ICameraBackend contract with
 * synthetic selectors and failing opens; no camera, no Media Foundation object,
 * and no physical device is required. The only production surfaces this file
 * includes are the frozen headers:
 *   src/camera/camera_backend.h
 *   src/camera/uvc_windows/uvc_backend.h
 *   src/core/{status,error,result,deadline}.h
 * No production .cpp is read.
 *
 * The snapshot fields are accessed by their brief-frozen names through `auto`,
 * so the file does not depend on any non-frozen snapshot type spelling. The
 * whole translation unit is Windows-only because the UVC backend is guarded by
 * _WIN32; on portable builds it is an empty translation unit, so the portable
 * suite is unaffected and provides no evidence for this Windows-only change.
 */

#if defined(_WIN32)

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <objbase.h>
#include <windows.h>

#include "camera/camera_backend.h"
#include "camera/uvc_windows/uvc_backend.h"
#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"

namespace {

namespace camera = cvforwin::camera;
namespace core = cvforwin::core;

using camera::CameraDescriptor;
using camera::CameraSelector;
using camera::CameraSettings;
using camera::PixelFormat;
using camera::UvcCameraBackend;
using core::Deadline;
using core::ErrorCode;
using core::Status;

/*
 * A descriptor that cannot resolve on a hardware-free station. `backend_key`
 * is the documented UVC key while the device path, VID, and PID name an absent
 * device, so identity resolution never succeeds and never falls back.
 */
CameraDescriptor absent_descriptor()
{
    CameraDescriptor descriptor;
    descriptor.backend_key = "uvc";
    descriptor.device_path = R"(\\?\usb#vid_0bad&pid_0bad#cvf102-absent-device)";
    descriptor.vendor_id = "0bad";
    descriptor.product_id = "0bad";
    descriptor.friendly_name = "cvf102 absent device";
    return descriptor;
}

CameraSettings default_settings()
{
    CameraSettings settings;
    settings.width = 640;
    settings.height = 480;
    settings.frame_rate = 30.0;
    settings.preferred_format = PixelFormat::bgr8;
    return settings;
}

CameraDescriptor make_descriptor(const std::string& device_path,
                                 const std::string& vendor_id,
                                 const std::string& product_id,
                                 const std::string& friendly_name)
{
    CameraDescriptor descriptor;
    descriptor.backend_key = "uvc";
    descriptor.device_path = device_path;
    descriptor.vendor_id = vendor_id;
    descriptor.product_id = product_id;
    descriptor.friendly_name = friendly_name;
    return descriptor;
}

/*
 * Observables derived from the frozen worker_snapshot() seam. The snapshot is
 * taken with `auto` so no non-frozen type name is required.
 */
bool worker_is_stopped(UvcCameraBackend& backend)
{
    const auto snapshot = backend.worker_snapshot();
    return !snapshot.worker_running;
}

bool counts_are_balanced(UvcCameraBackend& backend)
{
    const auto snapshot = backend.worker_snapshot();
    return snapshot.com_init_count == snapshot.com_uninit_count &&
           snapshot.mf_startup_count == snapshot.mf_shutdown_count;
}

bool command_is_marshalled(UvcCameraBackend& backend)
{
    const auto snapshot = backend.worker_snapshot();
    return snapshot.commands_marshalled > 0 && snapshot.worker_thread_id != snapshot.last_caller_thread_id;
}

struct ApartmentProbe {
    bool enumerated = false;
    bool enumerate_succeeded = false;
    bool enumerate_empty = false;
    bool open_failed_documented = false;
    bool capture_failed_documented = false;
    bool reconnect_failed_documented = false;
    bool marshalled = false;
    bool close_balanced = false;
    bool worker_stopped = false;
};

/*
 * Runs every ICameraBackend operation -- enumerate, open, capture, reconnect,
 * and close -- on whichever thread calls it. The probe records only observable
 * results; the caller thread's COM apartment is chosen by the enclosing test
 * (never initialized, or explicitly STA). Every operation must complete on that
 * host thread without a COM apartment failure, so none of these calls may
 * depend on the caller's apartment.
 */
void exercise_apartment(ApartmentProbe& probe)
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    const auto listed = backend.enumerate(deadline);
    probe.enumerated = true;
    probe.enumerate_succeeded = listed.has_value();
    if (listed.has_value()) {
        probe.enumerate_empty = listed.value().empty();
    }

    const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);
    probe.open_failed_documented =
        !opened.has_value() && (opened.failure().status == Status::camera_not_found || opened.failure().status == Status::camera_io);

    const auto captured = backend.capture(deadline);
    probe.capture_failed_documented = !captured.has_value() &&
                                      (captured.failure().status == Status::camera_not_found ||
                                       captured.failure().status == Status::camera_io);

    const auto reconnected = backend.reconnect(default_settings(), deadline);
    probe.reconnect_failed_documented = !reconnected.has_value() &&
                                        (reconnected.failure().status == Status::camera_not_found ||
                                         reconnected.failure().status == Status::camera_io);

    probe.marshalled = command_is_marshalled(backend);

    backend.close();
    probe.close_balanced = counts_are_balanced(backend);
    probe.worker_stopped = worker_is_stopped(backend);
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* B1: no host COM initialization.                                           */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-102 B1: a no-COM caller thread completes every operation without apartment failure",
          "[cvf-102][B1]")
{
    ApartmentProbe probe;
    std::thread caller([&probe] { exercise_apartment(probe); });
    caller.join();

    CHECK(probe.enumerated);
    CHECK(probe.enumerate_succeeded);
    CHECK(probe.enumerate_empty);
    CHECK(probe.open_failed_documented);
    CHECK(probe.capture_failed_documented);
    CHECK(probe.reconnect_failed_documented);
    CHECK(probe.marshalled);
    CHECK(probe.close_balanced);
    CHECK(probe.worker_stopped);
}

/* ------------------------------------------------------------------------- */
/* B2: STA-initialized host thread.                                          */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-102 B2: an STA caller thread completes every operation without apartment failure",
          "[cvf-102][B2]")
{
    ApartmentProbe probe;
    bool sta_ready = false;
    std::thread caller([&probe, &sta_ready] {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        sta_ready = SUCCEEDED(hr);
        exercise_apartment(probe);
        if (SUCCEEDED(hr)) {
            ::CoUninitialize();
        }
    });
    caller.join();

    CHECK(sta_ready);
    CHECK(probe.enumerate_succeeded);
    CHECK(probe.enumerate_empty);
    CHECK(probe.open_failed_documented);
    CHECK(probe.capture_failed_documented);
    CHECK(probe.reconnect_failed_documented);
    CHECK(probe.marshalled);
    CHECK(probe.close_balanced);
    CHECK(probe.worker_stopped);
}

/* ------------------------------------------------------------------------- */
/* B3: identity resolution, no first-device fallback.                        */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-102 B3: open with an identity that resolves nothing fails camera_not_found with no fallback",
          "[cvf-102][B3]")
{
    UvcCameraBackend backend;
    const auto opened = backend.open(absent_descriptor(), default_settings(), Deadline::from_timeout_ms(5000));

    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.failure().status == Status::camera_not_found);
    CHECK(opened.failure().code == ErrorCode::camera_not_found);

    backend.close();
}

TEST_CASE("CVF-102 B3: identity resolution is exact and never falls back to the first candidate",
          "[cvf-102][B3]")
{
    std::vector<CameraDescriptor> candidates;
    candidates.push_back(make_descriptor("/dev/a", "1234", "5678", "Cam A"));
    candidates.push_back(make_descriptor("/dev/b", "1234", "5678", "Cam B"));

    SECTION("a selector matching exactly one candidate resolves that descriptor")
    {
        CameraSelector selector;
        selector.device_path = "/dev/a";
        const auto resolved = camera::resolve_identity(candidates, selector);
        REQUIRE(resolved.has_value());
        CHECK(resolved.value().device_path == "/dev/a");
    }

    SECTION("a selector matching no candidate fails with camera_not_found")
    {
        CameraSelector selector;
        selector.device_path = "/dev/missing";
        const auto resolved = camera::resolve_identity(candidates, selector);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.failure().code == ErrorCode::camera_not_found);
    }

    SECTION("a selector matching two candidates fails ambiguous and does not take the first")
    {
        CameraSelector selector;
        selector.vendor_id = "1234";
        selector.product_id = "5678";
        const auto resolved = camera::resolve_identity(candidates, selector);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.failure().code == ErrorCode::camera_identity_ambiguous);
    }

    SECTION("an empty selector is rejected with selector_empty")
    {
        const CameraSelector selector;
        const auto resolved = camera::resolve_identity(candidates, selector);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.failure().code == ErrorCode::selector_empty);
    }
}

/* ------------------------------------------------------------------------- */
/* B4: idempotent close and close-less destruction.                          */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-102 B4: close before any open and repeated close are idempotent and balanced",
          "[cvf-102][B4]")
{
    UvcCameraBackend backend;

    backend.close();
    CHECK(worker_is_stopped(backend));
    CHECK(counts_are_balanced(backend));

    backend.close();
    backend.close();
    CHECK(worker_is_stopped(backend));
    CHECK(counts_are_balanced(backend));
}

TEST_CASE("CVF-102 B4: an open followed by destruction without close leaves no observable worker",
          "[cvf-102][B4]")
{
    {
        UvcCameraBackend backend;
        const auto opened = backend.open(absent_descriptor(), default_settings(), Deadline::from_timeout_ms(5000));
        REQUIRE_FALSE(opened.has_value());
        /* Deliberately no close(): the destructor must tear the worker down. */
    }

    /* Observable proxy for "no worker survived": a fresh backend still starts and balances. */
    UvcCameraBackend fresh;
    fresh.close();
    CHECK(worker_is_stopped(fresh));
    CHECK(counts_are_balanced(fresh));
}

/* ------------------------------------------------------------------------- */
/* B5: a failed startup leaves no running worker and balanced lifetime.      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-102 B5: a failed startup leaves no running worker and balances COM/MF lifetime",
          "[cvf-102][B5]")
{
    UvcCameraBackend backend;
    const auto opened = backend.open(absent_descriptor(), default_settings(), Deadline::from_timeout_ms(5000));
    REQUIRE_FALSE(opened.has_value());

    /*
     * With zero enumerable devices the reachable open failure is
     * camera_not_found (identity does not resolve); an activation or
     * configuration startup failure maps to camera_io. Both are documented
     * camera failures, and neither may leak the worker or unbalance teardown.
     */
    const Status status = opened.failure().status;
    CHECK((status == Status::camera_not_found || status == Status::camera_io));

    backend.close();
    CHECK(worker_is_stopped(backend));
    CHECK(counts_are_balanced(backend));
}

/* ------------------------------------------------------------------------- */
/* B6: the supplied deadline is respected; an expired deadline fails fast.   */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-102 B6: an already-expired deadline fails immediately for every operation",
          "[cvf-102][B6]")
{
    UvcCameraBackend backend;
    const auto immediate = Deadline::immediate();
    const auto started = std::chrono::steady_clock::now();

    SECTION("enumerate")
    {
        const auto result = backend.enumerate(immediate);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.failure().status == Status::timeout);
    }

    SECTION("open")
    {
        const auto result = backend.open(absent_descriptor(), default_settings(), immediate);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.failure().status == Status::timeout);
    }

    SECTION("capture")
    {
        const auto result = backend.capture(immediate);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.failure().status == Status::timeout);
        CHECK(result.failure().code == ErrorCode::capture_timed_out);
    }

    SECTION("reconnect")
    {
        const auto result = backend.reconnect(default_settings(), immediate);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.failure().status == Status::timeout);
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(2));

    backend.close();
}

/* ------------------------------------------------------------------------- */
/* Commands are marshalled to the owned worker thread.                       */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-102: every command is marshalled to the owned worker thread", "[cvf-102][marshalling]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);
    const auto before = backend.worker_snapshot();

    const auto listed = backend.enumerate(deadline);
    REQUIRE(listed.has_value());
    const auto after_enumerate = backend.worker_snapshot();
    CHECK(after_enumerate.commands_marshalled > before.commands_marshalled);
    CHECK(after_enumerate.worker_thread_id != after_enumerate.last_caller_thread_id);

    const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);
    REQUIRE_FALSE(opened.has_value());
    const auto after_open = backend.worker_snapshot();
    CHECK(after_open.commands_marshalled > after_enumerate.commands_marshalled);
    CHECK(after_open.worker_thread_id != after_open.last_caller_thread_id);

    const auto captured = backend.capture(deadline);
    REQUIRE_FALSE(captured.has_value());
    const auto after_capture = backend.worker_snapshot();
    CHECK(after_capture.commands_marshalled > after_open.commands_marshalled);
    CHECK(after_capture.worker_thread_id != after_capture.last_caller_thread_id);

    const auto reconnected = backend.reconnect(default_settings(), deadline);
    REQUIRE_FALSE(reconnected.has_value());
    const auto after_reconnect = backend.worker_snapshot();
    CHECK(after_reconnect.commands_marshalled > after_capture.commands_marshalled);
    CHECK(after_reconnect.worker_thread_id != after_reconnect.last_caller_thread_id);

    backend.close();
}

/* ------------------------------------------------------------------------- */
/* Boundary: zero enumerable devices.                                        */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-102 boundary: with zero enumerable devices enumerate succeeds empty and open fails",
          "[cvf-102][boundary]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    const auto listed = backend.enumerate(deadline);
    REQUIRE(listed.has_value());
    CHECK(listed.value().empty());

    const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.failure().code == ErrorCode::camera_not_found);

    backend.close();
}

/* ------------------------------------------------------------------------- */
/* Negative: operations before any open.                                     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-102 negative: capture and reconnect before open fail without crashing", "[cvf-102][negative]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    const auto captured = backend.capture(deadline);
    REQUIRE_FALSE(captured.has_value());
    CHECK((captured.failure().status == Status::camera_not_found ||
           captured.failure().status == Status::camera_io));

    const auto reconnected = backend.reconnect(default_settings(), deadline);
    REQUIRE_FALSE(reconnected.has_value());
    CHECK((reconnected.failure().status == Status::camera_not_found ||
           reconnected.failure().status == Status::camera_io));

    backend.close();
    CHECK(worker_is_stopped(backend));
    CHECK(counts_are_balanced(backend));
}

#endif /* defined(_WIN32) */
