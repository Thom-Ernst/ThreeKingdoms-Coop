#!/usr/bin/env python3
# Duplicate panel_player1 -> panel_player3 and panel_player2 -> panel_player4 in
# the 3K MP grand-campaign lobby UI layout, with full GUID remapping so nothing
# collides. Run with --write to produce the modified file; default is analyze-only.
import re, sys, uuid
from pathlib import Path

# Paths hang off the script's own folder, so moving this mod's directory cannot break them.
HERE = Path(__file__).resolve().parent
SRC = str(HERE / "extracted" / "ui" / "frontend ui" / "mp_grand_campaign.twui.xml")
FORCE = "--force-visible" in sys.argv
OUT = str(HERE / ("build_fv" if FORCE else "build") / "ui" / "frontend ui" / "mp_grand_campaign.twui.xml")

GUID = re.compile(r'\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{16}\b')

def difficulty_tooltip(source):
    # Resolve the active options subtree, not the hidden decorative slider.
    # Vanilla uses componentleveltooltip + tooltiplabel with tooltipslocalised.
    # The interactive handles intercept hover; give them and the label the same
    # help as the track. Native refresh changes geometry/text, not these attributes
    # (1.7.2 FUN_142D6E6F0, FUN_142D6B2F0); leave states and callbacks untouched.
    import xml.etree.ElementTree as ET
    root = ET.fromstring(source)
    label = root.find('hierarchy//mp_campaign_options/settings_parent/settings/'
                      'difficulty_settings/cmp_diff_tx')
    slider = label.find('difficulty_holder/cmp_diff_slider')
    targets = [label, slider, slider.find('handle'), slider.find('handle2')]
    loc = (HERE / 'localisation' / 'difficulty_tooltip.loc.tsv').read_text(encoding='utf-8')
    key, help_text, _ = loc.splitlines()[2].split('\t')
    for node in targets:
        guid = node.get('this')
        pattern = re.compile(r'^\t\t<' + node.tag + r'\n.*?^\t\t</' + node.tag + '>',
                             re.M | re.S)
        count = 0
        def update(match):
            nonlocal count
            block = match.group()
            if f'this="{guid}"' not in block.split('>', 1)[0]:
                return block
            count += 1
            for attr, value in [('componentleveltooltip', help_text), ('tooltiplabel', key)]:
                if re.search(r'\n\t\t\t' + attr + '=', block):
                    block = set_root_attr(block, attr, value)
                else:
                    block = insert_root_attr_after(block, 'tooltipslocalised', attr, value)
            return block
        source = pattern.sub(update, source)
        if count != 1:
            raise ValueError(f'Expected one difficulty tooltip target {node.tag}, found {count}')
    return source, loc

def new_guid(seed):
    h = uuid.uuid5(uuid.NAMESPACE_URL, seed).hex.upper()
    return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:32]}"

text = open(SRC, encoding="utf-8").read()
lines = text.split("\n")

hier_end = next(i for i, l in enumerate(lines) if l.strip() == "</hierarchy>")

def hier_subtree(tag):
    orx = re.compile(r'^(\t+)<' + tag + r' this=')
    for i in range(hier_end):
        m = orx.match(lines[i])
        if m:
            close = m.group(1) + "</" + tag + ">"
            for j in range(i + 1, hier_end):
                if lines[j] == close:
                    return i, j
    return None

# --- component-definition blocks (flat, indent-2), keyed by their 'this' GUID ---
comp0 = hier_end + 1
starts = [i for i in range(comp0, len(lines)) if re.match(r'^\t\t<[A-Za-z_]', lines[i])]
blocks = [(s, starts[k + 1] if k + 1 < len(starts) else len(lines)) for k, s in enumerate(starts)]
def block_this(a, b):
    m = GUID.search("\n".join(lines[a:b]))
    return m.group(0).upper() if m else None
