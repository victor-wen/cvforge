// CVF-105 independent black-box tests: C ABI pointer-length/capacity validation.
//
// Brief B3/B4 (stateless half): every pointer-length or pointer-capacity pair is
// validated before side effects. A null pointer with a nonzero length/capacity
// is CVF_STATUS_INVALID_ARGUMENT; a required root/identifier needs a non-null
// pointer and a positive valid length; output buffers validated to the required
// capacity follow the BUFFER_TOO_SMALL rule.
//
// The public header include/cvforwin/cvf_api.h is frozen and unchanged; this
// file includes it and exercises cvf_initialize without any camera or context
// setup, so it is fully portable and hardware-free.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "cvf105_test.helpers.h"

#include "cvforwin/cvf_api.h"

namespace {

using cvf105::TempDir;

struct ErrorBuffer {
    std::array<char, CVF_ERROR_MESSAGE_REQUIRED_CAPACITY> storage{};
    cvf_error_info_v1 info{};

    ErrorBuffer()
    {
        info.struct_size = sizeof(cvf_error_info_v1);
        info.error_code = 0;
        info.message_utf8 = storage.data();
        info.message_capacity = static_cast<std::uint32_t>(storage.size());
        info.message_bytes_written = 0;
        info.message_bytes_required = 0;
    }
};

// The returned options struct stores non-owning views into the caller's
// storage, so every caller must pass a string whose lifetime covers the
// subsequent cvf_initialize()/cvf_inspect() call. Taking std::string_view
// avoids silently constructing a temporary std::string from a `const char*`
// argument that would be destroyed at the end of the full expression.
cvf_init_options_v1 options_for(std::string_view config_root, std::string_view output_root)
{
    cvf_init_options_v1 options{};
    options.struct_size = sizeof(cvf_init_options_v1);
    options.abi_version = CVF_ABI_VERSION_V1;
    options.config_root_utf8 = config_root.data();
    options.config_root_utf8_bytes = static_cast<std::uint32_t>(config_root.size());
    options.output_root_utf8 = output_root.data();
    options.output_root_utf8_bytes = static_cast<std::uint32_t>(output_root.size());
    options.flags = 0;
    options.log_callback = nullptr;
    options.user_data = nullptr;
    return options;
}

constexpr const char* kMissingRoot = "/nonexistent/cvf105-1007-config-root";

}  // namespace

TEST_CASE("CVF-105 B3 contract: the ABI version is exported without state",
          "[cvf-105][B3][abi]")
{
    CHECK(cvf_get_abi_version() == CVF_ABI_VERSION_V1);
    CHECK(CVF_RESULT_JSON_REQUIRED_CAPACITY == 65536u);
    CHECK(CVF_ERROR_MESSAGE_REQUIRED_CAPACITY == 1024u);
    CHECK(CVF_IMAGE_PATH_REQUIRED_CAPACITY == 4096u);
}

