# Contributing to Three Kingdoms Coop

Help is welcome with runtime fixes, UI layout, automated tests, documentation
and multiplayer playtesting. Start with the [README](README.md) to build the
runtime and run the offline tests.

## Choose a change

Check existing issues and pull requests before starting. For a larger change or
an address rebase, open an issue describing the problem, proposed approach and
evidence first. Good starting tasks include clarifying build instructions,
improving an error message, reducing a bug to a repeatable case, or adding a
synthetic regression for a reported failure.

The unresolved fourth-player join crash and blocked campaign dilemmas need
investigation, but do not assume that changing a player-count constant fixes
them. Follow the state through the relevant lobby, session or campaign path.

## Submit a pull request

1. Fork and clone the repository, then create a branch for one focused change.
2. Make the change and document any new game-version or ABI assumptions near
   the affected code. Separate observed behavior from a hypothesis.
3. Run relevant checks. For runtime changes, build both Release and Debug with
   `runtime\build.bat all` and run `runtime\build.bat test` from an x64 Native
   Tools command prompt. Add a regression when it meaningfully reproduces the failure.
4. Explain the original problem, resulting behavior, checks performed, and
   anything untested in the pull request. Include reproduction steps when useful.

Installer changes should run the fixture tests in
[`tools/installer/README.md`](tools/installer/README.md), including both variant
switch directions. These tests use disposable fake game folders; do not point
them at a real installation.

For UI changes, include screenshots and describe the player count, resolution
and UI scale used. Keep generated packs and extracted game material out of the
pull request.

## Multiplayer validation

Offline tests check synthetic objects and local behavior. They cannot establish
that a live multiplayer session stays synchronized.

For changes to replicated state, faction ownership, turn sequencing or battle
commands, test with matching runtime and pack versions on every participant.
Record the player count, host/client roles, exact build identity, scenario and
outcome. Collect each participant's logs. A session starting successfully is
only one checkpoint: check the affected action, the next turn or transition,
and save/reload where relevant. State explicitly when live testing is pending.

Close the game and launcher before replacing DLLs. Use the developer installer
for diagnostics; the release DLL has no developer control pipe. Keep developer
facilities behind the existing build-mode boundary. New player-facing logging
also needs a corresponding review of `runtime/release-log-formats.json`.

## Game files and generated output

Extract required UI inputs from your own game installation and keep them local.
Do not commit game executables, extracted XML or textures, captures, save files,
generated packs, compiler output or installer EXEs. The public tree contains
source and authored text; the Workshop pack is distributed separately.

If you contribute replacement art, include its source and provenance. Review
logs, screenshots and reproductions for personal information before sharing.
Existing references to private research notes in source comments do not require
access to those notes to build or contribute.

## License

Submit project-code contributions under this repository's GPL-3.0-only license.
Include attribution and license details for any third-party code or art you add.
