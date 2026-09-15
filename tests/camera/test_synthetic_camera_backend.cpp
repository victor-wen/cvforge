// CVF-002 independent black-box tests: SyntheticCameraBackend behavior (brief B4).
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include "camera_test_support.h"

using namespace cvf002;
using namespace std::chrono_literals;

namespace {

cam::CameraDescriptor synthetic_descriptor() {
  return make_descriptor("synthetic", "/dev/synthetic-a", "vid-syn", "pid-syn", "Synthetic Camera");
}

const std::vector<std::pair<int, int>> kSamplePoints{{0, 0},  {1, 0},   {0, 1},
                                                     {63, 47}, {17, 29}, {31, 30}};

}  // namespace

TEST_CASE("CVF-002 B4 negative: capture before open fails with camera_not_open",
          "[cvf-002][B4][synthetic][negative]") {
  cam::SyntheticCameraConfig config{.descriptor = synthetic_descriptor()};
  cam::SyntheticCameraBackend backend{config};

  auto captured = backend.capture(generous_deadline());

  REQUIRE_FALSE(captured.has_value());
  CHECK(captured.failure().status == core::Status::camera_io);
  CHECK(captured.failure().code == core::ErrorCode::camera_not_open);
  CHECK(backend.capture_call_count == 1);
  CHECK(backend.open_call_count == 0);
}

TEST_CASE("CVF-002 B4: default config yields 64x48 BGR8 frames with the documented formula",
          "[cvf-002][B4][synthetic]") {
  const auto descriptor = synthetic_descriptor();
  cam::SyntheticCameraConfig config{.descriptor = descriptor};
  cam::SyntheticCameraBackend backend{config};

  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());
  CHECK(backend.open_call_count == 1);

  const auto before = std::chrono::steady_clock::now();
  auto first = backend.capture(generous_deadline());
  const auto after = std::chrono::steady_clock::now();
  REQUIRE(first.has_value());

  const auto& first_frame = first.value();
  CHECK(first_frame.metadata.sequence == 0);
  CHECK(first_frame.metadata.width == 64);
  CHECK(first_frame.metadata.height == 48);
  CHECK(first_frame.metadata.pixel_format == cam::PixelFormat::bgr8);
  CHECK(first_frame.metadata.captured_at >= before);
  CHECK(first_frame.metadata.captured_at <= after);
  CHECK(first_frame.pixels.type() == CV_8UC3);
  CHECK(first_frame.pixels.cols == 64);
  CHECK(first_frame.pixels.rows == 48);

  for (const auto& [x, y] : kSamplePoints) {
    INFO("first frame pixel (" << x << ", " << y << ")");
    CHECK(first_frame.pixels.at<cv::Vec3b>(y, x) == synthetic_pixel(first_frame.metadata.sequence, x, y));
  }

  auto second = backend.capture(generous_deadline());
  REQUIRE(second.has_value());
  CHECK(second.value().metadata.sequence == 1);
  CHECK(second.value().pixels.type() == CV_8UC3);
  CHECK(second.value().pixels.at<cv::Vec3b>(0, 0) == synthetic_pixel(1, 0, 0));
  CHECK(second.value().pixels.at<cv::Vec3b>(47, 63) == synthetic_pixel(1, 63, 47));
  CHECK(backend.capture_call_count == 2);
}

TEST_CASE("CVF-002 B4: an explicit config width and height define the frame size",
          "[cvf-002][B4][synthetic]") {
  const auto descriptor = synthetic_descriptor();
  cam::SyntheticCameraConfig config{.descriptor = descriptor, .width = 16, .height = 12};
  cam::SyntheticCameraBackend backend{config};

  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());
  auto captured = backend.capture(generous_deadline());

  REQUIRE(captured.has_value());
  CHECK(captured.value().metadata.width == 16);
  CHECK(captured.value().metadata.height == 12);
  CHECK(captured.value().pixels.cols == 16);
  CHECK(captured.value().pixels.rows == 12);
  CHECK(captured.value().pixels.at<cv::Vec3b>(11, 15) == synthetic_pixel(0, 15, 11));
}

TEST_CASE("CVF-002 B4: non-zero settings width and height override the config defaults",
          "[cvf-002][B4][synthetic]") {
  const auto descriptor = synthetic_descriptor();
  cam::SyntheticCameraConfig config{.descriptor = descriptor};  // defaults 64x48
  cam::SyntheticCameraBackend backend{config};

  cam::CameraSettings settings{};
  settings.width = 8;
  settings.height = 6;

  REQUIRE(backend.open(descriptor, settings, generous_deadline()).has_value());
  auto captured = backend.capture(generous_deadline());

  REQUIRE(captured.has_value());
  CHECK(captured.value().metadata.width == 8);
  CHECK(captured.value().metadata.height == 6);
  CHECK(captured.value().pixels.cols == 8);
  CHECK(captured.value().pixels.rows == 6);
  CHECK(captured.value().pixels.at<cv::Vec3b>(0, 0) == synthetic_pixel(0, 0, 0));
  CHECK(captured.value().pixels.at<cv::Vec3b>(3, 5) == synthetic_pixel(0, 5, 3));
}

TEST_CASE("CVF-002 B4 boundary: a zero settings dimension keeps the config default",
          "[cvf-002][B4][synthetic][boundary]") {
  const auto descriptor = synthetic_descriptor();
  cam::SyntheticCameraConfig config{.descriptor = descriptor, .width = 16, .height = 12};
  cam::SyntheticCameraBackend backend{config};

  cam::CameraSettings settings{};
  settings.width = 8;  // height remains zero = unspecified

  REQUIRE(backend.open(descriptor, settings, generous_deadline()).has_value());
  auto captured = backend.capture(generous_deadline());

  REQUIRE(captured.has_value());
  CHECK(captured.value().metadata.width == 8);
  CHECK(captured.value().metadata.height == 12);
  CHECK(captured.value().pixels.cols == 8);
  CHECK(captured.value().pixels.rows == 12);
}

TEST_CASE("CVF-002 B4: sequence increments from zero across captures",
          "[cvf-002][B4][synthetic]") {
  const auto descriptor = synthetic_descriptor();
  cam::SyntheticCameraConfig config{.descriptor = descriptor, .width = 4, .height = 3};
  cam::SyntheticCameraBackend backend{config};
  REQUIRE(backend.open(descriptor, default_settings(), generous_deadline()).has_value());

  for (std::uint64_t expected = 0; expected < 4; ++expected) {
    auto captured = backend.capture(generous_deadline());
    REQUIRE(captured.has_value());
    INFO("capture index " << expected);
    CHECK(captured.value().metadata.sequence == expected);
    CHECK(captured.value().metadata.pixel_format == cam::PixelFormat::bgr8);
    CHECK(captured.value().pixels.at<cv::Vec3b>(2, 3) == synthetic_pixel(expected, 3, 2));
  }
  CHECK(backend.capture_call_count == 4);
}