/* ------------------------------------------------------------------------- */
/* Null pointer arguments are rejected before any side effect.                */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B3 negative: a null options pointer is INVALID_ARGUMENT",
          "[cvf-105][B3][abi][negative]")
{
    cvf_context* context = nullptr;
    ErrorBuffer error;

    const cvf_status_t status = cvf_initialize(nullptr, &context, &error.info);

    CHECK(status == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(context == nullptr);
}

TEST_CASE("CVF-105 B3 negative: a null error structure and a null out_context are rejected",
          "[cvf-105][B3][abi][negative]")
{
    TempDir root("abi_null_out");
    const std::string config_root = root.path().string();
    const std::string output_root = (root.path() / "out").string();
    cvf_init_options_v1 options = options_for(config_root, output_root);
    cvf_context* context = nullptr;
    ErrorBuffer error;

    CHECK(cvf_initialize(&options, nullptr, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(cvf_initialize(&options, &context, nullptr) == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(context == nullptr);
}

/* ------------------------------------------------------------------------- */
/* Input root text-buffer combinations (required: non-null and positive).     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B4: a null config_root with a nonzero length is INVALID_ARGUMENT before side "
          "effects",
          "[cvf-105][B4][abi][negative]")
{
    TempDir root("abi_config_null_pos");
    const std::string output_root = root.path().string();
    const std::string valid_root = root.path().string();
    cvf_init_options_v1 options = options_for(valid_root, output_root);
    cvf_context* context = nullptr;
    ErrorBuffer error;

    options.config_root_utf8 = nullptr;
    options.config_root_utf8_bytes = 12u;

    const cvf_status_t status = cvf_initialize(&options, &context, &error.info);

    CHECK(status == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(context == nullptr);
}

TEST_CASE("CVF-105 B4: a null config_root with zero length is INVALID_ARGUMENT (required positive)",
          "[cvf-105][B4][abi][negative]")
{
    TempDir root("abi_config_null_zero");
    const std::string output_root = root.path().string();
    const std::string valid_root = root.path().string();
    cvf_init_options_v1 options = options_for(valid_root, output_root);
    cvf_context* context = nullptr;
    ErrorBuffer error;

    options.config_root_utf8 = nullptr;
    options.config_root_utf8_bytes = 0u;

    CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(context == nullptr);
}

TEST_CASE("CVF-105 B4: a non-null config_root with zero length is INVALID_ARGUMENT",
          "[cvf-105][B4][abi][negative]")
{
    TempDir root("abi_config_nonnull_zero");
    const std::string output_root = root.path().string();
    const std::string valid_root = root.path().string();
    cvf_init_options_v1 options = options_for(valid_root, output_root);
    cvf_context* context = nullptr;
    ErrorBuffer error;

    options.config_root_utf8_bytes = 0u;

    CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(context == nullptr);
}

TEST_CASE("CVF-105 B4: output_root null/non-null with zero or nonzero length is INVALID_ARGUMENT",
          "[cvf-105][B4][abi][negative]")
{
    TempDir root("abi_output_root_pairs");
    const std::string config_root = root.path().string();
    const std::string output_root = (root.path() / "out").string();
    cvf_context* context = nullptr;
    ErrorBuffer error;

    SECTION("null pointer with nonzero length")
    {
        cvf_init_options_v1 options = options_for(config_root, output_root);
        options.output_root_utf8 = nullptr;
        options.output_root_utf8_bytes = 5u;
        CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("null pointer with zero length")
    {
        cvf_init_options_v1 options = options_for(config_root, output_root);
        options.output_root_utf8 = nullptr;
        options.output_root_utf8_bytes = 0u;
        CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("non-null pointer with zero length")
    {
        cvf_init_options_v1 options = options_for(config_root, output_root);
        options.output_root_utf8_bytes = 0u;
        CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    }
    CHECK(context == nullptr);
}

TEST_CASE("CVF-105 B4 negative: an embedded NUL in a required root is INVALID_ARGUMENT",
          "[cvf-105][B4][abi][negative]")
{
    TempDir root("abi_embedded_nul");
    const std::string output_root = root.path().string();
    const std::string valid_root = root.path().string();
    const std::string raw_root = valid_root + std::string(1u, '\0') + "extra";
    cvf_init_options_v1 options = options_for(valid_root, output_root);
    cvf_context* context = nullptr;
    ErrorBuffer error;

    options.config_root_utf8 = raw_root.data();
    options.config_root_utf8_bytes = static_cast<std::uint32_t>(raw_root.size());

    CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(context == nullptr);
}

/* ------------------------------------------------------------------------- */
/* Error-info output buffer capacity rules.                                   */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B4: a null error message buffer with nonzero capacity is INVALID_ARGUMENT",
          "[cvf-105][B4][abi][negative]")
{
    TempDir root("abi_error_null_pos");
    const std::string config_root = root.path().string();
    const std::string output_root = (root.path() / "out").string();
    cvf_init_options_v1 options = options_for(config_root, output_root);
    cvf_context* context = nullptr;
    ErrorBuffer error;

    error.info.message_utf8 = nullptr;
    error.info.message_capacity = CVF_ERROR_MESSAGE_REQUIRED_CAPACITY;

    CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(context == nullptr);
}

TEST_CASE("CVF-105 B4: a short error message buffer reports the primary failure status and the "
          "required size",
          "[cvf-105][B4][abi][negative]")
{
    const std::string output_root = "/tmp";
    const std::string missing_root = kMissingRoot;

    // Reference run with the full required error buffer: the caller learns the
    // primary status and the exact bytes_required for this failure.
    cvf_init_options_v1 reference_options = options_for(missing_root, output_root);
    cvf_context* reference_context = nullptr;
    ErrorBuffer reference_error;
    const cvf_status_t reference_status =
        cvf_initialize(&reference_options, &reference_context, &reference_error.info);
    REQUIRE(reference_status != CVF_STATUS_OK);
    REQUIRE(reference_context == nullptr);
    REQUIRE(reference_error.info.message_bytes_required > 0u);

    // A non-null buffer with capacity zero stays a legal pointer-capacity pair;
    // the error-reporting channel keeps the primary status and reports the full
    // size instead of inventing BUFFER_TOO_SMALL.
    cvf_init_options_v1 options = options_for(missing_root, output_root);
    cvf_context* context = nullptr;
    ErrorBuffer error;
    error.info.message_capacity = 0u;

    const cvf_status_t status = cvf_initialize(&options, &context, &error.info);

    CHECK(status == reference_status);
    CHECK(context == nullptr);
    CHECK(error.info.message_bytes_written == 0u);
    CHECK(error.info.message_bytes_required == reference_error.info.message_bytes_required);
}

/* ------------------------------------------------------------------------- */
/* Structural validation stays intact.                                        */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B4 negative: struct_size, abi_version, reserved, and flag rules are enforced",
          "[cvf-105][B4][abi][negative]")
{
    TempDir root("abi_structural");
    const std::string config_root = root.path().string();
    const std::string output_root = (root.path() / "out").string();
    cvf_context* context = nullptr;
    ErrorBuffer error;

    SECTION("struct_size one below the v1 size")
    {
        cvf_init_options_v1 options = options_for(config_root, output_root);
        options.struct_size = sizeof(cvf_init_options_v1) - 1u;
        CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("abi_version mismatch")
    {
        cvf_init_options_v1 options = options_for(config_root, output_root);
        options.abi_version = CVF_ABI_VERSION_V1 + 1u;
        CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_ABI_MISMATCH);
    }
    SECTION("nonzero reserved word")
    {
        cvf_init_options_v1 options = options_for(config_root, output_root);
        options.reserved[0] = 1u;
        CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("unknown init flag bit")
    {
        cvf_init_options_v1 options = options_for(config_root, output_root);
        options.flags = 0x80000000u;
        CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("callback logging flag without a callback")
    {
        cvf_init_options_v1 options = options_for(config_root, output_root);
        options.flags = CVF_INIT_FLAG_CALLBACK_LOGGING;
        options.log_callback = nullptr;
        CHECK(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_INVALID_ARGUMENT);
    }
    CHECK(context == nullptr);
}

TEST_CASE("CVF-105 B3: a valid pointer set with a missing root fails without publishing a context",
          "[cvf-105][B3][abi]")
{
    // The exact status of a missing config root at the stateless boundary is not
    // pinned by the contract; the derivable properties are asserted (this
    // matches the existing CVF-001 stateless ABI test).
    const std::string output_root = "/tmp";
    const std::string missing_root = kMissingRoot;
    cvf_init_options_v1 options = options_for(missing_root, output_root);
    cvf_context* context = nullptr;
    ErrorBuffer error;

    const cvf_status_t status = cvf_initialize(&options, &context, &error.info);

    CHECK(status != CVF_STATUS_OK);
    CHECK(context == nullptr);
    CHECK(error.info.error_code != 0u);
    CHECK(error.info.message_bytes_required >= error.info.message_bytes_written + 1u);
}
