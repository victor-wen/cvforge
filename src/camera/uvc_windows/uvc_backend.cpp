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

#include "camera/uvc_windows/uvc_command_worker.h"
#include "camera/uvc_windows/uvc_frame_convert.h"
#include "camera/uvc_windows/uvc_frame_math.h"
#include "camera/uvc_windows/uvc_stream_generation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
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
/* Construction/startup budget; the worker resolves COM/MF startup within it. */
constexpr std::uint32_t k_worker_start_timeout_ms = 30000;

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

/*
 * Maps a Media Foundation uncompressed subtype to the frozen seam's native
 * format. Returns false for L8, which the seam does not model and which keeps
 * its single-channel legacy conversion.
 */
bool native_format_for_subtype(const GUID& subtype, uvc::NativePixelFormat& format) noexcept
{
    if (subtype_is(subtype, MFVideoFormat_RGB24)) {
        format = uvc::NativePixelFormat::rgb24;
    } else if (subtype_is(subtype, MFVideoFormat_RGB32)) {
        format = uvc::NativePixelFormat::rgb32;
    } else if (subtype_is(subtype, MFVideoFormat_ARGB32)) {
        format = uvc::NativePixelFormat::argb32;
    } else if (subtype_is(subtype, MFVideoFormat_YUY2)) {
        format = uvc::NativePixelFormat::yuy2;
    } else if (subtype_is(subtype, MFVideoFormat_UYVY)) {
        format = uvc::NativePixelFormat::uyvy;
    } else if (subtype_is(subtype, MFVideoFormat_NV12)) {
        format = uvc::NativePixelFormat::nv12;
    } else if (subtype_is(subtype, MFVideoFormat_I420)) {
        format = uvc::NativePixelFormat::i420;
    } else if (subtype_is(subtype, MFVideoFormat_YV12)) {
        format = uvc::NativePixelFormat::yv12;
    } else {
        return false;
    }
    return true;
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
 * One locked 2D-buffer view obtained with the documented preference order:
 * IMF2DBuffer2::Lock2DSize (which also yields the accessible bounds), then
 * IMF2DBuffer::Lock2D. The caller must call IMF2DBuffer::Unlock2D exactly once
 * when the lock succeeded.
 *
 * Lock2DSize reports its accessible range as (buffer_start, buffer_length),
 * where buffer_length is measured from buffer_start. The seam measures
 * accessible_bytes from the lowest addressed byte of the scanned region
 * instead, so buffer_start/buffer_length are kept raw here and rebased by
 * accessible_extent_from_lock_start() once the frame height is known. The
 * Lock2D path cannot report a range, so its bounds stay unknown.
 */
struct Locked2DView {
    BYTE* scanline0 = nullptr; /* visual top row */
    LONG pitch = 0; /* may be negative */
    BYTE* buffer_start = nullptr; /* Lock2DSize reported range start; null for Lock2D */
    std::size_t buffer_length = 0; /* Lock2DSize reported length from buffer_start */
    bool accessible_bytes_known = false;
};

HRESULT lock_2d_buffer(IMF2DBuffer* buffer_2d, Locked2DView& view)
{
    ComPtr<IMF2DBuffer2> buffer_2d_2;
    if (SUCCEEDED(buffer_2d->QueryInterface(__uuidof(IMF2DBuffer2), reinterpret_cast<void**>(buffer_2d_2.put())))) {
        BYTE* buffer_start = nullptr;
        DWORD buffer_length = 0;
        const HRESULT result =
            buffer_2d_2->Lock2DSize(MF2DBuffer_LockFlags_Read, &view.scanline0, &view.pitch, &buffer_start,
                                    &buffer_length);
        if (SUCCEEDED(result)) {
            view.buffer_start = buffer_start;
            view.buffer_length = static_cast<std::size_t>(buffer_length);
            view.accessible_bytes_known = true;
            return S_OK;
        }
    }
    /*
     * IMF2DBuffer::Lock2D cannot report the accessible length (the
     * GetContiguousLength value Microsoft documents as inapplicable to the
     * Lock2D view), so the bounds are left unknown and only structural checks
     * apply.
     */
    view.buffer_start = nullptr;
    view.buffer_length = 0;
    view.accessible_bytes_known = false;
    return buffer_2d->Lock2D(&view.scanline0, &view.pitch);
}

/*
 * Rebases a Lock2DSize-reported (buffer_start, buffer_length) range onto the
 * lowest addressed byte of the scanned region and returns the resulting
 * accessible byte count. Returns false when the arithmetic is unrepresentable
 * or the reported range does not strictly contain the addressed region, so the
 * caller can fail the capture instead of trusting the lock's numbers.
 * The pure arithmetic is covered by the portable uvc_frame_math.h helpers.
 */
bool accessible_extent_from_lock_start(const Locked2DView& view, std::uint32_t height, std::size_t& accessible)
{
    /* A non-null length from a null start is not a usable range. */
    if (view.buffer_start == nullptr) {
        return false;
    }
    return uvc::detail::lock_accessible_extent(reinterpret_cast<std::size_t>(view.scanline0),
                                               static_cast<std::int32_t>(view.pitch), height,
                                               reinterpret_cast<std::size_t>(view.buffer_start), view.buffer_length,
                                               accessible);
}

/* Minimum packed stride for a subtype the IMFMediaBuffer::Lock fallback uses. */
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

/*
 * Normalizes a converted BGR8 frame to the configured capture size when one is
 * requested. The frame is already owned and continuous from the seam.
 */
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

/*
 * Media Foundation L8 (8-bit luminance) is the only supported native subtype
 * the frozen seam does not model, so it keeps a dedicated single-plane
 * conversion. Every other supported subtype - packed RGB24/RGB32/ARGB32/YUY2/
 * UYVY and planar NV12/I420/YV12 - is decoded by
 * uvc::decode_locked_buffer_to_bgr8, so the seam and production share one
 * row-addressing and accessible-bounds-validating path (FR-019).
 */
cv::Mat copy_luma_rows(const BYTE* data, LONG stride, std::uint32_t width, std::uint32_t height)
{
    const int rows = static_cast<int>(height);
    const int columns = static_cast<int>(width);
    cv::Mat luma(rows, columns, CV_8UC1);
    const std::ptrdiff_t row_stride = static_cast<std::ptrdiff_t>(stride);
    for (int row = 0; row < rows; ++row) {
        std::memcpy(luma.ptr(row), data + static_cast<std::ptrdiff_t>(row) * row_stride,
                    static_cast<std::size_t>(columns));
    }
    return luma;
}

core::Result<cv::Mat> convert_luma_buffer(const BYTE* data, LONG stride, std::size_t available_bytes,
                                          std::uint32_t width, std::uint32_t height, const CameraSettings& settings)
{
    if (data == nullptr || width == 0 || height == 0) {
        return camera_failure(core::ErrorCode::capture_failed, "the Media Foundation sample has no video data");
    }
    if (width > k_max_frame_dimension || height > k_max_frame_dimension) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "the Media Foundation frame exceeds the supported dimension limit");
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width);
    const std::size_t magnitude =
        stride < 0 ? static_cast<std::size_t>(-static_cast<long long>(stride)) : static_cast<std::size_t>(stride);
    if (magnitude < row_bytes) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "the Media Foundation sample stride is smaller than one row");
    }
    const std::size_t rows = static_cast<std::size_t>(height);
    if (rows - 1u > (std::numeric_limits<std::size_t>::max() - row_bytes) / magnitude) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "the Media Foundation sample extent overflows the addressable range");
    }
    const std::size_t extent = (rows - 1u) * magnitude + row_bytes;
    /* Zero means the buffer could not report a length; structural checks still apply. */
    if (available_bytes != 0 && available_bytes < extent) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "the Media Foundation sample is smaller than the required frame extent");
    }
    try {
        const cv::Mat luma = copy_luma_rows(data, stride, width, height);
        cv::Mat pixels;
        cv::cvtColor(luma, pixels, cv::COLOR_GRAY2BGR);
        return normalize_frame_size(std::move(pixels), settings);
    } catch (const cv::Exception&) {
        return camera_failure(core::ErrorCode::capture_failed,
                              "OpenCV failed to convert the Media Foundation luminance frame to BGR8");
    }
}

