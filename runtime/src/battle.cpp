// battle.cpp - Battle: unit-ownership identity, rematch unlock, spectator latch, build-HUD-as-participant.
//
// Split out of the original single-file tw3k_coop.cpp. Behaviour is unchanged: this was pure
// code motion. ../tw3k_coop.ASBUILT.cpp remains the known-good single-file snapshot.

#include "tw3k.h"

#ifndef TW3K_RELEASE
// ---- ★★★ WHICH FIELD OF A UNIT SAYS WHO COMMANDS IT? (session 6l) -----------------------------
//
// The A/B is now solid and the two sides look IDENTICAL in the data:
//
//   2 players — the receiver gets the unit, sees it, orders it about, hands it back.  WORKS.
//   3 players — the receiver's slot record gains the unit, `battlefield unit=` is a real pointer
//               with the right id on every machine, and nothing is usable on screen.
//
// So the slot record is not where the difference lives. The next structure along is the battlefield
// unit object itself (the thing at `entry+0x50`, whose id sits at +0x3D94), and the question is
// whether the transfer rewrites the unit's own notion of who commands it.
//
// We do not know that field's offset — but we do not need to guess it. On any machine mid-battle
// there are two units with DIFFERENT owners: one the receiver now holds, one the giver still holds.
// Scan both objects in parallel and report every offset that reads the correct owning player id in
// BOTH. That is a handful of candidates instead of a hunt, and the intersection kills the false
// positives that scanning for a small integer like 0 or 2 would otherwise drown in.
//
// ★ And it is decisive either way, on the failing run alone:
//   * such an offset EXISTS  -> ownership did reach the unit object, and the failure is further
//                               downstream, in input/selection. Much smaller, and client-side.
//   * NO such offset exists  -> the received unit still believes it belongs to the giver. That is
//                               the bug, and it names the field the receive path forgets to write.
// Does `off` read `want` in EVERY unit this player holds? A real owner field does; a coincidence
// almost never does.
static bool allUnitsHold(uintptr_t slotObj, uint32_t slotIdx, size_t off, uint32_t want)
{
    uint32_t  n = 0;
    uintptr_t list = 0;
    if (!readAt(slotObj + (uintptr_t)slotIdx * SLOT_ENTRY_STRIDE + SLOT_ENTRY_UNITS, n) || !n)
        return false;
    if (!readAt(slotObj + (uintptr_t)slotIdx * SLOT_ENTRY_STRIDE + SLOT_ENTRY_LIST, list) ||
        list <= 0x10000)
        return false;

    for (uint32_t u = 0; u < n && u < 20; ++u) {
        uintptr_t obj = 0;
        uint32_t  v   = 0;
        if (!readAt(list + (uintptr_t)u * UNIT_REC_STRIDE + UNIT_REC_OBJ, obj) || obj <= 0x10000)
            return false;
        if (!readAt(obj + off, v) || v != want) return false;
    }
    return true;
}

static void probeUnitOwnerField(uintptr_t slotObj, uint32_t nSlots)
{
    uintptr_t objA = 0, objB = 0;
    uint32_t  idA  = 0, idB  = 0;

    for (uint32_t i = 0; i < nSlots && i < 16 && !objB; ++i) {
        uint32_t  n = 0;
        uintptr_t list = 0, obj = 0;
        if (!readAt(slotObj + (uintptr_t)i * SLOT_ENTRY_STRIDE + SLOT_ENTRY_UNITS, n) || !n) continue;
        if (!readAt(slotObj + (uintptr_t)i * SLOT_ENTRY_STRIDE + SLOT_ENTRY_LIST, list) ||
            list <= 0x10000) continue;
        if (!readAt(list + UNIT_REC_OBJ, obj) || obj <= 0x10000) continue;
        if (!objA) { objA = obj; idA = i; } else { objB = obj; idB = i; }
    }
    if (!objA || !objB) return;         // needs two players actually holding units

    logf("  --- owner-field probe: player %u's unit @%016llX  vs  player %u's unit @%016llX ---",
         idA, (unsigned long long)objA, idB, (unsigned long long)objB);

    // ★★★ Collect the "looks like an owner id" offsets BEFORE filtering, so we can print what every
    // unit actually reads there. Run 16 rejected 29 of them for not being consistent across all of a
    // player's units — but "inconsistent" is precisely what a HALF-APPLIED transfer looks like, so
    // throwing them away discarded the interesting case. A pass/fail was the wrong readout; the
    // values themselves are the answer.
    size_t cand[8];
    int    nCand = 0;

    int hits = 0, rejected = 0;
    for (size_t off = 0; off + 4 <= 0x4000; off += 4) {
        uint32_t a = 0, b = 0;
        if (!readAt(objA + off, a)) continue;
        if (!readAt(objB + off, b)) continue;
        if (a != idA || b != idB) continue;
        if (nCand < 8) cand[nCand++] = off;

        // ★ Confirm against EVERY unit both players hold, not just the first of each. Player ids are
        // tiny integers — 0 especially, which matches any zeroed field — so a single pair produces a
        // lot of coincidences. A real owner field reads its owner's id in all of that player's
        // units, and this check costs nothing.
        if (!allUnitsHold(slotObj, idA, off, idA) || !allUnitsHold(slotObj, idB, off, idB)) {
            ++rejected;
            continue;
        }
        logf("      +0x%04zX = %u / %u   <-- holds the OWNING player id in EVERY unit of both", off,
             a, b);
        if (++hits >= 16) { logf("      (stopping at 16 candidates)"); break; }
    }
    if (rejected)
        logf("      (%d offset(s) matched the first pair by coincidence and were rejected against "
             "the rest)", rejected);
    if (!hits)
        logf("      no offset holds the raw SESSION player id in EVERY unit — but see the table "
             "below before concluding anything from that.");

    // ---- ★★★ WHAT DOES EVERY UNIT ACTUALLY READ THERE? (session 6p) ----------------------------
    //
    // The pass/fail above cannot distinguish "this offset is noise" from "this offset is the owner
    // field and the transfer only updated some of them" — and the second is exactly the shape of the
    // bug we are chasing. So print the values instead of judging them.
    //
    // Read it like this, for a candidate offset:
    //     slot0=[0,0]  slot1=[1,1,1,1]   -> it IS the owner field and it IS being written. The
    //                                       failure is downstream, in whatever decides "may I
    //                                       command this".
    //     slot0=[0,1]  slot1=[1,1,1,1]   -> ★ the received unit still says it belongs to the giver.
    //                                       That is the bug, at a named offset.
    //     slot0=[0,7]  slot1=[1,4,2,9]   -> noise; the offset means something else entirely.
    if (nCand) {
        logf("      --- what every unit reads at those offsets (the whole point) ---");
        for (int c = 0; c < nCand; ++c) {
            char line[512];
            int  w = _snprintf_s(line, sizeof(line), _TRUNCATE, "      +0x%04zX:", cand[c]);
            for (uint32_t s = 0; s < nSlots && s < 8; ++s) {
                uint32_t  n = 0;
                uintptr_t list = 0;
                if (!readAt(slotObj + (uintptr_t)s * SLOT_ENTRY_STRIDE + SLOT_ENTRY_UNITS, n) || !n)
                    continue;
                if (!readAt(slotObj + (uintptr_t)s * SLOT_ENTRY_STRIDE + SLOT_ENTRY_LIST, list) ||
                    list <= 0x10000) continue;
                w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "  slot%u=[", s);
                for (uint32_t u = 0; u < n && u < 12; ++u) {
                    uintptr_t obj = 0;
                    uint32_t  v   = 0xFFFFFFFF;
                    readAt(list + (uintptr_t)u * UNIT_REC_STRIDE + UNIT_REC_OBJ, obj);
                    if (obj > 0x10000) readAt(obj + cand[c], v);
                    w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "%s%d",
                                     u ? "," : "", (int)v);
                }
                w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "]");
            }
            logf("%s", line);
        }
    }

    // ---- ★★ THE WIDER SWEEP (session 6n) --------------------------------------------------------
    //
    // Run 15 returned nothing above, and that is **not** evidence of the bug. It only rules out one
    // narrow shape: the unit storing the raw session player id. A battle unit is far more likely to
    // reference its owner by an army / alliance / team index, or by a pointer, none of which equal
    // the session id — so the first probe could not have found it even if everything worked.
    //
    // So stop guessing the encoding and just list the candidates: every offset where the two units
    // DIFFER and both values are small integers. Owner, team, army and alliance indices all look
    // like that; positions, health and pointers do not.
    //
    // ★ This is only meaningful as an A/B. Run it on the 2-player case (where the receiver really
    //   does command the unit) and on the 3-player case (where they do not), then compare:
    //     * an offset in the WORKING list but missing from the FAILING one is the field the receive
    //       path fails to write — the bug, named.
    //     * identical lists mean the unit object is not where the difference lives at all, and the
    //       divergence is in whatever the client consults to decide "may I command this".
    logf("      --- differing small-integer fields (candidates for owner/team/army index) ---");
    int diffs = 0;
    for (size_t off = 0; off + 4 <= 0x8000; off += 4) {
        uint32_t a = 0, b = 0;
        if (!readAt(objA + off, a)) continue;
        if (!readAt(objB + off, b)) continue;
        if (a == b || a >= 64 || b >= 64) continue;
        logf("      +0x%04zX = %u / %u", off, a, b);
        if (++diffs >= 40) { logf("      (stopping at 40)"); break; }
    }
    if (!diffs)
        logf("      none — the two unit objects agree on every small integer in 0x8000 bytes, so "
             "ownership is not recorded in the unit at all.");
}

