# CSV Import Pipeline

C++17 CSV 解析与校验库。v1.1 完成 Part A 的同步路径修复，详见 [版本报告](docs/REPORT_v1.1.md)。[原始审查](docs/archive/PART_A_AUDIT.md) 保留为历史基线，不是当前待修列表。

报告与结果统一从 [文档索引](docs/README.md) 进入。**v1.3.0 完成 Part C：有界内存与完整错误归档**。原始 1 GB 文件处理 10,000,000 行，全部 10,000 条错误已落盘并逐条核对；GetProcessMemoryInfo 峰值工作集约 5.32 MiB、峰值私有提交量约 1.17 MiB。详见 [版本报告](docs/REPORT_v1.3.0.md) 和 [完整错误日志](docs/evidence/v1.3.0/errors/sample_1gb.jsonl)。

## 发布范围

仓库保留核心库、压测程序、CMake 配置、报告和小型证据。
按用户指定范围，data、tests、third_party、Cursor 配置、AGENTS.md、.agents 目录及旧机器脚本仅留在本地，不跟踪。工作区 skill 与采集脚本仍可在本机使用。新克隆默认构建库与压测程序，测试需要本地准备完整测试源码、样本和 Google Test 1.14。

成功行数表示解析和校验通过，不代表写入数据库。v1.1 涵盖 Part A，v1.2.0 增加后台导入和限时停止，v1.3.0 增加全量错误落盘、背压协调和进程峰值内存证明；各版本报告独立保留。

## 结构

| 位置 | 职责 |
|---|---|
| [include/csvp](include/csvp) | 公共配置、严格类型访问器、错误与统计接口 |
| [src/parser.cpp](src/parser.cpp) | 64 KiB 顺序缓冲、行边界、编码字节检查、引号状态机 |
| [src/validator.cpp](src/validator.cpp) | 必填、类型、范围与正则校验 |
| [src/pipeline.cpp](src/pipeline.cpp) | 同步/后台导入、协作停止、有界错误详情及统计快照 |
| [src/error_log.cpp](src/error_log.cpp) | 独占创建、固定缓冲的完整 JSONL 错误归档 |
| [bench/bench_import.cpp](bench/bench_import.cpp) | 压测、报告与输入输出路径检查 |
| [tools/verify_archive.cjs](tools/verify_archive.cjs) | 对指定 1 GB 样本独立流式核对行数及所有错误原文 |
| [CMakePresets.json](CMakePresets.json) | 本机普通构建和验证预设 |
| [docs](docs) | 报告模板、版本报告、测试与性能证据 |

## 构建和运行

要求 C++17、CMake 3.16 以上；预设要求 CMake 3.21 以上。
本机预设指定 VS 2022、MSVC v143 和 D:/Software/VSBuildTools2022。
换电脑时调整 CMAKE_GENERATOR_INSTANCE，或删除该项让 CMake 自动发现实例。

在 VS Code 中将本项目选为活动项目，选择配置预设 CSV: Visual Studio 2022 x64，
再选择构建预设 CSV: Debug 或 CSV: Release，执行 CMake: Configure 和 CMake: Build。

从项目根目录运行（用自己的输入和报告路径替换示例）：

    .\build\Release\csvp_bench.exe 'D:\csv-input\input.csv' 3 'D:\csv-output\report.txt'

参数依次是 CSV、重复次数（默认 3）、报告路径（默认 bench_report.txt），以及可选的错误日志路径。默认错误日志与报告同目录，名为 `<报告主文件名>.errors.jsonl`；多轮运行分别使用 `.run-1.jsonl` 等文件。
输出父目录须存在，输入、报告、错误日志必须是不同文件。错误日志必须尚不存在，不覆盖已有归档；再次运行请选择新日志名。任何归档写入、关闭、报告或致命输入失败都返回非零退出码。
Debug 程序在 build/Debug，Release 程序在 build/Release。

压测规则按列顺序为 id（必填整数）、name（必填字符串）、age（必填整数）、score（必填浮点数）、active（可选布尔值）。不自动按表头名称重新排列字段。

对原始大文件运行并独立检查完整归档（从项目根目录执行，输出文件名须尚未使用）：

    .\build\Release\csvp_bench.exe .\data\sample_1gb.csv 1 .\build\v13-report.txt .\build\v13-errors.jsonl
    node .\tools\verify_archive.cjs .\data\sample_1gb.csv .\build\v13-errors.jsonl .\build\v13-verification.json

Node 仅供独立证据核对工具使用，不是 C++ 库的运行依赖。该核对器专用于题目给定的 10,000,000 行样本；它逐行检查数据分布、行数守恒以及全部错误的行号、原因、原始字节。核对耗时不算作导入耗时。

## 解析与校验接口边界

- 文件路径接受 UTF-8；Windows 内部使用宽字符文件接口，命令行工具处理 Unicode 参数。
- 读取状态区分 Row、End、Error；可恢复错误消费当前物理行后继续，致命 I/O 失败不当成普通坏行。
- 默认行内容上限为 16 MiB − 1（不含 CRLF 终止符）；max_line_bytes 可调整。超长行明确报错，保留的原始行前缀标记为截断。
- UTF-8 拒绝非法序列并支持首行 BOM。GBK 保留原始字节、验证双字节结构，不转码或查完整字符映射表；合法尾字节不会被当成自定义分隔符。
- 支持双引号引用、双写引号转义、引用字段内的 CR，以及混合 CRLF/LF。不支持字段中含 LF 的跨物理行记录。
- toInt/toDouble/toBool 对格式非法输入抛 invalid_argument；索引越界和数值溢出抛 out_of_range。tryToInt/tryToDouble 返回 optional，可区分合法零与失败。
- Parser 行号包含表头和空物理行；管道 total_lines 统计数据行（含跳过）。行号和累计统计使用 uint64_t。
- skip_bad_rows=true 跳过列数、可恢复解析、编码及字段校验错误；严格模式抛 runtime_error。无法打开文件或致命 I/O 令 Result.success=false。
- 默认保留最多 200 条错误详情、4 MiB 的对象与字符串内容预算，并持续累计 error_count/rows_skipped。截断会明确标记。
- error_limit=0 仅关闭数量限制；max_error_bytes=0 仅关闭字节预算。两者都为 0 才完全无限。该预算不是 allocator 或进程内存的精确硬上限。