/*
 * Converts one locked Media Foundation sample to BGR8 through the frozen seam so
 * the seam and production share one row-addressing and bounds path. The pitch is
 * applied exactly once with no row-index reversal, and the accessible bounds are
 * enforced whenever the lock reported them.
 */
core::Result<cv::Mat> convert_locked_to_bgr8(const BYTE* data, LONG stride, bool accessible_known,
                                             std::size_t accessible_bytes, const GUID& subtype, std::uint32_t width,
                                             std::uint32_t height, const CameraSettings& settings)
{
    uvc::NativePixelFormat native_format = uvc::NativePixelFormat::rgb24;
    if (!native_format_for_subtype(subtype, native_format)) {
        return convert_luma_buffer(data, stride, accessible_bytes, width, height, settings);
    }
    uvc::Locked2DBufferView view;
    view.scanline0 = data;
    view.pitch = static_cast<std::int32_t>(stride);
    view.width = width;
    view.height = height;
    view.format = native_format;
    view.accessible_bytes = accessible_bytes;
    view.accessible_bytes_known = accessible_known;
    core::Result<cv::Mat> pixels = uvc::decode_locked_buffer_to_bgr8(view);
    if (!pixels.has_value()) {
        return pixels.failure();
    }
    return normalize_frame_size(std::move(pixels).value(), settings);
}

