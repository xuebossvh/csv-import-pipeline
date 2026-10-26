# CSV Import Pipeline v1.1 缺陷修复与验证报告

执行：Codex 按用户指定范围实施。日期：2026-10-26。修复代码提交：`8c2f605`。最终版本标签：`v1.1`。提交时间记录见附件。

本报告按 [REPORT_TEMPLATE.md](REPORT_TEMPLATE.md) 填写。范围为用户列出的 **9 项 Part A** 及其直接关联的数据安全问题；Part B、Part C 尚未开展，本报告不记录其实施结果。初始版本、缺陷定位、修复前验证、代码修复与验收文档保留为不同提交。

## 验收结果

| 配置 | 执行用例 | 失败 | 结果 |
|---|---:|---:|---|
| x64 Debug，/Od | 79 | 0 | 通过 |
| x64 Release，/O2 | 79 | 0 | 通过，同一组断言与 Debug 一致 |
| x86 RelWithDebInfo，AddressSanitizer | 79 | 0 | 通过，退出 0，无 ASan 诊断 |
| 独立命令行回归 | 12 | 0 | 通过 |

共声明 80 个单元用例，仅 Part B 的 Concurrent.DISABLED_ShutdownNoDeadlock 保持禁用。原先 5 个 Parser 的 DISABLED 前缀已去掉；ErrorLimit 无 GTEST_SKIP，真实检查总处理量、错误总数和详情上限。两个明确的 Part B 骨架检查不代表特性完成。

本机只有 x86 ASan 运行库，因此 ASan 验证为 x86；不将其说成 x64 ASan 证据。原始输出及 XML、实际编译选项、运行库导入记录均附在下方。ASan 零诊断不等于穷尽证明所有内存问题或所有输入行为。

## Part A：缺陷修复

以下 9 项按模板的四项要求填写。短结果直接摘录原始输出，较长结果先汇总实际数值，再链接到附件并给出查找关键词。以下数据来自 v1.1 已有验收记录，本次为报告整理，没有重新运行测试；未提供 gtest_repeat 压力重复运行证据。各缺陷选用的测试可能重叠，不能将分项数量相加作为总用例数。

### 缺陷 A-1

**文件尾没有换行导致末行丢失**

- **复现步骤**：五列表头后放两条合法记录，最后一条不写 LF。旧程序只导入第一条且退出 0；原始证据见 [基线定向输出](evidence/baseline/part-a-debug-probes.txt) 的 EOF。
- **根因分析**：旧读取函数把“缓冲内没找到 LF”直接当成文件结束。
- **修复方法**：[解析器](../src/parser.cpp) 使用 64 KiB 顺序缓冲。文件真实结束时交付非空尾段，区分 Row、End、Error；同时处理恰好位于块边界的末行。
- **验证证据**：末行无换行的 3 项回归在 Debug、Release、x86 ASan 中均为 **3/3 成功、0 失败**。CLI 的 `EOF` 结果为 **导入 2 行、跳过 0 行、退出码 0**，末行未丢失。

  Debug 原始输出节选：

  ```text
  [       OK ] Parser.NoTrailingNewline (0 ms)
  [       OK ] Parser.HeaderAndFinalRecordWithoutNewlineRemainReadable (4 ms)
  [       OK ] Parser.Exact65536ByteFinalRecordWithoutNewline (6 ms)
  ```

  完整输出：[Debug 日志](evidence/v1.1/tests/debug-tests.txt)、[Release 日志](evidence/v1.1/tests/release-tests.txt)、[x86 ASan 日志](evidence/v1.1/tests/asan-tests.txt)；导入行数和退出码：[CLI 原始结果](evidence/v1.1/tests/cli-regressions.json)（查找 `"Case": "EOF"`）。

---

### 缺陷 A-2

**解析器重用的状态与资源管理**

