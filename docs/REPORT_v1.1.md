# CSV Import Pipeline v1.1 缺陷修复与验证报告

执行：Codex 按用户指定范围实施。日期：2026-10-26。修复代码提交：`8c2f605`。最终版本标签：`v1.1`。提交时间记录见附件。

本报告按 [REPORT_TEMPLATE.md](REPORT_TEMPLATE.md) 填写。范围为用户列出的 **9 项 Part A** 及其直接关联的数据安全问题；Part B 未实现。初始版本、缺陷定位、修复前验证、代码修复与验收文档保留为不同提交。

## 验收结果

| 配置 | 执行用例 | 失败 | 结果 |
|---|---:|---:|---|
| x64 Debug，/Od | 79 | 0 | 通过 |
| x64 Release，/O2 | 79 | 0 | 通过，同一组断言与 Debug 一致 |
| x86 RelWithDebInfo，AddressSanitizer | 79 | 0 | 通过，退出 0，无 ASan 诊断 |
| 独立命令行回归 | 12 | 0 | 通过 |

共声明 80 个单元用例，仅 Part B 的 Concurrent.DISABLED_ShutdownNoDeadlock 保持禁用。原先 5 个 Parser 的 DISABLED 前缀已去掉；ErrorLimit 无 GTEST_SKIP，真实检查总处理量、错误总数和详情上限。两个明确的 Part B 骨架检查不代表特性完成。

本机只有 x86 ASan 运行库，因此 ASan 验证为 x86；不将其说成 x64 ASan 证据。原始输出及 XML、实际编译选项、运行库导入记录均附在下方。ASan 零诊断不等于穷尽证明所有内存问题或所有输入行为。

## Part A 缺陷修复

### 缺陷 A-1 文件尾没有换行导致末行丢失

- **复现步骤**：五列表头后放两条合法记录，最后一条不写 LF。旧程序只导入第一条且退出 0；原始证据见 [基线定向输出](evidence/part-a-debug-probes.txt) 的 EOF。
- **根因分析**：旧读取函数把“缓冲内没找到 LF”直接当成文件结束。
- **修复方法**：[解析器](../src/parser.cpp) 使用 64 KiB 顺序缓冲。文件真实结束时交付非空尾段，区分 Row、End、Error；同时处理恰好位于块边界的末行。
- **验证证据**：Parser.NoTrailingNewline、HeaderAndFinalRecordWithoutNewlineRemainReadable、Exact65536ByteFinalRecordWithoutNewline 在三套配置均通过；命令行 EOF 回归通过。

### 缺陷 A-2 解析器重用的状态与资源管理

- **复现步骤**：同一解析器读取两个不同文件；再测先打开不存在文件、后打开有效文件。旧实现行号、已读字节和错误残留，每次成功重开还覆盖旧 malloc 指针。
- **根因分析**：重置只覆盖表头及偏移；资源拥有者允许默认浅拷贝。
- **修复方法**：[解析器接口](../include/csvp/parser.h) 与实现采用 RAII 文件所有权和可复用缓冲，禁止复制及移动，重开统一重置状态。每次错误有明确类别及原始行。
- **验证证据**：ReentryState、ReopenAfterFailureClearsOldErrorAndHeader、RepeatedOpenReadCyclesKeepIndependentResults、ResourceOwnershipAndCountersHaveSafeTypes 均通过；ASan 执行同组用例无诊断。另有实测内存数据，不把 ASan 结果单独当作泄漏穷尽证明。

### 缺陷 A-3 跳过坏行仍中止或静默丢后续数据

- **复现步骤**：正常行、列数错误行、正常行；旧 skip_bad_rows=true 仍退出 1。未闭合引号后放正常行，旧程序只导入前段却退出 0。
- **根因分析**：列数检查直接抛出；读取接口的 false 同时表示 EOF 和错误，管道没有区分。
- **修复方法**：[管道](../src/pipeline.cpp) 对列数、解析、编码和校验错误统一计数、收集和跳过；严格模式保留明确异常。无法打开及致命 I/O 通过 success=false 表达。无表头按规则列数检查，失败与正常结束都完成同步统计。
- **验证证据**：SkipBadRows、ColumnMismatchSkipsAndContinues、ParseErrorSkipsAndContinues、StrictModeRejectsColumnAndParseAndValidationErrors、NoHeaderUsesRuleCount 等通过；命令行 ColumnMismatch、ParseError 从旧失败变为通过。

