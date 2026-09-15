// CVF-002 independent black-box tests: FileCameraBackend behavior (brief B2, B3).
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "camera_frame_fixtures.h"
#include "camera_test_support.h"

using namespace cvf002;
using namespace std::chrono_literals;

namespace {

std::vector<std::array<std::uint8_t, 3>> solid_rgb(int width, int height,
                                                   std::array<std::uint8_t, 3> color) {
  return std::vector<std::array<std::uint8_t, 3>>(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height), color);
}

cam::CameraDescriptor file_descriptor(const std::string& device_path) {
  return make_descriptor("file", device_path, "vid-file", "pid-file", "File Camera");
}

}  // namespace

TEST_CASE("CVF-002 B2: enumerate returns the configured descriptor and counts the call",
          "[cvf-002][B2][file]") {
  TempFrameDir dir("file_enumerate");
  const auto frame = dir.write_ppm_p6("frame0.ppm", 2, 2, solid_rgb(2, 2, {10, 20, 30}));
  const auto descriptor = file_descriptor("/dev/file-enum");

  cam::FileCameraConfig config{.descriptor = descriptor, .frame_paths = {frame}};
  cam::FileCameraBackend backend{config};

  CHECK(backend.enumerate_call_count == 0);
  CHECK(backend.open_call_count == 0);
  CHECK(backend.capture_call_count == 0);
  CHECK(backend.reconnect_call_count == 0);
  CHECK(backend.close_call_count == 0);

  auto result = backend.enumerate(generous_deadline());

  REQUIRE(result.has_value());
  REQUIRE(result.value().size() == 1);
  check_descriptor(result.value().front(), descriptor);
  CHECK(backend.enumerate_call_count == 1);
  CHECK(backend.open_call_count == 0);
  CHECK(backend.capture_call_count == 0);
}

TEST_CASE("CVF-002 B2 negative: open with a different descriptor fails with descriptor_mismatch",
          "[cvf-002][B2][file][negative]") {
  TempFrameDir dir("file_mismatch");
  const auto frame = dir.write_ppm_p6("frame0.ppm", 2, 2, solid_rgb(2, 2, {1, 2, 3}));
  const auto configured = file_descriptor("/dev/file-configured");

  cam::FileCameraConfig config{.descriptor = configured, .frame_paths = {frame}};
  cam::FileCameraBackend backend{config};

  const auto other = make_descriptor("file", "/dev/file-other", configured.vendor_id,
                                     configured.product_id, configured.friendly_name);
  auto opened = backend.open(other, default_settings(), generous_deadline());

  REQUIRE_FALSE(opened.has_value());
  CHECK(opened.failure().status == core::Status::camera_not_found);
  CHECK(opened.failure().code == core::ErrorCode::descriptor_mismatch);
  CHECK(backend.open_call_count == 1);

  auto captured = backend.capture(generous_deadline());
  REQUIRE_FALSE(captured.has_value());
  CHECK(captured.failure().status == core::Status::camera_io);
  CHECK(captured.failure().code == core::ErrorCode::camera_not_open);
}

TEST_CASE("CVF-002 B2 negative: capture and reconnect before open fail with camera_not_open",
          "[cvf-002][B2][file][negative]") {
  TempFrameDir dir("file_not_open");
  const auto frame = dir.write_ppm_p6("frame0.ppm", 2, 2, solid_rgb(2, 2, {4, 5, 6}));
  const auto descriptor = file_descriptor("/dev/file-not-open");

  cam::FileCameraConfig config{.descriptor = descriptor, .frame_paths = {frame}};
  cam::FileCameraBackend backend{config};

  auto captured = backend.capture(generous_deadline());
  REQUIRE_FALSE(captured.has_value());
  CHECK(captured.failure().status == core::Status::camera_io);
  CHECK(captured.failure().code == core::ErrorCode::camera_not_open);

  auto reconnected = backend.reconnect(default_settings(), generous_deadline());
  REQUIRE_FALSE(reconnected.has_value());
  CHECK(reconnected.failure().status == core::Status::camera_io);
  CHECK(reconnected.failure().code == core::ErrorCode::camera_not_open);

  CHECK(backend.capture_call_count == 1);
  CHECK(backend.reconnect_call_count == 1);
  CHECK(backend.open_call_count == 0);
}

