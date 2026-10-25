#include "csvp/pipeline.h"
#include "csvp/parser.h"
#include "csvp/validator.h"

#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>

namespace {

using namespace csvp;

std::string dataPath(const char* filename) {
    return std::string(TEST_DATA_DIR) + filename;
}

std::vector<FieldRule> makeDefaultRules() {
    return {
        {"id", FieldType::Int, true},
        {"name", FieldType::String, true},
        {"age", FieldType::Int, true},
        {"score", FieldType::Double, true},
        {"active", FieldType::Bool, false},
    };
}

// ============================================================
// Concurrent.ImportAndStats
// ============================================================
TEST(Concurrent, ImportAndStats) {
    ImportPipeline::Config cfg;
    cfg.parser.delimiter = ',';
    cfg.parser.has_header = true;
    cfg.skip_bad_rows = false;

    ImportPipeline pipeline(cfg, makeDefaultRules());

    // 启动流式导入：导入在后台线程里跑，这一行马上返回
    std::atomic<bool> stop_flag{false};
    auto future = pipeline.importStreaming(
        dataPath("sample_100k.csv").c_str(),
        [&stop_flag](size_t processed) -> bool {
            // 这里不主动叫停，只记下进度；返回 true 表示"继续"
            (void)processed;
            return !stop_flag.load();
        });

    // 导入还在跑的时候，另起一个线程反复读 stats()，看会不会出问题
    std::atomic<bool> done{false};
    std::thread reader([&pipeline, &done, &stop_flag]() {
        for (int i = 0; i < 100 && !done.load(); ++i) {
            auto s = pipeline.stats();
            (void)s;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stop_flag.store(true);
    });

    // 等导入结束：future 就绪，说明后台已经收工
    future.wait();
    auto result = future.get();
    done.store(true);
    reader.join();

    std::cout << "Concurrent.ImportAndStats: imported=" << result.rows_imported << std::endl;
}

// ============================================================
// Concurrent.StatsThreadSafety
// ============================================================
TEST(Concurrent, StatsThreadSafety) {
    ImportPipeline pipeline(ImportPipeline::Config{}, {});

    // 多个线程同时调用 stats() 应当是安全的
    std::atomic<int> read_count{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&pipeline, &read_count]() {
            for (int j = 0; j < 1000; ++j) {
                auto s = pipeline.stats();
                (void)s;
                ++read_count;
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(read_count.load(), 4000);
    std::cout << "Concurrent.StatsThreadSafety: "
              << read_count.load() << " reads completed." << std::endl;
}

// ============================================================
// Concurrent.StatsAtomicRead (PASS)
// stats() 从原子计数器读取当前进度，不应崩溃。
// ============================================================
TEST(Concurrent, StatsAtomicRead) {
    ImportPipeline pipeline(ImportPipeline::Config{}, {});
    auto s = pipeline.stats();
    EXPECT_EQ(s.total_lines, 0u);
    std::cout << "Concurrent.StatsAtomicRead: stats() ok, total_lines="
              << s.total_lines << std::endl;
}

// ============================================================
// Concurrent.DISABLED_ShutdownNoDeadlock（Part B 验收）
// 把 importStreaming 和 shutdown 写完之后，去掉 DISABLED_ 前缀启用本用例。
// 它检查的是：后台还在干活的时候调用 shutdown(timeout)，
// 能不能在限定时间内正常返回——不卡死、不泄漏、不崩溃。
// ============================================================
TEST(Concurrent, DISABLED_ShutdownNoDeadlock) {
    ImportPipeline::Config cfg;
    cfg.parser.has_header = true;
    cfg.skip_bad_rows = false;
    ImportPipeline pipeline(cfg, makeDefaultRules());

    // 启动流式导入：导入在后台线程里跑，这一行马上返回
    auto future = pipeline.importStreaming(
        dataPath("sample_100k.csv").c_str(),
        [](size_t processed) -> bool {
            // 处理到第 20000 行时返回 false，等于告诉后台"到此为止"
            return processed < 20000;
        });

    // 刚启动就请求停止，最多只等 500 毫秒
    pipeline.shutdown(std::chrono::milliseconds(500));

    // 取结果：如果这一行卡住，就说明死锁了
    auto result = future.get();
    std::cout << "Concurrent.DISABLED_ShutdownNoDeadlock: imported="
              << result.rows_imported
              << " (expected: shutdown returned without deadlock)" << std::endl;
}

} // anonymous namespace