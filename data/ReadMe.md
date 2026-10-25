# data 目录说明

本目录存放单元测试和压测使用的 CSV 样本。所有文件都只用于验证，不参与库的编译。

## 文件一览

| 文件 | 大小 | 内容 | 使用它的用例 |
|---|---|---|---|
| `sample_utf8.csv` | 236 B | 正常数据：表头 + 10 行，5 列（id/name/age/score/active） | `Parser.UTF8Basic`、`Parser.HeaderMapping`、`Parser.ReentryState`、`Pipeline.SyncSmallClean` |
| `sample_noeol.csv` | 51 B | 表头 + 3 行，最后一行结尾没有换行符 | `Parser.NoTrailingNewline`、`Parser.ReentryState`（第二次解析） |
| `sample_gbk.csv` | 71 B | GBK 编码的中文数据，表头 + 2 行；其中一行含有不完整的双字节字符 | `Parser.ChineseGBKEncoding` |
| `sample_quote.csv` | 53 B | 表头 + 2 行，第 1 行含 `""` 形式的转义引号 | `Parser.QuotedEscapedQuote` |
| `sample_crlf.csv` | 54 B | 表头 + 3 行，混用 CRLF、LF 和单独的 `\r` | `Parser.CRLFLFMixedCR` |
| `sample_dirty.csv` | 192 B | 表头 + 7 行，包含多种脏数据：多一列、`N/A`、必填字段为空、`abc`、非法布尔值 `maybe`（`yes` 是合法值） | `Pipeline.SkipBadRows`、`Pipeline.ErrorLimit` |
| `demo_err.csv` | 126 B | 表头 + 5 行，其中 3 行有校验错误（共 5 个字段错误），适合演示报告输出 | 只用于 `csvp_bench` 演示，不参与单元测试 |
| `sample_100k.csv` | 3.1 MB | 表头 + 100000 行数据，可能有错误数据 | `Concurrent.ImportAndStats`、`Concurrent.ShutdownNoDeadlock`，以及压测 |
| `sample_1gb.csv` | 约 958 MB | 约 1000 万行（Part E 性能测试用） | 只用于压测，单元测试不会读取它 |

补充说明：

- `Parser.EmptyFile` 和 `Pipeline.MissingFile` 使用的是不存在的文件名，不读取本目录。
- 带 `DISABLED_` 前缀的用例默认不运行，去掉前缀后才会执行，详见下文。
- `sample_gbk.csv` 和 `sample_crlf.csv` 属于「边界数据」：用普通编辑器打开时可能出现乱码或行尾异常，这是数据本身的特征，不是文件损坏。

## 测试是如何找到这些文件的

编译时，`CMakeLists.txt` 会把本目录的绝对路径写成宏：

```cmake
target_compile_definitions(csvp_tests PRIVATE TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/data/")
```

测试代码里通过 `dataPath("sample_utf8.csv")` 把宏和文件名拼起来。因此：

- 不需要把 CSV 复制到可执行文件旁边；
- 在哪个目录运行测试都可以；
- 移动项目后需要重新执行 CMake 配置，否则 `TEST_DATA_DIR` 仍指向旧路径。

压测程序 `csvp_bench` 没有使用这个宏，它直接接收命令行传入的路径，所以路径要写对，或者从项目根目录运行。

## 运行测试

### 1. 构建

在项目根目录，使用 MSVC 的「x64 Native Tools Command Prompt」或任意已配置好 VS 环境的终端：

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release -j
```

### 2. 运行全部测试

```bat
ctest --test-dir build -C Release --output-on-failure
```

`-C Release` 不能省略：工程使用 Visual Studio 多配置生成器，必须指定配置。  
`--output-on-failure` 只在失败时打印详细信息，避免输出刷屏。

### 3. 只运行某一组或某一个用例

```bat
:: 只跑 Parser 开头的用例
ctest --test-dir build -C Release -R Parser --output-on-failure

:: 只跑其中一个用例
build\Release\csvp_tests.exe --gtest_filter=Parser.UTF8Basic