guid_block = {}
for a, b in blocks:
    g = block_this(a, b)
    if g and g not in guid_block:
        guid_block[g] = (a, b)

def set_root_attr(text, attr, value):
    # replace the FIRST occurrence of a root attribute line (attr="...")
    return re.sub(r'(\n\t+' + attr + r'=")[^"]*"', lambda m: m.group(1) + value + '"',
                  text, count=1)

def insert_root_attr_after(text, after_attr, new_attr, value):
    # insert a new attribute line immediately after the first occurrence of `after_attr`,
    # matching its indentation. Used for attributes the vanilla panel does not carry at all.
    pat = re.compile(r'(\n(\t+)' + after_attr + r'="[^"]*")')
    return pat.sub(lambda m: m.group(1) + '\n' + m.group(2) + new_attr + '="' + value + '"',
                   text, count=1)

def build(src_tag, dst_tag, offset, docking, anchor, dock_offset):
    hs = hier_subtree(src_tag)
    if not hs:
        print(f"!! {src_tag} hierarchy subtree not found"); return None
    ha, hb = hs
    hier_guids = []
    for l in lines[ha:hb + 1]:
        for g in GUID.findall(l):
            g = g.upper()
            if g not in hier_guids:
                hier_guids.append(g)

    # owned = hierarchy GUIDs + any GUID declared inside their own def blocks
    owned = list(hier_guids)
    defs_ranges = []
    missing = 0
    for g in hier_guids:
        if g in guid_block:
            a, b = guid_block[g]
            defs_ranges.append((a, b))
            for l in lines[a:b]:
                for gg in GUID.findall(l):
                    gg = gg.upper()
                    if gg not in owned:
                        owned.append(gg)
        else:
            missing += 1

    remap = {g: new_guid(f'coop/lobby/{dst_tag}/{g}') for g in owned}

    def remap_text(t):
        # replace every owned GUID (case-insensitive) with its new value
        def rep(m):
            return remap.get(m.group(0).upper(), m.group(0))
        return GUID.sub(rep, t)

    # ---- new hierarchy subtree ----
    hblock = "\n".join(lines[ha:hb + 1])
    hblock = remap_text(hblock)
    hblock = hblock.replace("<" + src_tag + " ", "<" + dst_tag + " ")
    hblock = hblock.replace("</" + src_tag + ">", "</" + dst_tag + ">")

    # ---- new component defs (only the ones this panel owns) ----
    seen = set(); out_defs = []
    for g in hier_guids:
        if g in guid_block:
            a, b = guid_block[g]
            if (a, b) in seen:
                continue
            seen.add((a, b))
            db = "\n".join(lines[a:b]).rstrip("\n")
            db = remap_text(db)
            if g == remap_first_panel_guid(hier_guids):
                pass
            out_defs.append(db)
    defs_text = "\n".join(out_defs)
    # rename the panel's OWN def tag + id, and shift its offset
    defs_text = defs_text.replace("<" + src_tag + "\n", "<" + dst_tag + "\n", 1)
    defs_text = defs_text.replace("</" + src_tag + ">", "</" + dst_tag + ">", 1)  # close tag too
    defs_text = defs_text.replace('id="' + src_tag + '"', 'id="' + dst_tag + '"', 1)
    # reposition the panel root: centre-dock so it sits mid-screen on any aspect
    # ratio (incl. ultrawide), instead of hugging the left/right edge.
    defs_text = set_root_attr(defs_text, "offset", offset)
    defs_text = set_root_attr(defs_text, "docking", docking)
    defs_text = set_root_attr(defs_text, "component_anchor_point", anchor)

    # ★★★ THE PANELS WERE LANDING ON TOP OF THE VANILLA ONES (found 2026-07-30, session 5z).
    #
    # Reported from a filled lobby: "p3 UI was rendered above p1" and player 1's dropdowns stopped
    # working. Setting `offset` alone does nothing for a DOCKED widget — position comes from the dock
    # point, and `offset` is a derived/cached value. Both panel_player1 and our panel_player3 dock
    # "Bottom Left" with no displacement, so the clone lands exactly on the original and swallows its
    # mouse input. Same for panel_player4 over panel_player2.
    #
    # This is almost certainly the real cause of the oldest open symptom too, "player 1's faction
    # controls became unresponsive" (5r) — an overlapping panel intercepts input whether or not it is
    # visible, which is why that symptom appeared even when player 3's panel was not being drawn.
    #
    # Vanilla expresses "docked, but displaced" with `dock_offset`: 112 of the 127 docked widgets in
    # this layout carry one (e.g. id="bottom" docking="Bottom Left" dock_offset="7.00,13.00").
    # panel_player1/2 omit it only because they sit exactly at their corners — 0 is the bottom-left,
    # and 1664 = 1920-256 is the bottom-right. So the clones need an explicit one.
    #
    # ⚠ The exact arithmetic of dock_offset is inferred, not proven: for `id="top"` it equals `offset`,
    # while for `id="bottom"` the two differ, so the sign convention for Bottom/Right docking is not
    # fully pinned down. If the panels come out displaced the wrong way or off-screen, flip the sign
    # here — it is one number per panel and needs no other change.
    defs_text = insert_root_attr_after(defs_text, "docking", "dock_offset", dock_offset)

    # ---- ❌ DO NOT strip the clones' ContextPropagator callbacks (tried in 5t, reverted in 5u) ----
    #
    # What stands: `button_start_campaign_holder` decides its own visibility with
    # `ContextVisibilitySetter / CcoFrontendFactionLeader / IsValidContext`, so the START button is
    # shown if and only if it holds a valid faction-leader context, delivered by a player panel's
    # `ContextPropagator ... GetIf(self.IsLocalPlayer, this)`. That is why a ready mask of "NOTHING
    # BLOCKING" and a missing start button are both true at once — unrelated systems.
    #
    # What was wrong: 5t assumed the three non-local panels would overwrite the real player's context
    # on those globally-named widgets, and stripped every target-bearing propagator from the clones.
    # The user's own observation kills it:
    #
    #   * the HOST never lost its start button, with four panels present throughout — so a non-local
    #     panel propagates NOTHING rather than propagating something invalid. No clobber exists.
    #   * the machine with no start button is PLAYER 3, and on player 3's machine panels 1 and 2 are
    #     non-local, so the only panel that can give the button a valid context is panel_player3 —
    #     a CLONE. Stripping its propagator guarantees player 3 never gets a button.
    #
    # And player 3's panel was never drawn either, so the two are not independent observations: both
    # follow from panel_player3 never holding a valid faction-leader context. That one fact makes
    # ContextVisibilitySetter hide the panel AND leaves the propagator with nothing to pass on.
    # FUN_142D62DA0 sets that context on the panel widget; the session 5s DLL fix calls it. No data
    # change is needed here — leave these callbacks alone.

    # ---- ALWAYS: a clone must not need a faction in order to become visible ----------------
    #
    # Run 7 exposed a second circular dependency, one layer above run 6's. A player panel carries
    # `ContextVisibilitySetter / CcoFrontendFactionLeader / IsValidContext`, so it is visible only
    # once it holds a valid faction-leader context — and the only thing that sets that context is
    # FUN_142D62DA0, which runs when a faction is chosen. But the faction dropdown lives INSIDE the
    # panel. So:
    #
    #     panel hidden until player 3 has a faction
    #       -> the dropdown they would pick it with is inside the hidden panel
    #         -> they cannot pick a faction
    #           -> the panel stays hidden
    #
    # In vanilla this never bites: panels 1 and 2 belong to players who are seated before the lobby
    # is interactive. It only appears for a cloned panel that has to come into existence mid-lobby.
    # In run 7 the loop was broken from outside by the F8 hotkey, and the log shows player 3 with no
    # panel for 82 seconds between joining (12:11:38) and F8 (12:13:00).
    #
    # Removing the gate from the CLONES lets their panels render as soon as they exist, so player 3
    # can use the ordinary dropdown and F8 stops being required. The vanilla panels keep theirs, so
    # players 1 and 2 behave exactly as before.
    #
    # ❌❌ TRIED IN 5x, REVERTED IN 5y — DO NOT REMOVE THIS GATE FROM THE CLONES.
    #
    # Stripping `ContextVisibilitySetter` from the clone roots does make them render without a faction,
    # but it breaks the lobby, confirmed on screen: in a lobby holding only player 1, a clone panel
    # rendered PLAYER 1's flag on the opposite side of the screen, drew over the character art, and
    # player 1's faction dropdowns stopped working.
    #
    # ★ THE MECHANISM: contexts INHERIT DOWN THE WIDGET HIERARCHY. A panel that owns no context of its
    # own displays its ANCESTOR's — which is player 1's. `ContextVisibilitySetter /
    # CcoFrontendFactionLeader / IsValidContext` is precisely the guard that stops an unoccupied panel
    # from rendering someone else's context. It is load-bearing, not decoration.
    #
    # ★ It also retro-explains the oldest unexplained symptom, "player 1's faction controls became
    # unresponsive" (5r): same cause, a visible clone panel that does not own a valid context.
    #
    # The real problem this was meant to solve — a clone panel is hidden until it has a faction, and the
    # faction dropdown lives inside it — must be solved from the DLL by giving the panel a genuine
    # context (assign the joining player a faction, as F8 does), NOT by removing the guard.

    if FORCE:
        # 1) remove the context-driven visibility gate (self-closing element)
        before = defs_text.count("ContextVisibilitySetter")
        defs_text = re.sub(
            r'[ \t]*<callback_with_context\s+callback_id="ContextVisibilitySetter"[^>]*/>\s*\n',
            '', defs_text)
        # 2) OnCreate/FadeIn start these panels fully transparent (alpha 00) and
        #    rely on a game-triggered fade-in that never fires for empty slots.
        #    Force fully-opaque white so they render immediately.
        alpha = defs_text.count('#FFFFFF00')
        defs_text = defs_text.replace('#FFFFFF00', '#FFFFFFFF')
        print(f"  [force-visible] {dst_tag}: removed {before} ContextVisibilitySetter, "
              f"fixed {alpha} transparent-alpha frames")

    return dict(src=src_tag, dst=dst_tag, hstart=ha, hend=hb,
                hier_guids=len(hier_guids), owned=len(owned), missing=missing,
                hblock=hblock, defs_text=defs_text, ndefs=len(out_defs))

