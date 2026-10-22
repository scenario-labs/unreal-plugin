// @ts-check
// Behaviour of the docs site pages; tools/build_site.py inlines this file into
// template.html. Two parts:
//   1. "On this page" folds into a box on narrow screens.
//   2. A finder for the current page. Each h2 or h3 with an id is an entry,
//      holding the text up to the next heading. A query matches an entry when
//      every query word starts some word of it; words in the heading count
//      more. No index file and no request leaves the page.

/** Lowercase with accents removed, so accented and plain letters compare equal. */
const plain = (/** @type {string} */ s) => s.normalize("NFD").replace(/\p{M}/gu, "").toLowerCase();
const WORD = /[\p{L}\p{N}]+/gu;
const wordsOf = (/** @type {string} */ s) => plain(s).match(WORD) ?? [];

// 1. "On this page"
{
  const box = /** @type {HTMLDetailsElement | null} */ (document.querySelector(".on-page-box"));
  const narrow = window.matchMedia("(max-width: 1100px)");
  if (box) {
    const fit = () => { box.open = !narrow.matches; };
    fit();
    narrow.addEventListener("change", fit);
    box.querySelector("summary")?.addEventListener("click", e => { if (!narrow.matches) e.preventDefault(); });
    box.addEventListener("click", e => {
      const t = /** @type {Element} */ (e.target);
      if (narrow.matches && t.closest("a")) box.open = false;
    });
  }
}

