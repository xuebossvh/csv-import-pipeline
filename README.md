# CSV Import Pipeline

一个轻量级的 CSV 数据导入与校验管道库，基于 `fast-cpp-csv-parser` 的设计思路改编。

## 仓库发布范围

本仓库保留核心库、压测程序、CMake 配置和审查文档。
`data/`、`tests/`、`third_party/`、Cursor 配置和 `build_msvc.bat`
作为本地练习资料或机器专用文件，不再跟踪；取消跟踪不会删除本地副本。
测试与样本仍可在本机使用，新克隆默认只构建核心库和压测程序。
早期提交中的文件仍保留在 Git 历史中。

## 功能概述

- CSV 文件解析（支持 UTF-8 / GBK 编码）
- 可配置的字段校验（类型、范围、正则、必填）
- 坏行跳过与错误收集
- 同步导入模式
- 流式导入模式（待实现）

## 代码架构

整体是一条「解析 → 校验 → 编排」的流水线，三个核心类各管一段，互相之间只通过公开头文件（`include/csvp/`）打交道：

```
             CSV 文件
                │
                ▼
      ┌────────────────────┐
      │      CsvParser     │  打开文件、按分隔符切分字段、逐行产出 Row
      │     (parser.h)     │  维护行号 / 已读字节数
      └─────────┬──────────┘
                │  Row（一行的字段列表，全部是字符串）
                ▼
      ┌────────────────────┐
      │   FieldValidator   │  按列规则（FieldRule）校验每个字段：
      │    (validator.h)   │  类型 / 必填 / 范围 / 正则
      └─────────┬──────────┘
                │  std::vector<CellError>（空 = 该行全部通过）
                ▼
      ┌────────────────────┐
      │   ImportPipeline   │  顶层入口：把上面两步串起来，
      │    (pipeline.h)    │  负责列数检查、错误收集、统计
      └─────────┬──────────┘
                │  Result（rows_imported / rows_skipped / errors / elapsed_ms）
                ▼
        调用方（csvp_tests / csvp_bench / 业务代码）
```

### 分层与依赖

| 模块 | 文件 | 职责 | 依赖 |
|---|---|---|---|
| 基础类型 | `include/csvp/types.h` | `ErrorCode`、`FieldType`、`FieldRule`、`CellError`、`RowError`、`Stats` 这些纯数据结构 | 无 |
| 行容器 | `include/csvp/row.h` | `Row`：一行数据的字段列表，提供 `toInt/toDouble/toBool/toString` 类型访问器 | types.h |
| 解析器 | `include/csvp/parser.h` + `src/parser.cpp` | `CsvParser`：`parse()` 打开文件、`readRow()` 逐行解析；内部行缓冲区为手动内存管理（malloc/free） | types.h、row.h |
| 校验器 | `include/csvp/validator.h` + `src/validator.cpp` | `FieldValidator`：规则由外部注入（`addRule/addRules`），`validate()` 校验一整行、`validateField()` 校验单个字段 | types.h、row.h |
| 管道 | `include/csvp/pipeline.h` + `src/pipeline.cpp` | `ImportPipeline`：顶层编排，组合 Parser + Validator，输出 `Result` 和 `Stats` | 上面全部 |

依赖方向是单向的：`types.h` 在最底层，`pipeline` 在最上层；`Parser` 和 `Validator` 互不认识，都只认 `Row` 和 `FieldRule`，由 `Pipeline` 负责把它们接在一起。因此解析和校验都可以单独构建、单独测试。

### 三个实现文件

**`src/parser.cpp` —— 解析器实现**

- `parse()`：关闭上一个打开的文件、以二进制方式打开新文件、分配行缓冲区、重置表头与读取偏移，然后按 `has_header` 读入表头；
- `readRow()`：把下一行读进来，处理行尾、跳过空行，再调用字段切分，成功后行号加一；
- `readLineFromFile()`：根据当前文件偏移把下一行读入缓冲区，同时更新偏移量和已读字节数（`bytesRead()` 的数据来源）；
- `splitLineToFields()` / `handleQuotedField()`：按 `delimiter` 切分普通字段和引号字段，`trim_spaces` 开启时修剪字段前后的空格；
- `isTrailingByte()`：按字节范围判断多字节字符的后续字节，供编码相关逻辑使用；
- 析构函数负责关闭文件和释放行缓冲区（`malloc/free` 手动管理）。

**`src/validator.cpp` —— 校验器实现**

