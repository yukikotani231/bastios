"""Copy the repository's documentation into the mdBook source directory.

usage: stage.py REPO_ROOT OUT_DIR

PAGES maps a repository file to its page in the book. A relative Markdown
link in a copied file is resolved against the file's place in the repository:
if it lands on another file in PAGES it becomes a link to that page, and if
it lands on any other file or directory of the repository it becomes a link
to it on GitHub. Links that resolve to nothing (code that happens to look
like a link, such as `cast[u64](x)`) are left alone.
"""

import os
import re
import sys

REPO_URL = "https://github.com/yukikotani231/bastios"

PAGES = {
    "docs/getting-started.md": "getting-started.md",
    "docs/using.md": "using.md",
    "docs/linux.md": "linux.md",
    "docs/architecture.md": "architecture.md",
    "docs/trusted-base.md": "trusted-base.md",
    "docs/security.md": "security.md",
    "docs/performance.md": "performance.md",
    "docs/aarch64.md": "aarch64.md",
    "docs/cloud.md": "cloud.md",
    "docs/real-hardware.md": "real-hardware.md",
    "docs/design/async-disk.md": "design-async-disk.md",
    "docs/design/page-cache.md": "design-page-cache.md",
    "CONTRIBUTING.md": "contributing.md",
    "SECURITY.md": "security-policy.md",
    "CHANGELOG.md": "changelog.md",
    "docs/releasing.md": "releasing.md",
}

LINK = re.compile(r"(\]\()([^)\s]+)(\))")
FENCE = re.compile(r"^\s*(```|~~~)")


def rewrite(root, src, line):
    def fix(m):
        target = m.group(2)
        if re.match(r"[a-z]+:", target) or target.startswith("#"):
            return m.group(0)
        path, _, anchor = target.partition("#")
        rel = os.path.normpath(os.path.join(os.path.dirname(src), path))
        if rel.startswith("..") or not os.path.exists(os.path.join(root, rel)):
            return m.group(0)
        frag = "#" + anchor if anchor else ""
        if rel in PAGES:
            new = PAGES[rel] + frag
        else:
            kind = "tree" if os.path.isdir(os.path.join(root, rel)) else "blob"
            new = f"{REPO_URL}/{kind}/main/{rel}{frag}"
        return m.group(1) + new + m.group(3)

    # Inline code spans are left as they are.
    parts = line.split("`")
    for i in range(0, len(parts), 2):
        parts[i] = LINK.sub(fix, parts[i])
    return "`".join(parts)


def main():
    root, out = sys.argv[1], sys.argv[2]
    for src, page in PAGES.items():
        with open(os.path.join(root, src)) as f:
            lines = f.read().split("\n")
        fenced = False
        for i, line in enumerate(lines):
            if FENCE.match(line):
                fenced = not fenced
            elif not fenced:
                lines[i] = rewrite(root, src, line)
        with open(os.path.join(out, page), "w") as f:
            f.write("\n".join(lines))


main()
