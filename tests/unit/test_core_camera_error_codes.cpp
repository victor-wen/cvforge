/*
 * Developer unit tests for the camera error codes added by CVF-002
 * (1400..1409) and their frozen broad-status mapping.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

#include "core/error.h"
#include "core/status.h"

using cvforwin::core::ErrorCode;
using cvforwin::core::Status;

namespace {

struct CodeCase {
    ErrorCode code;
    std::uint32_t value;
    Status status;
};

constexpr std::array<CodeCase, 10> kCameraCodes = {
    CodeCase{ErrorCode::camera_not_found, 1400u, Status::camera_not_found},
    CodeCase{ErrorCode::camera_identity_ambiguous, 1401u, Status::camera_not_found},
    CodeCase{ErrorCode::camera_not_open, 1402u, Status::camera_io},
    CodeCase{ErrorCode::camera_disconnected, 1403u, Status::camera_io},
    CodeCase{ErrorCode::capture_failed, 1404u, Status::camera_io},
    CodeCase{ErrorCode::capture_timed_out, 1405u, Status::timeout},
    CodeCase{ErrorCode::frames_exhausted, 1406u, Status::camera_io},
    CodeCase{ErrorCode::deadline_expired, 1407u, Status::timeout},
    CodeCase{ErrorCode::selector_empty, 1408u, Status::invalid_argument},
    CodeCase{ErrorCode::descriptor_mismatch, 1409u, Status::camera_not_found},
};

}  // namespace

TEST_CASE("camera error codes use the frozen 1400 numbering", "[core][error][camera]")
{
    for (const auto& entry : kCameraCodes) {
        CHECK(cvforwin::core::to_public_error_code(entry.code) == entry.value);
    }
}

TEST_CASE("camera error codes map to the frozen broad statuses", "[core][error][camera]")
{
    for (const auto& entry : kCameraCodes) {
        CHECK(cvforwin::core::status_for(entry.code) == entry.status);
    }
}

TEST_CASE("camera error codes have stable symbolic names", "[core][error][camera]")
{
    for (const auto& entry : kCameraCodes) {
        CHECK_FALSE(cvforwin::core::error_code_name(entry.code).empty());
    }
    CHECK(cvforwin::core::error_code_name(ErrorCode::camera_not_found) == "camera_not_found");
    CHECK(cvforwin::core::error_code_name(ErrorCode::camera_identity_ambiguous) == "camera_identity_ambiguous");
    CHECK(cvforwin::core::error_code_name(ErrorCode::camera_not_open) == "camera_not_open");
    CHECK(cvforwin::core::error_code_name(ErrorCode::camera_disconnected) == "camera_disconnected");
    CHECK(cvforwin::core::error_code_name(ErrorCode::capture_failed) == "capture_failed");
    CHECK(cvforwin::core::error_code_name(ErrorCode::capture_timed_out) == "capture_timed_out");
    CHECK(cvforwin::core::error_code_name(ErrorCode::frames_exhausted) == "frames_exhausted");
    CHECK(cvforwin::core::error_code_name(ErrorCode::deadline_expired) == "deadline_expired");
    CHECK(cvforwin::core::error_code_name(ErrorCode::selector_empty) == "selector_empty");
    CHECK(cvforwin::core::error_code_name(ErrorCode::descriptor_mismatch) == "descriptor_mismatch");
}