- `addRule()` / `addRules()`：保存外部传入的规则列表。规则是**按列顺序**存放的，校验时用列索引取对应规则；`FieldRule::name` 只用于填充报错信息里的列名；
- `validate()`：逐列校验一行，收集所有非 `Ok` 的结果；实际校验的列数取「行字段数」和「规则数」的较小值；
- `validateField()`：单个字段的检查顺序是 **必填 → 类型 → 范围 → 正则**。必填字段为空直接报 `RequiredFieldMissing`；非必填的空值跳过后续检查直接通过；
- `validateType()`：`Int` 用 `strtol`、`Double` 用 `strtod`，并要求整串都被消费完才算合法（`12abc` 判错）；`Bool` 接受 `true/false/TRUE/FALSE/1/0/yes/no`；`String` 一律通过；
- `validateRange()`：只在 `has_range` 为真时生效，按 `min_val/max_val` 比较；
- `validateRegex()`：用 `std::regex_match` 做整串匹配，正则本身写错时会捕获 `regex_error` 并转成一条错误信息。

**`src/pipeline.cpp` —— 管道实现**

- 构造函数：保存 `Config` 和规则列表，并把规则交给内部的 `FieldValidator`；
- `importSync()`：建解析器 → `parse()` 打开文件 → 循环 `readRow()` → 列数检查 → 交给 `validateAndCollect()` → 结束后填写 `Result` 和 `Stats`（行数、字节数、耗时）；
- `validateAndCollect()`：校验通过则 `rows_imported++`；失败则组装 `RowError`（行号、整行、`CellError` 列表），按 `skip_bad_rows` 决定记录后继续还是终止；错误列表受 `error_limit` 约束（0 表示不限）；
- `importStreaming()` / `shutdown()`：目前是骨架，函数体里的 TODO 注释写明了 Part B 要满足的接口契约，返回值是已完成部分的 `future`；
- `stats()`：返回统计快照，流式实现后由原子计数器提供实时进度。

### 一行数据的生命周期

以 `importSync("data.csv")` 为例：

1. `ImportPipeline` 用配置里的 `CsvParser::Config` 建一个解析器，`parse()` 打开文件并读出表头（`has_header=true` 时）；
2. 循环调用 `readRow(row)`，每得到一行就先做列数检查（和表头比），再交给 `FieldValidator::validate(row)`；
3. 校验通过 → `rows_imported++`；校验失败 → 组装 `RowError`（行号 + 整行 + 各字段的 `CellError`），按 `Config` 决定记入错误列表后继续（`skip_bad_rows=true`，受 `error_limit` 约束）还是失败退出；
4. 循环结束后把行数、字节数、耗时写进 `Result` 和 `Stats` 返回。

错误信息是两层的：`RowError`（哪一行、整行内容）里挂着一组 `CellError`（哪一列、原始值、错误码、消息），`csvp_bench` 的 txt 报告就是按这个结构打印的。

### 并发模型

- `importSync`：在调用方线程上同步执行，跑完才返回，当前已实现；
- `importStreaming` / `shutdown`：Part B 的待实现部分。接口契约已定死在 `pipeline.h`（后台线程干活、`progress` 回调每 1000 行报一次、`stop_requested_` 停止标志、结果走 `std::future`），配套的三个原子计数器（`hot_total_lines_` 等）也已预留，供实现后 `stats()` 反映实时进度；
- `stats()`：返回统计快照。同步路径读普通成员，流式路径读原子计数器。

### 构建产物

| 目标 | 内容 |
|---|---|
| `csvp` | 静态库，`src/` 三个 .cpp 编译打包，供其余目标链接 |
| `csvp_tests` | 可选的本地 gtest 单元测试；需要自行准备测试源码、Google Test 和样本 |
| `csvp_bench` | 命令行压测工具，校验规则固定为 5 列（`id/name/age/score/active`），结果写入 txt 报告；用法见下文 |

CMake 选项：`BUILD_TESTS` 默认关闭，`BUILD_BENCH` 默认开启，`ENABLE_ASAN` 默认关闭。已有构建缓存中的选项不会被默认值覆盖。MSVC 下统一 `/W4 /utf-8 /permissive-`，优化级别跟随 Debug/Release 配置。

## 构建要求

- C++17 或更高
- CMake ≥ 3.16
- MSVC 2019/2022 或 GCC 8+/Clang 7+
- Google Test（仅开启本地测试时需要，放在 `third_party/googletest`；不随仓库发布）

## 本机 VS Code 构建

在父工作区的 CMake 多项目列表中选择本项目后，使用本目录的
[CMakePresets.json](CMakePresets.json)：

1. 执行 `CMake: Select Configure Preset`，选择 `CSV: Visual Studio 2022 x64`。
2. 执行 `CMake: Select Build Preset`，选择 `CSV: Release`（压测）或 `CSV: Debug`（调试）。
3. 执行普通的 `CMake: Configure`，成功后执行 `CMake: Build`。

