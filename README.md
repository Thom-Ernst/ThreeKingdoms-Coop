# Three Kingdoms Coop

[![CI](https://github.com/Thom-Ernst/ThreeKingdoms-Coop/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/Thom-Ernst/ThreeKingdoms-Coop/actions/workflows/ci.yml)
[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue.svg)](LICENSE)
[![Support on Ko-fi](https://img.shields.io/badge/Ko--fi-support%20development-FF5E5B.svg?logo=ko-fi&logoColor=white)](https://ko-fi.com/mister_fordo)

Play a Total War: THREE KINGDOMS campaign with more than two players.

This experimental mod combines a runtime DLL with a UI pack to extend the game's
multiplayer lobby and support additional human factions. Three-player campaigns
and battle unit lending have been tested. Four-player support is a work in
progress: a fourth player can still crash on joining.

The current target is the **Windows Steam version of THREE KINGDOMS 1.7.2**.
Game updates can invalidate the runtime hooks. Every player needs the same
runtime build and UI pack version.

If you would like to support development, you can do so on
[Ko-fi](https://ko-fi.com/mister_fordo). Contributions and playtest reports are
welcome too.

## Install and play

The mod has two required parts: the **DLL installer from this repository's
[Releases section](https://github.com/Thom-Ernst/ThreeKingdoms-Coop/releases)** and
the **`tw3k_coop` pack from Steam Workshop**. Subscribing
to the Workshop item alone does not enable additional players.

1. Close THREE KINGDOMS and the Creative Assembly launcher.
2. Run `TW3K-Coop-Setup-<version>.exe` and select your game folder. Setup searches
   Steam libraries; find the folder through Steam's **Manage > Browse local files**.
3. Subscribe to `tw3k_coop` in Steam Workshop.
4. Start the launcher and enable `tw3k_coop` under **Mods**.
5. Have every participant install matching versions, then create or join a
   cooperative campaign lobby.

The first public release and Workshop item are being prepared. Download links
will be added when they are available.

If a Steam update or **Verify integrity of game files** stops the mod loading,
close the game and launcher and run Setup again. Remove the DLL component through
Windows **Apps & features > Three Kingdoms Coop**. Manage the Workshop pack through
Steam and the launcher. See the [installer guide](tools/installer/README.md).

## Current limitations and bug reports

This is experimental multiplayer code. A successful lobby join does not prove
that every campaign or battle path works with extra players.

- The fourth-player join crash is unresolved.
- Hidden or unpresented dilemmas can leave a campaign turn blocked.
- Compatibility with other mods has not been established for every combination.

To report a problem, open an issue with the game version, installer version,
number of players, host/client role, enabled mods, and steps to reproduce it.
Include what you expected and what happened. For multiplayer problems, logs from
each participant help distinguish a shared failure from one client's state.
The runtime writes `tw3k_coop_*.log` in the game folder. Review logs and saves for
player names or other personal information before uploading them. State whether
the build is Release or Debug; the developer build includes extra facilities.

## Contribute

Contributions are welcome in C++ runtime fixes, UI generators, regression tests,
documentation and reproducible playtesting. You can help without reverse
engineering the game: a clear reproduction, a smaller failing case, or a better
build instruction is useful work.

Read [CONTRIBUTING.md](CONTRIBUTING.md) for the contribution workflow, multiplayer
validation expectations and handling of locally extracted game files. For a
larger change, open an issue first so we can agree on the problem and approach.
For a focused fix, submit a pull request explaining the behavior change and how
you checked it.

## Build from source

Runtime builds require Windows, Visual Studio Build Tools with the x64 C++
toolchain, and the Windows SDK. Offline tests also require Python 3.10 or newer.
Clone or fork this repository and open an **x64 Native Tools command prompt** in
its root:

```bat
runtime\build.bat
runtime\build.bat test
```

The first command builds the player `runtime/tw3k_coop.dll` and the proxy
`runtime/proxy/amd_ags_x64.dll`. The second runs synthetic offline regression
tests; it does not launch the game. Build both runtime modes with:

```bat
runtime\build.bat all
```

The developer output is `runtime/tw3k_coop_debug.dll`. It adds diagnostics and a
local control pipe that are compiled out of the player build. Use the Debug
installer to install it under the filename the proxy loads. See
[build modes](runtime/DEBUG-BUILD.md).

To build an installer, install Inno Setup 6 and use Windows PowerShell 5.1 or
PowerShell 7. The build also uses Windows' .NET Framework 4 compiler:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/installer/Build-Installer.ps1 -Version 0.1.0
powershell -NoProfile -ExecutionPolicy Bypass -File tools/installer/Build-Installer.ps1 -Version 0.1.0 -Variant Debug
```

The Debug command requires the DLL produced by `runtime\build.bat all` or
`runtime\build.bat debug`. Installer output goes to `out/installer/`. See the
[installer guide](tools/installer/README.md) for compiler path overrides,
Workshop item IDs, and the offline install/repair/uninstall test suite.

## Build the data pack

The UI pack uses inputs extracted from your own game installation. This
repository supplies generators and authored tooltip text; it does not include
the vanilla UI files, textures or built pack. Follow
[pack development](docs/pack-development.md) for required inputs and build steps.
Runtime development and offline tests do not require rebuilding the pack.

## Source map

| Path | Purpose |
|---|---|
| `runtime/src/lobby.cpp`, `lobby_factions.cpp`, `panels.cpp` | Lobby limits, extra-player faction handling and player panels. |
| `runtime/src/session.cpp`, `saveload.cpp`, `savelobby_watch.cpp` | Session and save/load behavior. |
| `runtime/src/gift.cpp`, `battle.cpp`, `telestration.cpp` | Battle unit lending and multiplayer battle UI behavior. |
| `runtime/src/offsets.h` | Game-version-specific addresses and structure offsets. |
| `runtime/src/build_mode.h`, `runtime/release-log-formats.json` | Release/debug boundaries and player log formats. |
| `runtime/tests/` | Synthetic runtime regressions. |
| `runtime/proxy/` | DLL forwarder that loads the runtime at game startup. |
| `mods/coop_4player_ui/`, `mods/coop_gift_panel/` | UI and art generators. |
| `tools/installer/` | Installer, transaction helper, local developer client and fixture tests. |
| `docs/pack-development.md` | Building the Workshop pack from local game inputs. |

The runtime changes game behavior in memory; it does not patch the game executable
on disk. Hook comments and `offsets.h` describe the ABI assumptions. Some source
comments cite historical research notes that are absent from this public tree;
those notes are not build dependencies.

## License

Project source is licensed under **GPL-3.0-only**. See [LICENSE](LICENSE).
The source repository excludes game assets and generated packs.

Maintainers: see [releases and CI](docs/releases.md) for test artifacts, the
Workshop item configuration and the version-tag draft release workflow.
