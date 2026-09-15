// CVF-002 independent black-box tests: bounded capture_with_one_retry (brief B8, B9, B10).
#include <chrono>
#include <cstdint>

#include "camera_test_support.h"

using namespace cvf002;
using namespace std::chrono_literals;

namespace {

cam::CameraDescriptor retry_descriptor() {
  return make_descriptor("synthetic", "/dev/synthetic-retry", "vid-retry", "pid-retry", "Retry Camera");
}

cam::SyntheticCameraConfig retry_config() {
  return cam::SyntheticCameraConfig{.descriptor = retry_descriptor(), .width = 4, .height = 3};
}

}  // namespace

TEST_CASE("CVF-002 B8: the success path uses one capture, no reconnect, and no enumerate",
          "[cvf-002][B8][retry]") {
  const auto descriptor = retry_descriptor();
  cam::SyntheticCameraBackend backend{retry_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  auto result = cam::capture_with_one_retry(backend, default_settings(), generous_deadline());

  REQUIRE(result.has_value());
  CHECK(result.value().metadata.sequence == 0);
  CHECK(result.value().metadata.pixel_format == cam::PixelFormat::bgr8);
  CHECK(backend.capture_call_count == 1);
  CHECK(backend.reconnect_call_count == 0);
  CHECK(backend.enumerate_call_count == 0);
}

TEST_CASE("CVF-002 B9: a capture_error failure triggers one reconnect and one successful recapture",
          "[cvf-002][B9][retry]") {
  const auto descriptor = retry_descriptor();
  cam::SyntheticCameraBackend backend{retry_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_failure(1, FaultKind::capture_error);

  auto result = cam::capture_with_one_retry(backend, default_settings(), generous_deadline());

  REQUIRE(result.has_value());
  CHECK(backend.capture_call_count == 2);
  CHECK(backend.reconnect_call_count == 1);
  CHECK(backend.enumerate_call_count == 0);
}

TEST_CASE("CVF-002 B9: a persistent disconnect is cleared by the single reconnect",
          "[cvf-002][B9][retry]") {
  const auto descriptor = retry_descriptor();
  cam::SyntheticCameraBackend backend{retry_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_failure(1, FaultKind::disconnect);

  auto result = cam::capture_with_one_retry(backend, default_settings(), generous_deadline());

  REQUIRE(result.has_value());
  CHECK(backend.capture_call_count == 2);
  CHECK(backend.reconnect_call_count == 1);
  CHECK(backend.enumerate_call_count == 0);
}

TEST_CASE("CVF-002 B9: a second camera_io failure is returned as-is with no third attempt",
          "[cvf-002][B9][retry]") {
  const auto descriptor = retry_descriptor();
  cam::SyntheticCameraBackend backend{retry_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_failure(1, FaultKind::capture_error);
  backend.inject_capture_failure(2, FaultKind::capture_error);

  auto result = cam::capture_with_one_retry(backend, default_settings(), generous_deadline());

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::camera_io);
  CHECK(result.failure().code == core::ErrorCode::capture_failed);
  CHECK(backend.capture_call_count == 2);
  CHECK(backend.reconnect_call_count == 1);
  CHECK(backend.enumerate_call_count == 0);
}

TEST_CASE("CVF-002 B9 boundary: a failing reconnect is returned with no recapture",
          "[cvf-002][B9][retry][boundary]") {
  cam::SyntheticCameraBackend backend{retry_config()};  // never opened

  auto result = cam::capture_with_one_retry(backend, default_settings(), generous_deadline());

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::camera_io);
  CHECK(result.failure().code == core::ErrorCode::camera_not_open);
  CHECK(backend.capture_call_count == 1);
  CHECK(backend.reconnect_call_count == 1);
  CHECK(backend.enumerate_call_count == 0);
}

TEST_CASE("CVF-002 B10: a non-camera_io timeout is returned unchanged with zero reconnects",
          "[cvf-002][B10][retry]") {
  const auto descriptor = retry_descriptor();
  cam::SyntheticCameraBackend backend{retry_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_delay(1, 200ms);

  auto result =
      cam::capture_with_one_retry(backend, default_settings(), core::Deadline::from_timeout_ms(60));

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::timeout);
  CHECK(result.failure().code == core::ErrorCode::capture_timed_out);
  CHECK(backend.capture_call_count == 1);
  CHECK(backend.reconnect_call_count == 0);
  CHECK(backend.enumerate_call_count == 0);
}

TEST_CASE("CVF-002 B10: an expired deadline produces deadline_expired without capture or reconnect",
          "[cvf-002][B10][retry]") {
  const auto descriptor = retry_descriptor();
  cam::SyntheticCameraBackend backend{retry_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  auto result =
      cam::capture_with_one_retry(backend, default_settings(), core::Deadline::from_timeout_ms(0));

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::timeout);
  CHECK(result.failure().code == core::ErrorCode::deadline_expired);
  CHECK(backend.capture_call_count == 0);
  CHECK(backend.reconnect_call_count == 0);
  CHECK(backend.enumerate_call_count == 0);
}

TEST_CASE("CVF-002 B10 boundary: a deadline that expires before recapture yields a timeout failure",
          "[cvf-002][B10][retry][boundary]") {
  const auto descriptor = retry_descriptor();
  cam::SyntheticCameraBackend backend{retry_config()};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  backend.inject_capture_failure(1, FaultKind::capture_error);  // first capture fails with camera_io
  backend.inject_capture_delay(2, 200ms);                       // recapture would overrun the deadline

  const auto before = std::chrono::steady_clock::now();
  auto result =
      cam::capture_with_one_retry(backend, default_settings(), core::Deadline::from_timeout_ms(50));
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - before);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::timeout);
  CHECK(backend.capture_call_count >= 1);
  CHECK(backend.capture_call_count <= 2);
  CHECK(backend.reconnect_call_count == 1);
  CHECK(backend.enumerate_call_count == 0);
  CHECK(elapsed < 250ms);
}
