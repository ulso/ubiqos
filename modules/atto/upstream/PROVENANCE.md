# Atto, vendored

Hugh Barney's Atto emacs, in about two thousand lines, derived from Anthony
Howe's editor of 1993. **Public domain** -- the header of every file says so and
so does the project's README, which is why it can sit here unchanged.

    https://github.com/hughbarney/atto
    commit 0b2c5bb0a236eb8d0272cdc7c501758b4fcc413c (2022-11-05)

Copied rather than cloned so the build needs no network and the version is ours
to choose. **The files in this directory are unmodified.** Everything UbiqOS
needs to add is beside them in ../, so a later update is a copy over the top and
a look at the diff.

The curses these are built against is not ncurses: it is
modules/wasm/examples/curses, twenty-one calls over ANSI, written for the wasm
build and reused here unchanged. See that directory's README for what it does
and does not do.
