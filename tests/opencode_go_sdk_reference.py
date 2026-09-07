"""Optional, offline wire comparison with pinned official SDKs.

Install tests/opencode_go_sdk_requirements.txt in a separate virtualenv, then:
python tests/opencode_go_sdk_reference.py build/zed_opencode_go_contract
No credentials, external API requests, or repository files are sent.
"""

import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import anthropic
import openai
import httpx

SESSION = "zeda-fixture-session-0123456789abcdef"
SCHEMA = {"type": "object", "properties": {"key": {"type": "string"}}, "required": ["key"]}
SYSTEM = "Stable instructions"
USER = [{"role": "user", "content": "Use lookup"}]


def event(value):
    return "data: " + json.dumps(value, ensure_ascii=False) + "\n\n"


def responses_events(model):
    output = [{"id": "msg_fixture", "type": "message", "role": "assistant", "status": "completed",
               "content": [{"type": "output_text", "text": "done", "annotations": []}]}]
    return event({"type": "response.output_text.delta", "delta": "done", "item_id": "msg_fixture",
                  "output_index": 0, "content_index": 0, "sequence_number": 1}) + event({
        "type": "response.completed", "sequence_number": 2,
        "response": {"id": "resp_fixture", "object": "response", "created_at": 0, "model": model,
                     "status": "completed", "output": output,
                     "usage": {"input_tokens": 20, "output_tokens": 2, "total_tokens": 22,
                               "input_tokens_details": {"cached_tokens": 10},
                               "output_tokens_details": {"reasoning_tokens": 0}}}})


def chat_events(model):
    return event({"id": "chat_fixture", "object": "chat.completion.chunk", "created": 0, "model": model,
                  "choices": [{"index": 0, "delta": {"content": "done"}, "finish_reason": "stop"}],
                  "usage": {"prompt_tokens": 20, "completion_tokens": 2, "total_tokens": 22,
                            "prompt_tokens_details": {"cached_tokens": 10}}}) + "data: [DONE]\n\n"


def messages_events(model):
    parts = [
        {"type": "message_start", "message": {"id": "msg_fixture", "type": "message", "role": "assistant",
         "content": [], "model": model, "stop_reason": None, "stop_sequence": None,
         "usage": {"input_tokens": 10, "output_tokens": 0, "cache_read_input_tokens": 20,
                   "cache_creation_input_tokens": 30}}},
        {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}},
        {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "done"}},
        {"type": "content_block_stop", "index": 0},
        {"type": "message_delta", "delta": {"stop_reason": "end_turn", "stop_sequence": None},
         "usage": {"output_tokens": 2}},
        {"type": "message_stop"},
    ]
    return "".join("event: " + part["type"] + "\n" + event(part) for part in parts)


def check(binary, protocol, model):
    captured = []
    data = {"responses": responses_events, "chat/completions": chat_events,
            "messages": messages_events}[protocol](model).encode()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            captured.append((self.path, json.loads(self.rfile.read(int(self.headers["Content-Length"])))))
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}/v1"
    try:
        cpp = subprocess.run([binary, "sdk-reference", base, model], check=True,
                             capture_output=True, text=True, timeout=10)
        actual = json.loads(cpp.stdout)
        normalized = {"content": "", "input": 0, "cache_read": 0, "cache_write": 0, "output": 0}
        if protocol == "messages":
            client = anthropic.Anthropic(api_key="fixture-key", base_url=base.removesuffix("/v1"), max_retries=0,
                                         http_client=httpx.Client(trust_env=False, timeout=5))
            stream = client.messages.create(model=model, stream=True, max_tokens=131072, temperature=0,
                thinking={"type": "adaptive"},
                system=[{"type": "text", "text": SYSTEM, "cache_control": {"type": "ephemeral"}}],
                messages=[{"role": "user", "content": [{"type": "text", "text": "Use lookup",
                                                        "cache_control": {"type": "ephemeral"}}]}],
                tools=[{"name": "lookup", "description": "Read fixture data", "input_schema": SCHEMA}])
            for part in stream:
                if part.type == "message_start":
                    usage = part.message.usage
                    normalized["cache_read"] = usage.cache_read_input_tokens
                    normalized["cache_write"] = usage.cache_creation_input_tokens
                    normalized["input"] = usage.input_tokens + normalized["cache_read"] + normalized["cache_write"]
                elif part.type == "content_block_delta" and part.delta.type == "text_delta":
                    normalized["content"] += part.delta.text
                elif part.type == "message_delta":
                    normalized["output"] = part.usage.output_tokens
        else:
            client = openai.OpenAI(api_key="fixture-key", base_url=base, max_retries=0,
                                         http_client=httpx.Client(trust_env=False, timeout=5))
            if protocol == "responses":
                stream = client.responses.create(model=model, stream=True, store=False, temperature=0,
                    instructions=SYSTEM, input=USER, prompt_cache_key=SESSION, include=["reasoning.encrypted_content"],
                    tools=[{"type": "function", "name": "lookup", "description": "Read fixture data",
                            "parameters": SCHEMA, "strict": False}])
                for part in stream:
                    if part.type == "response.output_text.delta":
                        normalized["content"] += part.delta
                    elif part.type == "response.completed":
                        usage = part.response.usage
                        normalized.update(input=usage.input_tokens, output=usage.output_tokens,
                                          cache_read=usage.input_tokens_details.cached_tokens)
            else:
                stream = client.chat.completions.create(model=model, stream=True, temperature=0,
                    stream_options={"include_usage": True}, messages=[{"role": "system", "content": SYSTEM}] + USER,
                    tools=[{"type": "function", "function": {"name": "lookup", "description": "Read fixture data",
                                                             "parameters": SCHEMA}}])
                for part in stream:
                    for choice in part.choices:
                        normalized["content"] += choice.delta.content or ""
                    if part.usage:
                        normalized.update(input=part.usage.prompt_tokens, output=part.usage.completion_tokens,
                                          cache_read=part.usage.prompt_tokens_details.cached_tokens)
        stream.close()
        client.close()
        assert actual == normalized, (protocol, actual, normalized)
        assert len(captured) == 2
        assert captured[0] == captured[1], (protocol, captured)
        print(f"PASS {protocol}: SDK and C++ wire body and parsed output/usage agree")
    finally:
        server.shutdown()
        thread.join()
        server.server_close()


if __name__ == "__main__":
    for protocol, model in [("responses", "muse-spark-1.2-contributor"),
                            ("chat/completions", "deepseek-v4-flash"), ("messages", "minimax-m3")]:
        check(sys.argv[1], protocol, model)