TEST_CASE("CVF-002 B2: close is idempotent and counters reflect the exact call order",
          "[cvf-002][B2][file]") {
  TempFrameDir dir("file_order");
  const auto frame0 = dir.write_ppm_p6("frame0.ppm", 2, 2, solid_rgb(2, 2, {1, 1, 1}));
  const auto frame1 = dir.write_ppm_p6("frame1.ppm", 2, 2, solid_rgb(2, 2, {2, 2, 2}));
  const auto descriptor = file_descriptor("/dev/file-order");

  cam::FileCameraConfig config{.descriptor = descriptor, .frame_paths = {frame0, frame1}};
  cam::FileCameraBackend backend{config};

  auto opened = backend.open(descriptor, default_settings(), generous_deadline());
  REQUIRE(opened.has_value());

  auto first = backend.capture(generous_deadline());
  REQUIRE(first.has_value());
  CHECK(first.value().metadata.sequence == 0);

  auto second = backend.capture(generous_deadline());
  REQUIRE(second.has_value());
  CHECK(second.value().metadata.sequence == 1);

  auto reconnected = backend.reconnect(default_settings(), generous_deadline());
  REQUIRE(reconnected.has_value());

  (void)backend.close();
  (void)backend.close();

  auto after_close = backend.capture(generous_deadline());
  REQUIRE_FALSE(after_close.has_value());
  CHECK(after_close.failure().status == core::Status::camera_io);
  CHECK(after_close.failure().code == core::ErrorCode::camera_not_open);

  CHECK(backend.enumerate_call_count == 0);
  CHECK(backend.open_call_count == 1);
  CHECK(backend.capture_call_count == 3);
  CHECK(backend.reconnect_call_count == 1);
  CHECK(backend.close_call_count == 2);
}

TEST_CASE("CVF-002 B3: capture returns frames in order with owned BGR8 pixels and metadata",
          "[cvf-002][B3][file]") {
  TempFrameDir dir("file_frames");
  const auto frame0 = dir.write_ppm_p6(
      "frame0.ppm", 2, 2,
      {
          {255, 0, 0},  // (x=0, y=0) red
          {0, 255, 0},  // (x=1, y=0) green
          {0, 0, 255},  // (x=0, y=1) blue
          {255, 255, 255},
      });
  const auto frame1 = dir.write_ppm_p6("frame1.ppm", 3, 1,
                                       {
                                           {10, 20, 30},
                                           {40, 50, 60},
                                           {70, 80, 90},
                                       });
  const auto descriptor = file_descriptor("/dev/file-frames");

  cam::FileCameraConfig config{.descriptor = descriptor, .frame_paths = {frame0, frame1}};
  cam::FileCameraBackend backend{config};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  const auto before = std::chrono::steady_clock::now();
  auto first = backend.capture(generous_deadline());
  const auto after = std::chrono::steady_clock::now();
  REQUIRE(first.has_value());

  const auto& first_frame = first.value();
  CHECK(first_frame.metadata.sequence == 0);
  CHECK(first_frame.metadata.width == 2);
  CHECK(first_frame.metadata.height == 2);
  CHECK(first_frame.metadata.pixel_format == cam::PixelFormat::bgr8);
  CHECK(first_frame.metadata.captured_at >= before);
  CHECK(first_frame.metadata.captured_at <= after);
  CHECK(first_frame.pixels.type() == CV_8UC3);
  CHECK(first_frame.pixels.cols == 2);
  CHECK(first_frame.pixels.rows == 2);
  CHECK(first_frame.pixels.at<cv::Vec3b>(0, 0) == cv::Vec3b(0, 0, 255));
  CHECK(first_frame.pixels.at<cv::Vec3b>(0, 1) == cv::Vec3b(0, 255, 0));
  CHECK(first_frame.pixels.at<cv::Vec3b>(1, 0) == cv::Vec3b(255, 0, 0));
  CHECK(first_frame.pixels.at<cv::Vec3b>(1, 1) == cv::Vec3b(255, 255, 255));

  auto second = backend.capture(generous_deadline());
  REQUIRE(second.has_value());
  const auto& second_frame = second.value();
  CHECK(second_frame.metadata.sequence == 1);
  CHECK(second_frame.metadata.width == 3);
  CHECK(second_frame.metadata.height == 1);
  CHECK(second_frame.pixels.type() == CV_8UC3);
  CHECK(second_frame.pixels.cols == 3);
  CHECK(second_frame.pixels.rows == 1);
  CHECK(second_frame.pixels.at<cv::Vec3b>(0, 0) == cv::Vec3b(30, 20, 10));
  CHECK(second_frame.pixels.at<cv::Vec3b>(0, 1) == cv::Vec3b(60, 50, 40));
  CHECK(second_frame.pixels.at<cv::Vec3b>(0, 2) == cv::Vec3b(90, 80, 70));

  // Distinct frames must not alias one shared pixel buffer.
  CHECK(first_frame.pixels.data != second_frame.pixels.data);
}

