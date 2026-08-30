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

当前 CLI 使用 OpenCode Go，并按模型元数据自动选择 Responses、Chat Completions
或 Anthropic Messages 协议：

```bash
opencode auth login
zeda
```

zeda 默认从 OpenCode 内置凭证库
`~/.local/share/opencode/auth.json` 读取 `opencode-go` API Key，因此完成一次
OpenCode 登录后，后续启动无需再输入。`OPENCODE_GO_API_KEY` 仍可用于显式覆盖
内置凭证，`ZED_OPENCODE_AUTH_PATH` 可用于指定其他凭证文件位置。

构建时通过 CMake FetchContent 获取并固定以下开源依赖：

- [nlohmann/json 3.12.0](https://github.com/nlohmann/json)：JSON 解析和序列化
- [FTXUI 7.0.3](https://github.com/ArthurSonzogni/FTXUI)：交互式终端 UI 和 DOM 渲染
- [cpp-httplib 0.51.0](https://github.com/yhirose/cpp-httplib)：仅绑定本机回环地址的配置页和 DeepWiki HTTP 服务

默认模型是 `muse-spark-1.2-contributor`，也可以通过环境变量切换：

```bash
export ZED_MODEL="gpt-5.6-luna"
```

zeda 启动时直接使用内置模型目录，不执行外部模型发现，以避免阻塞欢迎页。运行中使用
`/model list` 查看当前目录；只有显式执行 `/model refresh` 时，才会通过本机
`opencode models opencode-go --verbose` 刷新模型、协议、上下文容量和思考档位。
使用 `/model <id>` 切换主模型，也可在启动前通过 `ZED_MODEL` 指定模型。

请确认 OpenCode Go 对所选模型的地区、配额和数据政策。`Contributor` 模型可能允许使用 prompts 和 completions 改进后续模型；不要在未接受该政策前发送凭证或敏感代码。

可选配置：

```text
ZED_CONTEXT_MODEL              上下文控制模型，默认与 ZED_MODEL 相同
ZED_REASONING_EFFORT           思考档位：auto/none/minimal/low/medium/high/xhigh/max/thinking，默认 low
ZED_QUICK_BASH                 Quick Bash 启动状态：on/off、true/false、1/0，默认 on
ZED_THEME                      终端主题：light 或 monaka，默认 light
ZED_WORKSPACE                  workspace 路径
ZED_SESSION_PATH               显式恢复或指定 Session 文件；默认每次启动新建
ZED_MAX_CONTEXT_TOKENS        上下文上限，默认 1000000
ZED_RESERVED_OUTPUT_TOKENS     为模型输出预留的 Token
ZED_CONTEXT_TRIGGER_TOKENS     触发上下文模型的 Token 阈值
ZED_MAX_TURNS                  单次请求最大 Agent 回合数
ZED_OPENCODE_AUTH_PATH         OpenCode auth.json 路径
ZED_OPENCODE_ENDPOINT          OpenCode Go API 基础地址，默认 https://opencode.ai/zen/go/v1
ZED_OPENCODE_PATH              /model refresh 使用的本机 opencode，默认从 PATH 查找
ZED_REQUEST_TIMEOUT_MS         Provider 请求超时时间，默认 120000
ZED_CLANGD_PATH                本机 clangd 可执行文件，默认从 PATH 查找 clangd
ZED_PLUGIN_PATH                追加外部插件发现目录，多个目录使用冒号分隔
```

### Web 配置

运行中输入 `/configure-web` 会启动一个仅监听 `127.0.0.1` 随机端口的页面并在默认
浏览器中打开。页面统一管理 Agent、Sub Agent、Skill 和上下文：

- 可新增多个主 Agent 配置档案，并选择一个 Active Agent；每个档案独立保存模型、
  reasoning、最大回合、输出上限、temperature、自动上下文压缩、Tool 权限和完整系统
  提示词；
- 可新增多个命名 Sub Agent；每个角色独立保存模型、reasoning、执行上限和完整系统
  提示词，并与 Explorer 一起进入 `subagent` 工具的动态 schema；
- 可新增、编辑、启用、停用或移除 workspace Skill；
- 可配置 Sub Agent 并发、总超时和聚合输出上限；
- 上下文控制模型、窗口预算、摘要输出上限和完整系统提示词。

页面采用无标签页的三栏单页结构：左侧同时显示 Agent、Sub Agent 和 Skill 清单，中间
编辑当前资源，右侧常驻上下文与 Sub Agent 执行参数。界面使用无圆角、无阴影的高密度
机械风格。

页面使用每次进程随机生成的访问令牌，拒绝非同源写入，不加载外部脚本，也不会读取、
显示或保存 API Key。配置经过严格校验后以 `0600` 权限原子写入当前 workspace 的：

```text
.zed/config.json
.zed/agent_management.json
.zed/zed_system_propmt.md
.zed/explorer_system_prompt.md
.zed/context_system_prompt.md
.zed/skills/<id>/SKILL.md
```

保存后重启 `zeda` 生效。已有环境变量保持最高优先级，例如 `ZED_MODEL` 会覆盖网页中
保存的主模型；Explorer 当前没有环境变量覆盖项。配置文件使用版本化且不接受额外字段
的 JSON schema，手工修改错误会在下次启动时明确报告，不会静默回退。

`agent_management.json` 保存 Agent 档案、Active Agent 和自定义 Sub Agent。为了兼容
已有配置，Active Agent 的运行参数和提示词也会同步到原有 `config.json` 与主提示词
文件；其他 Agent 档案和自定义 Sub Agent 的提示词以该管理文件为准。Skill 启用时位于
`.zed/skills/`，停用时移动到 `.zed/skills-disabled/`；从管理
页移除时会将完整目录移动到 `.zed/skill-archive/`，不会直接删除其中的附加资源。
Skill 根目录、Skill 目录和 `SKILL.md` 都不得是符号链接；指令必须是 UTF-8，单个
文件最多容纳 1 MiB 指令，启动时违反边界会明确报告并拒绝加载。

每个主 Agent 的 Tool 权限是运行时白名单，既控制发送给模型的 Tool schema，也阻止对
未授权 Tool 的执行；`*` 表示允许全部已注册 Tool，并用于兼容旧配置。自动上下文压缩
关闭后，低于模型硬窗口时不主动摘要；超过硬窗口时仍执行确定性裁剪，避免发送无效请求。
版本 1 的 Agent 管理文件会在内存中迁移为版本 2，并沿用原有全局压缩阈值。

运行中可用 `/reasoning` 查看当前模型支持的思考强度，或使用 `/reasoning <effort>`
调整后续模型请求。完整档位包括 `auto`、`none`、`minimal`、`low`、`medium`、
`high`、`xhigh`、`max` 和 `thinking`，命令会拒绝当前模型不支持的档位。例如：

```text
/reasoning high
```

切换模型时，如果原思考档位不受新模型支持，会自动回到 `auto`，由模型使用默认行为。

## 系统提示词

三个系统提示词文件分别用于主 Agent、Explorer 和上下文控制模型：

```text
.zed/zed_system_propmt.md
.zed/explorer_system_prompt.md
.zed/context_system_prompt.md
```

项目中缺少文件时，`zeda` 会写入对应的内置默认提示词；已有文件不会被覆盖。主 Agent
启用的 Skill 指令仍会附加在主提示词之后。网页或手工修改后需要重启 `zeda`。三个
文件都必须是非空的 UTF-8 普通文件，不能是符号链接，单个文件不得超过 1 MiB；创建、
读取或校验失败都会在启动时明确报错。

终端只提供两个内置主题，不支持自定义主题。`light` 使用 OpenCode
默认亮色风格，`monaka` 使用 Monokai 风格的暗色配色。运行中可以切换：

```text
/theme              查看当前主题
/theme light        切换亮色主题
/theme monaka       切换暗色主题
```

两个主题的输入区都使用很淡的紫罗兰色提示输入焦点。当前输入区和
已提交的用户消息使用相同底色，内容上下都各保留一行空白。
输入光标使用宽块闪烁样式，左侧提示符保持为 `›`。
底部 Token 摘要会显示当前上下文占用比例和最近一次模型输出的平均 Token 速率。
在输入区输入 `/` 或命令前缀时，界面会实时显示匹配的命令和说明。
输入完整命令名后，提示区会显示该命令的帮助和支持的二级选项；主题、
Quick Bash、思考强度和 Skill 名称都支持二级补全。使用 `↑` / `↓` 选择
候选项，按 `Enter` 或 `Tab` 自动补全命令或二级选项；补全后再按
`Enter` 执行。

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

内置 `grep` 不进入 `.git` 目录，且忽略包含 NUL 或非法 UTF-8 的二进制匹配行。
所有工具输出在进入 Agent Loop 前统一校验 UTF-8；坏字节会替换为可见的替换字符并附加警告。
Session 序列化错误会返回可见的 `session_error`，不会以未捕获异常终止进程。

每次模型工具调用都必须提供非空的 `purpose`，用于在终端中解释调用目的。
Bash 调用标题只显示这个目的，不显示具体 Shell 指令。
工具输出默认折叠。用户输入的模型请求、斜杠命令和 Quick Bash 命令及其执行
结果默认完整展开，仍可使用鼠标点击记录手动收起或重新展开。

每次启动默认在当前工作区的 `.zed/sessions/` 下创建新的 Session v2 JSONL
文件，已有文件不会被覆盖。文件头保存 Session ID、标题、工作区、Provider、
模型和时间；每次 Agent 请求使用 `turn_start`、`message`、`turn_end` 记录明确的
回合边界，并在每个边界刷新到磁盘。

如果进程在工具执行期间中断，下次打开时不会盲目重跑工具。zeda 会删除未写完的
最后一条 JSONL 记录，为缺失的工具结果追加“执行结果未知”的恢复记录，再把该回合
标记为 `interrupted`。这样后续模型上下文仍然合法，同时要求 Agent 先检查工作区。
工具结果的错误状态也会持久化，重开后不会把失败工具显示为成功。同一个 Session
同时只允许一个 zeda 进程写入；第二个进程会明确报告 Session 已被占用。

`/session` 按更新时间列出 Session、标题、回合数和中断状态。Session 可以创建、
打开、重命名和派生；仍兼容原来的 `/session <id>` 打开写法：

```text
/session
/session list
/session new [title]
/session open <id-or-title>
/session rename <title>
/session fork [title]
/session <id>
```

标题可以重复，但重复时必须使用唯一的 Session ID 打开。`fork` 创建独立文件并记录
父 Session ID，不会修改原 Session。模型用户输入按原文保存；启用的 Skill 指令只
进入当次 system prompt，不会伪装成用户输入写入历史。

仍可在启动时通过环境变量显式恢复或指定 Session 文件：

```bash
ZED_SESSION_PATH=".zed/sessions/session-<id>.jsonl" zeda
```

Session v2 不兼容早期 beta 的 message-only JSONL。旧文件不会被删除，`/session`
会将其标记为 `not Session v2`，但拒绝打开。CLI 内置 `read`、`write`、`edit`、
`grep`、`find`、`ls`、`bash` 和 `multi_bash` 工具，文件工具限制在当前工作区内。
`multi_bash` 可在一次工具调用中并行运行 2–8 条互不依赖的 Shell 命令，默认最多
同时运行 4 条，并按输入顺序返回各自的成功状态和输出。批次总输出受工具输出上限
约束；它适合不需要查看中间结果的检查、搜索和测试，从而减少模型往返和重复上下文
计费。存在前后依赖或高风险写操作时仍应使用单独工具调用。`find` 使用
`*` 和 `?` glob 搜索文件或目录，不扫描 `.git` 且不跟随符号链接；`ls` 只列出
指定目录的一层内容，并用 `/` 和 `@` 标记目录与符号链接。

外部命令、`clangd`、模型 HTTP 客户端和模型目录发现统一通过 `posix_spawn` 启动。
子进程默认只继承 `PATH`、locale、临时目录和编译工具链等明确允许的环境变量，不会
自动获得任意 Token、数据库连接串或云凭证；确有需要的可信 Worker 必须逐项声明。
Shell 超时或取消会终止整个进程组，即使主 Shell 已退出也不会继续等待后台进程管道。

## Sub Agent

Active Agent 可以通过内置 `subagent` 工具把读密集、彼此独立的调查任务委派给
Explorer 或网页中新增的 Sub Agent。使用 `/agents` 查看角色、模型、reasoning、执行
上限、工具白名单和当前可用状态；不提供用户直接运行 Sub Agent 的斜杠命令。

Explorer 默认使用 `opencode-go/muse-spark-1.2-contributor` 和 `low` reasoning，
不会跟随 `/model` 切换，也不会静默回退到主模型。可通过 `/configure-web` 为它选择
其他内置目录模型或禁用它；模型不存在或不支持所选 reasoning 时，`/agents` 会显示
`unavailable`，工具调用会明确失败。选择 Contributor 模型仍意味着接受前文所述
数据政策。无论模型或提示词如何配置，Explorer 和自定义 Sub Agent 的工具白名单都
不可放宽。Worker 注册的工具白名单固定为只读工具；修改提示词不能获得 Shell、写文件、
插件、Skill 或嵌套 Sub Agent 权限。

每个 Sub Agent 任务在独立的 `zeda --subagent-worker` 子进程和新上下文中运行，
只注册 `read`、`grep`、`find`、`ls` 和只读 `lsp`。Worker 不加载 Shell、写入工具、
Quick Bash、Skill、插件或 `subagent` 本身，也不创建 Session 文件。主 Session 仅
保存一次 `subagent` 调用及其最终结果；运行中的 queued、running、completed 和
failed 状态只显示在当前工具卡片中。

模型可使用三种互斥调用形式：

```json
{"agent":"explorer","task":"调查一个问题"}
{"tasks":[{"agent":"explorer","task":"调查 A"},{"agent":"explorer","task":"调查 B"}]}
{"chain":[{"agent":"explorer","task":"先调查 A"},{"agent":"explorer","task":"根据 {previous} 调查 B"}]}
```

Parallel 和 Chain 都接受 2–8 个任务；Parallel 默认最多同时运行 4 个并按输入顺序返回，
单个失败不会丢弃其他结果。Chain 用上一步结果替换 `{previous}`，首个失败后停止。
一次调用默认总超时为 10 分钟，每个 Worker 默认最多执行 12 个 Agent Loop 回合；每个任务的
输入和最终输出上限都是 32 KiB，聚合工具结果上限是 256 KiB。取消会终止所有活动
及排队任务。并发数、总超时、聚合输出、Worker 回合数和模型最终输出 Token 上限可在
配置页调整，但单任务输入和协议最终输出的 32 KiB 限制保持不变。子任务的模型 token
会计入终端累计用量，但不会覆盖主 Agent 的当前上下文占用。

zeda 只集成本机 `clangd`，不下载或启动其他语言服务器。C/C++ 文件可通过 `lsp`
工具查询 diagnostics、hover、definition、references 和 document symbols；成功的
`write` 与 `edit` 会等待当前文件的新诊断并附加到工具结果。启动时依次查找 workspace
根目录、`build/` 和 `build-debug/` 中的 `compile_commands.json`，也可用
`ZED_CLANGD_PATH` 指定其他本机 clangd。

## 外部插件与 DeepWiki

zeda 启动时从安装前缀的 `lib/zeda/plugins/` 发现版本化 C ABI 插件。开发时也会
检查可执行文件旁的 `plugins/`，并可用冒号分隔的 `ZED_PLUGIN_PATH` 追加目录。
`/plugins` 会列出成功加载和加载失败的插件；ABI 不匹配、manifest 错误和命令或工具
冲突都会显示为可见错误。插件是受信任的本机动态库，首版不提供进程隔离或热卸载。
命令的 `options_json` 项可以用 `"view":"document"` 声明交互式文档选项；该选项的
执行结果必须是 `schema_version: 1` 的 JSON，包含标题和非空页面数组。宿主会在工作
线程中解析并限制快照为 16 MiB、单页为 2 MiB、最多 128 页，普通非 TTY 输出不会
泄露该内部协议。

构建默认包含首个外部插件 DeepWiki。它只分析本地 C/C++ 仓库，使用 SQLite FTS5、
本机 clangd 和当前 OpenCode 模型生成中文 Wiki，不使用 embedding 服务。索引同时记录
clangd 符号、`#include` 关系和 CMake target/link 关系。首次使用：

```text
/deepwiki generate
/deepwiki status
/deepwiki tui
/deepwiki open
```

代码变化后执行 `/deepwiki update`。无变化时不会调用模型；普通变化只重新索引文件并
生成引用了这些文件的页面。Wiki、索引和状态保存在当前仓库的 `.zed/deepwiki/`，插件
不会修改源码或 `.gitignore`，因此未忽略 `.zed/` 的仓库会把它显示为未跟踪目录。

在全屏终端中执行 `/deepwiki tui` 会进入内置双栏浏览器：左侧是页面目录，右侧渲染
Markdown。用 `↑/↓` 或 `j/k` 选页，`Tab` 或 `←/→` 在目录和正文之间切换焦点，正文
支持方向键、翻页键和鼠标滚轮滚动，按 `Esc` 或 `q` 返回 Agent 对话。过期页面会显示
`stale` 标记；重定向输入输出时该命令会明确提示需要交互式 TTY，而不会输出内部 JSON。

`/deepwiki open` 只在 `127.0.0.1` 随机端口启动网页，并自动打开浏览器。网页提供目录、
Markdown、Mermaid 图、源码引用预览和流式问答；进程退出时服务停止。普通 Agent 也可
使用插件注册的 `deepwiki_structure`、`deepwiki_contents` 和 `deepwiki_search` 三个
只读工具。

没有 `compile_commands.json` 时仍可建立文本索引，但 `/deepwiki status` 会显示
`degraded`；正常状态还会显示当前提交和过期页面数。CMake 项目建议先生成编译数据库：

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

可以用 `-DZEDA_BUILD_DEEPWIKI_PLUGIN=OFF` 关闭插件构建。DeepWiki 插件额外链接系统
SQLite3，并固定使用 cpp-httplib 0.51.0；网页内置固定版本的 Marked、DOMPurify 和
Mermaid，不在运行时从 CDN 加载。

## 模块

```text
include/zed/core/       Agent Loop、消息、模型/工具接口、上下文管理
include/zed/providers/  OpenCode Go Provider
include/zed/session/    JSONL Session
include/zed/tools/      文件和 Shell 工具
include/zed/ui/         FTXUI 终端 UI
include/zed/skills/     Skill 发现和加载
include/zed/extensions/ Extension 命令注册
include/zed/plugins/    外部插件 C ABI 和宿主加载器
include/zed/subagents/  内置 Agent、Worker 协议与进程编排
include/zed/app/        启动配置与本地 Web 配置服务
plugins/deepwiki/       C/C++ DeepWiki 插件与本地网页资源
```

`ContextController` 是可选的。没有上下文模型时，`BasicContextManager` 使用确定性规则裁剪；接入模型后，在上下文达到阈值时请求结构化摘要和保留列表。

在全屏 TUI 中用鼠标左键拖拽选中文本并松开后，zeda 会通过 OSC 52
请求终端将所选文本写入剪贴板，并在底栏显示复制状态。普通单击
仍用于展开或收起可折叠记录。该能力需要终端支持 OSC 52。

终端采用类似 Codex CLI 的布局：欢迎页显示版本、模型、思考强度、工作目录和从进入
`main` 到界面就绪的启动耗时。耗时精确到 0.001 ms；仅单独显示严格大于 1 ms 的
阶段，其余阶段合并为 `other`，分项之和仍等于总耗时。例如：

```text
startup: 23.000 ms = config 2.000 + session 3.000 + setup 5.000 + plugins 10.000 + ui 2.000 + other 1.000
```

欢迎页与消息记录位于同一个可滚动区域，因此向下浏览后不会固定占用屏幕。鼠标滚轮每次固定滚动 3 行；默认自动跟随最新消息，向上滚动后暂停跟随，滚回底部、执行命令或提交新请求时恢复。输入框在没有命令补全列表时支持使用上下方向键切换本进程中已发送的模型 prompt，回到最新位置时恢复未发送草稿。底部固定输入框、运行状态和 token 指标。底栏左侧根据 Agent 事件显示紧凑状态；思考时会显示当前强度（例如 `low think`），活动状态带旋转动画，空闲时显示静态 `idle`。用户、助手、工具和错误消息分别渲染，只有助手内容会进入 Markdown 解析，避免输入或工具输出意外破坏对话布局。

底栏右侧显示 token 指标：`ctx` 是最近一次模型请求的 input tokens，`↑` 和 `↓` 分别是本次进程内所有模型调用的累计 input 与 output，`Σ` 后的值是两者之和。数值达到千级或百万级时使用 `k` 或 `m` 紧凑显示。一次 Agent 请求包含多轮工具调用时，每轮模型 usage 都会计入。`ctx` 可以点击，打开 Context analysis 面板；面板外的聊天页面会变暗，面板自身保持不透明。总量采用供应商返回的精确 usage，页面会显示容量、剩余空间、缓存命中，以及系统指令、用户消息、助手消息、工具往返、工具定义和协议开销的估算分布。再次点击 `ctx` 或按 Esc 关闭。

Provider 按模型选择 Responses、Chat Completions 或 Anthropic Messages 的 SSE
协议并增量读取模型输出。全屏终端会将密集的 `assistant_delta` 按约 30 FPS
合并刷新，避免事件队列直到响应结束才统一重绘；输入或输出被重定向时，普通 CLI
会立即写出并刷新每个文本增量。Session 仍只在一次模型响应成功并通过工具调用校验后
追加完整 assistant 消息。

主 Agent 请求不发送 `max_output_tokens`，因此 zeda 不设置单次响应输出上限，由模型供应商和模型自身的上下文窗口决定实际最大值。内部上下文摘要请求仍使用独立的 1024-token 上限。`ZED_RESERVED_OUTPUT_TOKENS` 只控制构建上下文时为输出预留的空间，不会截断模型输出。

Agent Loop 只在 Provider 明确发出 `response.completed`、不存在待执行工具调用且回复不是延后执行提示时正常结束。`response.incomplete`、`response.failed`、缺少终止事件和不一致的工具结束状态都会显示为错误；达到输出 token 上限不会再被误报为成功。默认 coding-agent 指令要求模型直接使用工具完成并验证工作。模型若只回复“正在构建”“马上开始”等短促进度提示，Agent Loop 会纠偏重试一次；再次不执行则明确失败，并且这些无效进度回复不会写入 Session。

Markdown 当前支持标题、段落、粗体、行内代码、链接、列表、引用、代码块和横线。Markdown 源文中分隔块级内容的一个或多个空行，会在终端中保留为一个空白行。表格使用 FTXUI 的表格布局，支持表头、单线边框、列分隔线、换行以及 `:---`、`:---:`、`---:` 对齐语法；表格单元格中的 `\\|` 会按字面竖线处理。
