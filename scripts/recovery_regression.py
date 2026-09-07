#!/usr/bin/env python3
"""Exercise response correction and HTTP recovery using the real zeda binary.

Uses the isolated PTY runner and local Chat Completions/Responses fixtures.
No real credentials or external model services are used.
"""

import argparse
from http.server import BaseHTTPRequestHandler
import json
from pathlib import Path
import shutil
import subprocess
import time

from tui_pty_regression import Regression


class RecoveryFixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            self.respond()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self.server.fixture_errors.append(str(error))
            self.send_error(500, 'Recovery fixture failed')

    def respond(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(body)
        messages = body.get('messages', body.get('input', []))
        prompt = next(m['content'] for m in reversed(messages) if m.get('role') == 'user')
        if not isinstance(prompt, str):
            prompt = '\n'.join(part.get('text', '') for part in prompt)
        case = prompt.removeprefix('TUI_')
        attempt = self.server.attempts.get(prompt, 0) + 1
        self.server.attempts[prompt] = attempt
        instructions = body.get('instructions', '') or messages[0]['content']
        if ((case in ('EMPTY_ANSWER', 'EMPTY_ALWAYS') and attempt > 1)
                or (case == 'EMPTY_CALLS' and attempt == 2)):
            assert 'Validation diagnostic:' in instructions
        if case.startswith('HTTP_'):
            assert 'Validation diagnostic:' not in instructions
        status = None
        if case == 'HTTP_401':
            status = 401
        elif case == 'HTTP_EXHAUST' or (case == 'HTTP_503' and attempt == 1):
            status = 503
        elif case == 'HTTP_CANCEL' or (case == 'HTTP_429' and attempt == 1):
            status = 429
        if status is not None:
            self.send_response(status)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Retry-After', '5' if case == 'HTTP_CANCEL' else ('1' if case == 'HTTP_429' else '0'))
            self.end_headers()
            self.wfile.write(b'data: {"choices":[{"delta":{"content":"ERROR_BODY_MUST_NOT_LEAK"},"finish_reason":"stop"}]}\n\n')
            return

        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        if case == 'FINAL_ONLY':
            payload = {'type': 'response.completed', 'response': {
                'status': 'completed', 'output': [{'type': 'message', 'role': 'assistant',
                    'content': [{'type': 'output_text', 'text': 'FINAL_ONLY_DONE'}]}]}}
        else:
            text = case + '_DONE'
            calls = None
            finish = 'stop'
            if case == 'QUOTED_PROGRESS':
                text = '“正在构建”表示编译过程尚未完成；working on it 是进度表达。QUOTED_PROGRESS_DONE'
            elif case == 'EMPTY_ALWAYS' or (case == 'EMPTY_ANSWER' and attempt == 1):
                text = ' \t\n'
            elif case == 'EMPTY_CALLS' and attempt <= 2:
                text, calls, finish = '', [], 'tool_calls'
                if attempt == 2:
                    calls = [{'index': 0, 'id': prompt + '-read', 'type': 'function',
                              'function': {'name': 'read', 'arguments': json.dumps({
                                  'path': 'fixture.txt', 'purpose': '检查恢复后的工具执行'})}}]
            elif case == 'EMPTY_CALLS':
                assert 'Validation diagnostic:' not in instructions
                assert 'TOOL_FILE_CONTENT' in json.dumps(messages)
            delta = {'content': text} if calls is None else {'tool_calls': calls}
            payload = {'choices': [{'delta': delta, 'finish_reason': finish}],
                       'usage': {'prompt_tokens': 20, 'completion_tokens': 4}}
        self.wfile.write(('data: ' + json.dumps(payload, ensure_ascii=False) + '\n\n').encode())
        self.wfile.flush()


class RecoveryRegression(Regression):
    def __init__(self, args):
        super().__init__(args)
        self.server.RequestHandlerClass = RecoveryFixture
        self.server.attempts = {}
        self.server.fixture_errors = []

    def cli_case(self, case, requests, outcome):
        session = self.work / (case + '.jsonl')
        env = dict(self.env, ZED_SESSION_PATH=str(session))
        if case == 'FINAL_ONLY':
            env['ZED_MODEL'] = 'muse-spark-1.2-contributor'
        started = time.monotonic()
        result = subprocess.run([str(self.args.binary.resolve())], input=case + '\n/exit\n',
                                env=env, cwd=self.work, text=True, capture_output=True, timeout=12)
        elapsed = time.monotonic() - started
        records = [json.loads(line) for line in session.read_text().splitlines()]
        end = next(r for r in reversed(records) if r.get('type') == 'turn_end')
        assistant = [r for r in records if r.get('role') == 'assistant']
        data = {'case': case, 'http_requests': self.server.attempts.get(case, 0),
                'outcome': end['outcome'], 'elapsed_seconds': elapsed,
                'stdout': result.stdout, 'stderr': result.stderr, 'session': records,
                'fixture_errors': self.server.fixture_errors}
        (self.out / (case.lower() + '.json')).write_text(json.dumps(data, ensure_ascii=False, indent=2))
        assert result.returncode == 0
        assert data['http_requests'] == requests, data
        assert end['outcome'] == outcome, data
        assert not self.server.fixture_errors, self.server.fixture_errors
        assert 'ERROR_BODY_MUST_NOT_LEAK' not in json.dumps(records)
        assert 'ERROR_BODY_MUST_NOT_LEAK' not in result.stdout
        assert 'Validation diagnostic:' not in json.dumps(records)
        if outcome == 'completed':
            assert case + '_DONE' in assistant[-1]['content']
            assert case + '_DONE' in result.stdout
        else:
            assert not assistant, data
        if case == 'HTTP_429':
            assert elapsed >= 0.9, 'Retry-After was ignored'
        self.passed(f'CLI {case}: {requests} request(s), {outcome}')

    def run(self):
        for case, requests, outcome in [
            ('EMPTY_CALLS', 3, 'completed'), ('EMPTY_ANSWER', 2, 'completed'),
            ('EMPTY_ALWAYS', 3, 'failed'), ('QUOTED_PROGRESS', 1, 'completed'),
            ('FINAL_ONLY', 1, 'completed'), ('HTTP_503', 2, 'completed'),
            ('HTTP_429', 2, 'completed'), ('HTTP_401', 1, 'failed'),
            ('HTTP_EXHAUST', 3, 'failed'),
        ]:
            self.cli_case(case, requests, outcome)

        self.start()
        self.submit('TUI_EMPTY_CALLS')
        self.wait('EMPTY_CALLS_DONE')
        self.shot('01-empty-tool-call-recovered')
        assert 'retrying (1/2)' in self.screen()
        self.passed('TUI empty tool call is corrected and real tool executes')

        self.submit('TUI_QUOTED_PROGRESS')
        self.wait('QUOTED_PROGRESS_DONE')
        self.shot('02-explanation-not-rejected')
        assert self.server.attempts['TUI_QUOTED_PROGRESS'] == 1
        self.passed('TUI quoted progress explanation is accepted')

        self.submit('TUI_HTTP_429')
        self.wait('retrying (1/2)')
        self.wait('HTTP 429')
        self.shot('03-http-backoff')
        self.wait('HTTP_429_DONE')
        self.shot('04-http-recovered')
        assert self.server.attempts['TUI_HTTP_429'] == 2
        self.passed('TUI HTTP retry is visible and recovery completes')

        self.submit('TUI_HTTP_CANCEL')
        # The new rate-limit event has a five-second wait, distinct from the
        # previously completed one-second retry above.
        deadline = time.monotonic() + 3
        while self.server.attempts.get('TUI_HTTP_CANCEL', 0) != 1 and time.monotonic() < deadline:
            time.sleep(0.05)
        assert self.server.attempts.get('TUI_HTTP_CANCEL') == 1
        time.sleep(0.15)
        start = time.monotonic()
        self.key('Escape')
        self.wait('idle')
        self.submit('AFTER_CANCEL')
        self.wait('AFTER_CANCEL_DONE')
        assert time.monotonic() - start < 2
        assert self.server.attempts['TUI_HTTP_CANCEL'] == 1
        self.shot('05-cancel-backoff')
        self.passed('TUI cancellation interrupts HTTP backoff without another attempt')
        self.submit('/exit')
        time.sleep(0.2)
        assert self.tmux('display-message', '-p', '-t', 'regression', '#{pane_dead_status}').stdout.strip() == '0'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/zeda'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--chrome', default=shutil.which('google-chrome') or shutil.which('chromium'))
    args = parser.parse_args()
    if not shutil.which('tmux') or not args.binary.is_file():
        parser.error('tmux and a built zeda binary are required')
    run = RecoveryRegression(args)
    error = None
    try:
        run.run()
    except Exception as exc:
        error = exc
        print(str(exc), flush=True)
    finally:
        run.finish(error)
    print(f'Artifacts: {run.out}', flush=True)
    return 1 if error else 0


if __name__ == '__main__':
    raise SystemExit(main())
