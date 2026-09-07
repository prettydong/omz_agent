#!/usr/bin/env python3
"""Opt-in real-PTY regression. Requires tmux; Chrome is optional for PNGs.

Uses only Python's standard library and a loopback SSE fixture. No real API keys,
user sessions, or model services are used. Screenshots render tmux's actual cell
buffer (including ANSI colors); they are not screenshots of a desktop emulator.
"""

import argparse
import hashlib
import html
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import threading
import time
import unicodedata
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


RICH = """# 中文与 Markdown 回归

这是 **加粗文字**、`inline_code` 和 English 混排。

| 项目 | 状态 | 数量 |
| :--- | :---: | ---: |
| 中文输入 | 通过 | 12 |
| stream | OK | 345 |

```cpp
int main() {
  // 中文注释
  return 0;
}
```

> 引用：保留中文、标点与空行。

- 第一项：输入与输出
- 第二项：表格与代码块

RICH_DONE
"""


class Fixture(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.server.requests.append(body)
        if self.path != '/chat/completions':
            self.send_error(404)
            return
        messages = body['messages']
        prompt = next(m['content'] for m in reversed(messages) if m['role'] == 'user')
        if not isinstance(prompt, str):
            prompt = '\n'.join(p.get('text', '') for p in prompt)
        if prompt == 'ERROR_TEST':
            self.send_response(400)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(b'{"error":{"message":"TUI_FIXTURE_ERROR"}}')
            return
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()

        def event(delta, finish=None):
            payload = {'choices': [{'index': 0, 'delta': delta, 'finish_reason': finish}]}
            self.wfile.write(('data: ' + json.dumps(payload, ensure_ascii=False) + '\n\n').encode())
            self.wfile.flush()

        try:
            if prompt == 'TOOL_TEST' and messages[-1]['role'] != 'tool':
                event({'tool_calls': [{'index': 0, 'id': 'tui-read-1', 'type': 'function',
                       'function': {'name': 'read', 'arguments': json.dumps({
                           'path': 'fixture.txt', 'purpose': '读取回归测试文件'})}}]}, 'tool_calls')
            else:
                if prompt == 'TOOL_TEST':
                    text = '工具读取完成。TOOL_DONE'
                elif prompt == 'LONG_TEST':
                    text = '# 长输出\n\n' + '\n'.join(
                        f'- LINE_{i:03d} 中文滚动检查 English {i}' for i in range(1, 91)) + '\n\nLONG_DONE'
                elif prompt == 'SLOW_TEST':
                    text = 'STREAM_STARTED\n\n' + '流式输出进行中。\n' * 200 + '\nSLOW_DONE'
                elif prompt.startswith('RECOVERY_TEST_'):
                    text = '取消与错误后可继续输入。RECOVERY_DONE_' + prompt.rsplit('_', 1)[1]
                else:
                    text = RICH
                for offset in range(0, len(text), 24):
                    event({'content': text[offset:offset + 24]})
                    time.sleep(0.25 if prompt == 'SLOW_TEST' else 0.015)
                event({}, 'stop')
            self.wfile.write(b'data: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            self.server.disconnects += 1


PALETTE = ['#000000', '#800000', '#008000', '#808000', '#000080', '#800080',
           '#008080', '#c0c0c0', '#808080', '#ff0000', '#00ff00', '#ffff00',
           '#0000ff', '#ff00ff', '#00ffff', '#ffffff']


def color256(n):
    if n < 16:
        return PALETTE[n]
    if n >= 232:
        return '#{0:02x}{0:02x}{0:02x}'.format(8 + (n - 232) * 10)
    n -= 16
    levels = [0, 95, 135, 175, 215, 255]
    return '#{:02x}{:02x}{:02x}'.format(levels[n // 36], levels[n // 6 % 6], levels[n % 6])


def ansi_html(text):
    """Render SGR colors from tmux capture-pane, retaining Unicode and spacing."""
    styles = {}
    parts = []
    for part in re.split(r'(\x1b\[[0-9;]*m)', text):
        if not part.startswith('\x1b['):
            style = ';'.join(f'{k}:{v}' for k, v in styles.items())
            for char in part:
                if char == '\n':
                    parts.append('\n')
                else:
                    width = 2 if unicodedata.east_asian_width(char) in ('W', 'F') else 1
                    if unicodedata.combining(char):
                        width = 0
                    parts.append(f'<span class="cell" style="width:{width * 10}px;{style}">{html.escape(char)}</span>')
            continue
        codes = [int(x or 0) for x in part[2:-1].split(';')]
        i = 0
        while i < len(codes):
            c = codes[i]
            if c == 0:
                styles.clear()
            elif c in (1, 2):
                styles['font-weight' if c == 1 else 'opacity'] = 'bold' if c == 1 else '0.7'
            elif c == 22:
                styles.pop('font-weight', None)
                styles.pop('opacity', None)
            elif c == 3:
                styles['font-style'] = 'italic'
            elif c == 23:
                styles.pop('font-style', None)
            elif c == 4:
                styles['text-decoration'] = 'underline'
            elif c == 24:
                styles.pop('text-decoration', None)
            elif c in (39, 49):
                styles.pop('color' if c == 39 else 'background-color', None)
            elif 30 <= c <= 37 or 90 <= c <= 97:
                styles['color'] = PALETTE[c - 30 if c < 90 else c - 90 + 8]
            elif 40 <= c <= 47 or 100 <= c <= 107:
                styles['background-color'] = PALETTE[c - 40 if c < 100 else c - 100 + 8]
            elif c in (38, 48) and i + 2 < len(codes):
                key = 'color' if c == 38 else 'background-color'
                if codes[i + 1] == 5:
                    styles[key] = color256(codes[i + 2])
                    i += 2
                elif codes[i + 1] == 2 and i + 4 < len(codes):
                    styles[key] = '#{:02x}{:02x}{:02x}'.format(*codes[i + 2:i + 5])
                    i += 4
            i += 1
    return ''.join(parts)


class Regression:
    def __init__(self, args):
        self.args = args
        self.out = args.output.resolve()
        self.out.mkdir(parents=True, exist_ok=False)
        self.temp = tempfile.TemporaryDirectory(prefix='omz-tui-')
        self.work = Path(self.temp.name)
        (self.work / 'fixture.txt').write_text('TOOL_FILE_CONTENT 中文文件\n')
        self.socket = str(self.work / 'tmux.sock')
        self.cols, self.rows = 120, 38
        self.checks = []
        self.findings = []
        self.shots = []
        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
        self.server.requests = []
        self.server.disconnects = 0
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'TERM': 'xterm-256color',
                    'OPENCODE_GO_API_KEY': 'tui-fixture-key',
                    'ZED_OPENCODE_ENDPOINT': f'http://127.0.0.1:{self.server.server_port}',
                    'ZED_MODEL': 'glm-5.1', 'ZED_CONTEXT_MODEL': 'glm-5.1',
                    'ZED_WORKSPACE': str(self.work),
                    'ZED_SESSION_PATH': str(self.work / 'session.jsonl'),
                    'ZED_OPENCODE_PATH': '/nonexistent-tui-fixture',
                    'ZED_THEME': 'light'}

    def tmux(self, *args, check=True):
        return subprocess.run(['tmux', '-f', '/dev/null', '-S', self.socket, *args],
                              text=True, capture_output=True, check=check)

    def start(self):
        command = shlex.join(['env', '-i', *(f'{k}={v}' for k, v in self.env.items()),
                              str(self.args.binary.resolve())])
        self.tmux('new-session', '-d', '-s', 'regression', '-x', str(self.cols),
                  '-y', str(self.rows), '-c', str(self.work), command)
        self.tmux('set-option', '-g', 'remain-on-exit', 'on')
        self.wait('Ask zeda')

    def screen(self, ansi=False):
        return self.tmux('capture-pane', '-p', *(['-e'] if ansi else []), '-t', 'regression').stdout

    def composer(self):
        return self.screen().splitlines()[-3]

    def wait(self, needle, timeout=8):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            current = self.screen()
            if needle in current:
                return current
            time.sleep(0.05)
        raise AssertionError(f'Timed out waiting for {needle!r}; screen:\n{self.screen()}')

    def wait_disconnect(self, previous):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if self.server.disconnects > previous:
                return
            time.sleep(0.05)
        raise AssertionError('Cancelled stream did not disconnect from fixture')

    def key(self, *keys):
        self.tmux('send-keys', '-t', 'regression', *keys)
        time.sleep(0.1)

    def type(self, text):
        self.tmux('send-keys', '-t', 'regression', '-l', '--', text)
        time.sleep(0.1)

    def clear(self):
        self.key('End')
        self.tmux('send-keys', '-t', 'regression', '-N', '250', 'BSpace')
        time.sleep(0.1)

    def submit(self, text):
        self.type(text)
        self.key('Enter')

    def passed(self, name):
        self.checks.append({'name': name, 'status': 'passed'})
        print('PASS ' + name, flush=True)

    def shot(self, name):
        time.sleep(0.15)
        ansi = self.screen(True)
        plain = self.screen()
        assert '\ufffd' not in plain, 'Replacement character in terminal screen'
        assert 'tui-fixture-key' not in plain, 'Credential exposed in terminal'
        (self.out / f'{name}.ansi').write_text(ansi)
        (self.out / f'{name}.txt').write_text(plain)
        cursor = self.tmux('display-message', '-p', '-t', 'regression',
                           '#{cursor_flag} #{cursor_x} #{cursor_y}').stdout.strip().split()
        page = '''<!doctype html><meta charset="utf-8"><style>
body {margin:0;background:#fafafa;color:#222;}
pre {margin:0;padding:16px;font-family:"DejaVu Sans Mono","Noto Sans Mono CJK SC",monospace;
font-size:16px;line-height:24px;white-space:pre;font-variant-ligatures:none;}
.cell {display:inline-block;height:24px;vertical-align:top;white-space:pre;}
</style><pre>''' + ansi_html(ansi) + '</pre>'
        if len(cursor) == 3 and cursor[0] == '1':
            page += f'<div style="position:absolute;left:{16 + int(cursor[1]) * 10}px;top:{16 + int(cursor[2]) * 24}px;width:10px;height:24px;background:#5555"></div>'
        source = self.out / f'{name}.html'
        source.write_text(page)
        self.shots.append(name)
        if self.args.chrome:
            result = subprocess.run([self.args.chrome, '--headless', '--no-sandbox',
                '--disable-gpu', '--hide-scrollbars', '--no-first-run',
                '--no-default-browser-check', '--disable-background-networking',
                '--force-device-scale-factor=1', '--allow-file-access-from-files',
                f'--user-data-dir={self.work / "chrome"}',
                f'--window-size={self.cols * 10 + 32},{self.rows * 24 + 32}',
                f'--screenshot={self.out / (name + ".png")}', source.as_uri()],
                capture_output=True, text=True, timeout=25)
            if result.returncode != 0 or not (self.out / (name + '.png')).exists():
                raise RuntimeError('Chrome screenshot failed: ' + result.stderr[-1000:])

    def resize(self, cols, rows):
        self.cols, self.rows = cols, rows
        self.tmux('resize-window', '-t', 'regression', '-x', str(cols), '-y', str(rows))
        time.sleep(0.3)

    def wheel(self, up, count):
        # SGR mouse event reaches the real application's PTY input.
        self.type(f'\x1b[<{64 if up else 65};25;10M' * count)
        time.sleep(0.2)

    def click_text(self, needle):
        row = next(i for i, line in enumerate(self.screen().splitlines()) if needle in line)
        self.type(f'\x1b[<0;5;{row + 1}M\x1b[<0;5;{row + 1}m')

    def run(self):
        self.start()
        assert 'idle' in self.screen().splitlines()[-1]
        self.shot('01-startup')
        self.passed('startup and fixed composer/footer at 120x38')

        self.type('/model')
        self.wait('GPT-5.6 Luna')
        context_details = re.search(r'Context (\d+).*Max output (\d+)', self.screen())
        assert context_details, 'Full-width model context details are missing'
        self.shot('02-model-menu')
        self.resize(60, 20)
        self.shot('02b-model-menu-narrow')
        assert '/model' in self.composer()
        if any(value not in self.screen() for value in context_details.groups()):
            self.findings.append({
                'id': 'TUI-001', 'status': 'open', 'severity': 'medium',
                'summary': '60-column model menu clips context/output details',
                'expected_values': list(context_details.groups()),
                'evidence': '02b-model-menu-narrow.png',
                'source': 'src/ui/terminal.cpp:685',
                'reproduce': 'Type /model, then resize terminal to 60 columns and 20 rows',
            })
            print('FINDING TUI-001: narrow model details are clipped', flush=True)
        self.resize(120, 38)
        self.key('Down', 'Down', 'Tab')
        assert '/model muse-spark-1.2-contributor' in self.composer()
        self.clear()
        self.passed('model menu arrow navigation and Tab completion')

        self.type('中文输入测试ABC')
        self.key('BSpace', 'BSpace', 'BSpace', 'BSpace')
        self.type('试完成')
        self.wait('中文输入测试完成')
        assert '中文输入测试完成' in self.composer()
        self.shot('03-chinese-input')
        self.key('Enter')
        self.wait('RICH_DONE')
        self.wait('idle')
        assert '中文输入' in self.screen() and 'return 0;' in self.screen()
        self.shot('04-markdown')
        self.passed('Chinese UTF-8 editing and Markdown/table/code rendering')

        self.type('未发送草稿')
        self.key('Up')
        assert '中文输入测试完成' in self.composer()
        self.key('Down')
        assert '未发送草稿' in self.composer()
        self.clear()
        self.passed('input history restores unsent Chinese draft')

        self.submit('LONG_TEST')
        self.wait('LONG_DONE')
        bottom = self.screen()
        self.shot('05-long-bottom')
        self.wheel(True, 12)
        scrolled = self.screen()
        assert scrolled != bottom and 'LONG_DONE' not in scrolled
        self.shot('06-long-scroll-up')
        self.wheel(False, 40)
        self.wait('LONG_DONE')
        self.passed('90-line output, mouse scroll up, return to bottom')

        self.resize(60, 20)
        self.wait('Ask zeda')
        self.shot('07-narrow-60x20')
        self.resize(160, 45)
        self.wait('LONG_DONE')
        self.shot('08-wide-160x45')
        self.resize(120, 38)
        self.passed('live resize 120x38 -> 60x20 -> 160x45 -> 120x38')

        self.submit('SLOW_TEST')
        self.wait('STREAM_STARTED')
        assert 'SLOW_DONE' not in self.screen()
        self.shot('09-streaming')
        before = time.monotonic()
        disconnects = self.server.disconnects
        self.key('Escape')
        self.wait_disconnect(disconnects)
        self.submit('RECOVERY_TEST_1')
        self.wait('RECOVERY_DONE_1')
        assert time.monotonic() - before < 5
        self.shot('10-cancel-recovery')
        self.passed('incremental output before completion; Escape cancel and next turn')

        self.submit('TOOL_TEST')
        self.wait('TOOL_DONE')
        assert any(any(m.get('role') == 'tool' and 'TOOL_FILE_CONTENT' in str(m)
                       for m in r['messages']) for r in self.server.requests)
        self.shot('11-tool-result')
        self.click_text('click to expand')
        self.wait('TOOL_FILE_CONTENT')
        self.shot('11b-tool-expanded')
        self.click_text('读取回归测试文件')
        assert 'TOOL_FILE_CONTENT' not in self.screen()
        self.passed('real read tool execution and tool card rendering')

        self.submit('SLOW_TEST')
        self.wait('STREAM_STARTED')
        disconnects = self.server.disconnects
        self.key('C-c')
        self.wait_disconnect(disconnects)
        self.submit('RECOVERY_TEST_CTRL_C')
        self.wait('RECOVERY_DONE_C')
        self.passed('Ctrl-C during streaming cancels without exiting')

        self.submit('ERROR_TEST')
        self.wait('HTTP request failed with status 400')
        self.shot('12-provider-error')
        self.submit('RECOVERY_TEST_2')
        self.wait('RECOVERY_DONE_2')
        self.passed('provider error visible and subsequent request succeeds')

        self.submit('/exit')
        time.sleep(0.3)
        assert self.tmux('display-message', '-p', '-t', 'regression', '#{pane_dead_status}').stdout.strip() == '0'
        persisted = (self.work / 'session.jsonl').read_text()
        assert 'RECOVERY_DONE_2' in persisted and 'RICH_DONE' in persisted
        assert 'SLOW_DONE' not in persisted and 'STREAM_STARTED' not in persisted
        self.tmux('kill-session', '-t', 'regression')
        self.start()
        self.wait('RECOVERY_DONE_2')
        self.shot('13-session-restored')
        self.passed('clean exit and persisted conversation restored in real TUI')
        self.key('C-c')
        assert self.tmux('display-message', '-p', '-t', 'regression', '#{pane_dead_status}').stdout.strip() == '0'
        self.passed('Ctrl-C while idle exits cleanly')

    def finish(self, error=None):
        if error:
            self.checks.append({'name': 'regression stopped', 'status': 'failed', 'error': str(error)})
            try:
                self.shot('failure')
            except Exception:
                pass
        result = {'binary': str(self.args.binary.resolve()),
                  'sha256': hashlib.sha256(self.args.binary.read_bytes()).hexdigest(),
                  'checks': self.checks, 'screenshots': self.shots,
                  'status': 'failed' if error else ('needs_attention' if self.findings else 'passed'),
                  'findings': self.findings,
                  'fixture_requests': len(self.server.requests),
                  'fixture_disconnects': self.server.disconnects,
                  'image_method': 'tmux PTY cell buffer + ANSI colors rendered with Chrome',
                  'limitations': ['No real model service', 'No desktop IME or clipboard integration',
                                  'Font rasterization belongs to Chrome, not user terminal emulator']}
        (self.out / 'results.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
        report = '<!doctype html><meta charset="utf-8"><title>omz TUI regression</title>'
        report += '<style>body{font:16px sans-serif;margin:24px}img{max-width:100%;border:1px solid #ccc}pre{white-space:pre-wrap}</style>'
        report += '<h1>omz agent TUI — real PTY regression</h1><pre>' + html.escape(json.dumps(result, ensure_ascii=False, indent=2)) + '</pre>'
        for name in self.shots:
            report += f'<h2>{name}</h2><a href="{name}.txt">Screen text</a> · <a href="{name}.html">Rendered screen</a><br>'
            if (self.out / f'{name}.png').exists():
                report += f'<img src="{name}.png">'
        (self.out / 'index.html').write_text(report)
        self.tmux('kill-server', check=False)
        self.server.shutdown()
        self.server.server_close()
        self.temp.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/zeda'))
    parser.add_argument('--output', type=Path, required=True, help='new artifact directory')
    parser.add_argument('--chrome', default=shutil.which('google-chrome') or shutil.which('chromium'))
    args = parser.parse_args()
    if not shutil.which('tmux') or not args.binary.is_file():
        parser.error('tmux and a built zeda binary are required')
    run = Regression(args)
    error = None
    try:
        run.run()
    except Exception as exc:
        error = exc
        print(str(exc), flush=True)
    finally:
        run.finish(error)
    print(f'Artifacts: {run.out}', flush=True)
    return 1 if error or run.findings else 0


if __name__ == '__main__':
    raise SystemExit(main())