### 缺陷 A-4 GBK 编码与分隔符边界

- **复现步骤**：使用 GBK、分隔符 |、无表头，输入字节 81 7C 7C 78 0A。81 7C 是合法双字节结构，期望两列；旧代码按字节把尾字节当成分隔符。
- **根因分析**：编码配置没有参与字段边界判定。原来的 sample_gbk.csv 还包含非法 D4 2C，不能证明合法 GBK 包含默认逗号。
- **修复方法**：[解析器](../src/parser.cpp) 整体识别合法双字节结构，再判断分隔符；非法结构报告 EncodingError，错误行消费后可继续。校正本地 GBK 样本，并保留非法输入回归。Windows 路径统一接受 UTF-8 后转换为宽字符，消除正常测试文件打不开的问题。
- **验证证据**：ChineseGBKEncoding、LegitimateGbkTrailByteIsNotTheCustomDelimiter、InvalidGbkDoesNotConsumeARealComma、GbkCharacterSplitAt65536ByteBoundaryKeepsItsTrailByte、ChineseUtf8AbsolutePathAndContent 均通过。
- **边界说明**：GBK 校验字节结构且保留原字节，不转码，也不穷尽检查整个字符映射表。默认逗号 2C 不属于合法 GBK 尾字节，不能靠吞掉真实逗号让旧错误样本过关。

### 缺陷 A-5 非法数值与真实零不可区分

- **复现步骤**：对 0、N/A、空字符串、越界索引、溢出数字、12abc、1.5abc 分别调用转换。旧访问器返回零或接受数字前缀；校验还会接受溢出数值、允许 NaN 绕范围。
- **根因分析**：转换忽略完整消费、ERANGE 和有限值，错误统一折叠为零。
- **修复方法**：[Row](../include/csvp/row.h) 严格转换并明确抛 invalid_argument/out_of_range，提供 optional 访问器；布尔转换同样区分失败与 false。[校验器](../src/validator.cpp) 复用这些转换，在既有 has_range 框架内处理非有限值、分数边界和缺失必填列，没有另造校验框架。
- **验证证据**：7 项 Row 测试及 17 项 Validator 测试全部通过，包括 IntegerOverflowAndNumericSuffixesAreRejected、NanCannotBypassRange、FractionalIntegerRangeBoundsAreNotTruncated；命令行 NumericOverflow、MissingColumn 均通过。

### 缺陷 A-6 内存增长与错误详情上限

- **复现步骤**：大量列数正确但类型非法的记录。旧 bench 先收集全部错误，最后才截取 200 条报告样本；同时每处理一行重复读取约 16 MiB。
- **根因分析**：报告样本上限没有约束运行中的错误结果；缓冲中未处理的数据没有复用。
- **修复方法**：顺序读取、复用块中剩余行；默认最多 200 条详情和 4 MiB 对象/字符串内容预算，满额后继续计数及验证而不复制完整错误字段。原始行被截断时显式传播标记。error_limit=0 或 max_error_bytes=0 分别只关闭对应限制。
- **验证证据**：ErrorLimit 使用 1000 条坏行和 1 条好行，核对真实处理量；ByteBudgetDoesNotLoseAggregateCounts、LongErrorDetailsRespectByteBudget、ParserTruncatedErrorPropagatesToResultAndStatistics 等通过。1000 万行输入实际工作集峰值 **5.46 MiB**，采样私有内存峰值 **1.14 MiB**。
- **计量边界**：详情字节预算不包含全部 allocator/vector capacity 开销，因此不是进程内存硬上限；采样私有内存也可能漏掉采样间隔中的瞬时峰值。见独立实测表。