/* ---------------------------------------------------------------------------
 * Asynchronous source-reader callback.
 * ------------------------------------------------------------------------- */

/*
 * Shared, reference-counted stream-generation state. The backend and every
 * callback it hands to Media Foundation own a reference, so a late callback
 * from a retired SourceReader always consults live state and never a dangling
 * pointer to a destroyed backend. StreamGenerationGate is internally
 * synchronized, so accepts()/discard_stale() are safe on Media Foundation
 * work-queue threads while begin_stream()/retire() run on the owned worker.
 */
struct StreamGenerationState {
    uvc::StreamGenerationGate gate;
    std::atomic<std::uint64_t> callbacks_created{0};
};

/*
 * Carries exactly one completed read or flush to the waiting capture call.
 * Media Foundation may invoke the callback from any thread, so every field is
 * guarded by the mutex and the waiter is woken through the condition variable.
 * Each callback belongs to exactly one SourceReader and carries that stream's
 * immutable generation; an event from a retired generation is counted as stale
 * and never touches the completion state, so it cannot satisfy a replacement.
 */
class ReadCallback final : public IMFSourceReaderCallback {
public:
    ReadCallback(std::shared_ptr<StreamGenerationState> generation_state, std::uint64_t generation)
        : generation_state_(std::move(generation_state)), generation_(generation)
    {
    }
    /*
     * IMFSourceReaderCallback is a COM interface: IUnknown has no virtual
     * destructor, so an override specifier here is invalid (MSVC C3668).
     */
    ~ReadCallback() = default;

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
        /*
         * A retired callback never writes completion state: the gate check
         * precedes every field, so the rejected event can neither produce a
         * frame nor falsely satisfy the capture waiter.
         */
        if (!generation_state_->gate.accepts(generation_)) {
            generation_state_->gate.discard_stale(generation_);
            return S_OK;
        }
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
        /* A stale flush completion must never report a false flush success. */
        if (!generation_state_->gate.accepts(generation_)) {
            generation_state_->gate.discard_stale(generation_);
            return S_OK;
        }
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

