#pragma once

#include "types.h"
#include "parser.h"
#include "validator.h"

#include <future>
#include <functional>
#include <chrono>
#include <atomic>
#include <memory>

namespace csvp {
namespace detail { class ErrorLogWriter; }

// ImportPipeline: CSV 导入管道的顶层入口。
// 串联 CsvParser（解析）+ FieldValidator（校验）+ 错误收集。
//
// 两种导入模式：
// - importSync:  同步导入，阻塞直到全部完成
// - importStreaming: 流式导入，支持进度回调和提前终止（Part B）
//
// 每个实例同时最多执行一个导入；重叠调用会抛出 logic_error。
class ImportPipeline {
public:
    struct Config {
        CsvParser::Config parser;
        bool skip_bad_rows = false;   // 是否跳过坏行继续导入
        size_t error_limit = 200;     // 最大错误详情条数；0 显式表示不限
        size_t max_error_bytes = 4 * 1024 * 1024; // 详情字节预算；0 显式表示不限
        std::string error_log_path;   // UTF-8；非空时全部错误写JSONL，独占创建不覆写
    };

    struct Result {
        uint64_t rows_imported = 0;
        uint64_t rows_skipped = 0;
        std::vector<RowError> errors; // 错误详情列表
        double elapsed_ms = 0.0;
        bool success = true;         // 致命文件/IO错误为false；strict坏行仍抛异常
        uint64_t error_count = 0;     // 全部错误行/致命错误数，不受详情上限影响
        bool errors_truncated = false;
        size_t error_bytes = 0;       // 保留对象及字符串内容；不含allocator额外开销
        bool cancelled = false;     // 协作停止后的部分结果；success仍表示无致命错误
        uint64_t errors_written = 0;
        bool error_log_complete = false; // 已观察错误均写出并close；不等于全输入成功
        std::string error_log_path;
    };

    ImportPipeline();
    ~ImportPipeline();
    ImportPipeline(const ImportPipeline&) = delete;
    ImportPipeline& operator=(const ImportPipeline&) = delete;

    // 传入解析器配置和校验规则列表
    ImportPipeline(const Config& cfg, std::vector<FieldRule> rules);

    // ---- 同步导入（当前实现）----
    Result importSync(const char* filepath);

    // ---- 流式导入 ----
    // 和 importSync 做同一件事，但放到后台线程里做：调用后马上返回，
    // 不占住调用方的线程。
    //   1. 读文件、解析、校验都在后台线程上完成，当前线程不等
    //   2. 每处理完1000条数据记录调用progress（含跳过，不含表头/空行）
    //   3. progress 返回 false 时，当作调用方要求停止，尽快收尾
    //   4. 最终结果（被叫停时就是已完成的那部分）通过返回的 std::future 交出
    //   5. 后台线程要经常看一眼停止标志，否则 shutdown() 喊停后
    //      它可能还在埋头干活，停不及时
    std::future<Result> importStreaming(const char* filepath,
                                         std::function<bool(size_t processed)> progress);

    // 请求停止正在进行的流式导入，最多等 timeout 毫秒。
    //   1. 先置位停止标志，通知后台线程收尾
    //   2. 最多等 timeout 毫秒，到点就返回，不强行结束线程
    //   3. pendingCount()随任务注册/完成更新，超时不会把未完成任务清零
    //   4. 不能死锁、泄漏资源或崩溃
    //   5. 不要用"一把大锁锁住全部操作"的方式实现
    void shutdown(std::chrono::milliseconds timeout);

    // shutdown关闭实例，不再接受新任务。任务数为已接受但尚未完成的导入数（0或1），
    // 不是尚未读取的行数；超时返回后任务可能继续收尾，完成后该值降为0。
    // pendingCount用于任务计数；需要结果时仍应等待返回的future。
    size_t pendingCount() const;

    // 并发安全的一致快照，每1000条数据记录以及结束/异常时发布。
    Stats stats() const;

private:
    struct SharedState;
    std::shared_ptr<SharedState> state_;
    void beginImport();
    Result runImport(const char* filepath, const std::function<bool(size_t)>& progress,
                     const std::shared_ptr<SharedState>& state);
    void validateAndCollect(const Row& row, uint64_t line_number,
                            const std::string& raw_line, Result& result,
                            detail::ErrorLogWriter* log);
    size_t detailBudget(const Result& result) const;
    void collectError(uint64_t line_number, const std::string& raw_line,
                      FieldValidator::ValidationResult details, Result& result,
                      detail::ErrorLogWriter* log);
    void collectError(uint64_t line_number, const std::string& raw_line,
                      ErrorCode code, const std::string& message, Result& result,
                      bool raw_line_truncated, detail::ErrorLogWriter* log);

    Config cfg_;
    std::vector<FieldRule> rules_;
    FieldValidator validator_;

    // 控制/发布状态由共享所有权管理，后台线程不捕获外部管道的this。
};

} // namespace csvp
