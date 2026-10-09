#!/usr/bin/env python3
"""
Build the placeholder gift panel into ui/battle ui/hud_battle.twui.xml.

This is the PACK half of the gift UI. The exe half is finished and proven (NETCODE_NOTES §6ss.1):
the DLL adds ten named entries to the CcoBattleRoot property table and the UI can reach them by
name. All this file does is put five buttons on screen that call them.

    CoopGiftToPlayer0..3   action   gift the current selection to that player
    CoopCloseGiftPanel     action   dismiss, gift nothing
    CoopGiftPanelWanted    query    true from the gift click until a target is chosen
    CoopCanGiftToPlayer0..3 query   whether that button should be live

★ DELIBERATELY CRUDE. runtime/GIFT_UI.md says the placeholder should be ugly and disposable, and a UI
person is expected to replace it. No widget id here is load-bearing — the DLL never touches a
widget — so this whole file can be deleted without a DLL rebuild.

WHY A SCRIPT AND NOT HAND-EDITED XML
    twui.xml is a GUID-keyed hierarchy of 28,758 lines, and this repo's own history says clone
    rather than author (see mods/tw3k_coop/dupe_panels.py, and the four twui facts in
    wiki/packs-and-ui.md). Everything below is a pure INSERTION — one block before </hud_battle>
    and one before </components> — so no existing byte is touched and the diff is reviewable.

THE TWO MARKUP SHAPES, BOTH TAKEN FROM VANILLA IN THIS SAME FILE
    query  (visibility):  no context_object_id at all, context_function_id="BattleRoot.<Prop>"
                          - vanilla does exactly this 5 times for "BattleRoot.IsDynastyMode == false"
    action (click):       context_object_id="CcoStaticObject" context_function_id="BattleRoot.<Action>"
                          - vanilla's auto_lose_btn does exactly this for BattleRoot.DevKillEntireArmy

    Neither shape is guessed. That mattered: "derive internal names from the pack, never from
    user-visible wording" has cost this project a wrong conclusion twice.

⚠ THE FIRST ATTEMPT USED context_object_id="CcoBattleRoot" AND IT DOES NOT WORK HERE.
    That form is real - vanilla uses it at line 7667 for IsMultiplayer - but it requires the widget
    to HAVE a CcoBattleRoot context in scope, and contexts are propagated per-subtree (that is what
    ContextInitBattleRoot does on specific widgets). This panel is a bare child of hud_battle with
    no context propagated to it, so the expression could never evaluate: the panel stayed hidden and
    nothing was ever queried.

    One cause, both symptoms. It explains the vanilla-bound diagnostic build not appearing AND the
    DLL's counter reading zero queries - which had looked like evidence that the engine could not
    reach our entries at all. It could; nothing ever asked it to.

    ⇒ Copying a shape from vanilla is not enough on its own. The shape has to come from a widget in
    the same POSITION - vanilla's line-7667 widget sits under something that established the
    context, and this one does not.

TEMPLATES CLONED
    dev_button_list  @28342  the container (a List LayoutEngine, sizetocontent)
    auto_lose_btn    @28369  the button    (rectangular backplate, 5 states)
    button_txt       @28657  the label

Usage:
    python build_panel.py --vanilla <extracted hud_battle.twui.xml> --out <built hud_battle.twui.xml>
"""

import argparse
import hashlib
import sys
from pathlib import Path

# --------------------------------------------------------------------------------------------
#  Configuration
# --------------------------------------------------------------------------------------------

# One button per entry: (widget suffix, the action to call, the label to draw).
BUTTONS = [
    ("p0",     "CoopGiftToPlayer0",  "Give to player 1"),
    ("p1",     "CoopGiftToPlayer1",  "Give to player 2"),
    ("p2",     "CoopGiftToPlayer2",  "Give to player 3"),
    ("p3",     "CoopGiftToPlayer3",  "Give to player 4"),
    ("cancel", "CoopCloseGiftPanel", "Cancel"),
]

# The DLL's own can-gift query, per recipient. The cancel button is always live.
CAN_QUERY = {
    "p0": "CoopCanGiftToPlayer0",
    "p1": "CoopCanGiftToPlayer1",
    "p2": "CoopCanGiftToPlayer2",
    "p3": "CoopCanGiftToPlayer3",
}

PANEL_ID = "coop_gift_panel"

