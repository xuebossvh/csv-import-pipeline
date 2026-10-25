#include "csvp/pipeline.h"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace csvp {

ImportPipeline::ImportPipeline(const Config& cfg, std::vector<FieldRule> rules)
    : cfg_(cfg), rules_(std::move(rules)) {
    validator_.addRules(rules_);
}

// ============================================================
// importSync
// ============================================================

ImportPipeline::Result ImportPipeline::importSync(const char* filepath) {
    Result result;
    auto t0 = std::chrono::steady_clock::now();

    CsvParser parser(cfg_.parser);
    if (!parser.parse(filepath)) {
        // 文件打开或表头解析失败
        RowError fatal;
        fatal.line_number = 0;
        fatal.raw_line = parser.lastError();
        result.errors.push_back(std::move(fatal));
        return result;
    }

    stats_.total_lines = 0;
    stats_.bytes_read = 0;
    stats_.rows_imported = 0;
    stats_.rows_skipped = 0;
    stats_.elapsed_ms = 0;

    Row row;
    while (parser.readRow(row)) {
        ++stats_.total_lines;

        // ---- 列数检查 ----
        if (row.size() != parser.header().size()) {
            std::ostringstream oss;
            oss << "Column count mismatch at line " << parser.lineNumber()
                << ": expected " << parser.header().size()
                << " columns, got " << row.size();
            throw std::runtime_error(oss.str());
        }

        // ---- 校验与收集 ----
        validateAndCollect(row, parser.lineNumber(), result);
    }

    // 后处理
    stats_.bytes_read = parser.bytesRead();
    stats_.rows_imported = result.rows_imported;
    stats_.rows_skipped = result.rows_skipped;

    auto t1 = std::chrono::steady_clock::now();
    result.elapsed_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    stats_.elapsed_ms = result.elapsed_ms;

    return result;
}

// ============================================================
// validateAndCollect
// ============================================================

void ImportPipeline::validateAndCollect(Row& row, size_t line_number, Result& result) {
    try {
        auto cell_errors = validator_.validate(row);

        if (cell_errors.empty()) {
            // 校验通过
            ++result.rows_imported;
            return;
        }

        // 校验失败：收集错误
        if (cfg_.skip_bad_rows) {
            // 跳过坏行模式：记录错误但不中断
            RowError row_err;
            row_err.line_number = line_number;

            std::ostringstream oss;
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) oss << ",";
                oss << row[i];
            }
            row_err.raw_line = oss.str();

            row_err.cell_errors = std::move(cell_errors);

            if (cfg_.error_limit == 0 || result.errors.size() < cfg_.error_limit) {
                result.errors.push_back(std::move(row_err));
            }
            ++result.rows_skipped;
        } else {
            // 非跳过模式：遇到错误立即失败
            RowError row_err;
            row_err.line_number = line_number;

            std::ostringstream oss;
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) oss << ",";
                oss << row[i];
            }
            row_err.raw_line = oss.str();
            row_err.cell_errors = std::move(cell_errors);
            result.errors.push_back(std::move(row_err));

            throw std::runtime_error("Validation failed and skip_bad_rows is disabled");
        }
    } catch (const std::runtime_error&) {
        if (!cfg_.skip_bad_rows) {
            throw;  // 重新抛出
        }
        // skip_bad_rows 模式下记录但继续
        RowError row_err;
        row_err.line_number = line_number;
        row_err.raw_line = "(exception occurred, raw line unavailable)";
        ++result.rows_skipped;
    }
}

// ============================================================
// importStreaming（骨架）
//
// 现在的 importSync 是"一口气读完"：调用方一旦调用它，当前线程就被占住，
// 直到整个文件处理完才返回。这期间既看不到进度，也没办法喊停，
// 文件一大就要干等很久。
//
// importStreaming 做的是同一件事，只是换了干活的方式：
// 调用后马上返回一个 future，真正的读文件、解析、校验交给后台线程；
// 干活过程中定期汇报进度，调用方不满意可以随时叫停。
// ============================================================

std::future<ImportPipeline::Result> ImportPipeline::importStreaming(
    const char* /*filepath*/,
    std::function<bool(size_t)> /*progress*/) {
    // TODO: 把上面说的"后台干活"实现出来。
    //
    // 整体流程还是那几步：打开文件 -> 一行行读出来解析 -> 逐字段校验
    // -> 累计成功/跳过/错误明细，只不过这一整套不要占着调用方的线程，
    // 而是放到后台线程里跑。
    //
    // 具体要求：
    // 1. 本函数要快速返回，不能在这里等文件读完；导入工作在后台线程里进行。
    //    实现时可以一次读一批（比如 1000 行）再统一处理，这只是手段，
    //    不要求死。真正的导入流程（读、解析、校验）必须都由后台线程完成。
    // 2. 后台线程每处理完 1000 行，调用一次 progress(已处理的行数)。
    //    注意这个回调是在后台线程上执行的，回调里别做太慢的事情。
    // 3. 碰到下面两种情况要尽快停下来：
    //    - progress 回调返回 false，表示调用方不想继续了；
    //    - stop_requested_ 被置成 true，这是 shutdown() 发出的停止信号。
    //    检查的时机自己把握，但别等到整个文件读完才看一眼。
    // 4. 最终结果放进返回的 future 里。就算中途被叫停也没关系，
    //    把已经处理完的那部分结果交出去即可，不要抛异常。
    // 5. 每处理一行，顺手把 hot_total_lines_、hot_rows_imported_、
    //    hot_rows_skipped_ 这几个原子计数器加上去，stats() 靠它们反映实时进度。

    std::promise<Result> promise;
    promise.set_value(Result{});  // 占位：返回空结果
    return promise.get_future();
}

void ImportPipeline::shutdown(std::chrono::milliseconds /*timeout*/) {
    // TODO: 让正在跑的导入停下来。
    //
    // 注意：这里做的是"打个招呼，让它自己收尾"，而不是把线程强行杀掉。
    // 线程没办法从外面直接掐断，只能靠后台那边看到信号后主动退出。
    //
    // 具体要求：
    // 1. 先把 stop_requested_ 置为 true，告诉后台线程"该收尾了"。
    // 2. 最多等 timeout 毫秒。到点就返回，不管后台有没有彻底停干净，
    //    绝不能无限期卡在这里。
    // 3. 返回时，把还没完成的任务数量写进 pending_count_，方便调用方查看。
    // 4. 整个过程不能死锁、不能泄漏线程或资源、不能崩溃。
    // 5. 这一条是对后台线程的配合要求：它必须隔一段时间看一眼 stop_requested_，
    //    否则如果它正卡在一个耗时的操作里（比如 std::stod 解析超长数字），
    //    shutdown 就只能一直干等到 timeout 才返回。
}

// ============================================================
// stats
// ============================================================

Stats ImportPipeline::stats() const {
    Stats s = stats_;
    s.total_lines = hot_total_lines_.load(std::memory_order_relaxed);
    s.rows_imported = hot_rows_imported_.load(std::memory_order_relaxed);
    s.rows_skipped = hot_rows_skipped_.load(std::memory_order_relaxed);
    return s;
}

} // namespace csvp