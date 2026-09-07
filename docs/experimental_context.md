# 实验性上下文管理

这个功能默认开启。没有配置文件或旧配置不含 `context.experimental_mode` 字段时，
主 Agent 使用持久笔记、可检索的原始历史和独立上下文窗口，不调用摘要控制模型。
显式设置为 `false` 时继续使用原来的 `BasicContextManager`、模型摘要控制器和
确定性裁剪，不会因为默认值改变而覆盖已有的关闭配置。

## Codex 当前机制与证据边界

核对日期：2026-09-07；本机安装的 Codex CLI 为 0.153.4。

官方当前公开了 `features.context_management.experimental_mode` 开关，默认关闭。
模型跨上下文窗口保留笔记，并能检索同一任务中的早期消息和工具结果，减少反复依赖
单一摘要的信息损失。官方产品说明将其作为有客户端和登录条件限制的实验功能。
来源：[Codex models](https://learn.chatgpt.com/docs/models#experimental-context-management)、
[配置参考](https://learn.chatgpt.com/docs/config-file/config-reference)。

与此并存的是常规压缩预算配置：`model_context_window`、
`model_auto_compact_token_limit`，以及计算整个活跃上下文或保留前缀之后增长部分的
`model_auto_compact_token_limit_scope`。
来源：[配置参考](https://learn.chatgpt.com/docs/config-file/config-reference)。

这里实现的是公开机制启发的 C++ 版本。官方文档和本机安装包能确认开关、笔记、
历史和窗口管理的方向，不能证明 Codex 内部所有算法和默认阈值。本项目没有声称复制
Codex 的私有实现，也不调用其受登录限制的服务。它复用当前模型的普通工具调用协议。

## 开启与关闭

无需配置即可默认开启。此前显式关闭过的 workspace，可在 zeda 中执行：

```text
/configure context set experimental-mode on
/exit
```

重新启动 `zeda` 生效。需要继续指定会话时，用 `/session open <id>`，或通过已有的
`ZED_SESSION_PATH` 指定原始 Session 文件。默认每次启动仍然新建 Session。

也可以使用 `/configure-web` 的“启用笔记与可搜索历史实验”复选框；或在现有
`.zed/config.json` 的 `context` 对象内加入布尔字段：

```json
"experimental_mode": true
```

不要用这一行替换整个配置文件。字符串 `"true"`、数字等会被拒绝。
用 `/configure` 检查保存的配置。设置命令和网页保存均不会在进行中的任务内切换模式。

恢复旧路径：执行 `/configure context set experimental-mode off`，然后重启。
原始 Session 和实验笔记不会被删除；关闭时不读取或修改实验状态，也不注册实验工具。

如果主 Agent 的工具列表是显式白名单，需同时允许 `context_notes`、
`context_history` 和 `new_context`；默认 `*` 已包括这些工具。缺少工具时，实验请求
明确失败，不会绕过工具权限或悄悄切换管理方式。现有 Sub Agent 独立内存 Session
和工具集维持原行为，实验工具仅接入主 Agent。

## 一次请求如何构建上下文

1. 原始消息仍按顺序追加到现有 Session JSONL，工具调用与工具结果保持完整配对。
2. 根据最新窗口检查点，选择本窗口保留的消息和检查点之后新增加的消息。
3. 保留系统指令、最新用户请求、最新消息和尚未被后续 assistant 消费的工具调用组。
   加入窗口恢复指引及有长度限制的笔记预览；笔记明确作为参考数据，不提升为系统指令。
4. 估算消息、工具 schema 和预留输出的预算。接近阈值时提示模型保存检查点；达到
   阈值时按确定性规则保留最近完整消息组，归档较早内容，为后续工作留下空间。
5. 模型也可以先写笔记，再调用 `new_context`。它只请求切窗；实际切换要等当前
   assistant 的整个工具批次和所有结果写入 Session 后，在下次模型请求之前执行。
6. 新窗口继续处理当前任务，通过笔记和历史工具恢复细节。原始历史不被替换成摘要。

自动窗口阈值继续使用现有 `compaction_trigger_tokens`；为 0 时取可用输入预算的
80%。通常保留到阈值的一半左右，为工具执行和历史读取留出空间；必须保留的内容
可超过这个软目标。关闭 Agent 的自动压缩设置时，不在软阈值自动切换，但仍可以显式
`new_context`，超出硬预算时仍做安全的窗口选择。

如果系统指令、最新请求、未消费工具结果和工具 schema 本身已超出硬预算，返回明确
错误，不截断这些内容，也不写入新的窗口检查点。需要增加预算或降低工具输出上限。
估算沿用项目的字节近似方法，不是模型 tokenizer 的精确计数；provider 的真实输入
用量仍按原有方式显示。

切窗发出 `context_window` 结构化事件，包含前后窗口 ID 和从活跃窗口移除的消息 ID；
终端显示窗口变化和数量。对话状态不因切窗被删除。

## 模型工具

所有调用仍通过 `ToolRegistry` 校验名称、参数和权限，且必须带 `purpose`。

| 工具 | 操作 | 用途 |
| --- | --- | --- |
| `context_notes` | `list/read/write/append/search` | 保存当前目标、约束、决定、验证结果、未完成工作和历史 ID |
| `context_history` | `windows/list/read/search` | 查看窗口，按原始消息 ID 读取，按字面量搜索消息、工具参数和结果 |
| `new_context` | 请求切窗 | 完成当前工具批次后进入新窗口，不结束用户任务 |

工具输出限制为 16 KiB，并保持有效 JSON。列表/搜索使用 `offset/limit`，按返回的
`next_offset` 继续；原文读取使用 `byte_offset/max_bytes`，按 `next_byte_offset` 继续，
直到 `truncated` 为 false。历史原文返回 `message_json` 文本片段，拼接后是完整消息
JSON，包含工具参数和结果；笔记原文返回 `text`。字节偏移必须位于 UTF-8 字符边界。
历史搜索是区分大小写的字面量查找，不依赖
embedding、向量数据库或外部服务。窗口过滤按消息首次进入的窗口划分；一个旧消息
可能仍被后续窗口保留，其原始 ID 不变。笔记命名是逻辑相对路径，不能访问任意文件。

## 持久化与恢复

原始 Session JSONL 格式不变。实验状态保存到对应 Session 文件旁的
`<session>.jsonl.context`，内容也是版本化 JSONL。扩展名有意不使用 `.jsonl`，避免
被现有 Session 列表当作另一段对话。按需创建，文件权限为 `0600`。

记录包括笔记写入/追加、切窗请求和窗口检查点。检查点保存历史长度、最后消息 ID、
保留的消息 ID 和切换原因。写入成功后才提交窗口变化；I/O、解析、预算错误和取消
均通过错误结果返回。窗口状态与当前 transcript 的边界不匹配时明确拒绝恢复。
工具使用 5 秒协作式执行预算；取消和超时在读取、扫描和写入边界检查，单次底层文件
系统调用不能被强制打断。单篇笔记最多 1 MiB、笔记正文合计最多 8 MiB，状态日志
最多 16 MiB；超过限制会明确报错。检测到损坏或不完整的日志记录时，不会用空状态
静默覆盖；原始 Session 历史仍保留。

`/session open` 动态切换到相应状态文件，`/session new` 从空笔记和初始窗口开始。
开启实验模式时，`/session fork` 复制原始消息并继承笔记与窗口检查点，分叉后独立写入。
未完成的切窗请求不随分叉复制。关闭模式下分叉保持原有行为，不读取实验状态。

这些笔记只在当前 Session 及其显式分叉中使用，不属于用户的全局长期记忆。
跨会话检索、多 Agent 共享记忆、向量检索和 provider 私有 reasoning 状态不在此次范围。

## 验证

```bash
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 tests/experimental_context_cli_smoke.py "$PWD/build/zeda"
```

单元与集成测试使用确定性的内存 Session/fake model，覆盖窗口预算、原始历史保留、
工具批次完整性、笔记/历史恢复、取消、错误、配置默认开启及显式关闭后的旧路径兼容。
CLI 脚本仅需 Python 标准库，通过本机回环假 provider 验证实际二进制的开关、切窗、
重启恢复、历史搜索、分叉和再次关闭；不访问真实 LLM API。

安装后可对实际命令重复验证：

```bash
cmake --install build --prefix "$HOME/.local"
zeda --version
python3 tests/experimental_context_cli_smoke.py "$(command -v zeda)"
```

这些检查证明运行流程及持久化行为。真实模型能否及时写出高质量笔记、复杂任务跨多次
窗口切换后的效果，需要另行用真实模型评估；测试不声称验证了这一点。
