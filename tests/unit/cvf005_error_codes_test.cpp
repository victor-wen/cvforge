// CVF-005 independent black-box tests: frozen diagnostics/artifact error-code
// numbering and broad-status mapping (brief interface_reference, negative_cases).
//
// This file deliberately includes only the pre-existing core error/status
// headers so that its RED evidence is the missing CVF-005 enumerators rather
// than the missing diagnostics/artifact headers.

#include <catch2/catch_test_macros.hpp>

#include "core/error.h"
#include "core/status.h"

#include <array>
#include <cstdint>
#include <set>
#include <string_view>

using cvforwin::core::ErrorCode;
using cvforwin::core::Status;

namespace {

struct CodeCase {
    ErrorCode code;
    std::uint32_t value;
    Status status;
    std::string_view name;
};

constexpr std::array<CodeCase, 7> kDiagnosticsArtifactCodes = {
    CodeCase{ErrorCode::diagnostics_invalid_config, 1700u, Status::invalid_argument,
             "diagnostics_invalid_config"},
    CodeCase{ErrorCode::log_write_error, 1701u, Status::internal_error, "log_write_error"},
    CodeCase{ErrorCode::artifacts_root_error, 1800u, Status::config_error, "artifacts_root_error"},
    CodeCase{ErrorCode::image_encode_error, 1801u, Status::internal_error, "image_encode_error"},
    CodeCase{ErrorCode::image_write_error, 1802u, Status::internal_error, "image_write_error"},
    CodeCase{ErrorCode::retention_error, 1803u, Status::internal_error, "retention_error"},
    CodeCase{ErrorCode::path_not_absolute, 1011u, Status::invalid_argument, "path_not_absolute"},
};

}  // namespace

TEST_CASE("CVF-005 error codes use the frozen 1700/1800 numbering", "[cvf-005][error][contract]")
{
    for (const auto& entry : kDiagnosticsArtifactCodes) {
        INFO(entry.name);
        CHECK(cvforwin::core::to_public_error_code(entry.code) == entry.value);
    }
}

TEST_CASE("CVF-005 error codes map to the frozen broad statuses", "[cvf-005][error][contract]")
{
    for (const auto& entry : kDiagnosticsArtifactCodes) {
        INFO(entry.name);
        CHECK(cvforwin::core::status_for(entry.code) == entry.status);
    }
}

TEST_CASE("CVF-005 error codes have stable symbolic names", "[cvf-005][error][contract]")
{
    for (const auto& entry : kDiagnosticsArtifactCodes) {
        INFO("value: " << entry.value);
        CHECK(cvforwin::core::error_code_name(entry.code) == entry.name);
    }
}

TEST_CASE("CVF-005 error code values are unique within the frozen numbering",
          "[cvf-005][error][contract]")
{
    std::set<std::uint32_t> values;
    for (const auto& entry : kDiagnosticsArtifactCodes) {
        CHECK(values.insert(entry.value).second);
    }
    CHECK(values.size() == kDiagnosticsArtifactCodes.size());
}