# --------------------------------------------------------------------------------------------
#  Diagnostic mode  (--diagnostic)
# --------------------------------------------------------------------------------------------
#
# Run 28 answered "did the UI ever ask for our entries?" with a flat NO — zero queries, on a
# machine where the pack was verifiably installed and enabled. That is one of two things, and the
# counter cannot tell them apart:
#
#   (a) the UI cannot REACH the DLL's new entries, or
#   (b) the panel is not in the UI tree at all, because this file's markup is wrong.
#
# Diagnostic mode swaps every one of our names for a VANILLA CcoBattleRoot member, changing nothing
# else. Vanilla names are guaranteed present in the table since startup, so:
#
#   panel appears   -> the markup, the hierarchy placement, the pack and the load order are all fine,
#                      and (a) is the answer: the problem is entry RESOLUTION, not the panel.
#   panel missing   -> (b): the markup is wrong and no amount of DLL work would have helped.
#
# IsMultiplayer is a query on CcoBattleRoot (FUN_1402B22D0 registers it 13th) and is true in any
# MP battle. ToggleCinematicMode is an ACTION on the same table with a visible effect, so it tests
# the click path as well as the visibility path.
DIAG_VISIBILITY = "IsMultiplayer"

# --------------------------------------------------------------------------------------------
#  Probe mode  (--probe)   — for plan D
# --------------------------------------------------------------------------------------------
#
# Visibility on a VANILLA query (so the panel is unconditionally on screen and cannot itself be the
# thing that fails), and every button on the HIJACKED entry with a DIFFERENT argument.
#
# ★ The arguments are deliberately NOT 0,1,2,3. A zero is what an argument that never arrived would
# also look like, so a button passing 0 proves nothing. 3/5/7/9/11 are values nothing else produces,
# which makes the log unambiguous about whether the value travelled.
PROBE_VISIBILITY = "IsMultiplayer"
PROBE_ACTION     = "GiftUnits"                 # what the DLL renames DevCycleArmy to
PROBE_ARGS       = ["3", "5", "7", "9", "11"]
DIAG_ACTION     = "ToggleCinematicMode"

# --------------------------------------------------------------------------------------------
#  Live mode  (--live)   — plan D, for real
# --------------------------------------------------------------------------------------------
#
# The probe answered its question: three buttons passing 3, 7 and 9 produced exactly those numbers
# in the DLL, with error=0 (§6vv.3, run 23:52). So the live pack is the probe with real player ids.
#
# ★ EVERY NAME HERE IS REACHABLE, which is the whole point and the thing plan B got wrong:
#
#   GiftUnits(id)   the HIJACKED entry — an 8-byte write inside the vanilla array the UI scans
#   IsMultiplayer   vanilla, registered 13th by FUN_1402B22D0
#
# ✗ The `Coop*` names are NOT used, in either mode. They are plan-B entries in a copied table the
#   UI never consults, so a button bound to one is a button that does nothing. That is what
#   CoopGiftPanelWanted was for, and it is why this panel is gated on IsMultiplayer instead: it is
#   on screen for the whole battle, which is crude but REACHABLE. Replacing it with a
#   selection-driven gate needs a vanilla query read out of the builder — not one guessed from
#   Warhammer 3's documentation (§6uu).
#
# ⇒ No cancel button in live mode: nothing raises or lowers the panel, so there is nothing to cancel.
# ⇒ No per-recipient greying either. CoopCanGiftToPlayerN is equally unreachable, and the honest
#   substitute costs a SECOND hijacked entry. Until then the DLL refuses an illegal recipient and
#   says so in the log, which is the same answer one move later.
LIVE_VISIBILITY = "GiftPanelOpen"              # ★ the SECOND hijacked entry, a query (entry 63)
LIVE_ACTION     = "GiftUnits"
LIVE_ARGS       = ["0", "1", "2", "3"]         # ★ the player id, exactly as the DLL reads it

# ★★ The panel hangs off the vanilla gift button rather than floating in the middle of the screen.
#
# Two separate mechanisms, both copied from the designer's pack rather than invented:
#
#   1. the panel is listed in the hierarchy INSIDE `button_gift_unit`, and carries the
#      `dynamic_child` / `dynamic_child_parent` user properties that re-parent it there;
#   2. `docking="Top Center External"` floats it above that button instead of inside it.
#
# ⚠ `offset` is INERT for a docked widget (wiki/packs-and-ui.md fact 1), which is why the position
# is expressed as dock_offset + anchor point. The numbers are theirs, and they are on screen in a
# shipped pack, so they are measured rather than guessed.
# ★ Per-recipient greying, which only became affordable with plan E. Plan D had two entries in the
# whole world and both were spent; a fallback name costs nothing, so each button can ask whether its
# recipient can actually receive.
#
# This is the fix for the 01:37 confusion: gifting to the third human needed the button labelled
# "player 4", because a save lobby seats players into slots 0/1/3 and these ids ARE slots. With the
# illegal buttons inactive, the numbering stops being something anyone has to reason about.
# ⚠ The labels count from ZERO, deliberately, because the argument the button passes IS the engine's
# player id and every log line uses that number. "Give to player 1" meaning id 0 is exactly the
# ambiguity that made the 01:37 run confusing to read.
LIVE_LABELS = ["Give to player 0", "Give to player 1", "Give to player 2", "Give to player 3"]