### 缺陷 A-7 行号与大文件偏移类型

- **复现步骤**：检查旧 int 行号及 Windows fseek(long)；超过 INT_MAX 行或 2 GiB 文件偏移存在窄化/溢出风险。
- **根因分析**：行号接口及成员为 int；Windows x64 的 long 仍是 32 位。
- **修复方法**：[公共类型](../include/csvp/types.h) 与解析器计数统一为 uint64_t，计数增量检查溢出；顺序读取去掉反复 fseek 和 long 转换。
- **验证证据**：ResourceOwnershipAndCountersHaveSafeTypes 验证计数接口类型和不可复制约束，三种架构配置均通过；1 GB、1000 万行实际计数正确。未以数十亿行或大于 2 GiB 文件进行压力复现，不能把 1 GB 测试称为穷尽边界验证。

### 缺陷 A-8 双引号转义与末列引号

- **复现步骤**：使用标准双写引号字段，以及最后一列为带引号的布尔字段。旧实现提前闭合，或额外增加空列。
- **根因分析**：没有明确的字段状态机；引用分支已提交字段，收尾又追加一次。
- **修复方法**：[字段解析](../src/parser.cpp) 区分普通、引用和闭合后的状态；双写引号还原为一个引号，统一提交字段，正确保留真实尾部空列。所有普通字段统一修剪空白，引用字段的空白保留。
- **验证证据**：QuotedEscapedQuote、LastQuotedFieldAndTrailingEmptyField、TrimsEveryOrdinaryFieldButPreservesQuotedSpaces、MisplacedQuotesAreErrorsAndTheNextRecordSurvives 通过；CLI EscapedQuote、LastQuoted、TrimField 通过。

### 缺陷 A-9 CRLF 与 LF 混用及字段中的 CR

- **复现步骤**：旧 sample_crlf.csv 实际没有任何 CR，表头与数据列数也不一致，不能支撑原断言。改用真实混合 CRLF/LF、引用字段内 CR 以及 CR/LF 跨 64 KiB 块的明确样本。
- **根因分析**：原验收数据损坏，字段内容与行终止符的契约不清；不能把正常 CRLF 的 CR 移除认定为错误。
- **修复方法**：只对 LF 之前的行终止 CR 做处理，引用字段内 CR 保留；无 LF 的文件尾段不无条件剥离 CR。允许合法上限内容加 CRLF，并对超长行消费到物理行尾后报错。
- **验证证据**：CRLFLFMixedCR、MaximumLineBoundaryAndOverlongLineRecovery、CrlfSplitAt65536ByteBoundaryDoesNotGrowTheField、精确 65536 字节末行及 GBK 跨块用例通过。修正后的本地样本与测试文件校验值在附件中，未把这些忽略文件强制推送。

## Part B 特性实现

### B-1 流式导入

不在本次范围内。importStreaming 继续返回占位结果，没有把同步修复描述为后台线程实现。对应骨架检查通过仅说明当前占位契约仍成立。

### B-2 优雅停止

不在本次范围内。shutdown 未实现后台取消或有界等待；ShutdownNoDeadlock 保持禁用。stats 仅承诺同步返回/抛异常后查询，不承诺与 importSync 并发读写。相邻原子计数器未被当成本次性能退化的已证实原因。

## Part C 补充结果

### C-1 构建与告警

MSVC 19.44.35229.0，C++17，业务目标使用 /W4。最终三套构建成功，未见业务编译告警。
本机 x64 ASan 运行库缺失，因此使用已安装的 x86 ASan；同时给 Google Test 与业务目标启用一致插桩，解决 annotate_string/annotate_vector 链接不匹配。没有通过关闭容器注解来掩盖不匹配。

Google Test 1.14 的 XML 窄路径写入遇到中文工作区问题，证据采集脚本改为先写独占临时目录，再复制到项目；实际项目中文路径测试仍使用原中文路径并通过。首次失败输出与最终正常退出输出均保留。

### C-2 同配置性能对比

