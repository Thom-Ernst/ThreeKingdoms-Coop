# Public Windows installer

Double-click `TW3K-Coop-Setup-<version>.exe`, allow Windows' administrator prompt,
and follow Setup. It searches Steam's libraries and preselects a validated game
folder. If several installations exist, choose yours; if none is found, use
Steam > right-click THREE KINGDOMS > Manage > Browse local files. The chosen
folder must contain `Three_Kingdoms.exe`. Close both the game and CA launcher.

The installer provides the DLL side. **Subscribe to
[`tw3k_coop` in Steam Workshop](https://steamcommunity.com/sharedfiles/filedetails/?id=3816463449)**,
then start the game's launcher, tick `tw3k_coop` under **Mods**, and
play. If the release includes a Workshop item ID, Setup offers its Workshop page
and reports whether that item is already downloaded in the game's Steam library.
Without an ID, it displays the subscription instructions without a link.
Setup does not edit `used_mods.txt`: the launcher rewrites it.

Steam updates or **Verify integrity of game files** can replace the mod's proxy.
If the mod stops loading, close the game and launcher and **re-run the same
Setup**. It keeps the latest Steam original and reinstalls the proxy. Remove
the mod through Windows **Apps & features > Three Kingdoms Coop**. Uninstall
restores/preserves Steam's original DLL, never touches Workshop files or saves,
and keeps `tw3k_coop_*.log` unless you tick **Also remove logs**.

The default player installer packages `runtime/tw3k_coop.dll`, the **release**
build. It has no developer control pipe and ships no recovery client.

Modders can build with `-Variant Debug` and use
`TW3K-Coop-Setup-<version>-debug.exe`. Its wizard and Apps & features entry say
**Three Kingdoms Coop Developer Build (Debug)**. It packages
`runtime/tw3k_coop_debug.dll` as `tw3k_coop.dll`, the filename the proxy loads,
and includes `TW3K-Coop-Control.ps1` and `TW3K-Coop-Recovery.cmd`.
See [DEBUG-BUILD.md](../../runtime/DEBUG-BUILD.md) for the developer facilities.
With debug installed, a player whose hidden decision blocks the turn can
double-click `TW3K-Coop-Recovery.cmd`. Only the player whose reply says
**MINE, this machine may answer it** can type a displayed option number;
Enter cancels, and the DLL checks ownership again. Windows' built-in PowerShell
runs this local client, with no rig inventory or remote controls.

Close the game and launcher and run either installer to switch variants in the
same game folder. Switching to release removes the debug clients. Switching to
debug adds them. Both use the same uninstall registration and preserve Steam's
original DLL; failed switches restore the previous DLL and clients.

The installer is unsigned. Windows SmartScreen may show a warning; after
checking that the download came from this project's public GitHub Release,
choose **More info > Run anyway**. There is no injector, background watcher,
scheduled task or antivirus exclusion added by Setup. If a file is quarantined,
Setup refuses verification and rolls back; inspect Windows Security's Protection
history before trying again.

## Offline build and verification

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/installer/Build-Installer.ps1 -Version 0.1.0
powershell -NoProfile -ExecutionPolicy Bypass -File tools/installer/Build-Installer.ps1 -Version 0.1.0 -Variant Debug
pwsh -NoProfile -File tools/installer/Test-Installer.ps1 -Installer out/installer/TW3K-Coop-Setup-0.1.0.exe -OtherInstaller out/installer/TW3K-Coop-Setup-0.1.0-debug.exe
powershell -NoProfile -ExecutionPolicy Bypass -File tools/installer/Test-Installer.ps1 -Installer out/installer/TW3K-Coop-Setup-0.1.0-debug.exe -OtherInstaller out/installer/TW3K-Coop-Setup-0.1.0.exe
```

Pass `-WorkshopItemId 3816463449` to link the project's Workshop item; CI reads
the same ID from the `WORKSHOP_ITEM_ID` repository variable. The item is currently
hidden while release preparation continues. Build requires Inno Setup 6
(`-ISCC` overrides its compiler path) and Windows' .NET Framework 4 compiler.
`-Variant` defaults to `Release`. `-DllPath` defaults to `runtime/tw3k_coop.dll`
for Release and `runtime/tw3k_coop_debug.dll` for Debug. An explicit path must
have the matching DLL mode in its file properties or the build refuses it.
The build reads that DLL's embedded
git stamp and records its SHA256. No `runtime/` code or binary is changed.
Each variant uses its own staging folder so both can share an output directory.
Each build produces one distributable EXE plus a companion hashes JSON for auditing;
publish the EXE as a public GitHub Release asset. Neither artifact embeds build
machine names or local build paths. Compiler timestamps mean rebuilds are not
promised to be byte-identical.

Setup defaults to administrator privileges because a default Steam installation
is under Program Files. `/CURRENTUSER` is an explicit command-line override for
writable game folders and offline fixture tests; it also places the uninstall
registration in HKCU. Normal players use the elevated wizard. All mod-owned
files, transaction snapshots and Inno uninstall executables live inside the
game folder; Inno uses its normal extraction/log temporary files and uninstall
registry entry. It writes no launcher settings, AppData or saves.

`/STEAMROOT=<folder>` overrides Steam discovery and disables registry/drive
discovery. Tests also pass `/FIXTUREROOT=<temp folder>` to reject targets and
library candidates outside that tree, plus a unique `/FIXTUREID` to isolate
uninstall registrations. With `/FIXTUREROOT`, the process guard checks only
matching processes inside the fixture tree, allowing offline tests while an
unrelated real game is running; normal installs still refuse every game/launcher
process. Test failures keep fixture files and logs for review.
Tests require `-OtherInstaller` for the opposite variant and exercise both switch
directions and failed switches alongside the full original fixture suite. Run each
variant as `-Installer` in both PowerShell editions. Never supply a real game
folder to tests.

## Proxy rules and independent updates

The positive identity marker is the `Tw3kCoopProxyMarker` export string, matching
`Install-TwProxy.ps1`. Read errors fail closed. Fresh installs copy the original
to `amd_ags_x64_orig.dll` before atomically replacing the active DLL. Re-runs
replace only the proxy. After Steam verification, the **current Steam DLL wins**:
the old saved original stays in a rollback snapshot until the new original and
proxy have passed SHA256 read-back plus a six-second antivirus recheck. Failures
restore snapshots; rerunning recovers a journal left by an interrupted Setup.
If our proxy has no valid saved original, verify game integrity in Steam first.

All seven obsolete packs from `Install-TwMod.ps1` are retired, together with
the old local `data/tw3k_coop.pack`. Setup reports each removed file. A local
copy alongside a Workshop copy makes pack resolution ambiguous; we have not
established exact same-name precedence offline, so Setup removes the local copy
and never modifies Workshop content. Setup also removes the two old test-aid packs, `skip_intro_movies.pack` and
`reveal_starting_map.pack`, and the exact old root helpers: `injector.exe`,
`Install-TwProxy.ps1`, `Invoke-TwControl.ps1`, `Watch-TwInject.ps1` and
`amd_ags_x64_proxy.dll`. Every removed path appears in the finish text and log;
failed installs restore these files. Uninstall leaves the entire `data` folder
alone, including a pack copied there after installation. Setup replaces the
uninstall log so an older installer cannot retain deletion entries for data packs. Other files are untouched.

The pack creates UI widgets and the DLL finds them by name. Independent updates
that disagree can leave missing lobby/gift controls, or controls the DLL no
longer recognizes. The installer cannot prove compatibility merely from a
Workshop directory existing. A small future runtime check could have the pack
provide a hidden `tw3k_coop_ui_api_<N>` widget and let the DLL log/show its expected
and observed UI API when it first binds the lobby. An absent or different marker
should explain which component to update. This proposal is not implemented here.
