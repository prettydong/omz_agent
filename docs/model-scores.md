# 模型编程综合参考分

核对日期：2026-09-07（Asia/Shanghai）。本地 `zeda` 的 `/model` 性能条采用此表。

这是一套面向 coding agent 的自定义参考分，不是任何机构发布的官方总分，也不是
omz_agent、OpenCode Go 服务或当前所选推理档位的实测成功率。价格、吞吐量和额度不参与性能分。

## 四个榜单与权重

| 榜单 | 权重 | 固定采用的数据源 | 测试内容 |
| --- | ---: | --- | --- |
| Terminal-Bench 2.1 | 40% | [Artificial Analysis 榜单快照](https://artificialanalysis.ai/leaderboards/models/) | 终端中的多步任务 |
| SWE-bench Verified | 30% | [Vals AI](https://www.vals.ai/benchmarks/swebench)，更新 2026-09-01 | 真实仓库问题修复，统一 bash-only mini-SWE-agent |
| LiveCodeBench | 20% | [Vals AI](https://www.vals.ai/benchmarks/lcb)，更新 2026-09-01 | 竞争编程题的代码正确率 |
| SciCode | 10% | [Artificial Analysis](https://artificialanalysis.ai/evaluations/scicode)，与 Terminal-Bench 同一快照 | 科学编程子问题 |

权重是本项目的产品选择：70% 偏向 agent 执行和项目修复，30% 补充算法和科学代码能力，
未经过用户任务集拟合。四个子项都转为 0–100 的正确率百分比；AA 的 0–1 原值乘以 100。
这里没有把 Elo、榜单名次或 AA Intelligence Index 当成百分比直接平均。

AA Intelligence/Coding Index 已包含 Terminal-Bench 与 SciCode，因此不再把这两个综合指数
额外加一遍，避免重复计分。[AA 方法说明](https://artificialanalysis.ai/methodology/intelligence-benchmarking)

```text
综合分 = round(Σ(可用榜单权重 × 原始百分比) / Σ(可用榜单权重))
```

只在最后四舍五入一次。缺失值保持 null，不填 0、不借用相似型号成绩。
`4榜` 是四个 benchmark，不表示四家独立评测机构；此版采用两家评测机构的数据。

- 至少 3 个榜单且可用权重不低于 70%，并且没有服务别名映射问题：显示 `综合 N · K榜`。
- 榜单或权重覆盖不足，或者服务别名与被测版本对应关系未验证：显示 `暂定 N · K榜`。
- 全部缺失：显示 `综合 · 资料不足`，性能条不填充。

覆盖率是数据完整度，不是统计置信度。暂定分不能与完整覆盖分当成严格的等条件排序；
相差一两分也不构成显著性结论。恢复缺失测试后，暂定分可能上升或下降。

## 全部模型

下表原始成绩保留两位小数供阅读；计算使用 JSON 中的完整精度。TB 为 AA Terminal-Bench 2.1，
SWE/LCB 为 Vals 的对应榜单，SC 为 AA SciCode。破折号表示该快照无匹配测量。

| 模型 ID | TB | SWE | LCB | SC | 显示分 | 覆盖 | 状态 |
| --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| `gpt-5.6-luna` | 80.90 | 93.00 | — | 53.59 | 82 | 3/4，80% | 综合 |
| `grok-4.6` | 88.39 | 95.60 | 88.22 | 56.48 | 87 | 4/4，100% | 综合 |
| `muse-spark-1.2-contributor` | 80.15 | 86.60 | — | 57.41 | 80 | 3/4，80% | 暂定：服务版本映射 |
| `muse-spark-1.3-contributor` | 85.77 | — | — | 58.33 | 80 | 2/4，50% | 暂定：服务版本映射 |
| `deepseek-v4-flash` | 78.65 | 88.80 | 87.26 | 50.35 | 81 | 4/4，100% | 暂定：服务版本映射 |
| `deepseek-v4-flash-vision-exp` | 74.16 | — | — | 49.65 | 69 | 2/4，50% | 暂定：服务版本映射 |
| `deepseek-v4-pro` | 78.65 | 96.40 | 87.53 | 51.04 | 83 | 4/4，100% | 暂定：服务版本映射 |
| `glm-5.1` | 61.80 | 76.40 | 81.38 | — | 71 | 3/4，90% | 综合 |
| `glm-5.2` | 77.90 | 82.80 | 69.50 | 51.16 | 75 | 4/4，100% | 综合 |
| `glm-5.3` | 83.90 | 95.40 | 80.53 | 59.03 | 84 | 4/4，100% | 综合 |
| `glm-5.3-flash` | 84.27 | 92.00 | 80.51 | 51.62 | 83 | 4/4，100% | 综合 |
| `hy3` | 64.42 | — | — | 48.61 | 61 | 2/4，50% | 暂定：覆盖不足 |
| `hy4-preview` | — | — | — | — | — | 0/4，0% | 资料不足 |
| `kimi-k2.6` | 65.92 | 76.20 | 86.77 | — | 74 | 3/4，90% | 综合 |
| `kimi-k2.7-code` | 67.42 | 78.20 | 82.05 | 47.80 | 72 | 4/4，100% | 综合 |
| `kimi-k3` | 85.02 | 93.40 | 87.19 | 59.49 | 85 | 4/4，100% | 综合 |
| `longcat-2.0` | 50.19 | — | — | 36.34 | 47 | 2/4，50% | 暂定：覆盖不足 |
| `mimo-v2.5` | 63.67 | 71.00 | 81.51 | 43.87 | 67 | 4/4，100% | 综合 |
| `mimo-v2.5-pro` | 65.17 | 74.00 | 81.35 | 50.58 | 70 | 4/4，100% | 综合 |
| `minimax-m2.7` | 55.43 | 73.80 | 79.93 | — | 67 | 3/4，90% | 综合 |
| `minimax-m3` | 65.17 | 75.00 | 82.15 | 47.11 | 70 | 4/4，100% | 综合 |
| `qwen3.6-plus` | 61.42 | 73.40 | 85.95 | — | 71 | 3/4，90% | 综合 |
| `qwen3.7-max` | 74.53 | 68.80 | 87.06 | 49.54 | 73 | 4/4，100% | 综合 |
| `qwen3.7-plus` | 61.05 | — | — | 46.06 | 58 | 2/4，50% | 暂定：覆盖不足 |
| `qwen3.8-flash` | — | — | — | — | — | 0/4，0% | 资料不足 |
| `qwen3.8-max` | 81.27 | 85.60 | 87.85 | 53.24 | 81 | 4/4，100% | 综合 |

## 模型版本与推理条件

- GPT-5.6 Luna 采用 AA `GPT-5.6 Luna (max)` 和 Vals SWE `max`。Vals LCB 当前快照没有 Luna，故其 82 分由三个榜单、80% 权重计算，不能拿别的 GPT 型号补齐。
- Grok 4.6 采用此次 AA 榜单主条目的 `high`，不是此前界面中的 `xhigh` 估算；Vals SWE/LCB 也为 `high`。
- DeepSeek Flash 使用 **0731**、Pro 使用 **0813** 的公开测量，保留完整日期版本。Go 的无日期服务别名未实测核验，因此标暂定。Flash 的 AA 是 max，而 Vals SWE/LCB 是 high，不能解读为同推理预算比较。
- Muse Spark 1.2/1.3 Contributor 分别参考公开 Muse Spark 1.2 xhigh / 1.3 max；Contributor 与公开测量检查点是否完全一致未验证，标暂定。
- DeepSeek Flash Vision Exp 参考公开 Flash Vision 测量；实验服务对应关系未验证，标暂定。
- Hy4 Preview 没有本快照的精确条目，不使用 Hy3 或 Hy3-preview 代替。Qwen3.8 Flash 不使用不同名称的 Qwen3.8-Flash-Next 代替。
- 其余榜单未说明的 reasoning effort 保持 null（机构默认配置），不擅自称为最高推理档位。

每项测量的原始字段、模型名称/slug、effort、更新时间与源文件 SHA-256 都保存在
[原始评分数据](../data/model-benchmarks-2026-09-07.json)。公开测量没有统一 omz 的工具集、
提示词、推理预算或上下文长度；分数只提供公开基准下的能力参考。

## 核验中排除的数据

1. 搜索摘要曾把 GPT-5.6 Luna 写成 52；本次直接下载的 AA 全榜、对应模型对象的
   `intelligenceIndex` 是 43.4414655969671。使用同一 HTML/Next Flight 响应的对象数据，
   不拼接旧网页摘要。新的综合分 82 与旧 AA 43 属于不同指标。
2. [LiveCodeBench 原站](https://livecodebench.github.io/leaderboard.html)所链接的数据快照不覆盖这里的新型号，
   因此统一采用 Vals 已发布的 LiveCodeBench 实现，不混用原站窗口或模型厂商自报值。
3. [SWE-bench 原站](https://www.swebench.com/)的不同 agent 提交不能直接当成模型排名；
   采用 Vals 对应模型的统一 harness 成绩。
4. Vals 的 SWE/LCB 页面标为归档，停止对新发布模型补测。它们仍可作为有历史数据模型的参考，
   但新模型缺项不可静默外推。没有使用另一版本 Terminal-Bench 3.0/4.0 的数字补 2.1。
5. Arena 属于人类偏好信号，不作为这版以程序测试正确率为基础的性能分子项。

## 可复算与更新

运行时使用编译进程序的本地表，打开模型选择器不会联网抓榜。

```bash
python3 scripts/generate_model_score_data.py --clang-format /path/to/clang-format
python3 scripts/generate_model_score_data.py --clang-format /path/to/clang-format --check
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

更新时应新建带日期的 JSON 快照、明确记录模型映射、同步生成器输入和文档，并重新构建。
生成的 [model_score_data.hpp](../src/app/model_score_data.hpp) 不手工修改。
计算逻辑在 [model_scores.hpp](../src/app/model_scores.hpp)，边界与公式测试在
[model_scores_smoke.cpp](../tests/model_scores_smoke.cpp)。
