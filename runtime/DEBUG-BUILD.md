# Release and debug builds

Open an x64 Native Tools command prompt with Visual Studio Build Tools and the
Windows SDK. `runtime\build.bat` builds the player `tw3k_coop.dll` and proxy;
`runtime\build.bat debug` builds `tw3k_coop_debug.dll`; `runtime\build.bat all`
builds both modes and the proxy. `runtime\build.bat test` runs offline regressions.
Windows file properties and startup logs identify RELEASE or DEBUG. Release source
identity contains product version and an optional short commit only, never a branch.

Release contains the automatic co-op fixes and player troubleshooting logs. The
`TW3K_RELEASE` compilation boundary removes the developer pipe, command dispatcher,
hotkeys, UI automation, memory probes, crash recorder and experimental patches.
There is no configuration flag that enables those capabilities in the player DLL.
The debug DLL adds those facilities; UI writes still require explicit arming.

Build installers with `tools/installer/Build-Installer.ps1` (release default) or
add `-Variant Debug`. Both DLLs load under the adjacent name `tw3k_coop.dll`.
Close the game and launcher and run the installer to switch modes. Release removes
the debug clients; debug includes `TW3K-Coop-Control.ps1` and the recovery CMD.
See [installer instructions](../tools/installer/README.md) for build requirements,
local recovery, installation, repair and offline fixture validation.

Release uses no PDB or absolute source paths. Debug is a developer build and should
be reviewed separately before distribution. The data pack remains on Steam Workshop.