// ------------------------------------------- B10: the sides are drawn the wrong way round
//
// **Observed** (tester, 2026-08-04): joined a battle on another player's side, and the HUD painted
// **our** units red and the **AI's** green — the alliance colouring inverted.
//
// ★★★ THE ENGINE'S CLASSIFIER IS FOUR FIELD READS, AND IT ALL HANGS ON ONE POINTER. `AllianceColour`
// on a battle unit (`FUN_142E7BC80`, CA's own description: *"returns the colour based on alliance
// (player/friendly/enemy/neutral)"*) does its deciding in `FUN_142E7BDA0(battleRoot, unit)`:
//
//     myArmy       = *(battleRoot + 0x160)        // PlayerArmyContext — offsets.h already has it
//     unitArmy     = *(unit + 0x590)              // FUN_142379DD0, one instruction
//     if (unitArmy == myArmy)                        -> MINE
//     myAlliance   = *(myArmy   + 0xA0)           // FUN_14102E660, one instruction
//     unitAlliance = *(unitArmy + 0xA0)
//     if (unitAlliance != myAlliance)                -> 2 = ENEMY   (red)
//     else                                           -> 1 = FRIENDLY (green)
//
// ⚠ §6jjj's `alliance(unit) = *(*(unit + 0x590) + 0x0A0)` is these two accessors composed. Same
// chain, now with each half named and its own engine function behind it.
//
// ⇒ **Every colour in the battle is decided by `battleRoot + 0x160`.** If it points at an army in
// the OTHER alliance, then this client's own units fail the identity test, fail the alliance test,
// and come out `2 = ENEMY` — while the enemy's units match that army's alliance and come out
// friendly. **Red for us, green for them: the reported symptom, exactly, from one wrong pointer.**
//
// ⚠ Suspected, not established — nothing has yet read `+0x160` in the inverted battle. This dump is
// what settles it, and it settles it either way:
//   * `myArmy` is an army on the other side -> B10 is that pointer, and it is the same *identity*
//     family as §6bbb's wrong-lender bug rather than anything to do with colour
//   * `myArmy` is correct and the classification still comes out inverted -> the fault is downstream
//     of the classifier, and this dump names that instead
void dumpBattleSides()
{
    logf("---- BATTLE SIDES (B10: whose units does this client think are friendly?) ----");

    uintptr_t br = 0;
    if (!readAt(g_base + RVA_BATTLE_ROOT_PTR, br) || !br) {
        logf("  not in a battle (battle root = 0) — nothing to read.");
        return;
    }

    uintptr_t myArmy = 0, myAlliance = 0;
    readAt(br + OFF_BR_MY_ARMY, myArmy);

    // ⚠ MEASURED TRAP (2026-08-04, 23:53): after the battle ends the root global keeps pointing at
    // the freed object, and `+0x160` came back as **0x52** — a small integer where an army pointer
    // belongs, with `IsSpectator` unreadable beside it. Reading that as "the army is wrong" would be
    // a finding invented out of a dead allocation, which is exactly the species of mistake the STALE
    // lobby-pointer note already exists to prevent. So: refuse, and say why.
    if (myArmy && myArmy <= 0x10000) {
        logf("  battleRoot=%016llX  myArmy(+0x160)=%016llX — that is NOT A POINTER.",
             (unsigned long long)br, (unsigned long long)myArmy);
        logf("  ⇒ The battle is over and this root is STALE: the object was freed and its memory now "
             "belongs to something else. REFUSING to read further. This dump only means anything "
             "while a battle is actually on screen.");
        return;
    }

    if (myArmy) readAt(myArmy + OFF_ARMY_ALLIANCE, myAlliance);

    uint8_t spectator = 0;
    {   // the same two-step the gift feature uses: *(br + 8) + 0x350 + 0x10
        uintptr_t lp = 0;
        if (readAt(br + OFF_BR_LOCAL_PLAYER, lp) && lp)
            readAt(lp + OFF_LP_SPECTATOR, spectator);
    }

    logf("  battleRoot=%016llX  myArmy(+0x160)=%016llX  myAlliance(+0xA0)=%016llX  spectator=%u",
         (unsigned long long)br, (unsigned long long)myArmy,
         (unsigned long long)myAlliance, spectator);

    if (!myArmy) {
        logf("  ★★★ myArmy IS NULL. Every unit then fails the identity test and the alliance compare");
        logf("      reads through a null army — which is the inversion reported, and names the field.");
        return;
    }

    // Every unit the MP session knows about, classified exactly the way the engine classifies it.
    const uintptr_t mp = capturedMp();
    uintptr_t slotObj = 0;
    if (!mp || !readAt(mp + OFF_MP_SLOTOBJ, slotObj) || !slotObj) {
        logf("  MP session not captured, so the per-unit walk is unavailable. The three pointers "
             "above are still the reading — compare myAlliance against the other machines'.");
        return;
    }

    uint32_t slots = 0;
    readAt(slotObj + OFF_SLOT_COUNT, slots);
    if (slots > 16) slots = 16;

    logf("  --- every unit the session holds, classified as the engine classifies it ---");
    logf("      %-6s %-18s %-18s %s", "player", "army", "alliance", "verdict");
    for (uint32_t s = 0; s < slots; ++s) {
        uint32_t  n = 0;
        uintptr_t list = 0;
        if (!readAt(slotObj + (uintptr_t)s * SLOT_ENTRY_STRIDE + SLOT_ENTRY_UNITS, n) || !n) continue;
        if (!readAt(slotObj + (uintptr_t)s * SLOT_ENTRY_STRIDE + SLOT_ENTRY_LIST, list) ||
            list <= 0x10000) continue;

        // One line per player, not per unit: every unit of a player shares its army, and a battle's
        // worth of identical lines would bury the reading.
        uintptr_t obj = 0;
        if (!readAt(list + UNIT_REC_OBJ, obj) || obj <= 0x10000) continue;

        uintptr_t army = 0, alliance = 0;
        readAt(obj + OFF_UNIT_ARMY, army);
        if (army) readAt(army + OFF_ARMY_ALLIANCE, alliance);

        const char* verdict = (army == myArmy)          ? "MINE (this client commands them)"
                            : (alliance != myAlliance)  ? "ENEMY  -> drawn RED"
                                                        : "friendly -> drawn green";
        logf("      %-6u %016llX  %016llX  %s%s", s, (unsigned long long)army,
             (unsigned long long)alliance, verdict,
             (army && army == myArmy) ? "" : "");
    }

    logf("  >>> READING IT: find the row for YOUR OWN player id. If it says ENEMY, the engine really");
    logf("      does hold this client on the other side, and `myArmy` above is the wrong army — the");
    logf("      colours are a symptom, not the bug. If it says MINE while the HUD paints red, the");
    logf("      fault is downstream of this classifier.");
    logf("  ⚠ Compare `myAlliance` across machines by POSITION in this table, never by pointer value —");
    logf("      these are per-process allocations and differ on healthy machines too.");
}