## v1.3.0 Part C 接口

- 库调用方设置 `Config.error_log_path` 启用完整归档；留空保留旧版仅返回内存详情的行为。v1.3 压测程序默认启用完整归档。
- 所有坏行在读取下一行前同步写入 JSONL；错误增多只增加磁盘文件，不形成无界内存队列。内存 `Result.errors` 仍遵守数量/字节限额，归档不受该限额影响。
- 每条日志包含 `line_number`、`raw_line` 和 `errors`（列号、列名、错误码、原因、字段原值）。行号包含表头，原文不含物理 LF/CRLF 终止符。JSON 字符串将原字节映射到 U+0000…U+00FF，非 ASCII 使用 `\u00XX`；JSON 解码后按 Latin-1 还原字节，再按原 CSV 编码解释，UTF-8/GBK 原字节都不丢失。
- `Result.errors_written` 记录已写出的完整记录数；只有显式刷新及关闭成功，`error_log_complete` 才为 true。该标志仅覆盖已观察错误，确认完整导入还须检查 `success && !cancelled` 及行数。
- 磁盘短写、刷新/关闭失败抛出异常；后台模式经 future 交付。原文被解析器行长上限截断时，完整归档模式明确失败，不把前缀冒充完整错误。
- 本次压测使用 1 MiB 物理行上限、200 条/4 MiB 内存预览、64 KiB 输入及输出缓冲。对给定五列样本的峰值内存由 Windows API 实测；这不是对任意超多列输入或用户主动关闭预算的通用 50 MB 保证。

## v1.2.0 Part B 接口

- `importStreaming(path, progress)` 复制路径并在后台读取、解析和校验；返回 promise 支持的 future，丢弃 future 不会隐式等待线程。
- 每处理1000条数据记录调用回调，好行/跳过的坏行都计入，表头/空行不计入；不足1000条的尾批不额外回调，允许空回调。
- 回调返回 false 时返回已处理部分并置 `Result.cancelled=true`；`success` 仍表示没有致命输入错误。回调及严格模式异常由 `future.get()` 重新抛出。
- `shutdown(timeout)` 关闭实例并请求停止，以同一个截止时间限制锁等待和任务等待；零/负超时直接返回。之后不再接受新导入。
- `pendingCount()` 返回导入任务计数（0或1），不是剩余行数；超时不提前清零。计数可能先于结果 future 就绪短暂归零，获取结果以 future 为准。回调内部调用 shutdown 不会等待自己。
- 单个实例最多一个活跃导入；同步/异步重入抛出logic_error。正常完成或回调取消后可以再导入，显式shutdown后须使用新实例。
- `stats()` 可并发读取一致快照，每1000条记录及结束/异常时发布。行解析、校验和回调不持有状态锁。
- 析构请求取消但不等待，后台拥有独立引擎及共享状态；不能强杀阻塞I/O或用户回调。调用方负责让回调所捕获的对象活到任务结束，需等待结果时使用future。

## 本地验证

当前业务提交 `b0d830d` 在 Debug、Release 和 x86 ASan 下均为 **120/120 通过**，无失败或禁用，包含短写、刷新失败和关闭失败注入。见 [逐项结果](docs/evidence/v1.3.0/metadata/validation.json) 与 [测试日志](docs/evidence/v1.3.0/tests)。UI 文件夹空项不计入测试数，逐用例覆盖的 XML 不作为全套结果。

原始 1 GB 文件导入耗时 **8.90902 秒**，999 万行成功、1 万行错误完整归档；[独立核对结果](docs/evidence/v1.3.0/metadata/archive-verification.json) 验证全部错误与源文件一致。内存与吞吐、缓存条件及测量范围见 [Part C 报告](docs/REPORT_v1.3.0.md)。

BUILD_TESTS 默认 OFF，BUILD_BENCH 默认 ON，ENABLE_ASAN 默认 OFF；已有缓存的显式值不被默认值覆盖。

本地具备 tests、data、third_party/googletest 后使用以下预设构建，再通过 CMake: Run Tests 验证：

| 配置预设 | 构建及测试预设 | 用途 |
|---|---|---|
| csv-msvc-x64-validation | csv-validation-debug | x64 Debug /Od |
| csv-msvc-x64-validation | csv-validation-release | x64 Release /O2 |
| csv-msvc-x86-asan | csv-asan | x86 ASan RelWithDebInfo |

本次 ASan 证据来自 x86；业务库和 Google Test 使用匹配的插桩，所需运行库复制到程序目录，[导入表](docs/evidence/v1.3.0/compilation/asan-runtime-imports.txt) 确认使用 `clang_rt.asan_dynamic-i386.dll`。不将本次结果表述为 x64 ASan 验证。

测试源码和样本仍按既有规则不发布；证据中的用例清单、校验值和输出用于记录实际验证内容，不能替代本地测试资料。

## 许可证

沿用原练习声明的 BSD-3-Clause 许可及 fast-cpp-csv-parser 设计来源说明。
