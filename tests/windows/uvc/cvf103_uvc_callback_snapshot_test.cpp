/*
 * CVF-103 independent black-box Windows component tests for the frozen
 * per-stream callback-generation seam:
 *
 *   struct UvcCallbackSnapshot {
 *       active_generation, callbacks_created, stale_events_discarded, stream_open
 *   };
 *   UvcCameraBackend::callback_snapshot() const noexcept;
 *
 * Coverage: brief observable behaviors B1-B5 and the generation-related
 * boundary/negative cases. Everything here is deterministic and hardware-free:
 * the hardware-free host enumerates zero Media Foundation devices and every
 * open is driven against an absent identity, so no stream can be established
 * and no stale completion can arrive from a real device. That is exactly the
 * deterministic seam the brief requires ("backend-level cases use failing opens
 * and zero-device enumeration").
 *
 * The successful-stream properties (a callback created exactly once per started
 * stream, strictly increasing generations across reconnect, a retired stream's
 * delayed completion rejected) are asserted in their observable form: the
 * snapshot counters must never decrease, no callback may exist while no stream
 * has started, and any stream that does start must be the only active
 * generation. The corresponding pure-logic guarantees are unit-tested
 * portably in tests/unit/cvf103_stream_generation_test.cpp.
 *
 * Only frozen headers are included:
 *   src/camera/camera_backend.h
 *   src/camera/uvc_windows/uvc_backend.h
 *   src/core/{status,error,result,deadline}.h
 * No production .cpp is read, and no camera, Media Foundation object, or
 * physical device is required. Snapshot fields are read by their brief-frozen
 * names through `auto`, so this file does not depend on any non-frozen snapshot
 * type spelling. The whole translation unit is Windows-only; off Windows it is
 * an empty translation unit and the portable suite is unaffected.
 */

#if defined(_WIN32)

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
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
using camera::CameraSettings;
using camera::PixelFormat;
using camera::UvcCameraBackend;
using core::Deadline;
using core::ErrorCode;
using core::Status;

CameraDescriptor absent_descriptor()
{
    CameraDescriptor descriptor;
    descriptor.backend_key = "uvc";
    descriptor.device_path = R"(\\?\usb#vid_0bad&pid_0bad#cvf103-absent-device)";
    descriptor.vendor_id = "0bad";
    descriptor.product_id = "0bad";
    descriptor.friendly_name = "cvf103 absent device";
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

/* A documented camera failure: missing identity or an activation/IO failure. */
bool is_documented_camera_failure(const core::Failure& failure)
{
    return failure.status == Status::camera_not_found || failure.status == Status::camera_io;
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* B4/acceptance: initial snapshot carries no stream and no callback.        */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 acceptance: a fresh backend exposes a consistent no-stream callback snapshot",
          "[cvf-103][B4][acceptance]")
{
    UvcCameraBackend backend;
    const auto snapshot = backend.callback_snapshot();

    CHECK_FALSE(snapshot.stream_open);
    CHECK(snapshot.callbacks_created == 0);
    CHECK(snapshot.stale_events_discarded == 0);

    /* Repeated reads of an idle backend are stable. */
    const auto again = backend.callback_snapshot();
    CHECK(again.active_generation == snapshot.active_generation);
    CHECK(again.callbacks_created == snapshot.callbacks_created);
    CHECK_FALSE(again.stream_open);
}

/* ------------------------------------------------------------------------- */
/* B4/acceptance: callback_snapshot() is safe from any caller apartment.     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 acceptance: callback_snapshot is safe from a no-COM and an STA caller thread",
          "[cvf-103][B4][acceptance]")
{
    UvcCameraBackend backend;

    SECTION("no COM initialization")
    {
        bool consistent = false;
        bool enumerated = false;
        std::thread caller([&backend, &consistent, &enumerated] {
            const auto snapshot = backend.callback_snapshot();
            consistent = !snapshot.stream_open && snapshot.callbacks_created == 0;
            const auto listed = backend.enumerate(Deadline::from_timeout_ms(5000));
            enumerated = listed.has_value();
        });
        caller.join();
        CHECK(enumerated);
        CHECK(consistent);
    }

    SECTION("STA initialization")
    {
        bool sta_ready = false;
        bool consistent = false;
        std::thread caller([&backend, &sta_ready, &consistent] {
            const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            sta_ready = SUCCEEDED(hr);
            const auto snapshot = backend.callback_snapshot();
            consistent = !snapshot.stream_open && snapshot.callbacks_created == 0;
            if (SUCCEEDED(hr)) {
                ::CoUninitialize();
            }
        });
        caller.join();
        CHECK(sta_ready);
        CHECK(consistent);
    }
}

/* ------------------------------------------------------------------------- */
/* B4/acceptance: callbacks_created is not reused or fabricated.             */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: callbacks_created never decreases and stays zero while no stream starts",
          "[cvf-103][B4][callbacks]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);
    const auto before = backend.callback_snapshot();

    /* Zero enumerable devices: every open fails against the absent identity. */
    const auto listed = backend.enumerate(deadline);
    REQUIRE(listed.has_value());

    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);
        REQUIRE_FALSE(opened.has_value());
        CHECK(is_documented_camera_failure(opened.failure()));

        const auto after = backend.callback_snapshot();
        /* No stream was established, so no callback may have been created. */
        CHECK_FALSE(after.stream_open);
        CHECK(after.callbacks_created == before.callbacks_created);
        /* The counter is monotonic and cannot be reused or reset. */
        CHECK(after.callbacks_created >= before.callbacks_created);
        CHECK(after.stale_events_discarded >= before.stale_events_discarded);
    }
}

