"""Check that every link from one page of the built site to another resolves,
anchor included.

usage: linkcheck.py HTML_DIR

External links (http, https, mailto) are not fetched. Exits 1 and lists the
broken links if there are any.
"""

import os
import re
import sys
from html.parser import HTMLParser
from urllib.parse import unquote, urlsplit

# mdBook's own pages and assets, which link to things this check need not follow.
SKIP = {"print.html", "404.html", "toc.html"}


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = set()
        self.links = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        if tag == "a" and a.get("href"):
            self.links.append(a["href"])


def parse(path):
    p = Page()
    with open(path, encoding="utf-8") as f:
        p.feed(f.read())
    return p


def main():
    root = sys.argv[1]
    pages = {}
    for dirpath, _, files in os.walk(root):
        for name in files:
            if name.endswith(".html") and name not in SKIP:
                path = os.path.join(dirpath, name)
                pages[os.path.relpath(path, root)] = parse(path)
    broken = []
    for page, parsed in sorted(pages.items()):
        for href in parsed.links:
            u = urlsplit(href)
            if u.scheme or href.startswith("//"):
                continue
            target = page if not u.path else os.path.normpath(
                os.path.join(os.path.dirname(page), unquote(u.path)))
            if not os.path.exists(os.path.join(root, target)):
                broken.append(f"{page}: {href} (no such file)")
            elif u.fragment and target in pages and unquote(u.fragment) not in pages[target].ids:
                broken.append(f"{page}: {href} (no such anchor)")
    for b in broken:
        print("broken link:", b)
    print(f"linkcheck: {len(pages)} pages, {len(broken)} broken links")
    sys.exit(1 if broken else 0)


main()
