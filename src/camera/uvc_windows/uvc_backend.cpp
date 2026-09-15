/*
 * Windows Media Foundation UVC backend implementation.
 *
 * The backend is built only under if(WIN32) and never in portable builds. It
 * uses Media Foundation system APIs only (mfplat/mf/mfreadwrite/mfuuid plus
 * ole32 for COM), keeps every COM object behind a small RAII pointer, and
 * converts all failures to camera_contract Result values. No exception and no
 * Media Foundation type escapes the module.
 *
 * Device identity: enumeration reads the symbolic link (device path), parses
 * the four-hex-digit vendor/product identifiers from the device instance ID
 * contained in that link, and reads the friendly name. open() re-enumerates
 * and resolves exactly one device through camera::resolve_identity; there is
 * no first-device fallback.
 *
 * Capture: the source reader runs asynchronously with
 * MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, so the requested format
 * (RGB32 at the configured size and frame rate) is negotiated by Media
 * Foundation itself; a native-format fallback converts common uncompressed
 * and YUV subtypes. Every sample becomes a continuous, owned BGR8 cv::Mat
 * normalized to the configured dimensions, and a deadline expiry cancels the
 * pending read with IMFSourceReader::Flush.
 */

#if defined(_WIN32)

#include "camera/uvc_windows/uvc_backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace cvforwin::camera {

namespace {

constexpr std::string_view k_backend_key = "uvc";
constexpr std::uint32_t k_max_frame_dimension = 16384;
constexpr DWORD k_max_native_types = 256;
constexpr std::chrono::milliseconds k_flush_grace{1000};

/* ---------------------------------------------------------------------------
 * Minimal RAII COM pointer.
 * ------------------------------------------------------------------------- */

template <typename Interface>
class ComPtr {
public:
    ComPtr() noexcept = default;

    ~ComPtr()
    {
        reset();
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept
        : pointer_(other.detach())
    {
    }

    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            reset(other.detach());
        }
        return *this;
    }

    Interface* get() const noexcept
    {
        return pointer_;
    }

    /* Releases the current pointer and returns the address for an out parameter. */
    Interface** put() noexcept
    {
        reset();
        return &pointer_;
    }

    Interface* operator->() const noexcept
    {
        return pointer_;
    }

    explicit operator bool() const noexcept
    {
        return pointer_ != nullptr;
    }

    Interface* detach() noexcept
    {
        Interface* value = pointer_;
        pointer_ = nullptr;
        return value;
    }

    /* Adopts one reference to value and releases the previous pointer. */
    void reset(Interface* value = nullptr) noexcept
    {
        Interface* previous = pointer_;
        pointer_ = value;
        if (previous != nullptr) {
            previous->Release();
        }
    }

    /* Adds a reference to value and stores it. */
    void copy_from(Interface* value) noexcept
    {
        if (value != nullptr) {
            value->AddRef();
        }
        reset(value);
    }

private:
    Interface* pointer_ = nullptr;
};

/* ---------------------------------------------------------------------------
 * Failures.
 * ------------------------------------------------------------------------- */

std::string hresult_text(HRESULT result)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return stream.str();
}

core::Failure hresult_failure(const char* operation, HRESULT result, core::ErrorCode code)
{
    return core::make_failure(core::Status::camera_io, code,
                              std::string{operation} + " failed with HRESULT " + hresult_text(result));
}

core::Failure camera_failure(core::ErrorCode code, std::string message)
{
    return core::make_failure(core::Status::camera_io, code, std::move(message));
}

core::Failure internal_failure()
{
    return core::make_failure(core::Status::internal_error, core::ErrorCode::internal_exception,
                              "an unexpected exception crossed the UVC camera backend boundary");
}

/* ---------------------------------------------------------------------------
 * Text and identity helpers.
 * ------------------------------------------------------------------------- */

/*
 * Converts a Media Foundation wide string to UTF-8 without ever emitting an
 * embedded NUL. GetAllocatedString reports a character count whose treatment of
 * the terminating NUL is not consistent across Media Foundation versions, so the
 * conversion stops at the first NUL character in the source and truncates the
 * result at the first NUL byte as well. Identity values (device path, vendor,
 * product, friendly name) therefore never carry a trailing or embedded NUL.
 */
std::string wide_to_utf8(const wchar_t* text, UINT32 length)
{
    if (text == nullptr || length == 0) {
        return {};
    }
    UINT32 content_length = 0;
    while (content_length < length && text[content_length] != L'\0') {
        ++content_length;
    }
    if (content_length == 0) {
        return {};
    }
    const int wide_length = static_cast<int>(content_length);
    const int required = WideCharToMultiByte(CP_UTF8, 0, text, wide_length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, text, wide_length, utf8.data(), required, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    utf8.resize(static_cast<std::size_t>(written));
    const std::size_t terminator = utf8.find('\0');
    if (terminator != std::string::npos) {
        utf8.resize(terminator);
    }
    return utf8;
}

std::string attribute_string(IMFAttributes* attributes, REFGUID key)
{
    wchar_t* value = nullptr;
    UINT32 length = 0;
    const HRESULT result = attributes->GetAllocatedString(key, &value, &length);
    std::string text;
    if (SUCCEEDED(result) && value != nullptr) {
        text = wide_to_utf8(value, length);
    }
    if (value != nullptr) {
        CoTaskMemFree(value);
    }
    return text;
}

bool is_identity_boundary(char character)
{
    return character == '#' || character == '&' || character == '\\' || character == '/' || character == '?' ||
           character == '_';
}

std::string hex4_lower(std::string_view text)
{
    if (text.size() != 4) {
        return {};
    }
    std::string lowered;
    lowered.reserve(4);
    for (char character : text) {
        if (character >= '0' && character <= '9') {
            lowered.push_back(character);
            continue;
        }
        const char lower = static_cast<char>(character | 0x20);
        if (lower >= 'a' && lower <= 'f') {
            lowered.push_back(lower);
            continue;
        }
        return {};
    }
    return lowered;
}

/*
 * Extracts the first four-hex-digit value after marker (for example "vid_")
 * inside a device instance ID. The marker must start at an identity token
 * boundary so "vid_046d&pid_0825" parses both values and a substring like
 * "covid_1234" is ignored. An unknown or malformed identifier stays empty,
 * which the descriptor contract allows.
 */
std::string parse_hex4_after(std::string_view source, std::string_view marker)
{
    if (marker.empty() || source.size() < marker.size() + 4) {
        return {};
    }
    for (std::size_t index = 0; index + marker.size() + 4 <= source.size(); ++index) {
        bool matches = true;
        for (std::size_t offset = 0; offset < marker.size(); ++offset) {
            const char lowered = static_cast<char>(source[index + offset] | 0x20);
            if (lowered != marker[offset]) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }
        if (index > 0 && !is_identity_boundary(source[index - 1])) {
            continue;
        }
        const std::string candidate = hex4_lower(source.substr(index + marker.size(), 4));
        if (!candidate.empty()) {
            return candidate;
        }
    }
    return {};
}

bool descriptor_equals(const CameraDescriptor& left, const CameraDescriptor& right)
{
    return left.backend_key == right.backend_key && left.device_path == right.device_path &&
           left.vendor_id == right.vendor_id && left.product_id == right.product_id &&
           left.friendly_name == right.friendly_name;
}

std::size_t find_descriptor_index(const std::vector<CameraDescriptor>& descriptors, const CameraDescriptor& target)
{
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        if (descriptor_equals(descriptors[index], target)) {
            return index;
        }
    }
    return descriptors.size();
}

