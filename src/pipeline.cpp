#include "csvp/pipeline.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace csvp {

ImportPipeline::ImportPipeline(const Config& cfg, std::vector<FieldRule> rules)
    : cfg_(cfg), rules_(std::move(rules)) {
    validator_.addRules(rules_);
}

ImportPipeline::Result ImportPipeline::importSync(const char* filepath) {
    Result result;
    stats_ = Stats{};
    hot_total_lines_.store(0, std::memory_order_relaxed);
    hot_rows_imported_.store(0, std::memory_order_relaxed);
    hot_rows_skipped_.store(0, std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    CsvParser parser(cfg_.parser);

    const auto finish = [&]() {
        stats_.bytes_read = parser.bytesRead();
        stats_.rows_imported = result.rows_imported;
        stats_.rows_skipped = result.rows_skipped;
        stats_.error_count = result.error_count;
        stats_.errors_truncated = result.errors_truncated;
        result.elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        stats_.elapsed_ms = result.elapsed_ms;
    };

    try {
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
                ++stats_.total_lines;
                ++result.rows_skipped;
                if (!cfg_.skip_bad_rows) {
                    throw std::runtime_error("CSV parsing failed at line " +
                                             std::to_string(parser.lineNumber()));
                }
                continue;
            }

            ++stats_.total_lines;
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
                continue;
            }
            validateAndCollect(row, parser.lineNumber(), parser.rawLine(), result);
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

// Part B remains deliberately unimplemented. No worker thread or cancellation
// machinery is introduced by the Part A synchronous-path fixes.
std::future<ImportPipeline::Result> ImportPipeline::importStreaming(
    const char* /*filepath*/, std::function<bool(size_t)> /*progress*/) {
    std::promise<Result> promise;
    promise.set_value(Result{});
    return promise.get_future();
}

void ImportPipeline::shutdown(std::chrono::milliseconds /*timeout*/) {
    // Part B: cooperative stop, bounded wait and pending_count_ are future work.
}

Stats ImportPipeline::stats() const {
    // The Part A path is synchronous. Call this after importSync returns/throws;
    // concurrent observation is not a thread-safety guarantee of this skeleton.
    return stats_;
}

} // namespace csvp