void dumpMpSession()
{
    const uintptr_t mp = capturedMp();
    if (!mp) {
        logf("MP session: not captured yet — install with F6, then open an MP campaign lobby.");
        return;
    }
    uintptr_t slotObj = 0;
    if (!readAt(mp + OFF_MP_SLOTOBJ, slotObj) || !slotObj) {
        logf("MP session: EMPIRE_MP=%016llX but slot object (+0xD3848) unreadable",
             (unsigned long long)mp);
        return;
    }
    uint32_t count = 0, occupied = 0;
    readAt(slotObj + OFF_SLOT_COUNT, count);
    readAt(slotObj + OFF_SLOT_OCCUPIED, occupied);
    uint8_t advMax = 0, advFree = 0, advSpec = 0;
    readAt(mp + OFF_MP_ADV_MAX, advMax);
    readAt(mp + OFF_MP_ADV_FREE, advFree);
    readAt(mp + OFF_MP_ADV_SPEC, advSpec);

    // Which player am I? Every instruction of the form "watch your own unit count" needs this.
    int localId = -1;
    __try {
        uintptr_t lp = 0;
        if (readAt(mp + OFF_MP_LOCAL_PLAYER, lp) && lp > 0x10000) {
            const uintptr_t lpVt = *(uintptr_t*)lp;
            if (lpVt > 0x10000)
                localId = (int)((uint32_t(*)(void*))(*(uintptr_t*)(lpVt + 0x20)))((void*)lp);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { localId = -1; }

    logf("=== MP SESSION === EMPIRE_MP=%016llX slotObj(+0xD3848)=%016llX",
         (unsigned long long)mp, (unsigned long long)slotObj);
    // ✗ Said "YOU ARE PLAYER %d" until 2026-08-11, and it was wrong whenever seating went
    // non-contiguous: on 2026-08-10 it printed "YOU ARE PLAYER 3" for a machine that was player 2
    // in slot 3. `localId` is `vt[0x20]()`, and this very line indexes the SLOT array with it two
    // screens below (`i == localId` -> `<<< YOU`), so the code already knew.
    // ★ Third instance today of a name read as a value. Print the seat, and cross to the id.
    if (localId >= 0)
        logf("  >>> *** YOU ARE IN SLOT %d ON THIS MACHINE *** (marked <<< YOU below)%s",
             localId,
             " — localPlayerId() is a SLOT index; the player id it maps to is on the +0x137C line");
    else
        logf("  >>> local player id not readable yet (normal before the session is up)");
    // ⚠ +0x1364 is NOT a max-players field. `wiki/multiplayer.md` corrected that long ago — it is a
    // DERIVED count of occupied slots, recomputed every update — but this line kept printing "MAX
    // PLAYERS", and on 2026-08-04 it was read back as one: a lobby forming was misread as a cap
    // falling from 2 to 1. A log label is evidence to whoever reads it next, so it has to be true.
    logf("  >>> slot entries(+0x1360)=%u   occupied slots(+0x1364, derived)=%u", count, occupied);
    logf("  advertised: max(+0xD23D8)=%u  freeSlots(+0xD23D9)=%u  spectators(+0xD23DA)=%u",
         advMax, advFree, advSpec);
    if (count && count <= 64) {
        for (uint32_t i = 0; i < count && i < 16; ++i) {
            uint32_t flags = 0, units = 0;
            readAt(slotObj + (uintptr_t)i * SLOT_ENTRY_STRIDE + SLOT_ENTRY_FLAGS, flags);
            // ★ THE UNIT COUNT, AND IT WORKS ON EVERY MACHINE (session 6h).
            //
            // The battle census used to need the battle manager, which only the machine that clicks
            // gift ever sees — so the RECIPIENT could never confirm anything. It turns out not to be
            // needed: the record `vt[0x228](svc, id)` returns is just `slotObj + id*0xF8 + 0x10`,
            // checked against a live log (slotObj ...7423F0, player 3's record ...7426E8, and
            // 3*0xF8 + 0x10 = 0x2F8). Its count at +4 is therefore slot+0x14 — the same field
            // FUN_140471B30 scans to prove the sender owns the unit. So the number of units a player
            // holds is readable from EMPIRE_MP alone, which every machine captures at lobby time.
            //
            // ⇒ the receiving player presses F5/F6 and sees their own count go up. That is the
            //    cross-machine confirmation the gift work has never had.
            readAt(slotObj + (uintptr_t)i * SLOT_ENTRY_STRIDE + SLOT_ENTRY_UNITS, units);
            logf("    slot[%u] flags(+0xF4)=%08X  occupied=%s spectator=%s  UNITS HELD=%u%s",
                 i, flags,
                 ((flags & 0x140) == 0x140 && !((flags >> 10) & 1)) ? "yes" : "no",
                 ((flags & 0x440) == 0x440 && !((flags >> 7) & 1)) ? "yes" : "no",
                 units, ((int)i == localId) ? "   <<< YOU" : "");

            // ★ WHERE IS THE PLAYER'S NAME? (2026-08-03)
            //
            // The gift panel labels its buttons "Give to player 0..3", which is honest but useless
            // to a human — it should say who. A CCO query CAN return a string: the value goes to
            // `out->vtable[0x80]` as a CaString, where a bool uses 0x50 (read out of FUN_142D59FA0,
            // the lobby's ReadyStatusText). What is missing is the NAME ITSELF.
            //
            // This slot record is 0xF8 bytes and we only read two fields of it, so the cheapest
            // question is whether a name pointer is sitting in the rest. Scan the record for
            // pointers that lead to printable ASCII and print what they say. Log-only, and only for
            // occupied slots, so it costs a few lines per F5 and changes nothing.
            const uintptr_t rec = slotObj + (uintptr_t)i * SLOT_ENTRY_STRIDE;
            for (uintptr_t off = 0; off + 8 <= SLOT_ENTRY_STRIDE; off += 8) {
                uintptr_t p = 0;
                if (!readAt(rec + off, p) || p < 0x10000) continue;
                char txt[48] = { 0 };
                if (!safeRead((void*)p, txt, sizeof(txt) - 1)) continue;
                int n = 0;
                while (n < 40 && txt[n] >= 0x20 && txt[n] < 0x7F) ++n;
                if (n >= 3 && txt[n] == '\0')
                    logf("          +0x%02llX -> \"%s\"   <-- candidate name/string field",
                         (unsigned long long)off, txt);
            }

            // ★★★ IS THERE A REAL UNIT BEHIND EACH ENTRY? (session 6j)
            //
            // Gifting is now known to move ownership in this record on every machine while nothing
            // changes hands on the battlefield, and the feature only ever targets SPECTATORS — you
            // cannot gift to a player who brought their own army. So the question is precisely what
            // a spectator ends up holding.
            //
            // FUN_1404A9EB0, which the engine's own gift handler uses to identify a unit, is a pure
            // field read and answers it:
            //     if (*(entry + 0x50) == 0) return -1;
            //     return *(uint32_t *)(*(entry + 0x50) + 0x3D94);
            // So `entry+0x50` is the BATTLEFIELD unit object. An entry whose +0x50 is null is a
            // record of ownership with no unit behind it — which is exactly what "they own it but
            // nothing appears" would look like, and it is one word to check.
            //
            // Pure reads only: this runs on the hotkey thread, not the game thread, so nothing here
            // may call into the engine.
            uintptr_t list = 0;
            if (units && units <= 64 &&
                readAt(slotObj + (uintptr_t)i * SLOT_ENTRY_STRIDE + SLOT_ENTRY_LIST, list) &&
                list > 0x10000) {
                for (uint32_t u = 0; u < units && u < 12; ++u) {
                    const uintptr_t entry = list + (uintptr_t)u * UNIT_REC_STRIDE;
                    uintptr_t obj = 0;
                    uint32_t  id  = 0xFFFFFFFF;
                    if (!readAt(entry + UNIT_REC_OBJ, obj)) continue;
                    if (obj > 0x10000) readAt(obj + UNIT_OBJ_ID, id);
                    logf("        unit[%u] entry=%016llX  battlefield unit=%016llX  id=%d%s",
                         u, (unsigned long long)entry, (unsigned long long)obj, (int)id,
                         (obj > 0x10000) ? "" : "   <-- NO UNIT BEHIND IT (owned on paper only)");
                }
            }
        }
    }
    // ---- ★★★ THE PLAYER-INDEX → SLOT-INDEX TABLE (session 6s) ---------------------------------
    //
    // Four different message handlers do the same thing before touching a slot:
    //
    //     if (playerIdx < 0x14) slot = *(uint *)(slotObj + 0x137C + playerIdx*4); else slot = -1;
    //
    // So the engine has **two index spaces** — a player index (0..19, what several messages carry)
    // and a slot index (0..3, what the slot array and the share list use) — and `slotObj+0x137C` is
    // the table that converts one to the other.
    //
    // ★ That matters here because the gift message carries a SLOT index (the share list pushes its
    // loop counter over the slot array) while other messages carry a PLAYER index. With two players
    // the two spaces almost certainly coincide, so a mix-up would be invisible; with three they need
    // not, and the gift lands somewhere that exists in one space and means nothing in the other.
    //
    // ⚠ And WE may be the reason they diverge: the seat hook exists precisely to force players into
    // contiguous slots, so a player's slot index is not necessarily the index the engine would have
    // given them. If this table is not the identity, that is the first thing to suspect.
    //
    // Cheap to print, and it either kills the idea or promotes it to the main line.
    {
        bool any = false;
        char line[256];
        int  w = _snprintf_s(line, sizeof(line), _TRUNCATE, "  player->slot table (+0x137C):");
        for (uint32_t i = 0; i < 20; ++i) {
            uint32_t v = 0xFFFFFFFF;
            if (!readAt(slotObj + 0x137C + (uintptr_t)i * 4, v)) break;
            if (v == 0xFFFFFFFF) continue;
            any = true;
            w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "  p%u->s%u%s", i, v,
                             (i == v) ? "" : " *");
        }
        if (any)
            logf("%s      (* = player index differs from slot index)", line);
        else
            logf("  player->slot table (+0x137C): all -1 (not populated yet)");
    }

    // ---- ★★★ WHO DOES THIS CLIENT THINK IT IS? (session 6x) ----------------------------------
    //
    // Run 19 moved the bug out of the network and into the client: the simulation record transferred
    // perfectly to a named player, on every machine, with a live battlefield unit attached — and the
    // recipient still could not command it, saw it rendered as an ALLY (blue, not green), and could
    // not drag a selection box around anything at all. An empty drag-box means the client's
    // controllable set is empty, not that one unit is special.
    //
    // These two fields are what the game's own UI asks when it wants those answers, so they are the
    // shortest path to the same truth. Both are pure reads — see RVA_BATTLE_ROOT_PTR above.
    //
    //   my army     null on a recipient  =>  they have no army for a unit to join, which explains
    //                                        every symptom at once, and the fix has a target
    //   my army   non-null on a recipient =>  they DO have an army and the unit was not put in it;
    //                                        the fault is narrower and lives in the receive path
    //
    // ★ The run that matters is the comparison: capture this in a 3-player battle (fails) AND in a
    // 2-player one (works). Whatever differs between those two lines is the bug.
    {
        // ⚠ `> 0x10000` is NOT enough, and this guard is inherited from the retired
        // `spectatorByteAddr()` rather than dropped with it. On the campaign map the global still
        // holds a FREED battle root, and run 22 printed `IsSpectator=255 at FBFFFC27FC50FFD9` from
        // it — a non-canonical address that sailed through the weak check and produced a line that
        // looked like data. Require user-mode canonical pointers, and a byte that actually reads as
        // a bool, or say "not in a battle".
        auto plausible = [](uintptr_t v) { return v > 0x10000 && v < 0x00007FFFFFFFFFFFull; };

        uintptr_t br = 0;
        if (readAt(g_base + RVA_BATTLE_ROOT_PTR, br) && plausible(br)) {
            uintptr_t myArmy = 0, localPlayer = 0;
            uint8_t   spec = 0xFF;
            const bool okArmy = readAt(br + OFF_BR_MY_ARMY, myArmy);
            const bool okLp   = readAt(br + OFF_BR_LOCAL_PLAYER, localPlayer);
            const bool okSpec = okLp && plausible(localPlayer) &&
                                safeRead((void*)(localPlayer + OFF_LP_SPECTATOR), &spec, 1) &&
                                spec <= 1;

            logf("  --- who this client thinks it is (battle root %016llX) ---",
                 (unsigned long long)br);
            logf("      PlayerArmyContext (+0x160) = %016llX%s",
                 (unsigned long long)myArmy,
                 !okArmy      ? "   (unreadable)"
                 : myArmy == 0 ? "   <<< NULL - this client has NO ARMY of its own"
                               : "   (this client owns an army)");
            if (okSpec) {
                logf("      IsSpectator                = %u%s", spec,
                     spec ? "   <<< the game considers this client a SPECTATOR" : "");

                // ✂ Carried over from the retired F9 handler, which printed this pair and nothing
                // else worth keeping. `+0x10` is "can command" and `+0x11` is "is not a combatant" —
                // the two bytes that together make a lending spectator — so reading only the first
                // cannot tell the working state from run 25's half-state.
                uint8_t second = 0xFF;
                if (safeRead((void*)(localPlayer + OFF_LP_SPECTATOR + 1), &second, 1))
                    logf("      +0x11 (not-a-combatant)    = %u%s", second,
                         (spec == 0 && second == 1)
                           ? "   <<< LENDING SPECTATOR: commands units, is not a combatant"
                           : (spec == 0 && second == 0)
                             ? "   <<< half-state - can command, but foreign army cards will appear"
                             : "");
            } else {
                logf("      IsSpectator                = unreadable (local player obj %016llX)",
                     (unsigned long long)localPlayer);
            }
        } else {
            logf("  --- battle root is null: not in a battle right now (this is normal in a lobby) ---");
        }
    }

    logf("=== end MP session ===");
}

// ------------------------------------------ ★★★ REMATCH IN A MULTIPLAYER BATTLE (session 6ff)
//
// The post-battle Rematch button is greyed out in every multiplayer battle, coop included. One
// predicate decides it:
//
//     FUN_142EE6E30(battleMgr):                            <- sole caller is the rematch button
//         S = FUN_141FF6530(mgr+8);                        // = *(mgr+8) + 0x64580
//         if (S[0] == 0 && !FUN_141F6D490(mgr)) return 1;  // enabled
//         return 0;                                        // greyed
//
// `S[0] != 0` is not our *reading* of that field — it is the engine's own definition of multiplayer.
// The `IsMultiplayer` databinding query (registered at 1402B243A via
// FUN_143086FE0(slot, name@1437F7618, fn, flags=0/query)) has getter FUN_1430A4A40, whose entire
// body is `return *FUN_141FF6530(DAT_1443C85E0+8) != 0`. Same field, same expression. So the rule is
// exactly `canRematch = !IsMultiplayer && !replay`, and in coop the first term settles it before the
// replay term is even evaluated.
//
// ★ WHY THIS IS WORTH TESTING INSTEAD OF ASSUMING THE GATE IS LOAD-BEARING: the rematch ACTION
// already has a written multiplayer path. FUN_142E926E0(mgr, _, isRematch=1) calls FUN_142EA3610:
//
//     if (*FUN_141FF6530(mgr+8) != 0) {           // IS multiplayer
//         cmd = &PTR_FUN_1434D4DD8; ...           // battle-end command object
//         FUN_141EF7390(FUN_14200ACB0(), &cmd);   // networked dispatch
//         return;
//     }
//     ... otherwise walk every army and serialise the same command locally
//
// The normal "leave battle" exit, FUN_142EA32F0 — which demonstrably works in our coop battles — is
// the SAME shape with the SAME command object and the SAME dispatch, differing only in a payload
// type code (rematch 1/2, exit 0/1). The plumbing exists; only the UI gate says no. That is the same
// pattern as the lobby "2", the panel count, and list[0] gifting.
//
// ⚠ WHAT THIS DOES NOT PROVE: that the full rematch flow resynchronises both clients. An MP branch
// existing is not an MP flow working — the standing lesson of this project is that a shape match is
// not a confirmation. Do NOT record this as "rematch works in coop" until a run says so.
//
// METHOD — no detour, no trampoline. The predicate is a leaf with EXACTLY ONE caller (14305BE63, the
// instruction immediately before the button_rematch lookup), so forcing its return cannot affect
// anything else in the game. We overwrite its first three bytes with `MOV AL,1 / RET`.
//
// ★ The write is a single ALIGNED 8-BYTE STORE, not a 3-byte memcpy. 0x142EE6E30 is 16-byte aligned,
// so one 64-bit store is atomic on x86-64 and a game thread entering the function can never observe
// a half-written prologue. Bytes 3..7 are rewritten with their own original values — they survive as
// dead code behind the RET — which is what lets a 3-byte change be expressed as an 8-byte atomic
// one. Restore is the same store in reverse.
//
// Signature is verified at ARM time, not at attach, and this site is deliberately NOT added to
// kSites: a mismatch here must disable this experiment only, never block the nine working hooks.
static constexpr uintptr_t RVA_CAN_REMATCH  = 0x02EE6E30;  // FUN_142EE6E30
static constexpr size_t    REMATCH_WORD_LEN = 8;

// Original first 8 bytes: MOV [RSP+0x10],RBX / PUSH RDI / (SUB RSP,0x20 starts here)
static const uint8_t EXPECT_CAN_REMATCH[REMATCH_WORD_LEN] = {
    0x48,0x89,0x5C,0x24,0x10,   0x57,   0x48,0x83
};
// Patched: MOV AL,1 / RET, then bytes 3..7 byte-for-byte unchanged.
static const uint8_t PATCH_CAN_REMATCH[REMATCH_WORD_LEN] = {
    0xB0,0x01,   0xC3,   0x24,0x10,   0x57,   0x48,0x83
};

// ── BINDING ── Kept in one place so re-binding is a one-line change and does not touch the hotkey
// loop's logic. This used to ride on Ctrl+F9 because every key was taken; F4 was freed in the hotkey
// clean-up (its lobby-guard status is part of F3's full capture), so it now has a plain key of its
// own. To re-bind again, change these two lines and nothing else.

bool g_rematchUnlocked = false;

bool setRematchUnlock(bool on)
{
    const uintptr_t addr = g_base + RVA_CAN_REMATCH;
    if ((addr & 7) != 0) {
        logf("rematch unlock: %016llX is not 8-byte aligned — refusing, because the atomic store is "
             "the only thing making this race-free", (unsigned long long)addr);
        return false;
    }

    const uint8_t* want = on ? EXPECT_CAN_REMATCH : PATCH_CAN_REMATCH;
    const uint8_t* set  = on ? PATCH_CAN_REMATCH  : EXPECT_CAN_REMATCH;

    uint8_t cur[REMATCH_WORD_LEN] = { 0 };
    if (!safeRead((void*)addr, cur, REMATCH_WORD_LEN)) {
        logf("rematch unlock: cannot read %016llX — not applied", (unsigned long long)addr);
        return false;
    }
    for (size_t i = 0; i < REMATCH_WORD_LEN; ++i) if (cur[i] != want[i]) {
        logf("rematch unlock: SIGNATURE MISMATCH at byte %zu (want %02X got %02X) — refusing. "
             "Either this machine's build differs, or the patch state is not what we think it is.",
             i, want[i], cur[i]);
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)addr, REMATCH_WORD_LEN, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("rematch unlock: VirtualProtect failed, err=%lu", GetLastError());
        return false;
    }
    long long word = 0;
    memcpy(&word, set, REMATCH_WORD_LEN);
    _InterlockedExchange64((volatile long long*)addr, word);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, REMATCH_WORD_LEN);
    DWORD tmp = 0;
    VirtualProtect((void*)addr, REMATCH_WORD_LEN, oldProtect, &tmp);

    g_rematchUnlocked = on;
    return true;
}