/* ---------------------------------------------------------------------------
 * Media type helpers.
 * ------------------------------------------------------------------------- */

bool subtype_is(const GUID& subtype, const GUID& expected)
{
    return IsEqualGUID(subtype, expected) != 0;
}

bool read_video_media_type(IMFMediaType* type, GUID& subtype, std::uint32_t& width, std::uint32_t& height,
                           double& frame_rate)
{
    GUID major = GUID_NULL;
    if (FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) || !subtype_is(major, MFMediaType_Video)) {
        return false;
    }
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        return false;
    }

    UINT32 frame_width = 0;
    UINT32 frame_height = 0;
    if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &frame_width, &frame_height))) {
        width = frame_width;
        height = frame_height;
    } else {
        width = 0;
        height = 0;
    }

    UINT32 rate_numerator = 0;
    UINT32 rate_denominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &rate_numerator, &rate_denominator)) &&
        rate_denominator != 0) {
        frame_rate = static_cast<double>(rate_numerator) / static_cast<double>(rate_denominator);
    } else {
        frame_rate = 0.0;
    }
    return true;
}

bool is_convertible_subtype(const GUID& subtype)
{
    return subtype_is(subtype, MFVideoFormat_RGB32) || subtype_is(subtype, MFVideoFormat_ARGB32) ||
           subtype_is(subtype, MFVideoFormat_RGB24) || subtype_is(subtype, MFVideoFormat_YUY2) ||
           subtype_is(subtype, MFVideoFormat_UYVY) || subtype_is(subtype, MFVideoFormat_NV12) ||
           subtype_is(subtype, MFVideoFormat_I420) || subtype_is(subtype, MFVideoFormat_YV12) ||
           subtype_is(subtype, MFVideoFormat_L8);
}

int format_rank(const GUID& subtype, PixelFormat preferred) noexcept
{
    const bool rgb = subtype_is(subtype, MFVideoFormat_RGB32) || subtype_is(subtype, MFVideoFormat_ARGB32) ||
                     subtype_is(subtype, MFVideoFormat_RGB24);
    const bool luma = subtype_is(subtype, MFVideoFormat_L8);
    switch (preferred) {
    case PixelFormat::mono8:
        return luma ? 0 : (rgb ? 2 : 1);
    case PixelFormat::bgr8:
    case PixelFormat::rgb8:
        return rgb ? 0 : (luma ? 2 : 1);
    default:
        return rgb ? 0 : 1;
    }
}

double score_native_type(const GUID& subtype, std::uint32_t width, std::uint32_t height, double frame_rate,
                         const CameraSettings& settings) noexcept
{
    double score = static_cast<double>(format_rank(subtype, settings.preferred_format));
    if (settings.width != 0 && settings.height != 0 && width != 0 && height != 0) {
        const double width_delta =
            std::fabs(static_cast<double>(width) - static_cast<double>(settings.width)) / static_cast<double>(settings.width);
        const double height_delta =
            std::fabs(static_cast<double>(height) - static_cast<double>(settings.height)) / static_cast<double>(settings.height);
        score += (width_delta + height_delta) * 100.0;
    }
    if (settings.frame_rate > 0.0 && frame_rate > 0.0) {
        score += std::fabs(frame_rate - settings.frame_rate) / settings.frame_rate * 10.0;
    }
    return score;
}

/* ---------------------------------------------------------------------------
 * Frame conversion.
 * ------------------------------------------------------------------------- */

/*
 * Bytes actually addressed by the converter for the given buffer stride, or 0
 * when the subtype/pitch combination is unsupported. This is the validation
 * bound against the locked buffer length: it covers the last row/plane the
 * conversion reads, including stride padding, so a padded or truncated sample
 * can never drive a read past the locked memory.
 *
 * Planar 4:2:0 layout (Media Foundation): the luma plane has H rows at the
 * luma pitch S; the two chroma planes each have H/2 rows at pitch S/2 and are
 * stored back to back after the luma plane. NV12 keeps the interleaved UV plane
 * at the luma pitch.
 */