:: 用通配符跑一组
build\Release\csvp_tests.exe --gtest_filter=Pipeline.*
```

### 4. 查看和运行隐藏用例

```bat
:: 列出全部用例（含 DISABLED_ 前缀的）
build\Release\csvp_tests.exe --gtest_list_tests

:: 让 DISABLED_ 用例也执行
build\Release\csvp_tests.exe --gtest_also_run_disabled_tests
```

### 5. 重复与随机顺序压测

用于发现偶发问题，例如状态未重置、内存越界：

```bat
build\Release\csvp_tests.exe --gtest_repeat=100 --gtest_shuffle
build\Release\csvp_tests.exe --gtest_repeat=100 --gtest_shuffle --gtest_break_on_failure
```

### 6. 单独运行某个样本

测试程序本身不支持「指定 CSV 文件」参数。需要观察某个样本时，用压测程序或自己写一个小的驱动程序：

```bat
build\Release\csvp_bench.exe data\sample_utf8.csv 3 data\sample_utf8_report.txt
```

## 压测工具用法

```bat
csvp_bench <csv文件> [重复次数] [报告路径]
```

- 重复次数默认 3；
- 报告路径默认是当前目录下的 `bench_report.txt`；
- 报告包含每轮的导入行数、跳过行数、耗时、吞吐量，以及错误行的列号、列名、原始值和错误码（错误明细最多抽样 200 条）。

示例：

```bat
:: 10 万行，跑 3 轮
build\Release\csvp_bench.exe data\sample_100k.csv 3 data\sample_100k_report.txt

:: 演示错误明细：3 行数据有错（共 5 个字段错误），报告里能逐列看到错误码
build\Release\csvp_bench.exe data\demo_err.csv 1 data\demo_err_report.txt