LIVE_CAN_QUERY = {
    "p0": "CanGiftToPlayer0",
    "p1": "CanGiftToPlayer1",
    "p2": "CanGiftToPlayer2",
    "p3": "CanGiftToPlayer3",
}

# ★ The player's NAME on the button, instead of a number nobody can map to a person.
#
# ⚠ THIS SHAPE HAS NO VANILLA PRECEDENT, and that is stated here rather than discovered later. All
# 25 `ContextTextLabel` bindings in the vanilla battle HUD carry a `context_object_id`; the only
# callbacks that use the bare `BattleRoot.<Prop>` form our fallback entries need are
# `ContextVisibilitySetter` (7) and `ContextCommandLeftClick` (2) — which are exactly the two shapes
# plan E has already been proven with. So a string binding may simply never be called, the way the
# ternary state binding never was.
#
# ⇒ Deliberately FAIL-VISIBLE: the static text= stays on the widget. If the binding is dead the
# button still reads "Give to player 2"; if it works it reads the person's name. A binding that
# failed to a BLANK button would be the failure that looks like success.
LIVE_NAME_QUERY = {
    "p0": "PlayerName0",
    "p1": "PlayerName1",
    "p2": "PlayerName2",
    "p3": "PlayerName3",
}

LIVE_PARENT_ID     = "button_gift_unit"
LIVE_PARENT_GUID   = "9D7BC631-71FA-4D4C-84EE278EB4DDB2C2"
LIVE_DOCKING       = "Top Center External"
LIVE_DOCK_OFFSET   = "99.00,-7.00"
LIVE_ANCHOR        = "0.50,1.00"

# ★★★ `update_constant` is the whole reason the panel can ever appear.
#
# Run 28 found our queries answered once when the panel bound and never again — which for a
# visibility binding means a panel that is stuck at whatever it read at bind time. This property is
# what makes the engine re-evaluate the binding, and it is in the designer's pack on exactly the
# callback we are copying. Without it the DLL can raise GiftPanelOpen all it likes and nothing moves.
LIVE_UPDATE_CONSTANT = True

# Reused verbatim from auto_lose_btn. A component_image is a shared resource referenced by GUID
# from each state's imagemetrics, so pointing at vanilla's own dev backplate costs us no art and
# no new image entry.
VANILLA_BACKPLATE = "ui/skins/default/dev/button_rectangular_backplate.png"


# --------------------------------------------------------------------------------------------
#  GUIDs
# --------------------------------------------------------------------------------------------

def guid(seed: str) -> str:
    """
    A deterministic GUID in Total War's format: 8-4-4-16 hex, 32 digits total.

    Deterministic on purpose - rebuilding this pack must produce byte-identical output, or every
    rebuild looks like a change in review. Seeded with a namespace so we cannot collide with a
    vanilla GUID by accident.
    """
    h = hashlib.sha256(("coop_gift_panel/" + seed).encode()).hexdigest().upper()
    return f"{h[0:8]}-{h[8:12]}-{h[12:16]}-{h[16:32]}"


def state_guids(prefix: str) -> dict:
    """The five button states plus their per-state image metric entries."""
    names = ["active", "down", "down_off", "hover", "inactive"]
    out = {n: guid(f"{prefix}/state/{n}") for n in names}
    out.update({f"img_{n}": guid(f"{prefix}/img/{n}") for n in names})
    return out


# --------------------------------------------------------------------------------------------
#  Emitters
# --------------------------------------------------------------------------------------------

def emit_hierarchy(indent: str, buttons=None) -> str:
    """The GUID-only tree entry that goes inside <hud_battle>."""
    i, i2, i3 = indent, indent + "\t", indent + "\t\t"
    lines = [f'{i}<{PANEL_ID} this="{guid("panel")}">']
    for suffix, _action, _label in (buttons if buttons is not None else BUTTONS):
        lines.append(f'{i2}<coop_gift_btn_{suffix} this="{guid(f"btn/{suffix}")}">')
        lines.append(f'{i3}<coop_gift_txt_{suffix} this="{guid(f"txt/{suffix}")}"/>')
        lines.append(f'{i2}</coop_gift_btn_{suffix}>')
    lines.append(f'{i}</{PANEL_ID}>')
    return "\n".join(lines) + "\n"