std::size_t required_extent_bytes(const GUID& subtype, std::uint32_t width, std::uint32_t height, LONG stride)
{
    if (width == 0 || height == 0 || stride == 0) {
        return 0;
    }
    const std::size_t row_span =
        stride < 0 ? static_cast<std::size_t>(-static_cast<long long>(stride)) : static_cast<std::size_t>(stride);
    constexpr std::size_t k_max_row_span = static_cast<std::size_t>(k_max_frame_dimension) * 8;
    if (row_span > k_max_row_span) {
        return 0;
    }
    const std::size_t rows = static_cast<std::size_t>(height);
    const std::size_t width_bytes = static_cast<std::size_t>(width);
    if (subtype_is(subtype, MFVideoFormat_RGB32) || subtype_is(subtype, MFVideoFormat_ARGB32)) {
        return (rows - 1) * row_span + width_bytes * 4;
    }
    if (subtype_is(subtype, MFVideoFormat_RGB24)) {
        return (rows - 1) * row_span + width_bytes * 3;
    }
    if (subtype_is(subtype, MFVideoFormat_YUY2) || subtype_is(subtype, MFVideoFormat_UYVY)) {
        return (rows - 1) * row_span + width_bytes * 2;
    }
    if (subtype_is(subtype, MFVideoFormat_L8)) {
        return (rows - 1) * row_span + width_bytes;
    }
    if (subtype_is(subtype, MFVideoFormat_NV12)) {
        return (rows + rows / 2 - 1) * row_span + width_bytes;
    }
    if (subtype_is(subtype, MFVideoFormat_I420) || subtype_is(subtype, MFVideoFormat_YV12)) {
        if (stride <= 0 || (stride % 2) != 0) {
            return 0;
        }
        const std::size_t chroma_span = row_span / 2;
        return rows * row_span + (rows - 1) * chroma_span + width_bytes / 2;
    }
    return 0;
}

LONG packed_stride_for(const GUID& subtype, std::uint32_t width) noexcept
{
    if (subtype_is(subtype, MFVideoFormat_RGB32) || subtype_is(subtype, MFVideoFormat_ARGB32)) {
        return static_cast<LONG>(width) * 4;
    }
    if (subtype_is(subtype, MFVideoFormat_RGB24)) {
        return static_cast<LONG>(width) * 3;
    }
    if (subtype_is(subtype, MFVideoFormat_YUY2) || subtype_is(subtype, MFVideoFormat_UYVY)) {
        return static_cast<LONG>(width) * 2;
    }
    if (subtype_is(subtype, MFVideoFormat_L8)) {
        return static_cast<LONG>(width);
    }
    return -1;
}

enum class PlanarLayout {
    nv12,
    i420,
    yv12,
};

/*
 * Copies a packed (single stride) frame into an owned continuous cv::Mat with
 * channels 8-bit components. A negative stride (bottom-up buffer) is flipped
 * into top-down order.
 */
cv::Mat copy_packed_rows(const BYTE* data, LONG stride, std::uint32_t width, std::uint32_t height, int channels)
{
    const int rows = static_cast<int>(height);
    const int columns = static_cast<int>(width);
    cv::Mat packed(rows, columns, CV_MAKETYPE(CV_8U, channels));
    const std::size_t row_bytes = static_cast<std::size_t>(columns) * static_cast<std::size_t>(channels);
    const bool bottom_up = stride < 0;
    const std::ptrdiff_t row_stride = static_cast<std::ptrdiff_t>(stride);
    for (int row = 0; row < rows; ++row) {
        const int source_row = bottom_up ? (rows - 1 - row) : row;
        const BYTE* source = data + static_cast<std::ptrdiff_t>(source_row) * row_stride;
        std::memcpy(packed.ptr(row), source, row_bytes);
    }
    return packed;
}

/*
 * Copies a planar 4:2:0 frame into the canonical I420 layout OpenCV expects
 * (Y plane, then U, then V). In Media Foundation's planar layout each chroma
 * plane has HALF the luma stride and HALF the luma height, so the chroma pitch
 * is stride/2, each plane has height/2 rows, and each chroma row carries
 * width/2 samples. I420 stores U then V; YV12 stores V then U, so the copy
 * re-orders YV12 into the canonical U-then-V layout and the caller always uses
 * COLOR_YUV2BGR_I420. NV12 keeps its interleaved UV plane at the luma pitch.
 * Planar YUV buffers are top-down and even-sized; the caller validates stride,
 * width, and height, and the full extent against the locked buffer length.
 */
cv::Mat copy_planar_rows(const BYTE* data, LONG stride, std::uint32_t width, std::uint32_t height, PlanarLayout layout)
{
    const int columns = static_cast<int>(width);
    const int luma_rows = static_cast<int>(height);
    const int chroma_plane_rows = luma_rows / 2;
    cv::Mat planar(luma_rows + chroma_plane_rows, columns, CV_8UC1);
    const std::ptrdiff_t row_stride = static_cast<std::ptrdiff_t>(stride);
    const std::size_t luma_row_bytes = static_cast<std::size_t>(width);
    const std::size_t chroma_row_bytes = static_cast<std::size_t>(width) / 2;

    for (int row = 0; row < luma_rows; ++row) {
        std::memcpy(planar.ptr(row), data + static_cast<std::ptrdiff_t>(row) * row_stride, luma_row_bytes);
    }
    if (layout == PlanarLayout::nv12) {
        for (int row = 0; row < chroma_plane_rows; ++row) {
            const std::ptrdiff_t source_row = static_cast<std::ptrdiff_t>(luma_rows + row);
            std::memcpy(planar.ptr(luma_rows + row), data + source_row * row_stride, luma_row_bytes);
        }
        return planar;
    }

    /* Chroma pitch is half the luma stride (planar 4:2:0 layout). */
    const std::ptrdiff_t chroma_stride = row_stride / 2;
    const BYTE* first_plane = data + static_cast<std::ptrdiff_t>(luma_rows) * row_stride;
    const BYTE* second_plane = data + static_cast<std::ptrdiff_t>(luma_rows) * row_stride +
                               static_cast<std::ptrdiff_t>(chroma_plane_rows) * chroma_stride;
    const bool first_plane_is_u = layout == PlanarLayout::i420;
    const BYTE* u_plane = first_plane_is_u ? first_plane : second_plane;
    const BYTE* v_plane = first_plane_is_u ? second_plane : first_plane;
    for (int row = 0; row < chroma_plane_rows; ++row) {
        const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>(row) * chroma_stride;
        std::memcpy(planar.ptr(luma_rows + row), u_plane + offset, chroma_row_bytes);
        std::memcpy(planar.ptr(luma_rows + chroma_plane_rows + row), v_plane + offset, chroma_row_bytes);
    }
    return planar;
}

