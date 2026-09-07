# Reading interaction verification

Actual frontend assets served by preview_server.py with deterministic Chinese wiki pages and simulated SSE. No live model calls.

Passed in Chromium:
- Page buttons expand/collapse headings; opening another page replaces the active chapter list.
- Clicking chapters and scrolling update aria-current and the toolbar chapter label, including the final chapter.
- Glossary clears the chapter navigation; title/description filtering still works.
- Selected paragraph text is automatically written to the real clipboard after granting browser clipboard permissions; copied contents matched the selection.
- Context menu appends selected text or paragraph with source to the existing draft without submitting.
- Source dialog context menu is interactive and inserts source location into draft, then closes the dialog.
- Draft height grows for inserted references, bounded at 180px.
- Shift+Enter inserts a newline; Enter submits the simulated question.
- Escape and wheel dismiss the context menu. Wheel assertion waits for event delivery.
- Clipboard rejection was simulated and produced the manual Ctrl/Cmd+C message.
- Computed ask-form padding/gap and search container padding are all 0px.
- JavaScript syntax and git diff whitespace checks passed. Console: zero errors/warnings.

Screenshots visually inspected: deepwiki-reading.png, deepwiki-reading-menu.png, deepwiki-reading-draft.png.
Layout also exercised at 1024x768; primary screenshot is 1440x1000.
Assets synchronized to build/plugins/deepwiki/resources.
