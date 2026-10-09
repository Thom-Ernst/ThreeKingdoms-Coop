// gift.cpp - Gifting: share instrumentation, per-player targeting, the unit census, and the CcoBattleRoot UI entries.
//
// Split out of the original single-file tw3k_coop.cpp. Behaviour is unchanged: this was pure
// code motion. ../tw3k_coop.ASBUILT.cpp remains the known-good single-file snapshot.

#include "tw3k.h"

// The command object the engine's own slot search hangs off: it walks *(cmdObj + 0xD3830), NOT
// *(EMPIRE_MP + 0xD3830). Cached here by the one place that resolves it, so the census can compare
// the two arrays against the right base. Zero until a gift click has resolved it.
static uintptr_t g_lastCmdObj = 0;


// ------------------------------------------- gift/share units instrumentation
//
// FUN_142EA8580(battleMgr) is the "Gift units" click handler (3K calls it Share internally; only
// the localised tooltip says gift). It picks its target by asking a service for the player list
// and taking list[0] — that single line is the whole 2-player assumption. The core command it
// then issues, virtual +0x240(this, targetPlayerId, unit, 0), already accepts ANY player id.
//
// So the one thing we need from a 3-player session is: does that list hold every OTHER player?
// If it holds 2 entries with 3 players, gifting to a chosen player is just "index N, not 0".

static constexpr uintptr_t RVA_SHARE_UNITS   = 0x02EA8580; // FUN_142EA8580
static constexpr size_t    SHARE_STOLEN_LEN  = 16;         // 4 whole insns, none RIP-relative
static constexpr uintptr_t RVA_GET_SHARE_SVC = 0x01FF6530; // FUN_141FF6530
static constexpr uintptr_t RVA_GAME_FREE     = 0x00670570; // FUN_140670570
static constexpr uintptr_t RVA_SHARE_MODE    = 0x02014A10; // FUN_142014A10 -> the mode struct
// Two bytes of that one struct, declared together because they are read together. +0x10 is
// IsSpectator (the byte the HUD-ctor hook clears, 6bb/6dd); +0x11 is the gift/return direction. The
// battle HUD constructor tests both, which is why clearing one changes the meaning of the other.
static constexpr uintptr_t RVA_SHARE_ARG_A   = 0x01FF52F0; // FUN_141FF52F0(mode) -> 1st arg of vt+0x230
static constexpr uintptr_t RVA_SHARE_ARG_B   = 0x01FF5A30; // FUN_141FF5A30(mode) -> 2nd arg of vt+0x230
static constexpr size_t    OFF_BM_SELECTION  = 0xE28;      // battleMgr -> selection object
static constexpr size_t    OFF_SEL_COUNT     = 0x154;
static constexpr size_t    OFF_SEL_UNITS     = 0x158;      // -> array of unit pointers
static constexpr size_t    VT_PLAYER_LIST    = 0x248;      // virtual: get player list
static constexpr size_t    VT_OWNER_ID       = 0x230;      // virtual: (argA,argB) -> owning player id
static constexpr size_t    VT_PLAYER_RECORD  = 0x228;      // virtual: playerId -> that player's record

typedef void  (*ShareUnitsFn)(void*);
typedef void* (*GetShareSvcFn)(void*);
typedef void  (*GameFreeFn)(void*);

Detour        g_shareDetour;
static ShareUnitsFn  g_origShareUnits = nullptr;
static volatile bool g_logShareList   = true;
static void* volatile g_lastBattleMgr = nullptr;   // last mgr seen in a gift click, for the F6 census

// ✂ F11 / FORCE RETURN retired 2026-08-04. The reasoning is kept because it is still the clearest
// statement of how the two directions differ, and the gift code below depends on that:
//
//   In the GIFT direction the handler searches the COMPUTED id's list and sends to `list[0]`; in the
//   RETURN direction it does the opposite. Run 24 measured both on the holder's own machine —
//   computed = 2 (the army owner), `list[0]` = 0 (itself) — so GIFT searched the owner's list for
//   units that were in its own: no match, no command. RETURN searches its own list and sends to the
//   owner, which is exactly a hand-back.
//
// F11 forced `+0x11 = 1` for one click to get that. It is unnecessary now: auto-arming sets `+0x11`
// at HUD construction for every lending spectator, and §6fff fixed the real defect underneath —
// the RETURN source is forced to the LOCAL player, the only source the engine's own sender-side
// check accepts.

// ---- gift-target UI state (see runtime/GIFT_UI.md) --------------------------------------------------
//
// PANEL MODE, armed with F6. With it on, clicking the gift icon does not gift: it raises
// g_giftPanelWanted and returns, leaving the selection untouched, so the pack's panel can appear and
// the player can choose a recipient. Choosing one calls `GiftUnits(id)`, which arms that id and
// re-enters the handler for real.
//
// The DLL never touches a widget. It answers a query the pack binds visibility to, and it exposes
// actions the pack binds buttons to — so presentation stays entirely pack-side and the placeholder
// panel can be replaced without a DLL rebuild.
volatile bool g_giftPanelMode   = false;   // F6: is there a panel to defer to?
volatile bool g_giftPanelWanted = false;   // what the GiftPanelOpen query answers
static volatile bool g_giftFromPanel   = false;   // re-entry guard: this click came from a button

// Defined further down, next to the list it builds. Declared here because the OPEN click has to
// build that list BEFORE it raises g_giftPanelWanted — see the note at the toggle (#54).
static void refreshRecipientList();

// Standard CA dynamic array shape, same as everywhere else in this engine.
struct CaVec32 { uint32_t capacity; uint32_t count; int32_t* data; };

// ---- the two index spaces, and the one place this file crosses between them (#45) ---------------
//
// ★★★ A PLAYER ID IS NOT A SLOT INDEX. This file spent months assuming it was, and every session
// agreed, because the seat hook makes seats contiguous and contiguous seats make the two numbers
// equal. On 2026-08-08 a player left and rejoined, took slot 3 and left slot 2 empty, and the gift
// panel sent real units to the wrong person.
//
// ⇒ Everything user-facing here now names which space it is in, and there is exactly ONE crossing:
// `slotForPlayer`. If a variable is called `id` it is a player id; a slot is called `slot`.
//
// ★ MEASURED, and it is the opposite of what wiki/multiplayer.md's command table used to say: the
// share list and `0xA9`'s target are **slot indices**. From the host's own log of that session
// (`tklogs/tw3k_coop_PWD11_20260808-002646.log`), where the players were ids 0/1/2 seated 0/3/1:
//
//     >>> SHARE PLAYER LIST: count=2      list[0] = 1      list[1] = 3
//
// There is no player 3 in that session — 1 and 3 are the occupied non-local SLOTS. And the unit
// census, which is indexed by slot, shows where a forced target=1 actually landed: slot 1 gained
// the three units and slot 3 did not. Slot 1 was player 2.
//
// ⚠ NOT established: whether the `senderId` field of the same payload is also a slot. The engine
// derives it locally (`FUN_140471AB0` calls `vtable[0x20]` itself) so we never supply it, and
// nothing here depends on the answer. `FUN_140471B30` bounds it against `+0x1360`, the slot COUNT,
// which suggests slot — but that is a reading, not a measurement, and it stays out of the wiki.

// The engine's own conversion, and the only one in this file. -1 means "no answer" — the table is
// unreadable, unpopulated for that id, or the id is out of the engine's own range.
//
// ⚠ Callers must treat -1 as REFUSE, never as "use the id instead". Falling back to the identity is
// precisely the bug: an unvalidated target is written straight into the payload, so a wrong slot
// hands the units to a real other player and an empty one destroys them.
static int slotForPlayer(int playerId)
{
    if (playerId < 0 || (uint32_t)playerId >= MAX_SESSION_PLAYERS) return -1;
    const uintptr_t mp = capturedMp();
    if (!mp) return -1;
    uint64_t slotObj = 0;
    if (!readAt(mp + OFF_MP_SLOTOBJ, slotObj) || !slotObj) return -1;
    uint32_t slot = 0xFFFFFFFF;
    if (!readAt((uintptr_t)slotObj + OFF_SLOT_PLAYER_TABLE + (uintptr_t)playerId * 4, slot))
        return -1;
    if (slot == 0xFFFFFFFF || slot >= MAX_SESSION_PLAYERS) return -1;
    return (int)slot;
}

// The name cache is keyed by LOBBY RECORD id. Verified in Ghidra 2026-09-12:
// FUN_142D71B90 copies an enumerated service id into record+0x10 and passes that SAME id to
// FUN_14046DAB0. The latter directly addresses service->slotObj + id*0xF8 (bounded by +0x1360).
// Thus lobby-record id denotes a slot; the compact +0x137C roster index must cross the table.
// This is an identity join through the engine mapping, never a button/roster position fallback.
static int lobbyRecordIdForRecipient(int recipient)
{
    return slotForPlayer(recipient);
}

// ★ Exposed for `control.cpp`'s `gift <playerId>`, which has to make the SAME player->slot crossing
// the panel makes. Two places arming one field in two different spaces is the bug this closes.
int slotForPlayerPublic(int playerId) { return slotForPlayer(playerId); }

// The reverse crossing, by search — the engine keeps no slot->player table, so this is the only way
// back. -1 when no player claims that slot, which is the normal reading of a vacant one.
//
// ⚠ Used only to LABEL a slot for a human reader. Nothing addresses anything by the result: going
// slot -> player -> slot would just be an expensive identity with two more places to be wrong.
int playerForSlot(int slot)
{
    if (slot < 0) return -1;
    for (uint32_t id = 0; id < MAX_SESSION_PLAYERS; ++id)
        if (slotForPlayer((int)id) == slot) return (int)id;
    return -1;
}

// One SLOT's flags. `FUN_1404AB370` and `FUN_14046FBC0` between them define every bit this file
// cares about:
//     bit 6 + bit 8 set, bit 10 clear  = occupied
//     flags & 3                        = alliance
//     (flags >> 2) & 0xF               = army
static bool slotFlagsBySlot(int slot, uint32_t& out)
{
    if (slot < 0 || (uint32_t)slot >= MAX_SESSION_PLAYERS) return false;
    const uintptr_t mp = capturedMp();
    if (!mp) return false;
    uint64_t slotObj = 0;
    if (!readAt(mp + OFF_MP_SLOTOBJ, slotObj) || !slotObj) return false;
    return readAt((uintptr_t)slotObj + (uintptr_t)slot * SLOT_ENTRY_STRIDE + SLOT_ENTRY_FLAGS, out);
}

