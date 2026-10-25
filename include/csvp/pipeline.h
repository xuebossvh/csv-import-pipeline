#pragma once

#include "types.h"
#include "parser.h"
#include "validator.h"

#include <future>
#include <functional>
#include <chrono>
#include <atomic>

namespace csvp {

// ImportPipeline: CSV 导入管道的顶层入口。
// 串联 CsvParser（解析）+ FieldValidator（校验）+ 错误收集。
//
// 两种导入模式：
// - importSync:  同步导入，阻塞直到全部完成
// - importStreaming: 流式导入，支持进度回调和提前终止（Part B）
//
// 注意：当前版本 importStreaming 和 shutdown 仅为骨架，
// 需要候选人完成实现。
class ImportPipeline {
public:
    struct Config {
        CsvParser::Config parser;
        bool skip_bad_rows = false;   // 是否跳过坏行继续导入
        size_t error_limit = 0;       // 最大收集错误数；0 表示不限制（全部收集）
    };

    struct Result {
        size_t rows_imported = 0;
        size_t rows_skipped = 0;
        std::vector<RowError> errors; // 错误详情列表
        double elapsed_ms = 0.0;
    };

    ImportPipeline() = default;

    // 传入解析器配置和校验规则列表
    ImportPipeline(const Config& cfg, std::vector<FieldRule> rules);

    // ---- 同步导入（当前实现）----
    Result importSync(const char* filepath);

    // ---- 流式导入（Part B：候选人实现）----
    // 和 importSync 做同一件事，但放到后台线程里做：调用后马上返回，
    // 不占住调用方的线程。
    //   1. 读文件、解析、校验都在后台线程上完成，当前线程不等
    //   2. 每处理完 1000 行，调用一次 progress(已处理的行数) 回调
    //   3. progress 返回 false 时，当作调用方要求停止，尽快收尾
    //   4. 最终结果（被叫停时就是已完成的那部分）通过返回的 std::future 交出
    //   5. 后台线程要经常看一眼停止标志，否则 shutdown() 喊停后
    //      它可能还在埋头干活，停不及时
    std::future<Result> importStreaming(const char* filepath,
                                         std::function<bool(size_t processed)> progress);

    // 请求停止正在进行的流式导入，最多等 timeout 毫秒。
    //   1. 先置位停止标志，通知后台线程收尾
    //   2. 最多等 timeout 毫秒，到点就返回，不强行结束线程
    //   3. 返回时把未完成的任务数写进 pending_count_
    //   4. 不能死锁、泄漏资源或崩溃
    //   5. 不要用"一把大锁锁住全部操作"的方式实现
    void shutdown(std::chrono::milliseconds timeout);

    // ---- 统计信息 ----
    Stats stats() const;

private:
    void validateAndCollect(Row& row, size_t line_number, Result& result);

    Config cfg_;
    std::vector<FieldRule> rules_;
    FieldValidator validator_;

    // ---- 热路径统计计数器 ----
    // 供 importStreaming 的后台线程在每行处理时递增。
    std::atomic<size_t> hot_total_lines_ = 0;
    std::atomic<size_t> hot_rows_imported_ = 0;
    std::atomic<size_t> hot_rows_skipped_ = 0;

    // ---- 停止标志 ----
    std::atomic<bool> stop_requested_ = false;
    size_t pending_count_ = 0;  // shutdown 超时后的未完成任务数

    Stats stats_;
    bool streaming_active_ = false;
};

} // namespace csvp