#endif // diagnostic dumps and rematch experiment

// ---------------------------------------------------------------- ★★★ the spectator latch (6y)
//
// Run 20 found the variable, after five runs of correlations that were not it. The SAME machine, the
// SAME player, the SAME session-layer flags (`08000B7F`, 0 units held) reads:
//
//     3-player battle (gifting fails):  IsSpectator = 1
//     2-player battle (gifting works):  IsSpectator = 0
//
// So the battle layer decides "you are a spectator" differently at two players than at three, and a
// client it marks as a spectator cannot command a unit however correctly the unit was given to it.
// That single byte explains every symptom at once: the empty drag-box, the blue-not-green rendering,
// the motionless unit, and the "END BATTLE" button where a participant gets "CONCEDE DEFEAT".
//
// ❌ RUN 21: forcing this byte mid-battle CRASHED the game, and the decompiler says exactly why.
//
// `FUN_142E68E90` is the battle HUD constructor — it is the single writer of the battle-root global
// (`DAT_1443C85E0 = param_1`) — and it reads this same byte at least five times **while building**:
//
//     if ((cVar2 == '\0') && (*(char *)(FUN_142014A10(param_1+1) + 0x10) == '\0') && ...)
//         { ... loop calling FUN_142E7C260 per army entry ... }     // skipped for a spectator
//     if (... || *(char *)(FUN_142014A10(param_1+1) + 0x10) != '\0') goto LAB_142E6A7C9;  // skips a block
//
// So a spectator's HUD is *built differently*: whole subsystems are never constructed. Flipping the
// byte afterwards tells that half-built HUD to behave like a participant's, and it dereferences
// something that was never made. The flag is a **description of how this client was assembled**, not
// a switch that changes it.
//
// ★ Two things make this a good failure. It crashed **only on the machine that pressed F9** — so this
// is purely local client state with no lockstep involvement, and we can experiment on one machine
// without endangering anyone else's session. And it names the fix site: the byte has to be 0 *before*
// `FUN_142E68E90` runs, not after.
//
// The write is removed. F9 now only reports, which is still worth a key because F5's version is
// buried in a hundred-line dump.
// ---------------------------------------------------------------- ★★★ BUILD THE HUD AS A PARTICIPANT
//
// Run 21 established that `IsSpectator` cannot be flipped after the fact: `FUN_142E68E90`, the battle
// HUD constructor, branches on it at least five times *while building*, so a spectator's HUD is
// assembled without whole subsystems and telling it afterwards to behave like a participant's crashes.
//
// So write the byte BEFORE those tests instead. `FUN_141F0D510` is the base constructor the HUD calls
// first, and it has **exactly one caller in the image** — `FUN_142E68E90` itself. That makes it a
// precise hook: nothing else in the game can be affected by it.
//
// Order inside FUN_142E68E90:
//     1. vtable slot writes
//     2. FUN_141F0D510(this, battle, 0)     <-- initialises the embedded object at this+8
//     3. ... setup ...
//     4. DAT_1443C85E0 = this
//     5. first test of the flag
//
// The hook has to run *after* step 2, not at step 1: `FUN_141F0D510` does `ADD RCX,8; CALL ...` to
// construct that embedded object, so at entry `*(this+8)` is not yet valid. Calling the original
// first and writing afterwards puts the write exactly between steps 2 and 5.
//
// ⚠ Only `+0x10` is cleared. `+0x11` sits in the same struct and was identified in an earlier session
// as the gift/return direction flag (RVA_SHARE_MODE above); the constructor's army-list condition
// wants both clear, but changing a flag whose meaning we know to be something else is how run 21
// happened. This logs it instead, loudly if it is non-zero, and that is the next thing to learn.
static constexpr uintptr_t RVA_HUD_BASE_CTOR  = 0x01F0D510;  // FUN_141F0D510
static constexpr size_t    HUDCTOR_STOLEN_LEN = 15;          // 3 whole MOVs, none RIP-relative
static constexpr size_t    OFF_Q_SPECTATOR_DUP = 0x378;      // the second copy of the struct (6dd)

