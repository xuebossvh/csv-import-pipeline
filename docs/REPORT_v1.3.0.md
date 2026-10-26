# CSV Import Pipeline v1.3.0：Part C 内存与完整错误归档报告

状态：**v1.3.0 验收完成**。业务实现提交为 `b0d830d`（初始实现为 `d24a903`）。Debug、Release、x86 ASan 均为 120/120 通过、0 失败、0 禁用；指定 1 GB 文件已完成全量导入及全部 10000 条错误归档，独立核对通过，进程双峰均低于 50,000,000 字节。发布标签为 [v1.3.0](https://github.com/xuebossvh/csv-import-pipeline/tree/v1.3.0)。

本报告只记录本次 Part C。此前的 [Part A 报告](REPORT_v1.1.md) 与 [Part B 报告](REPORT_v1.2.0.md) 独立保留，不以旧日志替代本版本验收。

## Part C：目标与设计

针对给定的约 1 GB、五列 CSV 样本，同时验证三件事：所有数据行均被处理；全部错误行及原因可从归档恢复；benchmark 进程的峰值工作集和峰值私有提交量均小于 **50,000,000 字节**。这不是任意 CSV、任意字段数或任意用户回调的通用内存硬保证。

### 内存策略与写入背压

| 环节 | 保留策略 |
|---|---|
| 文件读取 | [解析器](../src/parser.cpp) 顺序使用 64 KiB 输入缓冲，消费完才读取下一块 |
| 当前记录 | 保留当前物理行和解析后的字段；[benchmark](../bench/bench_import.cpp) 将行内容上限设为 1 MiB，给定样本为五个短字段 |
| 内存错误预览 | 默认最多 200 条、4 MiB 的对象及字符串内容预算；达到上限后继续处理与累计错误 |
| 完整错误归档 | [JSONL writer](../src/error_log.cpp) 使用固定 64 KiB stdio 缓冲，逐段转义并写出，不拼接整份日志或整行转义后的 JSON |
| 任务与输出 | 导入线程同步写日志，没有异步输出队列；写入变慢时导入随之等待，形成自然背压 |

有界内存预览与完整文件归档各有用途。归档开启时，[管道](../src/pipeline.cpp) 为坏行保留完整校验详情，写入 JSONL 后再按预算裁剪内存预览；不会因预览已满而停止归档。该方式将随错误数量增长的存储转移到文件，磁盘空间仍随错误数量增长。

4 MiB 是错误详情账本预算，不包含容器预留容量、分配器、运行库、线程栈和所有进程内存。1 MiB 行长也不能约束任意字段数：一行大量分隔符可生成大量空字段。本次小于 50 MB 的结论必须由指定五列样本的进程实测支持，不能推广到百万列输入。归档遇到原始行已被解析器截断时明确失败，要求提高限制或更换输入，不把前缀冒充完整原文。

### 完整错误归档及字节语义

- [配置接口](../include/csvp/pipeline.h) 新增可选 `error_log_path`；为空保留原行为，非空启用完整 JSON Lines 归档。benchmark 为每轮选择单独日志路径；独占创建，拒绝覆写已有日志，重复运行需要新的输出路径。
- 每条记录包含物理行号 `line_number`、`raw_line` 和各列错误的索引、名称、错误码、原因、原始值。行号包含表头和空行，与源文件的物理行对应。
- JSON 字符串将原始字节映射到 U+0000～U+00FF；非 ASCII 字节使用 `\u00XX`，引号、反斜杠和控制字符合法转义。读取 JSON 后按 Latin-1 还原即可得到原字节，不能再次按 UTF-8 编码来比较。UTF-8、GBK 或非法编码字节都可以无损保存。
- `raw_line` 保留分隔符、引号与空白，排除物理行的 LF/CRLF 终止符；独立核对也采用这一边界。
- 输出使用原子独占创建；输入、报告与日志不能引用同一文件。Windows 检查大小写别名、路径尾点/尾空格、备用数据流及 DOS 设备名，防止创建或覆盖错误目标。报告写入前再次检查已存在文件的等价性。

### 完成、失败与取消契约

| 状态 | 对外行为 |
|---|---|
| 完整记录写入 | `errors_written` 递增；表示已提交给 writer 的记录数，本身不是持久化证明 |
| 正常关闭 | 检查短写、流错误、flush 和 close；全部成功后才设置 `error_log_complete=true` |
| 日志写入或关闭失败 | 同步调用抛异常；后台调用通过结果 future 交付异常；不返回伪成功 |
| 严格模式遇坏行 | 先记录错误并显式关闭日志，再交付业务异常；同时发生关闭错误时明确报告日志失败 |
| 取消 / shutdown | 关闭已经处理部分的归档，返回部分结果并标记 `cancelled`；不宣称已处理整个输入 |
| 原始行超限被截断 | 归档模式明确失败，不保存截断记录作为完整错误 |

`error_log_complete` 只表示**已观察错误的归档完成且关闭成功**，不等于整个源文件导入成功。benchmark 要求同时满足 `success && !cancelled && error_log_complete`，并检查 `errors_written == error_count == rows_skipped`。坏行分类计数在可能抛异常的归档 I/O 之前更新，异常后的统计仍可核对；底层资源异常不会当作可跳过的普通数据错误。

writer 的析构只做不抛异常的关闭兜底。正常及异常退出都显式检查关闭结果；使用文件系统缓存的 flush/close 成功不等于额外承诺断电后的物理介质持久性。

## 验收结果

| 配置 / 检查 | 实际结果 | 证据 |
|---|---|---|
| x64 Debug `/Od` | 120/120，0 失败、0 禁用 | [构建](evidence/v1.3.0/compilation/debug-build.txt) · [CTest](evidence/v1.3.0/tests/debug-ctest.txt) |
| x64 Release `/O2` | 120/120，0 失败、0 禁用 | [构建](evidence/v1.3.0/compilation/release-build.txt) · [CTest](evidence/v1.3.0/tests/release-ctest.txt) |
| x86 ASan RelWithDebInfo | 120/120，0 失败、0 禁用；无 ASan 错误报告 | [构建](evidence/v1.3.0/compilation/asan-build.txt) · [CTest](evidence/v1.3.0/tests/asan-ctest.txt) |
| 1 GB 导入、归档及双峰内存 | 完成，退出码 0，未超时，双峰达标 | [测量 JSON](evidence/v1.3.0/bench/v1.3.0-release-1gb.json) · [报告](evidence/v1.3.0/bench/v1.3.0-release-1gb-report.txt) |
| 源文件与全部错误独立核对 | 1000 万行遍历完成，行号、原因和原字节全部一致 | [核对结果](evidence/v1.3.0/metadata/archive-verification.json) |

[验证汇总](evidence/v1.3.0/metadata/validation.json) 按实际用例名称核对三配置的执行、通过和禁用数量，并记录业务提交及二进制 SHA256。[编译选项](evidence/v1.3.0/metadata/build-options.json) 证明 Debug `/Od`、Release `/O2` 及 ASan 插桩；[运行库导入表](evidence/v1.3.0/compilation/asan-runtime-imports.txt) 包含 `clang_rt.asan_dynamic-i386.dll`，本次 ASan 验证范围为 x86。[本地验证文件清单](evidence/v1.3.0/metadata/local-test-manifest.json) 保存测试源码与独立核对脚本校验值。

120 项包括 Part A/B 既有回归及本次归档验收。用例分别通过 CTest 执行；计数以完整名单和原始日志为准，不计 UI 文件夹空项，不把逐次覆盖的单个 XML 当作完整测试报告。以下用例在三配置均通过：

| 重点 | 具体用例与原始日志 |
|---|---|
| 超过预览上限仍完整归档 | [Pipeline.ErrorArchiveContainsAllThousandErrorsBeyondMemoryCountCap](evidence/v1.3.0/tests/release-ctest.txt) |
| 字节预算用尽仍保留所有原因 | [Pipeline.ErrorArchiveKeepsEveryCauseWhenTheMemoryByteBudgetIsExhausted](evidence/v1.3.0/tests/release-ctest.txt) |
| 短写不计入完整记录 | [ErrorLogWriter.ShortWriteDoesNotCountThePartialRecordAndFailureRemainsSticky](evidence/v1.3.0/tests/asan-ctest.txt) |
| flush 失败及重复关闭 | [ErrorLogWriter.FlushFailureThrowsAndRepeatedCloseDoesNotReleaseTheStreamAgain](evidence/v1.3.0/tests/asan-ctest.txt) |
| close 失败不双重释放 | [ErrorLogWriter.CloseFailureAfterRealFcloseDoesNotCauseADoubleClose](evidence/v1.3.0/tests/asan-ctest.txt) |
| 严格失败前归档已观察错误 | [Pipeline.ErrorArchiveStrictFailureFlushesTheObservedErrorBeforeThrowing](evidence/v1.3.0/tests/debug-ctest.txt) |
| 取消只归档已处理部分 | [Pipeline.ErrorArchiveCancellationClosesAndFlushesExactlyTheProcessedPart](evidence/v1.3.0/tests/debug-ctest.txt) |
| GBK 字节可逆 | [Pipeline.ErrorArchiveEncodesGbkBytesAsReversibleLatin1Escapes](evidence/v1.3.0/tests/release-ctest.txt) |
| 拒绝 ADS 与设备路径 | [Pipeline.ErrorArchiveMissingInputAlternateStreamOrDeviceIsRejected](evidence/v1.3.0/tests/debug-ctest.txt) |

短写、flush 与 close 的三个故障场景使用组件 I/O 注入接缝验证，并非拔出真实磁盘或故障卷实验。另有创建失败、大小写和尾点别名、已有文件保护、非法 UTF-8 原字节、截断行拒绝等回归，名称和结果均可在验证汇总中核查。

## 1 GB 实测与独立核对

实测使用用户原始 `sample_1gb.csv`：**1,003,898,922 字节、10,000,000 条数据行**，未生成简化替代样本。文件 SHA256 为 `89fa6c306b9730ea71a782da699c64cec1efc6b4097852bb457bc30abc9477d0`。[完整归档](evidence/v1.3.0/errors/sample_1gb.jsonl) 为 2,547,788 字节、10000 条记录，SHA256 为 `d7839cc46c6b5657e66632505464ceea867e250da423c807638e3cc6b9003b6c`。

| 指标 | v1.3.0 实测 |
|---|---:|
| 成功 / 跳过 / 完整归档 | 9,990,000 / 10,000 / 10,000 行 |
| 内存错误预览 | 200 条，64692 字节；预览截断标记为真 |
| 单轮导入耗时 / 进程墙钟耗时 | 8.90902 s / 9.1490581 s |
| 进程峰值工作集 | 5,574,656 字节，约 5.32 MiB |
| 进程峰值私有提交量 | 1,228,800 字节，约 1.17 MiB |
| 双峰阈值 / 判定 | 均小于 50,000,000 字节，`WithinLimit=true` |
| 运行退出码 / 超时 | 0 / 否 |

[benchmark JSON](evidence/v1.3.0/bench/v1.3.0-release-1gb.json) 的 `Stdout` 保留原始输出，其中三个关键结果为：

```text
Run 1: 9990000 rows in 8909.02 ms (10000 skipped) [10000 error rows, 200 retained, 10000 archived]
PeakWorkingSetBytes: 5574656
PeakPrivateCommitBytes: 1228800
```

双峰判定取自 JSON 的 `ProcessMemoryInfo`，由进程在日志和报告关闭后调用 Windows `GetProcessMemoryInfo` 获取：`PeakWorkingSetSize` 为生命周期峰值工作集，`PeakPagefileUsage` 为峰值私有提交量；API 失败即报告失败。JSON 中外部采集器的工作集字段和 100 ms 私有内存采样保留作辅助记录，不用于替代这里的双峰判据。多轮运行也不能把这些进程峰值当作可重置的单轮计数。

[独立核对脚本](../tools/verify_archive.cjs) 逐行遍历源文件和 JSONL，不调用被测 CSV 解析器，核对全部 1000 万条数据及 10000 条错误。[核对结果](evidence/v1.3.0/metadata/archive-verification.json) 记录：

- 10000 个错误行号从 1001、2001……至 10000001，所有行号、`age=N/A` 原因及原始字节全部匹配，没有重复或遗漏。
- `9,990,000 + 10,000 = 10,000,000`，行数守恒；`allErrorLineNumbersReasonsAndRawBytesMatch=true`、`rowConservation=true`。
- 源文件最大物理行长度为 101 字节，文件末尾有换行；输入与归档 SHA256 一致性已核对。
- 独立核对耗时约 26.6067 s，**不计入 8.90902 s 导入耗时**；核对工具的内存也不混入被测进程峰值。

原运行写出的归档路径记录在 [原始 benchmark 报告](evidence/v1.3.0/bench/v1.3.0-release-1gb-report.txt) 中，为 `bench/v1.3.0-release-1gb-report.errors.jsonl`。整理时仅将文件移至 [errors/sample_1gb.jsonl](evidence/v1.3.0/errors/sample_1gb.jsonl)，字节内容未变。测量 JSON 的 `ErrorArchive` 同时保存原输出路径、最终路径与校验值，原始观测文本保留不改写。

### 相同输入的旧版基线

| 版本与工作量 | 单轮耗时 | 证据 |
|---|---:|---|
| v1.2.0 `4713fab`：仅保留 200 条错误预览 | 8838.61 ms | [基线测量](evidence/v1.3.0/bench/v1.2.0-release-1gb.json) |
| v1.3.0 `b0d830d`：相同预览加 10000 条完整归档 | 8909.02 ms | [本版测量](evidence/v1.3.0/bench/v1.3.0-release-1gb.json) |

两次均为 x64 Release `/O2`、相同原始文件、单次处理，成功与跳过数量相同。新增完整归档后本次增加 70.41 ms，约 0.80%；两版工作量不同，不能作为同功能加速对比。测量在同一机器顺序进行，启动前计算输入校验值，未清空系统文件缓存；单次热缓存结果不构成其他机器或输入的性能保证。

## 交付范围

本报告及 [验证汇总](evidence/v1.3.0/metadata/validation.json)、[编译证据](evidence/v1.3.0/compilation)、[测试日志](evidence/v1.3.0/tests)、[基准测量](evidence/v1.3.0/bench)、[完整错误归档](evidence/v1.3.0/errors/sample_1gb.jsonl) 已组成此次验收材料，版本标签为 [v1.3.0](https://github.com/xuebossvh/csv-import-pipeline/tree/v1.3.0)。完整归档用于核查全部错误，不等同于上传 1 GB 输入；本地测试、数据和第三方资料继续遵守既有不跟踪规则。Part A/B 报告及用户修改的旧报告均不改写。