def remap_first_panel_guid(hier_guids):
    return hier_guids[0] if hier_guids else None

def shift_offset(defs_text, dx):
    # shift the FIRST offset="x,y" (the panel root's) by dx in x
    m = re.search(r'offset="(-?\d+\.\d+),(-?\d+\.\d+)"', defs_text)
    if not m:
        return defs_text
    x = float(m.group(1)) + dx
    return defs_text[:m.start()] + f'offset="{x:.2f},{m.group(2)}"' + defs_text[m.end():]

# ---------------------------------------------------------------- layout
#
# Vanilla geometry (read from the panel defs, design space is 1920x1080):
#   panel ROOT     = 256 x 334
#   panel_player1  offset  0.00,746.00   docking "Bottom Left"   anchor 0.00,1.00
#   panel_player2  offset 1664.00,746.00 docking "Bottom Right"  anchor 1.00,1.00
# 1664 = 1920 - 256, i.e. each vanilla panel's ROOT hugs its screen corner exactly.
#
# The first attempt re-docked the new panels to "Bottom Center" with a 0.50 anchor
# and they came out overlapping — anchor semantics differ from what was assumed.
# So: DO NOT change docking or anchor. Keep each copy on the SAME docking as the
# panel it was cloned from and move it inboard along that same axis. Then a copy
# positions identically to a panel already known to render correctly, and the only
# variable is one number.
#
#   [P1][P3] .... map .... [P4][P2]
#
# Panels 1 and 2 are never touched, so the vanilla layout is unchanged if the
# extra panels are never populated.
#
# ★★★ 5z's dock_offset IS CORRECT AND PROVEN, THE DISTANCE WAS WRONG (found 2026-07-30, session 6f).
#
# A 3-player screenshot settled both halves at once. `dock_offset` really is what moves a docked
# widget, and +x really is rightward under "Bottom Left" docking: panel_player3 landed exactly 256
# to the right of panel_player1, as asked. The clone is no longer *on* the original.
#
# But it still overlapped it, because 256 is the width of the panel ROOT, not of the panel. The art
# and the frame live in `character_holder_left`, which is 502 wide:
#
#   panel_player1's copy   offset   0.00,-370.00  docking "Bottom Left"   502 x 704
#   panel_player2's copy   offset -246.00,-370.00 docking "Bottom Right"  502 x 704
#
# so a panel's real footprint is 502 px: P1 covers 0..502 and P2 covers 1418..1920. Displacing a
# clone by 256 therefore buries 246 px of the panel it sits beside — which is precisely what the
# screenshot shows, with player 1's two faction dropdowns clipped. (The dropdown template is 254
# wide at panel-x 40, so it runs to 294 and a 256 displacement cuts it.)
#
# Four 502-wide panels do not fit in 1920, so space them evenly instead: gap = (1920-502)/3 = 472.67,
# giving art at 0 / 472.67 / 945.33 / 1418. Adjacent panels overlap by 29 px of character art, and
# every interactive widget stays clear — the clones are later siblings, so they draw on top, and the
# widest control (a dropdown, ending at panel-x 294) is well inboard of the next panel's edge.
PANEL_W     = 256.0     # the ROOT width — what `offset` is expressed against
PANEL_VIS_W = 350.0     # compact_layout.py character holder width
DESIGN_W    = 1920.0
GAP         = PANEL_VIS_W + 40.0     # pair inner panels with outer panels; was 490px

