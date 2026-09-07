# OpenCode Go provider 契约与验证

验证日期：2026-09-07。范围是 coding agent 使用的文本、reasoning、函数工具及多轮对话，
覆盖 Go 的 Responses、Chat Completions、Anthropic Messages 三条接口。
图片、音频、供应商托管工具及服务端会话 API 尚未实现；新增模型的参数和能力需要继续验证。

## 会话身份与缓存

每个请求发送 `User-Agent: zeda/<构建版本>`、`x-opencode-client: zeda` 和
`x-opencode-session`。主 Agent 使用 Session 的 workspace、id、创建时间计算稳定的
非秘密指纹，因此恢复、重命名和继续对话保持标识；派生或切换到另一个 Session 使用其自身标识。
指纹用于会话关联，不承担认证或安全隔离。HTTP header 不发送 workspace 路径。

内存 Session（包括 Sub Agent）分别生成会话标识。上下文控制器的独立会话标识在其对象
生命周期内保持稳定。直接调用 provider 且未指定 `ModelRequest.session_id` 的请求被视为
独立调用，自动生成标识；有连续历史的调用方应显式复用标识。

- Responses：`store:false`，`prompt_cache_key` 与会话标识一致，请求 encrypted reasoning。
- Chat Completions：保持历史和 `reasoning_content`，依靠模型和 Go 的前缀缓存；不统一注入其他协议的缓存字段。
- Messages：system 文本和最后两个可缓存的对话消息使用 `cache_control: {type: ephemeral}`。
  thinking/redacted_thinking 块不附加缓存标记；同角色的连续块合并到同一消息，多个 tool result
  在紧随 assistant tool_use 的同一 user 消息中回传。
- 工具按名称排序后序列化。修改 system、工具定义、历史内容或压缩上下文会改变缓存前缀。
- 不默认发送 `prompt_cache_retention:24h`，不承诺缓存 TTL、固定上游或百分之百命中。

`ModelUsage.input_tokens` 统一表示总输入，包括缓存读取和缓存写入。
`cached_input_tokens` 只表示缓存读取，`cache_write_input_tokens` 单独表示写入。
Messages 的总输入是 `input_tokens + cache_read_input_tokens + cache_creation_input_tokens`。
流式 usage 更新按累计值更新，缺失字段保留先前值。Worker JSON 的缓存写入字段是可选的，
旧消息未提供时按零处理。

## 多轮状态与失败边界

`Message.model_state` 保存 provider 专用的 continuation，core 将其作为不透明字符串传递。
Session v2 message 记录增加可选同名字段；现有记录仍可读取。恢复和派生保留此字段。
旧版本程序会忽略该字段，降级后的多轮 reasoning 回传不在保证范围内。

Responses 保留原始 reasoning/message/function_call output items，并通过 `output_index`、
`item_id` 关联 arguments 增量；`call_id` 用于回传工具结果。Chat 保留 reasoning_content；
Messages 保留 thinking、signature、redacted_thinking 和工具块。
切换模型/协议时不向新模型发送旧模型的 continuation。

Continuation 和对应的 assistant 内容、工具调用必须一致。Hook 不得修改已绑定 continuation
的模型输出或工具参数，也不得修改宿主会话标识；不一致会在执行或下次模型请求前明确失败。
上下文估算保守计入不透明状态的字节数。

HTTP 层禁用用户 `.curlrc`，限制协议为 HTTP/HTTPS，保持子进程环境白名单。错误包含 HTTP
状态和有界的服务端错误描述，API Key 会脱敏；JSON 解析错误不回显原始 payload。
HTTP 408/429/5xx 和请求超时标记为可重试。当前 provider 不自动重试、不自动切换模型。
调用方若增加重试，应限制次数并显式呈现，避免部分流式输出重复或重放有副作用的工具。

SSE 支持网络任意分片、CRLF、注释和多行 data，按事件边界解析。取消和超时优先于残留
半包解析，并清理 curl 子进程。请求及响应分别限制为 16 MiB，SSE 行/事件限制为 1 MiB，
工具调用/输出块索引限制为 128，JSON 嵌套限制为 128 层。

缺少终态、非法工具参数、负数/小数 usage、未结束的内容块和显式服务端错误均可见。
Go 在 Responses 完成后追加的计费 `ping` 被接受；Chat 工具增量中的可空 id/type/name
按缺失片段处理。Chat 结束后的空 choice 与最终 usage 可以携带相同的 finish_reason，
但不得追加内容或改变结束原因。默认测试不访问外部服务。

## 可重复验证

