#!/usr/bin/env python3
"""Real TUI + worker-host regression for missing-purpose correction.

Reuses the isolated PTY/screenshot runner. Both model protocols are served by
a local fixture; the actual subagent host and read tool still execute.
"""

import argparse
from http.server import BaseHTTPRequestHandler
import json
from pathlib import Path
import shutil
import time

from tui_pty_regression import Regression


class RetryFixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        try:
            self.respond()
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self.server.fixture_errors.append(str(error))
            self.send_error(500, 'Retry fixture failed')

    def event(self, body):
        self.wfile.write(('data: ' + json.dumps(body, ensure_ascii=False) + '\n\n').encode())
        self.wfile.flush()

    def respond(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(body)
        worker = self.path == '/responses'
        assert self.path in ('/responses', '/chat/completions'), self.path
        messages = body['input'] if worker else body['messages']
        prompt = next(m['content'] for m in reversed(messages) if m.get('role') == 'user')
        if not isinstance(prompt, str):
            prompt = '\n'.join(part.get('text', '') for part in prompt)
        scenario = 'exhaust' if prompt == 'EXHAUST_PURPOSE_TEST' else ('worker' if worker else 'main')
        self.server.attempts[scenario] += 1
        attempt = self.server.attempts[scenario]
        correction = body.get('instructions', '') if worker else messages[0]['content']
        if attempt == 2 or (scenario == 'exhaust' and attempt == 3):
            assert 'Validation diagnostic:' in correction and 'purpose' in correction
            assert 'INVALID_ATTEMPT_PREFIX' not in json.dumps(messages)
        if scenario != 'exhaust' and attempt == 3:
            assert 'Validation diagnostic:' not in correction
            expected = 'TOOL_FILE_CONTENT' if worker else 'WORKER_DONE'
            assert expected in json.dumps(messages), f'Missing actual tool result: {expected}'

        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        time.sleep(0.2 if worker else 0.35)
        if attempt == 1 and not worker:
            self.event({'choices': [{'delta': {'content': 'INVALID_ATTEMPT_PREFIX'},
                                     'finish_reason': None}]})
            self.event({'choices': [{'delta': {'content': ' INVALID_PENDING_TAIL'},
                                     'finish_reason': None}]})
        call = None
        if attempt < 3 or scenario == 'exhaust':
            arguments = {'path': 'fixture.txt'} if worker else {
                'agent': 'explorer', 'task': 'WORKER_PURPOSE_TEST'}
            if attempt == 2 and scenario != 'exhaust':
                arguments['purpose'] = '读取测试文件' if worker else '委派代码调查'
            call = {'id': f'{scenario}-{attempt}', 'type': 'function',
                    'function': {'name': 'read' if worker else 'subagent',
                                 'arguments': json.dumps(arguments, ensure_ascii=False)}}

        if worker:
            output = []
            if call:
                output.append({'type': 'function_call', 'call_id': call['id'], **call['function']})
            else:
                self.event({'type': 'response.output_text.delta', 'delta': 'WORKER_DONE'})
            self.event({'type': 'response.completed', 'response': {
                'status': 'completed', 'output': output,
                'usage': {'input_tokens': 40, 'output_tokens': 4}}})
        else:
            delta = {'tool_calls': [{'index': 0, **call}]} if call else {
                'content': '# 自动补正已完成\n\n主 Agent 与子代理均补全了目的，实际文件读取成功。\n\nSUBAGENT_RECOVERED'}
            self.event({'choices': [{'delta': delta, 'finish_reason': 'tool_calls' if call else 'stop'}],
                        'usage': {'prompt_tokens': 100, 'completion_tokens': 10}})
            self.wfile.write(b'data: [DONE]\n\n')
            self.wfile.flush()


class RetryRegression(Regression):
    def __init__(self, args):
        super().__init__(args)
        self.server.RequestHandlerClass = RetryFixture
        self.server.attempts = {'main': 0, 'worker': 0, 'exhaust': 0}
        self.server.fixture_errors = []

    def run(self):
        self.start()
        self.submit('SUBAGENT_PURPOSE_TEST')
        self.wait('retrying (1/2)')
        self.shot('01-retrying-missing-purpose')
        self.passed('missing subagent purpose produces visible automatic retry')
        self.wait('SUBAGENT_RECOVERED', timeout=15)
        self.wait('idle')
        assert self.server.attempts['main'] == 3
        assert self.server.attempts['worker'] == 3
        assert not self.server.fixture_errors, self.server.fixture_errors
        assert 'INVALID_ATTEMPT_PREFIX' not in self.screen()
        assert 'INVALID_PENDING_TAIL' not in self.screen()
        self.shot('02-subagent-recovered')
        self.passed('main and real worker host both correct missing purpose and finish')
        self.passed('invalid streamed text is cleared from TUI and correction leaves next prompt')
        assert '↑420' in self.screen() and '↓42' in self.screen()
        self.passed('retry usage included for both main model and worker')

        self.submit('EXHAUST_PURPOSE_TEST')
        self.wait('after 2 correction retries', timeout=12)
        assert self.server.attempts['exhaust'] == 3
        assert self.server.attempts['worker'] == 3
        assert not self.server.fixture_errors, self.server.fixture_errors
        self.shot('03-retry-limit')
        self.passed('three invalid attempts stop with two-retry limit and no worker execution')
        assert '↑720' in self.screen() and '↓72' in self.screen()
        self.passed('last failed attempt is also included in usage')

        self.submit('/exit')
        time.sleep(0.2)
        persisted = (self.work / 'session.jsonl').read_text()
        assert 'SUBAGENT_RECOVERED' in persisted
        assert 'INVALID_ATTEMPT_PREFIX' not in persisted
        assert 'INVALID_PENDING_TAIL' not in persisted
        assert 'Validation diagnostic:' not in persisted
        # Only the corrected main call is persisted and linked to its result.
        assert 'main-2' in persisted and 'main-1' not in persisted
        assert 'exhaust-1' not in persisted and 'exhaust-2' not in persisted
        assert 'exhaust-3' not in persisted
        assert self.tmux('display-message', '-p', '-t', 'regression', '#{pane_dead_status}').stdout.strip() == '0'
        self.passed('session contains only valid tool calls/results and clean exit')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/zeda'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--chrome', default=shutil.which('google-chrome') or shutil.which('chromium'))
    args = parser.parse_args()
    if not shutil.which('tmux') or not args.binary.is_file():
        parser.error('tmux and a built zeda binary are required')
    run = RetryRegression(args)
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