typedef void* (*HudBaseCtorFn)(void*, void*, uint8_t);

Detour        g_hudCtorDetour;
static HudBaseCtorFn g_origHudBaseCtor = nullptr;

// ★★ ON BY DEFAULT since run 28. A spectator in a multiplayer battle is built able to receive and
// command lent units without anyone pressing anything. See the long comment in hudBaseCtorHook for
// why this is safe — the short version is that the state is inert until somebody gifts you a unit.
// Kept as a flag rather than hard-coded so it can be turned off without a rebuild if it ever needs
// to be ruled out of a run.
volatile bool g_autoLendingSpectator = true;

// ================================================================================================
// ★★★★★ #13 / B10 — THE SIDE AUDIT. Read-only. The FIX lives in `sidegate.cpp`.
// ================================================================================================
//
// ★ THE SYMPTOM, stated as ONE rule (tester, 2026-08-17), and it is what finally made the ticket
// tractable — worth more than every mechanism theory that came before it:
//
//     THE SPECTATOR ALWAYS LANDS ON THE ATTACKER'S SIDE.
//
//       * the human ATTACKS  -> the attacker is the human -> the spectator joins them. LOOKS RIGHT.
//       * the human DEFENDS  -> the attacker is the AI    -> the spectator joins the AI.  THE BUG.
//
// ⇒ Not "broken half the time": ONE rule that happens to be right half the time. Any fix that
// inverts or replaces the selection breaks the working half.
//
// ✅ CAUSE FOUND 2026-08-18 (§6uuu.53), and it is NOT in this file. The battle DESCRIPTOR builder
// `FUN_141864380` sets the local player's army index, and for a player with no army in the fight it
// does so only `if (humans == 2)`. Above two humans it falls through to a literal `idA = 0`, and
// `armies[0]` is the attacker. ⇒ The fix is one byte in `sidegate.cpp`, armed at attach.
//
// ⇒ WHAT THIS BLOCK IS NOW: the read-only measurement that got us there, kept because it is the only
// thing that reports a spectator's army AS IT IS BUILT. It logs `myArmy`/`myAlliance` at HUD-base-ctor
// time — ✅ and it answered its own question: `+0x160` IS already populated there, which is how we
// knew the choice was made upstream and stopped looking in the battle layer.
//
// ---------------------------------------------------------------------------- reading the sides
//
// ★ OUR OWN SIDE IS SOLID, and it is the engine's own expression. `FUN_141F719A0(battleRoot)` — the
// accessor `IsEnemyUnit` compares against — decompiles to exactly one line:
//
//     FUN_141F719A0(br)  =  BattleArmy_GetAlliance(*(br + 0x160))  =  *(*(br + 0x160) + 0xA0)
//
// which is the pair of dereferences `dumpBattleSides` already does. ✓ So `myAlliance` is right, and
// the engine compares alliances by POINTER IDENTITY (§6jjj) — no index mapping to get wrong.
//
// ⚠⚠ THE OTHER PLAYERS' SIDES ARE NOT. The obvious walk — slot -> unit entry -> `*(entry+0x50)` ->
// `+0x590` -> army -> `+0xA0` — came back as NON-POINTER GARBAGE on 2026-08-05 (`gift.cpp`, and
// RUN41 records the same table printing the local client as its own ENEMY). `+0x590` itself is
// measured correct for a battlefield unit (`FUN_142379DD0` is that one instruction), so the fault is
// upstream: `*(entry+0x50)` is not a battlefield unit on this path. That walk is therefore REFUSED
// here rather than reused — a gate built on a read that is known to lie is worse than no gate.
//
// ⇒ SO THE GATE STAYS IN ONE NUMBER SPACE: the session slot flags, where `alliance = flags & 3` is
// the field THE ENGINE'S OWN share-target search matches on (`FUN_14046FBC0`, §6iii). Comparing
// slot flag to slot flag crosses nothing. ✗ It is deliberately NOT compared against the alliance
// POINTER above — no mapping between those two spaces has ever been established, and inventing one
// is the exact mistake this project has paid for repeatedly. The audit prints both so that one
// battle settles whether they agree; until then only the flags decide.
//
// ⚠⚠⚠ AND A HAZARD THAT IS OURS. `flags & 3` is the same field `session.cpp` calls SLOT_TEAM_MASK —
// the lobby TEAM. The join hook raises the advertised player slots 2 -> 4 and leaves the advertised
// TEAM count at 2, so `i % teams` gives slots the teams 0,1,0,1 where every one of 48 logged vanilla
// lobbies had teams == slots (one team each). ⇒ At 3+ players this field is CARRYING OUR BUG, and
// two players can share a team who are not on the same battle side. The audit logs the raw flags of
// every slot for exactly that reason. Fixing the advertised team count is its own ticket; if it
// lands first, this gate gets more trustworthy, not less.
//
// ★ Read-only, called from inside the HUD constructor: pointer walks with plausibility refusals, no
// engine call, no allocation. The same rule the rest of this file follows.