输入均为同一份 3,146,588 字节的 10 万行文件，SHA256 相同，各运行三次，每轮导入 98,000 行、跳过 2,000 行。

| 程序 | 配置 | 平均导入耗时 | 进程工作集峰值 |
|---|---|---:|---:|
| 修复前 | Debug /Od | 37.863 秒 | 23.00 MiB |
| v1.1 | Debug /Od | 1.020 秒 | 6.83 MiB |
| v1.1 | Release /O2 | 0.0626 秒 | 5.67 MiB |

同 Debug 配置下，本次系列平均耗时改善约 **37.1 倍**。这包含顺序读取与错误保留策略等整套修复效果，不是单独隔离某一条优化的实验。Release 数值另列，不用于计算修复倍率。

每次采样先读取输入校验值，没有清空操作系统缓存；这是本机热缓存条件下的测量，非所有机器的性能保证。

### C-3 1 GB 内存实测与额外安全修复

| 项目 | 实测值 |
|---|---|
| 配置 | x64 Release /O2 |
| 输入 | 1,003,898,922 字节，约 1.004 GB |
| 重复次数 | 1 |
| 成功 / 跳过 | 9,990,000 / 10,000 |
| 程序内导入耗时 | 8.859 秒 |
| 整个进程墙钟时间 | 9.080 秒 |
| 峰值工作集 | 5.46 MiB，操作系统进程峰值计数 |
| 峰值私有内存 | 1.14 MiB，100 ms 周期采样 |
| 退出码 / 超时 | 0 / 否 |

操作系统文件缓存不计为本进程工作集。旧版 1 GB 的历史长时间运行未完成，本报告不编造其完整耗时，也不拿旧 Debug 与新 Release 计算修复倍率。

另外修复了报告覆盖输入、报告写失败仍提示成功、致命输入失败仍退出 0、重复次数参数宽松转换等直接关联安全问题。12 项 CLI 回归全部通过，覆盖原审查的 EOF、引号、列数、数值溢出、缺列及输出路径行为；危险覆盖复现只使用临时副本。

## 附件

- 工作区提交标准 skill 及采集脚本仅保留在本地，不随当前分支发布。
- [Git 历史快照](evidence/v1.1/git-history.txt)：定位基线 → 审查 → 提交标准 → 修复前测量 → 代码修复；最终文档提交及标签以仓库历史为准。
- [Debug 输出](evidence/v1.1/debug-tests.txt) / [XML](evidence/v1.1/debug-tests.xml)
- [Release 输出](evidence/v1.1/release-tests.txt) / [XML](evidence/v1.1/release-tests.xml)
- [ASan 输出](evidence/v1.1/asan-tests.txt) / [XML](evidence/v1.1/asan-tests.xml) / [运行库导入证明](evidence/v1.1/asan-runtime-imports.txt)
- [相同用例矩阵](evidence/v1.1/test-matrix.json)、[CLI 观察](evidence/v1.1/cli-regressions.json)、[本地测试与样本校验值](evidence/v1.1/local-test-manifest.json)
- [最终构建输出](evidence/v1.1/build-final.txt)、[CTest 输出](evidence/v1.1/ctest-final.txt)、[实际编译选项](evidence/v1.1/build-options.json)、[环境](evidence/v1.1/environment.json)
- [修复前 Debug](evidence/v1.1/before-debug-100k.json)、[修复后 Debug](evidence/v1.1/after-debug-100k.json)、[修复后 Release](evidence/v1.1/after-release-100k.json)
- [1 GB 实测](evidence/v1.1/after-release-1gb.json)、[程序报告](evidence/v1.1/after-release-1gb-report.txt)、[汇总](evidence/v1.1/summary.json)

测试源码、样本、第三方库及构建产物继续遵守用户指定的本地排除规则。当前分支包含修复实现、报告和证据，AGENTS.md、工作区 skill 及采集脚本仅保留本地；在另一台机器重跑测试仍需准备对应本地资料。临时父工程构建/测试桥接已移除，父工作区配置已恢复；正常 Debug/Release 输出目录也已更新为本次实现。
