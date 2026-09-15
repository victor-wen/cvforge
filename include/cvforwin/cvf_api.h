/*
 * cvforwin public C ABI, version 1.
 *
 * This header is the complete, frozen v1 contract between the cvforwin DLL and a
 * native C host. It is self-contained C11: it includes only <stdint.h> and
 * <stddef.h> and must never require a C++ library, OpenCV, vendor, or Windows
 * header to compile. It is also includable from C++20 through the extern "C"
 * guard below.
 *
 * Ownership and lifetime:
 *   - Every request, result, string, and JSON buffer is caller-owned. The DLL
 *     never allocates a buffer that the caller must free.
 *   - The DLL owns cvf_context storage and releases it only in cvf_shutdown.
 *     The caller never dereferences or frees the opaque context.
 *   - Input pointers and the log callback must stay valid for the duration of
 *     the call that uses them; cvf_initialize copies the values it retains.
 *
 * Textual buffer rules (cvf_error_info_v1 and the text members of
 * cvf_inspection_result_v1):
 *   - capacity is the caller buffer size in bytes.
 *   - bytes_written counts UTF-8 content bytes written, excluding the trailing
 *     NUL; the buffer is NUL-terminated whenever capacity >= 1.
 *   - bytes_required reports the buffer size needed to hold the full text,
 *     including the trailing NUL.
 *   - A NULL buffer pointer is legal only with capacity 0; NULL with capacity
 *     greater than 0 is CVF_STATUS_INVALID_ARGUMENT.
 *   - Input byte lengths exclude the terminating NUL; an embedded NUL is
 *     rejected.
 *
 * Validation:
 *   - struct_size must equal the exact v1 size of that structure; otherwise
 *     CVF_STATUS_INVALID_ARGUMENT.
 *   - abi_version must equal CVF_ABI_VERSION_V1; otherwise
 *     CVF_STATUS_ABI_MISMATCH.
 *   - reserved fields must be zero; a nonzero reserved field is
 *     CVF_STATUS_INVALID_ARGUMENT.
 *   - flags must not contain unknown bits; CVF_INIT_FLAG_CALLBACK_LOGGING
 *     requires a non-NULL log_callback.
 *   - Every entry point validates its arguments before any observable side
 *     effect. A NULL or invalid context yields CVF_STATUS_INVALID_CONTEXT.
 *
 * No exception ever crosses this ABI. Execution status and product verdict are
 * independent: a technical error always reports CVF_VERDICT_NOT_EVALUATED,
 * never CVF_VERDICT_FAIL.
 */

#ifndef CVFORWIN_CVF_API_H
#define CVFORWIN_CVF_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(CVF_BUILD_SHARED)
#define CVF_API __declspec(dllexport)
#else
#define CVF_API __declspec(dllimport)
#endif
#define CVF_CALL __cdecl
#else
#if defined(__GNUC__) && __GNUC__ >= 4
#define CVF_API __attribute__((visibility("default")))
#else
#define CVF_API
#endif
#define CVF_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

#define CVF_ABI_VERSION_V1 1u

#define CVF_RECIPE_ID_MAX_UTF8_BYTES 128u
#define CVF_REQUEST_ID_MAX_UTF8_BYTES 128u
#define CVF_INPUT_JSON_MAX_UTF8_BYTES 65536u

#define CVF_RESULT_JSON_REQUIRED_CAPACITY 65536u
#define CVF_ERROR_MESSAGE_REQUIRED_CAPACITY 1024u
#define CVF_IMAGE_PATH_REQUIRED_CAPACITY 4096u

/* ------------------------------------------------------------------------- */
/* Scalar types                                                              */
/* ------------------------------------------------------------------------- */

typedef uint32_t cvf_status_t;
typedef uint32_t cvf_verdict_t;

/* Opaque, incomplete context owned by the DLL. */
typedef struct cvf_context cvf_context;

/* ------------------------------------------------------------------------- */
/* Enumerations (exposed as fixed-width constants, never as C enums)         */
/* ------------------------------------------------------------------------- */

#define CVF_STATUS_OK 0u
#define CVF_STATUS_INVALID_ARGUMENT 1u
#define CVF_STATUS_ABI_MISMATCH 2u
#define CVF_STATUS_CONTEXT_LIMIT 3u
#define CVF_STATUS_INVALID_CONTEXT 4u
#define CVF_STATUS_CONFIG_ERROR 5u
#define CVF_STATUS_RECIPE_NOT_FOUND 6u
#define CVF_STATUS_CAMERA_NOT_FOUND 7u
#define CVF_STATUS_CAMERA_IO 8u
#define CVF_STATUS_TIMEOUT 9u
#define CVF_STATUS_BUFFER_TOO_SMALL 10u
#define CVF_STATUS_ALGORITHM_ERROR 11u
#define CVF_STATUS_REQUIRED_ARTIFACT_ERROR 12u
#define CVF_STATUS_INTERNAL_ERROR 13u