core::Result<cv::Mat> normalize_frame_size(cv::Mat pixels, const CameraSettings& settings)
{
    if (pixels.empty() || pixels.type() != CV_8UC3) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "the converted Media Foundation frame is not a BGR8 image");
    }
    if (settings.width == 0 || settings.height == 0) {
        return pixels;
    }
    const int requested_width = static_cast<int>(settings.width);
    const int requested_height = static_cast<int>(settings.height);
    if (pixels.cols == requested_width && pixels.rows == requested_height) {
        return pixels;
    }
    try {
        cv::Mat resized;
        cv::resize(pixels, resized, cv::Size(requested_width, requested_height), 0.0, 0.0, cv::INTER_AREA);
        return resized;
    } catch (const cv::Exception&) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "OpenCV failed to resize the Media Foundation frame to the configured size");
    }
}

core::Result<cv::Mat> convert_video_buffer(const BYTE* data, LONG stride, std::size_t available_bytes,
                                           const GUID& subtype, std::uint32_t width, std::uint32_t height,
                                           const CameraSettings& settings)
{
    if (data == nullptr || width == 0 || height == 0) {
        return camera_failure(core::ErrorCode::capture_failed, "the Media Foundation sample has no video data");
    }
    if (width > k_max_frame_dimension || height > k_max_frame_dimension) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "the Media Foundation frame exceeds the supported dimension limit");
    }
    const std::size_t extent = required_extent_bytes(subtype, width, height, stride);
    if (extent == 0) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "the Media Foundation stream uses an unsupported video subtype or pitch");
    }
    /* Zero means the buffer could not report a length; structural checks still apply. */
    if (available_bytes != 0 && available_bytes < extent) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "the Media Foundation sample is smaller than the required frame extent");
    }
    const std::ptrdiff_t row_stride = static_cast<std::ptrdiff_t>(stride);
    const std::ptrdiff_t row_bytes = static_cast<std::ptrdiff_t>(width);
    if (row_stride == 0 || (row_stride > 0 ? row_stride < row_bytes : -row_stride < row_bytes)) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "the Media Foundation sample stride is smaller than one row");
    }

    try {
        cv::Mat pixels;
        if (subtype_is(subtype, MFVideoFormat_RGB32) || subtype_is(subtype, MFVideoFormat_ARGB32)) {
            const cv::Mat packed = copy_packed_rows(data, stride, width, height, 4);
            cv::cvtColor(packed, pixels, cv::COLOR_BGRA2BGR);
        } else if (subtype_is(subtype, MFVideoFormat_RGB24)) {
            /* Media Foundation RGB24 memory order is B,G,R per pixel. */
            pixels = copy_packed_rows(data, stride, width, height, 3);
        } else if (subtype_is(subtype, MFVideoFormat_YUY2)) {
            const cv::Mat packed = copy_packed_rows(data, stride, width, height, 2);
            cv::cvtColor(packed, pixels, cv::COLOR_YUV2BGR_YUY2);
        } else if (subtype_is(subtype, MFVideoFormat_UYVY)) {
            const cv::Mat packed = copy_packed_rows(data, stride, width, height, 2);
            cv::cvtColor(packed, pixels, cv::COLOR_YUV2BGR_UYVY);
        } else if (subtype_is(subtype, MFVideoFormat_L8)) {
            const cv::Mat packed = copy_packed_rows(data, stride, width, height, 1);
            cv::cvtColor(packed, pixels, cv::COLOR_GRAY2BGR);
        } else if (subtype_is(subtype, MFVideoFormat_NV12)) {
            if (stride <= 0 || (width % 2) != 0 || (height % 2) != 0) {
                return camera_failure(core::ErrorCode::capture_failed,
                                      "the Media Foundation NV12 sample is not a top-down even-sized frame");
            }
            const cv::Mat planar = copy_planar_rows(data, stride, width, height, PlanarLayout::nv12);
            cv::cvtColor(planar, pixels, cv::COLOR_YUV2BGR_NV12);
        } else if (subtype_is(subtype, MFVideoFormat_I420)) {
            if (stride <= 0 || (stride % 2) != 0 || (width % 2) != 0 || (height % 2) != 0) {
                return camera_failure(core::ErrorCode::capture_failed,
                                      "the Media Foundation I420 sample is not a top-down even-sized frame with an "
                                      "even stride");
            }
            const cv::Mat planar = copy_planar_rows(data, stride, width, height, PlanarLayout::i420);
            cv::cvtColor(planar, pixels, cv::COLOR_YUV2BGR_I420);
        } else if (subtype_is(subtype, MFVideoFormat_YV12)) {
            if (stride <= 0 || (stride % 2) != 0 || (width % 2) != 0 || (height % 2) != 0) {
                return camera_failure(core::ErrorCode::capture_failed,
                                      "the Media Foundation YV12 sample is not a top-down even-sized frame with an "
                                      "even stride");
            }
            /* copy_planar_rows re-orders YV12 into the canonical Y,U,V layout. */
            const cv::Mat planar = copy_planar_rows(data, stride, width, height, PlanarLayout::yv12);
            cv::cvtColor(planar, pixels, cv::COLOR_YUV2BGR_I420);
        } else {
            return camera_failure(core::ErrorCode::capture_failed,
                                  "the Media Foundation stream uses an unsupported video subtype");
        }
        return normalize_frame_size(std::move(pixels), settings);
    } catch (const cv::Exception&) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "OpenCV failed to convert the Media Foundation frame to BGR8");
    }
}

/* ---------------------------------------------------------------------------
 * COM and Media Foundation lifetime.
 * ------------------------------------------------------------------------- */

class ComApartment {
public:
    ComApartment() noexcept
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(result);
    }

    ~ComApartment()
    {
        if (initialized_) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

private:
    bool initialized_ = false;
};

class MediaFoundationSession {
public:
    MediaFoundationSession() noexcept = default;
    ~MediaFoundationSession()
    {
        stop();
    }

    MediaFoundationSession(const MediaFoundationSession&) = delete;
    MediaFoundationSession& operator=(const MediaFoundationSession&) = delete;

