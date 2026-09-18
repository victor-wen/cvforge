/*
 * Production ArtifactSink backed by the managed CaptureStore.
 *
 * write_temp() encodes the frame as PNG into a same-root temporary file;
 * commit() publishes it by an atomic rename to the sanitized final name; and
 * discard() removes a temporary that will not be published. Identifiers are
 * remembered per temporary path so commit() can rebuild the final name without
 * carrying the frame again, while root containment and sanitization stay in
 * CaptureStore.
 */

#ifndef CVFORWIN_SRC_ARTIFACTS_CAPTURE_SINK_H_
#define CVFORWIN_SRC_ARTIFACTS_CAPTURE_SINK_H_

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include "artifacts/capture_store.h"
#include "artifacts/save_worker.h"
#include "core/result.h"

namespace cvforwin::artifacts {

class CaptureStoreSink final : public ArtifactSink {
public:
    explicit CaptureStoreSink(CaptureStore& store) noexcept
        : store_(store)
    {
    }

    core::Result<std::filesystem::path> write_temp(const SaveJob& job) override;
    core::Result<std::filesystem::path> commit(const std::filesystem::path& temp) override;
    void discard(const std::filesystem::path& temp) noexcept override;

private:
    struct Identity {
        std::string recipe_id;
        std::string request_id;
        std::uint64_t sequence = 0;
    };

    CaptureStore& store_;
    std::mutex mutex_;
    std::map<std::string, Identity> pending_;
};

}  // namespace cvforwin::artifacts

#endif /* CVFORWIN_SRC_ARTIFACTS_CAPTURE_SINK_H_ */