构建并运行独立协议测试：

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure -R 'zed_opencode_go_'
```

测试包含实际 headers 与 JSON、三种协议的两轮回传、UTF-8 单字节分片、首 delta 握手、
HTTP/SSE 错误、非法输入、超时/取消、Session 恢复/派生、Hook 透传和完整工具执行回合。
本机 fixture 使用随机回环端口，设置 I/O 和 CTest 超时并回收线程。

可选的官方 SDK 对照在独立 Python 环境运行；依赖不进入 C++ 运行产物：

```sh
python3 -m venv /tmp/zeda-go-sdk
/tmp/zeda-go-sdk/bin/pip install -r tests/opencode_go_sdk_requirements.txt
/tmp/zeda-go-sdk/bin/python tests/opencode_go_sdk_reference.py build/zed_opencode_go_contract
```

该测试让 C++ 和固定版本的 OpenAI/Anthropic Python SDK 请求同一本机 fixture，比较请求
路径、JSON 语义、解析后的文本和 usage。JSON 对象键顺序不影响比较；消息数组顺序保留。
SDK 禁用环境代理和自动重试。使用固定假凭证，不请求真实 API。

安装后验证实际二进制的文件读取、工具结果回传及进程重启后的 Session 续聊：

```sh
python3 tests/opencode_go_installed_smoke.py /opt/homebrew/bin/zeda
```

该脚本使用临时工作目录、假凭证和本机 HTTP fixture，不读取生产项目文件。

真实验证须显式运行，不注册到 CTest：

```sh
# OPENCODE_GO_API_KEY 由调用环境安全提供。
./build/zed_opencode_go_contract live muse-spark-1.2-contributor
./build/zed_opencode_go_contract live deepseek-v4-flash
./build/zed_opencode_go_contract live minimax-m3
```

每次最多三个请求，每个输出上限 1024 tokens、超时 120 秒，仅发送程序生成的合成数据，
工具结果也是合成内容，不执行文件或 Shell 工具。输出模型、usage、耗时、结束原因和工具数量，
不输出 API Key、prompt、reasoning 或回答原文。命中率取决于前缀、模型与服务端状态；
不将固定缓存命中阈值作为离线测试断言。

## 真实模型验证记录

2026-09-07，从 Go 的 `/zen/go/v1/models` 获取 35 个型号，逐个验证合成工具调用、
工具结果回传和后续对话。31 个型号通过三轮验证；这证明此次文本/函数工具路径可用，
不代表所有参数组合、多模态能力或后续服务状态均已验证。

| 模型系列 | 通过的型号 |
| --- | --- |
| MiniMax | `minimax-m3`、`minimax-m2.7`、`minimax-m2.5` |
| Kimi | `kimi-k3`、`kimi-k2.7-code`、`kimi-k2.6` |
| LongCat | `longcat-2.0` |
| GLM | `glm-5.2`、`glm-5.3-flash`、`glm-5.3`、`glm-5.1`、`glm-5` |
| DeepSeek | `deepseek-v4-pro`、`deepseek-v4-flash`、`deepseek-v4-flash-vision-exp`（仅文本） |
| Qwen | `qwen3.7-max`、`qwen3.8-max`、`qwen3.8-flash`、`qwen3.7-plus`、`qwen3.6-plus`、`qwen3.5-plus` |
| MiMo | `mimo-v2.5-pro`、`mimo-v2.5` |
| Hy | `hy4-preview`、`hy3` |
| GPT | `gpt-5.6-luna` |
| Grok | `grok-4.5`、`grok-4.6` |
| Muse | `muse-spark-1.3-contributor`、`muse-spark-1.2-contributor` |
| Omen | `omen-alpha` |

另外四个型号在首次请求被网关拒绝：

| 型号 | 实测错误 |
| --- | --- |
| `kimi-k2.5` | HTTP 400：上游路由限制下没有可用 provider |
| `mimo-v2-pro`、`mimo-v2-omni` | HTTP 400：上游不支持该型号 |
| `hy3-preview` | HTTP 400：型号不可用 |

这些错误会明确返回，不自动替换型号。模型目录列出型号不等于上游可用。

缓存读取示例（均为服务端 usage，非本地推算）：Muse 1.2 第二轮为 8945 / 9184
输入 tokens，MiniMax M3 第二轮为 8821 / 8890，DeepSeek V4 Flash 第三轮为
9472 / 9494。Kimi K3 前两轮为零、第三轮为 8448 / 8718，说明正确回传会话与历史
仍不能消除所有缓存 miss。这些数字来自一次合成负载测试，不是性能或命中率保证。

## 来源

- [Go 官方接入要求及协议端点](https://opencode.ai/docs/go/)
- [OpenCode 请求构造](https://github.com/anomalyco/opencode/blob/dev/packages/opencode/src/session/llm/request.ts)
- [OpenCode 参数与缓存转换](https://github.com/anomalyco/opencode/blob/dev/packages/opencode/src/provider/transform.ts)
- [OpenCode 网关路由](https://github.com/anomalyco/opencode/blob/dev/packages/console/app/src/routes/zen/util/handler.ts)
- [OpenAI SDK 文档](https://developers.openai.com/api/docs/libraries)
- [Anthropic 缓存语义](https://platform.claude.com/docs/en/build-with-claude/prompt-caching)
