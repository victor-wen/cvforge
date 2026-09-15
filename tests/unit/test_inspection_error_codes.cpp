/*
 * Developer unit tests for the algorithm error codes added by CVF-003
 * (1500..1508) and their frozen broad-status mapping.
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
    std::string_view name;
};

constexpr std::array<CodeCase, 9> kAlgorithmCodes = {
    CodeCase{ErrorCode::algorithm_not_found, 1500u, Status::algorithm_error, "algorithm_not_found"},
    CodeCase{ErrorCode::algorithm_duplicate_key, 1501u, Status::algorithm_error, "algorithm_duplicate_key"},
    CodeCase{ErrorCode::algorithm_key_invalid, 1502u, Status::invalid_argument, "algorithm_key_invalid"},
    CodeCase{ErrorCode::algorithm_parameters_invalid, 1503u, Status::config_error, "algorithm_parameters_invalid"},
    CodeCase{ErrorCode::algorithm_output_too_large, 1504u, Status::algorithm_error, "algorithm_output_too_large"},
    CodeCase{ErrorCode::algorithm_deadline_exceeded, 1505u, Status::timeout, "algorithm_deadline_exceeded"},
    CodeCase{ErrorCode::algorithm_exception, 1506u, Status::algorithm_error, "algorithm_exception"},
    CodeCase{ErrorCode::algorithm_frame_invalid, 1507u, Status::algorithm_error, "algorithm_frame_invalid"},
    CodeCase{ErrorCode::algorithm_input_invalid, 1508u, Status::invalid_argument, "algorithm_input_invalid"},
};

}  // namespace

TEST_CASE("algorithm error codes use the frozen 1500 numbering", "[core][error][algorithm]")
{
    for (const auto& entry : kAlgorithmCodes) {
        CHECK(cvforwin::core::to_public_error_code(entry.code) == entry.value);
    }
}

TEST_CASE("algorithm error codes map to the frozen broad statuses", "[core][error][algorithm]")
{
    for (const auto& entry : kAlgorithmCodes) {
        CHECK(cvforwin::core::status_for(entry.code) == entry.status);
    }
}

TEST_CASE("algorithm error codes have stable symbolic names", "[core][error][algorithm]")
{
    for (const auto& entry : kAlgorithmCodes) {
        CHECK(cvforwin::core::error_code_name(entry.code) == entry.name);
    }
}
