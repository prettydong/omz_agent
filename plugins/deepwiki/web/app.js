(() => {
  const token = new URLSearchParams(location.search).get("token") || "";
  const withToken = (path) => `${path}${path.includes("?") ? "&" : "?"}token=${encodeURIComponent(token)}`;

  const page = document.querySelector("#page");
  const toc = document.querySelector("#toc");
  const tocFilter = document.querySelector("#toc-filter");
  const tocCount = document.querySelector("#toc-count");
  const answerCard = document.querySelector("#answer-card");
  const answer = document.querySelector("#answer");
  const question = document.querySelector("#question");
  const askForm = document.querySelector("#ask-form");
  const askButton = document.querySelector("#ask-btn");
  const dialog = document.querySelector("#source-dialog");
  const toastEl = document.querySelector("#toast");
  const termsToggle = document.querySelector("#terms-toggle");
  const viewTitle = document.querySelector("#view-title");
  const viewTag = document.querySelector("#view-tag");

  document.querySelector("#filter-shortcut").textContent =
    /Mac|iPhone|iPad/.test(navigator.platform) ? "⌘ K" : "Ctrl K";

  let currentPageMarkdown = "";
  let terms = [];
  const termByProgram = new Map();
  let toastTimer = 0;

  const emptyIcon = '<svg viewBox="0 0 24 24" width="44" height="44" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"/></svg>';

  function toast(message, kind = "error") {
    toastEl.dataset.kind = kind;
    toastEl.textContent = message;
    toastEl.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { toastEl.hidden = true; }, 4200);
  }

  const main = document.querySelector("main");
  const readingMenu = document.querySelector("#reading-menu");
  let loadedPage = null;
  let pageRequest = 0;
  let sections = [];
  let scrollFrame = 0;
  let menuPayload = null;
  let menuFocus = null;
  let lastCopied = "";

  function collapsePages() {
    for (const button of toc.querySelectorAll(".page-button")) {
      button.classList.remove("active");
      button.removeAttribute("aria-current");
      button.setAttribute("aria-expanded", "false");
      button.nextElementSibling.hidden = true;
    }
  }

  function updateReadingSection() {
    if (!sections.length || !loadedPage) return;
    const toolbar = document.querySelector(".editor-head");
    const boundary = Math.max(0, toolbar.getBoundingClientRect().bottom) + 20;
    let active = sections[0];
    for (const section of sections) {
      if (section.heading.getBoundingClientRect().top <= boundary) active = section;
    }
    if (main.scrollHeight > main.clientHeight &&
        main.scrollTop + main.clientHeight >= main.scrollHeight - 2) active = sections.at(-1);
    for (const section of sections) {
      if (section === active) section.link.setAttribute("aria-current", "location");
      else section.link.removeAttribute("aria-current");
    }
    viewTitle.textContent = `${loadedPage.title} / ${active.link.textContent}`;
    viewTitle.title = viewTitle.textContent;
  }

  function scheduleReadingUpdate() {
    if (scrollFrame) return;
    scrollFrame = requestAnimationFrame(() => {
      scrollFrame = 0;
      updateReadingSection();
    });
  }
  main.addEventListener("scroll", scheduleReadingUpdate, { passive: true });
  window.addEventListener("scroll", scheduleReadingUpdate, { passive: true });
  window.addEventListener("resize", scheduleReadingUpdate);

  function buildSections(button) {
    const list = button.nextElementSibling;
    list.replaceChildren();
    sections = [...page.querySelectorAll("h1, h2, h3")].map((heading, index) => {
      heading.id = `wiki-section-${index}`;
      const link = document.createElement("a");
      link.href = `#${heading.id}`;
      link.className = `level-${heading.tagName.slice(1)}`;
      link.textContent = heading.tagName === "H1" ? "概览" : heading.textContent;
      link.addEventListener("click", (event) => {
        event.preventDefault();
        heading.scrollIntoView({ block: "start", behavior: "smooth" });
      });
      list.append(link);
      return { heading, link };
    });
    if (!sections.length) {
      const empty = document.createElement("div");
      empty.className = "toc-section-empty";
      empty.textContent = "此页没有章节标题";
      list.append(empty);
    }
    list.hidden = false;
    button.setAttribute("aria-expanded", "true");
    scheduleReadingUpdate();
  }

  function selectionPayload() {
    const selected = window.getSelection();
    if (!selected || selected.isCollapsed || !selected.toString().trim()) return null;
    const element = (node) => node?.nodeType === Node.ELEMENT_NODE ? node : node?.parentElement;
    const start = element(selected.anchorNode)?.closest("#page, #answer, #source-content");
    const end = element(selected.focusNode)?.closest("#page, #answer, #source-content");
    if (!start || start !== end) return null;
    return { text: selected.toString().trim(), source: contentSource(start), selected: true };
  }

  function contentSource(element) {
    if (element.closest("#source-content")) return document.querySelector("#source-title").textContent;
    if (element.closest("#answer")) return "Wiki 问答";
    return viewTitle.textContent;
  }

  async function copyText(value, automatic = false) {
    try {
      await navigator.clipboard.writeText(value);
      lastCopied = value;
      toast(automatic ? "已复制选中内容" : "已复制", "success");
    } catch (error) {
      toast("无法自动写入剪贴板，请使用 Ctrl/Cmd+C 复制。");
    }
  }

  function autoCopySelection() {
    if (readingMenu.matches(":popover-open")) return;
    const payload = selectionPayload();
    if (!payload) { lastCopied = ""; return; }
    if (payload.text !== lastCopied) void copyText(payload.text, true);
  }
  document.addEventListener("pointerup", (event) => {
    if (event.button === 0 && event.target.closest("#page, #answer, #source-content")) autoCopySelection();
  });
  document.addEventListener("keyup", (event) => {
    if (event.target.closest("input, textarea, [contenteditable='true']")) return;
    if (event.key === "Shift" || (event.shiftKey && event.key.startsWith("Arrow")) ||
        ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "a")) autoCopySelection();
  });

  function closeReadingMenu(restoreFocus = false) {
    if (!readingMenu.matches(":popover-open")) return;
    readingMenu.hidePopover();
    if (restoreFocus && menuFocus?.isConnected) menuFocus.focus({ preventScroll: true });
  }

  document.addEventListener("contextmenu", (event) => {
    if (event.target.closest("input, textarea, [contenteditable='true']")) return;
    const surface = event.target.closest("#page, #answer, #source-content");
    if (!surface) return;
    event.preventDefault();
    const block = event.target.closest("p, pre, li, h1, h2, h3, td, th, .line");
    menuPayload = selectionPayload() || {
      text: (block || surface).textContent.trim(), source: contentSource(surface), selected: false,
    };
    menuFocus = document.activeElement;
    document.querySelector("#menu-caption").textContent = menuPayload.selected ? "选中内容" : "当前段落";
    readingMenu.querySelector('[data-action="copy"]').disabled = !menuPayload.text;
    readingMenu.querySelector('[data-action="chat"]').disabled = !menuPayload.text || question.disabled;
    closeReadingMenu();
    (dialog.open ? dialog : document.body).append(readingMenu);
    readingMenu.showPopover();
    const rect = readingMenu.getBoundingClientRect();
    readingMenu.style.left = `${Math.max(8, Math.min(event.clientX, innerWidth - rect.width - 8))}px`;
    readingMenu.style.top = `${Math.max(8, Math.min(event.clientY, innerHeight - rect.height - 8))}px`;
    readingMenu.querySelector("button:not(:disabled)")?.focus({ preventScroll: true });
  });

  readingMenu.addEventListener("click", (event) => {
    const action = event.target.closest("button")?.dataset.action;
    if (!action || !menuPayload) return;
    const payload = menuPayload;
    closeReadingMenu();
    if (action === "copy") {
      void copyText(payload.text);
    } else if (action === "chat" && !question.disabled) {
      const quoted = `来自 ${payload.source}\n${payload.text.split("\n").map(line => `> ${line}`).join("\n")}`;
      question.value += `${question.value.trim() ? "\n\n" : ""}${quoted}\n\n`;
      resizeQuestion();
      if (dialog.open) dialog.close();
      question.focus();
      question.setSelectionRange(question.value.length, question.value.length);
      question.scrollTop = question.scrollHeight;
      toast("已添加到提问框，补充问题后发送", "success");
    }
  });
  readingMenu.addEventListener("keydown", (event) => {
    const items = [...readingMenu.querySelectorAll("button:not(:disabled)")];
    const index = items.indexOf(document.activeElement);
    if (event.key === "Escape" || event.key === "Tab") {
      if (event.key === "Escape") event.preventDefault();
      closeReadingMenu(true);
    } else if (["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) {
      event.preventDefault();
      const next = event.key === "Home" ? 0 : event.key === "End" ? items.length - 1 :
        (index + (event.key === "ArrowDown" ? 1 : -1) + items.length) % items.length;
      items[next]?.focus();
    }
  });
  document.addEventListener("pointerdown", (event) => {
    if (!event.target.closest("#reading-menu")) closeReadingMenu();
  });
  document.addEventListener("wheel", () => closeReadingMenu(), { passive: true });
  document.addEventListener("touchmove", () => closeReadingMenu(), { passive: true });
  window.addEventListener("resize", () => closeReadingMenu());
  dialog.addEventListener("close", () => closeReadingMenu());

  function resizeQuestion() {
    question.style.height = "58px";
    question.style.height = `${Math.min(180, question.scrollHeight + 2)}px`;
  }
  question.addEventListener("input", resizeQuestion);
  new ResizeObserver(() => {
    main.style.paddingBottom = `${document.querySelector(".ask").getBoundingClientRect().height + 24}px`;
  }).observe(document.querySelector(".ask"));

  question.addEventListener("keydown", (event) => {
    if (event.key === "Enter" && !event.shiftKey && !event.isComposing) {
      event.preventDefault();
      if (!askButton.disabled) askForm.requestSubmit();
    }
  });

  function configureMermaid() {
    mermaid.initialize({
      startOnLoad: false,
      securityLevel: "strict",
      theme: "base",
      themeVariables: {
        primaryColor: "#f8f8f4",
        primaryTextColor: "#1b1b1b",
        primaryBorderColor: "#55554f",
        secondaryColor: "#e6e6df",
        secondaryTextColor: "#1b1b1b",
        secondaryBorderColor: "#55554f",
        tertiaryColor: "#f1f1ed",
        tertiaryTextColor: "#1b1b1b",
        tertiaryBorderColor: "#96968d",
        lineColor: "#44443e",
        textColor: "#1b1b1b",
        edgeLabelBackground: "#f1f1ed",
      },
      flowchart: { curve: "step" },
      fontFamily: "Inter, PingFang SC, system-ui, sans-serif",
    });
  }

  configureMermaid();

  function escapeHtml(value) {
    return String(value)
      .replaceAll("&", "&amp;")
      .replaceAll('"', "&quot;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;");
  }

  const languageAliases = new Map([
    ["cc", "cpp"], ["cxx", "cpp"], ["hpp", "cpp"], ["h", "cpp"],
    ["shell", "bash"], ["sh", "bash"], ["zsh", "bash"],
    ["yml", "yaml"],
  ]);
  const languageNames = new Set(["c", "cpp", "cmake", "json", "bash", "yaml"]);
  const keywords = new Set([
    "alignas", "alignof", "and", "asm", "auto", "break", "case", "catch",
    "class", "concept", "const", "consteval", "constexpr", "constinit",
    "continue", "co_await", "co_return", "co_yield", "decltype", "default",
    "delete", "do", "else", "enum", "explicit", "export", "extern", "for",
    "friend", "goto", "if", "inline", "namespace", "new", "noexcept", "not",
    "operator", "or", "private", "protected", "public", "requires", "return",
    "sizeof", "static", "static_assert", "struct", "switch", "template", "this",
    "thread_local", "throw", "try", "typedef", "typeid", "typename", "union",
    "using", "virtual", "volatile", "while", "xor",
  ]);
  const types = new Set([
    "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int",
    "long", "short", "signed", "unsigned", "void", "wchar_t", "size_t", "string",
  ]);
  const literals = new Set(["true", "false", "null", "nullptr"]);
  const tokenPattern = /\/\/[^\n]*|\/\*[\s\S]*?\*\/|#[^\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\b(?:0[xX][\da-fA-F]+|0[bB][01]+|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)\b|\b[A-Za-z_]\w*\b/g;

  function normalizeLanguage(value) {
    const normalized = String(value || "").toLowerCase().replace(/^language-/, "");
    return languageAliases.get(normalized) || normalized;
  }

  function languageFromPath(value) {
    const path = String(value || "")
      .replace(/^\[/, "")
      .replace(/\]$/, "")
      .replace(/:\d+(?:-\d+)?$/, "");
    const name = path.split("/").pop()?.toLowerCase() || "";
    if (name === "cmakelists.txt" || name.endsWith(".cmake")) return "cmake";
    return normalizeLanguage(name.includes(".") ? name.split(".").pop() : "");
  }

  function tokenClass(token, language, source, end) {
    if (token.startsWith("//") || token.startsWith("/*")) return "tok-comment";
    if (token.startsWith("#")) return language === "bash" || language === "yaml" ? "tok-comment" : "tok-meta";
    if (token.startsWith('"') || token.startsWith("'")) {
      const rest = source.slice(end);
      return (language === "json" && /^\s*:/.test(rest)) ? "tok-property" : "tok-string";
    }
    if (/^(?:0[xX][\da-fA-F]+|0[bB][01]+|\d)/.test(token)) return "tok-number";
    if (literals.has(token)) return "tok-literal";
    if ((language === "yaml" || language === "json") && /^\s*:/.test(source.slice(end))) return "tok-property";
    if (types.has(token)) return "tok-type";
    if (keywords.has(token)) return "tok-keyword";
    if (/^\s*\(/.test(source.slice(end))) return "tok-call";
    return "";
  }

  function highlightText(source, language, target) {
    target.replaceChildren();
    tokenPattern.lastIndex = 0;
    let offset = 0;
    for (let match = tokenPattern.exec(source); match; match = tokenPattern.exec(source)) {
      if (match.index > offset) target.append(document.createTextNode(source.slice(offset, match.index)));
      const className = tokenClass(match[0], language, source, tokenPattern.lastIndex);
      if (className) {
        const span = document.createElement("span");
        span.className = className;
        span.textContent = match[0];
        target.append(span);
      } else {
        target.append(document.createTextNode(match[0]));
      }
      offset = tokenPattern.lastIndex;
    }
    if (offset < source.length) target.append(document.createTextNode(source.slice(offset)));
  }

  function highlightCodeBlocks(target) {
    for (const code of target.querySelectorAll("pre code:not(.language-mermaid)")) {
      const languageClass = [...code.classList].find((name) => name.startsWith("language-"));
      const language = normalizeLanguage(languageClass);
      if (!languageNames.has(language)) continue;
      const source = code.textContent;
      highlightText(source, language, code);
      code.parentElement.classList.add("syntax-highlighted");
      code.parentElement.dataset.language = language.toUpperCase();
    }
  }

  function expandTerms(markdown) {
    return markdown.replace(
      /\[([^\]\n]+)\]\(deepwiki-term:([^\s)]+)\)/g,
      (whole, chineseName, encodedProgramName) => {
        let programName = encodedProgramName;
        try { programName = decodeURIComponent(encodedProgramName); } catch (error) {}
        const term = termByProgram.get(programName);
        const explanation = term?.description ? `\n${term.description}` : "";
        const title = `程序名：${programName}${explanation}`;
        return `<abbr class="term-ref" title="${escapeHtml(title)}">${escapeHtml(chineseName)}</abbr>`;
      },
    );
  }

  async function renderMarkdown(target, markdown) {
    const linked = expandTerms(markdown).replace(
      /\[([^\]\n]+):(\d+)(?:-(\d+))?\]/g,
      (whole, path, line) => {
        const url = withToken(`/api/source?path=${encodeURIComponent(path)}&line=${line}`);
        return `[${whole}](${url} "查看源码")`;
      },
    );
    target.innerHTML = DOMPurify.sanitize(marked.parse(linked));
    for (const code of target.querySelectorAll("pre code.language-mermaid")) {
      const container = document.createElement("div");
      container.className = "mermaid";
      container.textContent = code.textContent;
      code.parentElement.replaceWith(container);
    }
    highlightCodeBlocks(target);
    await mermaid.run({ nodes: target.querySelectorAll(".mermaid"), suppressErrors: true });
    for (const link of target.querySelectorAll("a[href^='/api/source']")) {
      link.classList.add("cite");
      link.addEventListener("click", async (event) => {
        event.preventDefault();
        await openSource(link.getAttribute("href"), link.textContent);
      });
    }
  }

  async function openSource(url, title) {
    try {
      const response = await fetch(url);
      if (!response.ok) throw new Error(`源码读取失败 (${response.status})`);
      const line = Number(new URL(url, location.href).searchParams.get("line")) || 1;
      document.querySelector("#source-title").textContent = title;
      renderSource(await response.text(), line, languageFromPath(title));
      dialog.showModal();
    } catch (error) {
      toast(error.message);
    }
  }

  function renderSource(content, targetLine, language) {
    const view = document.querySelector("#source-content");
    view.replaceChildren();
    let active = null;
    for (const raw of content.split("\n")) {
      if (!raw.trim() && !view.hasChildNodes()) continue;
      const match = raw.match(/^\s*(\d+)\s{2}(.*)$/);
      const line = document.createElement("div");
      line.className = "line";
      const number = document.createElement("span");
      number.className = "ln";
      const text = document.createElement("span");
      text.className = "tx";
      if (match) {
        number.textContent = match[1];
        highlightText(match[2], language, text);
        if (Number(match[1]) === targetLine) {
          line.classList.add("active");
          active = line;
        }
      } else {
        highlightText(raw, language, text);
      }
      line.append(number, text);
      view.append(line);
    }
    if (active) requestAnimationFrame(() => active.scrollIntoView({ block: "center" }));
  }

  function showEmpty() {
    page.innerHTML = DOMPurify.sanitize(
      `<div class="empty">${emptyIcon}<h2>开始探索这个仓库</h2><p>在左侧选择一个 Wiki 页面，或在下方向 AI 提问。</p></div>`,
    );
  }

  function showError(message) {
    page.innerHTML = DOMPurify.sanitize(
      `<div class="empty">${emptyIcon}<h2>加载失败</h2><p></p></div>`,
    );
    page.querySelector(".empty p").textContent = message;
  }

  async function loadPage(id, button) {
    if (loadedPage?.id === id && !termsToggle.classList.contains("active")) {
      const expanded = button.getAttribute("aria-expanded") !== "true";
      button.setAttribute("aria-expanded", String(expanded));
      button.nextElementSibling.hidden = !expanded;
      return;
    }
    const request = ++pageRequest;
    loadedPage = null;
    sections = [];
    collapsePages();
    button.classList.add("active");
    button.setAttribute("aria-current", "page");
    termsToggle.classList.remove("active");
    const title = button.querySelector("strong").textContent;
    viewTitle.textContent = title;
    viewTag.textContent = "LOADING";
    page.innerHTML = DOMPurify.sanitize(
      '<div class="skeleton" aria-hidden="true"><div class="sk sk-title"></div><div class="sk sk-line w92"></div><div class="sk sk-line w78"></div></div>',
    );
    try {
      const response = await fetch(withToken(`/api/page?id=${encodeURIComponent(id)}`));
      if (!response.ok) throw new Error(`页面读取失败 (${response.status})`);
      const markdown = await response.text();
      if (request !== pageRequest) return;
      currentPageMarkdown = markdown;
      await renderMarkdown(page, currentPageMarkdown);
      if (request !== pageRequest) return;
      loadedPage = { id, title };
      viewTag.textContent = "MARKDOWN";
      main.scrollTop = 0;
      buildSections(button);
    } catch (error) {
      if (request !== pageRequest) return;
      currentPageMarkdown = "";
      viewTag.textContent = "ERROR";
      showError(error.message);
    }
  }

  function updateTocCount(visible, total) {
    tocCount.textContent = total === 0 ? "暂无页面" : `${visible} / ${total} 页`;
  }

  async function loadToc() {
    const response = await fetch(withToken("/api/toc"));
    const entries = await response.json();
    if (!Array.isArray(entries) || entries.length === 0) {
      toc.replaceChildren();
      const empty = document.createElement("div");
      empty.className = "toc-empty";
      empty.textContent = "还没有生成 Wiki，请在 zeda 中运行 /deepwiki generate";
      toc.append(empty);
      updateTocCount(0, 0);
      showEmpty();
      return;
    }
    toc.replaceChildren();
    entries.forEach((entry, index) => {
      const entryElement = document.createElement("div");
      entryElement.className = "toc-entry";
      const button = document.createElement("button");
      button.className = "page-button";
      button.type = "button";
      button.setAttribute("aria-expanded", "false");
      const children = document.createElement("div");
      children.id = `toc-sections-${index}`;
      children.className = "toc-sections";
      children.hidden = true;
      button.setAttribute("aria-controls", children.id);
      button.dataset.search = `${entry.title} ${entry.description || ""}`.toLowerCase();
      button.innerHTML = '<span class="idx"></span><span class="txt"><strong></strong></span>';
      button.querySelector(".idx").textContent = String(index + 1).padStart(2, "0");
      button.querySelector("strong").textContent = entry.title;
      button.title = entry.description || entry.title;
      button.addEventListener("click", () => loadPage(entry.id, button));
      entryElement.append(button, children);
      toc.append(entryElement);
    });
    updateTocCount(entries.length, entries.length);
    await loadPage(entries[0].id, toc.querySelector(".page-button"));
  }

  async function loadTerms() {
    const response = await fetch(withToken("/api/terms"));
    if (!response.ok) throw new Error(`名词 Wiki 读取失败 (${response.status})`);
    const payload = await response.json();
    terms = Array.isArray(payload) ? payload.filter((term) =>
      term && typeof term.program_name === "string" &&
      typeof term.chinese_name === "string" &&
      typeof term.description === "string") : [];
    termByProgram.clear();
    for (const term of terms) termByProgram.set(term.program_name, term);
  }

  function sourceParts(source) {
    const separator = source.lastIndexOf(":");
    if (separator <= 0) return null;
    const line = Number(source.slice(separator + 1));
    if (!Number.isInteger(line) || line < 1) return null;
    return { path: source.slice(0, separator), line };
  }

  function showTerms() {
    ++pageRequest;
    loadedPage = null;
    sections = [];
    collapsePages();
    termsToggle.classList.add("active");
    viewTitle.textContent = "GLOSSARY";
    viewTag.textContent = `${terms.length} TERMS`;
    toc.querySelectorAll("button").forEach((item) => item.classList.remove("active"));
    page.replaceChildren();

    const title = document.createElement("h1");
    title.textContent = "名词 Wiki";
    const intro = document.createElement("p");
    intro.className = "glossary-intro";
    intro.textContent = "主要程序名与中文作用名的对应关系。Wiki 正文优先显示中文名，悬停即可查看源码中的准确叫法。";
    page.append(title, intro);
    if (terms.length === 0) {
      const empty = document.createElement("p");
      empty.textContent = "尚未生成术语表，请运行 /deepwiki generate。";
      page.append(empty);
      return;
    }

    const table = document.createElement("table");
    table.className = "glossary-table";
    const head = document.createElement("thead");
    head.innerHTML = "<tr><th>中文作用名</th><th>程序名</th><th>类型</th><th>解释</th><th>源码</th></tr>";
    const body = document.createElement("tbody");
    for (const term of terms) {
      const row = document.createElement("tr");
      const chinese = document.createElement("td");
      const strong = document.createElement("strong");
      strong.textContent = term.chinese_name;
      chinese.append(strong);
      const program = document.createElement("td");
      const code = document.createElement("code");
      code.textContent = term.program_name;
      program.append(code);
      const kind = document.createElement("td");
      kind.className = "term-kind";
      kind.textContent = term.kind || "symbol";
      const description = document.createElement("td");
      description.textContent = term.description;
      const source = document.createElement("td");
      const location = sourceParts(term.source || "");
      if (location) {
        const button = document.createElement("button");
        button.type = "button";
        button.className = "glossary-source";
        button.textContent = term.source;
        button.addEventListener("click", () => openSource(
          withToken(`/api/source?path=${encodeURIComponent(location.path)}&line=${location.line}`),
          term.source,
        ));
        source.append(button);
      }
      row.append(chinese, program, kind, description, source);
      body.append(row);
    }
    table.append(head, body);
    page.append(table);
    document.querySelector("main").scrollTo({ top: 0, behavior: "smooth" });
  }

  termsToggle.addEventListener("click", showTerms);

  tocFilter.addEventListener("input", () => {
    const needle = tocFilter.value.trim().toLowerCase();
    let visible = 0;
    for (const button of toc.querySelectorAll(".page-button")) {
      const show = !needle || button.dataset.search.includes(needle);
      button.parentElement.hidden = !show;
      if (show) visible += 1;
    }
    updateTocCount(visible, toc.querySelectorAll("button").length);
  });

  askForm.addEventListener("submit", async (event) => {
    event.preventDefault();
    const value = question.value.trim();
    if (!value) return;
    answer.innerHTML = "";
    answerCard.hidden = false;
    answerCard.classList.add("streaming");
    answerCard.classList.remove("error");
    question.disabled = true;
    askButton.disabled = true;
    askButton.textContent = "思考中…";
    try {
      const response = await fetch(withToken("/api/ask"), {
        method: "POST",
        headers: { "Content-Type": "application/json", "X-DeepWiki-Token": token },
        body: JSON.stringify({ question: value }),
      });
      if (!response.ok) throw new Error(await response.text());
      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let buffer = "";
      let markdown = "";
      while (true) {
        const { value: chunk, done } = await reader.read();
        if (done) break;
        buffer += decoder.decode(chunk, { stream: true });
        const messages = buffer.split("\n\n");
        buffer = messages.pop();
        for (const message of messages) {
          const data = message.split("\n").find((line) => line.startsWith("data: "));
          if (!data || message.startsWith("event: done")) continue;
          if (message.startsWith("event: error")) throw new Error(data.slice(6));
          markdown += JSON.parse(data.slice(6)).delta || "";
          await renderMarkdown(answer, markdown);
        }
      }
      answerCard.classList.remove("streaming");
    } catch (error) {
      answerCard.classList.remove("streaming");
      answerCard.classList.add("error");
      const box = document.createElement("div");
      box.className = "answer-error";
      box.textContent = `问答失败：${error.message}`;
      answer.replaceChildren(box);
      toast(`问答失败：${error.message}`);
    } finally {
      question.disabled = false;
      askButton.disabled = false;
      askButton.textContent = "提问";
      question.focus();
    }
  });

  document.querySelector("#answer-close").addEventListener("click", () => {
    answerCard.hidden = true;
    answer.replaceChildren();
  });

  document.querySelector("#source-close").addEventListener("click", () => dialog.close());

  dialog.addEventListener("click", (event) => {
    if (event.target === dialog) dialog.close();
  });

  document.addEventListener("keydown", (event) => {
    if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "k") {
      event.preventDefault();
      tocFilter.focus();
      tocFilter.select();
      return;
    }
    if (event.key === "/" && !event.metaKey && !event.ctrlKey && !event.altKey &&
        !["INPUT", "TEXTAREA"].includes(document.activeElement?.tagName) && !dialog.open) {
      event.preventDefault();
      question.focus();
    }
  });

  async function bootstrap() {
    await loadTerms();
    await loadToc();
  }

  bootstrap().catch((error) => showError(error.message));
})();
