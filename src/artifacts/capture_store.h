/*
 * Managed capture persistence.
 *
 * CaptureStore owns one absolute captures root. save() encodes a frame as PNG
 * and writes it as a direct child of that root using a sanitized, bounded file
 * name, so an identifier can never form a path or escape the managed root.
 * enforce_retention() is the bounded housekeeping primitive: it deletes oldest
 * first, never follows a link or junction, and reports the number of deleted
 * files.
 *
 * Save failures are required-artifact failures for the runtime; this module
 * reports them as precise internal failures and performs no verdict mapping.
 */

#ifndef CVFORWIN_SRC_ARTIFACTS_CAPTURE_STORE_H_
#define CVFORWIN_SRC_ARTIFACTS_CAPTURE_STORE_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include <opencv2/core.hpp>

#include "core/result.h"
#include "core/status.h"

namespace cvforwin::artifacts {

enum class SaveDecision {
    skip,
    save,
};

struct CaptureSaveRequest {
    const cv::Mat& pixels;
    std::string_view recipe_id;
    std::string_view request_id;
    std::uint64_t sequence;
};

/*
 * Maps a recipe save policy and the execution outcome onto the persistence
 * decision: "never" skips, "always" saves, "fail_or_error" saves iff the
 * execution failed or the verdict is FAIL. Unknown tokens skip.
 */
SaveDecision decide_capture_save(std::string_view save_policy, core::Verdict verdict, bool execution_ok);

class CaptureStore {
public:
    /* Creates the absolute captures root if it is missing. */
    static core::Result<std::unique_ptr<CaptureStore>> create(const std::filesystem::path& captures_root);

    /* Encodes pixels as PNG and returns the absolute path written below the root. */
    core::Result<std::filesystem::path> save(const CaptureSaveRequest& request);

    /*
     * Non-recursive age-then-size retention over regular files only; symlinks
     * and other entry types are skipped and never followed. Returns the number
     * of deleted files. A scan or delete failure reports retention_error after
     * any partial deletions.
     */
    static core::Result<std::uint32_t> enforce_retention(const std::filesystem::path& captures_root,
                                                         std::chrono::seconds max_age,
                                                         std::uint64_t max_total_bytes);

    ~CaptureStore();

    CaptureStore(const CaptureStore&) = delete;
    CaptureStore& operator=(const CaptureStore&) = delete;

private:
    explicit CaptureStore(std::filesystem::path captures_root);

    std::filesystem::path root_;
};

}  // namespace cvforwin::artifacts

#endif /* CVFORWIN_SRC_ARTIFACTS_CAPTURE_STORE_H_ */