    /* Immutable generation of the SourceReader that owns this callback. */
    std::uint64_t generation() const noexcept
    {
        return generation_;
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
    std::shared_ptr<StreamGenerationState> generation_state_;
    const std::uint64_t generation_;
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
    /*
     * The backend owns one worker for its whole lifetime. The constructor asks
     * the worker to initialize COM as MTA and run one MFStartup; a failed
     * startup leaves no running worker and the balanced teardown below runs on
     * the worker before it exits. Construction never touches the caller's
     * apartment.
     */
    Impl()
    {
        (void)worker_.start(core::Deadline::from_timeout_ms(k_worker_start_timeout_ms), [this] { return startup_worker(); }, [this] { teardown_worker(); });
    }

    ~Impl()
    {
        close();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    core::Result<std::vector<CameraDescriptor>> enumerate(const core::Deadline& deadline)
    {
        record_caller();
        if (!worker_.running()) {
            return worker_unavailable_failure("the camera enumeration worker is not running");
        }
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                      "the camera enumeration deadline expired before enumeration started");
        }
        auto outcome = std::make_shared<core::Result<std::vector<CameraDescriptor>>>(placeholder<std::vector<CameraDescriptor>>());
        const uvc::CommandWorker::SubmitState state = worker_.submit(deadline, [this, outcome] {
            try {
                core::Result<Enumeration> enumerated = enumerate_devices();
                if (!enumerated.has_value()) {
                    *outcome = enumerated.failure();
                    return;
                }
                Enumeration devices = std::move(enumerated).value();
                *outcome = std::move(devices.descriptors);
            } catch (const std::exception&) {
                *outcome = internal_failure();
            } catch (...) {
                *outcome = internal_failure();
            }
        });
        return finalize(outcome, state, core::ErrorCode::deadline_expired,
                        "the camera enumeration deadline expired before enumeration completed");
    }