- **复现步骤**：同一解析器读取两个不同文件；再测先打开不存在文件、后打开有效文件。旧实现行号、已读字节和错误残留，每次成功重开还覆盖旧 malloc 指针。
- **根因分析**：重置只覆盖表头及偏移；资源拥有者允许默认浅拷贝。
- **修复方法**：[解析器接口](../include/csvp/parser.h) 与实现采用 RAII 文件所有权和可复用缓冲，禁止复制及移动，重开统一重置状态。每次错误有明确类别及原始行。
- **验证证据**：重用、失败后重开、重复开关和所有权类型共 4 项回归，三套配置各 **4/4 成功、0 失败**。x86 ASan 整套运行退出码 **0**，日志没有 ASan 错误诊断。

  x86 ASan 原始输出节选：

  ```text
  [       OK ] Parser.ReentryState (0 ms)
  [       OK ] Parser.ReopenAfterFailureClearsOldErrorAndHeader (4 ms)
  [       OK ] Parser.RepeatedOpenReadCyclesKeepIndependentResults (20 ms)
  ```

  原始证据：[Debug 日志](evidence/v1.1/tests/debug-tests.txt)、[Release 日志](evidence/v1.1/tests/release-tests.txt)、[x86 ASan 日志](evidence/v1.1/tests/asan-tests.txt)；[ASan XML](evidence/v1.1/tests/asan-tests.xml)。这些结果覆盖已测循环，不能单独作为所有泄漏场景的穷尽证明。

---

### 缺陷 A-3

**跳过坏行仍中止或静默丢后续数据**

- **复现步骤**：正常行、列数错误行、正常行；旧 skip_bad_rows=true 仍退出 1。未闭合引号后放正常行，旧程序只导入前段却退出 0。
- **根因分析**：列数检查直接抛出；读取接口的 false 同时表示 EOF 和错误，管道没有区分。
- **修复方法**：[管道](../src/pipeline.cpp) 对列数、解析、编码和校验错误统一计数、收集和跳过；严格模式保留明确异常。无法打开及致命 I/O 通过 success=false 表达。无表头按规则列数检查，失败与正常结束都完成同步统计。
- **验证证据**：跳过坏行与严格模式等 5 项回归，三套配置各 **5/5 成功、0 失败**。两项 CLI 记录的实际结果如下，说明坏行后面的正常记录仍被导入：

  | CLI 场景 | 导入行数 | 跳过行数 | 错误总数 / 保留详情 | 退出码 |
  |---|---:|---:|---:|---:|
  | ColumnMismatch | 2 | 1 | 1 / 1 | 0 |
  | ParseError | 2 | 1 | 1 / 1 | 0 |

  数据来源：[CLI 原始结果](evidence/v1.1/tests/cli-regressions.json)（查找对应 `Case`，行数见 `Stdout`）；单元测试输出：[Debug 日志](evidence/v1.1/tests/debug-tests.txt)、[Release 日志](evidence/v1.1/tests/release-tests.txt)、[x86 ASan 日志](evidence/v1.1/tests/asan-tests.txt)，查找 `Pipeline.ColumnMismatchSkipsAndContinues`、`Pipeline.ParseErrorSkipsAndContinues` 和 `Pipeline.StrictModeRejectsColumnAndParseAndValidationErrors`。

---

### 缺陷 A-4

**GBK 编码与分隔符边界**

- **复现步骤**：使用 GBK、分隔符 |、无表头，输入字节 81 7C 7C 78 0A。81 7C 是合法双字节结构，期望两列；旧代码按字节把尾字节当成分隔符。
- **根因分析**：编码配置没有参与字段边界判定。原来的 sample_gbk.csv 还包含非法 D4 2C，不能证明合法 GBK 包含默认逗号。
- **修复方法**：[解析器](../src/parser.cpp) 整体识别合法双字节结构，再判断分隔符；非法结构报告 EncodingError，错误行消费后可继续。校正本地 GBK 样本，并保留非法输入回归。Windows 路径统一接受 UTF-8 后转换为宽字符，消除正常测试文件打不开的问题。
- **验证证据**：GBK 样本、合法尾字节、非法编码、跨块字符及中文路径共 5 项回归，三套配置各 **5/5 成功、0 失败**。

  Debug 原始输出节选：

  ```text
  [       OK ] Parser.LegitimateGbkTrailByteIsNotTheCustomDelimiter (3 ms)
  [       OK ] Parser.InvalidGbkDoesNotConsumeARealComma (3 ms)
  [       OK ] Parser.GbkCharacterSplitAt65536ByteBoundaryKeepsItsTrailByte (6 ms)
  ```

  完整结果：[Debug 日志](evidence/v1.1/tests/debug-tests.txt)、[Release 日志](evidence/v1.1/tests/release-tests.txt)、[x86 ASan 日志](evidence/v1.1/tests/asan-tests.txt)，查找 `ChineseGBKEncoding` 和上列测试名。**边界说明**：实现验证 GBK 字节结构并保留原字节，不转码，也未穷尽字符映射表；默认逗号 2C 不是合法 GBK 尾字节。

