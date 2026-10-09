# Pack development

`mods/tw3k_coop/` is the single mod source folder. Its lobby and battle generators
produce different UI files in one shared `build/` tree, which becomes
`tw3k_coop.pack`. They are components of the same Workshop item.

Extract these inputs from your own game installation:

- `mods/tw3k_coop/extracted/ui/frontend ui/mp_grand_campaign.twui.xml`
- `mods/tw3k_coop/extracted/ui/battle ui/hud_battle.twui.xml`

Supply compatible transparent numbered badge images locally at
`mods/tw3k_coop/assets/extracted/gift_recipient_{1,2,3}.png`. The original badge
images are withheld pending provenance review. Vanilla XML, textures, badge
inputs and generated output stay local and ignored.

From the repository root, generate both UI components and badge textures:

```powershell
python mods/tw3k_coop/dupe_panels.py --write
python mods/tw3k_coop/validate.py
python mods/tw3k_coop/build_panel.py --live --vanilla "mods/tw3k_coop/extracted/ui/battle ui/hud_battle.twui.xml" --out "mods/tw3k_coop/build/ui/battle ui/hud_battle.twui.xml"
pwsh -File mods/tw3k_coop/Build-IconTextures.ps1
```

The lobby generator applies `compact_layout.py` and writes the authored
`localisation/difficulty_tooltip.loc.tsv` into `build/text/db/`. The live battle
generator applies `style_panel.py` automatically. No copying between mod folders
is needed. Run battle UI tests with `python mods/tw3k_coop/test_style_panel.py`;
these require the local battle XML.

With an RPFM Server running locally, build the combined pack:

```powershell
pwsh -File tools/Build-Packs.ps1 -Mods tw3k_coop -Uri http://localhost:45127/mcp
```

The builder imports the tooltip TSV as a localisation table and produces
`mods/tw3k_coop/tw3k_coop.pack`. It makes no game deployment. The result contains
modified game material and belongs on Workshop, outside the source repository.

The generator source retains some references to private research documents for
historical explanation. Those documents are intentionally absent; the build
depends on the local inputs above, not on the research archive.