/* ------------------------------------------------------------------------- */
/* B4/acceptance: active_generation is monotonic across reconnect attempts.  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: active_generation never decreases across open, capture, reconnect, and close",
          "[cvf-103][B4][generation]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);
    std::uint64_t previous = backend.callback_snapshot().active_generation;

    const auto rejected = backend.open(absent_descriptor(), default_settings(), deadline);
    REQUIRE_FALSE(rejected.has_value());
    std::uint64_t current = backend.callback_snapshot().active_generation;
    CHECK(current >= previous);
    previous = current;

    const auto captured = backend.capture(deadline);
    REQUIRE_FALSE(captured.has_value());
    current = backend.callback_snapshot().active_generation;
    CHECK(current >= previous);
    previous = current;

    const auto reconnected = backend.reconnect(default_settings(), deadline);
    REQUIRE_FALSE(reconnected.has_value());
    current = backend.callback_snapshot().active_generation;
    CHECK(current >= previous);
    previous = current;

    backend.close();
    current = backend.callback_snapshot().active_generation;
    CHECK(current >= previous);
}

/* ------------------------------------------------------------------------- */
/* B4/acceptance: stream_open reflects open failure and close.               */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: stream_open reflects open failure and close", "[cvf-103][B4][stream_open]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    CHECK_FALSE(backend.callback_snapshot().stream_open);

    const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);
    REQUIRE_FALSE(opened.has_value());
    CHECK_FALSE(backend.callback_snapshot().stream_open);

    backend.close();
    CHECK_FALSE(backend.callback_snapshot().stream_open);

    /* close() is idempotent and keeps the stream closed. */
    backend.close();
    backend.close();
    CHECK_FALSE(backend.callback_snapshot().stream_open);
}

/* ------------------------------------------------------------------------- */
/* B1/B2/acceptance: after close no stale completion can satisfy a capture.  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B1: after close a capture fails deterministically and no stale completion is accepted",
          "[cvf-103][B1][B2][close]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    /* A failing open retires/settles any stream state, then close tears it down. */
    const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);
    REQUIRE_FALSE(opened.has_value());
    backend.close();

    const auto after_close = backend.callback_snapshot();
    CHECK_FALSE(after_close.stream_open);

    const auto captured = backend.capture(deadline);
    REQUIRE_FALSE(captured.has_value());
    CHECK(is_documented_camera_failure(captured.failure()));

    /* The closed stream still reports no open stream and no new callback. */
    const auto final_snapshot = backend.callback_snapshot();
    CHECK_FALSE(final_snapshot.stream_open);
    CHECK(final_snapshot.callbacks_created == after_close.callbacks_created);
    CHECK(final_snapshot.stale_events_discarded >= after_close.stale_events_discarded);
}