TEST_CASE("CVF-002 B3: captured pixels remain owned after the backend is destroyed",
          "[cvf-002][B3][file]") {
  TempFrameDir dir("file_owned");
  const auto frame = dir.write_ppm_p6("frame0.ppm", 2, 1,
                                      {
                                          {255, 0, 0},
                                          {0, 0, 255},
                                      });
  const auto descriptor = file_descriptor("/dev/file-owned");
  cam::FileCameraConfig config{.descriptor = descriptor, .frame_paths = {frame}};

  cv::Mat held;
  {
    cam::FileCameraBackend backend{config};
    REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());
    auto captured = backend.capture(generous_deadline());
    REQUIRE(captured.has_value());
    held = captured.value().pixels;
  }

  REQUIRE_FALSE(held.empty());
  CHECK(held.data != nullptr);
  CHECK(held.type() == CV_8UC3);
  CHECK(held.at<cv::Vec3b>(0, 0) == cv::Vec3b(0, 0, 255));
  CHECK(held.at<cv::Vec3b>(0, 1) == cv::Vec3b(255, 0, 0));
}

TEST_CASE("CVF-002 B3: mono input is converted to BGR8", "[cvf-002][B3][file]") {
  TempFrameDir dir("file_mono");
  const auto frame = dir.write_pgm_p5("frame0.pgm", 2, 2, {10, 20, 30, 40});
  const auto descriptor = file_descriptor("/dev/file-mono");

  cam::FileCameraConfig config{.descriptor = descriptor, .frame_paths = {frame}};
  cam::FileCameraBackend backend{config};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  auto captured = backend.capture(generous_deadline());
  REQUIRE(captured.has_value());
  const auto& captured_frame = captured.value();
  CHECK(captured_frame.metadata.sequence == 0);
  CHECK(captured_frame.metadata.width == 2);
  CHECK(captured_frame.metadata.height == 2);
  CHECK(captured_frame.metadata.pixel_format == cam::PixelFormat::bgr8);
  CHECK(captured_frame.pixels.type() == CV_8UC3);
  CHECK(captured_frame.pixels.cols == 2);
  CHECK(captured_frame.pixels.rows == 2);
  CHECK(captured_frame.pixels.at<cv::Vec3b>(0, 0) == cv::Vec3b(10, 10, 10));
  CHECK(captured_frame.pixels.at<cv::Vec3b>(0, 1) == cv::Vec3b(20, 20, 20));
  CHECK(captured_frame.pixels.at<cv::Vec3b>(1, 0) == cv::Vec3b(30, 30, 30));
  CHECK(captured_frame.pixels.at<cv::Vec3b>(1, 1) == cv::Vec3b(40, 40, 40));
}

TEST_CASE("CVF-002 B3 negative: captures after the last frame fail with frames_exhausted",
          "[cvf-002][B3][file][negative]") {
  TempFrameDir dir("file_exhausted");
  const auto frame0 = dir.write_ppm_p6("frame0.ppm", 2, 2, solid_rgb(2, 2, {7, 7, 7}));
  const auto frame1 = dir.write_ppm_p6("frame1.ppm", 2, 2, solid_rgb(2, 2, {8, 8, 8}));
  const auto descriptor = file_descriptor("/dev/file-exhausted");

  cam::FileCameraConfig config{.descriptor = descriptor, .frame_paths = {frame0, frame1}};
  cam::FileCameraBackend backend{config};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  REQUIRE(backend.capture(generous_deadline()).has_value());
  REQUIRE(backend.capture(generous_deadline()).has_value());

  auto exhausted = backend.capture(generous_deadline());
  REQUIRE_FALSE(exhausted.has_value());
  CHECK(exhausted.failure().status == core::Status::camera_io);
  CHECK(exhausted.failure().code == core::ErrorCode::frames_exhausted);

  auto still_exhausted = backend.capture(generous_deadline());
  REQUIRE_FALSE(still_exhausted.has_value());
  CHECK(still_exhausted.failure().status == core::Status::camera_io);
  CHECK(still_exhausted.failure().code == core::ErrorCode::frames_exhausted);

  CHECK(backend.capture_call_count == 4);
}

TEST_CASE("CVF-002 B3 negative: an undecodable frame file fails with capture_failed",
          "[cvf-002][B3][file][negative]") {
  TempFrameDir dir("file_undecodable");
  const auto frame = dir.write_undecodable("not_an_image.ppm");
  const auto descriptor = file_descriptor("/dev/file-undecodable");

  cam::FileCameraConfig config{.descriptor = descriptor, .frame_paths = {frame}};
  cam::FileCameraBackend backend{config};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  auto captured = backend.capture(generous_deadline());
  REQUIRE_FALSE(captured.has_value());
  CHECK(captured.failure().status == core::Status::camera_io);
  CHECK(captured.failure().code == core::ErrorCode::capture_failed);

  auto again = backend.capture(generous_deadline());
  REQUIRE_FALSE(again.has_value());
  CHECK(again.failure().code == core::ErrorCode::capture_failed);
  CHECK(backend.capture_call_count == 2);
}
