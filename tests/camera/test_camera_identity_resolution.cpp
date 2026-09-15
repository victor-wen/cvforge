// CVF-002 independent black-box tests: camera identity resolution (brief B1).
#include <string>
#include <vector>

#include "camera_test_support.h"

using namespace cvf002;

namespace {

std::vector<cam::CameraDescriptor> three_distinct_candidates() {
  return {
      make_descriptor("file", "/dev/a", "vid-a", "pid-a", "Cam A"),
      make_descriptor("file", "/dev/b", "vid-b", "pid-b", "Cam B"),
      make_descriptor("file", "/dev/c", "vid-c", "pid-c", "Cam C"),
  };
}

}  // namespace

TEST_CASE("CVF-002 B1 negative: an empty selector is rejected with selector_empty",
          "[cvf-002][B1][identity][negative]") {
  const auto candidates = three_distinct_candidates();
  cam::CameraSelector selector{};  // all fields empty = unspecified

  auto result = cam::resolve_identity(candidates, selector);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::invalid_argument);
  CHECK(result.failure().code == core::ErrorCode::selector_empty);
}

TEST_CASE("CVF-002 B1 negative: zero matches fails with camera_not_found",
          "[cvf-002][B1][identity][negative]") {
  const auto candidates = three_distinct_candidates();

  cam::CameraSelector selector{};
  selector.device_path = "/dev/does-not-exist";
  auto result = cam::resolve_identity(candidates, selector);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::camera_not_found);
  CHECK(result.failure().code == core::ErrorCode::camera_not_found);
}

TEST_CASE("CVF-002 B1 negative: an empty candidate list fails with camera_not_found",
          "[cvf-002][B1][identity][negative]") {
  const std::vector<cam::CameraDescriptor> no_candidates;
  cam::CameraSelector selector{};
  selector.device_path = "/dev/a";

  auto result = cam::resolve_identity(no_candidates, selector);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::camera_not_found);
  CHECK(result.failure().code == core::ErrorCode::camera_not_found);
}

TEST_CASE("CVF-002 B1: a selector matching exactly one field resolves that descriptor",
          "[cvf-002][B1][identity]") {
  const auto candidates = three_distinct_candidates();

  {
    cam::CameraSelector selector{};
    selector.device_path = "/dev/b";
    auto result = cam::resolve_identity(candidates, selector);
    INFO("selector set on device_path");
    REQUIRE(result.has_value());
    check_descriptor(result.value(), candidates[1]);
  }
  {
    cam::CameraSelector selector{};
    selector.vendor_id = "vid-a";
    auto result = cam::resolve_identity(candidates, selector);
    INFO("selector set on vendor_id");
    REQUIRE(result.has_value());
    check_descriptor(result.value(), candidates[0]);
  }
  {
    cam::CameraSelector selector{};
    selector.product_id = "pid-c";
    auto result = cam::resolve_identity(candidates, selector);
    INFO("selector set on product_id");
    REQUIRE(result.has_value());
    check_descriptor(result.value(), candidates[2]);
  }
  {
    cam::CameraSelector selector{};
    selector.friendly_name = "Cam A";
    auto result = cam::resolve_identity(candidates, selector);
    INFO("selector set on friendly_name");
    REQUIRE(result.has_value());
    check_descriptor(result.value(), candidates[0]);
  }
}

TEST_CASE("CVF-002 B1 negative: two matching descriptors are ambiguous even when the first matches",
          "[cvf-002][B1][identity][negative]") {
  const std::vector<cam::CameraDescriptor> candidates{
      make_descriptor("file", "/dev/a", "shared-vid", "pid-a", "Cam A"),
      make_descriptor("file", "/dev/b", "shared-vid", "pid-b", "Cam B"),
  };

  cam::CameraSelector selector{};
  selector.vendor_id = "shared-vid";
  auto result = cam::resolve_identity(candidates, selector);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::camera_not_found);
  CHECK(result.failure().code == core::ErrorCode::camera_identity_ambiguous);
}

TEST_CASE("CVF-002 B1 boundary: two candidates sharing both selected fields are ambiguous",
          "[cvf-002][B1][identity][boundary]") {
  const std::vector<cam::CameraDescriptor> candidates{
      make_descriptor("file", "/dev/a", "vid-x", "pid-x", "Cam A"),
      make_descriptor("file", "/dev/b", "vid-x", "pid-x", "Cam B"),
  };

  cam::CameraSelector selector{};
  selector.vendor_id = "vid-x";
  selector.product_id = "pid-x";
  auto result = cam::resolve_identity(candidates, selector);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().status == core::Status::camera_not_found);
  CHECK(result.failure().code == core::ErrorCode::camera_identity_ambiguous);
}

TEST_CASE("CVF-002 B1 boundary: every specified selector field must match the same candidate",
          "[cvf-002][B1][identity][boundary]") {
  const std::vector<cam::CameraDescriptor> candidates{
      make_descriptor("file", "/dev/a", "vid-1", "pid-1", "Cam A"),
      make_descriptor("file", "/dev/b", "vid-1", "pid-2", "Cam B"),
      make_descriptor("file", "/dev/c", "vid-2", "pid-1", "Cam C"),
  };

  cam::CameraSelector selector{};
  selector.vendor_id = "vid-1";
  selector.product_id = "pid-1";
  auto result = cam::resolve_identity(candidates, selector);

  REQUIRE(result.has_value());
  check_descriptor(result.value(), candidates[0]);
}

TEST_CASE("CVF-002 B1 negative: no first-match fallback when two candidates share one field",
          "[cvf-002][B1][identity][negative]") {
  const std::vector<cam::CameraDescriptor> candidates{
      make_descriptor("file", "/dev/a", "vid-9", "pid-a", "Cam A"),
      make_descriptor("file", "/dev/b", "vid-9", "pid-b", "Cam B"),
      make_descriptor("file", "/dev/c", "vid-other", "pid-c", "Cam C"),
  };

  cam::CameraSelector selector{};
  selector.vendor_id = "vid-9";
  auto result = cam::resolve_identity(candidates, selector);

  REQUIRE_FALSE(result.has_value());
  CHECK(result.failure().code == core::ErrorCode::camera_identity_ambiguous);
}
