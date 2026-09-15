// CVF-004 independent black-box tests: frozen recipe-store error-code numbering
// and broad-status mapping (brief interface_reference, B2/B3).
#include <array>
#include <cstdint>
#include <set>
#include <string_view>

#include "cvf004_test_support.h"

using namespace cvf004;

namespace {

struct CodeCase {
    core::ErrorCode code;
    std::uint32_t value;
    core::Status status;
    std::string_view name;
};

constexpr std::array<CodeCase, 22> kRecipeStoreCodes = {
    CodeCase{core::ErrorCode::config_file_missing, 1600u, core::Status::config_error,
             "config_file_missing"},
    CodeCase{core::ErrorCode::config_parse_error, 1601u, core::Status::config_error,
             "config_parse_error"},
    CodeCase{core::ErrorCode::config_duplicate_key, 1602u, core::Status::config_error,
             "config_duplicate_key"},
    CodeCase{core::ErrorCode::config_unknown_key, 1603u, core::Status::config_error,
             "config_unknown_key"},
    CodeCase{core::ErrorCode::config_value_invalid, 1604u, core::Status::config_error,
             "config_value_invalid"},
    CodeCase{core::ErrorCode::config_schema_version, 1605u, core::Status::config_error,
             "config_schema_version"},
    CodeCase{core::ErrorCode::recipes_dir_missing, 1606u, core::Status::config_error,
             "recipes_dir_missing"},
    CodeCase{core::ErrorCode::config_io_error, 1607u, core::Status::config_error,
             "config_io_error"},
    CodeCase{core::ErrorCode::recipe_file_missing, 1610u, core::Status::config_error,
             "recipe_file_missing"},
    CodeCase{core::ErrorCode::recipe_parse_error, 1611u, core::Status::config_error,
             "recipe_parse_error"},
    CodeCase{core::ErrorCode::recipe_duplicate_key, 1612u, core::Status::config_error,
             "recipe_duplicate_key"},
    CodeCase{core::ErrorCode::recipe_unknown_key, 1613u, core::Status::config_error,
             "recipe_unknown_key"},
    CodeCase{core::ErrorCode::recipe_value_invalid, 1614u, core::Status::config_error,
             "recipe_value_invalid"},
    CodeCase{core::ErrorCode::recipe_schema_version, 1615u, core::Status::config_error,
             "recipe_schema_version"},
    CodeCase{core::ErrorCode::recipe_id_invalid, 1616u, core::Status::config_error,
             "recipe_id_invalid"},
    CodeCase{core::ErrorCode::recipe_id_duplicate, 1617u, core::Status::config_error,
             "recipe_id_duplicate"},
    CodeCase{core::ErrorCode::recipe_algorithm_unknown, 1618u, core::Status::config_error,
             "recipe_algorithm_unknown"},
    CodeCase{core::ErrorCode::recipe_parameters_invalid, 1619u, core::Status::config_error,
             "recipe_parameters_invalid"},
    CodeCase{core::ErrorCode::recipe_capture_invalid, 1620u, core::Status::config_error,
             "recipe_capture_invalid"},
    CodeCase{core::ErrorCode::recipe_artifacts_invalid, 1621u, core::Status::config_error,
             "recipe_artifacts_invalid"},
    CodeCase{core::ErrorCode::recipe_not_found, 1622u, core::Status::recipe_not_found,
             "recipe_not_found"},
    CodeCase{core::ErrorCode::recipe_io_error, 1623u, core::Status::config_error,
             "recipe_io_error"},
};

}  // namespace

TEST_CASE("CVF-004 error codes use the frozen 1600 numbering", "[cvf-004][error][contract]")
{
    for (const auto& entry : kRecipeStoreCodes) {
        INFO(entry.name);
        CHECK(core::to_public_error_code(entry.code) == entry.value);
    }
}

TEST_CASE("CVF-004 error codes map to the frozen broad statuses", "[cvf-004][error][contract]")
{
    for (const auto& entry : kRecipeStoreCodes) {
        INFO(entry.name);
        CHECK(core::status_for(entry.code) == entry.status);
    }
}

TEST_CASE("CVF-004 error codes have stable symbolic names", "[cvf-004][error][contract]")
{
    for (const auto& entry : kRecipeStoreCodes) {
        INFO("value: " << entry.value);
        CHECK(core::error_code_name(entry.code) == entry.name);
    }
}

TEST_CASE("CVF-004 error code values are unique within the frozen numbering",
          "[cvf-004][error][contract]")
{
    std::set<std::uint32_t> values;
    for (const auto& entry : kRecipeStoreCodes) {
        CHECK(values.insert(entry.value).second);
    }
    CHECK(values.size() == kRecipeStoreCodes.size());
}

TEST_CASE("CVF-004 B2: path_not_absolute keeps its frozen 1011 mapping",
          "[cvf-004][B2][error][contract]")
{
    CHECK(core::to_public_error_code(core::ErrorCode::path_not_absolute) == 1011u);
    CHECK(core::status_for(core::ErrorCode::path_not_absolute) == core::Status::invalid_argument);
}
