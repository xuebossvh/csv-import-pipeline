#include "csvp/pipeline.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <mutex>
#include <thread>

namespace csvp {

struct ImportPipeline::SharedState {
    std::timed_mutex mutex;
    std::shared_ptr<std::promise<void>> completion;
    std::shared_future<void> completion_future;
    std::atomic<bool> stop_requested{false};
    std::atomic<size_t> pending_count_{0}; // transitions serialized by mutex
    std::atomic<bool> closed{false};
    std::thread::id worker;
    Stats snapshot;
};

ImportPipeline::ImportPipeline() : state_(std::make_shared<SharedState>()) {}

ImportPipeline::ImportPipeline(const Config& cfg, std::vector<FieldRule> rules)
    : state_(std::make_shared<SharedState>()), cfg_(cfg), rules_(std::move(rules)) {
    validator_.addRules(rules_);
}

ImportPipeline::~ImportPipeline() {
    // A timed-out callback may still run: it owns its job state and input copies.
    // User callback captures must likewise remain valid until its future completes.
    shutdown(std::chrono::milliseconds::zero());
}

void ImportPipeline::beginImport() {
    auto completion = std::make_shared<std::promise<void>>();
    auto completed = completion->get_future().share();
    std::lock_guard<std::timed_mutex> lock(state_->mutex);
    if (state_->closed) throw std::logic_error("Import pipeline is shut down");
    if (state_->pending_count_ != 0) throw std::logic_error("An import is already active");
    state_->snapshot = Stats{};
    state_->completion = std::move(completion);
    state_->completion_future = std::move(completed);
    state_->stop_requested.store(false, std::memory_order_relaxed);
    state_->pending_count_ = 1;
}

ImportPipeline::Result ImportPipeline::importSync(const char* filepath) {
    beginImport();
    {
        std::lock_guard<std::timed_mutex> lock(state_->mutex);
        state_->worker = std::this_thread::get_id();
    }
    const auto done = [&] {
        {
            std::lock_guard<std::timed_mutex> lock(state_->mutex);
            state_->pending_count_ = 0;
            state_->worker = {};
            state_->completion->set_value();
        }
    };
    try {
        auto result = runImport(filepath, {}, state_);
        done();
        return result;
    } catch (...) {
        done();
        throw;
    }
}

ImportPipeline::Result ImportPipeline::runImport(
    const char* filepath, const std::function<bool(size_t)>& progress,
    const std::shared_ptr<SharedState>& state) {
    Result result;
    Stats current;
    const auto started = std::chrono::steady_clock::now();
    CsvParser parser(cfg_.parser);

    const auto finish = [&]() {
        current.bytes_read = parser.bytesRead();
        current.rows_imported = result.rows_imported;
        current.rows_skipped = result.rows_skipped;
        current.error_count = result.error_count;
        current.errors_truncated = result.errors_truncated;
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        current.elapsed_ms = result.elapsed_ms;
        std::lock_guard<std::timed_mutex> lock(state->mutex);
        state->snapshot = current;
    };

    const auto checkpoint = [&] {
        if (current.total_lines % 1000 != 0) return;
        finish();
        if (progress) {
            if (current.total_lines > std::numeric_limits<size_t>::max())
                throw std::overflow_error("Progress count exceeds size_t");
            if (!progress(static_cast<size_t>(current.total_lines))) {
                result.cancelled = true;
                state->stop_requested.store(true, std::memory_order_relaxed);
            }
        }
    };

    try {
        if ((state->stop_requested.load(std::memory_order_relaxed) || state->closed.load(std::memory_order_relaxed))) {
            result.cancelled = true;
            finish();
            return result;
        }
        if (!parser.parse(filepath)) {
            result.success = false;
            collectError(parser.lineNumber(), parser.rawLine(), parser.lastErrorCode(),
                         parser.lastError(), result, parser.rawLineTruncated());
            finish();
            return result;
        }

        const size_t expected_columns = cfg_.parser.has_header
            ? parser.header().size() : rules_.size();
        Row row;
        for (;;) {
            if ((state->stop_requested.load(std::memory_order_relaxed) || state->closed.load(std::memory_order_relaxed))) {
                result.cancelled = true;
                break;
            }
            const auto status = parser.readNext(row);
            if (status == CsvParser::ReadStatus::End) break;

            if (status == CsvParser::ReadStatus::Error) {
                const ErrorCode code = parser.lastErrorCode();
                const bool recoverable = code == ErrorCode::ParseError ||
                                         code == ErrorCode::EncodingError;
                collectError(parser.lineNumber(), parser.rawLine(), code,
                             parser.lastError(), result, parser.rawLineTruncated());
                if (!recoverable) {
                    result.success = false;
                    break;
                }
                ++current.total_lines;
                ++result.rows_skipped;
                if (!cfg_.skip_bad_rows) {
                    throw std::runtime_error("CSV parsing failed at line " +
                                             std::to_string(parser.lineNumber()));
                }
                checkpoint();
                continue;
            }

            ++current.total_lines;
            if (row.size() != expected_columns) {
                collectError(parser.lineNumber(), parser.rawLine(),
                             ErrorCode::ColumnCountMismatch,
                             "Column count mismatch: expected " +
                                 std::to_string(expected_columns) + ", got " +
                                 std::to_string(row.size()),
                             result);
                ++result.rows_skipped;
                if (!cfg_.skip_bad_rows) {
                    throw std::runtime_error("Column count mismatch at line " +
                                             std::to_string(parser.lineNumber()));
                }
                checkpoint();
                continue;
            }
            validateAndCollect(row, parser.lineNumber(), parser.rawLine(), result);
            checkpoint();
        }
    } catch (...) {
        // Finish the observable statistics, then preserve the original exception,
        // including bad_alloc. Resource exhaustion is never treated as a bad row.
        finish();
        throw;
    }
    finish();
    return result;
}

size_t ImportPipeline::detailBudget(const Result& result) const {
    if (cfg_.error_limit != 0 && result.errors.size() >= cfg_.error_limit) return 0;
    const size_t maximum = cfg_.max_error_bytes == 0
        ? std::numeric_limits<size_t>::max() : cfg_.max_error_bytes;
    if (result.error_bytes >= maximum ||
        maximum - result.error_bytes < sizeof(RowError)) return 0;
    return maximum - result.error_bytes - sizeof(RowError);
}

void ImportPipeline::collectError(uint64_t line_number, const std::string& raw_line,
                                   FieldValidator::ValidationResult details,
                                   Result& result) {
    ++result.error_count;
    const size_t maximum = cfg_.max_error_bytes == 0
        ? std::numeric_limits<size_t>::max() : cfg_.max_error_bytes;
    if ((cfg_.error_limit != 0 && result.errors.size() >= cfg_.error_limit) ||
        result.error_bytes >= maximum ||
        maximum - result.error_bytes < sizeof(RowError)) {
        result.errors_truncated = true;
        return;
    }

    size_t remaining = maximum - result.error_bytes - sizeof(RowError);
    RowError error;
    error.line_number = line_number;
    error.cell_errors = std::move(details.errors);
    error.details_truncated = details.details_truncated;
    // Callers build bounded details against detailBudget(result).
    remaining -= details.storage_bytes;
    const size_t raw_length = std::min(raw_line.size(), remaining);
    error.raw_line.assign(raw_line.data(), raw_length);
    error.details_truncated = error.details_truncated || raw_length != raw_line.size();
    result.errors_truncated = result.errors_truncated || error.details_truncated;
    result.error_bytes += sizeof(RowError) + details.storage_bytes + raw_length;
    result.errors.push_back(std::move(error));
}

void ImportPipeline::collectError(uint64_t line_number, const std::string& raw_line,
                                   ErrorCode code, const std::string& message,
                                    Result& result, bool raw_line_truncated) {
    FieldValidator::ValidationResult details;
    details.valid = false;
    const size_t budget = detailBudget(result);
    if (budget >= sizeof(CellError)) {
        CellError error;
        error.code = code;
        const size_t length = std::min(message.size(), budget - sizeof(CellError));
        error.message.assign(message.data(), length);
        error.details_truncated = length != message.size();
        details.storage_bytes = sizeof(CellError) + length;
        details.details_truncated = error.details_truncated;
        details.errors.push_back(std::move(error));
    } else {
        details.details_truncated = true;
    }
    details.details_truncated = details.details_truncated || raw_line_truncated;
    collectError(line_number, raw_line, std::move(details), result);
}

void ImportPipeline::validateAndCollect(const Row& row, uint64_t line_number,
                                        const std::string& raw_line, Result& result) {
    auto details = validator_.validateBounded(row, detailBudget(result));
    if (details.valid) {
        ++result.rows_imported;
        return;
    }
    collectError(line_number, raw_line, std::move(details), result);
    ++result.rows_skipped;
    if (!cfg_.skip_bad_rows) {
        throw std::runtime_error("Validation failed at line " +
                                 std::to_string(line_number));
    }
}

std::future<ImportPipeline::Result> ImportPipeline::importStreaming(
    const char* filepath, std::function<bool(size_t)> progress) {
    // Copy before registration; allocation failures leave the pipeline idle.
    const bool null_path = filepath == nullptr;
    std::string path = filepath ? filepath : "";
    auto config = cfg_;
    auto rules = rules_;
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    auto launch = std::make_shared<std::promise<bool>>();
    auto launched = launch->get_future().share();
    auto state = state_;
    beginImport();
    try {
        std::thread thread([state, promise, launched, path = std::move(path), null_path,
                            config = std::move(config), rules = std::move(rules),
                            progress = std::move(progress)]() mutable {
            if (!launched.get()) return;
            {
                std::lock_guard<std::timed_mutex> lock(state->mutex);
                state->worker = std::this_thread::get_id();
            }
            try {
                // The worker owns a separate engine: never dereference the caller's this.
                ImportPipeline engine(config, std::move(rules));
                auto result = engine.runImport(null_path ? nullptr : path.c_str(), progress, state);
                std::lock_guard<std::timed_mutex> lock(state->mutex);
                state->pending_count_ = 0;
                state->worker = {};
                promise->set_value(std::move(result));
                state->completion->set_value();
            } catch (...) {
                std::lock_guard<std::timed_mutex> lock(state->mutex);
                state->pending_count_ = 0;
                state->worker = {};
                promise->set_exception(std::current_exception());
                state->completion->set_value();
            }
        });
        // A promise-backed future has no implicit join on destruction.
        // detach only releases the OS thread handle; shared ownership retains resources.
        try {
            thread.detach();
        } catch (...) {
            // The start gate prevents any callback/I/O before detach succeeds.
            // On failure the joinable worker only exits the gate; never terminate.
            launch->set_value(false);
            thread.join();
            throw;
        }
        launch->set_value(true);
    } catch (...) {
        {
            std::lock_guard<std::timed_mutex> lock(state->mutex);
            state->pending_count_ = 0;
            state->worker = {};
            state->completion->set_value();
        }
        throw;
    }
    return future;
}

void ImportPipeline::shutdown(std::chrono::milliseconds timeout) {
    auto state = state_;
    const auto started = std::chrono::steady_clock::now();
    // Saturate extreme timeouts instead of overflowing the steady-clock duration.
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::time_point::max() - started);
    const auto bounded = std::min(std::max(timeout, std::chrono::milliseconds::zero()), maximum);
    const auto deadline = started + bounded;
    state->closed.store(true, std::memory_order_relaxed);
    state->stop_requested.store(true, std::memory_order_relaxed);
    std::unique_lock<std::timed_mutex> lock(state->mutex, std::defer_lock);
    if (bounded == std::chrono::milliseconds::zero()) return;
    if (!lock.try_lock_until(deadline)) return;
    if (state->worker == std::this_thread::get_id()) return;
    auto completed = state->completion_future;
    lock.unlock();
    // Unlike condition_variable::wait_until, this cannot re-lock the state
    // mutex after timeout. Completion covers statistics and result publication.
    if (completed.valid()) completed.wait_until(deadline);
}

size_t ImportPipeline::pendingCount() const {
    return state_->pending_count_.load(std::memory_order_acquire);
}

Stats ImportPipeline::stats() const {
    std::lock_guard<std::timed_mutex> lock(state_->mutex);
    return state_->snapshot;
}

} // namespace csvp
