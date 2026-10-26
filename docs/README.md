# 报告与验证结果索引

| 版本 / 阶段 | 内容 | 报告 | 结果与状态 |
|---|---|---|---|
| v1.1 | Part A 缺陷修复 | [REPORT_v1.1.md](REPORT_v1.1.md) | 已发布；[测试](evidence/v1.1/tests)、[构建](evidence/v1.1/compilation)、[压测与内存](evidence/v1.1/bench)、[环境与汇总](evidence/v1.1/metadata) |
| v1.2.0 | Part B 后台导入与限时停止 | [REPORT_v1.2.0.md](REPORT_v1.2.0.md) | 验收完成；[版本标签](https://github.com/xuebossvh/csv-import-pipeline/tree/v1.2.0)；Debug / Release / x86 ASan 各 96/96 通过；[验证汇总](evidence/v1.2.0/metadata/validation.json)、[测试](evidence/v1.2.0/tests)、[构建](evidence/v1.2.0/compilation)、[压测与内存](evidence/v1.2.0/bench) |
| 修复前基线 | 原始问题定位 | [历史审查](archive/PART_A_AUDIT.md) | [原始复现结果](evidence/baseline)；描述旧实现，不是当前待修清单 |

新报告继续使用 [REPORT_TEMPLATE.md](REPORT_TEMPLATE.md)。Part C 未开展，不记录完成结果。

## 文件保留规则

- 每个版本只保留一份主报告；报告中的证据使用相对链接，便于本地和仓库页面跳转。
- `tests/`：保留可直接阅读的测试日志；如采集到完整 XML、CLI 结果或用例矩阵则按其用途保留。v1.2.0 为逐用例 CTest，复用 XML 路径会覆盖输出，因此使用原始日志加 [逐名称核对的结果 JSON](evidence/v1.2.0/metadata/validation.json)，不提供全套 XML，也不把 UI 文件夹空项计入 96 项。
- `compilation/`：编译结果、编译选项、ASan 运行库及构建问题排查记录。失败记录保留用于说明环境问题，不列为验收通过证据。
- `bench/`：JSON 保存各轮数据、输入/程序校验值、内存采样及归并的原始输出；v1.2.0 的 100k 结果以四份版本/配置 JSON 为入口，1 GB 另保留 [错误详情报告](evidence/v1.2.0/bench/v1.2.0-release-1gb-report.txt)。不再单独保存重复的 100k 输出或空 stderr。
- `metadata/`：环境、测试样本校验值、Git 历史及结果汇总。
- 曾发布的 v1.1 标签和历史提交不改写；旧版本结果保持在各自版本目录，不作为新业务实现的替代验收证据。
- v1.2.0 的业务提交为 `4713fab`；构建与测试已经实际执行，证据目录不使用占位通过日志。对应发布标签为 [v1.2.0](https://github.com/xuebossvh/csv-import-pipeline/tree/v1.2.0)。
