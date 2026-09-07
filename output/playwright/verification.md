# DeepWiki UI verification

Changed: plugins/deepwiki/web/index.html, style.css, app.js.

Browser: Playwright Chromium. Desktop: 1440×1000. Mobile: 390×844.
The preview server serves the actual frontend files and existing bundled vendor libraries.
API responses use deterministic Chinese documentation and a simulated SSE answer.
Source preview reads the first 55 lines of the local agent_loop.cpp file.
No live model calls or backend generation were tested.

Passed:
- node --check plugins/deepwiki/web/app.js
- git diff --check -- plugins/deepwiki/web
- Page filtering hides nonmatching entries.
- Switching page after scrolling resets the reading pane to its beginning.
- Page title and active navigation match the selected page.
- Source citation opens a highlighted source dialog and closes correctly.
- Glossary and horizontally scrollable tables render.
- Simulated streamed answer displays citations and re-enables submission.
- Mobile navigation opens, selects a page, and closes.
- Mobile document and reading pane do not overflow the viewport horizontally.
- Empty TOC displays an empty state.
- Simulated HTTP 503 displays a visible loading error with status context.
- Normal browser flows reported zero console errors/warnings before deliberate HTTP 503 injection.

Screenshots visually inspected: before, desktop, mobile, mobile navigation,
glossary, source preview, answer.
Resources copied into build/plugins/deepwiki/resources for the existing build.