:: Part E 性能题：1 GB 文件，只跑 1 次
build\Release\csvp_bench.exe data\sample_1gb.csv 1 data\sample_1gb_report.txt
```

`sample_1gb.csv` 是 Part E 的性能题样本，接近 1 GB。只跑 1 次，不要加入单元测试或循环；优化前跑完可能需要数小时，建议先用 `sample_100k.csv` 建立基线，再对比优化前后的吞吐量。

## 每个样本怎么测、看什么

下面全部用 `csvp_bench` 和十六进制查看，不依赖 gtest，所以即使手里是去掉 `tests/` 的版本也能照做。命令默认从项目根目录执行，报告统一写到 `build\` 下（路径可以自己改）。

先认识三个观察点：

- **控制台**：`Run 1: <成功行数> rows in ... (<跳过行数> skipped) [<错误行数> error rows]`，三个数字分别是成功导入行数、跳过行数、错误行条数。
- **报告**：`--- Error Rows (sample) ---` 下面每个 `line N:` 是一整行，后面的 `[col i 'name'] value='...' code=... msg=...` 说明具体是哪个字段错了。
- **原始字节**：`od -An -tx1c data/sample_crlf.csv`（git-bash）或 `Format-Hex data\sample_crlf.csv`（PowerShell）。`2c` 是逗号，`0d` 是 `\r`，`0a` 是 `\n`。判断数据有没有被解析改坏时，要拿报告和原始字节对照。

另外注意：报告里的整行（`raw_line`）是解析后用逗号重新拼出来的，不是文件里的原始字节。像回车、引号这类信息，以十六进制看到的内容为准。

### sample_utf8.csv —— 先确认基线正常

```bat
build\Release\csvp_bench.exe data\sample_utf8.csv 1 build\utf8_report.txt
```

控制台应显示 10 行，没有 skipped/error；报告里错误数为 0。如果这一步就不对，先检查构建和运行目录，不要急着看后面的样本。

### sample_noeol.csv —— 最后一行没有换行

```bat
build\Release\csvp_bench.exe data\sample_noeol.csv 1 build\noeol_report.txt
```

控制台只报 2 行。用十六进制看文件末尾：表头 + 3 行数据都在，最后一行 `3,gamma,300` 后面没有 `0a`。报告里也只有前两行数据——最后一行没有被读入。

### sample_dirty.csv —— “跳过坏行”没有生效

```bat
build\Release\csvp_bench.exe data\sample_dirty.csv 1 build\dirty_report.txt
```

`csvp_bench` 内部已经设置 `skip_bad_rows = true`，但程序仍会打印 `Benchmark failed: Column count mismatch at line 4: expected 5 columns, got 6` 并退出，报告可能来不及生成。也就是说，一行坏数据仍然打断了整个导入。修复后这条命令应当正常结束，坏行体现在 `skipped` 和 `Error Rows` 里。

### sample_quote.csv —— 转义引号让解析提前停止

```bat
build\Release\csvp_bench.exe data\sample_quote.csv 1 build\quote_report.txt
```

控制台显示 `0 rows`、吞吐量为 0，看起来像空文件；但文件里有表头 + 2 行数据。原因是第 1 行数据中的 `""` 让解析在这里失败，后面的数据也没有被读入。报告里没有 Error Rows，因为这一行根本没有被当成有效数据行。用十六进制确认 `1,"he said ""hi""",x` 的原始字节。

### sample_gbk.csv —— 中文字段被拆成多列

```bat
build\Release\csvp_bench.exe data\sample_gbk.csv 1 build\gbk_report.txt
```

程序会打印 `Benchmark failed: Column count mismatch at line 3: expected 4 columns, got 5`，第 2 条数据被多拆出一列。文件是 GBK 编码，用普通编辑器打开可能是乱码，这是正常的；用十六进制看那条中文字段，确认它在字节层面被逗号切开。

### sample_crlf.csv —— 字段末尾的 CR 丢了

```bat
build\Release\csvp_bench.exe data\sample_crlf.csv 1 build\crlf_report.txt
```

这个文件只有 3 列，而 bench 的校验规则固定是 5 列，所以 3 行数据都会因为第 3 列被当成 `age` 校验而跳过——这是工具配置导致的，不是解析本身的问题。真正要看的是报告 `Error Rows` 里的整行：`line 3` 显示 `2,bob,has_cr`。再用十六进制看原文件，这一段是 `has_cr 0d 0a`；样本约定 `0d` 是字段内容、`0a` 才是行尾，解析结果却把 `0d` 去掉了（#10）。

### demo_err.csv —— 看错误报告长什么样

```bat
build\Release\csvp_bench.exe data\demo_err.csv 1 build\demo_err_report.txt
```

控制台显示 `2 rows ... (3 skipped) [3 error rows]`；报告里有 3 个 `line` 块、5 条字段错误，覆盖 `RequiredFieldMissing` 和 `TypeConversionError`。写测试报告时可以拿它当格式参考。

### sample_100k.csv —— 吞吐量与错误累积

```bat
build\Release\csvp_bench.exe data\sample_100k.csv 1 build\100k_report.txt
```

控制台会给出导入行数、跳过行数和吞吐量（Release 下通常是几千行/秒，具体看机器）。文件里混有坏行；默认 `error_limit = 0`（不限制），坏行会全部留在 `Result.errors` 里，坏行越多内存占用越高。想观察内存变化，运行时打开任务管理器看 `csvp_bench.exe` 的内存曲线；想看稳定性，把重复次数改成 3 再跑。

报告本身最多抽样 200 条错误明细，这不影响 `Result.errors` 的全量收集。

### sample_1gb.csv —— Part E 性能题

```bat
build\Release\csvp_bench.exe data\sample_1gb.csv 1 build\1gb_report.txt
```

这是性能题，不是功能测试：只跑 1 次，不要加进单元测试或循环。关注总耗时和 `rows/sec`。当前实现下会跑得很慢（可能以小时计），这正是 Part E 要优化的对象；建议先在 `sample_100k.csv` 上记录基线，优化后用同一个命令对比 1 GB 文件的吞吐量。运行前确认磁盘空间（约 958 MB）和可用内存。

如果只是想确认大文件下程序不崩，用 `sample_100k.csv` 就够了；1 GB 样本主要用来看优化前后的吞吐差异。

## 添加自己的样本

1. 在 `data/` 下新建一个 CSV，保持体积尽量小（几十字节到几 KB 即可）；
2. 在 `tests/` 中新增用例，用 `TEST_DATA_DIR` 拼接路径：

```cpp
std::string dataPath(const char* filename) {
    return std::string(TEST_DATA_DIR) + filename;
}