    HRESULT start() noexcept
    {
        if (started_) {
            return S_OK;
        }
        const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (SUCCEEDED(result)) {
            started_ = true;
        }
        return result;
    }

    void stop() noexcept
    {
        if (!started_) {
            return;
        }
        MFShutdown();
        started_ = false;
    }

    bool active() const noexcept
    {
        return started_;
    }

private:
    bool started_ = false;
};

/* ---------------------------------------------------------------------------
 * Asynchronous source-reader callback.
 * ------------------------------------------------------------------------- */

/*
 * Carries exactly one completed read or flush to the waiting capture call.
 * Media Foundation may invoke the callback from any thread, so every field is
 * guarded by the mutex and the waiter is woken through the condition variable.
 */
class ReadCallback final : public IMFSourceReaderCallback {
public:
    ReadCallback() = default;
    ~ReadCallback() override = default;

    ReadCallback(const ReadCallback&) = delete;
    ReadCallback& operator=(const ReadCallback&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
            *object = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return references_.fetch_add(1) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = references_.fetch_sub(1) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnReadSample(HRESULT status, DWORD /*stream_index*/, DWORD stream_flags,
                                           LONGLONG /*timestamp*/, IMFSample* sample) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            read_status_ = status;
            read_flags_ = stream_flags;
            read_sample_.copy_from(sample);
            read_completed_ = true;
        }
        condition_.notify_all();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnFlush(DWORD /*stream_index*/) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            flush_completed_ = true;
        }
        condition_.notify_all();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnEvent(DWORD /*stream_index*/, IMFMediaEvent* /*event*/) override
    {
        return S_OK;
    }

    std::mutex& mutex() noexcept
    {
        return mutex_;
    }

    std::condition_variable& condition() noexcept
    {
        return condition_;
    }

    /* Clears the completion state before a new ReadSample is issued. */
    void begin_read()
    {
        read_status_ = S_OK;
        read_flags_ = 0;
        read_sample_.reset();
        read_completed_ = false;
    }

    bool read_completed() const noexcept
    {
        return read_completed_;
    }

    HRESULT read_status() const noexcept
    {
        return read_status_;
    }

    DWORD read_flags() const noexcept
    {
        return read_flags_;
    }

    IMFSample* read_sample() const noexcept
    {
        return read_sample_.get();
    }

    void begin_flush() noexcept
    {
        flush_completed_ = false;
    }

    bool flush_completed() const noexcept
    {
        return flush_completed_;
    }

private:
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    std::condition_variable condition_;
    bool read_completed_ = false;
    HRESULT read_status_ = S_OK;
    DWORD read_flags_ = 0;
    ComPtr<IMFSample> read_sample_;
    bool flush_completed_ = false;
};

}  // namespace

/* ---------------------------------------------------------------------------
 * Backend implementation.
 * ------------------------------------------------------------------------- */

class UvcCameraBackend::Impl {
public:
    Impl() = default;

    ~Impl()
    {
        close();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    core::Result<std::vector<CameraDescriptor>> enumerate(const core::Deadline& deadline)
    {
        try {
            if (deadline.expired()) {
                return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                          "the camera enumeration deadline expired before enumeration started");
            }
            core::Result<Enumeration> enumerated = enumerate_devices();
            if (!enumerated.has_value()) {
                return enumerated.failure();
            }
            Enumeration devices = std::move(enumerated).value();
            return std::move(devices.descriptors);
        } catch (const std::exception&) {
            return internal_failure();
        } catch (...) {
            return internal_failure();
        }
    }

    core::Result<void> open(const CameraDescriptor& descriptor, const CameraSettings& settings,
                            const core::Deadline& deadline)
    {
        try {
            teardown_stream();
            if (deadline.expired()) {
                return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                          "the camera open deadline expired before the device was activated");
            }
            const core::Result<void> activated = activate(descriptor, settings, deadline, false);
            if (!activated.has_value()) {
                close();
                return activated.failure();
            }
            return {};
        } catch (const std::exception&) {
            close();
            return internal_failure();
        } catch (...) {
            close();
            return internal_failure();
        }
    }

    core::Result<CapturedFrame> capture(const core::Deadline& deadline)
    {
        try {
            return capture_or_failure(deadline);
        } catch (const std::exception&) {
            return internal_failure();
        } catch (...) {
            return internal_failure();
        }
    }

    core::Result<void> reconnect(const CameraSettings& settings, const core::Deadline& deadline)
    {
        try {
            if (!has_identity()) {
                return camera_failure(core::ErrorCode::camera_not_open,
                                      "the UVC camera has never been opened, so there is nothing to reconnect");
            }
            if (deadline.expired()) {
                return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                          "the camera reconnect deadline expired before the device was re-activated");
            }
            const CameraDescriptor descriptor = descriptor_;
            teardown_stream();
            const core::Result<void> activated = activate(descriptor, settings, deadline, true);
            if (!activated.has_value()) {
                close();
                return activated.failure();
            }
            return {};
        } catch (const std::exception&) {
            close();
            return internal_failure();
        } catch (...) {
            close();
            return internal_failure();
        }
    }

    void close() noexcept
    {
        teardown_stream();
        if (mf_.active()) {
            ComApartment apartment;
            mf_.stop();
        }
        sequence_ = 0;
    }

