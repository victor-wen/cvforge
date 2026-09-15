/*
 * ABI mirror structures and validation for the public v1 boundary.
 *
 * The validation boundary receives the public structs as opaque byte ranges and
 * mirrors them into these plain C++ values before validating them. The mirrors
 * are internal: they are never aliases of the public ABI structs and they never
 * appear in an exported declaration.
 *
 * The expected struct size is always supplied by the C API translation unit,
 * which includes the real public header, so the v1 layout has a single source
 * of truth and core_model stays ABI-independent.
 *
 * Every validator returns std::nullopt when the value is acceptable and a
 * Failure otherwise. Validation is side-effect free and runs before any
 * observable work.
 */

#ifndef CVFORWIN_SRC_CORE_ABI_VALIDATION_H_
#define CVFORWIN_SRC_CORE_ABI_VALIDATION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "core/error.h"

namespace cvforwin::core {

inline constexpr std::uint32_t kAbiVersionV1 = 1u;
inline constexpr std::uint32_t kRequiredResultJsonCapacity = 65536u;
inline constexpr std::uint32_t kRequiredErrorMessageCapacity = 1024u;
inline constexpr std::uint32_t kRequiredImagePathCapacity = 4096u;
inline constexpr std::uint32_t kMaxRecipeIdBytes = 128u;
inline constexpr std::uint32_t kMaxRequestIdBytes = 128u;
inline constexpr std::uint32_t kMaxInputJsonBytes = 65536u;
inline constexpr std::uint32_t kDefaultTimeoutMs = 5000u;

inline constexpr std::uint32_t kInitFlagFileLogging = 1u << 0;
inline constexpr std::uint32_t kInitFlagCallbackLogging = 1u << 1;
inline constexpr std::uint32_t kKnownInitFlagsMask = kInitFlagFileLogging | kInitFlagCallbackLogging;

inline constexpr std::uint32_t kLogLevelTrace = 0u;
inline constexpr std::uint32_t kLogLevelCritical = 5u;

inline constexpr std::size_t kReservedCount8 = 8u;
inline constexpr std::size_t kReservedCount4 = 4u;

struct InitOptionsView {
    std::uint32_t struct_size = 0;
    std::uint32_t abi_version = 0;
    std::string_view config_root;
    std::string_view output_root;
    std::uint32_t flags = 0;
    bool log_callback_present = false;
    std::array<std::uint32_t, kReservedCount8> reserved{};
};

struct ErrorInfoView {
    std::uint32_t struct_size = 0;
    bool message_present = false;
    std::uint32_t message_capacity = 0;
    std::array<std::uint32_t, kReservedCount4> reserved{};
};

struct InspectionRequestView {
    std::uint32_t struct_size = 0;
    std::uint32_t abi_version = 0;
    std::string_view recipe_id;
    std::string_view request_id;
    std::string_view input_json;
    bool input_json_present = false;
    std::array<std::uint32_t, kReservedCount8> reserved{};
};

struct InspectionResultView {
    std::uint32_t struct_size = 0;
    bool output_json_present = false;
    std::uint32_t output_json_capacity = 0;
    bool image_path_present = false;
    std::uint32_t image_path_capacity = 0;
    bool error_message_present = false;
    std::uint32_t error_message_capacity = 0;
    std::array<std::uint32_t, kReservedCount8> reserved{};
};

std::optional<Failure> validate_error_info(const ErrorInfoView& error, std::size_t expected_struct_size);
std::optional<Failure> validate_init_options(const InitOptionsView& options, std::size_t expected_struct_size);
std::optional<Failure> validate_inspection_request(const InspectionRequestView& request,
                                                   std::size_t expected_struct_size);
std::optional<Failure> validate_inspection_result(const InspectionResultView& result,
                                                  std::size_t expected_struct_size);

}  // namespace cvforwin::core

#endif /* CVFORWIN_SRC_CORE_ABI_VALIDATION_H_ */