---

### 缺陷 A-5

**非法数值与真实零不可区分**

- **复现步骤**：对 0、N/A、空字符串、越界索引、溢出数字、12abc、1.5abc 分别调用转换。旧访问器返回零或接受数字前缀；校验还会接受溢出数值、允许 NaN 绕范围。
- **根因分析**：转换忽略完整消费、ERANGE 和有限值，错误统一折叠为零。
- **修复方法**：[Row](../include/csvp/row.h) 严格转换并明确抛 invalid_argument/out_of_range，提供 optional 访问器；布尔转换同样区分失败与 false。[校验器](../src/validator.cpp) 复用这些转换，在既有 has_range 框架内处理非有限值、分数边界和缺失必填列，没有另造校验框架。
- **验证证据**：三套配置均执行 **7 项 Row + 17 项 Validator，共 24/24 成功、0 失败**，覆盖非法值与合法零的区分、溢出、非有限值及范围边界。

  Debug 原始输出节选：

  ```text
  [       OK ] Row.InvalidIntegerIsNotSilentlyConvertedToZero (0 ms)
  [       OK ] Row.InvalidDoubleIsNotSilentlyConvertedToZero (0 ms)
  [       OK ] Row.OptionalConversionsPreserveAValidZeroAndRejectInvalidValues (0 ms)
  ```

  CLI `NumericOverflow` 实际 **导入 0 行、跳过 2 行、错误 2 条、退出码 0**；`MissingColumn` 实际 **导入 0 行、跳过 1 行、错误 1 条、退出码 0**。完整结果：[CLI 原始结果](evidence/v1.1/tests/cli-regressions.json)；24 项测试输出较长，见 [Debug 日志](evidence/v1.1/tests/debug-tests.txt)、[Release 日志](evidence/v1.1/tests/release-tests.txt)、[x86 ASan 日志](evidence/v1.1/tests/asan-tests.txt)，分别查找 `tests from Row`、`tests from Validator`。

---

### 缺陷 A-6

**内存增长与错误详情上限**

- **复现步骤**：大量列数正确但类型非法的记录。旧 bench 先收集全部错误，最后才截取 200 条报告样本；同时每处理一行重复读取约 16 MiB。
- **根因分析**：报告样本上限没有约束运行中的错误结果；缓冲中未处理的数据没有复用。
- **修复方法**：顺序读取、复用块中剩余行；默认最多 200 条详情和 4 MiB 对象/字符串内容预算，满额后继续计数及验证而不复制完整错误字段。原始行被截断时显式传播标记。error_limit=0 或 max_error_bytes=0 分别只关闭对应限制。
- **验证证据**（错误上限、内存与前后性能）：4 项上限/截断回归在三套配置各 **4/4 成功、0 失败**。`ErrorLimit` 中 `error_limit=3` 的断言结果为：**总行数 1001、导入 1、跳过 1000、错误总数 1000、保留详情 3、截断标记 true**。

  Debug 原始输出节选：

  ```text
  [       OK ] Pipeline.ErrorLimit (20 ms)
  [       OK ] Pipeline.ByteBudgetDoesNotLoseAggregateCounts (4 ms)
  ```

  测试输出：[Debug 日志](evidence/v1.1/tests/debug-tests.txt)、[Release 日志](evidence/v1.1/tests/release-tests.txt)、[x86 ASan 日志](evidence/v1.1/tests/asan-tests.txt)。下表是与本缺陷直接相关的已有性能与内存实测，不作为 Part C 的完成记录：

  | 实测项目 | 结果 | 原始附件 |
  |---|---|---|
  | 10 万行，修复前 Debug /Od，3 次平均 | 37.863 秒；工作集峰值 23.00 MiB | [修复前数据](evidence/v1.1/bench/before-debug-100k.json) |
  | 同一输入，修复后 Debug /Od，3 次平均 | 1.020 秒；工作集峰值 6.83 MiB；约改善 37.1 倍 | [修复后数据](evidence/v1.1/bench/after-debug-100k.json) |
  | 1,003,898,922 字节输入，Release /O2，1 次 | 导入 9,990,000；跳过 10,000；错误 10,000；仅保留 200 条详情 | [程序报告](evidence/v1.1/bench/after-release-1gb-report.txt) |
  | 同一次大文件运行 | 导入耗时 8.859 秒；进程墙钟 9.080 秒；工作集峰值 5.46 MiB；采样私有内存峰值 1.14 MiB；退出码 0 | [内存采样记录](evidence/v1.1/bench/after-release-1gb.json) |

  10 万行对比使用同一份 3,146,588 字节文件，每次导入 98,000 行、跳过 2,000 行。两组均为 Debug，未拿 Debug 与 Release 计算修复倍率。采样前读取文件校验值且未清空系统缓存，结果属于本机热缓存条件。**计量边界**：详情预算不包含全部分配器和 vector 容量开销，不是进程内存硬上限；私有内存按 100 ms 采样，可能漏掉瞬时峰值。旧版 1 GB 运行未完成，没有完整耗时可作前后对比。