    core::Result<void> open(const CameraDescriptor& descriptor, const CameraSettings& settings,
                            const core::Deadline& deadline)
    {
        record_caller();
        if (!worker_.running()) {
            return worker_unavailable_failure("the camera open worker is not running");
        }
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                      "the camera open deadline expired before the device was activated");
        }
        auto outcome = std::make_shared<core::Result<void>>(placeholder<void>());
        const uvc::CommandWorker::SubmitState state = worker_.submit(deadline, [this, descriptor, settings, outcome, deadline] {
            try {
                teardown_stream();
                if (deadline.expired()) {
                    *outcome = core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                                  "the camera open deadline expired before the device was activated");
                    return;
                }
                const core::Result<void> activated = activate(descriptor, settings, deadline, false);
                if (!activated.has_value()) {
                    /* Release any partial Media Foundation objects on the worker. */
                    teardown_stream();
                    *outcome = activated.failure();
                    return;
                }
                *outcome = core::Result<void>{};
            } catch (const std::exception&) {
                teardown_stream();
                *outcome = internal_failure();
            } catch (...) {
                teardown_stream();
                *outcome = internal_failure();
            }
        });
        return finalize(outcome, state, core::ErrorCode::deadline_expired,
                        "the camera open deadline expired before the device was activated");
    }

    core::Result<CapturedFrame> capture(const core::Deadline& deadline)
    {
        record_caller();
        if (!worker_.running()) {
            return worker_unavailable_failure("the camera capture worker is not running");
        }
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::capture_timed_out,
                                      "the capture deadline expired before the frame request was issued");
        }
        auto outcome = std::make_shared<core::Result<CapturedFrame>>(placeholder<CapturedFrame>());
        const uvc::CommandWorker::SubmitState state = worker_.submit(deadline, [this, outcome, deadline] {
            try {
                *outcome = capture_or_failure(deadline);
            } catch (const std::exception&) {
                *outcome = internal_failure();
            } catch (...) {
                *outcome = internal_failure();
            }
        });
        return finalize(outcome, state, core::ErrorCode::capture_timed_out,
                        "the capture deadline expired before the frame request completed");
    }

    core::Result<void> reconnect(const CameraSettings& settings, const core::Deadline& deadline)
    {
        record_caller();
        if (!worker_.running()) {
            return worker_unavailable_failure("the camera reconnect worker is not running");
        }
        if (deadline.expired()) {
            return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                      "the camera reconnect deadline expired before the device was re-activated");
        }
        auto outcome = std::make_shared<core::Result<void>>(placeholder<void>());
        const uvc::CommandWorker::SubmitState state =
            worker_.submit(deadline, [this, settings, outcome, deadline] {
                try {
                    if (!has_identity()) {
                        *outcome = camera_failure(core::ErrorCode::camera_not_open,
                                                  "the UVC camera has never been opened, so there is nothing to reconnect");
                        return;
                    }
                    const CameraDescriptor descriptor = descriptor_;
                    teardown_stream();
                    if (deadline.expired()) {
                        *outcome = core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                                      "the camera reconnect deadline expired before the device was re-activated");
                        return;
                    }
                    const core::Result<void> activated = activate(descriptor, settings, deadline, true);
                    if (!activated.has_value()) {
                        teardown_stream();
                        *outcome = activated.failure();
                        return;
                    }
                    *outcome = core::Result<void>{};
                } catch (const std::exception&) {
                    teardown_stream();
                    *outcome = internal_failure();
                } catch (...) {
                    teardown_stream();
                    *outcome = internal_failure();
                }
            });
        return finalize(outcome, state, core::ErrorCode::deadline_expired,
                        "the camera reconnect deadline expired before the device was re-activated");
    }

    /*
     * Idempotent, safe on a never-opened backend and from any host thread.
     * Stream references are released on the worker, then MFShutdown and
     * CoUninitialize run in reverse order on the worker before it is joined.
     */
    void close() noexcept
    {
        worker_.stop_and_join();
        sequence_ = 0;
    }

    UvcWorkerSnapshot snapshot() const noexcept
    {
        UvcWorkerSnapshot value;
        value.com_init_count = com_init_count_.load(std::memory_order_relaxed);
        value.com_uninit_count = com_uninit_count_.load(std::memory_order_relaxed);
        value.mf_startup_count = mf_startup_count_.load(std::memory_order_relaxed);
        value.mf_shutdown_count = mf_shutdown_count_.load(std::memory_order_relaxed);
        value.commands_marshalled = worker_.commands_executed();
        value.worker_thread_id = worker_thread_id_.load(std::memory_order_relaxed);
        value.last_caller_thread_id = last_caller_thread_id_.load(std::memory_order_relaxed);
        value.worker_running = worker_.running();
        return value;
    }

    /*
     * Read-only callback diagnostics. Every value comes from the shared,
     * internally synchronized generation state, so this is safe from any host
     * thread at any time and never touches a Media Foundation object.
     */
    UvcCallbackSnapshot callback_snapshot() const noexcept
    {
        UvcCallbackSnapshot value;
        value.active_generation = generation_state_->gate.active_generation();
        value.callbacks_created = generation_state_->callbacks_created.load(std::memory_order_relaxed);
        value.stale_events_discarded = generation_state_->gate.stale_events_discarded();
        value.stream_open = stream_open_.load(std::memory_order_relaxed);
        return value;
    }