def emit_panel(indent: str, visibility: str = "CoopGiftPanelWanted", anchored: bool = False) -> str:
    """
    The container. Cloned from dev_button_list, with two changes that matter:

      - is_dev_only is DROPPED. That flag is why the dev list never appears in a retail build.
      - visible="false" stays, and a ContextVisibilitySetter raises it. `visibility` names the query
        that decides: a VANILLA one (IsMultiplayer) in probe and live builds, because the DLL's own
        CoopGiftPanelWanted sits in a table the UI never reads.

    docking/dock_offset rather than offset: wiki/packs-and-ui.md fact 1 - offset is INERT for a
    docked widget, and clones positioned with it landed on top of the originals and swallowed
    their mouse input.

    `anchored` hangs the panel off the vanilla gift button instead of the screen centre: the
    dynamic_child user properties re-parent it, and Top Center External floats it above the button.
    """
    i = indent
    g = guid("panel")
    s = guid("panel/state")
    docking = LIVE_DOCKING if anchored else "Center"
    offset  = LIVE_DOCK_OFFSET if anchored else "0.00,0.00"
    anchor  = LIVE_ANCHOR if anchored else "0.50,0.50"

    # The re-parenting properties, and the refresh property on the visibility binding. Both are
    # lifted from the designer's shipped pack rather than guessed - see the Live mode note above.
    reparent = ""
    if anchored:
        reparent = (
            f'{i}\t<userproperties>\n'
            f'{i}\t\t<property\n{i}\t\t\tname="dynamic_child"\n{i}\t\t\tvalue="1"/>\n'
            f'{i}\t\t<property\n{i}\t\t\tname="dynamic_child_parent"\n'
            f'{i}\t\t\tvalue="{LIVE_PARENT_ID}"/>\n'
            f'{i}\t</userproperties>\n'
        )
    # Self-closing unless there is a refresh block to carry, so the older builds still reproduce
    # byte for byte and only the live pack changes shape.
    if anchored and LIVE_UPDATE_CONSTANT:
        visibility_cb = (
            f'{i}\t\t<callback_with_context\n'
            f'{i}\t\t\tcallback_id="ContextVisibilitySetter"\n'
            f'{i}\t\t\tcontext_function_id="BattleRoot.{visibility}">\n'
            f'{i}\t\t\t<child_m_user_properties>\n'
            f'{i}\t\t\t\t<property\n{i}\t\t\t\t\tname="update_constant"\n{i}\t\t\t\t\tvalue=""/>\n'
            f'{i}\t\t\t</child_m_user_properties>\n'
            f'{i}\t\t</callback_with_context>\n'
        )
    else:
        visibility_cb = (
            f'{i}\t\t<callback_with_context\n'
            f'{i}\t\t\tcallback_id="ContextVisibilitySetter"\n'
            f'{i}\t\t\tcontext_function_id="BattleRoot.{visibility}"/>\n'
        )

    return f'''{i}<{PANEL_ID}
{i}\tthis="{g}"
{i}\tid="{PANEL_ID}"
{i}\tvisible="false"
{i}\tdocking="{docking}"
{i}\tdock_offset="{offset}"
{i}\tcomponent_anchor_point="{anchor}"
{i}\tpriority="1000000"
{i}\ttooltipslocalised="true"
{i}\tuniqueguid="{g}"
{i}\tcurrentstate="{s}"
{i}\tdefaultstate="{s}">
{i}\t<callbackwithcontextlist>
{visibility_cb}{i}\t</callbackwithcontextlist>
{reparent}{i}\t<states>
{i}\t\t<default
{i}\t\t\tthis="{s}"
{i}\t\t\tname="default"
{i}\t\t\twidth="172"
{i}\t\t\theight="74"
{i}\t\t\ttexthbehaviour="Never split"
{i}\t\t\tuniqueguid="{s}"/>
{i}\t</states>
{i}\t<LayoutEngine
{i}\t\ttype="List"
{i}\t\tspacing="0.00,6.00"
{i}\t\tsizetocontent="true">
{i}\t\t<columnwidths>
{i}\t\t\t<column width="8"/>
{i}\t\t</columnwidths>
{i}\t</LayoutEngine>
{i}</{PANEL_ID}>
'''