该预设使用本机 `D:/Software/VSBuildTools2022` 中的 MSVC v143 x64，
不依赖父工程的 Qt/vcpkg 配置。换电脑时需调整预设中的
`CMAKE_GENERATOR_INSTANCE`，或删除该项让 CMake 自动检测 Visual Studio。
Release 程序生成在 `build/Release/`，Debug 程序生成在 `build/Debug/`。

## 快速开始

按上一节选择配置与构建预设，执行 `CMake: Configure` 和 `CMake: Build`。
准备符合五列规则的 CSV 文件后，从项目根目录运行：

```powershell
# 将输入和报告路径替换为自己的路径，报告不要与输入指向同一个文件。
.\build\Release\csvp_bench.exe 'D:\csv-input\input.csv' 3 'D:\csv-output\report.txt'
```

参数依次为 CSV 路径、重复次数和报告路径；报告的父目录需要已存在。
Debug 构建使用 `build/Debug/` 中的程序。报告包括成功行数、跳过行数及错误明细。

本地测试需要完整的 `tests/`、`data/` 和 `third_party/googletest/`。
准备好这些资料后，在 CMake 缓存设置中开启 `BUILD_TESTS`，重新配置和构建，
再通过 `CMake: Run Tests` 运行。ASan 验证也需要测试资料，并应选择与编译选项兼容的配置。

## API 速览

```cpp
#include <csvp/pipeline.h>

// 定义校验规则
std::vector<csvp::FieldRule> rules = {
    {"id",    csvp::FieldType::Int,    true},
    {"name",  csvp::FieldType::String, true},
    {"age",   csvp::FieldType::Int,    true},
    {"score", csvp::FieldType::Double, false},
};

// 配置管道
csvp::ImportPipeline::Config cfg;
cfg.parser.has_header = true;
cfg.skip_bad_rows = false;

// 执行导入
csvp::ImportPipeline pipeline(cfg, rules);
auto result = pipeline.importSync("data.csv");

std::cout << "Imported: " << result.rows_imported
          << ", Skipped: " << result.rows_skipped
          << ", Time: " << result.elapsed_ms << " ms\n";
```

## 已知限制

以下限制来自 `fast-cpp-csv-parser` 原项目的设计边界：

- 单行最大长度 16 MB
- 引号转义仅支持 `""` 形式
- 不支持字段内嵌换行
- GBK 编码路径未经充分测试

## 缺陷定位指引

> 本仓库是有意保留了若干缺陷的**遗留代码**。已确认问题及验证状态见 [Part A 审查报告](docs/PART_A_AUDIT.md)；本地测试和样本不随当前仓库发布。
>
> 所有标 `DISABLED_` 前缀的测试是**隐藏验收用例**——修好后去掉前缀即可验证。

## 目录结构

下面展示完整的本地练习目录；标记“仅本地”的资料不随 Git 仓库发布。

```
csv-import-pipeline/
├── CMakeLists.txt
├── build_msvc.bat             # 仅本地：旧机器专用构建脚本
├── README.md
├── docs/
│   ├── PART_A_AUDIT.md         # 缺陷审查与验证状态
│   └── REPORT_TEMPLATE.md      # 交付报告模板
├── include/csvp/
│   ├── parser.h          # CSV 解析器
│   ├── row.h             # 行数据容器
│   ├── validator.h       # 字段校验器
│   ├── pipeline.h        # 导入管道
│   └── types.h           # 基础类型
├── src/
│   ├── parser.cpp
│   ├── validator.cpp
│   └── pipeline.cpp
├── tests/                    # 仅本地
│   ├── test_parser.cpp
│   ├── test_validator.cpp
│   ├── test_pipeline.cpp
│   └── test_concurrent.cpp
├── bench/
│   └── bench_import.cpp
└── data/                     # 仅本地
    ├── ReadMe.md              # ⭐ 样本一览与运行方法（含 VS 调试配置）
    ├── sample_utf8.csv       # 正常数据
    ├── sample_noeol.csv      # 无尾换行
    ├── sample_gbk.csv        # GBK 编码
    ├── sample_dirty.csv      # 脏数据
    ├── sample_quote.csv      # 引号转义
    ├── sample_crlf.csv       # CRLF/LF 混用
    ├── sample_100k.csv       # 10 万行压测
    ├── sample_1gb.csv        # Part E：约 1 GB / 1000 万行（性能题）
    └── demo_err.csv          # 带校验错误样本（演示报告错误明细）
```

## 许可证

本项目基于 BSD-3-Clause 许可发布，继承自 fast-cpp-csv-parser。
