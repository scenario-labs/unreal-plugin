#!/usr/bin/env python3
# Copyright (c) 2026 Scenario Inc. Licensed under the MIT License.
"""Build the docs site: the handbook and its companion pages, one HTML page each.

Usage: uv run --no-project --with markdown==3.10.3 python tools/build_site.py --output DIR [--docs DIR]

Pages (Markdown in docs/, or in --docs for a trial build):
  USER_GUIDE.md       -> index.html            (the handbook, with the install steps)
  QUICK_START.md      -> quick-start.html
  MCP_GUIDE.md        -> agents.html
  TROUBLESHOOTING.md  -> troubleshooting.html

Every page shares docs/site/template.html, with docs/site/site.css and the
page script docs/site/handbook.js (the in-page finder) inlined. Links between these Markdown files
become links between the pages; links to other repository files point at
GitHub; images stay local (docs/images/ is copied to images/). The version and
description come from Plugins/Scenario/Scenario.uplugin. Download links point
at the GitHub releases.
"""
import argparse
import html
import json
import pathlib
import re
import shutil
import sys

import markdown

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPO = "https://github.com/scenario-labs/unreal-plugin"
SITE = ROOT / "docs/site"
UPLUGIN = ROOT / "Plugins/Scenario/Scenario.uplugin"
ENGINES = "5.6, 5.7 and 5.8"
# (Markdown file, page, label in the site navigation)
PAGES = [
    ("USER_GUIDE.md", "index.html", "User guide"),
    ("QUICK_START.md", "quick-start.html", "Quick start"),
    ("MCP_GUIDE.md", "agents.html", "Scenario MCP"),
    ("TROUBLESHOOTING.md", "troubleshooting.html", "Troubleshooting"),
]
LINK_RE = re.compile(r"(!?)\]\((?!https?://|#|mailto:)([^)\s]+)\)")
INSTALL = """<h2 id="install">Install</h2>
<ol>
  <li>Download the zip for your Unreal version from the <a href="{releases}">GitHub releases</a> and unzip it into your project, so that <code>Scenario.uplugin</code> sits at <code>&lt;Project&gt;/Plugins/Scenario/Scenario.uplugin</code>. Create the <code>Plugins</code> folder if the project has none.</li>
  <li>Open the project. In <strong>Edit &gt; Plugins</strong>, search for Scenario, check that it is enabled, and restart the editor if Unreal asks.</li>
  <li>Click the <strong>Scenario</strong> button in the level editor toolbar and sign in.</li>
</ol>
<p class="note">Release zips from GitHub are not signed yet. If macOS refuses to load the plugin's libraries, run <code>xattr -dr com.apple.quarantine &lt;Project&gt;/Plugins/Scenario</code> and open the project again.</p>
"""


def inline(name: str, element: str) -> str:
    """A file of docs/site/ placed inside its own <style> or <script> element."""
    text = (SITE / name).read_text(encoding="utf-8")
    if f"</{element}" in text.casefold():
        raise SystemExit(f"docs/site/{name} must not close its <{element}> element")
    return text


def plugin() -> dict:
    return json.loads(UPLUGIN.read_text(encoding="utf-8-sig"))


def rewrite_links(text: str, docs: pathlib.Path) -> str:
    """Page-to-page links become .html links, images stay local, other files point at GitHub."""
    pages = {source: page for source, page, _ in PAGES}

    def target(match: re.Match) -> str:
        bang, link = match.group(1), match.group(2)
        path, _, fragment = link.partition("#")
        suffix = f"#{fragment}" if fragment else ""
        name = pathlib.PurePosixPath(path).name
        if path and pathlib.PurePosixPath(path).parent in (pathlib.PurePosixPath("."), pathlib.PurePosixPath("")) and name in pages:
            return f"{bang}]({pages[name]}{suffix})"
        if not path:
            return match.group(0)
        if path.startswith("images/"):
            return f"{bang}]({path}{suffix})"
        resolved = (docs / path).resolve()
        try:
            relative = resolved.relative_to(ROOT).as_posix()
        except ValueError:
            # A trial build from another folder: resolve against the real docs folder.
            relative = (ROOT / "docs" / path).resolve().relative_to(ROOT).as_posix()
        kind = "tree" if path.endswith("/") else "blob"
        return f"{bang}]({REPO}/{kind}/main/{relative}{suffix})"

    return LINK_RE.sub(target, text)