def emit_button(indent: str, suffix: str, action: str, diag: bool = False,
                fn_override: str = None, can_query: str = None) -> str:
    """
    One button, cloned from auto_lose_btn.

    The click is the whole point:

        callback_id="ContextCommandLeftClick"
        context_object_id="CcoStaticObject"
        context_function_id="BattleRoot.<action>"

    ★ The action takes NO ARGUMENT. That is the design decision the whole exe half rests on -
    actions in this engine are nullary and fetch their own root (ToggleCinematicMode reads
    DAT_1443C9E50 itself), so one action per recipient removes the "how does a twui expression
    pass an argument" question entirely. See runtime/GIFT_UI.md.

    A recipient button also carries a ContextStateSetter on the DLL's CoopCanGiftToPlayerN query,
    so the engine greys it out for anyone who is not a legal target - which is the same set the
    sender-side check enforces, so the button cannot offer what the command would refuse.
    """
    i = indent
    g = guid(f"btn/{suffix}")
    img = guid(f"btn/{suffix}/image")
    st = state_guids(f"btn/{suffix}")

    # What this button actually calls. Computed up front rather than inline, because a nested
    # conditional inside the f-string below is exactly where the last edit went wrong.
    if fn_override is not None:
        fn_expr = fn_override                        # plan D: the hijacked name, with its argument
    elif diag:
        fn_expr = DIAG_ACTION                        # vanilla action, to test the panel itself
    else:
        fn_expr = action                             # the original plan-B design, now unreachable

    # `can_query` is the live path (a plan-E name, reachable); CAN_QUERY is the old plan-B one, which
    # the UI cannot see at all — binding that would disable every button for a reason nobody could
    # find. So the fallback name wins where it exists, and diag/probe builds carry neither.
    can = can_query if can_query else (None if (diag or fn_override is not None)
                                       else CAN_QUERY.get(suffix))
    state_cb = ""
    if can and can_query:
        # ★★ HIDE, DO NOT GREY — and the two reasons point the same way.
        #
        # 1. tester asked for it: an unavailable recipient should vanish and let the list close up,
        #    rather than sit there as dead furniture. The panel's LayoutEngine is a sizetocontent
        #    List, so an invisible child collapses the row instead of leaving a hole.
        #
        # 2. ✗ The greyed-out version DID NOT WORK, and took the panel down with it (2026-08-03).
        #    `ContextStateSetter` with a TERNARY —
        #        BattleRoot.CanGiftToPlayer0 ? "active" : "inactive"
        #    — left every button inactive, the can-gift functions were NEVER CALLED (not one
        #    "asked by the UI" line), and the panel's own visibility query fell from ~3000 polls to
        #    3. A broken expression on a child appears to poison the parent's refresh as well.
        #
        # ⇒ This uses the shape that is PROVEN with a fallback name: `ContextVisibilitySetter` on a
        # plain property, exactly as the panel's own visibility does with GiftPanelOpen. No ternary,
        # no comparison, nothing the expression compiler has to type-check — which is the difference
        # that matters, since our fallback supplies a function and no type metadata.
        state_cb = (
            f'{i}\t\t<callback_with_context\n'
            f'{i}\t\t\tcallback_id="ContextVisibilitySetter"\n'
            f'{i}\t\t\tcontext_function_id="BattleRoot.{can}">\n'
            f'{i}\t\t\t<child_m_user_properties>\n'
            f'{i}\t\t\t\t<property\n{i}\t\t\t\t\tname="update_constant"\n{i}\t\t\t\t\tvalue=""/>\n'
            f'{i}\t\t\t</child_m_user_properties>\n'
            f'{i}\t\t</callback_with_context>\n'
        )
    elif can:
        state_cb = (
            f'{i}\t\t<callback_with_context\n'
            f'{i}\t\t\tcallback_id="ContextStateSetter"\n'
            f'{i}\t\t\tcontext_function_id="BattleRoot.{can} ? &quot;active&quot; : &quot;inactive&quot;"/>\n'
        )

    def imagemetrics(which, tiled):
        extra = f'\n{i}\t\t\t\t\ttile="true"\n{i}\t\t\t\t\tmargin="0.00,60.00,0.00,60.00"' if tiled else ""
        return (
            f'{i}\t\t\t\t<imagemetrics>\n'
            f'{i}\t\t\t\t\t<image\n'
            f'{i}\t\t\t\t\t\tthis="{st["img_" + which]}"\n'
            f'{i}\t\t\t\t\t\tuniqueguid="{st["img_" + which]}"\n'
            f'{i}\t\t\t\t\t\tcomponentimage="{img}"\n'
            f'{i}\t\t\t\t\t\twidth="172"{extra}/>\n'
            f'{i}\t\t\t\t</imagemetrics>\n'
        )

    return f'''{i}<coop_gift_btn_{suffix}
{i}\tthis="{g}"
{i}\tid="coop_gift_btn_{suffix}"
{i}\tallowverticalresize="false"
{i}\tpriority="1000000"
{i}\ttooltipslocalised="true"
{i}\tsoundcategory="UI_GBL_TMP_Square_Large_Text_Button"
{i}\tuniqueguid="{g}"
{i}\tcurrentstate="{st["active"]}"
{i}\tdefaultstate="{st["active"]}">
{i}\t<callbackwithcontextlist>
{i}\t\t<callback_with_context
{i}\t\t\tcallback_id="ContextCommandLeftClick"
{i}\t\t\tcontext_object_id="CcoStaticObject"
{i}\t\t\tcontext_function_id="BattleRoot.{fn_expr}"/>
{state_cb}{i}\t\t<callback_with_context callback_id="StatePropagatorCallback"/>
{i}\t</callbackwithcontextlist>
{i}\t<componentimages>
{i}\t\t<component_image
{i}\t\t\tthis="{img}"
{i}\t\t\tuniqueguid="{img}"
{i}\t\t\timagepath="{VANILLA_BACKPLATE}"
{i}\t\t\twidth="254"
{i}\t\t\theight="60"/>
{i}\t</componentimages>
{i}\t<states>
{i}\t\t<active
{i}\t\t\tthis="{st["active"]}"
{i}\t\t\tname="active"
{i}\t\t\twidth="172"
{i}\t\t\tinteractive="true"
{i}\t\t\tuniqueguid="{st["active"]}">
{imagemetrics("active", True)}{i}\t\t\t<transitionmap>
{i}\t\t\t\t<transition
{i}\t\t\t\t\ttransition_m_target_state="{st["hover"]}"
{i}\t\t\t\t\ttransition_m_transition_time="200"
{i}\t\t\t\t\ttransition_m_interpolation_property_mask="64"/>
{i}\t\t\t</transitionmap>
{i}\t\t</active>
{i}\t\t<down
{i}\t\t\tthis="{st["down"]}"
{i}\t\t\tname="down"
{i}\t\t\twidth="172"
{i}\t\t\tinteractive="true"
{i}\t\t\ttextshadervars="0.30,0.00,0.00,0.00"
{i}\t\t\tuniqueguid="{st["down"]}">
{imagemetrics("down", False)}{i}\t\t\t<transitionmap>
{i}\t\t\t\t<transition
{i}\t\t\t\t\tindex="3"
{i}\t\t\t\t\ttransition_m_target_state="{st["hover"]}"/>
{i}\t\t\t\t<transition
{i}\t\t\t\t\tindex="1"
{i}\t\t\t\t\ttransition_m_target_state="{st["down_off"]}"/>
{i}\t\t\t</transitionmap>
{i}\t\t</down>
{i}\t\t<down_off
{i}\t\t\tthis="{st["down_off"]}"
{i}\t\t\tname="down_off"
{i}\t\t\twidth="172"
{i}\t\t\tinteractive="true"
{i}\t\t\tuniqueguid="{st["down_off"]}">
{imagemetrics("down_off", True)}{i}\t\t\t<transitionmap>
{i}\t\t\t\t<transition transition_m_target_state="{st["down"]}"/>
{i}\t\t\t\t<transition
{i}\t\t\t\t\tindex="8"
{i}\t\t\t\t\ttransition_m_target_state="{st["active"]}"/>
{i}\t\t\t</transitionmap>
{i}\t\t</down_off>
{i}\t\t<hover
{i}\t\t\tthis="{st["hover"]}"
{i}\t\t\tname="hover"
{i}\t\t\twidth="172"
{i}\t\t\tinteractive="true"
{i}\t\t\tshader_name="brighten_t0"
{i}\t\t\tshadervars="1.00,0.00,0.00,0.00"
{i}\t\t\ttextshadervars="0.30,0.00,0.00,0.00"
{i}\t\t\tuniqueguid="{st["hover"]}">
{imagemetrics("hover", True)}{i}\t\t\t<transitionmap>
{i}\t\t\t\t<transition
{i}\t\t\t\t\tindex="1"
{i}\t\t\t\t\ttransition_m_target_state="{st["active"]}"
{i}\t\t\t\t\ttransition_m_transition_time="200"
{i}\t\t\t\t\ttransition_m_interpolation_property_mask="64"/>
{i}\t\t\t\t<transition
{i}\t\t\t\t\tindex="2"
{i}\t\t\t\t\ttransition_m_target_state="{st["down"]}"/>
{i}\t\t\t</transitionmap>
{i}\t\t</hover>
{i}\t\t<inactive
{i}\t\t\tthis="{st["inactive"]}"
{i}\t\t\tname="inactive"
{i}\t\t\twidth="172"
{i}\t\t\tinteractive="true"
{i}\t\t\tdisabled="true"
{i}\t\t\tuniqueguid="{st["inactive"]}">
{imagemetrics("inactive", False)}{i}\t\t</inactive>
{i}\t</states>
{i}</coop_gift_btn_{suffix}>
'''