enum class SpectatorSide { Unknown, WithAHuman, OppositeEveryHuman };

int localPlayerId();   // defined below, next to the other session reads

#ifndef TW3K_RELEASE
static SpectatorSide auditSpectatorSide(uintptr_t br)
{
    // ⚠ `br` is the HUD constructor's own `this`, NOT the battle-root global: `DAT_1443C85E0 = this`
    // happens at step 4 of FUN_142E68E90 and we are hooked at step 2, so the global is still whatever
    // the PREVIOUS battle left in it. Reading it here would have been a stale-pointer finding.
    uintptr_t myArmy = 0, myAlliance = 0;
    readAt(br + OFF_BR_MY_ARMY, myArmy);

    if (myArmy && myArmy <= 0x10000) {
        logf("   SIDE AUDIT: myArmy(+0x160)=%016llX is not a pointer — REFUSING to read further.",
             (unsigned long long)myArmy);
        return SpectatorSide::Unknown;
    }
    if (!myArmy) {
        // ⚠ EXPECTED, POSSIBLY. Nothing establishes that +0x160 is populated this early in the
        // constructor — that is precisely what this audit is here to find out on the first battle
        // that runs it. If it always reads 0, the gate cannot live at this hook and has to move
        // later into FUN_142E68E90; that is a finding, not a failure.
        logf("   SIDE AUDIT: myArmy(+0x160) reads 0 at HUD-base-ctor time. Either this client has no "
             "army (a true spectator) or the field is not written yet. ⇒ verdict UNKNOWN, and the "
             "participant build proceeds exactly as before.");
        return SpectatorSide::Unknown;
    }
    readAt(myArmy + OFF_ARMY_ALLIANCE, myAlliance);
    logf("   SIDE AUDIT: myArmy=%016llX  myAlliance=%016llX  (= the engine's own FUN_141F719A0)",
         (unsigned long long)myArmy, (unsigned long long)myAlliance);

    const uintptr_t mp = capturedMp();
    uintptr_t slotObj = 0;
    if (!mp || !readAt(mp + OFF_MP_SLOTOBJ, slotObj) || !slotObj) {
        logf("   SIDE AUDIT: no MP session captured — the slot flags are unavailable. verdict UNKNOWN.");
        return SpectatorSide::Unknown;
    }

    uint32_t slots = 0;
    if (!readAt(slotObj + OFF_SLOT_COUNT, slots) || slots == 0 || slots > SLOT_MAX_ENTRIES) {
        logf("   SIDE AUDIT: slot count reads %u — not credible, REFUSING the walk. verdict UNKNOWN.",
             slots);
        return SpectatorSide::Unknown;
    }

    // ★ #45: a player id is NOT a slot index. The engine's own conversion is the table at +0x137C,
    // and every handler that needs it does exactly this line. Getting it wrong is how a gift once
    // landed on whoever happened to hold that slot.
    const int pid = localPlayerId();
    uint32_t  mySlot = 0xFFFFFFFF;
    if (pid >= 0 && (uint32_t)pid < MAX_SESSION_PLAYERS)
        readAt(slotObj + OFF_SLOT_PLAYER_TABLE + (uintptr_t)pid * 4, mySlot);

    if (mySlot >= slots) {
        logf("   SIDE AUDIT: this client's player id %d maps to slot %u of %u — cannot place "
             "ourselves in the slot list. verdict UNKNOWN.", pid, mySlot, slots);
        return SpectatorSide::Unknown;
    }

    uint32_t myFlags = 0;
    if (!readAt(slotObj + (uintptr_t)mySlot * SLOT_ENTRY_STRIDE + SLOT_ENTRY_FLAGS, myFlags)) {
        logf("   SIDE AUDIT: slot %u flags unreadable. verdict UNKNOWN.", mySlot);
        return SpectatorSide::Unknown;
    }
    const uint32_t myTeam = myFlags & SLOT_TEAM_MASK;

    logf("   SIDE AUDIT: player id %d = slot %u  flags=%08X  team(flags&3)=%u", pid, mySlot,
         myFlags, myTeam);

    int  others = 0, sharing = 0;
    for (uint32_t s = 0; s < slots; ++s) {
        if (s == mySlot) continue;
        uint32_t f = 0;
        if (!readAt(slotObj + (uintptr_t)s * SLOT_ENTRY_STRIDE + SLOT_ENTRY_FLAGS, f)) continue;
        if (f & SLOT_FLAG_VACANT) continue;                 // an open seat is nobody

        ++others;
        const uint32_t team = f & SLOT_TEAM_MASK;
        if (team == myTeam) ++sharing;
        logf("      slot %u  flags=%08X  team=%u  spectator=%u   %s", s, f, team,
             (f & SLOT_FLAG_SPEC) ? 1u : 0u,
             (team == myTeam) ? "<== SAME TEAM as this client" : "other team");
    }

    if (others == 0) {
        logf("   SIDE AUDIT: no other occupied slot resolved, so there is nothing to compare "
             "against. verdict UNKNOWN.");
        return SpectatorSide::Unknown;
    }
    if (sharing > 0) {
        logf("   SIDE AUDIT: %d of %d other player(s) share this client's team ⇒ WITH A HUMAN. This "
             "is the half that already works, and nothing is withheld.", sharing, others);
        return SpectatorSide::WithAHuman;
    }
    logf("   SIDE AUDIT: NONE of the %d other player(s) share this client's team ⇒ OPPOSITE EVERY "
         "HUMAN. This is the #13 shape: the human is defending and we have been placed with the "
         "attacker.", others);
    return SpectatorSide::OppositeEveryHuman;
}