TEST(MyCase, MySample) {
    csvp::CsvParser parser(csvp::CsvParser::Config{});
    ASSERT_TRUE(parser.parse(dataPath("my_sample.csv").c_str()));
    // ... 断言
}
```

3. 重新编译后单独运行：

```bat
build\Release\csvp_tests.exe --gtest_filter=MyCase.MySample
```

如果新增了文件，记得同步更新本文件的表格，方便其他人查阅。

### 在 Visual Studio 里直接跑样本（不用每次敲命令）

上面的命令可以写进 VS 工程的调试参数，之后每次按 Ctrl+F5 就行，换样本只改一处文件名。前提是已经用 CMake 生成了 `build\csv_import_pipeline.sln`（`build_msvc.bat` 或前面“构建”一节的两条命令）。

1. 先确保 `build\Release\csvp_bench.exe` 已经编译出来；
2. 用 VS 打开 `build\csv_import_pipeline.sln`；
3. 在“解决方案资源管理器”里右键 `csvp_bench` → **设为启动项目**；
4. 再右键 `csvp_bench` → **属性**（Alt+Enter）；
5. 左上角“配置”选 **所有配置**，左侧进入 **配置属性 → 调试**，填两项：

   | 项 | 值 |
   |---|---|
   | 命令参数 | `data\sample_utf8.csv 1 build\utf8_report.txt` |
   | 工作目录 | `$(ProjectDir)..` |

6. 确定后，把工具栏的解决方案配置选成 **Release**，按 **Ctrl+F5**（开始执行不调试）。

控制台会打印导入行数和吞吐量，报告写到 `build\utf8_report.txt`。以后要看别的样本，只改“命令参数”里的文件名即可，不用再开终端。

几个容易踩的坑：

- **工作目录不能留空**。CMake 生成的工程在 `build\` 下，默认工作目录也是 `build\`，`data\sample_utf8.csv` 会被当成 `build\data\sample_utf8.csv` 而找不到，控制台显示 0 行。`$(ProjectDir)..` 就是让程序回到项目根目录再跑。（项目根下的 `data\` 才是样本目录。）
- 这些调试参数保存在 `csvp_bench.vcxproj.user` 里，重新执行 CMake 生成工程不会丢。
- `F5` 是带调试启动，程序结束控制台会立刻关闭；只想看输出用 `Ctrl+F5`，结束时会停在“按任意键继续”。
- 测吞吐量时必须切到 **Release**。Debug 配置没有开优化，速度慢很多，数字没有参考价值。
- 报告路径同样是相对工作目录的，所以写 `build\xxx.txt` 正好落在 `build` 下。
- 自己的样本如果列结构不是 5 列（`id/name/age/score/active`），`csvp_bench` 会按固定规则校验，整行被跳过。要观察解析效果，样本最好按这 5 列来造；或者改 `bench/bench_import.cpp` 里的 `makeDefaultRules()`。

如果样本很多、想完全不碰属性页，有两种进阶做法：在“配置管理器”里为每个样本复制一份 Release 配置（如 `BenchUtf8`、`BenchDirty`），每个配置填不同参数，工具栏下拉切换；或者在 `CMakeLists.txt` 里给 `csvp_bench` 设置默认调试参数，让所有人打开工程就能直接跑：

```cmake
set_target_properties(csvp_bench PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VS_DEBUGGER_COMMAND_ARGUMENTS "data/sample_utf8.csv 1 build/bench_report.txt")
```
