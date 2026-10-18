// @ts-check
// Search inside the current page: no service, no index file. The sections are
// the page's h2 and h3 headings with the text under them; every query word
// must appear (prefix match), titles rank above text. tools/build_site.py
// inlines this file into template.html.
(() => {
  const input = /** @type {HTMLInputElement | null} */ (document.getElementById("search-input"));
  const panel = document.getElementById("search-panel");
  const list = document.getElementById("search-results");
  const status = document.getElementById("search-status");
  const article = document.querySelector(".handbook");
  if (!input || !panel || !list || !status || !article) return;

  /** @param {string} text */
  const fold = text => text.normalize("NFD").replace(/\p{M}/gu, "").toLowerCase();

  /** @type {{id: string, title: string, trail: string, text: string, foldTitle: string, foldText: string}[]} */
  const sections = [];
  let parent = "";
  for (const heading of article.querySelectorAll("h2[id], h3[id]")) {
    const title = heading.textContent?.trim() ?? "";
    if (heading.tagName === "H2") parent = title;
    let text = "";
    for (let node = heading.nextElementSibling; node && !/^H[1-3]$/.test(node.tagName); node = node.nextElementSibling) {
      text += " " + (node.textContent ?? "");
    }
    text = text.replace(/\s+/g, " ").trim();
    sections.push({
      id: heading.id, title, text,
      trail: heading.tagName === "H3" ? parent : "",
      foldTitle: fold(title), foldText: fold(text),
    });
  }

  /** @param {string} query */
  const search = query => {
    const terms = fold(query).match(/[\p{L}\p{N}]+/gu) ?? [];
    if (!terms.length) return [];
    const results = [];
    for (const section of sections) {
      let score = 0;
      let all = true;
      for (const term of terms) {
        // Terms hold letters and digits only, so they need no escaping.
        const pattern = new RegExp("(^|[^\\p{L}\\p{N}])" + term, "u");
        const inTitle = pattern.test(section.foldTitle);
        const inText = pattern.test(section.foldText);
        if (!inTitle && !inText) { all = false; break; }
        score += (inTitle ? 3 : 0) + (inText ? 1 : 0);
      }
      if (all) results.push({ section, score, terms });
    }
    return results.sort((a, b) => b.score - a.score).slice(0, 12);
  };

  /**
   * Text with the query terms wrapped in <mark>, built as DOM nodes.
   * @param {HTMLElement} target @param {string} text @param {string[]} terms
   */
  const highlight = (target, text, terms) => {
    const folded = fold(text);
    let at = 0;
    while (at < text.length) {
      let next = -1;
      let length = 0;
      for (const term of terms) {
        const found = folded.indexOf(term, at);
        if (found !== -1 && (next === -1 || found < next)) { next = found; length = term.length; }
      }
      if (next === -1) { target.append(text.slice(at)); break; }
      target.append(text.slice(at, next));
      const mark = document.createElement("mark");
      mark.textContent = text.slice(next, next + length);
      target.append(mark);
      at = next + length;
    }
  };

  /** @param {string} text @param {string[]} terms */
  const snippet = (text, terms) => {
    const folded = fold(text);
    const first = Math.min(...terms.map(t => { const i = folded.indexOf(t); return i === -1 ? Infinity : i; }));
    const start = Number.isFinite(first) ? Math.max(0, first - 50) : 0;
    const piece = text.slice(start, start + 150);
    return (start > 0 ? "..." : "") + piece + (start + 150 < text.length ? "..." : "");
  };

  let selected = -1;
  const links = () => /** @type {HTMLAnchorElement[]} */ ([...list.querySelectorAll("a")]);
  const select = index => {
    const all = links();
    selected = all.length ? (index + all.length) % all.length : -1;
    all.forEach((link, i) => link.setAttribute("aria-selected", String(i === selected)));
    all[selected]?.scrollIntoView({ block: "nearest" });
  };
  const close = () => { panel.hidden = true; input.setAttribute("aria-expanded", "false"); selected = -1; };

  const update = () => {
    const query = input.value.trim();
    list.replaceChildren();
    if (!query) { close(); return; }
    const results = search(query);
    for (const { section, terms } of results) {
      const item = document.createElement("li");
      const link = document.createElement("a");
      link.href = `#${section.id}`;
      if (section.trail) {
        const trail = document.createElement("span");
        trail.className = "result-trail";
        trail.textContent = section.trail;
        link.append(trail);
      }
      const title = document.createElement("span");
      title.className = "result-title";
      highlight(title, section.title, terms);
      const text = document.createElement("span");
      text.className = "result-text";
      highlight(text, snippet(section.text, terms), terms);
      link.append(title, text);
      item.append(link);
      list.append(item);
    }
    status.textContent = results.length ? "" : "No section of this page matches. Try another page of the handbook.";
    panel.hidden = false;
    input.setAttribute("aria-expanded", "true");
    select(results.length ? 0 : -1);
  };

  input.addEventListener("input", update);
  input.addEventListener("keydown", event => {
    if (event.key === "ArrowDown") { event.preventDefault(); select(selected + 1); }
    else if (event.key === "ArrowUp") { event.preventDefault(); select(selected - 1); }
    else if (event.key === "Enter" && selected >= 0) { event.preventDefault(); links()[selected].click(); close(); input.blur(); }
    else if (event.key === "Escape") { input.value = ""; close(); input.blur(); }
  });
  list.addEventListener("click", event => { if (/** @type {Element} */ (event.target).closest("a")) close(); });
  document.addEventListener("click", event => {
    if (!(/** @type {Element} */ (event.target).closest(".search"))) close();
  });
  document.addEventListener("keydown", event => {
    const target = /** @type {Element} */ (event.target);
    if (event.key === "/" && !target.closest("input, textarea, [contenteditable]")) {
      event.preventDefault();
      input.focus();
    }
  });
})();
