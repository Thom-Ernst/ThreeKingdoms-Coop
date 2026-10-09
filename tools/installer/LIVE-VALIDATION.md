# Live installer acceptance check

Use the release candidate built by public CI, on the Windows Steam version of
THREE KINGDOMS 1.7.2. The offline fixture suite must pass first. Do not point
`Test-Installer.ps1` at a real game folder: live validation uses the normal wizard.

Record the installer version, public source commit, Windows version, selected
Steam library, Workshop pack version and results below. Keep Setup's log and
the runtime log from the launch. Review logs before sharing them publicly.

1. Close the game and Creative Assembly launcher. Back up existing mod DLLs and
   any local packs that Setup identifies as legacy files. Record their hashes.
2. Run the player installer through its normal elevated wizard. Confirm that
   Steam discovery finds the intended game folder, the summary lists the intended
   changes, and completion links to Workshop item `3816463449`.
3. Subscribe to the Workshop item using an account with access to the hidden
   page. Enable `tw3k_coop` in the launcher. Confirm the pack was downloaded and
   that the runtime log records the expected release build.
4. Launch a cooperative campaign with matching versions on each participant.
   Confirm lobby controls, turn blending and battle unit lending, then advance
   a turn and check save/reload. Record player count and any failures.
5. Close the game and launcher. Verify game integrity in Steam. Run Setup again
   to repair the proxy, then confirm that the game and mod load again.
6. Put a disposable, clearly named test pack in `data/` after installation.
   Uninstall Three Kingdoms Coop through Windows Apps & features. Confirm the
   Steam original DLL is restored, the disposable pack and Workshop content
   remain, and a launch with the Workshop mod disabled succeeds. Remove the
   disposable test pack yourself after checking preservation.
7. If upgrading an older zip installation is part of this release, repeat using
   that supported starting state and verify the reported legacy cleanup. Restore
   any desired local files from the backup after the check.

The Steam verification step can download files. Run it outside an active play
session. Mark unavailable scenarios as untested; passing offline fixtures alone
does not establish live Steam or campaign acceptance.