def emit_label(indent: str, suffix: str, label: str, name_prop: str = None) -> str:
    """
    The button's text. Cloned from button_txt.

    ⚠ textlabel is DROPPED on purpose. Vanilla's carries a loc key
    (button_txt_active_Text_a1d2034) and this pack ships no loc, so keeping it would either draw
    vanilla's "Player - Auto Lose" or nothing at all. Plain text= is what a placeholder wants.

    `name_prop` binds the text to a DLL-supplied player name. The static text= is kept underneath
    it on purpose — see LIVE_NAME_QUERY: this shape has no vanilla precedent, so it has to be
    readable as a failure rather than as an empty button.
    """
    i = indent
    g = guid(f"txt/{suffix}")
    a = guid(f"txt/{suffix}/active")
    n = guid(f"txt/{suffix}/inactive")

    def state(tag, sg, colour):
        return f'''{i}\t\t<{tag}
{i}\t\t\tthis="{sg}"
{i}\t\t\tname="{tag}"
{i}\t\t\twidth="165"
{i}\t\t\theight="26"
{i}\t\t\ttext="{label}"
{i}\t\t\ttextvalign="Center"
{i}\t\t\ttexthalign="Center"
{i}\t\t\ttexthbehaviour="Never split"
{i}\t\t\tfont_m_font_name="dev_font"
{i}\t\t\tfont_m_size="12"
{i}\t\t\tfont_m_colour="{colour}"
{i}\t\t\tfont_m_leading="1"
{i}\t\t\tfontcat_name="dev_button_text"
{i}\t\t\tuniqueguid="{sg}"/>
'''

    name_cb = ""
    if name_prop:
        name_cb = (
            f'{i}\t<callbackwithcontextlist>\n'
            f'{i}\t\t<callback_with_context\n'
            f'{i}\t\t\tcallback_id="ContextTextLabel"\n'
            f'{i}\t\t\tcontext_function_id="BattleRoot.{name_prop}">\n'
            f'{i}\t\t\t<child_m_user_properties>\n'
            f'{i}\t\t\t\t<property\n{i}\t\t\t\t\tname="update_constant"\n{i}\t\t\t\t\tvalue=""/>\n'
            f'{i}\t\t\t</child_m_user_properties>\n'
            f'{i}\t\t</callback_with_context>\n'
            f'{i}\t</callbackwithcontextlist>\n'
        )

    return f'''{i}<coop_gift_txt_{suffix}
{i}\tthis="{g}"
{i}\tid="coop_gift_txt_{suffix}"
{i}\toffset="3.00,3.00"
{i}\tallowverticalresize="false"
{i}\tdocking="Center"
{i}\tcomponent_anchor_point="0.50,0.50"
{i}\tpriority="1000000"
{i}\ttooltipslocalised="true"
{i}\tuniqueguid="{g}"
{i}\tisrelativeresize="true"
{i}\tcurrentstate="{a}"
{i}\tdefaultstate="{a}">
{name_cb}{i}\t<states>
{state("active", a, "#FFF8D7FF")}{state("inactive", n, "#8A8A8AFF")}{i}\t</states>
{i}</coop_gift_txt_{suffix}>
'''