def render(source: pathlib.Path, docs: pathlib.Path) -> tuple[str, str, list]:
    """Title (the first # heading), HTML body and the h2 contents of one Markdown page."""
    text = source.read_text(encoding="utf-8")
    title_match = re.match(r"\A\s*# ([^\n]+)\n+", text)
    title = title_match.group(1).strip() if title_match else source.stem.replace("_", " ").title()
    body = text[title_match.end():] if title_match else text
    # The page shows the version in its header; a "Version x.y.z" line under the title repeats it.
    body = re.sub(r"\A(Version [^\n]*\n+)", "", body)
    md = markdown.Markdown(extensions=["toc", "tables", "fenced_code", "sane_lists"],
                           extension_configs={"toc": {"toc_depth": "2-3"}})
    content = md.convert(rewrite_links(body, docs))
    contents = [(t["id"], t["name"]) for t in md.toc_tokens]
    return title, content, contents


def title_html(title: str) -> str:
    """'Scenario for Unreal Engine: user guide' -> the part after the colon in italics."""
    head, sep, tail = title.partition(": ")
    return f"{html.escape(head)}: <em>{html.escape(tail)}</em>" if sep else html.escape(title)


def build(output: pathlib.Path, docs: pathlib.Path) -> None:
    if output.exists() and any(output.iterdir()):
        raise SystemExit(f"{output} is not empty")
    missing = [source for source, _, _ in PAGES if not (docs / source).is_file()]
    if missing:
        raise SystemExit(f"missing in {docs}: {', '.join(missing)}")
    output.mkdir(parents=True, exist_ok=True)
    shutil.copytree(ROOT / "docs/images", output / "images")
    if (docs / "images").is_dir() and docs.resolve() != (ROOT / "docs").resolve():
        shutil.copytree(docs / "images", output / "images", dirs_exist_ok=True)

    info = plugin()
    version = str(info["VersionName"])
    description = info["Description"]
    template = (SITE / "template.html").read_text(encoding="utf-8")
    style, script = inline("site.css", "style"), inline("handbook.js", "script")
    releases = f"{REPO}/releases"
    hero = (output / "images/studio.png").is_file()

    for source, page, label in PAGES:
        title, content, contents = render(docs / source, docs)
        index = page == "index.html"
        current = ' aria-current="page"'
        nav = "\n".join(
            f'<li><a href="{p}"{current if p == page else ""}>{html.escape(name)}</a></li>'
            for _, p, name in PAGES
        )
        toc = list(contents)
        if index:
            toc.insert(0, ("install", "Install"))
        lead = ""
        if index:
            lead = (
                f'<p class="lead">{html.escape(description)}</p>\n'
                f'<section class="panel" aria-labelledby="download">\n'
                f'  <p class="eyebrow" id="download">Latest release</p>\n'
                f'  <a class="button" href="{releases}/latest">Download {html.escape(version)}</a>\n'
                f'  <dl>\n'
                f'    <dt>Unreal Engine</dt><dd>{ENGINES}, editor plugin in C++</dd>\n'
                f'    <dt>License</dt><dd>MIT</dd>\n'
                f'    <dt>Source</dt><dd><a href="{REPO}">{REPO.removeprefix("https://")}</a></dd>\n'
                f'  </dl>\n'
                f'</section>\n'
            )
            if hero:
                lead += ('<img class="hero" src="images/studio.png" alt="The Scenario Studio tab in the '
                         'Unreal Editor" width="1600" height="1000">\n')
            lead += INSTALL.format(releases=html.escape(releases))
        slots = {
            "page_title": html.escape(title if "Scenario" in title else f"{title} · Scenario for Unreal Engine"),
            "title": title_html(title),
            "description": html.escape(description),
            "version": html.escape(version),
            "label": html.escape(label),
            "nav": nav,
            "toc": "\n".join(f'<li><a href="#{i}">{n}</a></li>' for i, n in toc),
            "lead": lead,
            "content": content,
            "source": f"{REPO}/blob/main/docs/{source}",
            "repo": REPO,
            "releases": releases,
            "style": style,
            "script": script,
        }
        rendered = re.sub(r"{{\s*(\w+)\s*}}", lambda m: slots[m.group(1)], template)
        (output / page).write_text(rendered, encoding="utf-8")
    (output / ".nojekyll").write_text("")
    print(f"site {version}: {len(PAGES)} pages in {output}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--docs", type=pathlib.Path, default=ROOT / "docs",
                        help="folder holding the Markdown pages (default: docs/), for trial builds")
    args = parser.parse_args()
    build(args.output.resolve(), args.docs.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
