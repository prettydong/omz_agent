(() => {
  const token = new URLSearchParams(location.search).get("token") || "";
  const withToken = (path) => `${path}${path.includes("?") ? "&" : "?"}token=${encodeURIComponent(token)}`;

  const page = document.querySelector("#page");
  const toc = document.querySelector("#toc");
  const tocFilter = document.querySelector("#toc-filter");
  const tocCount = document.querySelector("#toc-count");
  const themeToggle = document.querySelector("#theme-toggle");
  const answerCard = document.querySelector("#answer-card");
  const answer = document.querySelector("#answer");
  const question = document.querySelector("#question");
  const askForm = document.querySelector("#ask-form");
  const askButton = document.querySelector("#ask-btn");
  const dialog = document.querySelector("#source-dialog");
  const toastEl = document.querySelector("#toast");

  const SUN = '<svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M4.9 4.9l1.4 1.4m11.4 11.4 1.4 1.4M2 12h2m16 0h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></svg>';
  const MOON = '<svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8Z"/></svg>';

  let currentPageMarkdown = "";
  let toastTimer = 0;

  const emptyIcon = '<svg viewBox="0 0 24 24" width="44" height="44" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"/></svg>';

  function toast(message) {
    toastEl.textContent = message;
    toastEl.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { toastEl.hidden = true; }, 4200);
  }

  function isDark() {
    return document.documentElement.dataset.theme === "dark";
  }

  function configureMermaid() {
    mermaid.initialize({
      startOnLoad: false,
      securityLevel: "strict",
      theme: isDark() ? "dark" : "default",
      fontFamily: "Inter, PingFang SC, system-ui, sans-serif",
    });
  }

  function refreshThemeIcons() {
    themeToggle.innerHTML = isDark() ? SUN : MOON;
    themeToggle.title = isDark() ? "切换到浅色主题" : "切换到深色主题";
  }

  async function rerenderIfPossible() {
    configureMermaid();
    if (currentPageMarkdown && !answerCard.hidden) await renderMarkdown(answer, currentPageMarkdown);
  }

  themeToggle.addEventListener("click", () => {
    const next = isDark() ? "light" : "dark";
    document.documentElement.dataset.theme = next;
    try { localStorage.setItem("deepwiki-theme", next); } catch (error) {}
    refreshThemeIcons();
    rerenderIfPossible().catch(() => {});
  });

  configureMermaid();
  refreshThemeIcons();

  async function renderMarkdown(target, markdown) {
    const linked = markdown.replace(
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
    if (event.key === "/" && !event.metaKey && !event.ctrlKey && !event.altKey &&
        !["INPUT", "TEXTAREA"].includes(document.activeElement?.tagName) && !dialog.open) {
      event.preventDefault();
      question.focus();
    }
  });

  loadToc().catch((error) => showError(error.message));
})();
