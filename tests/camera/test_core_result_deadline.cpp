// CVF-002 independent black-box tests: core Deadline and Result behaviors (brief B11, B12).
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

#include "camera_test_support.h"

using namespace cvf002;
using namespace std::chrono_literals;

TEST_CASE("CVF-002 B11: from_timeout_ms(0) is immediately expired with zero remaining",
          "[cvf-002][B11][deadline]") {
  auto deadline = core::Deadline::from_timeout_ms(0);
  CHECK(deadline.expired());
  CHECK(deadline.remaining() == 0ms);
}

TEST_CASE("CVF-002 B11: from_timeout_ms(5000) is not expired and remaining is bounded",
          "[cvf-002][B11][deadline]") {
  auto deadline = core::Deadline::from_timeout_ms(5000);
  CHECK_FALSE(deadline.expired());
  CHECK(deadline.remaining() >= 0ms);
  CHECK(deadline.remaining() <= 5000ms);
}

TEST_CASE("CVF-002 B11: a short deadline expires and remaining clamps to zero",
          "[cvf-002][B11][deadline]") {
  auto deadline = core::Deadline::from_timeout_ms(1);
  CHECK_FALSE(deadline.expired());
  std::this_thread::sleep_for(25ms);
  CHECK(deadline.expired());
  CHECK(deadline.remaining() == 0ms);
}

TEST_CASE("CVF-002 B11: immediate() is expired", "[cvf-002][B11][deadline]") {
  auto deadline = core::Deadline::immediate();
  CHECK(deadline.expired());
  CHECK(deadline.remaining() == 0ms);
}

TEST_CASE("CVF-002 B12: a valued Result exposes its value and throws on failure()",
          "[cvf-002][B12][result]") {
  core::Result<int> result{42};
  CHECK(result.has_value());
  CHECK(static_cast<bool>(result));
  CHECK(result.value() == 42);
  CHECK_THROWS_AS(result.failure(), std::logic_error);
}

TEST_CASE("CVF-002 B12: an error Result exposes its Failure and throws on value()",
          "[cvf-002][B12][result]") {
  core::Failure failure{};
  failure.status = core::Status::camera_io;
  failure.code = core::ErrorCode::capture_failed;
  failure.message = "CVF-002 probe failure";

  core::Result<int> result{std::move(failure)};
  CHECK_FALSE(result.has_value());
  CHECK_FALSE(static_cast<bool>(result));
  CHECK(result.failure().status == core::Status::camera_io);
  CHECK(result.failure().code == core::ErrorCode::capture_failed);
  CHECK(result.failure().message == "CVF-002 probe failure");
  CHECK_THROWS_AS(result.value(), std::logic_error);
}

TEST_CASE("CVF-002 B12: Result<void> defaults to success and carries a Failure",
          "[cvf-002][B12][result]") {
  core::Result<void> success;
  CHECK(success.has_value());
  CHECK(static_cast<bool>(success));
  CHECK_THROWS_AS(success.failure(), std::logic_error);

  core::Failure failure{};
  failure.status = core::Status::timeout;
  failure.code = core::ErrorCode::deadline_expired;
  failure.message = "CVF-002 deadline probe";

  core::Result<void> error{std::move(failure)};
  CHECK_FALSE(error.has_value());
  CHECK_FALSE(static_cast<bool>(error));
  CHECK(error.failure().status == core::Status::timeout);
  CHECK(error.failure().code == core::ErrorCode::deadline_expired);
  CHECK(error.failure().message == "CVF-002 deadline probe");
}

TEST_CASE("CVF-002 B12: a Result round-trips a non-trivial value type",
          "[cvf-002][B12][result]") {
  core::Result<std::string> result{std::string{"cvf-002"}};
  REQUIRE(result.has_value());
  CHECK(result.value() == "cvf-002");
}
