# Industrial detail refinement

Image reference generated with the built-in image generation tool from the previous accepted screenshot.
Reference: ../imagegen/deepwiki-industrial-reference.png
Exact prompt: ../imagegen/deepwiki-industrial-prompt.txt

Applied details: existing gray/black palette and two-column layout retained; larger sidebar typography retained; navigation separators; rectangular keyboard shortcut; stronger toolbar controls; monochrome Mermaid flowcharts with stepped edges; rectangular source citation tags; table column rules; visible input focus.

Verification: actual web assets served locally with deterministic sample Chinese documents and simulated SSE answers. No live model/backend generation calls.

Passed: Node syntax check; git diff --check; page filtering; Cmd+K focus; glossary; source preview and close; simulated streamed answer and close. Chromium console: 0 errors, 0 warnings. At 1024px viewport the document width was 1024px. Computed sidebar title/description sizes: 16px/13px.

Visually inspected screenshots: deepwiki-industrial-desktop.png (1440x1000), deepwiki-industrial-compact.png (1024x768), deepwiki-industrial-source.png.

Final frontend assets synchronized into build/plugins/deepwiki/resources. No installation or C++ changes.