#define CVF_VERDICT_NOT_EVALUATED 0u
#define CVF_VERDICT_PASS 1u
#define CVF_VERDICT_FAIL 2u

#define CVF_LOG_LEVEL_TRACE 0u
#define CVF_LOG_LEVEL_DEBUG 1u
#define CVF_LOG_LEVEL_INFO 2u
#define CVF_LOG_LEVEL_WARN 3u
#define CVF_LOG_LEVEL_ERROR 4u
#define CVF_LOG_LEVEL_CRITICAL 5u

#define CVF_INIT_FLAG_FILE_LOGGING (1u << 0)
#define CVF_INIT_FLAG_CALLBACK_LOGGING (1u << 1)

/* ------------------------------------------------------------------------- */
/* Callback                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * Synchronous log sink. message_utf8 is a NUL-terminated UTF-8 string owned by
 * the DLL and valid only for the duration of the callback. user_data is the
 * value supplied in cvf_init_options_v1 and must remain valid until
 * cvf_shutdown returns. The callback must not re-enter this context.
 */
typedef void(CVF_CALL* cvf_log_callback)(uint32_t level, const char* message_utf8, void* user_data);

/* ------------------------------------------------------------------------- */
/* Versioned structures                                                      */
/* ------------------------------------------------------------------------- */

typedef struct cvf_init_options_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* config_root_utf8;
    uint32_t config_root_utf8_bytes;
    const char* output_root_utf8;
    uint32_t output_root_utf8_bytes;
    uint32_t flags;
    cvf_log_callback log_callback;
    void* user_data;
    uint32_t reserved[8];
} cvf_init_options_v1;

typedef struct cvf_error_info_v1 {
    uint32_t struct_size;
    uint32_t error_code;
    char* message_utf8;
    uint32_t message_capacity;
    uint32_t message_bytes_written;
    uint32_t message_bytes_required;
    uint32_t reserved[4];
} cvf_error_info_v1;

typedef struct cvf_inspection_request_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* recipe_id_utf8;
    uint32_t recipe_id_utf8_bytes;
    const char* request_id_utf8;
    uint32_t request_id_utf8_bytes;
    uint32_t timeout_ms;
    const char* input_json_utf8;
    uint32_t input_json_utf8_bytes;
    uint32_t reserved[8];
} cvf_inspection_request_v1;

typedef struct cvf_inspection_result_v1 {
    uint32_t struct_size;
    cvf_status_t status;
    cvf_verdict_t verdict;
    uint32_t error_code;
    uint32_t warning_flags;
    uint32_t elapsed_ms;
    char* output_json;
    uint32_t output_json_capacity;
    uint32_t output_json_bytes_written;
    uint32_t output_json_bytes_required;
    char* image_path;
    uint32_t image_path_capacity;
    uint32_t image_path_bytes_written;
    uint32_t image_path_bytes_required;
    char* error_message;
    uint32_t error_message_capacity;
    uint32_t error_message_bytes_written;
    uint32_t error_message_bytes_required;
    uint32_t reserved[8];
} cvf_inspection_result_v1;

/* ------------------------------------------------------------------------- */
/* Functions                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Returns CVF_ABI_VERSION_V1. Requires neither a context nor initialization and
 * has no side effects.
 */
CVF_API uint32_t CVF_CALL cvf_get_abi_version(void);

/*
 * Validates options, loads global configuration and all recipes, creates
 * diagnostics, resolves and opens one camera, and publishes a context only
 * after complete success. On any failure *out_context stays NULL.
 * The error parameter is required and must be a writable v1 error structure.
 */
CVF_API cvf_status_t CVF_CALL cvf_initialize(const cvf_init_options_v1* options, cvf_context** out_context,
                                             cvf_error_info_v1* error);

/*
 * Validates a complete candidate recipe set and atomically replaces the active
 * snapshot. On any error the previous snapshot stays active. The error
 * parameter is required and must be a writable v1 error structure.
 */
CVF_API cvf_status_t CVF_CALL cvf_reload_recipes(cvf_context* context, cvf_error_info_v1* error);

/*
 * Performs one normal inspection cycle: queueing, acquisition, inspection,
 * result encoding, and requested persistence, all under the end-to-end
 * deadline. A non-OK return mirrors into result->status and always leaves
 * result->verdict at CVF_VERDICT_NOT_EVALUATED.
 */
CVF_API cvf_status_t CVF_CALL cvf_inspect(cvf_context* context, const cvf_inspection_request_v1* request,
                                          cvf_inspection_result_v1* result);

/*
 * Closes resources and invalidates the opaque context. The host must stop new
 * calls and let active calls finish before calling cvf_shutdown.
 */
CVF_API cvf_status_t CVF_CALL cvf_shutdown(cvf_context* context);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CVFORWIN_CVF_API_H */
