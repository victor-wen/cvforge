#include "inspection/registry.h"

#include <exception>
#include <utility>

#include "core/error.h"

namespace cvforwin::inspection {

namespace {

constexpr std::size_t k_max_key_length = 64;

bool is_valid_key(std::string_view key) noexcept
{
    if (key.empty() || key.size() > k_max_key_length) {
        return false;
    }
    for (const char character : key) {
        const bool valid = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                           character == '.' || character == '_' || character == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

core::Failure duplicate_key(std::string_view key)
{
    return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_duplicate_key,
                              "algorithm key already registered: " + std::string(key));
}

core::Failure key_invalid(std::string_view reason)
{
    return core::make_failure(core::Status::invalid_argument, core::ErrorCode::algorithm_key_invalid,
                              std::string(reason));
}

}  // namespace

core::Result<void> AlgorithmRegistry::add(std::unique_ptr<IInspectionAlgorithm> algorithm)
{
    if (!algorithm) {
        return key_invalid("algorithm registry cannot register a null algorithm");
    }

    const std::string_view key = algorithm->key();
    if (!is_valid_key(key)) {
        return key_invalid("algorithm key must match [a-z0-9._-]{1,64}");
    }
    for (const auto& existing : algorithms_) {
        if (existing->key() == key) {
            return duplicate_key(key);
        }
    }

    algorithms_.push_back(std::move(algorithm));
    return core::Result<void>{};
}

core::Result<const IInspectionAlgorithm*> AlgorithmRegistry::find(std::string_view key) const
{
    for (const auto& algorithm : algorithms_) {
        if (algorithm->key() == key) {
            return core::Result<const IInspectionAlgorithm*>{algorithm.get()};
        }
    }
    return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_not_found,
                              "no algorithm registered for key: " + std::string(key));
}

std::vector<std::string> AlgorithmRegistry::keys() const
{
    std::vector<std::string> registered;
    registered.reserve(algorithms_.size());
    for (const auto& algorithm : algorithms_) {
        registered.emplace_back(algorithm->key());
    }
    return registered;
}

std::size_t AlgorithmRegistry::size() const noexcept
{
    return algorithms_.size();
}

bool AlgorithmRegistry::empty() const noexcept
{
    return algorithms_.empty();
}

core::Result<AlgorithmResult> dispatch(const IInspectionAlgorithm& algorithm, const AlgorithmRequest& request)
{
    if (request.deadline.expired()) {
        return core::make_failure(core::Status::timeout, core::ErrorCode::algorithm_deadline_exceeded,
                                  "algorithm dispatch deadline expired before invocation");
    }
    if (request.frame.pixels.empty()) {
        return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_frame_invalid,
                                  "algorithm dispatch requires a frame with pixels");
    }

    try {
        /*
         * dispatch() accepts a const reference for a read-only call surface,
         * while the frozen contract keeps inspect() non-const. The registry
         * only ever stores non-const algorithm objects, so removing the
         * caller-side constness here is well-defined.
         */
        core::Result<AlgorithmResult> outcome = const_cast<IInspectionAlgorithm&>(algorithm).inspect(request);
        if (!outcome.has_value()) {
            return outcome.failure();
        }

        AlgorithmResult result = std::move(outcome).value();
        const std::size_t serialized_bytes = result.measurements.dump().size() + result.defects.dump().size();
        if (serialized_bytes > k_max_result_json_bytes) {
            return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_output_too_large,
                                      "algorithm result exceeds the " + std::to_string(k_max_result_json_bytes) +
                                          "-byte JSON bound");
        }
        return result;
    } catch (const std::exception& exception) {
        return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_exception,
                                  std::string("algorithm threw an exception: ") + exception.what());
    } catch (...) {
        return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_exception,
                                  "algorithm threw an unknown exception");
    }
}

core::Result<AlgorithmResult> dispatch(const IPreparedAlgorithm& prepared, const AlgorithmRequest& request)
{
    if (request.deadline.expired()) {
        return core::make_failure(core::Status::timeout, core::ErrorCode::algorithm_deadline_exceeded,
                                  "algorithm dispatch deadline expired before invocation");
    }
    if (request.frame.pixels.empty()) {
        return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_frame_invalid,
                                  "algorithm dispatch requires a frame with pixels");
    }

    try {
        /*
         * IPreparedAlgorithm::inspect() is non-const in the frozen contract,
         * while dispatch() accepts a const reference for a read-only call
         * surface. Prepared objects are published once and never mutated during
         * inspect, so removing the caller-side constness here is well-defined.
         */
        core::Result<AlgorithmResult> outcome = const_cast<IPreparedAlgorithm&>(prepared).inspect(request);
        if (!outcome.has_value()) {
            return outcome.failure();
        }

        AlgorithmResult result = std::move(outcome).value();
        const std::size_t serialized_bytes = result.measurements.dump().size() + result.defects.dump().size();
        if (serialized_bytes > k_max_result_json_bytes) {
            return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_output_too_large,
                                      "algorithm result exceeds the " + std::to_string(k_max_result_json_bytes) +
                                          "-byte JSON bound");
        }
        return result;
    } catch (const std::exception& exception) {
        return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_exception,
                                  std::string("prepared algorithm threw an exception: ") + exception.what());
    } catch (...) {
        return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_exception,
                                  "prepared algorithm threw an unknown exception");
    }
}

}  // namespace cvforwin::inspection