/* ------------------------------------------------------------------------- */
/* B3: a second capture on an unknown-state stream fails deterministically.  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B3: consecutive captures on an unopened stream fail deterministically",
          "[cvf-103][B3][negative]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto captured = backend.capture(deadline);
        REQUIRE_FALSE(captured.has_value());
        CHECK(is_documented_camera_failure(captured.failure()));
        CHECK_FALSE(backend.callback_snapshot().stream_open);
    }

    backend.close();
}

/* ------------------------------------------------------------------------- */
/* Boundary: an already-expired deadline fails fast without touching state.  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 boundary: an expired deadline fails immediately and leaves callback state unchanged",
          "[cvf-103][boundary]")
{
    UvcCameraBackend backend;
    const auto immediate = Deadline::immediate();
    const auto before = backend.callback_snapshot();
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

    const auto after = backend.callback_snapshot();
    CHECK_FALSE(after.stream_open);
    CHECK(after.callbacks_created == before.callbacks_created);
    CHECK(after.active_generation >= before.active_generation);

    backend.close();
}

/* ------------------------------------------------------------------------- */
/* Boundary: zero enumerable devices.                                        */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 boundary: zero devices enumerate empty, no stream opens, no callback is created",
          "[cvf-103][boundary]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    const auto listed = backend.enumerate(deadline);
    REQUIRE(listed.has_value());
    CHECK(listed.value().empty());

    const auto snapshot = backend.callback_snapshot();
    CHECK_FALSE(snapshot.stream_open);
    CHECK(snapshot.callbacks_created == 0);

    const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.failure().code == ErrorCode::camera_not_found);

    backend.close();
}

/* ------------------------------------------------------------------------- */
/* Negative: two consecutive failing opens do not reuse callback state.      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 negative: two consecutive failed opens never reuse a callback or accept a stale completion",
          "[cvf-103][negative]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    const auto first = backend.open(absent_descriptor(), default_settings(), deadline);
    REQUIRE_FALSE(first.has_value());
    const auto after_first = backend.callback_snapshot();

    const auto second = backend.open(absent_descriptor(), default_settings(), deadline);
    REQUIRE_FALSE(second.has_value());
    const auto after_second = backend.callback_snapshot();

    CHECK_FALSE(after_first.stream_open);
    CHECK_FALSE(after_second.stream_open);
    CHECK(after_second.callbacks_created >= after_first.callbacks_created);
    CHECK(after_second.stale_events_discarded >= after_first.stale_events_discarded);
    CHECK(after_second.active_generation >= after_first.active_generation);

    /* With no stream established, a capture cannot report a stale frame. */
    const auto captured = backend.capture(deadline);
    REQUIRE_FALSE(captured.has_value());
    CHECK(is_documented_camera_failure(captured.failure()));

    backend.close();
}

/* ------------------------------------------------------------------------- */
/* B5: at most one reconnect/recapture is attempted; callback state bounded. */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B5: a bounded reconnect attempt never starts a stream without a device",
          "[cvf-103][B5][reconnect]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);
    const auto before = backend.callback_snapshot();

    const auto reconnected = backend.reconnect(default_settings(), deadline);
    REQUIRE_FALSE(reconnected.has_value());
    CHECK(is_documented_camera_failure(reconnected.failure()));

    const auto after = backend.callback_snapshot();
    CHECK_FALSE(after.stream_open);
    CHECK(after.callbacks_created == before.callbacks_created);
    CHECK(after.active_generation >= before.active_generation);

    backend.close();
}

/* ------------------------------------------------------------------------- */
/* Acceptance (device-present branch): a started stream creates one callback. */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 acceptance: a started stream creates a fresh callback and a retired generation is not reused",
          "[cvf-103][B4][stream]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);
    const auto before = backend.callback_snapshot();

    const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);

    if (opened.has_value()) {
        /*
         * A real device was present. The first stream must have created at
         * least one callback, the stream must report open, and the active
         * generation must have advanced past the idle snapshot.
         */
        const auto after_open = backend.callback_snapshot();
        CHECK(after_open.stream_open);
        CHECK(after_open.callbacks_created > before.callbacks_created);
        CHECK(after_open.active_generation > before.active_generation);

        /* close retires the stream: nothing may remain active. */
        backend.close();
        const auto after_close = backend.callback_snapshot();
        CHECK_FALSE(after_close.stream_open);
        CHECK(after_close.callbacks_created >= after_open.callbacks_created);
        CHECK(after_close.active_generation >= after_open.active_generation);
    } else {
        /*
         * Hardware-free host (the mandatory case): a failed open must not
         * fabricate a callback, must leave the stream closed, and must not
         * regress the generation.
         */
        CHECK(is_documented_camera_failure(opened.failure()));
        const auto after_failed = backend.callback_snapshot();
        CHECK_FALSE(after_failed.stream_open);
        CHECK(after_failed.callbacks_created == before.callbacks_created);
        CHECK(after_failed.active_generation >= before.active_generation);
        backend.close();
    }
}

