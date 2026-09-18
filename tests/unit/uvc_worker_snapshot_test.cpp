/*
 * Developer-owned tests for the Windows UVC worker/COM lifetime seam.
 *
 * This file is intentionally empty on portable builds: the UVC backend is
 * guarded by _WIN32 and its Media Foundation path cannot be exercised (or
 * faked) on Linux. On Windows it drives the real UvcCameraBackend through the
 * internal worker_snapshot() seam and the ICameraBackend contract from a host
 * thread that never initialized COM. It complements the independent
 * cvf102 Windows component suite (which remains the acceptance evidence) with
 * a developer check that the snapshot counters are balanced and that every
 * command is marshalled onto the owned worker thread. Execution evidence for
 * this file is produced on the Windows/CI station, never here.
 *
 * File name does not start with "cvf1": the independent suite owns that prefix.
 */

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)

#include <chrono>
#include <thread>

#include "camera/camera_backend.h"
#include "camera/uvc_windows/uvc_backend.h"
#include "core/deadline.h"
#include "core/error.h"
#include "core/status.h"

namespace {

using cvforwin::camera::CameraDescriptor;
using cvforwin::camera::CameraSettings;
using cvforwin::camera::PixelFormat;
using cvforwin::camera::UvcCameraBackend;
using cvforwin::core::Deadline;

CameraDescriptor absent_descriptor()
{
    CameraDescriptor descriptor;
    descriptor.backend_key = "uvc";
    descriptor.device_path = R"(\\?\usb#vid_0bad&pid_0bad#developer-worker-probe)";
    descriptor.vendor_id = "0bad";
    descriptor.product_id = "0bad";
    descriptor.friendly_name = "developer worker probe";
    return descriptor;
}

}  // namespace

TEST_CASE("UVC developer: commands are marshalled to the owned worker and close balances lifetime",
          "[uvc][worker-developer]")
{
    UvcCameraBackend backend;

    const auto before = backend.worker_snapshot();
    const std::thread::id caller = std::this_thread::get_id();
    (void)caller;

    /* Enumerate from a host thread that never called CoInitializeEx. */
    const auto listed = backend.enumerate(Deadline::from_timeout_ms(5000));
    REQUIRE(listed.has_value());

    const auto after = backend.worker_snapshot();
    CHECK(after.commands_marshalled > before.commands_marshalled);
    CHECK(after.worker_thread_id != after.last_caller_thread_id);
    CHECK(after.worker_running);

    backend.close();

    const auto closed = backend.worker_snapshot();
    CHECK_FALSE(closed.worker_running);
    CHECK(closed.com_init_count == closed.com_uninit_count);
    CHECK(closed.mf_startup_count == closed.mf_shutdown_count);
}

TEST_CASE("UVC developer: close is idempotent and a failed open still balances lifetime",
          "[uvc][worker-developer]")
{
    UvcCameraBackend backend;

    CameraSettings settings;
    settings.width = 640;
    settings.height = 480;
    settings.frame_rate = 30.0;
    settings.preferred_format = PixelFormat::bgr8;

    const auto opened = backend.open(absent_descriptor(), settings, Deadline::from_timeout_ms(5000));
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.failure().status == cvforwin::core::Status::camera_not_found);

    backend.close();
    backend.close();
    backend.close();

    const auto snapshot = backend.worker_snapshot();
    CHECK_FALSE(snapshot.worker_running);
    CHECK(snapshot.com_init_count == snapshot.com_uninit_count);
    CHECK(snapshot.mf_startup_count == snapshot.mf_shutdown_count);
}

TEST_CASE("UVC developer: an expired deadline fails fast for enumerate and open", "[uvc][worker-developer]")
{
    UvcCameraBackend backend;
    const auto immediate = Deadline::immediate();
    const auto started = std::chrono::steady_clock::now();

    const auto listed = backend.enumerate(immediate);
    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.failure().status == cvforwin::core::Status::timeout);

    CameraSettings settings;
    const auto opened = backend.open(absent_descriptor(), settings, immediate);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.failure().status == cvforwin::core::Status::timeout);

    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    backend.close();
}

#else

TEST_CASE("UVC developer: Windows-only worker seam is not exercised on portable builds", "[uvc][worker-developer]")
{
    /* The Windows UVC backend is absent here by design; its evidence is Windows-only. */
    CHECK(true);
}

#endif /* defined(_WIN32) */
