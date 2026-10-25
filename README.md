# CSV Import Pipeline v1.1

C++17 CSV 解析与校验库。v1.1 完成 Part A 的同步路径修复，详见 [版本报告](docs/REPORT_v1.1.md) 和 [工作区提交标准 skill](.agents/skills/csvp-delivery/SKILL.md)。[原始审查](docs/PART_A_AUDIT.md) 保留为历史基线，不是当前待修列表。

## 发布范围

仓库保留核心库、压测程序、CMake 配置、skill、报告和小型证据。
按用户指定范围，data、tests、third_party、Cursor 配置及旧机器脚本仅留在本地，不跟踪。新克隆默认构建库与压测程序，测试需要本地准备完整测试源码、样本和 Google Test 1.14。

成功行数表示解析和校验通过，不代表写入数据库。importStreaming、shutdown 仍为 Part B 骨架，v1.1 不实现后台导入或优雅停止。

## 结构

| 位置 | 职责 |
|---|---|
| [include/csvp](include/csvp) | 公共配置、严格类型访问器、错误与统计接口 |
| [src/parser.cpp](src/parser.cpp) | 64 KiB 顺序缓冲、行边界、编码字节检查、引号状态机 |
| [src/validator.cpp](src/validator.cpp) | 必填、类型、范围与正则校验 |
| [src/pipeline.cpp](src/pipeline.cpp) | 坏行策略、有界错误详情及同步统计 |
| [bench/bench_import.cpp](bench/bench_import.cpp) | 压测、报告与输入输出路径检查 |
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

参数依次是 CSV、重复次数（默认 3）和报告路径（默认 bench_report.txt）。
报告父目录须存在，报告不得与输入指向同一个文件。致命输入错误或报告写入失败返回非零退出码。
Debug 程序在 build/Debug，Release 程序在 build/Release。

压测规则按列顺序为 id（必填整数）、name（必填字符串）、age（必填整数）、score（必填浮点数）、active（可选布尔值）。不自动按表头名称重新排列字段。

## v1.1 接口与边界

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
- stats() 在同步导入返回或抛异常后查询；本版本不承诺与 importSync 并发读写同一个对象。

## 本地验证

BUILD_TESTS 默认 OFF，BUILD_BENCH 默认 ON，ENABLE_ASAN 默认 OFF；已有缓存的显式值不被默认值覆盖。

本地具备 tests、data、third_party/googletest 后使用以下预设构建，再通过 CMake: Run Tests 验证：

| 配置预设 | 构建及测试预设 | 用途 |
|---|---|---|
| csv-msvc-x64-validation | csv-validation-debug | x64 Debug /Od |
| csv-msvc-x64-validation | csv-validation-release | x64 Release /O2 |
| csv-msvc-x86-asan | csv-asan | x86 ASan RelWithDebInfo |

本机缺少 x64 ASan 运行库，本次 ASan 证据来自 x86。业务库和 Google Test 使用匹配的插桩，所需运行库复制到程序目录。

测试源码和样本仍按既有规则不发布；证据中的用例清单、校验值和输出用于记录实际验证内容，不能替代本地测试资料。

## 许可证

沿用原练习声明的 BSD-3-Clause 许可及 fast-cpp-csv-parser 设计来源说明。
