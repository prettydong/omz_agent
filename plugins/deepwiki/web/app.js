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

  let currentPageMarkdown = "";
  let terms = [];
  const termByProgram = new Map();
  let toastTimer = 0;

  const emptyIcon = '<svg viewBox="0 0 24 24" width="44" height="44" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"/></svg>';

  function toast(message) {
    toastEl.textContent = message;
    toastEl.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { toastEl.hidden = true; }, 4200);
  }

  function configureMermaid() {
    mermaid.initialize({
      startOnLoad: false,
      securityLevel: "strict",
      theme: "default",
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
      renderSource(await response.text(), line);
      dialog.showModal();
    } catch (error) {
      toast(error.message);
    }
  }

  function renderSource(content, targetLine) {
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
        text.textContent = match[2];
        if (Number(match[1]) === targetLine) {
          line.classList.add("active");
          active = line;
        }
      } else {
        text.textContent = raw;
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
    termsToggle.classList.remove("active");
    viewTitle.textContent = "DOCUMENT";
    viewTag.textContent = "MARKDOWN";
    page.innerHTML = DOMPurify.sanitize(
      '<div class="skeleton" aria-hidden="true"><div class="sk sk-title"></div><div class="sk sk-line w92"></div><div class="sk sk-line w78"></div><div class="sk sk-line w85"></div><div class="sk sk-line w60"></div><div class="sk sk-line w92"></div><div class="sk sk-line w70"></div></div>',
    );
    try {
      const response = await fetch(withToken(`/api/page?id=${encodeURIComponent(id)}`));
      if (!response.ok) throw new Error(`页面读取失败 (${response.status})`);
      currentPageMarkdown = await response.text();
      await renderMarkdown(page, currentPageMarkdown);
      window.scrollTo({ top: 0, behavior: "smooth" });
    } catch (error) {
      currentPageMarkdown = "";
      showError(error.message);
    }
    toc.querySelectorAll("button").forEach((item) => item.classList.remove("active"));
    button?.classList.add("active");
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
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.search = `${entry.title} ${entry.description || ""}`.toLowerCase();
      button.innerHTML = '<span class="idx"></span><span class="txt"><strong></strong><small></small></span>';
      button.querySelector(".idx").textContent = String(index + 1).padStart(2, "0");
      button.querySelector("strong").textContent = entry.title;
      button.querySelector("small").textContent = entry.description || "";
      button.addEventListener("click", () => loadPage(entry.id, button));
      toc.append(button);
    });
    updateTocCount(entries.length, entries.length);
    loadPage(entries[0].id, toc.firstElementChild);
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
    for (const button of toc.querySelectorAll("button")) {
      const show = !needle || button.dataset.search.includes(needle);
      button.hidden = !show;
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