private:
    struct Enumeration {
        std::vector<CameraDescriptor> descriptors;
        std::vector<ComPtr<IMFActivate>> activations;
    };

    bool has_identity() const noexcept
    {
        return !descriptor_.device_path.empty() || !descriptor_.vendor_id.empty() || !descriptor_.product_id.empty() ||
               !descriptor_.friendly_name.empty();
    }

    void teardown_stream() noexcept
    {
        reader_.reset();
        source_.reset();
        open_ = false;
        needs_reconnect_ = false;
        output_subtype_ = GUID_NULL;
        output_width_ = 0;
        output_height_ = 0;
    }

    core::Result<Enumeration> enumerate_devices()
    {
        ComApartment apartment;
        const HRESULT startup = mf_.start();
        if (FAILED(startup)) {
            return hresult_failure("MFStartup", startup, core::ErrorCode::capture_failed);
        }

        ComPtr<IMFAttributes> attributes;
        HRESULT result = MFCreateAttributes(attributes.put(), 1);
        if (FAILED(result)) {
            return hresult_failure("MFCreateAttributes", result, core::ErrorCode::capture_failed);
        }
        result = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (FAILED(result)) {
            return hresult_failure("IMFAttributes::SetGUID", result, core::ErrorCode::capture_failed);
        }

        IMFActivate** devices = nullptr;
        UINT32 count = 0;
        result = MFEnumDeviceSources(attributes.get(), &devices, &count);
        if (FAILED(result)) {
            return hresult_failure("MFEnumDeviceSources", result, core::ErrorCode::capture_failed);
        }

        std::vector<ComPtr<IMFActivate>> activations;
        activations.reserve(count);
        for (UINT32 index = 0; index < count; ++index) {
            ComPtr<IMFActivate> activation;
            activation.reset(devices[index]);
            activations.push_back(std::move(activation));
        }
        CoTaskMemFree(devices);

        Enumeration enumerated;
        enumerated.activations.reserve(activations.size());
        enumerated.descriptors.reserve(activations.size());
        for (ComPtr<IMFActivate>& activation : activations) {
            const std::string symbolic_link =
                attribute_string(activation.get(), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            if (symbolic_link.empty()) {
                /*
                 * A device without a symbolic link has no stable device path,
                 * so it cannot satisfy the descriptor contract and is not
                 * enumerated; it is never replaced by a guessed identity.
                 */
                continue;
            }
            CameraDescriptor descriptor;
            descriptor.backend_key = std::string{k_backend_key};
            descriptor.device_path = symbolic_link;
            descriptor.vendor_id = parse_hex4_after(symbolic_link, "vid_");
            descriptor.product_id = parse_hex4_after(symbolic_link, "pid_");
            descriptor.friendly_name = attribute_string(activation.get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            enumerated.descriptors.push_back(std::move(descriptor));
            enumerated.activations.push_back(std::move(activation));
        }
        return enumerated;
    }

    core::Result<void> activate(const CameraDescriptor& descriptor, const CameraSettings& settings,
                                const core::Deadline& deadline, bool recovery)
    {
        core::Result<Enumeration> enumerated = enumerate_devices();
        if (!enumerated.has_value()) {
            if (recovery) {
                return camera_failure(core::ErrorCode::camera_disconnected,
                                      "the UVC device could not be enumerated during reconnect");
            }
            return enumerated.failure();
        }
        Enumeration devices = std::move(enumerated).value();
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                      "the camera activation deadline expired while enumerating the device");
        }

        const CameraSelector selector{descriptor.device_path, descriptor.vendor_id, descriptor.product_id,
                                      descriptor.friendly_name};
        const core::Result<CameraDescriptor> matched = resolve_identity(devices.descriptors, selector);
        if (!matched.has_value()) {
            if (recovery) {
                return camera_failure(core::ErrorCode::camera_disconnected,
                                      "the configured UVC device has not reappeared");
            }
            if (matched.failure().status == core::Status::camera_not_found &&
                matched.failure().code == core::ErrorCode::camera_identity_ambiguous) {
                return matched.failure();
            }
            return core::make_failure(core::Status::camera_not_found, core::ErrorCode::camera_not_found,
                                      "the camera descriptor does not match exactly one attached UVC device: " +
                                          matched.failure().message);
        }

        const std::size_t index = find_descriptor_index(devices.descriptors, matched.value());
        if (index >= devices.descriptors.size()) {
            return core::make_failure(core::Status::camera_not_found, core::ErrorCode::camera_not_found,
                                      "the resolved UVC descriptor could not be located in the enumeration");
        }

        const core::Result<void> started = start_stream(devices.activations[index].get(), settings, deadline);
        if (!started.has_value()) {
            return started.failure();
        }
        descriptor_ = descriptor;
        settings_ = settings;
        open_ = true;
        needs_reconnect_ = false;
        return {};
    }

    core::Result<void> start_stream(IMFActivate* device_activate, const CameraSettings& settings,
                                    const core::Deadline& deadline)
    {
        ComPtr<IMFMediaSource> source;
        HRESULT result =
            device_activate->ActivateObject(__uuidof(IMFMediaSource), reinterpret_cast<void**>(source.put()));
        if (FAILED(result)) {
            return hresult_failure("IMFActivate::ActivateObject", result, core::ErrorCode::capture_failed);
        }
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                      "the camera activation deadline expired before the source reader was created");
        }

        if (!callback_) {
            callback_.reset(new ReadCallback());
        }
        ComPtr<IMFAttributes> attributes;
        result = MFCreateAttributes(attributes.put(), 2);
        if (FAILED(result)) {
            return hresult_failure("MFCreateAttributes", result, core::ErrorCode::capture_failed);
        }
        result = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback_.get());
        if (FAILED(result)) {
            return hresult_failure("IMFAttributes::SetUnknown", result, core::ErrorCode::capture_failed);
        }
        result = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (FAILED(result)) {
            return hresult_failure("IMFAttributes::SetUINT32", result, core::ErrorCode::capture_failed);
        }

        ComPtr<IMFSourceReader> reader;
        result = MFCreateSourceReaderFromMediaSource(source.get(), attributes.get(), reader.put());
        if (FAILED(result)) {
            return hresult_failure("MFCreateSourceReaderFromMediaSource", result, core::ErrorCode::capture_failed);
        }

        reader_ = std::move(reader);
        source_ = std::move(source);
        const core::Result<void> selected = select_format(settings, deadline);
        if (!selected.has_value()) {
            reader_.reset();
            source_.reset();
            return selected.failure();
        }
        return {};
    }

    /*
     * Primary negotiation: request RGB32 at the configured frame size and
     * frame rate; advanced video processing makes Media Foundation insert the
     * converter/decoder it needs. Fallback: pick the closest convertible
     * native media type. The negotiated type is cached for conversion. Every
     * attempt is bounded by the activation deadline.
     */
    core::Result<void> select_format(const CameraSettings& settings, const core::Deadline& deadline)
    {
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                      "the camera activation deadline expired before format negotiation started");
        }
        ComPtr<IMFMediaType> requested;
        HRESULT result = MFCreateMediaType(requested.put());
        if (SUCCEEDED(result)) {
            result = requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(result)) {
            result = requested->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        }
        if (SUCCEEDED(result) && settings.width != 0 && settings.height != 0) {
            result = MFSetAttributeSize(requested.get(), MF_MT_FRAME_SIZE, settings.width, settings.height);
        }
        if (SUCCEEDED(result) && settings.frame_rate > 0.0) {
            const double scaled = settings.frame_rate * 1000.0;
            const double clamped = scaled > 4294967.0 ? 4294967.0 : scaled;
            const UINT32 numerator = static_cast<UINT32>(clamped + 0.5);
            if (numerator > 0) {
                result = MFSetAttributeRatio(requested.get(), MF_MT_FRAME_RATE, numerator, 1000);
            }
        }
        if (SUCCEEDED(result)) {
            result = requested->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        }
        if (SUCCEEDED(result)) {
            result = reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                                                  requested.get());
            if (SUCCEEDED(result)) {
                return refresh_output_type();
            }
        }

        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                      "the camera activation deadline expired during format negotiation");
        }
        const core::Result<void> native = select_native_format(settings, deadline);
        if (native.has_value()) {
            return native;
        }
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                      "the camera activation deadline expired during format negotiation");
        }

        /* Last resort: request RGB32 without size or rate constraints. */
        ComPtr<IMFMediaType> relaxed;
        result = MFCreateMediaType(relaxed.put());
        if (SUCCEEDED(result)) {
            result = relaxed->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        }
        if (SUCCEEDED(result)) {
            result = relaxed->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        }
        if (SUCCEEDED(result)) {
            result = reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                                                  relaxed.get());
        }
        if (SUCCEEDED(result)) {
            return refresh_output_type();
        }
        return native.failure();
    }

    core::Result<void> select_native_format(const CameraSettings& settings, const core::Deadline& deadline)
    {
        ComPtr<IMFMediaType> best;
        double best_score = std::numeric_limits<double>::max();
        for (DWORD index = 0; index < k_max_native_types; ++index) {
            if (deadline.expired()) {
                return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                          "the camera activation deadline expired while examining native formats");
            }
            ComPtr<IMFMediaType> candidate;
            const HRESULT native = reader_->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                                               index, candidate.put());
            if (native == MF_E_NO_MORE_TYPES) {
                break;
            }
            if (FAILED(native)) {
                return hresult_failure("IMFSourceReader::GetNativeMediaType", native, core::ErrorCode::capture_failed);
            }
            GUID subtype = GUID_NULL;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            double frame_rate = 0.0;
            if (!read_video_media_type(candidate.get(), subtype, width, height, frame_rate)) {
                continue;
            }
            if (!is_convertible_subtype(subtype)) {
                continue;
            }
            const double score = score_native_type(subtype, width, height, frame_rate, settings);
            if (score < best_score) {
                best_score = score;
                best = std::move(candidate);
            }
        }
        if (!best) {
            return camera_failure(core::ErrorCode::capture_failed,
                                  "the UVC device exposes no convertible video media type");
        }
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                      "the camera activation deadline expired while examining native formats");
        }
        const HRESULT selected =
            reader_->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, best.get());
        if (FAILED(selected)) {
            return hresult_failure("IMFSourceReader::SetCurrentMediaType", selected, core::ErrorCode::capture_failed);
        }
        return refresh_output_type();
    }

    core::Result<void> refresh_output_type()
    {
        ComPtr<IMFMediaType> current;
        const HRESULT result = reader_->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                                            current.put());
        if (FAILED(result)) {
            return hresult_failure("IMFSourceReader::GetCurrentMediaType", result, core::ErrorCode::capture_failed);
        }
        GUID subtype = GUID_NULL;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        double frame_rate = 0.0;
        if (!read_video_media_type(current.get(), subtype, width, height, frame_rate)) {
            return camera_failure(core::ErrorCode::capture_failed,
                                  "the UVC stream does not expose a video media type");
        }
        output_subtype_ = subtype;
        output_width_ = width;
        output_height_ = height;
        return {};
    }

    core::Result<CapturedFrame> capture_or_failure(const core::Deadline& deadline)
    {
        if (!open_ || !reader_ || !callback_) {
            return camera_failure(core::ErrorCode::camera_not_open, "the UVC camera session is not open");
        }
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::capture_timed_out,
                                      "the capture deadline expired before the frame request was issued");
        }
        if (needs_reconnect_) {
            return camera_failure(core::ErrorCode::camera_disconnected,
                                  "the UVC stream needs a reconnect before the next capture");
        }

        while (true) {
            {
                std::lock_guard<std::mutex> lock(callback_->mutex());
                callback_->begin_read();
            }
            const HRESULT issued =
                reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr,
                                    nullptr, nullptr);
            if (FAILED(issued)) {
                needs_reconnect_ = true;
                return hresult_failure("IMFSourceReader::ReadSample", issued, core::ErrorCode::capture_failed);
            }

            std::unique_lock<std::mutex> lock(callback_->mutex());
            const bool completed = callback_->condition().wait_for(
                lock, deadline.remaining(), [this] { return callback_->read_completed(); });
            if (!completed) {
                lock.unlock();
                if (!cancel_pending_read(deadline)) {
                    needs_reconnect_ = true;
                }
                return core::make_failure(core::Status::timeout, core::ErrorCode::capture_timed_out,
                                          "the capture deadline expired before a Media Foundation sample arrived");
            }

            const HRESULT sample_status = callback_->read_status();
            const DWORD sample_flags = callback_->read_flags();
            ComPtr<IMFSample> sample;
            sample.copy_from(callback_->read_sample());
            lock.unlock();

            if (FAILED(sample_status)) {
                needs_reconnect_ = true;
                return hresult_failure("IMFSourceReaderCallback::OnReadSample", sample_status,
                                       core::ErrorCode::capture_failed);
            }
            if ((sample_flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                const core::Result<void> refreshed = refresh_output_type();
                if (!refreshed.has_value()) {
                    needs_reconnect_ = true;
                    return refreshed.failure();
                }
            }
            if (sample) {
                const core::Result<cv::Mat> converted = convert_sample(sample.get());
                if (!converted.has_value()) {
                    return converted.failure();
                }
                CapturedFrame frame;
                frame.pixels = std::move(converted).value();
                frame.metadata.sequence = sequence_;
                frame.metadata.captured_at = std::chrono::steady_clock::now();
                frame.metadata.width = static_cast<std::uint32_t>(frame.pixels.cols);
                frame.metadata.height = static_cast<std::uint32_t>(frame.pixels.rows);
                frame.metadata.pixel_format = PixelFormat::bgr8;
                ++sequence_;
                return frame;
            }
            if ((sample_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                needs_reconnect_ = true;
                return camera_failure(core::ErrorCode::capture_failed, "the UVC stream ended");
            }
            /* STREAMTICK or a flag-only completion: retry within the deadline. */
            if (deadline.expired()) {
                return core::make_failure(core::Status::timeout, core::ErrorCode::capture_timed_out,
                                          "the capture deadline expired before a Media Foundation sample arrived");
            }
        }
    }

    /*
     * Cancels an outstanding asynchronous read without waiting past the capture
     * deadline: the flush wait is min(k_flush_grace, remaining deadline). The
     * Flush request itself is asynchronous and returns immediately. When the
     * deadline is already spent or the flush does not complete in that budget,
     * the stream is marked for a reconnect instead of being reused in an unknown
     * state; a late completion is cleared so it cannot leak into the next read.
     */
    bool cancel_pending_read(const core::Deadline& deadline)
    {
        {
            std::lock_guard<std::mutex> lock(callback_->mutex());
            callback_->begin_flush();
        }
        const HRESULT result =
            reader_->Flush(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
        if (FAILED(result)) {
            return false;
        }
        const std::chrono::milliseconds budget = std::min(k_flush_grace, deadline.remaining());
        if (budget <= std::chrono::milliseconds::zero()) {
            std::lock_guard<std::mutex> lock(callback_->mutex());
            callback_->begin_read();
            return false;
        }
        std::unique_lock<std::mutex> lock(callback_->mutex());
        const bool flushed = callback_->condition().wait_for(lock, budget, [this] {
            return callback_->flush_completed();
        });
        /* A late completion from the cancelled read must not leak into the next capture. */
        callback_->begin_read();
        return flushed;
    }

    core::Result<cv::Mat> convert_sample(IMFSample* sample) const
    {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT result = sample->ConvertToContiguousBuffer(buffer.put());
        if (FAILED(result)) {
            return hresult_failure("IMFSample::ConvertToContiguousBuffer", result, core::ErrorCode::capture_failed);
        }

        ComPtr<IMF2DBuffer> buffer_2d;
        if (SUCCEEDED(buffer->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(buffer_2d.put())))) {
            BYTE* scanline = nullptr;
            LONG stride = 0;
            result = buffer_2d->Lock2D(&scanline, &stride);
            if (FAILED(result)) {
                return hresult_failure("IMF2DBuffer::Lock2D", result, core::ErrorCode::capture_failed);
            }
            DWORD contiguous_length = 0;
            const HRESULT length_result = buffer_2d->GetContiguousLength(&contiguous_length);
            const std::size_t available =
                SUCCEEDED(length_result) ? static_cast<std::size_t>(contiguous_length) : 0;
            core::Result<cv::Mat> pixels =
                (SUCCEEDED(length_result) && available == 0)
                    ? camera_failure(core::ErrorCode::capture_failed,
                                     "the Media Foundation sample buffer is empty")
                    : convert_video_buffer(scanline, stride, available, output_subtype_, output_width_, output_height_,
                                           settings_);
            buffer_2d->Unlock2D();
            return pixels;
        }

        const LONG packed_stride = packed_stride_for(output_subtype_, output_width_);
        if (packed_stride < 0) {
            return camera_failure(core::ErrorCode::capture_failed,
                                  "the Media Foundation sample does not expose a 2D buffer for this video subtype");
        }
        BYTE* data = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        result = buffer->Lock(&data, &max_length, &current_length);
        if (FAILED(result)) {
            return hresult_failure("IMFMediaBuffer::Lock", result, core::ErrorCode::capture_failed);
        }
        core::Result<cv::Mat> pixels =
            current_length == 0
                ? camera_failure(core::ErrorCode::capture_failed, "the Media Foundation sample buffer is empty")
                : convert_video_buffer(data, packed_stride, static_cast<std::size_t>(current_length), output_subtype_,
                                       output_width_, output_height_, settings_);
        buffer->Unlock();
        return pixels;
    }

    MediaFoundationSession mf_;
    ComPtr<ReadCallback> callback_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<IMFMediaSource> source_;
    CameraDescriptor descriptor_;
    CameraSettings settings_;
    GUID output_subtype_ = GUID_NULL;
    std::uint32_t output_width_ = 0;
    std::uint32_t output_height_ = 0;
    std::uint64_t sequence_ = 0;
    bool open_ = false;
    bool needs_reconnect_ = false;
};

UvcCameraBackend::UvcCameraBackend()
    : impl_(std::make_unique<Impl>())
{
}

UvcCameraBackend::~UvcCameraBackend() = default;

std::string_view UvcCameraBackend::backend_key() const noexcept
{
    return k_backend_key;
}

core::Result<std::vector<CameraDescriptor>> UvcCameraBackend::enumerate(const core::Deadline& deadline)
{
    return impl_->enumerate(deadline);
}

core::Result<void> UvcCameraBackend::open(const CameraDescriptor& descriptor, const CameraSettings& settings,
                                          const core::Deadline& deadline)
{
    return impl_->open(descriptor, settings, deadline);
}

core::Result<CapturedFrame> UvcCameraBackend::capture(const core::Deadline& deadline)
{
    return impl_->capture(deadline);
}

core::Result<void> UvcCameraBackend::reconnect(const CameraSettings& settings, const core::Deadline& deadline)
{
    return impl_->reconnect(settings, deadline);
}

void UvcCameraBackend::close() noexcept
{
    impl_->close();
}

}  // namespace cvforwin::camera

#endif /* defined(_WIN32) */
