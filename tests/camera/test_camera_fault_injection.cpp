// CVF-002 independent black-box tests: fault injection semantics (brief B5, B6, B7).
#include <chrono>
#include <cstdint>

#include "camera_test_support.h"

using namespace cvf002;
using namespace std::chrono_literals;

namespace {

cam::CameraDescriptor fault_descriptor() {
  return make_descriptor("synthetic", "/dev/synthetic-fault", "vid-fault", "pid-fault", "Fault Camera");
}

cam::SyntheticCameraConfig fault_config() {
  return cam::SyntheticCameraConfig{.descriptor = fault_descriptor(), .width = 4, .height = 3};
}

}  // namespace

TEST_CASE("CVF-002 B5: a disconnect injection persists across captures until reconnect clears it",
          "[cvf-002][B5][faults]") {
  const auto descriptor = fault_descriptor();
  cam::SyntheticCameraBackend backend{fault_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_failure(2, FaultKind::disconnect);

  auto first = backend.capture(generous_deadline());
  REQUIRE(first.has_value());
  CHECK(first.value().metadata.sequence == 0);

  auto second = backend.capture(generous_deadline());
  REQUIRE_FALSE(second.has_value());
  CHECK(second.failure().status == core::Status::camera_io);
  CHECK(second.failure().code == core::ErrorCode::camera_disconnected);

  auto third = backend.capture(generous_deadline());
  REQUIRE_FALSE(third.has_value());
  CHECK(third.failure().status == core::Status::camera_io);
  CHECK(third.failure().code == core::ErrorCode::camera_disconnected);
  CHECK(backend.capture_call_count == 3);

  auto reconnected = backend.reconnect(default_settings(), generous_deadline());
  REQUIRE(reconnected.has_value());
  CHECK(backend.reconnect_call_count == 1);

  auto fourth = backend.capture(generous_deadline());
  REQUIRE(fourth.has_value());
  CHECK(backend.capture_call_count == 4);
}

TEST_CASE("CVF-002 B5 negative: reconnect on a closed backend fails with camera_not_open",
          "[cvf-002][B5][faults][negative]") {
  cam::SyntheticCameraBackend backend{fault_config()};

  auto first = backend.reconnect(default_settings(), generous_deadline());
  REQUIRE_FALSE(first.has_value());
  CHECK(first.failure().status == core::Status::camera_io);
  CHECK(first.failure().code == core::ErrorCode::camera_not_open);

  auto second = backend.reconnect(default_settings(), generous_deadline());
  REQUIRE_FALSE(second.has_value());
  CHECK(second.failure().status == core::Status::camera_io);
  CHECK(second.failure().code == core::ErrorCode::camera_not_open);
  CHECK(backend.reconnect_call_count == 2);
}

TEST_CASE("CVF-002 B5 boundary: two reconnects after a disconnect both succeed and clear the fault",
          "[cvf-002][B5][faults][boundary]") {
  const auto descriptor = fault_descriptor();
  cam::SyntheticCameraBackend backend{fault_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_failure(1, FaultKind::disconnect);

  auto disconnected = backend.capture(generous_deadline());
  REQUIRE_FALSE(disconnected.has_value());
  CHECK(disconnected.failure().code == core::ErrorCode::camera_disconnected);

  REQUIRE(backend.reconnect(default_settings(), generous_deadline()).has_value());
  REQUIRE(backend.reconnect(default_settings(), generous_deadline()).has_value());
  CHECK(backend.reconnect_call_count == 2);

  auto recovered = backend.capture(generous_deadline());
  REQUIRE(recovered.has_value());
  auto still_recovered = backend.capture(generous_deadline());
  REQUIRE(still_recovered.has_value());
  CHECK(backend.capture_call_count == 3);
}

TEST_CASE("CVF-002 B6: a capture_error injection fails exactly one call and the next call succeeds",
          "[cvf-002][B6][faults]") {
  const auto descriptor = fault_descriptor();
  cam::SyntheticCameraBackend backend{fault_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_failure(1, FaultKind::capture_error);

  auto failed = backend.capture(generous_deadline());
  REQUIRE_FALSE(failed.has_value());
  CHECK(failed.failure().status == core::Status::camera_io);
  CHECK(failed.failure().code == core::ErrorCode::capture_failed);

  auto recovered = backend.capture(generous_deadline());
  REQUIRE(recovered.has_value());
  CHECK(recovered.value().metadata.pixel_format == cam::PixelFormat::bgr8);
  CHECK(recovered.value().pixels.at<cv::Vec3b>(0, 0) ==
        synthetic_pixel(recovered.value().metadata.sequence, 0, 0));
  CHECK(backend.capture_call_count == 2);
}

TEST_CASE("CVF-002 B6: clear_faults removes a pending capture fault", "[cvf-002][B6][faults]") {
  const auto descriptor = fault_descriptor();
  cam::SyntheticCameraBackend backend{fault_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_failure(1, FaultKind::capture_error);
  backend.clear_faults();

  auto captured = backend.capture(generous_deadline());
  REQUIRE(captured.has_value());
  CHECK(captured.value().metadata.sequence == 0);
  CHECK(backend.capture_call_count == 1);
}

TEST_CASE("CVF-002 B7: a delay shorter than the remaining deadline returns the frame",
          "[cvf-002][B7][faults]") {
  const auto descriptor = fault_descriptor();
  cam::SyntheticCameraBackend backend{fault_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_delay(1, 20ms);

  const auto before = std::chrono::steady_clock::now();
  auto captured = backend.capture(generous_deadline());
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - before);

  REQUIRE(captured.has_value());
  CHECK(elapsed >= 10ms);
  CHECK(elapsed < 500ms);
}

TEST_CASE("CVF-002 B7: a delay longer than the remaining deadline times out at the deadline",
          "[cvf-002][B7][faults]") {
  const auto descriptor = fault_descriptor();
  cam::SyntheticCameraBackend backend{fault_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_delay(1, 200ms);
  const auto deadline = core::Deadline::from_timeout_ms(60);

  const auto before = std::chrono::steady_clock::now();
  auto captured = backend.capture(deadline);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - before);

  REQUIRE_FALSE(captured.has_value());
  CHECK(captured.failure().status == core::Status::timeout);
  CHECK(captured.failure().code == core::ErrorCode::capture_timed_out);
  CHECK(elapsed >= 30ms);   // the remaining deadline is honored...
  CHECK(elapsed < 180ms);   // ...and the full injected delay (200 ms) is not slept
}

TEST_CASE("CVF-002 B7: an already-expired deadline times out immediately",
          "[cvf-002][B7][faults]") {
  const auto descriptor = fault_descriptor();
  cam::SyntheticCameraBackend backend{fault_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_delay(1, 200ms);

  const auto before = std::chrono::steady_clock::now();
  auto captured = backend.capture(core::Deadline::immediate());
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - before);

  REQUIRE_FALSE(captured.has_value());
  CHECK(captured.failure().status == core::Status::timeout);
  CHECK(captured.failure().code == core::ErrorCode::capture_timed_out);
  CHECK(elapsed < 100ms);
}

TEST_CASE("CVF-002 B7 boundary: a delay equal to the nominal remaining stays bounded",
          "[cvf-002][B7][faults][boundary]") {
  const auto descriptor = fault_descriptor();
  cam::SyntheticCameraBackend backend{fault_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  // Brief rule: delay <= remaining returns a frame, delay > remaining times out. At exact
  // nominal equality the wall clock has already consumed part of the timeout before the
  // comparison happens, so either documented outcome is acceptable; this case pins the
  // bounded behavior instead of guessing which side of the comparison the clock lands on.
  backend.inject_capture_delay(1, 50ms);
  const auto deadline = core::Deadline::from_timeout_ms(50);

  const auto before = std::chrono::steady_clock::now();
  auto captured = backend.capture(deadline);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - before);

  const bool timed_out =
      !captured.has_value() && captured.failure().code == core::ErrorCode::capture_timed_out;
  CHECK((captured.has_value() || timed_out));
  if (!captured.has_value()) {
    CHECK(captured.failure().status == core::Status::timeout);
  }
  CHECK(elapsed < 250ms);
}

TEST_CASE("CVF-002 B5/B6 boundary: faults injected past the reached call count have no effect",
          "[cvf-002][B5][B6][faults][boundary]") {
  const auto descriptor = fault_descriptor();
  cam::SyntheticCameraBackend backend{fault_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_failure(9, FaultKind::capture_error);
  backend.inject_capture_delay(9, 200ms);

  const auto before = std::chrono::steady_clock::now();
  for (int index = 0; index < 3; ++index) {
    auto captured = backend.capture(generous_deadline());
    INFO("capture " << index);
    REQUIRE(captured.has_value());
    CHECK(captured.value().metadata.sequence == static_cast<std::uint64_t>(index));
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - before);

  CHECK(backend.capture_call_count == 3);
  CHECK(elapsed < 300ms);
}
