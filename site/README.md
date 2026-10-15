# The documentation website

`site/build.sh` builds the website into `site/build/html` with
[mdBook](https://rust-lang.github.io/mdBook/) (CI uses 0.4.52; install it with
`cargo install mdbook --version 0.4.52` or from its release page).

The pages are the repository's own Markdown files (`docs/`, `CONTRIBUTING.md`,
`CHANGELOG.md`, `SECURITY.md`), copied in at build time by `stage.py`, which
also turns their relative links into links between pages or to files on
GitHub. Only the table of contents (`SUMMARY.md`) and the overview page
(`overview.md`) live here. To add a page, add it to `PAGES` in `stage.py` and
to `SUMMARY.md`.

After building, `linkcheck.py` fails the build if a link between pages points
at a missing page or anchor.

## Publishing

The site is served by GitHub Pages at <https://yukikotani231.github.io/bastios/>
(`site-url` in `book.toml` must match the path if that changes). Every push to
`main` publishes it through `.github/workflows/pages.yml`. This needs
Settings > Pages > Build and deployment > Source: **GitHub Actions** once.

To check the result locally: `site/build.sh`, then
`python3 -m http.server -d site/build/html` and open <http://localhost:8000/>.