# `offset` is kept in step with the intended position for readability, but for a docked widget it is
# `dock_offset` that actually moves it.
p3 = build("panel_player1", "panel_player3",
           offset=f"{GAP:.2f},746.00",                          # 390 — right of P1
           docking="Bottom Left",  anchor="0.00,1.00",
           dock_offset=f"{GAP:.2f},0.00")                       # inboard from the left edge
p4 = build("panel_player2", "panel_player4",
           offset=f"{DESIGN_W - PANEL_W - GAP:.2f},746.00",     # left of P2
           docking="Bottom Right", anchor="1.00,1.00",
           dock_offset=f"{-GAP:.2f},0.00")                      # inboard from the right edge

for p in (p3, p4):
    if p:
        print(f"{p['src']} -> {p['dst']}: hier rows {p['hstart']+1}-{p['hend']+1}, "
              f"{p['hier_guids']} hierarchy GUIDs, {p['owned']} owned GUIDs, "
              f"{p['ndefs']} component defs copied, {p['missing']} missing defs")

if "--write" in sys.argv and p3 and p4:
    out = lines[:]
    # insert new hierarchy subtrees right after panel_player2's hierarchy close
    p2h = hier_subtree("panel_player2")
    ins_h = p2h[1] + 1
    new_hier = (p3["hblock"] + "\n" + p4["hblock"]).split("\n")
    out[ins_h:ins_h] = new_hier
    # recompute component insertion point (after shift) — insert at end of the
    # components list, right before the final root close tag(s).
    # Find the last line index of the components (end of file minus trailing).
    # We append the new defs just before the last two closing lines.
    text2 = "\n".join(out)
    # insert defs before the final "</...>" pair that closes hierarchy root/layout
    marker = "\n\t</components>" if "\t</components>" in text2 else None
    add = "\n" + p3["defs_text"] + "\n" + p4["defs_text"]
    if marker:
        text2 = text2.replace(marker, add + marker, 1)
    else:
        # fallback: append before last closing tag
        idx = text2.rstrip().rfind("\n</")
        text2 = text2[:idx] + add + text2[idx:]
    from compact_layout import compact_layout
    text2 = compact_layout(text2)
    text2, loc = difficulty_tooltip(text2)
    import os
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w", encoding="utf-8", newline="\n").write(text2)
    loc_out = HERE / ("build_fv" if FORCE else "build") / 'text' / 'db' / 'difficulty_tooltip.loc.tsv'
    loc_out.parent.mkdir(parents=True, exist_ok=True)
    loc_out.write_text(loc, encoding='utf-8', newline='\n')

    # validation report
    allg = GUID.findall(text2)
    from collections import Counter
    dupes = [g for g, c in Counter(x.upper() for x in allg).items() if c > 2]
    print("WROTE", OUT)
    print("total GUID occurrences:", len(allg))
    print("GUIDs appearing >2 times (each should appear exactly twice: hierarchy+def):",
          len(dupes), dupes[:10])
    for tag in ("panel_player1","panel_player2","panel_player3","panel_player4"):
        print(f"  <{tag} count:", text2.count("<"+tag+" ") + text2.count("<"+tag+"\n"))

    # ---- checks derived from the exe RE (see ../../NETCODE_NOTES.md session 5c) ----
    #
    # These are not cosmetic. Each one is a thing the game code actually does, and
    # if the duplicate fails it the panel silently never populates.
    ok = True

    # 1. FUN_142D72090 populates a panel by scanning that widget's callback list for
    #    the "MPCampaignPlayer" callback. A copy without it is inert.
    for tag in ("panel_player3", "panel_player4"):
        import xml.etree.ElementTree as ET
        component = ET.fromstring(text2).find(f'components/{tag}')
        if component is None:
            print(f"  !! {tag}: definition block not found"); ok = False; continue
        blk = ET.tostring(component, encoding='unicode')
        if 'callback_id="MPCampaignPlayer"' in blk:
            print(f"  OK {tag}: MPCampaignPlayer callback present (populator can find it)")
        else:
            print(f"  !! {tag}: MPCampaignPlayer callback MISSING — panel would never populate")
            ok = False

    # 2. FUN_142D45230 looks up "faction_group_dropdown"/"faction_dropdown" as CHILDREN
    #    of each panel; without them that player gets no faction UI.
    for tag in ("panel_player3", "panel_player4"):
        h = re.search(r'^(\t+)<' + tag + r' this=(.*?)^\1</' + tag + r'>',
                      text2, re.S | re.M)
        if not h:
            print(f"  !! {tag}: hierarchy subtree not found"); ok = False; continue
        sub = h.group(0)
        miss = [w for w in ("faction_dropdown", "faction_group_dropdown", "dy_player_name")
                if "<" + w + " " not in sub]
        if miss:
            print(f"  !! {tag}: missing child widgets {miss}"); ok = False
        else:
            print(f"  OK {tag}: faction dropdowns + name label present")

    # 3. FUN_140561000 resolves panels by a GLOBAL name hash and then checks the hit
    #    is under the lobby root, so the id must be exact and unique.
    for tag in ("panel_player3", "panel_player4"):
        n = text2.count('id="' + tag + '"')
        print(("  OK " if n == 1 else "  !! ") + f"{tag}: id=\"{tag}\" appears {n}x (want exactly 1)")
        if n != 1:
            ok = False

    print("RE-DERIVED CHECKS:", "ALL PASSED" if ok else "FAILED — do not ship this build")
    if not ok:
        sys.exit(1)
