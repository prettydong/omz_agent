"""Run the actual installed zeda against a local two-turn provider fixture."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def event(value):
    return "data: " + json.dumps(value) + "\n\n"


requests = []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append((dict(self.headers), body))
        if len(requests) == 1:
            delta = {"reasoning_content": "Inspect synthetic fixture", "tool_calls": [{
                "index": 0, "id": "fixture_read", "type": "function",
                "function": {"name": "read", "arguments": json.dumps({
                    "purpose": "Check synthetic input", "path": "fixture.txt"})}}]}
            finish = "tool_calls"
        else:
            delta = {"content": "INSTALLED_GO_OK"}
            finish = "stop"
        data = (event({"choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
                       "usage": {"prompt_tokens": 100, "completion_tokens": 10,
                                 "prompt_tokens_details": {"cached_tokens": 80}}})
                + "data: [DONE]\n\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


binary = str(Path(sys.argv[1]).resolve())
server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
thread = threading.Thread(target=server.serve_forever)
thread.start()
try:
    with tempfile.TemporaryDirectory(prefix="zeda-installed-go-") as directory:
        root = Path(directory)
        (root / "fixture.txt").write_text("SYNTHETIC_FILE_VALUE\n")
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("ZED_", "OPENCODE_"))}
        env.update(OPENCODE_GO_API_KEY="fixture-key", ZED_WORKSPACE=directory,
                   ZED_SESSION_PATH=str(root / "session.jsonl"), ZED_MODEL="deepseek-v4-flash",
                   ZED_REASONING_EFFORT="auto", ZED_REQUEST_TIMEOUT_MS="3000",
                   ZED_OPENCODE_ENDPOINT=f"http://127.0.0.1:{server.server_port}/v1")
        result = subprocess.run([binary], input="Read fixture.txt\n/exit\n", env=env,
                                cwd=root, text=True, capture_output=True, timeout=25)
        assert result.returncode == 0, result.stderr
        assert "INSTALLED_GO_OK" in result.stdout, result.stdout[-2000:]
        assert len(requests) == 2, len(requests)
        first_headers = {k.lower(): v for k, v in requests[0][0].items()}
        second_headers = {k.lower(): v for k, v in requests[1][0].items()}
        assert first_headers["user-agent"].startswith("zeda/")
        assert first_headers["x-opencode-session"] == second_headers["x-opencode-session"]
        messages = requests[1][1]["messages"]
        assert any(m.get("reasoning_content") == "Inspect synthetic fixture" for m in messages)
        assert any(m.get("role") == "tool" and m.get("tool_call_id") == "fixture_read"
                   and "SYNTHETIC_FILE_VALUE" in m["content"] for m in messages)
        resumed = subprocess.run([binary], input="Continue\n/exit\n", env=env,
                                 cwd=root, text=True, capture_output=True, timeout=25)
        assert resumed.returncode == 0 and "INSTALLED_GO_OK" in resumed.stdout
        assert len(requests) == 3
        resumed_headers = {k.lower(): v for k, v in requests[2][0].items()}
        assert resumed_headers["x-opencode-session"] == first_headers["x-opencode-session"]
        assert any(m.get("reasoning_content") == "Inspect synthetic fixture"
                   for m in requests[2][1]["messages"])
        records = [json.loads(line) for line in (root / "session.jsonl").read_text().splitlines()]
        assert any(r.get("type") == "message" and r.get("model_state") for r in records)
        assert records[-1]["type"] == "turn_end" and records[-1]["outcome"] == "completed"
        assert sum(r.get("type") == "turn_end" for r in records) == 2
        print("PASS installed binary: stable headers across restart, actual read tool, reasoning replay, two persisted completed turns")
finally:
    server.shutdown()
    thread.join()
    server.server_close()
