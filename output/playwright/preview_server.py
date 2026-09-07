from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import urlparse, parse_qs
import json
ROOT = Path(__file__).resolve().parents[2]
PAGES = [
 ('overview','系统架构概览','从请求到执行，理解 Agent 的核心协作方式'),
 ('loop','Agent 执行流程','模型响应、工具调度与回合状态转换'),
 ('provider','模型与协议','供应商适配与流式消息处理'),
 ('session','会话与恢复','持久化边界、失败处理与安全恢复'),
 ('tools','工具与扩展','工具契约、插件生命周期与执行边界')]
MARKDOWN = '''# 系统架构概览

zeda 是一个面向 C/C++ 工作区的 coding agent。它将模型调用、工具执行与会话记录串成可追踪的工作流程，让每一次改动都有明确的上下文。

## 核心设计

系统围绕 **Agent Loop** 运转。核心层负责状态转换，供应商负责协议适配，工具通过统一注册表执行；终端界面消费事件并展示进度。 [src/core/agent_loop.cpp:1]

> 阅读建议：先了解组件之间的数据流，再进入「Agent 执行流程」查看完整回合。

## 一次请求如何流转

```mermaid
flowchart LR
    A[用户请求] --> B[Agent Loop]
    B --> C[模型供应商]
    C --> D[工具注册表]
    D --> E[会话记录]
    E --> B
```

## 关键组件

| 组件 | 职责 | 边界 |
| --- | --- | --- |
| 核心循环 | 协调模型与工具执行 | 发出类型化事件 |
| 模型供应商 | 适配协议与增量响应 | 返回明确错误 |
| 会话存储 | 持久化消息和执行结果 | 支持中断恢复 |

## 执行约束

```cpp
const auto result = registry.execute(call, cancellation);
if (!result) {
  return report_error(result.error());
}
```

工具执行前验证参数，执行后限制输出。恢复遇到未知副作用时，需要明确决定后续动作。 [src/core/agent_loop.cpp:20]
'''
class Handler(BaseHTTPRequestHandler):
 def do_GET(self):
  parsed=urlparse(self.path); path=parsed.path
  if path=='/api/toc': return self.send(json.dumps([dict(id=i,title=t,description=d) for i,t,d in PAGES],ensure_ascii=False),'application/json')
  if path=='/api/terms': return self.send(json.dumps([dict(program_name='AgentLoop',chinese_name='核心循环',description='协调模型响应、工具调用与回合状态转换。',kind='class',source='src/core/agent_loop.cpp:1')],ensure_ascii=False),'application/json')
  if path=='/api/page':
   page_id=parse_qs(parsed.query).get('id',['overview'])[0]
   title=next((t for i,t,d in PAGES if i==page_id),'系统架构概览')
   return self.send(MARKDOWN.replace('# 系统架构概览','# '+title,1),'text/plain')
  if path=='/api/source': return self.send(''.join(f'{n}  {line}\n' for n,line in enumerate((ROOT/'src/core/agent_loop.cpp').read_text().splitlines()[:55],1)),'text/plain')
  if path=='/favicon.ico': self.send_response(204); self.end_headers(); return
  asset=ROOT/'build/plugins/deepwiki/resources'/path.lstrip('/') if path.startswith('/vendor/') else ROOT/'plugins/deepwiki/web'/('index.html' if path=='/' else path.lstrip('/'))
  if not asset.is_file(): self.send_error(404); return
  self.send(asset.read_bytes(),{'.html':'text/html','.css':'text/css','.js':'application/javascript'}.get(asset.suffix,'text/plain'))
 def do_POST(self):
  self.rfile.read(int(self.headers.get('Content-Length','0')))
  delta='核心循环在工具执行前验证参数，随后记录结果。恢复路径需要保留未知结果状态。 [src/core/agent_loop.cpp:20]'
  self.send('data: '+json.dumps({'delta':delta})+'\n\nevent: done\ndata: {}\n\n','text/event-stream')
 def send(self,data,kind):
  if isinstance(data,str): data=data.encode()
  self.send_response(200); self.send_header('Content-Type',kind+'; charset=utf-8'); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
ThreadingHTTPServer(('127.0.0.1',8766),Handler).serve_forever()