# --------------------------------------------------------------------------------------------
#  Insertion
# --------------------------------------------------------------------------------------------

def build(vanilla: str, diag: bool = False, probe: bool = False, live: bool = False) -> tuple:
    """
    Two insertions, and in live mode one tag that has to change shape.

    Returns (built, reversals), where each reversal is a (text, what_it_replaced) pair. Applying all
    of them to the built file must reproduce vanilla byte for byte - so the claim is PROVEN rather
    than asserted, and it stays proven now that one line is genuinely rewritten.
    """
    if PANEL_ID in vanilla:
        raise SystemExit(f"'{PANEL_ID}' is already present - refusing to insert it twice.")

    # Live mode drops the cancel button: the gift icon itself toggles the panel now, so a cancel
    # would be a second way to do what clicking the button again already does.
    buttons = BUTTONS[:4] if live else BUTTONS

    # 1. hierarchy. Live mode puts the panel INSIDE button_gift_unit so it hangs off the gift icon;
    #    everything else puts it at the top of hud_battle.
    #
    # ⚠ This is the one place the live build is not purely additive: vanilla's button_gift_unit is a
    # self-closing tag and it has to become a container. That is one line changed, and the check in
    # main() reverses it explicitly rather than letting the additions-only proof quietly weaken.
    if live:
        selfclosing = f'<{LIVE_PARENT_ID} this="{LIVE_PARENT_GUID}"/>'
        if vanilla.count(selfclosing) != 1:
            raise SystemExit(f"expected exactly one self-closing '{LIVE_PARENT_ID}', "
                             f"found {vanilla.count(selfclosing)}")
        pi = "\t" * 8                                   # button_gift_unit's own indentation
        hier = (f'<{LIVE_PARENT_ID} this="{LIVE_PARENT_GUID}">\n'
                + emit_hierarchy(pi + "\t", buttons).rstrip("\n")
                + f'\n{pi}</{LIVE_PARENT_ID}>')
        out = vanilla.replace(selfclosing, hier)
        undo_hier = (hier, selfclosing)
    else:
        close_hud = "\n\t\t\t</hud_battle>"
        if vanilla.count(close_hud) != 1:
            raise SystemExit(f"expected exactly one '</hud_battle>' at three tabs, "
                             f"found {vanilla.count(close_hud)}")
        hier = "\n" + emit_hierarchy("\t\t\t\t", buttons).rstrip("\n")
        out = vanilla.replace(close_hud, hier + close_hud)
        undo_hier = (hier, "")

    # 2. definitions, before </components>. Top-level components sit at two tabs.
    close_comp = "\n\t</components>"
    if out.count(close_comp) != 1:
        raise SystemExit(f"expected exactly one '</components>' at one tab, "
                         f"found {out.count(close_comp)}")

    # Probe and live modes both take their visibility from a VANILLA query, so the panel is
    # unconditionally on screen and cannot itself be the thing that fails; they differ only in what
    # the buttons pass — deliberately-odd constants to prove the argument travels, or real ids.
    visibility = (LIVE_VISIBILITY if live else
                  PROBE_VISIBILITY if probe else
                  DIAG_VISIBILITY if diag else "CoopGiftPanelWanted")

    defs = [emit_panel("\t\t", visibility, anchored=live)]
    for n, (suffix, action, label) in enumerate(buttons):
        fn = (f"{LIVE_ACTION}({LIVE_ARGS[n]})"  if live  else
              f"{PROBE_ACTION}({PROBE_ARGS[n]})" if probe else None)
        defs.append(emit_button("\t\t", suffix, action, diag, fn,
                                LIVE_CAN_QUERY.get(suffix) if live else None))
        text = LIVE_LABELS[n] if live else (label if not probe else fn)
        defs.append(emit_label("\t\t", suffix, text,
                               LIVE_NAME_QUERY.get(suffix) if live else None))
    if live:
        from style_panel import style_live_components
        defs = style_live_components(defs, guid)
    body = "\n" + "".join(defs).rstrip("\n")

    return out.replace(close_comp, body + close_comp), [undo_hier, (body, "")]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--vanilla", required=True, type=Path,
                    help="extracted vanilla ui/battle ui/hud_battle.twui.xml")
    ap.add_argument("--out", required=True, type=Path,
                    help="where to write the modified copy")
    ap.add_argument("--live", action="store_true",
                    help="plan D for real: buttons call the HIJACKED GiftUnits(0..3) with the "
                         "recipient's player id, visibility on a vanilla query")
    ap.add_argument("--probe", action="store_true",
                    help="plan-D probe: visibility on a vanilla query, every button on the "
                         "HIJACKED entry with a distinct argument")
    ap.add_argument("--diagnostic", action="store_true",
                    help="bind to VANILLA CcoBattleRoot members instead of ours - see the "
                         "Diagnostic mode note at the top of this file")
    args = ap.parse_args()

    # newline="" keeps the file's own line endings out of Python's translation layer, so we can
    # see what they actually are instead of guessing. hud_battle.twui.xml is CRLF, and writing it
    # back as LF would make every one of its 28,758 lines show as changed in review - which is
    # exactly how a one-line edit becomes unreviewable.
    with open(args.vanilla, "r", encoding="utf-8", newline="") as f:
        raw = f.read()
    crlf = "\r\n" in raw
    if crlf and "\n" in raw.replace("\r\n", ""):
        raise SystemExit("source has MIXED line endings - stopping rather than guessing.")

    if sum([bool(args.live), bool(args.probe), bool(args.diagnostic)]) > 1:
        raise SystemExit("--live, --probe and --diagnostic are three different packs. Pick one.")

    src = raw.replace("\r\n", "\n") if crlf else raw
    built, blocks = build(src, args.diagnostic, args.probe, args.live)

    # Prove the additions-only claim instead of asserting it: strip our two blocks back out and
    # the result must be the original, byte for byte.
    check = built
    for text, original in blocks:
        check = check.replace(text, original, 1)
    if check != src:
        raise SystemExit("BUG: reversing the inserted blocks did not reproduce the original. "
                         "Something existing was modified - refusing to write.")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\r\n" if crlf else "\n") as f:
        f.write(built)

    print(f"[+] {args.out}")
    print(f"    {4 if args.live else len(BUTTONS)} buttons, "
          f"+{built.count(chr(10)) - src.count(chr(10))} lines, "
          f"{'CRLF' if crlf else 'LF'} preserved")
    if args.live:
        print(f"    reversible: PROVEN — undoing the two blocks AND restoring the self-closing "
              f"<{LIVE_PARENT_ID}> reproduces vanilla exactly (1 line changed, {len(BUTTONS[:4])*2 + 1} added blocks)")
    else:
        print(f"    additions only: PROVEN (stripping the inserted blocks reproduces vanilla exactly)")
    if args.live:
        print(f"    ** LIVE BUILD ** visibility={LIVE_VISIBILITY}  "
              f"buttons={LIVE_ACTION}({'/'.join(LIVE_ARGS)})")
        print(f"       needs the DLL that renames DevCycleArmy. The argument IS the player id.")
    elif args.probe:
        print(f"    ** PLAN-D PROBE BUILD ** visibility={PROBE_VISIBILITY}  "
              f"buttons={PROBE_ACTION}({'/'.join(PROBE_ARGS)})")
        print(f"       needs the DLL that renames DevCycleArmy. Must not be shipped.")
    elif args.diagnostic:
        print(f"    ** DIAGNOSTIC BUILD ** visibility={DIAG_VISIBILITY}  action={DIAG_ACTION}")
        print(f"       this tests the PANEL, not the DLL. It must not be shipped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