private:
    struct Enumeration {
        std::vector<CameraDescriptor> descriptors;
        std::vector<ComPtr<IMFActivate>> activations;
    };

    /* A placeholder failure that is observable only when a command never ran. */
    template <typename T>
    static core::Result<T> placeholder()
    {
        return core::make_failure(core::Status::internal_error, core::ErrorCode::internal_unexpected,
                                  "the Media Foundation worker did not complete the command");
    }

    static core::Failure worker_unavailable_failure(std::string message)
    {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::capture_failed, std::move(message));
    }

    void record_caller() noexcept
    {
        last_caller_thread_id_.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
    }

    /* Runs on the worker before it serves commands. */
    bool startup_worker() noexcept
    {
        worker_thread_id_.store(static_cast<std::uint32_t>(::GetCurrentThreadId()), std::memory_order_relaxed);
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(com)) {
            return false;
        }
        com_initialized_ = true;
        com_init_count_.fetch_add(1, std::memory_order_relaxed);
        const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(mf)) {
            /* The teardown hook balances COM even when MF startup fails. */
            return false;
        }
        mf_started_ = true;
        mf_startup_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    /*
     * Runs on the worker after the command loop ends (including after a failed
     * startup). Teardown order: release reader/source/callback, MFShutdown,
     * CoUninitialize.
     */
    void teardown_worker() noexcept
    {
        teardown_stream();
        if (mf_started_) {
            MFShutdown();
            mf_started_ = false;
            mf_shutdown_count_.fetch_add(1, std::memory_order_relaxed);
        }
        if (com_initialized_) {
            ::CoUninitialize();
            com_initialized_ = false;
            com_uninit_count_.fetch_add(1, std::memory_order_relaxed);
        }
        worker_thread_id_.store(0, std::memory_order_relaxed);
    }

    template <typename T>
    core::Result<T> finalize(const std::shared_ptr<core::Result<T>>& outcome, uvc::CommandWorker::SubmitState state,
                             core::ErrorCode timeout_code, std::string stopped_message) const
    {
        if (state == uvc::CommandWorker::SubmitState::completed) {
            return std::move(*outcome);
        }
        if (state == uvc::CommandWorker::SubmitState::unavailable) {
            return worker_unavailable_failure(std::move(stopped_message));
        }
        return core::make_failure(core::Status::timeout, timeout_code,
                                  "the operation did not complete before its deadline");
    }

    bool has_identity() const noexcept
    {
        return !descriptor_.device_path.empty() || !descriptor_.vendor_id.empty() || !descriptor_.product_id.empty() ||
               !descriptor_.friendly_name.empty();
    }

    void teardown_stream() noexcept
    {
        /*
         * Retire the generation before releasing the reader/source/callback so a
         * delayed event from this stream can never satisfy a replacement: once
         * retired, every further callback for it is stale and discarded.
         */
        generation_state_->gate.retire();
        stream_open_.store(false, std::memory_order_relaxed);
        reader_.reset();
        source_.reset();
        callback_.reset();
        open_ = false;
        needs_reconnect_ = false;
        output_subtype_ = GUID_NULL;
        output_width_ = 0;
        output_height_ = 0;
    }

    core::Result<Enumeration> enumerate_devices()
    {
        /* COM and MF startup are owned by the worker for the whole backend lifetime. */
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

        /*
         * A fresh callback object with a fresh monotonic generation for every
         * started stream; one callback instance is never reused by a different
         * SourceReader, so a retired reader's late event cannot mutate the
         * completion state of its replacement.
         */
        const std::uint64_t generation = generation_state_->gate.begin_stream();
        callback_.reset(new ReadCallback(generation_state_, generation));
        generation_state_->callbacks_created.fetch_add(1, std::memory_order_relaxed);
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
            teardown_stream();
            return selected.failure();
        }
        stream_open_.store(true, std::memory_order_relaxed);
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

        /*
         * Hold the stream's callback for the whole capture so its lifetime is
         * independent of the member pointer, and read its immutable generation.
         * A completion satisfies the waiter only while that generation is still
         * the gate's active generation.
         */
        ComPtr<ReadCallback> callback;
        callback.copy_from(callback_.get());
        const std::uint64_t generation = callback->generation();

        while (true) {
            {
                std::lock_guard<std::mutex> lock(callback->mutex());
                callback->begin_read();
            }
            const HRESULT issued =
                reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr,
                                    nullptr, nullptr);
            if (FAILED(issued)) {
                needs_reconnect_ = true;
                return hresult_failure("IMFSourceReader::ReadSample", issued, core::ErrorCode::capture_failed);
            }

            std::unique_lock<std::mutex> lock(callback->mutex());
            const bool completed = callback->condition().wait_for(lock, deadline.remaining(), [this, &callback, generation] {
                return callback->read_completed() && generation_state_->gate.accepts(generation);
            });
            if (!completed) {
                lock.unlock();
                if (!cancel_pending_read(deadline, callback.get(), generation)) {
                    needs_reconnect_ = true;
                }
                return core::make_failure(core::Status::timeout, core::ErrorCode::capture_timed_out,
                                          "the capture deadline expired before a Media Foundation sample arrived");
            }

            const HRESULT sample_status = callback->read_status();
            const DWORD sample_flags = callback->read_flags();
            ComPtr<IMFSample> sample;
            sample.copy_from(callback->read_sample());
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
                core::Result<cv::Mat> converted = convert_sample(sample.get());
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
    bool cancel_pending_read(const core::Deadline& deadline, ReadCallback* callback, std::uint64_t generation)
    {
        {
            std::lock_guard<std::mutex> lock(callback->mutex());
            callback->begin_flush();
        }
        const HRESULT result =
            reader_->Flush(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM));
        if (FAILED(result)) {
            return false;
        }
        const std::chrono::milliseconds budget = std::min(k_flush_grace, deadline.remaining());
        if (budget <= std::chrono::milliseconds::zero()) {
            std::lock_guard<std::mutex> lock(callback->mutex());
            callback->begin_read();
            return false;
        }
        std::unique_lock<std::mutex> lock(callback->mutex());
        const bool flushed = callback->condition().wait_for(lock, budget, [this, callback, generation] {
            return callback->flush_completed() && generation_state_->gate.accepts(generation);
        });
        /* A late completion from the cancelled read must not leak into the next capture. */
        callback->begin_read();
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
            Locked2DView locked;
            result = lock_2d_buffer(buffer_2d.get(), locked);
            if (FAILED(result)) {
                return hresult_failure("IMF2DBuffer lock", result, core::ErrorCode::capture_failed);
            }
            /*
             * Lock2DSize reports its length from the lock's start pointer; the
             * seam measures accessible_bytes from the lowest addressed byte of
             * the scanned region, so rebase it and refuse a range that does not
             * actually contain that region instead of trusting the lock.
             */
            std::size_t accessible_bytes = 0;
            if (locked.accessible_bytes_known && locked.buffer_length == 0u) {
                buffer_2d->Unlock2D();
                return camera_failure(core::ErrorCode::capture_failed,
                                      "the Media Foundation sample buffer is empty");
            }
            if (locked.accessible_bytes_known &&
                !accessible_extent_from_lock_start(locked, output_height_, accessible_bytes)) {
                buffer_2d->Unlock2D();
                return camera_failure(core::ErrorCode::capture_failed,
                                      "the locked Media Foundation buffer range does not contain the scanned region");
            }
            core::Result<cv::Mat> pixels =
                convert_locked_to_bgr8(locked.scanline0, locked.pitch, locked.accessible_bytes_known,
                                       accessible_bytes, output_subtype_, output_width_, output_height_, settings_);
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
                : convert_locked_to_bgr8(data, packed_stride, true, static_cast<std::size_t>(current_length),
                                         output_subtype_, output_width_, output_height_, settings_);
        buffer->Unlock();
        return pixels;
    }

    /* The dedicated worker that owns COM, Media Foundation, and every held interface. */
    uvc::CommandWorker worker_;
    /*
     * Shared with every ReadCallback so a callback kept alive by a Media
     * Foundation work-queue thread consults live state without a raw pointer to
     * this backend. Created once and never replaced, so callback_snapshot() can
     * read it from any host thread.
     */
    std::shared_ptr<StreamGenerationState> generation_state_{std::make_shared<StreamGenerationState>()};
    std::atomic<std::uint64_t> com_init_count_{0};
    std::atomic<std::uint64_t> com_uninit_count_{0};
    std::atomic<std::uint64_t> mf_startup_count_{0};
    std::atomic<std::uint64_t> mf_shutdown_count_{0};
    std::atomic<std::uint32_t> worker_thread_id_{0};
    std::atomic<std::uint32_t> last_caller_thread_id_{0};
    /* Worker-thread-only lifetime flags (COM/MF owned by the worker). */
    bool com_initialized_ = false;
    bool mf_started_ = false;
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
    /* Cross-thread mirror of open_ for callback_snapshot(); worker writes only. */
    std::atomic<bool> stream_open_{false};
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

UvcWorkerSnapshot UvcCameraBackend::worker_snapshot() const noexcept
{
    return impl_->snapshot();
}

UvcCallbackSnapshot UvcCameraBackend::callback_snapshot() const noexcept
{
    return impl_->callback_snapshot();
}

}  // namespace cvforwin::camera

#endif /* defined(_WIN32) */