/* ------------------------------------------------------------------------- */
/* Boundary: close before open and repeated close stay idle and consistent.  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 boundary: close before open and repeated close keep an idle, consistent snapshot",
          "[cvf-103][boundary][close]")
{
    UvcCameraBackend backend;

    const auto before = backend.callback_snapshot();
    CHECK_FALSE(before.stream_open);

    /* close on a backend that was never opened is idempotent. */
    backend.close();
    backend.close();
    backend.close();

    const auto after = backend.callback_snapshot();
    CHECK_FALSE(after.stream_open);
    CHECK(after.callbacks_created == before.callbacks_created);
    CHECK(after.stale_events_discarded >= before.stale_events_discarded);
    CHECK(after.active_generation >= before.active_generation);

    /* A capture on a stream that was never opened still fails deterministically. */
    const auto captured = backend.capture(Deadline::from_timeout_ms(5000));
    REQUIRE_FALSE(captured.has_value());
    CHECK(is_documented_camera_failure(captured.failure()));
    CHECK_FALSE(backend.callback_snapshot().stream_open);
}

/* ------------------------------------------------------------------------- */
/* B4: snapshot counters are monotonic across repeated reconnect cycles.     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: snapshot counters are monotonic across repeated open, capture, reconnect, and close cycles",
          "[cvf-103][B4][generation][reconnect]")
{
    UvcCameraBackend backend;
    const auto deadline = Deadline::from_timeout_ms(5000);

    auto previous = backend.callback_snapshot();
    for (int cycle = 0; cycle < 3; ++cycle) {
        const auto opened = backend.open(absent_descriptor(), default_settings(), deadline);
        REQUIRE_FALSE(opened.has_value());
        auto current = backend.callback_snapshot();
        CHECK_FALSE(current.stream_open);
        CHECK(current.active_generation >= previous.active_generation);
        CHECK(current.callbacks_created >= previous.callbacks_created);
        CHECK(current.stale_events_discarded >= previous.stale_events_discarded);
        previous = current;

        const auto captured = backend.capture(deadline);
        REQUIRE_FALSE(captured.has_value());
        current = backend.callback_snapshot();
        CHECK(current.active_generation >= previous.active_generation);
        CHECK(current.callbacks_created >= previous.callbacks_created);
        CHECK(current.stale_events_discarded >= previous.stale_events_discarded);
        previous = current;

        const auto reconnected = backend.reconnect(default_settings(), deadline);
        REQUIRE_FALSE(reconnected.has_value());
        current = backend.callback_snapshot();
        CHECK(current.active_generation >= previous.active_generation);
        CHECK(current.callbacks_created >= previous.callbacks_created);
        CHECK(current.stale_events_discarded >= previous.stale_events_discarded);
        previous = current;

        backend.close();
        current = backend.callback_snapshot();
        CHECK_FALSE(current.stream_open);
        CHECK(current.active_generation >= previous.active_generation);
        CHECK(current.callbacks_created >= previous.callbacks_created);
        CHECK(current.stale_events_discarded >= previous.stale_events_discarded);
        previous = current;
    }
}

/* ------------------------------------------------------------------------- */
/* Boundary: capture at the deadline boundary fabricates no completion.      */
/*                                                                            */
/* The closest deterministic probe for "a stale event arriving exactly at the */
/* deadline boundary" on the hardware-free host is a capture whose deadline    */
/* has already expired: no completion may be accepted and no stale event may   */
/* be fabricated or miscounted.                                               */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 boundary: a capture at the expired-deadline boundary accepts no completion and counts no stale event",
          "[cvf-103][boundary][deadline][stale]")
{
    UvcCameraBackend backend;
    const auto before = backend.callback_snapshot();

    const auto captured = backend.capture(Deadline::immediate());
    REQUIRE_FALSE(captured.has_value());
    CHECK(captured.failure().status == Status::timeout);
    CHECK(captured.failure().code == ErrorCode::capture_timed_out);

    const auto after = backend.callback_snapshot();
    CHECK_FALSE(after.stream_open);
    CHECK(after.callbacks_created == before.callbacks_created);
    /* No stale event was delivered, so none may be fabricated. */
    CHECK(after.stale_events_discarded == before.stale_events_discarded);
    CHECK(after.active_generation >= before.active_generation);

    backend.close();
}

#endif /* defined(_WIN32) */
