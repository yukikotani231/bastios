#!/usr/bin/env bash
# Builds the documentation website into site/build/html with mdBook.
#
# The pages are not kept twice: this script copies them from docs/,
# CONTRIBUTING.md, CHANGELOG.md and SECURITY.md into
# site/build/src, rewrites their relative links (to another page of the site
# when the target is one, otherwise to the file on GitHub), runs mdBook, and
# then checks that every link inside the site resolves.
#
# usage: site/build.sh        (needs mdbook on PATH, or MDBOOK=...)
set -euo pipefail
cd "$(dirname "$0")"
MDBOOK=${MDBOOK:-mdbook}

rm -rf build
mkdir -p build/src
cp SUMMARY.md overview.md build/src/
python3 stage.py .. build/src
"$MDBOOK" build
python3 linkcheck.py build/html
echo "site: built site/build/html"