// ✂ RETIRED 2026-08-04 (tester): `g_buildAsParticipant` (F9) and `g_suppressForeignArmies` (F7).
//
// Both were manual arming for the two bytes that make a lending spectator, from when this was an
// experiment run one byte at a time so a run could tell them apart. Auto-arming has set BOTH since
// run 28 and is confirmed in game on three machines with nothing pressed — so the flags could only
// ever be false, and every condition below reduced to `autoArm` on its own.
//
// ⇒ Removed rather than left inert. A flag that is always false reads as a live option to whoever
// finds it next, which is exactly the complaint that got plan B deleted: *it read as working
// machinery in the log and was not.*
#endif

static long          g_hudCtorCalls = 0;

static void* hudBaseCtorHook(void* self, void* battle, uint8_t flag)
{
    resetGiftPanelState("battle HUD construction");
    void* r = g_origHudBaseCtor ? g_origHudBaseCtor(self, battle, flag) : nullptr;
    TW3K_DIAGNOSTIC(++g_hudCtorCalls);

    // Same chain the game uses: FUN_142014A10(self+8) = *(self+8) + 0x350.
    uintptr_t p = 0;
    if (!readAt((uintptr_t)self + OFF_BR_LOCAL_PLAYER, p) || p <= 0x10000) {
        logf("HUD ctor: embedded object at self+8 unreadable — leaving everything alone");
        return r;
    }
    const uintptr_t mode = p + 0x350;

    // The battle object holds TWO copies of this struct (6dd): FUN_141FB6180 builds one at Q+0x350
    // and immediately a second at Q+0x368, so a duplicate spectator byte lives at Q+0x378. Nothing is
    // known to read the duplicate — no accessor of the standard form exists for it — so we do not
    // touch it. But printing it costs nothing and turns "the HUD came out half-participant" from an
    // investigation into a glance.
    uint8_t spec = 0xFF, dir = 0xFF, dup = 0xFF;
    safeRead((void*)(mode + OFF_MODE_SPECTATOR), &spec, 1);
    safeRead((void*)(mode + OFF_MODE_DIRECTION), &dir, 1);
    safeRead((void*)(p + OFF_Q_SPECTATOR_DUP), &dup, 1);
    diagLogf("HUD ctor #%ld: IsSpectator=%u  +0x11=%u  (mode struct %016llX)  [duplicate copy at "
         "Q+0x378 reads %u - not modified]",
         g_hudCtorCalls, spec, dir, (unsigned long long)mode, dup);

    // ★★ AUTOMATIC, since run 28. This used to require F9, and the arming is what kept costing runs:
    // run 26 never pressed F7 and pressed F11 on the giver, run 27 double-tapped F9 so one lender
    // built a genuine spectator HUD, run 28 could not be tested at all because nobody was armed.
    // Worse than losing a test, a mis-arm is now known to cost a real unit: §6rr measured a
    // misclassified client RECEIVING a gift it could then neither see nor hand back, leaving the
    // unit standing on the field under nobody's control for the rest of the battle.
    //
    // ⇒ The honest reading is that this was never a per-machine choice. In a coop battle there is no
    // case where we want a spectator to be UNABLE to receive units.
    //
    // ★ The argument that makes this safe: THE STATE IS INERT UNTIL UNITS ARRIVE. `+0x10 = 0` grants
    // command over what this client owns, and a spectator owns nothing — so a pure spectator who is
    // never gifted anything is unaffected, and behaves exactly as before. It only starts to matter at
    // the moment somebody gifts them something, which is precisely when we need it to have been set.
    //
    // ⚠ This does mean an un-armed client is no longer byte-for-byte vanilla in a battle, which the
    // project's own safety rule used to guarantee. That rule was written when this was experimental;
    // the lending round trip has been proven end to end three times now (runs 25, 25b, 27). F9 and F7
    // stay as manual overrides, and F1 still removes everything.
    const bool autoArm = g_autoLendingSpectator && spec != 0 && capturedMp() != 0;

    // ★★★ #13 / B10 — MEASURE ON EVERY SPECTATOR BUILD, ACT ONLY WHEN ARMED.
    //
    // The audit runs even with the gate disarmed, and even with `lend off`, because #13 has
    // reproduced twice in many battles: a session that fires it with nobody at a keyboard should
    // still leave the verdict in the log rather than needing to be caught live.
    //
    // ★ And when the gate is DISARMED it says what it WOULD have done. That is the whole point of
    // the split: a run can confirm the predicate fires in exactly the defensive case and stays quiet
    // in the attacking one WITHOUT changing a single byte of behaviour. Arming comes after that, not
    // before it — `hfcount` was armed on a theory once and it cost two sessions to unpick.
    // ✂ THE GATE THAT USED TO BE HERE IS GONE, and the reason is worth more than the code was.
    //
    // `lend guard` withheld the participant build when no other player shared this client's
    // `flags & 3`. ❌ That predicate is WRONG: `flags & 3` is the lobby team at LOBBY time only — by
    // battle time the low bits are rewritten (`0B7F` on spectators, `0341` on the host, and `&3 = 3`
    // is not a valid team at `teams=2`). What it actually separated was participant from spectator,
    // which is true in BOTH halves of the bug, so it could never discriminate.
    //
    // ⇒ Deleted rather than left disarmed. A switch that cannot work reads as a live option to
    // whoever finds it next — the same complaint that got plan B and the F7/F9 keys removed.
    //
    // ★ The real fix is `sidegate.cpp`, armed at attach, one byte, in the descriptor builder — the
    // layer that actually chooses. The audit below stays: it is read-only, it is what caught the
    // wrong predicate, and it is the only thing that reports a spectator's army as it is built.
#ifndef TW3K_RELEASE
    if (spec != 0 && capturedMp() != 0)
        auditSpectatorSide((uintptr_t)self);
#endif

    if (autoArm) {
        diagLogf("   >>> AUTO LENDING SPECTATOR: this client is a spectator in a multiplayer battle, so "
             "it is being built able to receive and command lent units. No keypress needed.");
        diagLogf("       (This state does nothing at all until somebody gifts you a unit.)");
    }

    if (!autoArm) {
        if (spec) {
            diagLogf("   this client will build a SPECTATOR HUD, and auto-arming did NOT apply: "
                 "autoLending=%d spectator=%u mpSession=%d",
                 g_autoLendingSpectator ? 1 : 0, spec, capturedMp() ? 1 : 0);
        }
        return r;
    }
    if (!spec && !dup) { diagLogf("   already a participant — nothing to do."); return r; }

    // ★ RUN 22 (6ff): clearing ONLY the +0x350 copy produced a genuinely half-participant client -
    // units green and commandable, but box-select dead and the end-battle menu misbehaving. The
    // duplicate at Q+0x368 is NOT dead storage: FUN_142019AB0 passes `Q+0x368` wholesale to
    // FUN_141FB8CF0 and `Q+0x378` as its own argument to FUN_141FCE6C0. So a client whose two copies
    // disagree is inconsistent in exactly the way run 21 taught us to avoid.
    //
    // Both copies are initialised identically from the same descriptor by FUN_141FB6180, so setting
    // them together RESTORES the engine's own invariant rather than inventing a state.
    struct Poke { uintptr_t addr; const char* tag; uint8_t before; };
    Poke pokes[] = {
        { mode + OFF_MODE_SPECTATOR, "+0x360 (live copy)", spec },
        { p + OFF_Q_SPECTATOR_DUP,   "+0x378 (duplicate)", dup  },
    };

    for (Poke& k : pokes) {
        if (k.before == 0) continue;
        DWORD oldProtect = 0;
        if (!VirtualProtect((void*)k.addr, 1, PAGE_READWRITE, &oldProtect)) {
            logf("   !! VirtualProtect failed for %s (%lu) - NOT modified", k.tag, GetLastError());
            continue;
        }
        *(volatile uint8_t*)k.addr = 0;
        DWORD tmp = 0;
        VirtualProtect((void*)k.addr, 1, oldProtect, &tmp);

        uint8_t after = 0xFF;
        safeRead((void*)k.addr, &after, 1);
        diagLogf("   >>> %s: %u -> %u", k.tag, k.before, after);
    }
    diagLogf("   >>> done BEFORE the HUD is built. This client should construct itself as a participant.");

    // ★ RUN 25 (6ll): the OTHER byte, and why it now has a job.
    //
    // `FUN_142E68E90` registers every army in the battle with this client's HUD, and the condition
    // guarding that loop reads BOTH bytes:
    //
    //     if (FUN_141F6D490(this) == 0 && mode->+0x10 == 0 && mode->+0x11 == 0) {
    //         for (each entry in vector at this[0x2C])  FUN_142E7C260(this, entry);
    //     }
    //
    // Clearing +0x10 to gain command therefore TURNS THAT LOOP ON for a client that owns nothing —
    // which is exactly what run 25 saw: the whole army's cards on every armed machine from the first
    // second of the battle, while the model correctly reported UNITS HELD=0.
    //
    // Setting +0x11 skips the loop, and the honest argument for it is that the model is already
    // right: this client holds nothing, so a HUD that shows it nothing is the HUD telling the truth.
    // Gifted units arrive through a different, dynamic path (run 25 watched them appear and, after
    // the hand-back, disappear), so nothing that was working is being given up.
    //
    // ⚠ Two things to keep honest about it:
    //   * `+0x10 = 0` with `+0x11 = 1` is a combination the engine does not produce on its own. A
    //     natural spectator has both set; a natural participant has both clear.
    //   * the mode struct survives to click time — run 25 logged the same address at HUD
    //     construction and at the gift click — so this also leaves the client's gift button in the
    //     RETURN direction, which is what a holder of borrowed units wants anyway. If it holds, F11
    //     becomes unnecessary and will say so ("already reads 1 - nothing forced").
    //
    // Auto-arming sets BOTH bytes, because `+0x10 = 0` alone is the half-state run 25 measured:
    // clearing it to gain command switches ON the army-registration loop for a client that owns
    // nothing, so every other player's unit cards appear on a HUD that cannot command any of them.
    // `+0x10 = 0, +0x11 = 1` is the pair run 25b proved — "commands units, is not a combatant".
    // (This used to be armable on its own with F7, so a run could tell the two bytes apart. That
    // question is long settled and the key is retired.)
    if (autoArm) {
        if (dir != 0) {
            diagLogf("   >>> +0x11 already reads %u - the army-list loop was going to be skipped "
                 "anyway. Nothing written.", dir);
        } else {
            DWORD oldProtect = 0;
            if (!VirtualProtect((void*)(mode + OFF_MODE_DIRECTION), 1, PAGE_READWRITE, &oldProtect)) {
                logf("   !! VirtualProtect failed for +0x11 (%lu) - NOT modified", GetLastError());
            } else {
                *(volatile uint8_t*)(mode + OFF_MODE_DIRECTION) = 1;
                DWORD tmp = 0;
                VirtualProtect((void*)(mode + OFF_MODE_DIRECTION), 1, oldProtect, &tmp);

                uint8_t after = 0xFF;
                safeRead((void*)(mode + OFF_MODE_DIRECTION), &after, 1);
                diagLogf("   >>> +0x11 (army-list gate): %u -> %u. This client will NOT register the "
                     "other players' armies with its HUD, so it starts the battle holding nothing "
                     "and showing nothing. Gifts still arrive normally.", dir, after);
            }
        }
    } else if (dir == 0) {
        diagLogf("   >>> note: +0x11 reads 0, so this client WILL register every army in the battle with "
             TW3K_MODE_TEXT("its HUD and show cards for units it does not own (run 25). F7 suppresses that.", "its HUD and show cards for units it does not own (run 25). The automatic spectator fix suppresses that."));
    }
    return r;
}