// Reproduces the handler's own list fetch. Safe because we run on the game's thread, inside the
// click, with exactly the state the original would see. Any fault disables further attempts
// rather than risking a repeat.
#ifndef TW3K_RELEASE
static void logSharePlayerList(void* mgr)
{
    __try {
        auto  getSvc = (GetShareSvcFn)(g_base + RVA_GET_SHARE_SVC);
        void* svc    = getSvc((char*)mgr + 8);
        if (!svc) { logf("  share: service ptr null"); return; }
        const uintptr_t inner = *(uintptr_t*)svc;
        if (!inner) { logf("  share: inner ptr null"); return; }

        void*           obj = (void*)(inner + 8);
        const uintptr_t vt  = *(uintptr_t*)obj;
        if (!vt) { logf("  share: vtable null"); return; }

        auto    fn  = (void(*)(void*, void*))(*(uintptr_t*)(vt + VT_PLAYER_LIST));
        CaVec32 out = { 0, 0, nullptr };
        fn(obj, &out);

        // ⚠ The entries are SLOT indices, measured — see the index-space note at the top of this
        // file. The name "share PLAYER list" is the engine's shape, not a claim about the space.
        logf("  >>> SHARE PLAYER LIST: count=%u   (the handler always gifts to list[0]; entries are "
             "SLOT indices, not player ids)", out.count);
        int32_t shareIds[16];
        int     shareN = 0;
        if (out.data && out.count && out.count < 64) {
            for (uint32_t i = 0; i < out.count; ++i) {
                if (shareN < 16) shareIds[shareN++] = out.data[i];
                const int owner = playerForSlot(out.data[i]);
                logf("      list[%u] = slot %d (%s)%s", i, out.data[i],
                     owner >= 0 ? knownPlayerName(lobbyRecordIdForRecipient(owner)) : "no player id claims that slot",
                     i == 0 ? "   <-- HARDCODED TARGET" : "   <-- unreachable in vanilla");
            }
        }
        if (out.count >= 2)
            logf("  >>> list holds >1 other player: gifting to a chosen player is an INDEX change.");
        else
            logf("  >>> only %u entry: cannot yet conclude the list generalises.", out.count);
        const int32_t list0 = (out.data && out.count) ? out.data[0] : -1;
        if (out.data) ((GameFreeFn)(g_base + RVA_GAME_FREE))(out.data);

        // ---- resolve what a targeting fix would actually have to hook -------------------------
        //
        // The handler issues its command as a virtual: cmd = FUN_142001730(mgr+8), then
        // (*(cmd+8)->vtable[0x240])(cmd+8, targetPlayerId, unit, 0). That call already takes the
        // target as a parameter, so per-player gifting is a one-argument rewrite — but the concrete
        // function cannot be found statically: FUN_142001730 just returns *(*mgr + 0x64580), and the
        // class of the sub-object at +8 is unknown, so there is no vtable to read in Ghidra.
        //
        // It is trivial to resolve HERE, at the exact moment the engine is about to use it. Logging
        // it as an RVA turns a static dead end into an address that can be disassembled and hooked
        // next session — the same move as the _ReturnAddress() trick that found the slot notifier.
        {
            const uintptr_t cmdMgrPtr = *(uintptr_t*)((char*)mgr + 8);
            if (cmdMgrPtr > 0x10000) {
                const uintptr_t cmd = *(uintptr_t*)(cmdMgrPtr + 0x64580);
                if (cmd > 0x10000) {
                    const uintptr_t cmdObj = cmd + 8;
                    // Published for the census: the engine's slot array is *(cmdObj + 0xD3830), and
                    // reading it off EMPIRE_MP instead is what produced a bogus "DIFFERENT" verdict
                    // on every gift click on 2026-08-05. This is the only place cmdObj is resolved.
                    g_lastCmdObj = cmdObj;
                    const uintptr_t cmdVt  = *(uintptr_t*)cmdObj;
                    if (cmdVt > 0x10000) {
                        const uintptr_t fn = *(uintptr_t*)(cmdVt + 0x240);
                        logf("  >>> GIFT COMMAND resolved: cmdObj=%016llX vtable=%016llX(RVA_%08llX) "
                             "slot0x240=%016llX(RVA_%08llX)",
                             (unsigned long long)cmdObj,
                             (unsigned long long)cmdVt,
                             (unsigned long long)(cmdVt >= g_base ? cmdVt - g_base : cmdVt),
                             (unsigned long long)fn,
                             (unsigned long long)(fn >= g_base ? fn - g_base : fn));
                        logf("      ^ that RVA is the function to hook to redirect a gift: its 2nd "
                             "argument IS the target player id.");
                    }
                }
            }
        }

        // Which direction is this click? FUN_142014A10(mgr+8)+0x11 selects gift vs return, and the
        // two branches swap the roles of the list entry and the computed player id — so any override
        // must know which one it is looking at.
        uint8_t mode = 0xFF;
        void*   m    = nullptr;
        {
            auto acf0 = (void*(*)(void*))(g_base + RVA_SHARE_MODE);
            m = acf0((char*)mgr + 8);
            if (m && readAt((uintptr_t)m + 0x11, mode))
                logf("  >>> direction flag (FUN_142014A10+0x11) = %u  -> %s", mode,
                     mode == 0 ? "GIFT (target comes from list[0])"
                               : "RETURN (target is the computed owner; list[0] is the source)");
        }

        // ---- ★ WHOSE unit list does the handler search, and who receives? ------------------------
        //
        // FUN_142EA8580 computes exactly TWO player ids and the direction flag decides which is
        // which. Decompiled in full (run 25) — both branches are the same four calls in a different
        // order:
        //
        //   computed = vt[0x230](svc, FUN_141FF52F0(mode), FUN_141FF5A30(mode));
        //   list0    = vt[0x248](svc, &list), list.count ? list.data[0] : -1;
        //
        //   +0x11 == 0  (GIFT)  :  search computed's list, send to list0
        //   +0x11 == 1  (RETURN):  search list0's list,    send to computed
        //
        // It does not gift the selection; it gifts whatever it can FIND in the searched record:
        //   rec = vt[0x228](svc, searchId);       // base rec+8, count rec+4, stride 0x78
        //   for each selected unit u (id at u+0x4A4):
        //       if (FUN_1404A9EB0(e) == id(u)) -> issue one command for e
        //
        // So a selection of 5 can produce five commands, or none, and the difference is invisible
        // from outside — an empty `rec`, or a searchId naming a player that is not us, and the click
        // does nothing whatsoever. That is precisely what run 24 hit: the armed holder was searching
        // the ARMY OWNER's list for units that were sitting in its own.
        //
        // ★ Neither FUN_141FF52F0 nor FUN_141FF5A30 reads +0x11 — both read only mode+8 (array base)
        // and mode+0x14 (index), so `computed` is the SAME id in both directions. Flipping the byte
        // therefore swaps the two roles and changes nothing else, which is what makes the F11
        // experiment below a one-byte change with a predictable result rather than a hope.
        if (m) {
            auto argA = (uint32_t(*)(void*))(g_base + RVA_SHARE_ARG_A);
            auto argB = (uint32_t(*)(void*))(g_base + RVA_SHARE_ARG_B);
            auto own  = (uint32_t(*)(void*, uint32_t, uint32_t))(*(uintptr_t*)(vt + VT_OWNER_ID));
            const uint32_t computed = own(obj, argA(m), argB(m));

            const int searchId = (mode == 0) ? (int)computed : (int)list0;
            const int targetId = (mode == 0) ? (int)list0    : (int)computed;

            logf("  >>> ROLES in this direction: SEARCH player %d's unit list, SEND to player %d "
                 "(computed=%d from vt[0x230], list[0]=%d)",
                 searchId, targetId, (int)computed, (int)list0);

            // ---- ★★ WHO IS IN WHICH GROUP — the two numbers every 2-army case turns on ----------
            //
            // `vt[0x230]` is FUN_14046FBC0, and it decodes the session slot flags as
            //
            //     alliance = flags & 3        army = (flags >> 2) & 0xF
            //
            // then returns the **first** occupied slot matching the (alliance, army) pair the
            // share-mode object carries in argA/argB. So "the owner" has never meant "whoever lent
            // this unit" — it means "the first player in this group". With one army and a spectator
            // the two are always the same player and the distinction is invisible; with two armies,
            // allied or opposed, it is the whole question.
            //
            // Read-only, and it decides nothing. It exists so that one 2-army battle produces
            // evidence instead of impressions — this project's record on reasoning about behaviour
            // it has not measured is three wrong theories out of three.
            logf("  >>> BATTLE GROUPS (alliance = flags&3, army = flags>>2&0xF; the engine searches "
                 "for the FIRST occupied slot matching alliance=%u army=%u)", argA(m), argB(m));
            // ⚠ THIS LOOP WALKS SLOTS, and used to print each one as "player N" with
            // `knownPlayerName(N)` beside it. That is how the 2026-08-08 log came to say
            // *"player 3 — player"* in a session whose player ids were 0/1/2, and it is a
            // large part of why #45 read as a naming fault rather than an addressing one. Each row
            // now says which slot it is and which player id owns it, and the name is looked up
            // through that id and the verified roster-to-lobby-name join.
            {
                const int meSlot = slotForPlayer(localPlayerId());
                for (int s = 0; s < (int)MAX_SESSION_PLAYERS; ++s) {
                    uint32_t f = 0;
                    if (!slotFlagsBySlot(s, f)) continue;
                    const bool occ = (f & 0x140) == 0x140 && !((f >> 10) & 1);
                    const bool grp = ((f >> 6) & 1) && !((f >> 10) & 1);  // FUN_14046FBC0's own test
                    if (!occ && !grp) continue;
                    bool inList = false;
                    for (int k = 0; k < shareN; ++k) if (shareIds[k] == s) inList = true;
                    const int   owner = playerForSlot(s);
                    const char* nm    = (owner >= 0) ? knownPlayerName(lobbyRecordIdForRecipient(owner)) : "";
                    char who[32];
                    if (owner >= 0) _snprintf_s(who, sizeof(who), _TRUNCATE, "player %d", owner);
                    else            _snprintf_s(who, sizeof(who), _TRUNCATE, "no player id");
                    logf("        slot %d (%s)  alliance=%u army=%u  %s%s%s%s   %s",
                         s, who, f & 3, (f >> 2) & 0xF,
                         occ ? "occupied" : "NOT-occupied",
                         inList ? " · in share list" : "",
                         (s == (int)computed) ? " · <== the engine's computed owner" : "",
                         (s == meSlot) ? " · <<< YOU" : "",
                         (nm && nm[0]) ? nm : "");
                }
            }

            auto      getRec = (void*(*)(void*, uint32_t))(*(uintptr_t*)(vt + VT_PLAYER_RECORD));
            void*     rec    = getRec(obj, (uint32_t)searchId);
            uint32_t  nUnits = 0;
            uintptr_t base   = 0;
            if (rec && readAt((uintptr_t)rec + 4, nUnits) && readAt((uintptr_t)rec + 8, base))
                logf("  >>> SEARCHING player %d's unit list for the selection: record=%016llX "
                     "entries=%u%s", searchId, (unsigned long long)rec, nUnits,
                     nUnits ? "" : "   <-- EMPTY: no command can be issued, the click is a no-op");
            else
                logf("  >>> SEARCHING player %d's unit list: record unreadable (%016llX)",
                     searchId, (unsigned long long)rec);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("  share: player-list read FAULTED — disabling further attempts (harmless)");
        g_logShareList = false;
    }
}
#endif


// ---- per-player gift targeting -----------------------------------------------------------------
//
// FUN_142EA8580 picks its target with `target = vec.count ? vec.data[0] : -1` and hands it to
// (*(cmd+8)->vtable[0x240])(cmd+8, target, unit, 0), which already accepts any player id. With two
// players list[0] is unambiguous; with three the engine simply takes the first.
//
// The concrete command function cannot be found in Ghidra (FUN_142001730 just returns
// *(*(mgr+8) + 0x64580) and the sub-object's class is unknown), so it cannot be detoured the way every
// other hook here is — those all rely on byte patterns verified statically.
//
// It does not need to be. We can redirect the call by swapping the vtable POINTER on that one object
// for the duration of the click: copy its vtable, replace slot 0x240 with a thunk, point the object at
// the copy, run the original, restore. No code is written, no byte pattern is needed, and the change
// lives on a heap object for a few microseconds — the same function-pointer-swap approach already
// noted as the clean, anti-tamper-safe technique.
//
// OFF BY DEFAULT: with no target armed nothing is swapped at all and behaviour is byte-for-byte
// vanilla, so this cannot disturb a build that works.
static constexpr size_t OFF_CMD_MGR  = 0x64580;  // *(mgr+8) + 0x64580 -> command manager
static constexpr size_t VT_GIFT_CMD  = 0x240;    // slot on the object at cmd+8
static constexpr size_t VT_COPY_SIZE = 0x400;    // generous: we only need up to 0x240

// ★ The command RETURNS a bool (FUN_140471AB0 -> ulonglong, AL). Run 9 could not tell a delivered
// gift from a gift that was never sent, because this was typed void and the thunk logged nothing.
typedef uint64_t (*GiftCmdFn)(void*, uint32_t, void*, int);

static GiftCmdFn     g_realGiftCmd   = nullptr;
volatile long g_giftTargetIdx = -1;   // ARMED PLAYER ID (not a list index); -1 = vanilla

// ★★★ THE RETURN DIRECTION SEARCHES THE WRONG PLAYER'S UNITS, and it is the same species of bug as
// the target: one hardcoded `list[0]` doing duty for a decision (2026-08-03, 09:50 run).
//
// FUN_142EA8580 computes both ids up front and the direction byte decides which is which:
//
//     GIFT   : search `computed`, send to list[0]
//     RETURN : search list[0],    send to `computed`
//
// So in the RETURN direction the SOURCE — whose unit list is scanned for the selection — is
// list[0], the first entry of the engine's share list. That list is the same on every machine, so
// list[0] names ONE player, and a hand-back therefore only works on the machine that happens to be
// that player. Measured exactly so:
//
//     host   (player 0, list[0]=0): searched its own list, found the units, 3 commands issued  ✓
//     client (player 1, list[0]=0): searched player 0's list, found nothing, five clicks in a row
//                                   logged NO GIFT COMMAND WAS ISSUED                          ✗
//
// ★ The correct source is not "list[0]" and not "the computed owner" — it is THIS MACHINE. The
// command's sender id is derived locally inside FUN_140471AB0 (`(*(*(this+0xD3880))->vtable[0x20])()`)
// and cannot be spoofed, and FUN_140471B30 then requires the unit to be in THAT player's list. So a
// RETURN naming any source but the local player can only ever produce commands the engine refuses —
// or, as here, no commands at all. Forcing the local player is strictly more correct, and it is a
// NO-OP on the machine where the hand-back already works, which is why it is unconditional rather
// than armed.
//
// ⇒ The search id feeds exactly one call, `vt[0x228](svc, id)`, and nothing else reads it (checked
// against the full decompilation). Overriding that one call is therefore exactly equivalent to
// overriding the search id, with no other reachable effect.
//
// ★ And it needs no new machinery: FUN_141FF6530 returns the ADDRESS of `*(mgr+8)+0x64580` while
// FUN_142001730 returns its VALUE, so the share service and the gift command are THE SAME OBJECT —
// vtable 0x143275A88 carries 0x228, 0x230, 0x240 and 0x248 alike. The copy this file already makes
// for slot 0x240 serves both.
typedef void* (*PlayerRecordFn)(void*, uint32_t);
static PlayerRecordFn g_realPlayerRecord = nullptr;
static long           g_returnSourceId   = -1;   // decided at arm time; -1 = leave the engine alone
static volatile long  g_recordForceId    = -1;   // armed only around the original call, consumed once

static void* playerRecordThunk(void* self, uint32_t id)
{
    // One-shot: the handler asks exactly once, and consuming the latch here means a later caller
    // (our own AFTER census runs before the vtable is restored) can never be redirected by accident.
    const long force = InterlockedExchange(&g_recordForceId, -1);

    uint32_t used = id;
    if (force >= 0 && (uint32_t)force != id) {
        used = (uint32_t)force;
        diagLogf("  >>> RETURN SOURCE CORRECTED: the handler was about to search player %u's unit list "
             "(list[0]), but the units being handed back are held by player %ld — THIS machine. "
             "Searching player %ld's list instead; the engine derives the sender id locally, so no "
             "other list can pass its own check.", id, force, force);
    }
    return g_realPlayerRecord ? g_realPlayerRecord(self, used) : nullptr;
}
static volatile long g_giftTargetId  = -1;   // the id actually in force during one click
static volatile long g_giftCmdCalls  = 0;    // commands issued during the current click
static uint8_t       g_vtCopy[VT_COPY_SIZE];

// Runs in place of the engine's command call. Substitutes the target player id and forwards.
//
// ★★★ THE BLIND SPOT THIS CLOSES (run 9, 2026-07-30). The 8 gift clicks in run 9 logged "OVERRIDE
// ACTIVE" and "gift units done" and nothing in between — but both of those lines are written by US,
// around the call, and neither says the engine ever issued a command. FUN_142EA8580 only issues one
// per unit it can MATCH in the owner's unit list; match nothing and the click is a silent no-op that
// looks identical in the log to a gift that went out and was dropped by the receiver.
//
// Two numbers separate every remaining hypothesis, and both are here:
//   * how many commands the click issued  — 0 means the handler never got as far as the network;
//   * what each one returned              — 0 means FUN_140471B30's sender-side check refused it
//                                           (sender slot out of range, slot bit 6 clear, or the unit
//                                           not found in the sender's own list), so nothing was sent.
// If the count is non-zero and every return is 1, the message DID go on the wire with our target and
// the problem is purely receive-side — which is the one place we have never looked.
static uint64_t giftCmdThunk(void* self, uint32_t target, void* unit, int flag)
{
    const long     override = g_giftTargetId;
    const uint32_t used     = (override >= 0) ? (uint32_t)override : target;

    uint64_t rc = 0;
    if (g_realGiftCmd) rc = g_realGiftCmd(self, used, unit, flag);

    const long n = InterlockedIncrement(&g_giftCmdCalls);
#ifndef TW3K_RELEASE
    if (n <= 8) {
        logf("  >>> GIFT COMMAND #%ld ISSUED: target=%d (engine asked for %d) unit=%016llX "
             "-> returned %u  %s",
             n, (int)used, (int)target, (unsigned long long)unit, (unsigned)(rc & 0xFF),
             (rc & 0xFF) ? "*** message 0xA9 SENT ***"
                         : "!!! REJECTED by the sender-side check — NOTHING went on the wire");

        // ★★ §6bbb, the ONE measurement the wrong-lender fix is blocked on.
        //
        // `unit` here is a player-list ENTRY, not a battlefield unit — the battle object is at
        // `entry + 0x50` (`UNIT_REC_OBJ`), which is what `FUN_140471B30` itself reads. From there
        // §6jjj's chain gives the army and the alliance.
        //
        // ⚠ THE WHOLE FIX RESTS ON WHAT THIS PRINTS. The proposed repair identifies the true lender
        // by matching `*(unit + 0x590)` against each player's army — which only works if a lent
        // unit KEEPS the lender's army pointer after the transfer. Nothing has ever measured that.
        // Lend across armies, hand back, and compare this line before and after: if the army
        // pointer follows the unit, the fix is writable; if it moves to the borrower, it is not,
        // and no amount of care in the code would have saved it.
        // ✗✗ WITHDRAWN 2026-08-05 — this printed GARBAGE on its first real run and must not be read.
        //
        // It read `*(entry + 0x50)` as a battlefield unit and then `+0x590` as that unit's army.
        // The measured values were `army=010C010A010A0102`, `army=007F007F00007F7F`,
        // `army=00000000001460A8` — none of them a pointer. The slot table's army column, built the
        // same way, was equally bad (`00F8A97E007AFE8D`).
        //
        // So one of the two links is wrong: either `entry + 0x50` is not the battle unit on this
        // path, or `+0x590` does not apply to whatever it is. §6jjj established `+0x590` on a
        // **battlefield unit object**, and the object handed to the gift command may be a
        // participant/roster record instead — a distinction that was assumed away rather than
        // checked.
        //
        // ⚠ It is printed as RAW BYTES with no interpretation now, and labelled unverified, because
        // the honest state is "we do not yet know what this field is". Printing it as `army=` next
        // to a §6bbb question invited exactly the reading it cannot support.
        // ⇒ To fix it properly: find the battle-unit object from the gift path in Ghidra —
        // `FUN_140471B30` uses `*(param_4 + 0x50)`, so start by reading what IT does with that
        // pointer, rather than assuming it is the same object §6jjj measured.
        __try {
            uintptr_t obj = 0; uint64_t at590 = 0;
            if (readAt((uintptr_t)unit + UNIT_REC_OBJ, obj) && obj > 0x10000) {
                readAt(obj + OFF_UNIT_ARMY, at590);
                logf("      list entry=%016llX  *(entry+0x50)=%016llX  *(that+0x590)=%016llX"
                     "   ⚠ UNVERIFIED: the +0x590 read came back as non-pointer garbage on "
                     "2026-08-05, so this is raw bytes, NOT an army pointer. Do not use it for "
                     "§6bbb until the object at +0x50 is identified.",
                     (unsigned long long)unit, (unsigned long long)obj,
                     (unsigned long long)at590);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
#endif
    return rc;
}

// Reads the share list into `out`. Returns the count, or -1 if it could not be read.
static int readShareList(void* mgr, int32_t* out, int maxOut)
{
    __try {
        auto  getSvc = (GetShareSvcFn)(g_base + RVA_GET_SHARE_SVC);
        void* svc    = getSvc((char*)mgr + 8);
        if (!svc) return -1;
        const uintptr_t inner = *(uintptr_t*)svc;
        if (inner <= 0x10000) return -1;
        void*           obj = (void*)(inner + 8);
        const uintptr_t vt  = *(uintptr_t*)obj;
        if (vt <= 0x10000) return -1;

        auto    fn  = (void(*)(void*, void*))(*(uintptr_t*)(vt + VT_PLAYER_LIST));
        CaVec32 vec = { 0, 0, nullptr };
        fn(obj, &vec);

        int n = 0;
        if (vec.data && vec.count < 64)
            for (uint32_t i = 0; i < vec.count && n < maxOut; ++i) out[n++] = vec.data[i];
        if (vec.data) ((GameFreeFn)(g_base + RVA_GAME_FREE))(vec.data);
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---- ★★★ THE UNIT CENSUS ------------------------------------------------------------------------
//
// A gift is a MOVE between two players' unit lists, and `vt[0x228](svc, playerId)` hands us any
// player's list with its count at +4. So counting before and after shows the transfer happen — or
// not — **from the giving machine alone**, with nobody having to report what they saw on screen.
//
// This is what run 9 needed. The report that came back was "the units did not move, but the UI
// changed formation, as if they changed owners", which is exactly what an ownership change to
// YOURSELF looks like (F10's first press arms id 0, and the gifter was player 0). If that reading is
// right the command is executing fine and the whole problem is which player can receive; if it is
// wrong the counts will not budge. Either way the counts settle it and an opinion cannot.
//
// ⚠ TIMING: the transfer is applied when the command comes back round, not inside the click, so the
// AFTER line may legitimately still read the old numbers. The BEFORE line of the *next* click is the
// one that shows a settled result — or press F6 a second later, which now dumps the same census.
#ifndef TW3K_RELEASE
static void logUnitCensus(void* mgr, const char* when)
{
    __try {
        auto  getSvc = (GetShareSvcFn)(g_base + RVA_GET_SHARE_SVC);
        void* svc    = getSvc((char*)mgr + 8);
        if (!svc) return;
        const uintptr_t inner = *(uintptr_t*)svc;
        if (inner <= 0x10000) return;
        void*           obj = (void*)(inner + 8);
        const uintptr_t vt  = *(uintptr_t*)obj;
        if (vt <= 0x10000) return;

        auto acf0 = (void*(*)(void*))(g_base + RVA_SHARE_MODE);
        void* m   = acf0((char*)mgr + 8);
        if (!m) return;

        auto argA   = (uint32_t(*)(void*))(g_base + RVA_SHARE_ARG_A);
        auto argB   = (uint32_t(*)(void*))(g_base + RVA_SHARE_ARG_B);
        auto own    = (uint32_t(*)(void*, uint32_t, uint32_t))(*(uintptr_t*)(vt + VT_OWNER_ID));
        auto getRec = (void*(*)(void*, uint32_t))(*(uintptr_t*)(vt + VT_PLAYER_RECORD));

        const uint32_t ownerId = own(obj, argA(m), argB(m));

        // Only ask about ids the engine itself uses: the computed owner, every share-list entry and
        // the armed target. Probing arbitrary ids would be inventing indices into an engine array.
        int32_t ids[20];
        int     n = 0;
        ids[n++] = (int32_t)ownerId;
        int32_t list[16] = { 0 };
        const int ln = readShareList(mgr, list, 16);
        for (int i = 0; i < ln && n < 20; ++i) ids[n++] = list[i];
        if (g_giftTargetIdx >= 0 && n < 20) ids[n++] = (int32_t)g_giftTargetIdx;

        // ★★★ §6bbb: the two assumptions the wrong-lender fix would otherwise be written on.
        //
        // (1) IS THE MOD LOOKING AT THE ENGINE'S ARRAY? Everything the mod knows about slots comes
        //     from `*(EMPIRE_MP + 0xD3848)`; the search the engine runs to decide a RETURN's
        //     recipient (`FUN_14046FBC0`, reached through the `vt[0x230]` thunk at 0x14046A950,
        //     which is literally `MOV RCX,[RCX+0xD3830]; JMP …`) walks `*(cmdObj + 0xD3830)`.
        //     Those two expressions differ by 0x18 and NOTHING has ever printed both. Every guard
        //     the fix would add assumes they are the same object. One line settles it.
        //
        // (2) DOES A SLOT'S (alliance, army) SMALL-INT PAIR RELATE TO ITS ARMY POINTER? The search
        //     matches `alliance == (flags & 3)` and `army == ((flags >> 2) & 0xF)` — small ints —
        //     while §6jjj gives object POINTERS. the backlog's stated fix, "feed FUN_14046FBC0 the
        //     unit's alliance/army", is **not writable as written**, because no mapping between
        //     those two number spaces exists anywhere in this project. Printing them side by side
        //     is what would establish one.
        {
            // ✗✗ THIS COMPARISON WAS WRONG ON ITS FIRST REAL RUN (2026-08-05) AND IS WITHDRAWN.
            //
            // It read BOTH arrays off `capturedMp()`. Only one of them lives there. The engine's
            // search is reached through the `vt[0x230]` thunk at 0x14046A950, which is
            // `MOV RCX,[RCX+0xD3830]; JMP FUN_14046FBC0` — and that `RCX` is the **command object**,
            // not EMPIRE_MP. So `*(EMPIRE_MP + 0xD3830)` is a read of an unrelated field, and it
            // duly came back as `0000000000847C11`: not a pointer, not an array, not anything.
            //
            // ⚠ The line then announced "⚠ DIFFERENT — every slot-based guard in this file is
            // describing a different array" on every single gift click, which is a false alarm
            // manufactured by a bad read. That is worse than printing nothing, and it is exactly
            // the failure this project keeps writing rules about: a number was printed without a
            // check that the address it came from meant anything.
            //
            // ⇒ Rewritten against the object the engine actually uses. `cmdObj` is resolved by the
            // arming path and logged as `GIFT COMMAND resolved: cmdObj=…`, so it is available; when
            // it is not, this says so rather than substituting a base that is to hand.
            const uintptr_t mp     = capturedMp();
            const uintptr_t cmdObj = g_lastCmdObj;
            uint64_t modSlots = 0, engSlots = 0;
            if (mp)     readAt(mp + OFF_MP_SLOTOBJ, modSlots);
            if (cmdObj) readAt(cmdObj + 0xD3830, engSlots);

            if (!cmdObj) {
                logf("      [%s] slot arrays: mod reads +0xD3848 = %016llX ; the engine's array is "
                     "*(cmdObj + 0xD3830) and cmdObj is not resolved on this path — NOT COMPARED "
                     "(it is not EMPIRE_MP, and reading it there is what produced a bogus "
                     "\"DIFFERENT\" on 2026-08-05).", when, (unsigned long long)modSlots);
            } else {
                logf("      [%s] slot arrays: mod reads *(EMPIRE_MP+0xD3848) = %016llX ; the engine "
                     "searches *(cmdObj+0xD3830) = %016llX %s", when,
                     (unsigned long long)modSlots, (unsigned long long)engSlots,
                     (modSlots && modSlots == engSlots)
                         ? "-> SAME OBJECT (the mod's slot reads describe what the engine searches)"
                         : "-> ⚠ DIFFERENT — every slot-based guard in this file is then describing "
                           "a different array from the one the engine searches");
            }

            if (modSlots) {
                uint32_t slots = 0;
                readAt((uintptr_t)modSlots + OFF_SLOT_COUNT, slots);
                if (slots > 16) slots = 16;
                logf("      [%s] %-4s %-9s %-6s  %s", when,
                     "slot", "flags", "a/army", "raw reads (UNVERIFIED - see the note in the code)");
                for (uint32_t s = 0; s < slots; ++s) {
                    const uintptr_t rec = (uintptr_t)modSlots + (uintptr_t)s * SLOT_ENTRY_STRIDE;
                    uint32_t flags = 0, units = 0;
                    uintptr_t list = 0, obj = 0, army = 0, alliance = 0;
                    if (!readAt(rec + SLOT_ENTRY_FLAGS, flags)) continue;
                    if (!((flags >> 6) & 1)) continue;             // the engine's own occupied test
                    readAt(rec + SLOT_ENTRY_UNITS, units);
                    if (units && readAt(rec + SLOT_ENTRY_LIST, list) && list > 0x10000 &&
                        readAt(list + UNIT_REC_OBJ, obj) && obj > 0x10000) {
                        readAt(obj + OFF_UNIT_ARMY, army);
                        if (army) readAt(army + OFF_ARMY_ALLIANCE, alliance);
                    }
                    // The two small ints are exactly what FUN_14046FBC0 matches on, so they are
                    // printed in its own terms: alliance = flags & 3, army = (flags >> 2) & 0xF.
                    // ⚠ The last two columns are the SAME unverified read as the per-unit line
                    // below — `*(entry+0x50) + 0x590` — and they came back as non-pointer garbage
                    // on 2026-08-05. Printed as raw bytes and labelled, not as "army/alliance".
                    logf("      [%s] %-4u %08X  %u/%-4u  raw+0x590=%016llX raw+0xA0=%016llX  (%u unit(s))",
                         when, s, flags, flags & 3, (flags >> 2) & 0xF,
                         (unsigned long long)army, (unsigned long long)alliance, units);
                }
                logf("      [%s] ⇒ Reading it: if two slots share an (a/army) pair they are ONE "
                     "group, and the engine's search returns the FIRST of them — which is §6bbb's "
                     "whole bug. And the pointer columns beside the small ints are the mapping "
                     "nobody has: if they line up consistently, the fix becomes writable.", when);
            }
        }

        for (int i = 0; i < n; ++i) {
            bool dup = false;
            for (int j = 0; j < i; ++j) if (ids[j] == ids[i]) dup = true;
            if (dup || ids[i] < 0) continue;

            void*    rec = getRec(obj, (uint32_t)ids[i]);
            uint32_t cnt = 0;
            if (rec && readAt((uintptr_t)rec + 4, cnt))
                logf("      [%s] player %d holds %u unit(s)%s%s", when, ids[i], cnt,
                     ids[i] == (int32_t)ownerId       ? "   <-- OWNER (the gifter)" : "",
                     ids[i] == (int32_t)g_giftTargetIdx ? "   <-- ARMED TARGET"     : "");
            else
                logf("      [%s] player %d has NO record — it cannot hold units in this battle, so a "
                     "gift to it has nowhere to land", when, ids[i]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("      [%s] unit census FAULTED (harmless, skipped)", when);
    }
}
#endif


// F6, in a battle: re-run the census a moment after a gift, once the command has come back round.
// This is the reading that says whether the units actually moved.
#ifndef TW3K_RELEASE
void dumpUnitCensusOnDemand()
{
    // Always report this, even when there is no battle to census: "has the UI ever asked?" is
    // answerable in a lobby, and it is the question run 28 could not answer.
    reportCcoQueryCounts();
    reportCcoHijack();

    void* mgr = g_lastBattleMgr;
    if (!mgr || (uintptr_t)mgr <= 0x10000) {
        logf("unit census: no gift click seen yet this session — click gift once first.");
        return;
    }
    uintptr_t sel = 0;
    if (!readAt((uintptr_t)mgr + OFF_BM_SELECTION, sel) || sel <= 0x10000) {
        logf("unit census: the cached battle manager is stale (battle over) — nothing to read.");
        return;
    }
    logf("=== UNIT CENSUS (F6, on demand) ===");
    logUnitCensus(mgr, "NOW   ");
    logf("=== end unit census ===");
}
#endif


// Returns true if the vtable was swapped; the caller must restore it afterwards.
//
// ★ CHANGED AFTER RUN 9: this now swaps in the GIFT direction whether or not a target is armed.
// Unarmed, the thunk forwards the engine's own target unmodified to the engine's own function — the
// only difference from vanilla is a log line — and in exchange a plain unhotkeyed click finally
// reports whether the engine issued a command and whether it was accepted. Run 9 spent a whole
// 3-machine session unable to answer that. The swap itself is not new risk: it ran eight times in
// run 9 with no fault and no failed restore.
static bool armGiftOverride(void* mgr, uintptr_t& cmdObjOut, uintptr_t& origVtOut)
{
    g_giftTargetId = -1;
    long want = g_giftTargetIdx;                     // -1 = observe only, do not redirect

    __try {
        // TARGET OVERRIDE is gift-direction only. FUN_142014A10(mgr+8)+0x11 selects gift vs return,
        // and the two branches swap the roles of the list entry and the computed id, so forcing a
        // target in the return direction would send the units somewhere nobody asked for.
        //
        // ★ The SWAP itself now happens in both directions. It is what counts the commands and reads
        // their return values, and a return is exactly the click we most need those numbers for —
        // run 24 could only say "no command was issued" for a gift. In the return direction the
        // thunk forwards the engine's own target untouched, so the only difference from vanilla is
        // the log.
        auto  acf0 = (void*(*)(void*))(g_base + RVA_SHARE_MODE);
        void* m    = acf0((char*)mgr + 8);
        uint8_t mode = 0xFF;
        if (!m || !readAt((uintptr_t)m + OFF_MODE_DIRECTION, mode)) {
            logf("  gift target: mode struct unreadable — leaving vanilla behaviour");
            return false;
        }
        // ★★ THE RETURN DIRECTION NOW HONOURS AN ARMED TARGET, and the reason it did not is now
        // known to be the bug rather than the safeguard.
        //
        // `vt[0x230]` is a thunk into FUN_14046FBC0, which is a SEARCH, not a lookup:
        //
        //     for each occupied slot:  if (argA == (flags & 3) && argB == (flags >> 2 & 0xF))
        //                                  return slotIndex;
        //
        // — the first occupied player whose (alliance, army) matches the pair the SHARE-MODE object
        // carries. That is the local battle context, not the unit's history, so "the owner" really
        // means "the first player in this alliance and army". In vanilla that is always right,
        // because a spectator can only ever hold units from the group it is attached to.
        //
        // ✗ It is wrong for us. Run 2026-08-03: a spectator attached to army 1 was lent a unit by
        // army 2's leader (player 2); returning it computed player 0 — army 1's leader — and the
        // unit went to someone who never owned it. The log said so exactly:
        //     ROLES ... SEND to player 0 (computed=0 from vt[0x230], list[0]=1)
        //
        // In this direction the roles are swapped — the search is list[0] (whoever is holding) and
        // the TARGET is the computed id — so overriding the target replaces exactly the value that
        // is wrong, and leaves the search alone.
        //
        // ⚠ Unarmed behaviour is unchanged: with no target armed the engine's own id still stands.
        // This makes a wrong return FIXABLE by hand (F10 the true lender, then click); it does not
        // yet make it automatic. That needs the unit's own owner, which is the next question.
        // ✗✗ BUT NOT WHEN THE TARGET CAME FROM THE PANEL — measured in a real battle, 2026-08-05.
        //
        // tester: *"player A gifts to player B 1 unit, this works, however when player B wants to
        // return the unit they can only select 'player 3' … the unit is gifted to player 3 who can
        // now only return it to player B."* Units bounced between the two spectators and could
        // never get home.
        //
        // The capture says the engine was RIGHT and we overrode it:
        //     ROLES: SEARCH player 1's unit list, SEND to player 0 (computed=0 from vt[0x230])
        //     player 0  alliance=0 army=0   occupied   <== the engine's computed owner  (the lender)
        //     player 1  alliance=3 army=15  occupied · in share list   <<< YOU
        //     player 2  alliance=3 army=15  occupied · in share list
        //     GIFT COMMAND #1 ISSUED: target=2 (engine asked for 0)
        //
        // ⇒ **In a RETURN there is exactly ONE correct recipient and the engine already computed
        // it.** `3/15` is the spectator encoding (§6iii — a spectator has no alliance and no army),
        // so players 1 and 2 are both spectators and player 0 is the only army owner. The panel,
        // however, offers the SHARE LIST — which is the set of spectators — because that is the
        // right set for a GIFT. Offering it for a RETURN means every button on it is wrong.
        //
        // ★ The distinction that fixes it already existed: `g_giftFromPanel`. An `F10` arming is a
        // deliberate act by someone who has read the log and knows the engine's answer is wrong;
        // a panel click is just "give this back". So:
        //
        //     RETURN + armed by F10     -> override  (the §6bbb cross-army workaround, kept)
        //     RETURN + chosen by panel  -> DO NOT     (the engine's computed owner is correct)
        //
        // ⚠ This does NOT fix §6bbb, and must not be read as doing so. With two armies the engine's
        // computed owner can still be the wrong player, and `F10` is still the workaround. What it
        // fixes is us breaking the case the engine gets right, which is the common one.
        if (mode != 0 && want >= 0 && g_giftFromPanel) {
            diagLogf("  gift target: this click is a RETURN (flag=%u) and the target came from the "
                 TW3K_MODE_TEXT("PANEL, not from F10 — IGNORING it. A return has exactly one correct recipient ", "PANEL — IGNORING it. A return has exactly one correct recipient ")
                 "and the engine has already computed it; the panel offers the share list, which "
                 "is the set of SPECTATORS and is the right set for a gift and the wrong one for a "
                 TW3K_MODE_TEXT("return. (F10 still overrides, for the cross-army case in §6bbb.)", "return. (The cross-army case is described in §6bbb.)"), mode);
            want = -1;
        }
        else if (mode != 0 && want >= 0) {
            diagLogf("  gift target: this click is a RETURN (flag=%u) and target %ld IS armed by hand — "
                 "overriding the engine's computed owner, which is the first player in this "
                 "alliance+army and NOT necessarily who lent the unit (FUN_14046FBC0).", mode, want);
        }

        // ★ ARM BY PLAYER ID, NOT BY LIST INDEX (revised after the first live gift attempt).
        //
        // The user gifted in a 3-player battle and "lost control of the unit but neither player got
        // it". That is the `vec.count == 0` path: with an empty list vanilla sets target = -1 and
        // issues the command anyway, so the units leave the giver and go to nobody.
        //
        // The list is probably SPECTATORS rather than arbitrary players — 3K's tooltip reads "give
        // control of the selected units to the spectator" and the engine has a plural spectator API
        // (SpectatorSlotList / CanAddSpectatorSlot / " (spectators %d/%d)").
        //
        // ⚠ But do NOT assume it is empty. In 3K everyone except the player who started the battle
        // normally joins AS A SPECTATOR, so in an ordinary coop battle the list should be POPULATED.
        // An earlier note claimed otherwise on the strength of `advertised spectators(+0xD23DA)=0` and
        // the session slot flags — those describe the CAMPAIGN SESSION's advertised spectator slots,
        // which is a different thing from an in-battle spectator role.
        //
        // Either way, arming a player id rather than a list index is the robust choice: it works
        // whether the list is empty (where vanilla would send the units to -1) or populated (where
        // vanilla just takes the first entry). Log what vanilla would have used, so one armed click
        // settles what the list actually holds.
#ifndef TW3K_RELEASE
        int32_t list[16] = { 0 };
        const int n = readShareList(mgr, list, 16);
        const long vanillaTarget = (n > 0) ? (long)list[0] : -1;
#endif
        if (want > 0xFF) return false;               // nonsense id — leave the engine alone entirely

        const uintptr_t inner = *(uintptr_t*)((char*)mgr + 8);
        if (inner <= 0x10000) return false;
        const uintptr_t cmd = *(uintptr_t*)(inner + OFF_CMD_MGR);
        if (cmd <= 0x10000) return false;
        const uintptr_t cmdObj = cmd + 8;
        const uintptr_t vt     = *(uintptr_t*)cmdObj;
        if (vt <= 0x10000) return false;

        g_realGiftCmd = (GiftCmdFn)(*(uintptr_t*)(vt + VT_GIFT_CMD));
        if (!g_realGiftCmd) return false;

        // The RETURN source correction, decided here and armed by the caller immediately before the
        // original runs. Only in the RETURN direction, and only when we know who we are.
        g_returnSourceId = -1;
        if (mode != 0) {
            const int me = localPlayerId();
            if (me >= 0) g_returnSourceId = me;
            else diagLogf("  >>> RETURN source: local player id unknown, so the engine's own list[0] "
                      "stands. If this machine is not list[0] the hand-back will find no units.");
        }

        g_realPlayerRecord = (PlayerRecordFn)(*(uintptr_t*)(vt + VT_PLAYER_RECORD));
        if (!g_realPlayerRecord) return false;

        memcpy(g_vtCopy, (void*)vt, VT_COPY_SIZE);
        *(uintptr_t*)(g_vtCopy + VT_GIFT_CMD)     = (uintptr_t)&giftCmdThunk;
        *(uintptr_t*)(g_vtCopy + VT_PLAYER_RECORD) = (uintptr_t)&playerRecordThunk;
        *(uintptr_t*)cmdObj = (uintptr_t)g_vtCopy;

        g_giftTargetId = want;
#ifndef TW3K_RELEASE
        if (want >= 0) {
            // ⚠ "player id" was a LIE in this line until 2026-08-10, and it mattered: `want` is
            // `g_giftTargetIdx`, which `coopGiftToPlayer` sets to `slotForPlayer(playerId)` — a SLOT
            // INDEX. On a contiguous session the two coincide and nobody noticed; on the first
            // non-contiguous one the log read "forcing player id 3" for a button labelled "player 2",
            // which looks like a bug in the send and is actually two correct values in two spaces.
            logf("  >>> GIFT TARGET OVERRIDE ACTIVE: forcing SLOT %ld  (vanilla would have used "
                 "slot %ld, from a share list of %d entr%s); real command RVA_%08llX",
                 want, vanillaTarget, n < 0 ? 0 : n, (n == 1) ? "y" : "ies",
                 (unsigned long long)((uintptr_t)g_realGiftCmd - g_base));
            if (vanillaTarget < 0)
                logf("      ^ vanilla would have sent these units to -1 (nobody) — this is the case "
                     "that loses units with no spectator present.");
        } else if (mode != 0) {
            logf("  >>> return probe armed (no override): this is the RETURN direction, so the "
                 "recipient is the id the engine computes, NOT list[0]=%ld; real command RVA_%08llX",
                 vanillaTarget, (unsigned long long)((uintptr_t)g_realGiftCmd - g_base));
            logf("      ⚠ that computed id is the FIRST occupied player in this alliance+army, not "
                 TW3K_MODE_TEXT("whoever lent the unit. If the lender is in another army, F10 their id before ", "whoever lent the unit. ")
                 TW3K_MODE_TEXT("clicking and the override sends it back correctly.", ""));
        } else {
            logf("  >>> gift probe armed (no override): the engine's own target %ld stands; "
                 "real command RVA_%08llX", vanillaTarget,
                 (unsigned long long)((uintptr_t)g_realGiftCmd - g_base));
        }

        // FUN_140471AB0 bails out before sending if *(cmdObj+0xD3880) is null, and derives the SENDER
        // id from that object's vtable[0x20]. FUN_140471B30 then demands the unit be in *that*
        // player's slot list — so if the sender id disagrees with the owner id the UI used to find
        // the units (logged just above), every command returns 0 and no gift can ever leave.
        {
            const uintptr_t lp = *(uintptr_t*)(cmdObj + 0xD3880);
            if (lp <= 0x10000) {
                logf("      !! command-layer local-player object is NULL — FUN_140471AB0 returns "
                     "before sending. No gift can go out at all.");
            } else {
                const uintptr_t lpVt = *(uintptr_t*)lp;
                if (lpVt > 0x10000) {
                    auto getId = (uint32_t(*)(void*))(*(uintptr_t*)(lpVt + 0x20));
                    logf("      command-layer SENDER id (vtable[0x20]) = %d", (int)getId((void*)lp));
                }
            }
        }
#endif
        cmdObjOut = cmdObj; origVtOut = vt;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("  gift target: FAULTED while arming — falling back to vanilla for this click");
        g_giftTargetId = -1;
        return false;
    }
}

// Defined below, beside the direction byte it reads. Forward-declared because the click handler has
// to know which way this click goes BEFORE it decides whether to open the panel at all.
static int giftDirection(void* mgr);

static void shareUnitsHook(void* mgr)
{
    // Mirror the original's OWN precondition before touching anything. FUN_142EA8580 returns
    // immediately when the selection is empty, and outside a battle the battle-manager global can
    // legitimately be null — walking it there faults where vanilla would simply have returned.
    // (This is exactly the bug that crashed the first 3-machine attempt: the hook dereferenced a
    // null manager during lobby setup.)
#ifndef TW3K_RELEASE
    static volatile long announced = 0;
    if (InterlockedCompareExchange(&announced, 1, 0) == 0)
        logf("share hook: FIRST CALL, mgr=%016llX", (unsigned long long)mgr);

#endif
    uintptr_t sel = 0; uint32_t selCount = 0;
    const bool sane = mgr && (uintptr_t)mgr > 0x10000 &&
                      readAt((uintptr_t)mgr + OFF_BM_SELECTION, sel) && sel > 0x10000 &&
                      readAt(sel + OFF_SEL_COUNT, selCount) && selCount > 0;

    uintptr_t cmdObj = 0, origVt = 0;
    bool swapped = false;

    // ✂ RETIRED 2026-08-04 (tester): FORCE RETURN (F11), and the two locals that carried it.
    //
    // It forced `+0x11 = 1` for the duration of one gift click, from when the hand-back only worked
    // on one machine per battle. That cause was found and fixed at its source (§6fff): the RETURN
    // handler searched **`list[0]`'s** unit list, and the source is now forced to the LOCAL player —
    // the only source the engine's own sender-side check accepts. `+0x11` is also the byte the gift
    // click already reads for its direction, and auto-arming sets it correctly, so forcing it had
    // nothing left to add. Removed rather than left inert.

    // ★ PANEL MODE: intercept the click and let the UI ask who, instead of gifting immediately.
    //
    // Deliberately BEFORE the original is called and before anything is swapped, and it returns
    // without touching the selection — FUN_142EA8580 clears the selection itself (FUN_1430C35A0), and
    // the units still have to be selected when the player picks a recipient a moment later.
    // ★★★ A RETURN DOES NOT GET A PANEL (tester's call, 2026-08-08).
    //
    // Reported live: on a non-battle-host the panel showed the right players, and **both** buttons
    // sent the units back to the battle host anyway. Correct behaviour, dishonest UI — the units
    // being held are BORROWED, so the click is a return, and the target has been deliberately taken
    // from the engine rather than the panel since 2026-08-03: a return has exactly one right
    // recipient and the panel offers the share list, which is the set of SPECTATORS.
    //
    // The old shape asked a question it had already decided to ignore. Offering a choice that is
    // then discarded is worse than offering none, because the player believes they made it — and
    // here they believed it twice and concluded the feature was broken.
    //
    // ⇒ On a return, fall straight through to the ordinary path: the engine computes the owner and
    // the hand-back happens on the first click, as it did before panel mode existed.
    //
    // ✗ NOT done by hiding every button. That renders an empty panel, which is the fail-INVISIBLE
    // mode this whole feature exists to avoid — and it is indistinguishable from the pack being
    // absent or the resolver being dead.
    // ✓ F10 is UNAFFECTED and remains the way to redirect a return: it is read further down, past
    //   this branch, and is still the §6bbb cross-army workaround.
    if (sane && g_giftPanelMode && !g_giftFromPanel && giftDirection(mgr) == 1) {
        if (g_giftPanelWanted) {           // a panel left open by a previous GIFT click
            g_giftPanelWanted = false;
            diagLogf("=== GIFT UNITS clicked === closing the open gift panel: this click is a RETURN.");
        }
        diagLogf("  panel skipped: this is the RETURN direction, so there is nothing to choose — the "
             "engine computes the one correct recipient and the panel's share list is the wrong "
             TW3K_MODE_TEXT("set for it. Handing back now. (F10 still redirects a return if the lender is in ", "set for it. Handing back now.")
             TW3K_MODE_TEXT("another army.)", ""));
        // fall through to the ordinary gift path below
    }
    else if (sane && g_giftPanelMode && !g_giftFromPanel) {
        g_lastBattleMgr = mgr;
        // ★ TOGGLE, not set. The gift icon is now the panel's open/close control, which is what
        // makes it behave like a menu hanging off that button rather than something that appears
        // once and has no way back. Choosing a recipient closes it too (coopGiftToPlayer).
        //
        // ★★★ #54 — BUILD THE LIST BEFORE OPENING, AND THE THREE CLICKS ARE OURS.
        //
        // refreshRecipientList() returns immediately while the panel is up, deliberately: a list
        // rebuilt under the mouse could retarget a button between the label being drawn and the
        // click landing, which is #45's defect class. But the FIRST click raised the flag before
        // the list had ever been built, so opening the panel froze the one thing that would have
        // filled it. Every CanGiftToPlayer query then found g_recipient[] still all -1 and answered
        // false, so the panel rendered with every button dead — the fail-invisible mode this
        // feature exists to avoid. Measured end to end on the first battle of 2026-08-12
        // (tw3k_coop_PWD11_20260812-212057.log):
        //
        //     21:44:58.861  click 1 -> GiftPanelOpen TRUE          (list never built)
        //     21:44:58.927  CanGiftToPlayer0..3 calls #1-3 -> false
        //     21:44:59.997  click 2 -> false (closed again)        (freeze lifts)
        //     21:45:00.051  RECIPIENTS: 2 live                     54 ms after the close
        //     21:45:00.052  CanGiftToPlayer0/1 false -> TRUE on call #12
        //     21:45:00.498  click 3 -> TRUE, and now it is populated
        //
        // ✗ NOT a readiness race, and not the battle root warming up: those were the standing
        // reading and this timeline refutes both — nothing was waited for, the refresh simply could
        // not run. It also explains why a SECOND battle takes one click: the list is already built
        // from a poll taken while the panel was down, and survives.
        // ✗ NOT fixed by letting the refresh run while the panel is open. That reintroduces the
        // rebuild-under-the-mouse the freeze exists to prevent, in the exact window the labels are
        // being drawn. Building it once, while the panel is still down, satisfies both.
        if (!g_giftPanelWanted) refreshRecipientList();
        g_giftPanelWanted = !g_giftPanelWanted;
        diagLogf("=== GIFT UNITS clicked === panel mode: GiftPanelOpen -> %s for %u selected unit(s), "
             "and NOT gifting. The pack's panel decides who.",
             g_giftPanelWanted ? "TRUE" : "false (closed again)", selCount);
        return;
    }

    if (sane) {
        diagLogf("=== GIFT UNITS clicked === battleMgr=%016llX  selectedUnits=%u%s",
             (unsigned long long)mgr, selCount, g_giftFromPanel ? "  (from the gift panel)" : "");
        g_lastBattleMgr = mgr;                       // so F6 can re-census after the dust settles

#ifndef TW3K_RELEASE
        if (g_logShareList) logSharePlayerList(mgr);
        logUnitCensus(mgr, "BEFORE");
#endif
        g_giftCmdCalls = 0;
        swapped = armGiftOverride(mgr, cmdObj, origVt);
    }

    // Arm the RETURN source correction for the duration of the original call ONLY. The AFTER census
    // below runs while the vtable is still swapped, and it asks vt[0x228] for every candidate — so
    // the window is closed the instant the handler returns, on top of the thunk's own one-shot.
    if (swapped) g_recordForceId = g_returnSourceId;
    if (g_origShareUnits) g_origShareUnits(mgr);
    g_recordForceId = -1;

    // ★ The one line run 9 needed and did not have.
    if (sane && swapped) {
        const long issued = g_giftCmdCalls;
        if (issued == 0)
            logf("  !!! NO GIFT COMMAND WAS ISSUED — the handler matched none of the %u selected "
                 "unit(s) in the searched unit list, so nothing was sent and nothing can arrive. "
                 "The failure is in FUN_142EA8580's lookup, NOT in targeting or the receiver. "
                 "(The RETURN source was %s, so this is no longer the run-09:50 case where the "
                 "wrong player's list was searched — check the units really are held HERE.)",
                 selCount,
                 g_returnSourceId >= 0 ? "corrected to this machine" : "left as the engine's own");
        else
            diagLogf("  === %ld gift command(s) issued for %u selected unit(s) ===", issued, selCount);
#ifndef TW3K_RELEASE
        logUnitCensus(mgr, "AFTER ");
#endif
        diagLogf("  (the transfer lands when the command comes back round, so AFTER may still read the "
             TW3K_MODE_TEXT("old numbers — press F6 in a second, or read the BEFORE line of the next click.)", "old numbers — read the local log.)"));
    }

    // Restore before anything else can touch the object. Unconditional and outside the arming
    // __try: if the original threw, the swapped vtable must still not be left in place.
    if (swapped) {
        __try {
            *(uintptr_t*)cmdObj = origVt;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            logf("  !! gift target: FAILED TO RESTORE the vtable at %016llX — expect instability",
                 (unsigned long long)cmdObj);
        }
        g_giftTargetId = -1;
    }

    // (The direction byte used to be restored here, because FORCE RETURN had forced it for the
    // duration of the click. Nothing writes it now, so there is nothing to put back — see the note
    // where those locals were declared.)

    if (sane) diagLogf("=== gift units done ===");
}

static const uint8_t EXPECT_SHARE_UNITS[SHARE_STOLEN_LEN] = {
    0x4C,0x8B,0xDC,                        // MOV R11, RSP
    0x49,0x89,0x4B,0x08,                   // MOV [R11+8], RCX
    0x41,0x57,                             // PUSH R15
    0x48,0x81,0xEC,0x90,0x00,0x00,0x00     // SUB RSP, 0x90
};

// =================================================================================================
//  The gift-target UI, exe half:  new named entries on CcoBattleRoot
//
//  Contract, widget ids and the pack half: runtime/GIFT_UI.md
//
//  FUN_1402B22D0 is the CcoBattleRoot property table IN CODE. Every entry is one call:
//
//      FUN_143086FE0(&slot, "IsSpectator", FUN_1430A50B0, 0);          // flags 0 = query
//      FUN_143086FE0(&slot, "ToggleCinematicMode", FUN_1430B66D0, 2);  // flags 2 = action
//      FUN_143086FE0(&DAT_1443E0970, 0, 0, 0);                         // null-name terminator
//
//  and the registrar writes the slot, then — on its first call only — registers the TABLE BASE
//  (&DAT_1443E0370) under the string "CcoBattleRoot". Only the base is registered, so a new entry
//  has to live inside the array the base points at: calling FUN_143086FE0 with a slot of our own
//  would write a struct nothing ever reads.
//
//  ❌ PLAN A — extend in place — IS RULED OUT (run 26, on three machines). Writing over the
//  terminator needs (10+1)*0x18 = 264 bytes of padding after it. Only 32 are free: byte 32 reads
//  0x50 at runtime, so something else owns that memory. The guard refused rather than corrupting it,
//  which cost one injection and no battle. Zero in the static image proved nothing, exactly as the
//  comment that used to sit here suspected.
//
//  ❌ PLAN B — copy the array into our own memory and re-point the registry — IS DELETED (2026-08-03).
//  It installed perfectly on three machines and changed a pointer nobody reads: the property lookup
//  takes its base from `LEA RCX,[0x1443E0370]`, an immediate compiled into the code, and never
//  consults the registry at all (§6vv). It then sat here inert for a day, printing a line that read
//  like working machinery — which is the specific cost of keeping a dead approach around. The record
//  of what it did and why it failed is §6ss/§6vv in NETCODE_NOTES.md; the code is gone.
//
//  ⚠ The ten `Coop*` entries went with it, including the four `CoopGiftToPlayerN` actions. They were
//  never wrong — they were the answer to a question that turned out not to be asked. HANDOVER long
//  recorded the blocker as "how does a twui expression pass an argument", so the design gave every
//  recipient its own nullary name. §6ww then measured an argument arriving intact (3, 7 and 9 read
//  back as 3, 7 and 9), and ONE action has served every recipient since.
//
//  ⇒ What survives below is what plan D and plan E both need: the table's own geometry, the entry
//  layout, and the two helpers that hand a query its value.
// =================================================================================================

static constexpr uintptr_t RVA_CCO_TABLE_BASE = 0x043E0370;  // DAT_1443E0370, stride 0x18
static constexpr uintptr_t RVA_CCO_TERMINATOR = 0x043E0970;  // the null-name entry that ends it
static constexpr size_t    CCO_ENTRY_SIZE     = 0x18;
static constexpr size_t    CCO_QUERY_SET_SLOT = 0x50;        // out->vtable[0x50](out, value)

// Derived, not counted by hand: FUN_1402B22D0 emits one call per entry from the base up to the
// terminator, so the span IS the count. 0x600 / 0x18 = 64, and entry[14] is "IsSpectator" — which
// is exactly the slot the guard below probes, so the two facts check each other.
static constexpr size_t    CCO_ORIG_COUNT =
    (RVA_CCO_TERMINATOR - RVA_CCO_TABLE_BASE) / CCO_ENTRY_SIZE;
static_assert(CCO_ORIG_COUNT == 64, "the CcoBattleRoot table is not the 64 entries we analysed");

// ⚠ The registry that holds the table base — FUN_14057E3B0 -> &DAT_143BFDC88, keyed by the string
// "CcoBattleRoot" and read with FUN_142D1E2A0 — is deliberately NOT wired up here any more. It is a
// real registry and it holds a real base; it simply is not where the UI looks (§6vv). Anyone tempted
// to reach for it again should read that section first.

// Exactly the three fields FUN_143086FE0 writes.
struct CcoEntry {
    void*       fn;      // +0x00
    uint32_t    flags;   // +0x08   0 = query, 2 = action
    uint32_t    pad;     // +0x0C
    const char* name;    // +0x10
};
static_assert(sizeof(CcoEntry) == CCO_ENTRY_SIZE, "CcoEntry must match the engine's slot layout");

// A query does not return its value — it hands it to the result object it is given.
// FUN_1430A50B0 ("IsSpectator") is the model: (*(out->vtable + 0x50))(out, byteValue).
static void ccoReturnBool(void* out, bool value)
{
    __try {
        if (!out) return;
        const uintptr_t vt = *(uintptr_t*)out;
        if (vt <= 0x10000) return;
        auto set = (void(*)(void*, uint8_t))(*(uintptr_t*)(vt + CCO_QUERY_SET_SLOT));
        if (set) set(out, value ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A query that faults must not take the UI down with it.
    }
}

// The battle manager the click handler wants is the same global its own thunk loads.
static void* ccoBattleRoot()
{
    uintptr_t root = 0;
    if (!readAt(g_base + RVA_BATTLE_ROOT_PTR, root) || root <= 0x10000) return nullptr;
    return (void*)root;
}

// The whole of a gift-to-player action: arm the target, re-enter the handler, put things back.
//
// It calls the HOOKED address on purpose, so the click goes through shareUnitsHook and produces the
// same census, share list and per-command return values as a hotkey-armed gift. g_giftFromPanel stops
// that re-entry from bouncing straight back into panel mode.
//
// ★★★ THIS IS THE CROSSING (#45). The caller names a PERSON; the payload wants a SLOT. Everything
// above this line is in player-id space and everything below it — `g_giftTargetIdx`, the vtable
// thunk, the `0xA9` payload — is in slot space.
//
// ⚠ It REFUSES when the seat cannot be resolved, and that is the whole point of the fix. The old
// code passed the id straight through, so a player id of 1 armed slot 1: a real other player on a
// non-contiguous session, or, if that slot happened to be empty, the unit destroyed outright.
static void coopGiftToPlayer(int playerId)
{
    void* mgr = ccoBattleRoot();

    const int slot = slotForPlayer(playerId);
    const char* who = knownPlayerName(lobbyRecordIdForRecipient(playerId));
    diagLogf("=== gifting to player %d (%s), who sits in slot %d — as asked by the UI ===",
         playerId, (who && who[0]) ? who : "no captured name", slot);

    if (!mgr) {
        diagLogf("   battle root is null — not in a battle. Nothing done.");
        g_giftPanelWanted = false;
        return;
    }
    if (slot < 0) {
        logf("   ⇒ REFUSED: player %d has no seat in the +0x137C table, so there is no slot to "
             "address. The payload target is a SLOT index and an unowned one destroys the units, "
             "so nothing is sent. (#45)", playerId);
        g_giftPanelWanted = false;
        return;
    }

    const long saved = g_giftTargetIdx;   // an F10 arming is the player's, not ours to discard
    g_giftTargetIdx  = slot;
    g_giftFromPanel  = true;
    g_giftPanelWanted = false;            // the question has been answered; the panel may close

    __try {
        ((ShareUnitsFn)(g_base + RVA_SHARE_UNITS))(mgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("   !! the gift handler faulted — restoring state and carrying on");
    }

    g_giftFromPanel = false;
    g_giftTargetIdx = saved;
}

int localPlayerId();   // defined with the session dump, further down

// Which way this click goes. 0 = GIFT (owner lending out), non-zero = RETURN (holder handing back).
// -1 means the mode struct could not be read, and every caller treats that as "assume GIFT", which
// is the direction with the stricter check.
static int giftDirection(void* mgr)
{
    if (!mgr) return -1;
    __try {
        auto  acf0 = (void*(*)(void*))(g_base + RVA_SHARE_MODE);
        void* m    = acf0((char*)mgr + 8);
        uint8_t mode = 0xFF;
        if (!m || !readAt((uintptr_t)m + OFF_MODE_DIRECTION, mode)) return -1;
        return mode == 0 ? 0 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Legality, answered by the engine rather than by us: the share list IS the set of players a gift may
// be sent to (FUN_14046FC80 filters it to in-battle spectators), so a button for anyone outside it
// would be a button the sender-side check is right to refuse.
//
// ✗ …but ONLY IN THE GIFT DIRECTION, and taking that for granted broke the hand-back completely
// (2026-08-03). On the machine holding borrowed units the click is a RETURN, and the share list
// there holds the SPECTATORS — never the lender. So every recipient failed this test, the panel
// refused all four buttons, and returning a unit did nothing at all. The log said it four times in a
// row and it still reads like a legality result rather than a bug: "not in the share list, or it is
// you".
//
// ⇒ In the RETURN direction the share list is simply the wrong question. Anyone who is not us is a
// candidate, because the recipient we want is whoever lent the unit — and that player is, by the
// definition of this bug, in a different group from the list. The engine's own sender-side check
// still runs afterwards and still refuses anything impossible; we log its verdict either way.
// ✗✗ …and "anyone who is not us" was too generous, which COST A UNIT (2026-08-03, 09:50 run).
//
// The panel offered the host three recipients — 1, 2 and 3 — in a THREE-player battle. Slot 3 was
// empty (`slot[3] flags(+0xF4)=000000C1 occupied=no` in that machine's own session dump). Clicking
// it sent `0xA9` with target=3, the command `returned 1`, and the unit was never seen again: the
// sender validates nothing about the target, so a gift to a slot nobody occupies destroys the unit.
// The census closes it arithmetically — 7 + 2 + 0 units held out of the ten that started.
//
// ⇒ Occupancy is now required in BOTH directions, using the engine's own predicate (bit 6 and bit 8
// set, bit 10 clear — read out of FUN_1404AB370 and printed by every session dump). In the GIFT
// direction this is belt-and-braces, because share-list membership already implies it; in the RETURN
// direction it is the whole guard.
// ⚠ TRI-STATE, deliberately: 1 = occupied, 0 = empty, **-1 = the session could not be read**.
//
// Collapsing "cannot tell" into "empty" would hide all four buttons the moment EMPIRE_MP was not
// captured — a panel that renders as an empty box, which is precisely the fail-INVISIBLE mode this
// whole feature is built to avoid. Unknown therefore means "do not filter", and the caller falls
// back to the behaviour that at least shows something.
static int slotOccupancyBySlot(int slot)
{
    if (slot < 0 || (uint32_t)slot >= MAX_SESSION_PLAYERS) return 0;
    uint32_t flags = 0;
    if (!slotFlagsBySlot(slot, flags)) return -1;
    return ((flags & 0x140) == 0x140 && !((flags >> 10) & 1)) ? 1 : 0;
}

// ---- the alliance a unit belongs to, and the laundering guard it makes possible (§6jjj) ---------
//
// ★ tester, 2026-08-03, after handing a unit all the way across a versus battle: "it would be good if
// a spectator can not transfer a unit from one owner to the other — if they are not allied."
//
// That is the right rule and §6iii.2 is why it is needed: a spectator sits in BOTH hostile sides'
// share lists, so without this a unit lent by one army can be handed to its enemy and kept.
//
// ★★ The chain is two plain field reads, no calls, read out of `IsEnemyUnit` (FUN_142EA71B0 ->
// FUN_142E7BDA0), which does exactly this comparison for its own purposes:
//
//     army     = *(unit + 0x590)     FUN_142379DD0, a one-line accessor
//     alliance = *(army + 0x0A0)     FUN_14102E660, likewise
//
// and the engine compares the results by POINTER IDENTITY, not by index — which is why this needs no
// mapping between the battle's alliances and the session slot flags' `flags & 3`. Two units are on
// the same side iff these two pointers are equal. That is the whole guard.
//
// ⚠ It deliberately does NOT use the slot flags. `alliance = flags & 3` is a different number space
// and nothing has ever established that the two agree; comparing across them would be exactly the
// kind of plausible unverified mapping this project keeps being punished by.
// ⚠ `OFF_UNIT_ARMY` / `OFF_ARMY_ALLIANCE` moved to offsets.h on 2026-08-04 — B10's battle-side dump
// reads the same two fields, and a second copy of an offset is how the two drift apart.
static constexpr size_t OFF_PLIST_UNIT     = 0x50;   // player unit-list entry -> the unit object
static constexpr size_t PLIST_STRIDE       = 0x78;

static void* allianceOfUnit(void* unit)
{
    uintptr_t army = 0, alliance = 0;
    if (!unit) return nullptr;
    if (!readAt((uintptr_t)unit + OFF_UNIT_ARMY, army) || army <= 0x10000) return nullptr;
    if (!readAt(army + OFF_ARMY_ALLIANCE, alliance) || alliance <= 0x10000) return nullptr;
    return (void*)alliance;
}

// The alliance the current selection belongs to. Null means "do not know", and every caller treats
// that as "do not filter" — the same tri-state discipline as slotOccupancy, and for the same reason:
// a guard that hides every button when it cannot read something is worse than the hazard it guards.
// A MIXED selection also returns null, because there is no single right answer for it.
static void* allianceOfSelection(void* mgr)
{
    uintptr_t sel = 0, units = 0;
    uint32_t  count = 0;
    if (!mgr) return nullptr;
    if (!readAt((uintptr_t)mgr + OFF_BM_SELECTION, sel) || sel <= 0x10000) return nullptr;
    if (!readAt(sel + OFF_SEL_COUNT, count) || count == 0 || count > 64) return nullptr;
    if (!readAt(sel + OFF_SEL_UNITS, units) || units <= 0x10000) return nullptr;

    void* common = nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t u = 0;
        if (!readAt(units + (uintptr_t)i * sizeof(uintptr_t), u) || u <= 0x10000) continue;
        void* a = allianceOfUnit((void*)u);
        if (!a) continue;
        if (!common) common = a;
        else if (common != a) return nullptr;      // mixed — no single answer
    }
    return common;
}

// A player's own alliance, taken from a unit they still hold. Null means "do not know" — which is
// the honest answer for a player holding nothing, and there is no other place to ask: a spectator's
// session slot carries no alliance at all (§6iii measured 3/15, every bit set).
static void* allianceOfPlayer(void* mgr, int id)
{
    __try {
        auto  getSvc = (GetShareSvcFn)(g_base + RVA_GET_SHARE_SVC);
        void* svc    = getSvc((char*)mgr + 8);
        if (!svc) return nullptr;
        const uintptr_t inner = *(uintptr_t*)svc;
        if (inner <= 0x10000) return nullptr;
        void*           obj = (void*)(inner + 8);
        const uintptr_t vt  = *(uintptr_t*)obj;
        if (vt <= 0x10000) return nullptr;

        auto     getRec = (void*(*)(void*, uint32_t))(*(uintptr_t*)(vt + VT_PLAYER_RECORD));
        void*    rec    = getRec(obj, (uint32_t)id);
        uint32_t cnt    = 0;
        uintptr_t base  = 0;
        if (!rec || !readAt((uintptr_t)rec + 4, cnt) || !readAt((uintptr_t)rec + 8, base))
            return nullptr;
        if (!cnt || cnt > 256 || base <= 0x10000) return nullptr;

        for (uint32_t i = 0; i < cnt; ++i) {
            uintptr_t u = 0;
            if (!readAt(base + (uintptr_t)i * PLIST_STRIDE + OFF_PLIST_UNIT, u) || u <= 0x10000)
                continue;
            if (void* a = allianceOfUnit((void*)u)) return a;
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// True if handing the selection to the player in `slot` would move it across the alliance line.
// Unknown is NOT crossing: see the tri-state note above.
//
// ⚠ SLOT, not player id — inferred rather than measured. `allianceOfPlayer` asks the share service's
// `vt[VT_PLAYER_RECORD](obj, x)`, and the sibling accessor on that same object, `vt[VT_PLAYER_LIST]`,
// is the share list, whose entries are measured slots. So x is almost certainly a slot too. Passing
// a slot is in any case never worse than what this did before #45, because before #45 the caller
// handed it a number that was a slot whenever it was right about anything.
static bool wouldCrossAllianceLine(void* mgr, int slot, void** selOut, void** tgtOut)
{
    void* selAll = allianceOfSelection(mgr);
    void* tgtAll = allianceOfPlayer(mgr, slot);
    if (selOut) *selOut = selAll;
    if (tgtOut) *tgtOut = tgtAll;
    return selAll && tgtAll && selAll != tgtAll;
}

// ---- WHO IS ACTUALLY IN THIS BATTLE: the panel's recipient list (#45) ----------------------------
//
// ★★★ THE RULE (tester, 2026-08-08): *"the assumption a player slot is sequential for the UI should
// never be used, it should always look at which id's are actually in the game."*
//
// So a button is a POSITION IN THIS LIST. Not a player id, not a slot. Button 0 is whoever the
// enumeration puts first, and three things follow — which together are the fix:
//
//   * MEMBERSHIP comes from the engine's own occupancy predicate over the slot flags;
//   * every button carries a PLAYER ID, resolved once when the list is built, so a button knows
//     *who* it targets rather than *where* they sat;
//   * the COUNT comes from the same enumeration. The panel offered two options in a three-player
//     battle because the count was a fixed property of the pack — the same mistake wearing a
//     different hat.
//
// ⚠ FROZEN WHILE THE PANEL IS OPEN. These queries are polled ~10x a second. A list rebuilt under the
// mouse could retarget a button between the label being drawn and the click landing — the exact
// class of defect this whole change exists to remove — so it is rebuilt only while the panel is
// down, and held still while it is up.
//
// ⚠ The ceiling is the PACK's: four buttons ship, so a fifth live recipient cannot be offered. That
// is a pack change, not a DLL one, and it is logged rather than silently dropped.
static constexpr int MAX_RECIPIENTS = 4;

static int g_recipient[MAX_RECIPIENTS] = { -1, -1, -1, -1 };   // player ids, by button position
static int g_recipientCount            = 0;
static int g_recipientOverflow         = 0;   // live recipients the pack has no button for

void resetGiftPanelState(const char* reason)
{
    const bool hadState = g_giftPanelWanted || g_recipientCount != 0;
    g_giftPanelWanted = false;
    g_recipientCount = 0;
    g_recipientOverflow = 0;
    for (auto& recipient : g_recipient) recipient = -1;
    g_lastBattleMgr = nullptr;
    g_lastCmdObj = 0;
    if (hadState) diagLogf("GIFT PANEL: closed and recipient snapshot cleared (%s)", reason);
}


static void refreshRecipientList()
{
    if (g_giftPanelWanted) return;            // frozen while the panel is up

    // ⚠ WHICH SPACE IS `localPlayerId()` IN? It is `mp+0xD3898 -> vtable[0x20]()`, and every session
    // that has ever printed it had the local player in matching id and slot, so nothing distinguishes
    // them. Both tests are applied — by id, and by the seat that id resolves to — so we exclude
    // ourselves whichever it turns out to be, and the pair is logged so one non-contiguous session
    // settles it for good.
    // ★★★ CORRECTED 2026-08-11: `localPlayerId()` returns a SLOT INDEX, not a player id.
    //
    // The reading below this ("a compact session index") is REFUTED by the very log line it was
    // drawn from. `tw3k_coop_MSI_20260810-204309.log` line 186:
    //
    //     player->slot table (+0x137C):  p0->s0  p1->s1  p2->s3 *
    //
    // That session had exactly THREE players and they are 0, 1, 2 — the table is indexed by player
    // id and works perfectly. `localPlayerId()` returned **3**. There is no player 3, and 3 is out
    // of range for a compact index over three players too, so BOTH readings die. It is this
    // machine's SLOT: `p2->s3` says the local player is player 2 sitting in slot 3.
    //
    // ⚠ The `<<< YOU` marker is NOT evidence for this — `dumpMpSession` prints it when
    // `slotIndex == localPlayerId()`, so it already assumes the answer. The independent fact is
    // simply that 3 is not a player in that session.
    //
    // ★ And the codebase already disagreed with itself: `dumpMpSession` indexes the SLOT array with
    // this value while this function compared it against PLAYER ids. One of the two had to be wrong;
    // the log says it was this one.
    const int meSlot = localPlayerId();        // a SLOT index, despite the name
    const int me     = playerForSlot(meSlot);  // ...so cross to player space for the id compare

    // ★★★ THE QUESTION ABOVE IS ANSWERED, AND THE ANSWER IS "THEY ARE DIFFERENT SPACES".
    //
    // 2026-08-09 evening, the first genuinely non-contiguous session (players 0, 1 and 3 — no
    // player 2). On the machine that `localPlayerId()` called player 3:
    //
    //     ★ RECIPIENTS: 3 live (I am player 3 in slot -1) — … button2=player 2(slot 3,unnamed)
    //
    // `slotForPlayer(3)` returned **-1** while `slotForPlayer(2)` returned **3**. So the `+0x137C`
    // table is indexed by a COMPACT session index (0,1,2) and `localPlayerId()` is NOT in that
    // space. ⇒ both self-exclusion tests silently failed and the panel offered a button that gifts
    // to YOURSELF — the third occupant, labelled "player 2", was that machine.
    //
    // ⚠⚠ THE ADDRESSING IS DELIBERATELY NOT CHANGED HERE. The send path resolves the button through
    // the slot and reached the right person all evening; rewriting the id space on one session's
    // evidence is how a working feature gets broken. What changes is that the failure is now LOUD,
    // because it was completely silent and cost an unexplained self-gift button.
    // Now that `meSlot` IS the seat, the only way to lose self-exclusion is an unreadable session.
    // ⚠ Kept loud: this was silent for three sessions and cost an unexplained gift-to-yourself
    // button, and a guard that fails quietly is the thing this file exists to stop.
    if (meSlot < 0) {
        static int shouted = -2;
        if (shouted != meSlot) {
            shouted = meSlot;
            logf("🔴 RECIPIENTS: localPlayerId() returned %d — no seat, so this machine cannot "
                 "identify itself and SELF-EXCLUSION IS OFF. One of the buttons below may gift to "
                 "YOU. (Expected only when the MP session has not been captured yet.)", meSlot);
        }
    } else {
        static int announced = -2;
        if (announced != meSlot) {
            announced = meSlot;
            diagLogf("RECIPIENTS: this machine is slot %d = player %d. Self-exclusion is by SEAT, which "
                 "is the space localPlayerId() is actually in — see the note in refreshRecipientList.",
                 meSlot, me);
        }
    }

    int next[MAX_RECIPIENTS] = { -1, -1, -1, -1 };
    int n = 0, overflow = 0;

    for (uint32_t id = 0; id < MAX_SESSION_PLAYERS; ++id) {
        const int slot = slotForPlayer((int)id);
        if (slot < 0) continue;                       // no seat: not in this session
        // ★ SEAT is the authoritative test — one seat is one machine, whatever id reaches it.
        if (meSlot >= 0 && slot == meSlot) continue;
        // Belt and braces, and deliberately kept: when the two spaces coincide (contiguous seating,
        // which is every session before 2026-08-09) this is the same test, and when they do not it
        // costs nothing. `me` is -1 if the seat could not be crossed back, and -1 matches no id.
        if (me >= 0 && (int)id == me) continue;
        if (slotOccupancyBySlot(slot) != 1) continue; // vacant or spectator
        if (n < MAX_RECIPIENTS) next[n++] = (int)id;
        else                    ++overflow;
    }

    bool changed = (n != g_recipientCount) || (overflow != g_recipientOverflow);
    for (int i = 0; i < MAX_RECIPIENTS && !changed; ++i) changed = (next[i] != g_recipient[i]);
    if (!changed) return;

    for (int i = 0; i < MAX_RECIPIENTS; ++i) g_recipient[i] = next[i];
    g_recipientCount    = n;
    g_recipientOverflow = overflow;

#ifndef TW3K_RELEASE
    char line[320];
    int  w = _snprintf_s(line, sizeof(line), _TRUNCATE,
                         "★ RECIPIENTS: %d live (I am player %d in slot %d) —", n, me, meSlot);
    for (int i = 0; i < n; ++i) {
        const char* nm = knownPlayerName(lobbyRecordIdForRecipient(g_recipient[i]));
        w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "  button%d=player %d(slot/nameId %d,%s)",
                         i, g_recipient[i], slotForPlayer(g_recipient[i]),
                         (nm && nm[0]) ? nm : "unnamed");
    }
    logf("%s%s", line,
         overflow ? "   ⚠ AND MORE THAN THE PACK HAS BUTTONS FOR — see the overflow line below" : "");
#endif
    if (overflow)
        logf("   ⚠ %d further live recipient(s) cannot be offered: the pack ships %d buttons. Nobody "
             "is silently dropped from the count — this line IS the count being honest.",
             overflow, MAX_RECIPIENTS);
}

// The player id a button targets, or -1 if that button has nobody behind it.
static int recipientAt(int button)
{
    if (button < 0 || button >= MAX_RECIPIENTS) return -1;
    return g_recipient[button];
}

// Legality for one BUTTON. Everything inside works in whichever space each layer actually uses, and
// says which one in the name.
static bool coopCanGiftFromButton(int button)
{
    void* mgr = ccoBattleRoot();
    if (!mgr) return false;

    refreshRecipientList();

    const int id = recipientAt(button);
    if (id < 0) return false;                 // ★ the count, enforced: no person, no button
    const int slot = slotForPlayer(id);
    if (slot < 0) return false;

    // ⚠ Rate-limited by CHANGE, not by count: this is polled ~10x/second, but a button appearing or
    // disappearing is exactly the thing a run needs to be able to read afterwards.
    const int occupied = slotOccupancyBySlot(slot);
#ifndef TW3K_RELEASE
    static int lastOccupied[MAX_RECIPIENTS] = { 1, 1, 1, 1 };

    if (occupied != lastOccupied[button]) {
        lastOccupied[button] = occupied;
        logf("CCO QUERY: button %d (player %d, slot %d) is now %s — it %s. (An unoccupied slot "
             "accepts the command and returns 1, and the unit is then gone: the sender validates "
             "nothing about the target.)", button, id, slot,
             occupied == 1 ? "an OCCUPIED slot" :
             occupied == 0 ? "an EMPTY slot"    : "UNREADABLE (session not captured)",
             occupied == 0 ? "is hidden" : "may show");
    }
#endif
    if (occupied == 0) return false;

    if (giftDirection(mgr) == 1) {
        // RETURN: the share list does not describe who may receive — but the alliance line does.
        // Handing a borrowed unit to someone on the other side is how units change owner
        // permanently (§6iii.2), and it is the one thing a hand-back must not be able to do.
        void* selAll = nullptr; void* tgtAll = nullptr;
        const bool crosses = wouldCrossAllianceLine(mgr, slot, &selAll, &tgtAll);

        // Rate-limited by CHANGE: this is polled ~10x/second, and what a run needs to be able to
        // read afterwards is a button appearing or disappearing, not every poll that agreed.
#ifndef TW3K_RELEASE
        static int lastCrossed[MAX_RECIPIENTS] = { -1, -1, -1, -1 };
        const int now = crosses ? 1 : 0;
        if (lastCrossed[button] != now) {
            lastCrossed[button] = now;
            logf("CCO QUERY: hand-back to player %d (slot %d) is %s (selection alliance=%016llX, "
                 "their alliance=%016llX)%s", id, slot,
                 crosses ? "BLOCKED — that is the other side" : "allowed",
                 (unsigned long long)selAll, (unsigned long long)tgtAll,
                 (!selAll || !tgtAll) ? "   [one side unknown — allowing, see the tri-state note]"
                                      : "");
        }
#endif
        return !crosses;
    }

    // GIFT: the share list is the engine's own set of legal targets — and its entries are SLOTS, so
    // this comparison is in slot space. Comparing it against a player id is what #45 was.
    int32_t list[16] = { 0 };
    const int n = readShareList(mgr, list, 16);
    for (int i = 0; i < n; ++i)
        if (list[i] == slot) return true;
    return false;
}

// --- did the UI ever ASK? ------------------------------------------------------------------------
//
// Run 28 put the panel on screen and it never appeared, with the DLL logging `raising
// CoopGiftPanelWanted` fifteen times. That leaves three very different causes and the log could not
// tell them apart, because a query answers silently:
//
//   1. the UI never resolves our entries at all  -> they are registered but not CONSUMED;
//   2. the UI asks once when the battle HUD binds and never again -> the expression is fine and what
//      is missing is a re-evaluation trigger (vanilla drives these off named events such as
//      BattleSelectionChanged, or off a context refresh we are not causing);
//   3. the UI asks constantly and gets false -> our own flag or expression is wrong.
//
// One counter per entry separates all three, and it costs nothing: these are called by the UI, not
// by us, so a call is itself the finding. Rate-limited because a per-frame query would otherwise
// bury the log.
//
// ★ It answered (2) in the end: §6aaa found the missing trigger was `update_constant` in our own
// markup, not anything absent from the engine. The counters stay because they are the cheapest thing
// on this list that can tell a dead binding from a working one, and the string binding still needs
// them (§6ggg).
#ifndef TW3K_RELEASE
static volatile long g_ccoQueryCalls[4];   // one per CanGiftToPlayerN
#endif

// ★★ #54: RATE-LIMITING BY COUNT HID THE ONE TRANSITION THAT MATTERS.
//
// The gift panel needs three clicks in the first battle of a session, and the measured reason is
// that it opens EMPTY: the first queries all answer false, the player reads that as "nothing
// happened", clicks again (closing it) and a third time. Whether that is `ccoBattleRoot()`,
// `recipientAt()` or `slotForPlayer()` cannot be told from the log, because calls #1-3 then #500 is
// exactly the window in which the answer is still false and nobody can see it flip.
//
// ⇒ Report every CHANGE as well, the way the occupancy and alliance checks already do. A per-frame
// query stays quiet while the answer is stable, so this costs nothing in the normal case and prints
// the single line the diagnosis needs: the moment false becomes true, and how long that took.
#ifndef TW3K_RELEASE
static long g_ccoQueryLast[4] = { -1, -1, -1, -1 };   // -1 = never answered yet
#endif

static bool ccoCountAndReport(int slot, const char* name, bool value)
{
#ifndef TW3K_RELEASE
    const long n = InterlockedIncrement(&g_ccoQueryCalls[slot]);

    const long now = value ? 1 : 0;
    if (g_ccoQueryLast[slot] != now) {
        const long was = g_ccoQueryLast[slot];
        g_ccoQueryLast[slot] = now;
        // ⚠ `was == -1` is the FIRST answer, not a transition — say so rather than printing
        // "changed from -1", which reads like a value the engine gave us.
        if (was < 0)
            logf("★ CCO QUERY: %s first answered %s (call #%ld).", name, value ? "TRUE" : "false", n);
        else
            logf("★★★ CCO QUERY: %s changed %s -> %s on call #%ld.%s", name,
                 was ? "TRUE" : "false", value ? "TRUE" : "false", n,
                 value ? "  ⇒ the button is LIVE from here; if the panel was opened before this, it"
                         " rendered EMPTY and that is #54."
                       : "  ⇒ the button just went dead.");
    }

    if (n <= 3 || (n % 500) == 0) {
        logf("CCO QUERY: %s asked by the UI (call #%ld) -> %s   <<< the engine IS reaching our "
             "entries", name, n, value ? "true" : "false");
    }

#endif
    return value;
}

#ifndef TW3K_RELEASE
void reportCcoQueryCounts()
{
    static const char* names[4] = {
        "CanGiftToPlayer0", "CanGiftToPlayer1", "CanGiftToPlayer2", "CanGiftToPlayer3"
    };
    long total = 0;
    for (int i = 0; i < 4; ++i) total += g_ccoQueryCalls[i];

    if (total == 0) {
        logf("CCO QUERIES: not one of our per-button queries has EVER been asked for by the UI.");
        logf("   ⇒ the resolver is answering (watch for 'CCO RESOLVER: answered') but nothing is "
             "asking. Either no pack is installed, or its bindings lack `update_constant` and were "
             "evaluated once at bind time.");
        return;
    }
    logf("CCO QUERIES: the UI IS reaching our entries. Counts:");
    for (int i = 0; i < 4; ++i)
        logf("      %-22s %ld", names[i], g_ccoQueryCalls[i]);
    logf("   ⇒ a low, non-zero count means it asked once at bind time and never re-evaluated — that "
         "is a missing refresh trigger, not a missing entry.");
}
#endif


// --- the registered functions themselves ---------------------------------------------------------
//
// Declared with two ignored parameters: the engine passes the CCO instance and, for a query, the
// result object. An action that ignores both is safe either way under the Microsoft x64 ABI, since
// arguments arrive in registers and the caller cleans up.

// ⚠ The trailing digit is a BUTTON POSITION, not a player id — the pack's names are unchanged
// (`CanGiftToPlayer0`..`3`) because changing them would need a pack rebuild for no gain, but what
// they mean changed with #45. Button 0 is the first live recipient, whoever that is.
static void ccoCanGiftTo0(void*, void* out) { ccoReturnBool(out, ccoCountAndReport(0, "CanGiftToPlayer0", coopCanGiftFromButton(0))); }
static void ccoCanGiftTo1(void*, void* out) { ccoReturnBool(out, ccoCountAndReport(1, "CanGiftToPlayer1", coopCanGiftFromButton(1))); }
static void ccoCanGiftTo2(void*, void* out) { ccoReturnBool(out, ccoCountAndReport(2, "CanGiftToPlayer2", coopCanGiftFromButton(2))); }
static void ccoCanGiftTo3(void*, void* out) { ccoReturnBool(out, ccoCountAndReport(3, "CanGiftToPlayer3", coopCanGiftFromButton(3))); }

// ---- player names on the buttons ----------------------------------------------------------------
//
// "Give to player 2" is honest and useless; it should say who. A CCO query CAN return a string — the
// value goes to `out->vtable[0x80]` as a CA string, where a bool uses 0x50 (read out of
// FUN_142D59FA0, the lobby's ReadyStatusText) — and plan E makes the four names free.
//
// ⚠ FAIL-VISIBLE, and deliberately so. `ContextTextLabel` is used 25 times in the vanilla battle HUD
// and EVERY one of them carries a `context_object_id`; not one uses the bare `BattleRoot.<Prop>`
// form our entries need. Only ContextVisibilitySetter and ContextCommandLeftClick do that, and those
// are exactly the two shapes plan E has already proven. So this binding may simply never be called,
// the way the ternary state binding never was (§6eee) — and if so the buttons keep the static text
// the pack ships. A blank button would have been the failure that looks like success.
//
// ⚠ The names come from the LOBBY, which no longer exists by the time a battle runs, so they are
// captured on the way past and served from our own table (see captureLobbyPlayerNames).
static constexpr size_t CCO_QUERY_SET_STRING = 0x80;   // out->vtable[0x80](out, CaString*)

// The value handed over is the engine's OWN wide string, copied out of the lobby record while that
// still existed (see captureLobbyPlayerNames). Nothing is built or allocated here, so this costs
// nothing at ~10 polls a second and there is no release rule to get wrong.
//
// ✗✗ THE "FAIL-VISIBLE" DESIGN WAS WRONG, and a live run disproved it (§6iii). This used to hand
// back NOTHING when no name had been captured, on the reasoning that the button would then keep the
// static text the pack ships. It does not: `ContextTextLabel` REPLACES the label, so a binding that
// returns nothing renders **blank**. tester saw exactly that — an unnamed but perfectly legal
// recipient, with an empty button, which still worked when clicked.
//
// ⇒ A string binding must ALWAYS return a value. With no captured name we build "Player N" instead.
//
// ✅★ THE MEASUREMENT (2026-08-04, tester's screenshot). The fallback was built with the engine's
// **narrow** constructor (FUN_140662DF0) while the captured names are the engine's own **wide**
// strings. The log recorded `no captured name for player 2 — handing back "Player 3" (narrow CA
// string)` and that button drew **blank**. A captured (wide) name in the same build drew correctly.
//
// ✅ FIXED 2026-08-05: the fallback is now built WIDE, with `FUN_140663120` — a single-call wide
// constructor that is the exact mirror of the narrow one, so the three-step builder the backlog
// described is not needed. See `RVA_MAKE_CASTRING_W` in offsets.h for how it was identified.
//
// ⚠⚠ **WHAT IS ESTABLISHED, AND WHAT IS NOT** — this file has been wrong about this once already.
//
// | claim | status |
// |---|---|
// | slot `0x80` is handed a **wide** CA string by the engine's own code | ★ **established.** `FUN_142D59FA0` (`ReadyStatusText`, the only known engine implementation of a string-returning CCO query) calls `(*(*out + 0x80))(out, s)` on both branches, and its shared epilogue then destroys `s` against the **wide** empty sentinel `0x143C5CB90` |
// | slot `0x80` **copies** rather than taking ownership | ★ **established**, by the same read: the engine frees the string immediately after the call returns |
// | a narrow string is **rejected** by slot `0x80` | 🔎 **SUSPECTED ONLY.** The concrete class of `out` was never found, so the code at slot `0x80` has never been read |
//
// ✗ **And the previous version of this comment asserted the rejection as tested and correct.** It
// was not. There is a competing explanation that fits the ABI exactly: narrow `"Player 3"` is 8
// characters, which fits the narrow small-string optimisation, so its bytes are
// `50 6C 61 79 65 72 20 33 | 00 … 88`. A **wide** reader sees the `0x88` marker, takes an SSO length
// of 8 and reads four CJK-range code points followed by NULs — which a HUD font draws as nothing.
// "Silently rejected" and "consumed as wide and rendered as glyphless" are indistinguishable on a
// screenshot, and both demand this same fix. ⇒ The fix is right; the *reason* is still one of two.
//
// ⇒ So this build also LOGS the two addresses that would settle it, once, on the first query: the
// object's vtable and the function in slot `0x80`, printed as Ghidra addresses. That costs nothing
// at runtime and turns the last inferred link into something that can simply be disassembled.
//
// ⚠ The buffer stays STATIC per button — but NOT for the reason previously written here. The engine
// does not take ownership (established above). It is static because the WIDE constructor
// **allocates**: "Player N" is over the wide SSO threshold, so building it per query would leak one
// game-heap allocation per call. One allocation per button per distinct occupant, never freed.
//
// ★ `button` is a POSITION IN THE RECIPIENT LIST (#45). It used to be a player id, which is how a
// three-player session came to draw the same person's name on two buttons: the labels were read out
// of the name table by position while the recipients were seated non-contiguously.
static void ccoReturnPlayerName(void* out, int button)
{
    if (button < 0 || button >= MAX_RECIPIENTS || !out) return;

    refreshRecipientList();
    const int id = recipientAt(button);
    if (id < 0) return;                    // no occupant: no label, and the button is greyed anyway

    const int nameId = lobbyRecordIdForRecipient(id);
    const void* ca = knownPlayerNameCa(nameId);
    if (!ca) {
        // ⚠ A FORMATTED buffer now, where this used to hold four fixed wide literals. It could do
        // that while the button *was* the player id; a button is a position now, so the label has to
        // name whoever is currently behind it — and it must be REBUILT when that changes, or the
        // panel shows a stale name, which is the very complaint that opened #45.
        alignas(8) static uint8_t fallback[MAX_RECIPIENTS][16] = { { 0 } };
        static int                builtFor[MAX_RECIPIENTS] = { -1, -1, -1, -1 };
        if (builtFor[button] != id) {
            wchar_t label[32];
            swprintf_s(label, L"Player %d", id + 1);
            __try {
                ((MakeCaStringWFn)(g_base + RVA_MAKE_CASTRING_W))(fallback[button], label);
                builtFor[button] = id;
                diagLogf("CCO QUERY: no captured name for player %d — button %d hands back \"Player %d\" "
                     "as a WIDE CA string (FUN_140663120). The narrow build of this drew blank on "
                     "2026-08-04; if this one still does, the label is not the problem.",
                     id, button, id + 1);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                logf(TW3K_MODE_TEXT("CCO QUERY: building the fallback name for player %d faulted — leaving the ", "gift name: building the fallback name for player %d faulted — leaving the ")
                     "label alone, which renders blank.", id);
                return;
            }
        }
        ca = fallback[button];
    }

    __try {
        const uintptr_t vt = *(uintptr_t*)out;
        if (vt <= 0x10000) return;
        const uintptr_t setter = *(uintptr_t*)(vt + CCO_QUERY_SET_STRING);

        // ★ ONE LINE THAT CLOSES AN OPEN QUESTION, printed once per process.
        //
        // Nothing in this project has ever identified the class behind `out`, which is why "a narrow
        // string is rejected" is still only suspected. Both of these are Ghidra addresses directly
        // (this binary has no ASLR slide), so one line of log is enough to go and read the code.
#ifndef TW3K_RELEASE
        static volatile long announced = 0;
        if (InterlockedCompareExchange(&announced, 1, 0) == 0)
            logf("CCO QUERY: the string result object's vtable is %016llX and slot 0x80 is %016llX "
                 "— both are Ghidra addresses (no ASLR slide). ⇒ Disassemble the second one and the "
                 "narrow-vs-wide question stops being an inference.",
                 (unsigned long long)vt, (unsigned long long)setter);

#endif
        if (setter > 0x10000) ((void(*)(void*, const void*))setter)(out, ca);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A query that faults must not take the UI down with it.
    }
}

#ifndef TW3K_RELEASE
static volatile long g_nameQueryCalls = 0;
#endif

static void ccoNameAsked(int button)
{
#ifndef TW3K_RELEASE
    const long n = InterlockedIncrement(&g_nameQueryCalls);
    if (n <= 4) {
        const int   id   = recipientAt(button);
        const char* name = (id >= 0) ? knownPlayerName(lobbyRecordIdForRecipient(id)) : "";
        logf("★★★ CCO QUERY: PlayerName%d asked by the UI (call #%ld) -> player %d, \"%s\" — a STRING "
             "binding is reaching our entries. This is the shape with no vanilla precedent, so it "
             "working at all is a finding. %s",
             button, n, id, (name && name[0]) ? name : "(none captured)",
             // ✗ This used to say "the button keeps its static text". It does not, and §6jjj had
             // already established why: ContextTextLabel REPLACES the label, so a binding with
             // nothing to give renders blank. The line was describing the design that was disproven.
             knownPlayerNameCa(lobbyRecordIdForRecipient(id))
               ? ""
               : "No captured name for the resolved lobby record; using the wide Player N fallback.");
    }
#endif
}

static void ccoPlayerName0(void*, void* out) { ccoNameAsked(0); ccoReturnPlayerName(out, 0); }
static void ccoPlayerName1(void*, void* out) { ccoNameAsked(1); ccoReturnPlayerName(out, 1); }
static void ccoPlayerName2(void*, void* out) { ccoNameAsked(2); ccoReturnPlayerName(out, 2); }
static void ccoPlayerName3(void*, void* out) { ccoNameAsked(3); ccoReturnPlayerName(out, 3); }

// =================================================================================================
//  PLAN D — hijack a dev entry INSIDE the array the UI already scans
//
//  Plans A and B are both dead, for different reasons, and D exists because of what killed B.
//
//    A  extend over the terminator      ❌ only 32 free bytes at runtime, needs 264 (run 26)
//    B  re-point the registry base      ❌ works perfectly, wrong pointer — the property lookup
//                                          `thunk_FUN_146E940D0` takes its base from
//                                          `LEA RCX,[0x1443E0370]`, an immediate compiled into the
//                                          code, and never consults the registry at all
//    D  overwrite an existing entry     ★ an 8-byte write INSIDE the vanilla array, so every
//                                          consumer finds it however it got there
//
//  ★ TARGET: `DevCycleArmy`, entry 62 at RVA 0x043E0940. Chosen over `DevKillEntireArmy` because
//  vanilla's own `hud_battle.twui.xml` references DevKillEntireArmy twice (the dev buttons) and
//  DevCycleArmy ZERO times — so hijacking it cannot disturb anything the game itself does. Both are
//  registered with `_guard_check_icall`, i.e. they are dev stubs with no real implementation.
//
//  ★★ WE REWRITE THE NAME TOO. The slot is { void* fn; u32 flags; u32 pad; const char* name; }, so
//  `name` is just a pointer — swapping it for a literal in our own DLL means the hijacked entry can
//  be called whatever we like. Two consequences:
//    * a pack's buttons no longer appear to call "kill entire army", which was plan D's one real
//      drawback;
//    * the UI designer's own binding, `GiftUnits(...)`, works UNMODIFIED — that name exists in
//      Warhammer 3 and in CA's shipped documentation, but not in this binary (§6uu).
//
//  ★★★ AND THE ACTION TAKES AN ARGUMENT. Confirmed twice over: vanilla 3K writes
//  `BattleRoot.DevKillEntireArmy( x )`, `SetSpeed(2)`, `NumMpVotesForSpeed(0)`, and WH3's own gift
//  UI writes `GiftUnits(this.PlayerSlot)`. So ONE hijacked action serves every recipient, which is
//  what retired the four per-recipient entries — that design only ever existed because we believed
//  actions were nullary.
//
//  ⚠ THIS IS A PROBE. The handler deliberately does not gift yet: it logs both incoming parameters
//  RAW, because how an argument actually arrives is exactly what is unknown. A CCO argument may be
//  an integer in a register, a boxed value, or a pointer to a variant — assuming would be the fourth
//  wrong assumption in a row. Once the log says what arrives, the gift call is three lines.
// =================================================================================================

// Entry 62, "DevCycleArmy" — absolute 0x1443E0940.
// ⚠ Derived, not typed: the first version of this line carried an extra digit (0x0443DF240) and
// pointed 4 GB outside the image. The static_assert below is the check that would have caught it at
// compile time, and it is cheap enough that every entry RVA should carry one.
static constexpr uintptr_t RVA_CCO_DEVCYCLEARMY = RVA_CCO_TABLE_BASE + 62 * CCO_ENTRY_SIZE;
static_assert(RVA_CCO_DEVCYCLEARMY == 0x043E0940, "DevCycleArmy is not where we think it is");
static_assert(RVA_CCO_DEVCYCLEARMY <  RVA_CCO_TERMINATOR, "entry 62 must precede the terminator");
static const char          CCO_HIJACK_NAME[]    = "GiftUnits";   // what we present it as

// ★ THE SECOND HIJACK — entry 63, `DevKillEntireArmy`, turned into the panel's visibility QUERY.
//
// This is what anchors the panel to the gift button instead of leaving it on screen all battle. The
// pack binds the popup's ContextVisibilitySetter to this name; the DLL raises it when the gift icon
// is clicked and drops it when a recipient is chosen.
//
// ⚠ It is the LAST dev stub. Both dev entries are now spent, so a third hijacked name would have to
// come out of a real vanilla property — which is a different risk entirely.
//
// Why this entry is safe to take: like DevCycleArmy it is registered with `_guard_check_icall`, i.e.
// a stub with no implementation, and vanilla's only two references to it are the dev buttons inside
// `dev_button_list`, which carries `is_dev_only` and therefore never renders in a retail build.
//
// ★ We rewrite the FLAGS as well as the fn and the name: `DevKillEntireArmy` is registered as an
// ACTION (flags=2) and we need a QUERY (flags=0). All three fields live in the same 0x18 slot we
// already own, so this costs nothing extra.
static constexpr uintptr_t RVA_CCO_DEVKILLENTIREARMY = RVA_CCO_TABLE_BASE + 63 * CCO_ENTRY_SIZE;
static_assert(RVA_CCO_DEVKILLENTIREARMY == 0x043E0958, "DevKillEntireArmy is not where we think it is");
static_assert(RVA_CCO_DEVKILLENTIREARMY <  RVA_CCO_TERMINATOR, "entry 63 must precede the terminator");
static_assert(RVA_CCO_DEVKILLENTIREARMY == RVA_CCO_TERMINATOR - CCO_ENTRY_SIZE,
              "entry 63 must be the LAST entry before the terminator");
static const char          CCO_QUERY_NAME[]     = "GiftPanelOpen";

static volatile bool g_ccoHijacked   = false;
static void*         g_hijackOrigFn  = nullptr;
static const char*   g_hijackOrigNam = nullptr;
static volatile long g_hijackCalls   = 0;
static volatile bool g_ccoQueryHijacked = false;
static CcoEntry g_queryOriginal = {};
static CcoEntry g_actionOriginal = {};
static DWORD g_queryRemovalProtection = 0, g_actionRemovalProtection = 0;
#ifndef TW3K_RELEASE
static volatile long g_panelQueryCalls  = 0;
#endif

// ★★★ THE CCO ABI, read out of a vanilla action that genuinely takes an argument.
//
// `SetSpeed` is `FUN_142EC5260`, registered flags=2 on CcoBattleTimeControl, and the twui calls it
// as `SetSpeed(0.4)`. Decompiled, it is unambiguous:
//
//     void SetSpeed(context, p2, p3, ArgList* p4) {
//         if (p4 != 0) {
//             n = thunk_FUN_146DC87A0(p4);              // argument COUNT
//             if (0 < n && n < 2) {
//                 v = FUN_140524FB0(p4);                // the argument VALUE
//                 FUN_142EC52E0(context, v);            // ...then the real work
//                 return;
//             }
//             error("The number of mandatory arguments doesn't match the expected list.");
//         }
//     }
//
// ⇒ The argument list is the **FOURTH** parameter (R9), not the second. The first probe read RDX,
// which is unused here — which is exactly why it saw a stable-looking pointer with none of our
// constants in it. Nothing was wrong with the hijack; the handler was looking in the wrong register.
//
// ⚠ `thunk_FUN_146DC87A0` sits at RVA 0x06DC87A0, inside the 187 MB RWX `.xcode` region — Denuvo's.
// The value accessor `FUN_140524FB0` is in ordinary code at RVA 0x00524FB0. So we dump the list
// object AND call the accessor under __try, and let the log say which one answered.
//
// ★★★★★ AND THE ACCESSOR RETURNS A **float**, IN XMM0 — NOT a u32 in EAX (§6vv.3).
//
//     float FUN_140524fb0(ArgCursor* c) {
//         ...evaluate argument c->base + c->cursor in c->context...
//         c->cursor = c->cursor + 1;                       // ★ it CONSUMES one argument
//         if (tag == 2) return f;                          // a float literal
//         if (tag == 1) return (float)(int)f;              // an int literal, converted
//         return 0.0f;
//     }
//
// The first build read `EAX` and logged `0x405BAC6E` on both clicks. That is not a value at all:
// `0x1405BAC20` is `FUN_1405BAC20`, the temp-value teardown the accessor calls last, and the number
// logged is 0x4E bytes into it. EAX held a leftover code address from inside that call, which is
// precisely why it never moved when the button did. Vanilla settles the signature independently:
// `FUN_142ECFBD0` (a numeric CcoBattleTimeControl action) does `fVar9 = (float)FUN_140524fb0(p4);`
// and compares it against a stored float.
//
// ⇒ An int argument arrives through the SAME accessor — the tag==1 branch converts it — so
// `GiftUnits(3)` is `(int)3.0f`. There is no separate integer accessor to hunt for.
//
// ★ The object in R9 is a CURSOR, not a list header, and this is its layout (read out of the
// accessor itself, then confirmed against the live dump in the 23:01 run):
//
//     +0x00  int   base index of the first argument      (0)
//     +0x04  int   cursor — POST-INCREMENTED by each read
//     +0x08  ptr   the argument expression node          ★ differs per call: each button has its own
//     +0x10  ptr   the evaluation context                (its +0x5E20 is the error reporter)
//     +0x18  byte  error flag, set when evaluation returns status 3
//
// ⚠ Two corrections to what the notes recorded from that run:
//   * `+0x04 == 1` is **not** "the argument count". It is the cursor, already advanced, because the
//     probe had called the accessor before it dumped. It does prove the call carried one argument
//     and that we consumed it — but by a different route than the one written down.
//   * `+0x28` is **not** a type handler. `0x140576DF6` is the return address of `CALL R10` at
//     `0x140576DF3` in `FUN_140576CC0`, the CCO dispatch site itself. The object is ~0x20 bytes and
//     everything past it in a 64-byte dump is unrelated stack.
//
// ⚠ Because the read advances the cursor, call it EXACTLY ONCE per invocation and keep the result.
static constexpr uintptr_t RVA_CCO_ARG_VALUE = 0x00524FB0;   // float FUN_140524FB0(ArgCursor*)

// Read the one argument out of the cursor. Returns false if it could not be read at all, which is
// a different thing from reading a number we do not like — the caller reports them differently.
static bool ccoReadArg(void* cursor, int& out)
{
    if (!cursor) return false;
    bool ok = false;
    __try {
        const float f = ((float(*)(void*))(g_base + RVA_CCO_ARG_VALUE))(cursor);
        out = (int)f;
        ok  = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("      !! the value accessor faulted.");
    }
    return ok;
}

// `GiftPanelOpen` — the panel's visibility, answered through the SECOND hijacked entry.
//
// A query is `void(context, ResultObject* out)` and hands its value to the result object rather than
// returning it: `(*(out->vtable + 0x50))(out, byte)`. Confirmed twice — `FUN_1430A50B0` (IsSpectator)
// on CcoBattleRoot, and `FUN_142D46CE0` (IsSinglePlayer) on the lobby type.
//
// ⚠ The pack MUST carry `update_constant` on this binding. Run 28 established that a context binding
// is otherwise evaluated once when the panel binds and never again — which for a visibility query
// means a panel that can never appear, no matter what the DLL sets. The designer's own pack uses
// exactly this property, which is where we learnt it.
static void ccoGiftPanelOpen(void*, void* out)
{
#ifndef TW3K_RELEASE
    const long n = InterlockedIncrement(&g_panelQueryCalls);
#endif
    const bool v = g_giftPanelWanted;
#ifndef TW3K_RELEASE
    if (n <= 3 || (n % 1000) == 0)
        logf("CCO QUERY: %s asked by the UI (call #%ld) -> %s", CCO_QUERY_NAME, n, v ? "true" : "false");
#endif
    ccoReturnBool(out, v);
}

// The gift action itself — reached through plan E's resolver today, and through the hijacked entry
// only if plan E could not install. Live as of the 23:52 run, where three different buttons produced
// 3, 7 and 9 with error=0: the argument travels, so ONE entry serves every recipient.
static void ccoHijackedGiftUnits(void* ctx, void* p2, void* p3, void* argCursor)
{
    const long n = InterlockedIncrement(&g_hijackCalls);

    // ★ The argument is a BUTTON POSITION since #45, not a player id. The pack is unchanged — its
    // buttons still pass 0..3 — but what those numbers mean is now "the Nth live recipient".
    int button = -1;
    if (!ccoReadArg(argCursor, button)) {
        logf("★★★ GiftUnits (#%ld): the argument could not be read — NOTHING GIFTED.", n);
        diagLogf("      ctx (RCX) = %016llX   ArgCursor (R9) = %016llX",
             (unsigned long long)ctx, (unsigned long long)argCursor);
        return;
    }

    // ⚠ REFUSE ANYTHING THAT IS NOT A BUTTON POSITION, LOUDLY. The probe pack passes 3/5/7/9/11, and
    // `3` is a perfectly legal position — so a stale probe pack does not fail, it silently gifts to
    // whoever button 3 holds. The log has to make that visible rather than let it read as a
    // successful click.
    if (button < 0 || button >= MAX_RECIPIENTS) {
        logf("★★★ GiftUnits(%d) (#%ld): NOT a button position (expected 0-%d) — nothing gifted.",
             button, n, MAX_RECIPIENTS - 1);
        diagLogf("      ⇒ this is almost certainly the PROBE pack, which passes 3/5/7/9/11. Install the "
             "live pack; ⚠ note its own GiftUnits(3) button IS a valid position and would have "
             "gifted.");
        return;
    }

    refreshRecipientList();
    const int id = recipientAt(button);

    diagLogf("★★★ GiftUnits(%d) (#%ld) — the UI called the hijacked entry with a live argument. Button "
         "%d currently targets player %d (%s).", button, n, button, id,
         (id >= 0) ? ((knownPlayerName(lobbyRecordIdForRecipient(id)) && knownPlayerName(lobbyRecordIdForRecipient(id))[0]) ? knownPlayerName(lobbyRecordIdForRecipient(id))
                                                                      : "no captured name")
                   : "NOBODY — that button has no live recipient behind it");
    diagLogf("      ctx (RCX) = %016llX   p2 (RDX) = %016llX   p3 (R8) = %016llX   ArgCursor (R9) = %016llX",
         (unsigned long long)ctx, (unsigned long long)p2, (unsigned long long)p3,
         (unsigned long long)argCursor);

    // Legality is the engine's answer, not ours (see coopCanGiftFromButton): the share list is the
    // set of slots a gift may go to. Refusing here rather than at the button is deliberate for now —
    // the greying-out query would need a SECOND hijacked entry, and that is its own variable.
    if (!coopCanGiftFromButton(button)) {
        const int dir = giftDirection(ccoBattleRoot());
        diagLogf("      ⇒ button %d (player %d) is not a legal recipient here (%s). Nothing gifted.",
             button, id,
             dir == 1 ? "RETURN direction, and that seat is yours, empty or across the alliance line"
                      : "GIFT direction, and that seat is not in the share list, or it is you");
        if (dir != 1) {
            int32_t list[16] = { 0 };
            const int n = readShareList(ccoBattleRoot(), list, 16);
            char buf[128] = { 0 };
            int  off = 0;
            for (int i = 0; i < n && off < (int)sizeof(buf) - 8; ++i)
                off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE, "%d ", list[i]);
            diagLogf("      ⇒ the share list holds slots: %s— ★ SLOTS, measured (#45). The buttons are "
                 "positions in the live-recipient list and each one resolves to a player id, so a "
                 "seat missing from this list is a button the can-gift query greys out.",
                 n > 0 ? buf : "(nothing) ");
        }
        return;
    }

    coopGiftToPlayer(id);
}

// =================================================================================================
//  PLAN E — answer for names the engine cannot find, instead of spending its entries
//
//  Plan D works but is out of room: both dev stubs are hijacked, and a third name would have to be
//  taken from a property the game actually uses. Plan E removes the ceiling entirely.
//
//  ★ EVERY CCO property lookup goes through one thunk:
//
//      1405BC250:  E9 7B 7E 8D 06      JMP 0x146E940D0        ; 40+ call sites
//
//  and the scan behind it returns 0 at the terminator (§6uu). So a name we invented is not an
//  error today — it is a MISS, with a clean, well-defined return value.
//
//  ⇒ We redirect the thunk and act as a FALLBACK: call the real implementation first, and answer
//  from our own registry only when it found nothing. Consequences worth stating plainly:
//
//    * vanilla behaviour is BIT-IDENTICAL. We act only where the engine already returned "not
//      found", so no vanilla name can change meaning — which is not true of plan D.
//    * ANY CCO type can be extended, not just CcoBattleRoot, because the table base identifies it.
//    * the arrays are never written to at all, so BOTH DEV STUBS GO BACK to the game.
//    * it catches consumers we never found — the assumption that killed plan B (§6uu).
//
//  ⚠ The one thing this cannot do is change what an EXISTING name means. That is plan D's job, and
//  it is why the hijack code below is kept rather than deleted.
// =================================================================================================

// ⚠ NOT A THUNK ANY MORE. In 1.7.1 this address held five bytes of `JMP` into the Denuvo .xcode
// blob and plan E redirected that displacement. 1.7.2 removed Denuvo and the linear scan is
// compiled in place here — same algorithm instruction for instruction, same jump displacements,
// re-encoded from Clang-style register moves (48 89 CB / 29 C1 / 31 C0) to MSVC ones (48 8B D9 /
// 2B C8 / 33 C0). So it is hooked as a function entry now, not redirected as a thunk.
static constexpr uintptr_t RVA_CCO_RESOLVER = 0x005BC650;   // the linear scan itself

// PUSH RBX ; SUB RSP,0x20 — the two instructions entryRedirect5 steals. Six bytes, and the reason
// it is six and not fourteen is that a rel32 CALL starts at +0x0C; see entryRedirect5 in
// detour.cpp. Neither of these is relocatable, which is the whole requirement.
static const uint8_t EXPECT_CCO_RESOLVER[6] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };
static constexpr uintptr_t RVA_CASTRING_CSTR      = 0x00668CD0;   // FUN_140668CD0(CaString*) -> char*
static constexpr uintptr_t RVA_CCO_BATTLEROOT_TBL = RVA_CCO_TABLE_BASE;

using CcoResolveFn = void* (*)(void*, void*);

static Detour       g_resolverDetour;
static uintptr_t    g_origResolver     = 0;      // the trampoline: stolen bytes + jump back
#ifndef TW3K_RELEASE
static volatile long g_resolverLookups = 0;
#endif
#ifndef TW3K_RELEASE
static volatile long g_resolverAnswers = 0;
#endif

// ★★ #73 — THE CALL-VOLUME INSTRUMENT, and why these are PLAIN `++` and not Interlocked.
//
// The question this exists to answer is "is the slow panel US or is it vanilla?", and that is an
// ORDER-OF-MAGNITUDE question: 300 calls and 3,000,000 calls demand completely different responses.
// A racing increment loses a few counts and still answers it. A LOCKED increment would put back
// exactly the per-lookup bus-locked RMW this ticket just removed — measuring the cost by
// reintroducing it.
//
// ⇒ Deliberately approximate. Read them as magnitudes, never as exact totals, and never derive
// anything from a small difference between two of them.
#ifndef TW3K_RELEASE
static unsigned long long g_ccoCalls   = 0;   // every entry into the hook
#endif
#ifndef TW3K_RELEASE
static unsigned long long g_ccoMisses  = 0;   // the engine's own resolver said "no such name"
#endif
#ifndef TW3K_RELEASE
static unsigned long long g_ccoForeign = 0;   // ...and it was not even our table (the cheap way out)
#endif

// ★★ #73 — THE TABLE ADDRESS, RESOLVED ONCE AT INSTALL.
//
// Every entry below targets the same type, so "is this our table?" is ONE compare — not a
// ten-iteration scan that recomputes `g_base + rva` on each step. That scan used to run on every
// MISS, and misses are the ordinary case: the engine probes for optional names all over the UI and
// each probe reaches this hook. tester, 2026-08-18: opening the bandit reforms tree took seconds.
//
// ⚠ The per-entry `tableRva` is KEPT, because the struct exists to allow a second type later.
// `installCcoResolverHook` checks they still all agree and leaves this at 0 if they do not, which
// puts the hook back on the general scan rather than silently answering for the wrong type.
static uintptr_t g_ccoTableAddr = 0;

// Our own entries, in the same shape the engine's table uses but never written into it.
struct CcoExtra {
    uintptr_t   tableRva;   // which type this belongs to
    const char* name;
    void*       fn;
};

// Forward declarations: the implementations are the same ones plan D pointed the stubs at.
static void ccoHijackedGiftUnits(void*, void*, void*, void*);
static void ccoGiftPanelOpen(void*, void*);

// ★ Four per-recipient queries, which plan D could never have afforded — it had two entries in the
// whole world and both were spent. Plan E makes a name free, so the panel can grey out anyone who
// cannot receive instead of offering four buttons of which three refuse.
//
// This is the fix for the confusion in the 01:37 run: gifting to the third human needed the button
// labelled "player 4", because a save lobby seats players into slots 0/1/3 and our ids ARE slots.
// With these bound, the illegal buttons stop being clickable and the numbering stops mattering.
static const CcoExtra g_ccoExtras[] = {
    { RVA_CCO_BATTLEROOT_TBL, "GiftUnits",        (void*)&ccoHijackedGiftUnits },
    { RVA_CCO_BATTLEROOT_TBL, "GiftPanelOpen",    (void*)&ccoGiftPanelOpen     },
    { RVA_CCO_BATTLEROOT_TBL, "CanGiftToPlayer0", (void*)&ccoCanGiftTo0        },
    { RVA_CCO_BATTLEROOT_TBL, "CanGiftToPlayer1", (void*)&ccoCanGiftTo1        },
    { RVA_CCO_BATTLEROOT_TBL, "CanGiftToPlayer2", (void*)&ccoCanGiftTo2        },
    { RVA_CCO_BATTLEROOT_TBL, "CanGiftToPlayer3", (void*)&ccoCanGiftTo3        },
    // ⚠ Distinct from the bare "PlayerName" the binary already carries (@1437D8570, a property on
    // another type): plan E only ever answers a name the engine's own scan MISSED, so a collision
    // would silently hand the UI vanilla's entry instead of ours. The digit suffix keeps them apart.
    { RVA_CCO_BATTLEROOT_TBL, "PlayerName0",      (void*)&ccoPlayerName0       },
    { RVA_CCO_BATTLEROOT_TBL, "PlayerName1",      (void*)&ccoPlayerName1       },
    { RVA_CCO_BATTLEROOT_TBL, "PlayerName2",      (void*)&ccoPlayerName2       },
    { RVA_CCO_BATTLEROOT_TBL, "PlayerName3",      (void*)&ccoPlayerName3       },
};

static void* ccoResolveHook(void* table, void* nameObj)
{
    TW3K_DIAGNOSTIC(++g_ccoCalls);                       // see the note at g_ccoCalls: plain on purpose

    // The real scan first. If the engine knows the name, nothing we do can affect it.
    void* fn = nullptr;
    __try {
        fn = ((CcoResolveFn)g_origResolver)(table, nameObj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }

    // ★★ #73 — A PLAIN READ BEFORE THE ATOMIC. This runs for EVERY property the UI asks for, and an
    // InterlockedIncrement is a locked read-modify-write on one shared cache line. On a path the UI
    // walks for every bound property of every node in a panel, that is a real cost — paid to gate a
    // log line that stops after the third call.
    // ⚠ The relaxed read is ALLOWED to race: two threads can both pass it and both increment. The
    // `seen <= 3` below still bounds the output, so the worst case is a duplicated line, never a
    // wrong one. Deliberate — a lock here would reintroduce exactly what this removes.
#ifndef TW3K_RELEASE
    if (g_resolverLookups < 3) {
        const long seen = InterlockedIncrement(&g_resolverLookups);
        if (seen <= 3)
            logf("CCO RESOLVER: hook is live (lookup #%ld, table=%016llX) — every property the UI asks "
                 "for now passes through here.", seen, (unsigned long long)table);
    }

#endif
    if (fn) return fn;
    TW3K_DIAGNOSTIC(++g_ccoMisses);

    // A miss. Check the table base BEFORE touching the name: misses are ordinary traffic (the
    // engine probes for optional names all over the UI) and this runs on the UI's hot path.
    const uintptr_t tbl = (uintptr_t)table;
    if (g_ccoTableAddr) {
        if (tbl != g_ccoTableAddr) { TW3K_DIAGNOSTIC(++g_ccoForeign); return nullptr; }   // one compare
    } else {
        bool ours = false;                              // entries disagree on the table — scan
        for (const CcoExtra& e : g_ccoExtras)
            if (tbl == g_base + e.tableRva) { ours = true; break; }
        if (!ours) { TW3K_DIAGNOSTIC(++g_ccoForeign); return nullptr; }
    }

    __try {
        const char* want = ((const char* (*)(void*))(g_base + RVA_CASTRING_CSTR))(nameObj);
        if (!want) return nullptr;
        for (const CcoExtra& e : g_ccoExtras) {
            // ⚠ The per-entry table test only runs on the fallback path now. Above has already
            // settled the table; re-deciding it ten times per miss is what this costs us.
            if (!g_ccoTableAddr && tbl != g_base + e.tableRva) continue;
            if (want[0] != e.name[0])       continue;   // one byte kills most before the strcmp
            if (strcmp(want, e.name) != 0)  continue;
#ifndef TW3K_RELEASE
            const long n = InterlockedIncrement(&g_resolverAnswers);
            if (n <= 5 || (n % 2000) == 0)
                logf("★★★ CCO RESOLVER: answered \"%s\" (#%ld) — a name that exists NOWHERE in the "
                     "game's own table. No vanilla entry was spent.", e.name, n);
#endif
            return e.fn;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("CCO RESOLVER: faulted reading the wanted name — returning not-found, as the engine "
             "already had.");
    }
    return nullptr;
}

// ★★★ #73 — the one line that says whether the slow panel is OURS.
//
// tester, 2026-08-18, on the build that removed the per-lookup atomic and the ten-iteration scan:
// *"the opening of the bandit reforms (bandit network) is still pretty slow ... might just be
// vanilla behaviour."* That is the right doubt to have, and nothing in the logs could settle it —
// the hook had no reported call counter at all, so "we are called constantly" and "we are barely
// called" produced identical output.
//
// ⇒ Read it like this, after opening the slow panel once:
//   calls in the HUNDREDS      the hook is not the cost. The panel is slow in vanilla too, and #73
//                              should be closed as "not ours" rather than optimised further.
//   calls in the MILLIONS      the cost is CALL VOLUME, not the work inside the hook, and the fix
//                              is a different shape — an early-out in the thunk, or not hooking the
//                              resolver at all and registering the names another way.
//   misses ~= calls            the engine's own table answers almost nothing through this path,
//                              which would make the miss path the one that matters.
//   foreign ~= misses          our table is rarely the one being asked, so the early-out added in
//                              this ticket is doing the work it was added for.
#ifndef TW3K_RELEASE
void reportCcoResolver()
{
    logf("CCO resolver (#73): calls=%llu  misses=%llu  foreign-table=%llu  answered=%ld  (hook %s)",
         g_ccoCalls, g_ccoMisses, g_ccoForeign, g_resolverAnswers,
         g_resolverDetour.active ? "ACTIVE" : "inactive");
    if (g_ccoCalls == 0) {
        logf("  ⇒ never called. Either the UI has not asked for a property yet, or the thunk "
             "redirect did not take — check for 'CCO resolver: INSTALLED' above.");
        return;
    }
    logf("  ⇒ %llu call(s). Hundreds means this hook is NOT the cost of a slow panel and #73 closes "
         "as \"not ours\"; millions means the cost is call VOLUME and the fix is a different shape "
         "(an early-out in the thunk), not more tuning inside the hook.", g_ccoCalls);
    logf("  ⚠ These counters are PLAIN increments and race under load — magnitudes only, never "
         "exact totals, and never a small difference between two of them.");
}
#endif


bool installCcoResolverHook()
{
    if (!entryRedirect5(g_resolverDetour, g_base + RVA_CCO_RESOLVER,
                        sizeof(EXPECT_CCO_RESOLVER), EXPECT_CCO_RESOLVER,
                        (uintptr_t)&ccoResolveHook, &g_origResolver, "CCO resolver"))
        return false;

    // ⚠ REGRESSION, 01:12 run: this line was missing and the whole panel silently did not exist.
    // Panel mode used to be armed inside plan D's query installer, which plan E does not call — so
    // the gift icon kept gifting directly, GiftPanelOpen stayed false forever, the panel never
    // appeared, and the run looked "unchanged from the earlier version" because it WAS unchanged.
    // The query resolving through plan E is what proved the hook worked; the action was never
    // reached at all.
    g_giftPanelMode = true;

    // ★ #73 — collapse the "is this our table" scan to one compare, but only if the entries really
    // do all name the same type. If a second type is ever added, this stays 0 and the hook keeps
    // the general scan: slower, and correct, which is the right way round.
    {
        bool uniform = true;
        for (const CcoExtra& e : g_ccoExtras)
            if (e.tableRva != g_ccoExtras[0].tableRva) { uniform = false; break; }
        g_ccoTableAddr = uniform ? (g_base + g_ccoExtras[0].tableRva) : 0;
        if (!uniform)
            diagLogf("   ⚠ CCO extras name more than one table, so the resolver keeps the per-entry "
                 "scan on every miss (#73's fast path is off). Correct, just slower.");
    }

    diagLogf("   ★ PLAN E IS LIVE. %zu name(s) added without touching the game's table:",
         sizeof(g_ccoExtras) / sizeof(g_ccoExtras[0]));
    for (const CcoExtra& e : g_ccoExtras)
        diagLogf("        %-16s on the table at RVA_%08llX", e.name, (unsigned long long)e.tableRva);
    diagLogf("   ⇒ DevCycleArmy and DevKillEntireArmy are LEFT ALONE — the game keeps both dev entries.");
    diagLogf("   ⚠ Watch for 'CCO RESOLVER: answered' below. If it never appears, the UI is not reaching "
         TW3K_MODE_TEXT("us and F6 gives back the F10 + gift-icon path.", "us verify the installed pack."));
    return true;
}

void removeCcoResolverHook()
{
    detourRemove(g_resolverDetour, "CCO resolver");
}

// Entry 63 -> the panel's visibility query. Same write as the first hijack, one field wider: the
// flags go 2 (action) -> 0 (query) as well, because that is what the entry has to BE now.
static bool installCcoPanelQuery()
{
    if (g_ccoQueryHijacked) return true;
    const char* stage = "entry pointer";
    __try {
        CcoEntry* e = (CcoEntry*)(g_base + RVA_CCO_DEVKILLENTIREARMY);

        stage = "raw read of the slot";
        uint8_t raw[CCO_ENTRY_SIZE] = { 0 };
        if (!safeRead(e, raw, sizeof(raw))) {
            logf("CCO QUERY HIJACK: cannot read the slot at RVA_%08llX — NOT hijacking. The panel "
                 "will be visible for the whole battle, as it was before.",
                 (unsigned long long)RVA_CCO_DEVKILLENTIREARMY);
            return false;
        }

        stage = "VirtualProtect";
        DWORD old = 0;
        if (!VirtualProtect(e, sizeof(CcoEntry), PAGE_READWRITE, &old)) {
            logf("CCO QUERY HIJACK: VirtualProtect failed (%lu) — NOT hijacking.", GetLastError());
            return false;
        }

        memcpy(&g_queryOriginal, raw, sizeof(g_queryOriginal));
        g_ccoQueryHijacked = true; // a partial write must still be removed
        stage = "write fn/flags/name";
        e->fn    = (void*)&ccoGiftPanelOpen;
        e->flags = 0;                                  // action -> query
        e->name  = CCO_QUERY_NAME;

        stage = "restore protection";
        DWORD tmp = 0;
        VirtualProtect(e, sizeof(CcoEntry), old, &tmp);

        stage = "read-back";
        if (e->fn != (void*)&ccoGiftPanelOpen || e->flags != 0 ||
            strcmp(e->name, CCO_QUERY_NAME) != 0) {
            logf("CCO QUERY HIJACK: the write did not stick — entry 63 left as found.");
            return false;
        }

        g_ccoQueryHijacked = true;
#ifdef TW3K_RELEASE
        logf("FIX gift panel query: fallback installed");
#endif
        g_giftPanelMode    = true;     // ★ there is now a panel to defer to, so stop gifting directly
        diagLogf("CCO QUERY HIJACK: entry 63 \"DevKillEntireArmy\" -> \"%s\" (query) at RVA_%08llX.",
             CCO_QUERY_NAME, (unsigned long long)RVA_CCO_DEVKILLENTIREARMY);
        diagLogf("   ★ PANEL MODE IS NOW AUTOMATIC: clicking the gift icon raises %s instead of gifting, "
             TW3K_MODE_TEXT("and the pack's panel — anchored to the gift button — asks who. F6 still toggles it off.", "and the pack's panel — anchored to the gift button — asks who. The panel is selected automatically."),
             CCO_QUERY_NAME);
        diagLogf("   ⚠ The pack must bind visibility with `update_constant`, or the UI asks once at bind "
             "time and the panel can never appear (run 28).");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("CCO QUERY HIJACK: FAULTED at stage \"%s\" (code %08lX). Entry 63 is untouched and the "
             "gift itself is unaffected.", stage, GetExceptionCode());
        return false;
    }
}

bool installCcoHijack()
{
    if (g_ccoHijacked) return true;
    // Staged, and each stage says so BEFORE it runs. The first attempt logged a bare "FAULTED" and
    // that is the readout this project keeps being punished by — plan B had just memcpy'd all 64
    // entries INCLUDING this one, so "the memory is unreadable" was already disproven and the log
    // still could not say which line died.
    const char* stage = "entry pointer";
    __try {
        CcoEntry* e = (CcoEntry*)(g_base + RVA_CCO_DEVCYCLEARMY);

        stage = "raw read of the slot";
        uint8_t raw[CCO_ENTRY_SIZE] = { 0 };
        if (!safeRead(e, raw, sizeof(raw))) {
            logf("CCO HIJACK: cannot even read the slot at RVA_%08llX — NOT hijacking.",
                 (unsigned long long)RVA_CCO_DEVCYCLEARMY);
            return false;
        }
        char hex[80] = { 0 };
        for (size_t i = 0; i < sizeof(raw); ++i)
            _snprintf_s(hex + i * 3, 4, _TRUNCATE, "%02X ", raw[i]);
        diagLogf("CCO HIJACK: slot at RVA_%08llX reads: %s",
             (unsigned long long)RVA_CCO_DEVCYCLEARMY, hex);

        stage = "name compare";
        if (!e->name || strcmp(e->name, "DevCycleArmy") != 0) {
            diagLogf("CCO HIJACK: entry at RVA_%08llX is \"%s\", expected \"DevCycleArmy\" — NOT hijacking.",
                 (unsigned long long)RVA_CCO_DEVCYCLEARMY, e->name ? e->name : "<null>");
            return false;
        }

        g_hijackOrigFn  = e->fn;
        g_hijackOrigNam = e->name;

        stage = "VirtualProtect";
        DWORD old = 0;
        if (!VirtualProtect(e, sizeof(CcoEntry), PAGE_READWRITE, &old)) {
            logf("CCO HIJACK: VirtualProtect failed (%lu) — NOT hijacking.", GetLastError());
            return false;
        }
        diagLogf("CCO HIJACK: page made writable (was protect=0x%lX). Writing fn then name.", old);

        memcpy(&g_actionOriginal, raw, sizeof(g_actionOriginal));
        g_ccoHijacked = true;
        stage = "write fn";
        e->fn   = (void*)&ccoHijackedGiftUnits;
        stage = "write name";
        e->name = CCO_HIJACK_NAME;

        stage = "restore protection";
        DWORD tmp = 0;
        VirtualProtect(e, sizeof(CcoEntry), old, &tmp);

        stage = "read-back";
        if (e->fn != (void*)&ccoHijackedGiftUnits || strcmp(e->name, CCO_HIJACK_NAME) != 0) {
            logf("CCO HIJACK: the write did not stick — table left as found.");
            return false;
        }

        g_ccoHijacked = true;
#ifdef TW3K_RELEASE
        logf("FIX gift action: fallback installed");
#endif
        diagLogf("CCO HIJACK: entry 62 \"DevCycleArmy\" -> \"%s\" at RVA_%08llX (fn %p -> %p).",
             CCO_HIJACK_NAME, (unsigned long long)RVA_CCO_DEVCYCLEARMY,
             g_hijackOrigFn, (void*)&ccoHijackedGiftUnits);
        diagLogf("   This is INSIDE the vanilla array, which is the array the UI's own lookup scans "
             "(its base is an immediate, not a pointer — see plan B's failure).");
        diagLogf("   ★ LIVE: %s(id) gifts the current selection to player id, 0-3. The argument is read "
             "out of the cursor in R9 as a float (§6vv.3); 3/7/9 came back correct in the 23:52 run.",
             CCO_HIJACK_NAME);
        diagLogf("   ⚠ Anything outside 0-3 is refused and logged — but the PROBE pack's GiftUnits(3) "
             "button passes a VALID id, so a stale probe pack gifts to player 3 rather than failing.");

        // The second entry. Deliberately AFTER the first has succeeded and reported: if this one
        // faults, the gift still works and the panel is simply always-on, which is exactly the
        // behaviour of the pack that already ran.
        installCcoPanelQuery();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("CCO HIJACK: FAULTED at stage \"%s\" (code %08lX). The table is untouched.",
             stage, GetExceptionCode());
        return false;
    }
}

static void restoreCcoEntry(uintptr_t rva, const CcoEntry& original, volatile bool& active, DWORD& removalProtection)
{
    if (!active) return;
    __try {
        auto* entry = (CcoEntry*)(g_base + rva);
        DWORD old = 0;
        if (!VirtualProtect(entry, sizeof(*entry), PAGE_READWRITE, &old)) {
            logf("CCO restore failed at RVA_%08llX; retaining active state; restart required", (unsigned long long)rva);
            return;
        }
        if (!removalProtection) removalProtection = old;
        memcpy(entry, &original, sizeof(*entry));
        DWORD tmp = 0;
        if (!VirtualProtect(entry, sizeof(*entry), removalProtection, &tmp)) {
            logf("CCO protection restore failed; retaining active state; restart required");
            return;
        }
        removalProtection = 0;
        active = false;
        diagLogf("CCO restored at RVA_%08llX", (unsigned long long)rva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("CCO restore faulted; retaining active state and DLL; restart required");
    }
}

void removeCcoHijack()
{
    restoreCcoEntry(RVA_CCO_DEVKILLENTIREARMY, g_queryOriginal, g_ccoQueryHijacked, g_queryRemovalProtection);
    restoreCcoEntry(RVA_CCO_DEVCYCLEARMY, g_actionOriginal, g_ccoHijacked, g_actionRemovalProtection);
}

#ifndef TW3K_RELEASE
void reportCcoHijack()
{
    if (!g_ccoHijacked) { logf("CCO HIJACK: not installed."); return; }
    logf("CCO HIJACK: \"%s\" is live, called %ld time(s) by the UI.",
         CCO_HIJACK_NAME, g_hijackCalls);
    if (g_hijackCalls == 0)
        logf("   ⇒ nothing has invoked it yet. Needs a pack binding a button to BattleRoot.%s(3), "
             "and a click.", CCO_HIJACK_NAME);
}
#endif


bool installShareHook()
{
    return detourInstall(g_shareDetour, g_base + RVA_SHARE_UNITS, SHARE_STOLEN_LEN,
                         EXPECT_SHARE_UNITS, (uintptr_t)&shareUnitsHook,
                         (void**)&g_origShareUnits, "share hook");
}

// Read-only build check — confirms this exe matches the analysed one without installing anything.
//
// Reports WHICH KIND of failure, because once the DLL is allowed to arm itself late (session 6t)
// the two mean different things: UNREADABLE is "those bytes are not there yet", which waiting fixes,
// while MISMATCH is "a different game build", which waiting never will.
//
// ⚠ Both are still retried. Under Denuvo a page can read as *garbage* rather than as unmapped before
// it is decrypted, so an early mismatch is not proof of a bad build — it is only proof at the point
// we give up. The give-up message says which kind it ended on, and that is the diagnosis.
enum SigState { SIG_OK = 0, SIG_UNREADABLE, SIG_MISMATCH };

static SigState checkSite(uintptr_t rva, const uint8_t* expected, size_t len, const char* tag,
                          bool verbose)
{
    const uintptr_t addr = g_base + rva;
    uint8_t cur[32] = { 0 };
    if (!safeRead((void*)addr, cur, len)) {
        if (verbose) diagLogf("  %-13s UNREADABLE at %016llX", tag, (unsigned long long)addr);
        return SIG_UNREADABLE;
    }
    for (size_t i = 0; i < len; ++i) if (cur[i] != expected[i]) {
        if (verbose) diagLogf("  %-13s MISMATCH at %016llX byte %zu (want %02X got %02X)",
                          tag, (unsigned long long)addr, i, expected[i], cur[i]);
        return SIG_MISMATCH;
    }
    if (verbose) diagLogf("  %-13s OK  (%016llX)", tag, (unsigned long long)addr);
    return SIG_OK;
}

// True only if every site matches. The out-counts let the retry loop say something useful without
// re-printing nine lines a second.
bool verifyAllSignatures(bool verbose, int* unreadable, int* mismatched)   // defaults are on the declaration in tw3k.h
{
    struct Site { uintptr_t rva; const uint8_t* bytes; size_t len; const char* tag; };
    static const Site kSites[] = {
        { RVA_LOBBY_REFRESH,  EXPECT_LOBBY_REFRESH,  LOBBY_STOLEN_LEN,  "lobby guard"  },
        { RVA_MP_ADVERTISE,   EXPECT_MP_ADVERTISE,   MP_STOLEN_LEN,     "MP session"   },
        { RVA_SHARE_UNITS,    EXPECT_SHARE_UNITS,    SHARE_STOLEN_LEN,  "share"        },
        { RVA_JOIN_HANDLER,   EXPECT_JOIN_HANDLER,   JOIN_STOLEN_LEN,   "join hook"    },
        { RVA_SLOT_APPEND,    EXPECT_SLOT_APPEND,    SEAT_STOLEN_LEN,   "seat order"   },
        { RVA_PANEL_POPULATE, EXPECT_PANEL_POPULATE, PANEL_STOLEN_LEN,  "panel probe"  },
        { RVA_SLOT_CHANGED,   EXPECT_SLOT_CHANGED,   SLOTCH_STOLEN_LEN, "slot-changed" },
        { RVA_LOBBY_TICK,     EXPECT_LOBBY_TICK,     TICK_STOLEN_LEN,   "lobby tick"   },
        { RVA_PANEL_RESET,    EXPECT_PANEL_RESET,    RESET_STOLEN_LEN,  "panel reset"  },
        { RVA_TELESTRATION_CTOR, EXPECT_TELESTRATION_CTOR, TELESTRATION_STOLEN,
                                                                        "telestration" },
    };

    int nUnread = 0, nBad = 0;
    for (const Site& s : kSites) {
        const SigState st = checkSite(s.rva, s.bytes, s.len, s.tag, verbose);
        if      (st == SIG_UNREADABLE) ++nUnread;
        else if (st == SIG_MISMATCH)   ++nBad;
    }
    if (unreadable) *unreadable = nUnread;
    if (mismatched) *mismatched = nBad;
    return nUnread == 0 && nBad == 0;
}
