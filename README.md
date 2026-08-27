# zeda

这是一个使用 C++20 实现的、类似 Pi 的 coding agent。

## 构建

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 安装

构建后将 `zeda` 安装到已经位于 `PATH` 中的用户级目录：

```bash
cmake --install build --prefix "$HOME/.local"
zeda --version
```

如果 `~/.local/bin` 尚未位于 `PATH`，请将下面一行加入 Shell 配置（例如 `~/.zshrc`），然后重新打开终端：

```bash
export PATH="$HOME/.local/bin:$PATH"
```

也可以安装到系统级目录，让所有用户都能直接启动：

```bash
sudo cmake --install build --prefix /usr/local
```

## 运行

当前 CLI 使用 OpenCode Go 的 Responses API：

```bash
export OPENCODE_GO_API_KEY="你的 OpenCode Go API Key"
zeda
```

构建时通过 CMake FetchContent 获取并固定以下开源依赖：

- [nlohmann/json 3.12.0](https://github.com/nlohmann/json)：JSON 解析和序列化
- [FTXUI 7.0.3](https://github.com/ArthurSonzogni/FTXUI)：交互式终端 UI 和 DOM 渲染

默认模型是 `muse-spark-1.2-contributor`，也可以通过环境变量切换：

```bash
export ZED_MODEL="deepseek-v4-flash"
```

请确认 OpenCode Go 对所选模型的地区、配额和数据政策。`Contributor` 模型可能允许使用 prompts 和 completions 改进后续模型；不要在未接受该政策前发送凭证或敏感代码。

可选配置：

```text
ZED_CONTEXT_MODEL              上下文控制模型，默认与 ZED_MODEL 相同
ZED_REASONING_EFFORT           思考强度：none、low、medium、high，默认 low
ZED_QUICK_BASH                 Quick Bash 启动状态：on/off、true/false、1/0，默认 on
ZED_THEME                      终端主题：light 或 monaka，默认 light
ZED_WORKSPACE                  workspace 路径
ZED_SESSION_PATH               显式恢复或指定 Session 文件；默认每次启动新建
ZED_MAX_CONTEXT_TOKENS        上下文上限，默认 1000000
ZED_RESERVED_OUTPUT_TOKENS     为模型输出预留的 Token
ZED_CONTEXT_TRIGGER_TOKENS     触发上下文模型的 Token 阈值
ZED_MAX_TURNS                  单次请求最大 Agent 回合数
ZED_OPENCODE_ENDPOINT          OpenCode Responses API 地址
ZED_REQUEST_TIMEOUT_MS         Provider 请求超时时间，默认 120000
```

运行中可用 `/reasoning` 查看当前思考强度，或使用
`/reasoning none|low|medium|high` 调整后续模型请求。例如：

```text
/reasoning high
```

终端只提供两个内置主题，不支持自定义主题。`light` 使用 OpenCode
默认亮色风格，`monaka` 使用 Monokai 风格的暗色配色。运行中可以切换：

```text
/theme              查看当前主题
/theme light        切换亮色主题
/theme monaka       切换暗色主题
```

两个主题的输入区都使用很淡的紫罗兰色提示输入焦点。
在输入区输入 `/` 或命令前缀时，界面会实时显示匹配的命令和说明。
输入完整命令名后，提示区会显示该命令的帮助和支持的二级选项；主题、
Quick Bash、思考强度和 Skill 名称都支持二级补全。使用 `↑` / `↓` 选择
候选项，按 `Tab` 自动补全命令或二级选项。

Quick Bash 是内置的可选输入扩展，不是插件。默认开启，可在运行中切换：

```text
/quick-bash          查看状态
/quick-bash on       开启
/quick-bash off      关闭
```

开启后，裸输入的 `pwd`、`ls`、`ps`、`pgrep`、`kill`、`pkill`、`which` 和
`whoami` 会绕过模型并直接通过内置 Bash 工具执行。其他输入仍发送给模型。
Quick Bash 只接受单条简单命令和参数，不支持 `cd`、管道、重定向、命令连接、
变量展开或命令替换。命令和输出只显示在当前终端，不写入 Session，也不计入
token 指标。

每次模型工具调用都必须提供非空的 `purpose`，用于在终端中解释调用目的。
Bash 调用标题只显示这个目的，不显示具体 Shell 指令。
工具输出和用户输入命令的执行结果默认折叠，但用户自己输入的斜杠命令或
Quick Bash 命令始终可见。使用鼠标点击记录可展开详细结果，再次点击可收起。

每次启动默认在当前工作区的 `.zed/sessions/` 下创建新的 JSONL Session，旧 Session 不会被覆盖。启动界面显示当前 Session 名称，`/session` 可查看完整路径。需要恢复某个 Session 时显式指定：

```bash
ZED_SESSION_PATH=".zed/sessions/session-<id>.jsonl" zeda
```

旧版本的 `.zed/session.jsonl` 会原样保留，也可以通过同一环境变量恢复。CLI 内置 `read`、`write` 和 `bash` 工具，文件工具限制在当前工作区内。

## 模块

```text
include/zed/core/       Agent Loop、消息、模型/工具接口、上下文管理
include/zed/providers/  OpenCode Go Provider
include/zed/session/    JSONL Session
include/zed/tools/      文件和 Shell 工具
include/zed/ui/         FTXUI 终端 UI
include/zed/skills/     Skill 发现和加载
include/zed/extensions/ Extension 命令注册
```

`ContextController` 是可选的。没有上下文模型时，`BasicContextManager` 使用确定性规则裁剪；接入模型后，在上下文达到阈值时请求结构化摘要和保留列表。

终端采用类似 Codex CLI 的布局：欢迎页显示版本、模型、思考强度、Quick Bash 状态、工作目录、Session、工具和快捷键，并与消息记录位于同一个可滚动区域，因此向下浏览后不会固定占用屏幕。鼠标滚轮每次固定滚动 3 行；默认自动跟随最新消息，向上滚动后暂停跟随，滚回底部、执行命令或提交新请求时恢复。输入框在没有命令补全列表时支持使用上下方向键切换本进程中已发送的模型 prompt，回到最新位置时恢复未发送草稿。底部固定输入框、运行状态和 token 指标。底栏左侧根据 Agent 事件显示状态；思考时会显示当前强度（例如 `low thinking`），活动状态带旋转动画，空闲时显示静态 `idle`。用户、助手、工具和错误消息分别渲染，只有助手内容会进入 Markdown 解析，避免输入或工具输出意外破坏对话布局。

底栏右侧显示 token 指标：`ctx` 是最近一次模型请求的 input tokens，`↑` 和 `↓` 分别是本次进程内所有模型调用的累计 input 与 output，`→` 后的值是两者之和。数值达到千级或百万级时使用 `k` 或 `m` 紧凑显示。一次 Agent 请求包含多轮工具调用时，每轮模型 usage 都会计入。

Provider 使用 Responses API 的 SSE 模式增量读取模型输出。全屏终端会将密集的 `assistant_delta` 按约 30 FPS 合并刷新，避免事件队列直到响应结束才统一重绘；输入或输出被重定向时，普通 CLI 会立即写出并刷新每个文本增量。Session 仍只在一次模型响应成功并通过工具调用校验后追加完整 assistant 消息。

主 Agent 请求不发送 `max_output_tokens`，因此 zeda 不设置单次响应输出上限，由模型供应商和模型自身的上下文窗口决定实际最大值。内部上下文摘要请求仍使用独立的 1024-token 上限。`ZED_RESERVED_OUTPUT_TOKENS` 只控制构建上下文时为输出预留的空间，不会截断模型输出。

Agent Loop 只在 Provider 明确发出 `response.completed`、不存在待执行工具调用且回复不是延后执行提示时正常结束。`response.incomplete`、`response.failed`、缺少终止事件和不一致的工具结束状态都会显示为错误；达到输出 token 上限不会再被误报为成功。默认 coding-agent 指令要求模型直接使用工具完成并验证工作。模型若只回复“正在构建”“马上开始”等短促进度提示，Agent Loop 会纠偏重试一次；再次不执行则明确失败，并且这些无效进度回复不会写入 Session。

Markdown 当前支持标题、段落、粗体、行内代码、链接、列表、引用、代码块和横线。表格使用 FTXUI 的表格布局，支持表头、单线边框、列分隔线、换行以及 `:---`、`:---:`、`---:` 对齐语法；表格单元格中的 `\\|` 会按字面竖线处理。
