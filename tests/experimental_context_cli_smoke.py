#!/usr/bin/env python3
"""Exercise a built/installed zeda against a loopback-only scripted provider.

Run: python3 tests/experimental_context_cli_smoke.py /absolute/path/to/zeda
No real API, shell tools, user configuration, or third-party Python packages.
"""

import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading


def main():
    executable = Path(sys.argv[1]).resolve(strict=True)
    captured = []
    responses = []
    errors = []

    class Provider(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            try:
                assert self.path == "/v1/chat/completions", self.path
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                captured.append(body)
                validate, delta, finish = responses.pop(0)
                validate(body)
                payload = json.dumps({
                    "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
                    "usage": {"prompt_tokens": 1000, "completion_tokens": 50},
                })
                output = ("data: " + payload + "\n\ndata: [DONE]\n\n").encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(output)))
                self.end_headers()
                self.wfile.write(output)
            except Exception as error:
                errors.append(repr(error))
                self.send_error(500, "fixture assertion failed")

    def names(body):
        return {tool["function"]["name"] for tool in body["tools"]}

    def messages(body):
        return json.dumps(body["messages"], ensure_ascii=False)

    def expect_disabled(body):
        assert not {"context_notes", "context_history", "new_context"} & names(body)
        assert "Experimental context management is enabled" not in messages(body)

    def expect_enabled(body):
        assert {"context_notes", "context_history", "new_context"} <= names(body)
        assert "Experimental context management is enabled" in messages(body)

    def tool(index, tool_name, **arguments):
        return {"index": index, "id": "fixture-" + str(len(captured)) + "-" + str(index),
                "type": "function", "function": {"name": tool_name, "arguments": json.dumps(
                    {"purpose": "Verify context recovery", **arguments})}}

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Provider)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="zeda-context-cli-") as temporary:
            root = Path(temporary)
            workspace = root / "workspace"
            workspace.mkdir()
            (workspace / "proof.txt").write_text("TOOL_BATCH_PERSISTED\n")
            session = workspace / ".zed" / "sessions" / "primary.jsonl"
            archive = Path(str(session) + ".context")
            env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                   "LANG": "C.UTF-8", "NO_PROXY": "127.0.0.1,localhost",
                   "OPENCODE_GO_API_KEY": "local-fixture-key",
                   "ZED_OPENCODE_AUTH_PATH": str(root / "no-auth.json"),
                   "ZED_WORKSPACE": str(workspace), "ZED_SESSION_PATH": str(session),
                   "ZED_MODEL": "glm-5.1", "ZED_REASONING_EFFORT": "auto",
                   "ZED_OPENCODE_ENDPOINT": f"http://127.0.0.1:{server.server_port}/v1",
                   "ZED_REQUEST_TIMEOUT_MS": "5000"}

            def run(commands):
                result = subprocess.run([str(executable)], cwd=workspace, env=env,
                                        input=commands + "\n/exit\n", text=True,
                                        capture_output=True, timeout=30)
                assert result.returncode == 0, result.stderr
                assert not errors, errors
                assert not responses, f"Unused provider responses: {len(responses)}; {result.stdout}"
                assert "local-fixture-key" not in result.stdout + result.stderr
                return result.stdout

            responses.append((expect_enabled, {"content": "DEFAULT_ON_OK"}, "stop"))
            assert "DEFAULT_ON_OK" in run("fixture default context memory")

            run("/configure context set experimental-mode off")
            config_file = workspace / ".zed" / "config.json"
            assert json.loads(config_file.read_text())["context"]["experimental_mode"] is False
            responses.append((expect_disabled, {"content": "BASELINE_NEEDLE: 7349"}, "stop"))
            assert "BASELINE_NEEDLE" in run("fixture baseline")
            assert not archive.exists(), "Disabled mode created context state"

            run("/configure context set experimental-mode on")
            assert json.loads(config_file.read_text())["context"]["experimental_mode"] is True
            assert not archive.exists(), "Enabling config changed the active running mode"

            def after_reset(body):
                expect_enabled(body)
                assert "Current context window: window-2" in messages(body)
                assert "TOOL_BATCH_PERSISTED" in messages(body)
                assert "checkpoint evidence" in messages(body)

            responses.append((expect_enabled, {"tool_calls": [
                tool(0, "context_notes", action="write", name="checkpoint", text="checkpoint evidence: inspect BASELINE_NEEDLE"),
                tool(1, "new_context"),
                tool(2, "read", path="proof.txt"),
            ]}, "tool_calls"))
            responses.append((after_reset, {"content": "WINDOW_RECOVERY_OK"}, "stop"))
            assert "WINDOW_RECOVERY_OK" in run("fixture context switch")
            state = [json.loads(line) for line in archive.read_text().splitlines()]
            assert any(record["type"] == "checkpoint" for record in state)
            assert any(record["type"] == "note" for record in state)
            assert archive.stat().st_mode & 0o777 == 0o600

            def restart(body):
                expect_enabled(body)
                assert "Current context window: window-2" in messages(body)
                assert "checkpoint evidence" in messages(body)

            def searched(body):
                expect_enabled(body)
                assert "7349" in body["messages"][-1]["content"]

            responses.append((restart, {"tool_calls": [tool(0, "context_history", action="search", query="BASELINE_NEEDLE")]}, "tool_calls"))
            responses.append((searched, {"content": "RESTART_HISTORY_OK"}, "stop"))
            assert "RESTART_HISTORY_OK" in run("fixture resume")

            output = run("/session fork context-fork\n/session list")
            assert "forked session: context-fork" in output
            forks = [path for path in session.parent.glob("*.jsonl") if path != session]
            assert len(forks) == 1
            fork_state = Path(str(forks[0]) + ".context").read_text()
            assert "checkpoint evidence" in fork_state and "window-2" in fork_state

            original_archive = archive.read_bytes()
            run("/configure context set experimental-mode off")
            responses.append((expect_disabled, {"content": "DISABLED_AGAIN_OK"}, "stop"))
            assert "DISABLED_AGAIN_OK" in run("fixture disabled again")
            assert archive.read_bytes() == original_archive
            print("PASS: default on, explicit disable/enable, full tool batch, window reset, "
                  "restart/history, fork, disable; 7 scripted HTTP requests; no real API")
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


if __name__ == "__main__":
    main()
