# CSV Import Pipeline v1.2.0：Part B 特性实现报告

状态：**v1.2.0 验收完成**。2026-10-26 对业务提交 `4713fab` 完成 Debug、Release 和 x86 ASan 构建与测试，每种配置均为 96/96 通过、0 失败、0 禁用。本报告与 v1.1 的 [Part A 报告](REPORT_v1.1.md) 分开维护；本次不包含 Part C，发布标签为 [v1.2.0](https://github.com/xuebossvh/csv-import-pipeline/tree/v1.2.0)。

## Part B：特性实现

### B-1: 流式导入（importStreaming + progress 回调）

- **设计思路**：同步与异步共用导入循环，保留 Part A 的解析、校验、错误预算和异常行为。后台持有路径、配置、规则副本；每 1000 条已处理数据记录发布统计并调用进度回调，好行与跳过的坏行都算，不计表头和空行，不对尾部不足 1000 条额外回调。空回调合法；返回 false 后以 `Result.cancelled=true` 交付已处理的部分，`success` 仍表示没有致命输入错误。回调与严格校验异常由 `future.get()` 重新抛出。
- **线程模型**：使用线程与 promise/future。工作线程不捕获调用方管道的 this，而是创建自己的导入引擎，并持有共享控制状态。线程启动门在 detach 成功后才放行；detach 异常时线程仅退出启动门，再由调用方回收。丢弃 future 不会隐式等待后台线程。单实例只允许一个活跃导入，正常完成或回调取消后可重用。
- **关键代码变更摘要**：[pipeline.h](../include/csvp/pipeline.h) 增加取消结果字段和只读任务数接口，禁止复制；[pipeline.cpp](../src/pipeline.cpp) 共用 runImport、在线程中交付结果或异常；[CMakeLists.txt](../CMakeLists.txt) 显式链接 Threads::Threads。统计使用局部计数与批量快照，没有逐行大锁；快照在回调前发布，回调在锁外执行。

### B-2: 优雅停止（shutdown + timeout）

- **设计思路**：停止标志及关闭标志为原子变量。shutdown 关闭实例，后续导入被拒绝；后台在读取下一条记录前检查停止。以入口时刻计算绝对截止时间，使用 timed_mutex 限制获取状态锁的等待；复制每任务独立的 `shared_future<void>` 后释放锁，再 `wait_until` 同一截止时间。完成信号覆盖统计及结果发布，超时返回不重新获取业务锁。零/负超时只通知，不等待；回调线程调用 shutdown 时不等待自己。
- **取消机制**：超时仅令shutdown返回，不强杀线程。管道析构请求停止并立即返回，后台持有的引擎、文件和控制状态在任务退出时释放。用户回调和系统I/O若长期阻塞，任务仍未结束；其引用捕获的外部对象必须由调用方保持有效。callback返回false只取消当前任务；显式shutdown则关闭整个实例。
- **未完成任务数汇报**：公开 `size_t pendingCount() const`。任务注册后为 1，完成或异常交付时归 0；超时不提前清零。该值是导入任务计数，不是未知的剩余 CSV 行数，查询不等待状态锁。计数可能先于结果 future 就绪短暂归零，取得结果以 `future.get()` / 等待结果 future 为准。

## 验收结果

| 配置 | 测试结果 | 构建与原始测试日志 |
|---|---|---|
| x64 Debug `/Od` | 96/96，0 失败、0 禁用 | [构建](evidence/v1.2.0/compilation/debug-build.txt) · [CTest](evidence/v1.2.0/tests/debug-ctest.txt) |
| x64 Release `/O2` | 96/96，0 失败、0 禁用 | [构建](evidence/v1.2.0/compilation/release-build.txt) · [CTest](evidence/v1.2.0/tests/release-ctest.txt) |
| x86 ASan RelWithDebInfo | 96/96，0 失败、0 禁用；无 ASan 错误报告 | [构建](evidence/v1.2.0/compilation/asan-build.txt) · [CTest](evidence/v1.2.0/tests/asan-ctest.txt) |

96 项包含 74 项既有回归与 22 项并发、生命周期及接口相关验收。已启用停止用例，并将原骨架占位断言替换为真实导入/关闭断言。三种配置执行同一组用例；[名单及计数校验](evidence/v1.2.0/metadata/validation.json) 记录业务提交、二进制校验值、用例名称及每项结果，[编译选项](evidence/v1.2.0/metadata/build-options.json) 记录实际配置。ASan 证据来自 x86；[运行库导入表](evidence/v1.2.0/compilation/asan-runtime-imports.txt) 确认加载 `clang_rt.asan_dynamic-i386.dll`，不将其表述为 x64 ASan 验证。

本次由 CMake Tools 对发现的用例分别执行 CTest。原始输出包含 UI 文件夹空项，未计入 96 项；最终结果以原始日志及逐名称核对的 JSON 为准。各次运行复用同一 XML 输出位置，最后一个用例会覆盖前面的内容，因此本次不提供或声称持有全套 XML。

关键已通过场景（数值来自已通过的测试断言）：

- 2500 条记录的回调序列为 `[1000,2000]`；坏行计入进度，表头和空行不计入。
- 混合数据导入 1875 行、跳过 625 行，共 625 个错误，仅保留 3 条详情；回调仍为 `[1000,2000]`。
- 回调返回 false 在第 1000 条处取消并返回部分结果；正常完成或回调取消后可重用实例。
- 回调异常及严格校验异常通过 future 交付；活跃任务槽位释放，同步/异步重入被拒绝。
- 在回调内阻塞后台线程，限时 shutdown 返回后仍报告 1 个任务；释放回调、等待结果后归零。相应测试断言是调用耗时 **小于 500 ms**，不能据此声称实际耗时精确等于传入的 20 ms。
- 回调内部调用 shutdown 不自等待；销毁管道或丢弃 future 不隐式等待后台；输入参数副本保持有效；多个读者可读取一致的统计快照。

Release 原始输出节选如下，完整逐项记录见 [Release CTest 日志](evidence/v1.2.0/tests/release-ctest.txt)。行末秒数是整个用例耗时，不是 shutdown 单次调用耗时：

```text
[ctest] 1/1 Test #92: Concurrent.DestroyingTheInstanceDoesNotBlockAndTheFutureStaysValid ...   Passed    0.07 sec
[ctest] 1/1 Test #77: Concurrent.ImportAndStats ........   Passed    0.07 sec
[ctest] 1/1 Test #91: Concurrent.ShutdownTimeoutReportsPendingUntilTheWorkerActuallyFinishes ...   Passed    0.09 sec
```

常用 `build/Debug`、`build/Release` 输出也已重新构建。正常 Release 程序对 `sample_utf8.csv` 的示例运行导入 10 行、跳过 0 行、错误 0 行，退出码 0。

## 同步路径基准与内存

本次顺序运行 v1.1（`8c2f605`）与 v1.2.0（`4713fab`）的同一 100000 行样本：3,146,588 字节，每个程序重复 3 次，每次均导入 98000 行、跳过 2000 行。下表使用程序报告的单轮耗时平均值，不使用包含进程启动的总墙钟时间。

| 配置 | v1.1 平均耗时 | v1.2.0 平均耗时 | 变化 | 原始测量 |
|---|---:|---:|---:|---|
| x64 Debug `/Od` | 1033.837 ms | 1000.352 ms | −3.24% | [v1.1](evidence/v1.2.0/bench/v1.1-debug-100k.json) · [v1.2.0](evidence/v1.2.0/bench/v1.2.0-debug-100k.json) |
| x64 Release `/O2` | 64.077 ms | 63.719 ms | −0.56% | [v1.1](evidence/v1.2.0/bench/v1.1-release-100k.json) · [v1.2.0](evidence/v1.2.0/bench/v1.2.0-release-100k.json) |

这些结果未观察到同步导入明显退化。压测运行在单台机器，启动前计算输入校验值，未清空操作系统文件缓存；三次热缓存测量不能保证其他机器、负载或异步吞吐量。本节用于检查 Part B 共享导入路径的开销，不代表完成 Part C 性能优化。

Release 对 1,003,898,922 字节样本单次处理的 [测量 JSON](evidence/v1.2.0/bench/v1.2.0-release-1gb.json) 与 [结果报告](evidence/v1.2.0/bench/v1.2.0-release-1gb-report.txt) 显示：

| 指标 | 实测值 |
|---|---:|
| 导入 / 跳过 | 9,990,000 / 10,000 行 |
| 程序导入耗时 | 8.90618 s |
| 进程墙钟耗时 | 9.10949 s |
| 峰值工作集 | 5,369,856 字节，约 5.12 MiB |
| 周期采样的私有内存峰值 | 1,150,976 字节，约 1.10 MiB |
| 采样周期 / 数量 | 100 ms / 85 次 |
| 错误详情 | 总错误 10000 行，保留 200 条、64692 字节，截断标记为真 |

工作集峰值来自操作系统进程计数；私有内存峰值为周期采样值，可能漏掉更短的峰值。两者均不是系统文件缓存大小，也不证明所有输入下的内存硬上限。所有本次基准进程退出码为 0、未超时；输入与程序的 SHA256 保存在对应 JSON 中。

## 附件与交付范围

- [验证汇总与用例名单](evidence/v1.2.0/metadata/validation.json)、[编译选项](evidence/v1.2.0/metadata/build-options.json)、[构建证据](evidence/v1.2.0/compilation)、[测试日志](evidence/v1.2.0/tests)、[基准与内存证据](evidence/v1.2.0/bench)。
- [本地测试与样本校验值](evidence/v1.2.0/metadata/local-test-manifest.json) 标识实际参与验证的本地资料。
- 业务实现提交为 `4713fab`；本文及证据整理不改变上述已测业务实现。
- 本地测试源码、数据、Google Test、工作区 skill 及采集脚本继续遵守不跟踪规则；附件保存可核查的小型证据，不发布 1 GB 样本。
