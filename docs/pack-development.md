# Pack development

The two UI generators consume files extracted locally from your own game:

- `mods/coop_4player_ui/extracted/ui/frontend ui/mp_grand_campaign.twui.xml`
- `mods/coop_gift_panel/extracted/ui/battle ui/hud_battle.twui.xml`
- `mods/coop_gift_panel/assets/extracted/gift_recipient_{1,2,3}.png`

Vanilla XML, native textures, extracted badges and every generated `build/` tree
stay local. The badge images are withheld pending provenance review; supply your
own compatible transparent numbered images. No copyrighted input is in Git.

Run the lobby generator with `python mods/coop_4player_ui/dupe_panels.py --write`,
then its validator on the generated XML. Run `build_panel.py --live`, with
`--vanilla` and `--out` paths if needed, then apply `style_panel.py` to its output.
The icon script writes three 46-by-46 textures. Check each generator's arguments.
Assemble the resulting UI and texture trees under `mods/tw3k_coop/build/`; copy
the authored tooltip TSV into its `text/db/` directory. Keep this tree ignored.

With an RPFM Server running locally, run
`pwsh -File tools/Build-Packs.ps1 -Mods tw3k_coop -Uri http://localhost:45127/mcp`.
The builder imports TSV tables and creates the single pack. It makes no game
deployment. The result contains modified game material and is intended for the
Workshop distribution route, not the public source repository.

The generator source retains some references to private research documents for
historical explanation. Those documents are intentionally absent; the build
depends on the local inputs above, not on the research archive.
