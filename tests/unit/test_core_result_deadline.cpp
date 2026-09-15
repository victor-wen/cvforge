/*
 * Developer unit tests for the internal Result and Deadline core additions
 * (CVF-002). The camera contract consumes both types directly; these tests pin
 * the value/failure and deadline semantics locally.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"

using cvforwin::core::Deadline;
using cvforwin::core::ErrorCode;
using cvforwin::core::Failure;
using cvforwin::core::Result;
using cvforwin::core::Status;

using namespace std::chrono_literals;

TEST_CASE("Result carries a value and exposes it", "[core][result]")
{
    Result<int> valued{42};
    REQUIRE(valued.has_value());
    CHECK(static_cast<bool>(valued));
    CHECK(valued.value() == 42);
    CHECK_THROWS_AS(valued.failure(), std::logic_error);
}

TEST_CASE("Result carries a Failure and reports no value", "[core][result]")
{
    Failure failure{};
    failure.status = Status::camera_io;
    failure.code = ErrorCode::capture_failed;
    failure.message = "unit probe";

    Result<int> failed{std::move(failure)};
    REQUIRE_FALSE(failed.has_value());
    CHECK_FALSE(static_cast<bool>(failed));
    CHECK(failed.failure().status == Status::camera_io);
    CHECK(failed.failure().code == ErrorCode::capture_failed);
    CHECK(failed.failure().message == "unit probe");
    CHECK_THROWS_AS(failed.value(), std::logic_error);
}

TEST_CASE("Result round-trips a non-trivial movable value", "[core][result]")
{
    Result<std::string> valued{std::string{"cvforwin"}};
    REQUIRE(valued.has_value());
    CHECK(valued.value() == "cvforwin");

    Result<std::string> moved{std::move(valued)};
    REQUIRE(moved.has_value());
    CHECK(moved.value() == "cvforwin");
}

TEST_CASE("Result<void> defaults to success", "[core][result]")
{
    Result<void> success;
    REQUIRE(success.has_value());
    CHECK(static_cast<bool>(success));
    CHECK_THROWS_AS(success.failure(), std::logic_error);
}

TEST_CASE("Result<void> carries a Failure", "[core][result]")
{
    Failure failure{};
    failure.status = Status::timeout;
    failure.code = ErrorCode::deadline_expired;

    Result<void> failed{failure};
    REQUIRE_FALSE(failed.has_value());
    CHECK_FALSE(static_cast<bool>(failed));
    CHECK(failed.failure().status == Status::timeout);
    CHECK(failed.failure().code == ErrorCode::deadline_expired);
}

TEST_CASE("Deadline from_timeout_ms(0) is immediately expired", "[core][deadline]")
{
    const auto deadline = Deadline::from_timeout_ms(0);
    CHECK(deadline.expired());
    CHECK(deadline.remaining() == 0ms);
}

TEST_CASE("Deadline immediate() is expired", "[core][deadline]")
{
    const auto deadline = Deadline::immediate();
    CHECK(deadline.expired());
    CHECK(deadline.remaining() == 0ms);
}

TEST_CASE("Deadline remaining is bounded by the requested timeout", "[core][deadline]")
{
    const auto deadline = Deadline::from_timeout_ms(5000);
    CHECK_FALSE(deadline.expired());
    CHECK(deadline.remaining() >= 0ms);
    CHECK(deadline.remaining() <= 5000ms);
}

TEST_CASE("Deadline expires after the timeout and clamps remaining to zero", "[core][deadline]")
{
    const auto deadline = Deadline::from_timeout_ms(1);
    CHECK_FALSE(deadline.expired());
    std::this_thread::sleep_for(25ms);
    CHECK(deadline.expired());
    CHECK(deadline.remaining() == 0ms);
}
