# Releases and CI

Pushes to `main`, pull requests and manual runs build both runtime modes and the
proxy on a Windows runner, run the offline runtime regressions, and package both
installers. Four fixture jobs exercise Release and Debug in PowerShell 7 and
Windows PowerShell 5.1. Build assets and test logs are retained for 14 days.
CI does not launch the game, build the Workshop pack or contact a test rig.

Use **Actions > Windows build and installers > Run workflow** to make a test
build. Supply a version such as `0.1.0-rc.1`. Manual runs create workflow
artifacts, not GitHub releases. The default branch build version is `0.1.0-ci`.

## Prepare a release

1. Land the reviewed fixes on public `main` and wait for CI to pass.
2. Upload the matching pack to Workshop. Set the repository Actions variable
   `WORKSHOP_ITEM_ID` to its numeric item ID under **Settings > Secrets and
   variables > Actions > Variables**. It is public configuration, not a secret.
   The current item is [3816463449](https://steamcommunity.com/sharedfiles/filedetails/?id=3816463449),
   and the repository variable is configured. The item is hidden during release
   preparation; make it accessible to players when the public release is ready.
3. Validate the intended build in a live multiplayer session. Check the changed
   paths, a subsequent turn or transition, and save/reload where relevant. Also
   check Steam integrity/repair, an upgrade from the old zip install, and
   uninstall with a manually added pack surviving removal.
4. Tag the reviewed public commit and push just that tag:

   ```text
   git tag v0.1.0
   git push origin v0.1.0
   ```

   Use a new version for each release. `v0.1.0-rc.1` is also accepted. Tags
   without a configured Workshop item ID fail before packaging.
5. The tag workflow repeats the builds and tests, then creates a **draft**
   GitHub Release with player and developer installers, their build manifests,
   a source ZIP and `SHA256SUMS.txt`.
6. Review the draft, add the Workshop link and version-specific changes/known
   issues, and publish it. Mark release candidates as prereleases.

The draft stage lets maintainers complete live checks and release notes before
making a download public. Rerunning a tagged workflow may replace assets on its
draft, but refuses to change assets on an already published release.

## Build inputs

GitHub action dependencies are pinned to commit hashes. Inno Setup 6.7.3 is
downloaded from its upstream release and checked against its SHA256. Runtime
compilation uses the Windows runner's MSVC and SDK; their versions appear in the
build log. Each installer manifest records the source commit, DLL stamp,
compiler version and input/output hashes. Timestamp fields mean rebuilt EXEs
are not guaranteed to be byte-identical.

Player releases are unsigned for now. No signing credential is configured.
Review changes to workflows and installer code as part of release review.