---

### 缺陷 A-7

**行号与大文件偏移类型**

- **复现步骤**：检查旧 int 行号及 Windows fseek(long)；超过 INT_MAX 行或 2 GiB 文件偏移存在窄化/溢出风险。
- **根因分析**：行号接口及成员为 int；Windows x64 的 long 仍是 32 位。
- **修复方法**：[公共类型](../include/csvp/types.h) 与解析器计数统一为 uint64_t，计数增量检查溢出；顺序读取去掉反复 fseek 和 long 转换。
- **验证证据**：类型与所有权约束测试在三套配置各 **1/1 成功、0 失败**。

  Debug 原始输出节选：

  ```text
  [       OK ] Parser.ResourceOwnershipAndCountersHaveSafeTypes (0 ms)
  ```

  大文件实测为 **9,990,000 行导入 + 10,000 行跳过 = 10,000,000 行**，退出码 **0**，见 [1 GB 程序报告](evidence/v1.1/bench/after-release-1gb-report.txt) 的 `run rows_imported rows_skipped` 汇总行及 [采样记录](evidence/v1.1/bench/after-release-1gb.json)。类型检查原始结果见 [Debug 日志](evidence/v1.1/tests/debug-tests.txt)、[Release 日志](evidence/v1.1/tests/release-tests.txt)、[x86 ASan 日志](evidence/v1.1/tests/asan-tests.txt)。验证局限：没有使用数十亿行或大于 2 GiB 文件做压力复现，1 GB 实测不构成极限边界证明。

---

### 缺陷 A-8

**双引号转义与末列引号**

- **复现步骤**：使用标准双写引号字段，以及最后一列为带引号的布尔字段。旧实现提前闭合，或额外增加空列。
- **根因分析**：没有明确的字段状态机；引用分支已提交字段，收尾又追加一次。
- **修复方法**：[字段解析](../src/parser.cpp) 区分普通、引用和闭合后的状态；双写引号还原为一个引号，统一提交字段，正确保留真实尾部空列。所有普通字段统一修剪空白，引用字段的空白保留。
- **验证证据**：双引号转义、末列引号/空列、空白及非法引号恢复共 4 项回归，三套配置各 **4/4 成功、0 失败**。

  Debug 原始输出节选：

  ```text
  [       OK ] Parser.QuotedEscapedQuote (0 ms)
  [       OK ] Parser.LastQuotedFieldAndTrailingEmptyField (3 ms)
  ```

  CLI 实际结果：`EscapedQuote` **导入 2 行**，`LastQuoted` **导入 1 行**，`TrimField` **导入 1 行**；三者均 **跳过 0 行、退出码 0**。完整输出见 [CLI 原始结果](evidence/v1.1/tests/cli-regressions.json)（查找上述 `Case`）和 [Debug 日志](evidence/v1.1/tests/debug-tests.txt)、[Release 日志](evidence/v1.1/tests/release-tests.txt)、[x86 ASan 日志](evidence/v1.1/tests/asan-tests.txt)。

