# TUI PTY regression

Run from the project root after building `zeda`:

```bash
python3 scripts/tui_pty_regression.py \
  --binary build/zeda \
  --output artifacts/tui-regression-$(date +%Y%m%d-%H%M%S)
```

This is an opt-in integration check requiring Linux/POSIX, Python 3 and `tmux`.
For PNGs, install Google Chrome or Chromium, or supply `--chrome /path/to/chrome`.
DejaVu Sans Mono and Noto Sans Mono CJK SC provide the screenshot fonts.
It is separate from default CTest because it needs these external UI tools.

The runner starts the actual binary in a private tmux PTY, drives keys and SGR
mouse events, and checks the resulting terminal screen. A loopback HTTP/SSE
fixture supplies deterministic model output and one real workspace file-read
tool call. It uses a temporary workspace/session, a dummy credential and an
explicit loopback endpoint. The tmux configuration and pane environment are
isolated; user credentials and sessions are not needed. It does not invoke
real model APIs or change application configuration outside its temporary workspace.
Normal completion and caught exceptions clean up the private tmux server,
HTTP fixture and temporary workspace. Force-killing the runner can leave temporary
resources behind.

The output directory must be new. It contains `results.json`, an `index.html`
gallery, and `.txt`, `.ansi`, `.html` and optional `.png` screen artifacts.
Exit status is 0 when all checks pass without findings, and 1 on a failed check
or a detected visual defect. Screen capture continues after the known narrow
model-menu defect so the remaining interactions can still be checked.

PNG images are rendered from the actual PTY cell buffer with captured colors
and cursor position. Character columns are fixed so font fallback does not
change layout. This validates application layout; it is not a desktop-terminal
pixel screenshot and does not validate desktop IME composition, system clipboard,
emoji/grapheme rasterization, or every terminal emulator. Chinese coverage sends
UTF-8 input and edits it with backspace. Session recovery covers a clean restart,
not process-crash recovery. The runner currently tests the light theme and the
Chat Completions fixture protocol.

## 2026-09-07 result

Tested `/home/zdong/dev/omz_agent/build/zeda`, version `zeda 0.2`.
SHA-256: `1e72097b44f647916ca4159e4db62eab9050d9279ee56ffaa1273d523d8cd622`.
The installed `/home/zdong/.local/bin/zeda` had the same hash. This source tree
has no `.git` metadata; it is newer than the separate `web_pi/omz_agent` checkout.

Existing CTest suite: **20/20 passed**. PTY interaction checks: **12/12 passed**.
Visual inspection found **one open issue**, so the overall UI result needs attention.

Covered interactions:

1. Startup, composer and status footer at 120×38.
2. Model-menu arrow navigation and Tab completion.
3. Chinese/ASCII backspace editing, Markdown, tables and code blocks.
4. Input-history navigation with unsent Chinese draft restoration.
5. Ninety-line output, mouse-wheel scrollback and returning to the bottom.
6. Live resize through 120×38, 60×20 and 160×45.
7. Output visible before stream completion; Esc cancellation and next request.
8. Actual file-read tool execution and tool-card expand/collapse.
9. Ctrl-C during streaming cancels the request without closing the app.
10. HTTP 400 is visible and the next request succeeds.
11. `/exit` returns 0; restarting restores completed conversation messages.
12. Ctrl-C while idle exits with status 0.

Both cancellation checks also require the fixture to observe a disconnected
stream. The final Session must not contain either cancelled partial assistant
output or its never-completed final marker.

### TUI-001 — model details clipped at 60 columns

Reproduction: start the TUI, type `/model`, and resize to 60×20.
The right-hand context line shows `Context 1050000 · Max output`, but its
`128000` output value is clipped. The price-unit description is also truncated.
Model selection and the input area remain usable. At 120 columns, the full
details are visible.

The model guide uses a horizontal layout with up to 32 columns for model names
and unwrapped detail text (`src/ui/terminal.cpp`, `render_terminal_command_guide`).
A future narrow-width layout should wrap or stack the details while retaining
model selection. This regression task records the issue; production layout is
unchanged.

Evidence: [narrow screenshot](../artifacts/tui-regression-2026-09-07/final/02b-model-menu-narrow.png),
[normal-width screenshot](../artifacts/tui-regression-2026-09-07/final/02-model-menu.png),
[full gallery and machine-readable checks](../artifacts/tui-regression-2026-09-07/final/index.html).

## Tool-call correction regression

Run the missing-purpose scenario separately with the same PTY prerequisites:

```bash
python3 scripts/subagent_retry_regression.py \
  --binary build/zeda \
  --output artifacts/tui-regression-purpose-$(date +%Y%m%d-%H%M%S)
```

The fixture first omits `purpose` from the main model's `subagent` call. After
correction, it starts the actual worker host, whose first `read` call also omits
`purpose`. The worker corrects its call and reads a real temporary file. The
main model then receives the worker's result and completes the request.
The main model uses Chat Completions; the worker uses Responses. Both services
are simulated locally, with no external API calls.

The runner checks visible retry feedback, removal of rejected streamed text,
corrected calls/results in Session, temporary feedback removal from subsequent
requests, and token accounting including both models' rejected responses.
A second request repeatedly omits `purpose` and must stop after two correction
retries without starting another worker.

On 2026-09-07, the old installed binary reproduced the immediate failure.
The updated build and installed binary both passed **7/7** targeted checks,
using nine simulated model requests. The full CTest suite passed **20/20**.
Installed SHA-256:
`08211f6857adfea8eb7e08d2cc45dadcb6ae814bb3b64e88987d5aeebdd3d29f`.

Evidence: [old failure](../artifacts/tui-regression-purpose-retry/before/failure.png),
[installed retry/success/limit gallery](../artifacts/tui-regression-purpose-retry/installed/index.html).

## Response and HTTP recovery regression

```bash
python3 scripts/recovery_regression.py \
  --binary build/zeda \
  --output artifacts/tui-regression-recovery-$(date +%Y%m%d-%H%M%S)
```

This runner covers the four follow-up review findings with local fixtures and
the actual CLI/TUI binary:

- Empty tool-call responses receive correction, then execute the valid read tool.
- Empty/whitespace answers receive correction; persistent empty answers fail
  after the shared two-retry limit without an empty completed Session message.
- Responses final-only text is displayed and persisted without requiring deltas.
- Explanations containing “正在构建” or “working on it” complete on the first try.
- 503 and 429 recover, Retry-After is respected, 401 is not retried, and repeated
  transient errors stop after three total HTTP attempts.
- TUI retry notices remain separate from generated text. Esc interrupts a
  five-second server backoff and the next request succeeds without an extra
  cancelled request being sent.

On 2026-09-07, the updated installed binary passed **13/13** CLI/PTY checks.
The existing actual-worker purpose-recovery runner also passed **7/7**, and
the complete CTest suite passed **20/20**. Provider smoke tests additionally
cover final-text de-duplication, HTTP-date/oversized Retry-After, timeout budgets,
cancellation on a zero-delay retry, permanent errors and error-body isolation.
The plugin regression checks that transport notices do not enter DeepWiki text.

Installed SHA-256:
`59c0c3dfca870d6fc6152c317ed000239c74db4c8b478f08791f66441d3b2449`.

Evidence: [installed recovery gallery and CLI records](../artifacts/tui-regression-recovery-fixes/installed/index.html),
[subagent compatibility checks](../artifacts/tui-regression-recovery-fixes/subagent-compatibility/index.html).
These remain local simulated-service tests, not measurements of a live provider.
