/*
 * Developer-owned tests for the Windows UVC per-stream callback-generation
 * seam (UvcCameraBackend::callback_snapshot()).
 *
 * This file is intentionally empty on portable builds: the UVC backend is
 * guarded by _WIN32 and its Media Foundation path cannot be exercised (or
 * faked) on Linux. On Windows it drives the real backend through the internal
 * callback_snapshot() seam from a host thread that never initialized COM and
 * asserts that generation/callback counters are monotonic and that a failed
 * open fabricates nothing. It complements the independent cvf103 Windows
 * component suite (which remains the acceptance evidence). Execution evidence
 * for this file is produced on the Windows/CI station, never here.
 *
 * File name does not start with "cvf1": the independent suite owns that prefix.
 */

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)

#include <chrono>
#include <cstdint>

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
    descriptor.device_path = R"(\\?\usb#vid_0bad&pid_0bad#developer-generation-probe)";
    descriptor.vendor_id = "0bad";
    descriptor.product_id = "0bad";
    descriptor.friendly_name = "developer generation probe";
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

}  // namespace

TEST_CASE("UVC developer: a fresh backend reports no stream and no callback", "[uvc][generation-developer]")
{
    UvcCameraBackend backend;
    const auto snapshot = backend.callback_snapshot();

    CHECK_FALSE(snapshot.stream_open);
    CHECK(snapshot.callbacks_created == 0);
    CHECK(snapshot.stale_events_discarded == 0);
    CHECK(snapshot.active_generation == 0);
}

TEST_CASE("UVC developer: a failed open never fabricates a callback or leaves the stream open",
          "[uvc][generation-developer]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);
    const auto before = backend.callback_snapshot();

    const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);
    REQUIRE_FALSE(opened.has_value());
    CHECK((opened.failure().status == cvforwin::core::Status::camera_not_found ||
           opened.failure().status == cvforwin::core::Status::camera_io));

    const auto after = backend.callback_snapshot();
    CHECK_FALSE(after.stream_open);
    CHECK(after.callbacks_created == before.callbacks_created);
    CHECK(after.stale_events_discarded >= before.stale_events_discarded);
    CHECK(after.active_generation >= before.active_generation);

    backend.close();
    CHECK_FALSE(backend.callback_snapshot().stream_open);
}

TEST_CASE("UVC developer: callback counters never decrease across close and an unopened capture",
          "[uvc][generation-developer]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    const auto before = backend.callback_snapshot();
    const auto captured = backend.capture(deadline);
    REQUIRE_FALSE(captured.has_value());
    const auto after = backend.callback_snapshot();
    CHECK(after.callbacks_created >= before.callbacks_created);
    CHECK(after.stale_events_discarded >= before.stale_events_discarded);
    CHECK(after.active_generation >= before.active_generation);

    backend.close();
    backend.close();
    const auto closed = backend.callback_snapshot();
    CHECK_FALSE(closed.stream_open);
    CHECK(closed.callbacks_created >= after.callbacks_created);
}

#else

TEST_CASE("UVC developer: Windows-only callback-generation seam is not exercised on portable builds",
          "[uvc][generation-developer]")
{
    /* The Windows UVC backend is absent here by design; its evidence is Windows-only. */
    CHECK(true);
}

#endif /* defined(_WIN32) */