static const uint8_t EXPECT_HUD_BASE_CTOR[HUDCTOR_STOLEN_LEN] = {
    0x48,0x89,0x5C,0x24,0x10,        // MOV [RSP+0x10], RBX
    0x48,0x89,0x74,0x24,0x18,        // MOV [RSP+0x18], RSI
    0x48,0x89,0x7C,0x24,0x20         // MOV [RSP+0x20], RDI
};

bool installHudCtorHook()
{
    return detourInstall(g_hudCtorDetour, g_base + RVA_HUD_BASE_CTOR, HUDCTOR_STOLEN_LEN,
                         EXPECT_HUD_BASE_CTOR, (uintptr_t)&hudBaseCtorHook,
                         (void**)&g_origHudBaseCtor, "HUD ctor");
}

void removeHudCtorHook() { detourRemove(g_hudCtorDetour, "HUD ctor"); }

// The address of the IsSpectator byte, or 0 if we are not in a battle. Pure reads, no game calls:
// FUN_1430A50B0 reads +0x10 of FUN_142014A10(battleRoot + 8), and FUN_142014A10 is `return *p+0x350`.
// Which player id is this machine? Run 23 was lost to not knowing: the recipient armed itself, the
// giver gifted "as usual", vanilla took list[0] — and list[0] was the OTHER spectator. Two units went
// to a machine that was still a spectator, the armed machine held nothing, and the whole run answered
// neither question it was built to answer.
//
// So the arm message now states the id out loud, because the giver has to aim at it.
// Same virtual dumpMpSession() has called for many runs, wrapped the same way.
int localPlayerId()
{
    const uintptr_t mp = capturedMp();
    if (!mp) return -1;
    int id = -1;
    __try {
        uintptr_t lp = 0;
        if (readAt(mp + OFF_MP_LOCAL_PLAYER, lp) && lp > 0x10000) {
            const uintptr_t lpVt = *(uintptr_t*)lp;
            if (lpVt > 0x10000)
                id = (int)((uint32_t(*)(void*))(*(uintptr_t*)(lpVt + 0x20)))((void*)lp);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { id = -1; }
    return id;
}

// ✂ `spectatorByteAddr()` lived here and is retired with F9, its only caller. Its run-22 guard was
// worth more than the function — it is now inline in `dumpMpSession`, above.
