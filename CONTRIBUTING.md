# Contributing to BastiOS

Thanks for your interest. BastiOS is pre-release and moves together with [bastiolang](https://github.com/yukikotani231/bastiolang): a kernel change often needs a compiler change first, and large changes are best discussed in an issue.

## Before you send a pull request

1. Build and test against bastiolang's `main` (see "Build and run" in [README.md](README.md)):
   ```sh
   make test
   ```
   CI runs the same thing.
2. Kernel logic goes in Bastio (`src/`, `user/*.bastio`), where it is proved. Assembly and C (`boot/`, `user/*.S`) are the trusted base: add to them only what Bastio cannot express yet, and say why in the commit.
3. Every new shell command or kernel feature gets a case in `test/shell_test.sh`.

Don't copy counts (lines, pages, tests) into prose; they go stale. Give the command that counts them instead.

## Commits

`<area>: <summary>`, where area is `kernel`, `boot`, `user`, `build`, `test`, `docs`, `ci` and so on. The body says honestly what is proved and what is trusted.

## License

Unless you say otherwise, any contribution you submit for inclusion is licensed as described in [README.md](README.md#license), without any additional terms or conditions.
