#include "artifacts/capture_sink.h"

#include <system_error>
#include <utility>

#include "core/status.h"

namespace cvforwin::artifacts {

core::Result<std::filesystem::path> CaptureStoreSink::write_temp(const SaveJob& job)
{
    const CaptureSaveRequest request{job.pixels, job.recipe_id, job.request_id, job.sequence};
    core::Result<std::filesystem::path> temporary = store_.save_temporary(request);
    if (!temporary.has_value()) {
        return temporary.failure();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[temporary.value().string()] =
            Identity{job.recipe_id, job.request_id, job.sequence};
    }
    return temporary.value();
}

core::Result<std::filesystem::path> CaptureStoreSink::commit(const std::filesystem::path& temp)
{
    Identity identity;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = pending_.find(temp.string());
        if (found == pending_.end()) {
            return core::make_failure(core::Status::internal_error,
                                      core::ErrorCode::image_write_error,
                                      "temporary capture is not registered for commit");
        }
        identity = found->second;
        pending_.erase(found);
    }

    const cv::Mat empty;
    const CaptureSaveRequest request{empty, identity.recipe_id, identity.request_id, identity.sequence};
    return store_.publish_temporary(temp, request);
}

void CaptureStoreSink::discard(const std::filesystem::path& temp) noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(temp.string());
    }
    std::error_code error;
    std::filesystem::remove(temp, error);
}

}  // namespace cvforwin::artifacts