// 2. Finder
{
  const form = /** @type {HTMLFormElement | null} */ (document.querySelector(".finder"));
  const field = /** @type {HTMLInputElement | null} */ (document.getElementById("finder-q"));
  const drop = /** @type {HTMLElement | null} */ (document.querySelector(".finder-drop"));
  const listbox = document.getElementById("finder-list");
  const empty = document.querySelector(".finder-empty");
  const body = document.querySelector("article.handbook");

  if (form && field && drop && listbox && empty && body) {
    /** @typedef {{id: string, heading: string, under: string, text: string, headWords: string[], textWords: string[]}} Entry */
    /** @type {Entry[]} */
    const entries = [];
    let h2 = "";
    for (const h of body.querySelectorAll("h2[id], h3[id]")) {
      const heading = (h.textContent ?? "").trim();
      const level = h.tagName === "H2" ? 2 : 3;
      if (level === 2) h2 = heading;
      const parts = [];
      for (let el = h.nextElementSibling; el; el = el.nextElementSibling) {
        const m = /^H([1-6])$/.exec(el.tagName);
        if (m && Number(m[1]) <= level) break;
        parts.push(el.textContent ?? "");
      }
      const text = parts.join(" ").replace(/\s+/g, " ").trim();
      entries.push({ id: h.id, heading, under: level === 3 ? h2 : "", text, headWords: wordsOf(heading), textWords: wordsOf(text) });
    }

    /** How many words of `words` start with `term`. */
    const hits = (/** @type {string[]} */ words, /** @type {string} */ term) => words.filter(w => w.startsWith(term)).length;

    /** @param {string[]} terms */
    const rank = terms => entries
      .map(entry => {
        let score = 0;
        for (const term of terms) {
          const inHead = hits(entry.headWords, term);
          const inText = Math.min(hits(entry.textWords, term), 5);
          if (!inHead && !inText) return null;
          score += inHead * 4 + inText;
        }
        return { entry, score };
      })
      .filter(r => r !== null)
      .sort((a, b) => b.score - a.score)
      .slice(0, 10);

    /** Appends `text` to `el`, wrapping the start of each matching word in <mark>. */
    const paint = (/** @type {HTMLElement} */ el, /** @type {string} */ text, /** @type {string[]} */ terms) => {
      for (const piece of text.split(/([\p{L}\p{N}]+)/u)) {
        const p = plain(piece);
        const term = terms.filter(t => p.startsWith(t)).sort((a, b) => b.length - a.length)[0];
        // Accent folding can change a word's length; then mark the whole word.
        const cut = term ? (p.length === piece.length ? term.length : piece.length) : 0;
        if (!cut) { el.append(piece); continue; }
        const mark = document.createElement("mark");
        mark.textContent = piece.slice(0, cut);
        el.append(mark, piece.slice(cut));
      }
    };

    /** The sentence of `text` with the first matching word, at most about 160 characters. */
    const excerpt = (/** @type {string} */ text, /** @type {string[]} */ terms) => {
      const sentences = text.match(/[^.!?]+[.!?]*/g) ?? [text];
      const found = sentences.find(s => wordsOf(s).some(w => terms.some(t => w.startsWith(t)))) ?? sentences[0] ?? "";
      const s = found.trim();
      if (s.length <= 160) return s;
      // Long sentence: keep a window that starts a little before the first match.
      const p = plain(s);
      const at = Math.min(...terms.map(t => { const i = p.search(new RegExp(`(^|[^\\p{L}\\p{N}])${t}`, "u")); return i < 0 ? Infinity : i; }));
      const from = Number.isFinite(at) && at > 60 ? s.lastIndexOf(" ", at - 40) + 1 : 0;
      return (from ? "..." : "") + s.slice(from, from + 157).trimEnd() + "...";
    };

    let active = -1;
    const options = () => /** @type {HTMLElement[]} */ ([...listbox.querySelectorAll("[role=option]")]);
    const highlight = (/** @type {number} */ i) => {
      const all = options();
      active = all.length ? (i + all.length) % all.length : -1;
      all.forEach((o, n) => o.setAttribute("aria-selected", n === active ? "true" : "false"));
      if (active >= 0) {
        field.setAttribute("aria-activedescendant", all[active].id);
        all[active].scrollIntoView({ block: "nearest" });
      } else field.removeAttribute("aria-activedescendant");
    };
    const show = (/** @type {boolean} */ open) => {
      drop.hidden = !open;
      field.setAttribute("aria-expanded", open ? "true" : "false");
      if (!open) highlight(-1);
    };
    const go = (/** @type {number} */ i) => {
      const link = options()[i]?.querySelector("a");
      if (!link) return;
      show(false);
      field.blur();
      location.hash = link.hash;
    };

    const refresh = () => {
      const terms = [...new Set(wordsOf(field.value))];
      listbox.replaceChildren();
      if (!terms.length) { show(false); return; }
      const found = rank(terms);
      found.forEach(({ entry }, n) => {
        const li = document.createElement("li");
        li.id = `finder-opt-${n}`;
        li.setAttribute("role", "option");
        const a = document.createElement("a");
        a.href = `#${entry.id}`;
        a.tabIndex = -1;
        if (entry.under) {
          const path = document.createElement("span");
          path.className = "hit-path";
          path.textContent = entry.under;
          a.append(path);
        }
        const head = document.createElement("span");
        head.className = "hit-title";
        paint(head, entry.heading, terms);
        const line = document.createElement("span");
        line.className = "hit-text";
        paint(line, excerpt(entry.text, terms), terms);
        a.append(head, line);
        li.append(a);
        listbox.append(li);
      });
      empty.textContent = found.length ? "" : "Nothing on this page matches. The other handbook pages are in the menu.";
      show(true);
      highlight(found.length ? 0 : -1);
    };

    field.addEventListener("input", refresh);
    field.addEventListener("focus", () => { if (field.value.trim()) refresh(); });
    field.addEventListener("keydown", e => {
      const keys = { ArrowDown: () => highlight(active + 1), ArrowUp: () => highlight(active - 1),
        Enter: () => go(active), Escape: () => { field.value = ""; show(false); field.blur(); } };
      const act = keys[/** @type {keyof keys} */ (e.key)];
      if (act) { e.preventDefault(); act(); }
    });
    form.addEventListener("submit", e => e.preventDefault());
    listbox.addEventListener("click", e => {
      const li = /** @type {Element} */ (e.target).closest("[role=option]");
      if (!li) return;
      e.preventDefault();
      go(options().indexOf(/** @type {HTMLElement} */ (li)));
    });
    document.addEventListener("pointerdown", e => {
      if (!form.contains(/** @type {Node} */ (e.target))) show(false);
    });
    document.addEventListener("keydown", e => {
      const t = /** @type {Element} */ (e.target);
      if (e.key !== "/" || t.closest("input, textarea, select, [contenteditable]")) return;
      e.preventDefault();
      field.focus();
    });
  }
}