---

### 缺陷 A-9

**CRLF 与 LF 混用及字段中的 CR**

- **复现步骤**：旧 sample_crlf.csv 实际没有任何 CR，表头与数据列数也不一致，不能支撑原断言。改用真实混合 CRLF/LF、引用字段内 CR 以及 CR/LF 跨 64 KiB 块的明确样本。
- **根因分析**：原验收数据损坏，字段内容与行终止符的契约不清；不能把正常 CRLF 的 CR 移除认定为错误。
- **修复方法**：只对 LF 之前的行终止 CR 做处理，引用字段内 CR 保留；无 LF 的文件尾段不无条件剥离 CR。允许合法上限内容加 CRLF，并对超长行消费到物理行尾后报错。
- **验证证据**：混合行尾、超长行恢复、CRLF 跨块、末行精确边界及 GBK 跨块共 5 项回归，三套配置各 **5/5 成功、0 失败**。

  Debug 原始输出节选：

  ```text
  [       OK ] Parser.CRLFLFMixedCR (0 ms)
  [       OK ] Parser.MaximumLineBoundaryAndOverlongLineRecovery (3 ms)
  [       OK ] Parser.CrlfSplitAt65536ByteBoundaryDoesNotGrowTheField (7 ms)
  ```

  完整结果：[Debug 日志](evidence/v1.1/tests/debug-tests.txt)、[Release 日志](evidence/v1.1/tests/release-tests.txt)、[x86 ASan 日志](evidence/v1.1/tests/asan-tests.txt)，查找上述测试名；修正样本的 SHA256 及本地测试版本记录见 [测试与样本清单](evidence/v1.1/metadata/local-test-manifest.json)。样本和测试继续仅保留本地，附件中的清单用于标识本次验证输入。

---

## 附件

- 工作区提交标准 skill 及采集脚本仅保留在本地，不随当前分支发布。
- [Git 历史快照](evidence/v1.1/metadata/git-history.txt)：定位基线 → 审查 → 提交标准 → 修复前测量 → 代码修复；最终文档提交及标签以仓库历史为准。
- [Debug 输出](evidence/v1.1/tests/debug-tests.txt) / [XML](evidence/v1.1/tests/debug-tests.xml)
- [Release 输出](evidence/v1.1/tests/release-tests.txt) / [XML](evidence/v1.1/tests/release-tests.xml)
- [ASan 输出](evidence/v1.1/tests/asan-tests.txt) / [XML](evidence/v1.1/tests/asan-tests.xml) / [运行库导入证明](evidence/v1.1/compilation/asan-runtime-imports.txt)
- [相同用例矩阵](evidence/v1.1/tests/test-matrix.json)、[CLI 观察](evidence/v1.1/tests/cli-regressions.json)、[本地测试与样本校验值](evidence/v1.1/metadata/local-test-manifest.json)
- [最终构建输出](evidence/v1.1/compilation/build-final.txt)、[CTest 输出](evidence/v1.1/tests/ctest-final.txt)、[实际编译选项](evidence/v1.1/compilation/build-options.json)、[环境](evidence/v1.1/metadata/environment.json)
- [修复前 Debug](evidence/v1.1/bench/before-debug-100k.json)、[修复后 Debug](evidence/v1.1/bench/after-debug-100k.json)、[修复后 Release](evidence/v1.1/bench/after-release-100k.json)
- [1 GB 实测](evidence/v1.1/bench/after-release-1gb.json)、[程序报告](evidence/v1.1/bench/after-release-1gb-report.txt)、[汇总](evidence/v1.1/metadata/summary.json)

测试源码、样本、第三方库及构建产物继续遵守用户指定的本地排除规则。当前分支包含修复实现、报告和证据，AGENTS.md、工作区 skill 及采集脚本仅保留本地；在另一台机器重跑测试仍需准备对应本地资料。临时父工程构建/测试桥接已移除，父工作区配置已恢复；正常 Debug/Release 输出目录也已更新为本次实现。
