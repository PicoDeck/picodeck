"""Every link in docs/ goes somewhere.

docs/ is published to picodeck.net/docs from each release, where a broken
relative link or anchor is a 404 or a page that opens at the top, and nothing
else notices. Anchors follow the site's rule (github-slugger's). A link to this
repository on GitHub must name a file that exists here.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
DOCS = REPO / "docs"
GITHUB = "https://github.com/PicoDeck/picodeck/"

PAGES = sorted(DOCS.glob("*.md"))
LINK = re.compile(r"(?<!\\)\]\(([^()\s]*(?:\([^()\s]*\)[^()\s]*)*)(?:\s+\"[^\"]*\")?\)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")


def prose(page: Path):
    """(line number, line) outside fenced code, with inline code emptied, so a
    `[x](y)` shown as code is not a link. A heading keeps its code: it counts
    towards the anchor."""
    fence = None
    for number, line in enumerate(page.read_text(encoding="utf-8").splitlines(), 1):
        mark = re.match(r"\s*(```+|~~~+)", line)
        if mark:
            token = mark.group(1)[:3]
            if fence is None:
                fence = token
            elif fence == token:
                fence = None
            continue
        if fence is None:
            yield number, line


def slug(heading: str) -> str:
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)  # a link's text
    text = text.replace("`", "").replace("*", "")
    text = re.sub(r"[^\w\- ]", "", text.strip().lower())
    return text.replace(" ", "-")


def anchors(page: Path) -> set[str]:
    seen: dict[str, int] = {}
    out = set()
    for _, line in prose(page):
        m = HEADING.match(line)
        if not m:
            continue
        base = slug(m.group(2))
        n = seen.get(base, 0)
        seen[base] = n + 1
        out.add(base if n == 0 else f"{base}-{n}")
    return out


ANCHORS = {page.name: anchors(page) for page in PAGES}


def links(page: Path):
    for number, line in prose(page):
        for target in LINK.findall(re.sub(r"`[^`]*`", "``", line)):
            yield number, target


def broken(page: Path):
    for number, target in links(page):
        where = f"{page.name}:{number}: {target}"
        if target.startswith(GITHUB):
            m = re.match(r"(?:blob|tree|raw)/[^/]+/(.+?)(?:#.*)?$", target[len(GITHUB):])
            if m and not (REPO / m.group(1)).exists():
                yield f"{where}: no such path in this repository"
        elif re.match(r"[a-z][a-z0-9+.-]*:", target) or target.startswith("//"):
            continue  # another site
        else:
            name, _, anchor = target.partition("#")
            name = name or page.name
            if name.startswith("/") or not (DOCS / name).exists():
                yield f"{where}: no such page"
            elif anchor and anchor not in ANCHORS.get(name, ()):
                yield f"{where}: no such heading"


@pytest.mark.parametrize("page", PAGES, ids=lambda p: p.name)
def test_links_resolve(page):
    assert list(broken(page)) == []


def _sidebar_pages():
    def walk(items):
        for item in items:
            if "page" in item:
                yield item["page"]
            yield from walk(item.get("items", []))

    return list(walk(json.loads((DOCS / "_sidebar.json").read_text(encoding="utf-8"))))


def test_sidebar_names_pages_that_exist():
    assert [p for p in _sidebar_pages() if not (DOCS / p).is_file()] == []


def test_every_page_is_in_the_sidebar():
    assert sorted({p.name for p in PAGES} - set(_sidebar_pages())) == []


def test_the_checker_finds_a_broken_link(tmp_path):
    """A checker that sees no links would pass everything."""
    assert sum(1 for page in PAGES for _ in links(page)) > 200
    page = tmp_path / "x.md"
    page.write_text(
        "[a](index.md#nope) [b](Nope.md) `[c](Nope.md)` "
        f"[d]({GITHUB}blob/master/no/such/file.c) [e](index.md)\n"
    )
    found = "\n".join(broken(page))
    assert "index.md#nope" in found
    assert "Nope.md" in found and found.count("Nope.md") == 1
    assert "no/such/file.c" in found
    assert "[e]" not in found


def test_slug_matches_the_site():
    assert slug("`picocalc.fs.open(path [, mode])`") == "picocalcfsopenpath--mode"
    assert slug("5. Copy your own apps") == "5-copy-your-own-apps"
    assert slug("Bundle the libraries you use") == "bundle-the-libraries-you-use"
    assert slug("`system_clock_khz`") == "system_clock_khz"
