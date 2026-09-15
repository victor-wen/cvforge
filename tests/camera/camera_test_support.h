#pragma once

// CVF-002 black-box test support (owner: test-engineer).
//
// Shared aliases and helpers for the independent camera-contract suite. This header
// only compiles against the frozen interface headers listed in the CVF-002 test brief
// and never includes production .cpp files.
//
// Interface spellings used here are derived from the CVF-002 brief's frozen
// interface_reference; the small set of assumptions beyond that text is recorded in
// .ai/reports/CVF-002-test-red.yaml (author assumptions).

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Frozen interface headers first: a missing interface header must be the first
// diagnostic in the author (RED) phase.
#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"

#include "camera/camera_backend.h"
#include "camera/captured_frame.h"
#include "camera/test_backends/file_camera_backend.h"
#include "camera/test_backends/synthetic_camera_backend.h"

#include <catch2/catch_test_macros.hpp>
#include <opencv2/core.hpp>

namespace cvf002 {

namespace core = cvforwin::core;
namespace cam = cvforwin::camera;

// Brief: inject_capture_failure(call_1based, disconnect|capture_error).
// Assumed spelling for the shared fault-kind enum.
using FaultKind = cam::FaultKind;

inline core::Deadline generous_deadline() {
  return core::Deadline::from_timeout_ms(5000);
}

inline cam::CameraSettings default_settings() { return cam::CameraSettings{}; }

inline cam::CameraDescriptor make_descriptor(std::string backend_key, std::string device_path,
                                             std::string vendor_id, std::string product_id,
                                             std::string friendly_name) {
  return cam::CameraDescriptor{
      .backend_key = std::move(backend_key),
      .device_path = std::move(device_path),
      .vendor_id = std::move(vendor_id),
      .product_id = std::move(product_id),
      .friendly_name = std::move(friendly_name),
  };
}

inline void check_descriptor(const cam::CameraDescriptor& actual,
                             const cam::CameraDescriptor& expected) {
  CHECK(actual.backend_key == expected.backend_key);
  CHECK(actual.device_path == expected.device_path);
  CHECK(actual.vendor_id == expected.vendor_id);
  CHECK(actual.product_id == expected.product_id);
  CHECK(actual.friendly_name == expected.friendly_name);
}

// Expected synthetic pixel per brief B4:
//   pixel(x, y) = BGR((x + y + seq) & 0xFF, (x + 2y + seq) & 0xFF, (2x + y + seq) & 0xFF)
inline cv::Vec3b synthetic_pixel(std::uint64_t sequence, int x, int y) {
  const auto byte = [](std::uint64_t value) {
    return static_cast<unsigned char>(value & 0xFFu);
  };
  return cv::Vec3b(byte(static_cast<std::uint64_t>(x + y) + sequence),
                   byte(static_cast<std::uint64_t>(x + 2 * y) + sequence),
                   byte(static_cast<std::uint64_t>(2 * x + y) + sequence));
}

}  // namespace cvf002
