// lobby.cpp - Lobby: faction assignment, auto-faction, tick drain, slot-cache guard, player-vector and ready-check watches.
//
// Split out of the original single-file tw3k_coop.cpp. Behaviour is unchanged: this was pure
// code motion. ../tw3k_coop.ASBUILT.cpp remains the known-good single-file snapshot.

#include "tw3k.h"

// ---- faction assignment: make the lobby notice a player who has no panel --------------------
//
// Run 6 observed the fresh lobby learning about a player when that player's faction-change
// message (id 0xCD) arrives: 1.7.2 FUN_1404982E0 case 0xCD deserialises the slot index off the wire and
// notifies the lobby's listener. Player 3 has no panel, so no faction dropdown, so it never sends
// that message — and therefore never appears in the lobby. That fresh-lobby dependency is circular.
// ★ B4-S2 established a separate two-TEAM enumeration bound in the save-lobby record builder.
// saveload.cpp repairs that path using saved faction records, without sending a new faction.
//
// This breaks the circle by sending the message ourselves:
//
//   this    = *(*(DAT_1443B7238 + 0x4E8) + 0x98) + 0x18     // an inline sub-object
//   service = this->vtable[0x208](this)
//   localId = service->vtable[0x20](service)                // do NOT hardcode an id
//   FUN_1404A6580(service, localId, partyKey, factionKey, 0)
//
// Using the game's own local player id (rather than assuming "player 3 == 2") also sidesteps the
// unresolved question of whether that argument is a player id or a slot index: whatever the engine
// hands us for this machine is by definition correct for this machine.
//
// Both key strings are resolved internally against DB tables — factionKey against `factions_table`
// (FUN_1409212A0) and partyKey against `political_parties_table` (FUN_14094C7A0) — so they must be
// real keys. Every pair below was verified against the actual vanilla tables in database.pack, not
// guessed from the naming pattern.
//
// MUST run on a game thread: this builds a network message and hands it to the transport. So a
// keypress only QUEUES an index, and the request is drained inside whichever hook fires next —
// the same discipline the slot expansion uses.

static constexpr uintptr_t RVA_SET_FACTION    = 0x004A6580; // FUN_1404A6580
static constexpr uintptr_t RVA_FRONTEND_ROOT2 = 0x043B7238; // DAT_1443B7238

typedef bool (*SetFactionFn)(void*, uint8_t, void*, void*, uint32_t);
typedef void* (*GetServiceFn)(void*);
typedef uint32_t (*GetLocalIdFn)(void*);

struct FactionChoice { const char* faction; const char* party; };

// Playable warlords, all verified present in db/factions_tables + db/political_parties_tables.
// Cao Cao and Liu Bei are deliberately LAST: they are the usual picks for players 1 and 2, and a
// duplicate faction is itself a blocking condition (ready-mask bit 28).
static const FactionChoice kFactionChoices[] = {
    { "3k_main_faction_sun_jian",    "3k_main_political_party_sun_jian_ruler"    },
    { "3k_main_faction_yuan_shao",   "3k_main_political_party_yuan_shao_ruler"   },
    { "3k_main_faction_gongsun_zan", "3k_main_political_party_gongsun_zan_ruler" },
    { "3k_main_faction_kong_rong",   "3k_main_political_party_kong_rong_ruler"   },
    { "3k_main_faction_ma_teng",     "3k_main_political_party_ma_teng_ruler"     },
    { "3k_main_faction_liu_biao",    "3k_main_political_party_liu_biao_ruler"    },
    { "3k_main_faction_tao_qian",    "3k_main_political_party_tao_qian_ruler"    },
    { "3k_main_faction_yuan_shu",    "3k_main_political_party_yuan_shu_ruler"    },
    { "3k_main_faction_zheng_jiang", "3k_main_political_party_zheng_jiang_ruler" },
    { "3k_main_faction_zhang_yan",   "3k_main_political_party_zhang_yan_ruler"   },
    { "3k_main_faction_dong_zhuo",   "3k_main_political_party_dong_zhuo_ruler"   },
    { "3k_main_faction_cao_cao",     "3k_main_political_party_cao_cao_ruler"     },
    { "3k_main_faction_liu_bei",     "3k_main_political_party_liu_bei_ruler"     },
};
static constexpr long kFactionCount = (long)(sizeof(kFactionChoices) / sizeof(kFactionChoices[0]));

static volatile long g_factionRequest = -1;   // queued index; -1 = nothing pending
static volatile long g_factionNext    = 0;    // which choice the next keypress will use

// The local player id, cached by whoever last resolved it ON THE GAME THREAD. -1 = never resolved.
//
// ⚠ It is cached rather than resolved on demand because resolving means **calling** an engine
// function through a vtable, and `F8` arrives on the hotkey thread. Every engine call in this file
// is deliberately deferred to `drainFactionRequest` for that reason; the seat must not be the one
// exception. The lobby tick refreshes this about once a second, so it is populated within a second
// of a lobby existing — long before anyone can press anything.
static volatile long g_localSeat = -1;

// Resolve the lobby/session player service through the global chain. Returns null unless every
// link is present, so this is safe to call at the main menu.
static void* resolvePlayerService()
{
    uintptr_t root = 0, a = 0, b = 0;
    if (!readAt(g_base + RVA_FRONTEND_ROOT2, root) || !root) return nullptr;
    if (!readAt(root + 0x4E8, a) || !a)                      return nullptr;
    if (!readAt(a + 0x98, b) || !b)                          return nullptr;

    const uintptr_t self = b + 0x18;          // the sub-object IS `this`
    uintptr_t vtbl = 0, fn = 0;
    if (!readAt(self, vtbl) || vtbl <= 0x10000)              return nullptr;
    if (!readAt(vtbl + 0x208, fn) || fn <= 0x10000)          return nullptr;

    __try {
        return ((GetServiceFn)fn)((void*)self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void drainFactionRequest()
{
    const long idx = InterlockedExchange(&g_factionRequest, -1);
    if (idx < 0 || idx >= kFactionCount) return;

    const FactionChoice& c = kFactionChoices[idx];

    void* service = resolvePlayerService();
    if (!service) {
        diagLogf("SET FACTION: player service not available yet — open the MP campaign lobby first");
        return;
    }

    uintptr_t svtbl = 0, fnLocalId = 0;
    if (!readAt((uintptr_t)service, svtbl) || svtbl <= 0x10000 ||
        !readAt(svtbl + 0x20, fnLocalId) || fnLocalId <= 0x10000) {
        logf("SET FACTION: service vtable unreadable — aborting");
        return;
    }

    __try {
        const uint32_t localId = ((GetLocalIdFn)fnLocalId)(service);
        if (localId == 0xFFFFFFFF) {
            diagLogf("SET FACTION: local player id is -1 (not in a lobby yet) — aborting");
            return;
        }
        InterlockedExchange(&g_localSeat, (long)localId);   // game thread: safe to publish

        // CA strings are 16 bytes; give them room and zero them first.
        alignas(16) uint8_t sFaction[48] = { 0 };
        alignas(16) uint8_t sParty[48]   = { 0 };
        auto makeStr = (MakeCaStringFn)(g_base + RVA_MAKE_CASTRING);
        makeStr(sFaction, c.faction);
        makeStr(sParty,   c.party);

        auto setFaction = (SetFactionFn)(g_base + RVA_SET_FACTION);
        const bool sent = setFaction(service, (uint8_t)localId, sParty, sFaction, 0);

        diagLogf(">>> SET FACTION: localPlayerId=%u faction=\"%s\" party=\"%s\" -> send %s",
             localId, c.faction, c.party, sent ? "OK" : "FAILED");
        diagLogf("    send status is transport only; receiver still checks ownership, lock and duplicates, then acknowledges accepted/previous keys");
        diagLogf("    (watch for `slot-changed: slot=%u` on the OTHER machines — that is the lobby "
             "learning this player exists)", localId);

        // The receiver gates on the target slot's flags before doing anything:
        //     if ((flags & 0x40) && (flags & 0x480) == 0)      // exists, not vacant, not spectator
        // so print them here. If bit 7 or bit 10 were set, the message would be dropped silently on
        // arrival and no amount of resending would help.
        uint64_t slotObj = 0;
        const uintptr_t mp = capturedMp();
        if (mp && readAt(mp + OFF_MP_SLOTOBJ, slotObj) && slotObj) {
            const uintptr_t slot = (uintptr_t)slotObj + (uintptr_t)localId * SLOT_ENTRY_STRIDE;
            uint32_t flags = 0; uint32_t curFaction = 0;
            if (readAt(slot + SLOT_ENTRY_FLAGS, flags) && readAt(slot + 0x60, curFaction)) {
                const bool passes = (flags & 0x40) && ((flags & 0x480) == 0);
                diagLogf("    local slot[%u] flags=%08X factionIdx=%d -> receiver guard %s",
                     localId, flags, (int)curFaction,
                     passes ? "PASSES" : "WOULD REJECT (bit7 vacant or bit10 spectator set!)");
            }
        }
        // 1.7.2 service +20 (14046E960) maps the local network player through
        // slots+137C to this actual slot id. It is not a record vector position.
        // The two CA strings are intentionally not freed: a couple of small leaks per keypress is
        // preferable to replicating the engine's conditional-free dance in a probe build.
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("SET FACTION: FAULTED while sending — nothing applied");
    }
}

// Pick a faction NOBODY ELSE HOLDS.
//
// The receiver applies the faction and then checks `slot->currentFaction == requested` before
// committing — a faction already held by another player is refused there. The first build let every
// machine start at the same entry in the list, so when F8 was pressed on two machines they both
// asked for sun_jian and the second was silently rejected. Reading the taken set first removes that
// whole failure mode, and it is cheap: each lobby player record carries its faction key at +0x18,
// which is exactly what the run-6 probe already prints.
//
// `skip` lets a repeated keypress advance to the next unused faction, in case one is refused for a
// reason we cannot see from here.
//
// ---- ★★ SEAT-ANCHORED SINCE 2026-08-05, and the reason is a hole in the paragraph above ---------
//
// The filter is only as good as the record vector it reads, and that vector is **intermittent**: on
// 2026-08-04 at 12:43 it reached 3 on every machine, and at 14:15 — with a third player seated — the
// host's stayed at `count=2`. When it cannot be read this function says so ("list not visible yet")
// and falls through to the unfiltered list, where **every machine picks entry 0**. That is exactly
// the two-machines-both-ask-for-sun_jian collision the filter was written to remove, returning
// whenever the lobby is slow rather than whenever the code is wrong.
//
// So the scan now STARTS AT THE SEAT and wraps: seat 2 begins at `gongsun_zan`, seat 3 at
// `kong_rong`. Two factionless players no longer collide even when neither can see the other, and
// the filter goes back to being an optimisation rather than the only thing standing between the
// lobby and a silent rejection.
//
// ⚠ It does not disturb the "Cao Cao and Liu Bei last" rule above. They sit at indices 11 and 12,
// and for any seat 0..3 the rotated scan reaches them only after **eight** factions are taken, which
// a four-player lobby cannot do.
//
// ⚠ `seat` here is the local player id, which B4 established is reassigned and non-compact across a
// save-load. That is fine and is in fact what this wants: the property being relied on is that two
// players have DIFFERENT ids, not that the ids are 0..3 or contiguous. The modulo makes any id safe.
static long pickUnusedFaction(long seat, long skip)
{
    char taken[16][64] = { { 0 } };
    int  nTaken = 0;

    // Same staleness trap as the F3 capture: after a campaign starts this pointer is freed memory,
    // and reading player records out of it would hand back garbage faction keys. Fall through to the
    // unfiltered list rather than trusting them (F8 is a lobby-only action anyway).
    const uintptr_t lobby = lobbyLooksLive(g_liveLobby) ? g_liveLobby : 0;
    uint32_t count = 0; uint64_t recs = 0;
    if (lobby && readAt(lobby + OFF_PLAYER_COUNT, count) &&
        readAt(lobby + OFF_PLAYER_RECORDS, recs) && recs && count <= 16) {
        for (uint32_t i = 0; i < count; ++i) {
            const uintptr_t rec = (uintptr_t)recs + (uintptr_t)i * 0x48;
            uint32_t len = 0; uint64_t data = 0;
            if (!readAt(rec + 0x18, len) || !readAt(rec + 0x20, data)) continue;
            if (!len || len > 62 || !data || data == EMPTY_STR_SENTINEL)  continue;
            uint8_t raw[64] = { 0 };
            if (!safeRead((void*)data, raw, len)) continue;
            memcpy(taken[nTaken], raw, len);
            taken[nTaken][len] = '\0';
            ++nTaken;
        }
    }

    const long start = ((seat % kFactionCount) + kFactionCount) % kFactionCount;   // negatives too

    long seen = 0;
    for (long n = 0; n < kFactionCount; ++n) {
        const long i = (start + n) % kFactionCount;
        bool used = false;
        for (int t = 0; t < nTaken; ++t)
            if (strcmp(taken[t], kFactionChoices[i].faction) == 0) { used = true; break; }
        if (used) continue;
        if (seen++ == skip) {
            diagLogf("SET FACTION: %d faction(s) already taken in this lobby%s", nTaken,
                 nTaken ? ":" : " (list not visible yet — the seat anchor is what stops a collision)");
            for (int t = 0; t < nTaken; ++t) diagLogf("      taken: %s", taken[t]);
            diagLogf("      seat %ld anchors the scan at \"%s\"; skip=%ld -> \"%s\"",
                 seat, kFactionChoices[start].faction, skip, kFactionChoices[i].faction);
            return i;
        }
    }
    return start;   // everything taken (or nothing readable) — this seat's own first entry
}

// `advance` is the difference between a person and a retry, and it is the whole of this fix.
//
// ✗ **What it was doing wrong (found 2026-08-05):** every call advanced `g_factionNext`, including
// the automatic ones. Auto-faction retries up to four times, a second apart, so a player whose
// seating needed three goes silently walked three entries down the list — which is how one client ended
// on Yuan Shao after three attempts at `sun_jian`, and nobody chose that.
//
// ★ And the retries are not evidence of a refusal. Auto-faction fires again because `slot->faction`
// is *still* -1 a second later, and the ordinary reason for that is that the message has not landed
// yet. Answering "not yet" with "then ask for a different faction" is the wrong move: it turns a
// slow lobby into a changed one.
//
//   F8 (a person, who watched it not work)  -> advance: try the next unused faction
//   auto-faction (a timer, a second later)  -> do not: ask for the SAME one again
void queueFactionRequest(bool advance)
{
    const long skip = advance ? InterlockedExchange(&g_factionNext, g_factionNext + 1)
                              : g_factionNext;

    const long seat = g_localSeat;
    if (seat < 0)
        diagLogf("SET FACTION: the local seat is not resolved yet, so the scan starts at entry 0 — "
             "which is the one case where two machines can still collide. It resolves within a "
             "second of the lobby ticking; if this line appears, the press was that early.");

    const long idx = pickUnusedFaction(seat < 0 ? 0 : seat, skip);
    InterlockedExchange(&g_factionRequest, idx);
    diagLogf("SET FACTION queued: \"%s\" — applied on the next game-thread hook.%s",
         kFactionChoices[idx].faction,
         advance ? " Press again to try the next unused faction."
                 : " (a retry of the same request, not a new choice)");
}

#ifndef TW3K_RELEASE
// Debug pipe `faction set <key>`: ask for one specific faction from the list, so a test can build a
// lobby with a known turn order (#56 needs a chosen faction 3rd). Accepts the full key or the part
// after `3k_main_faction_`. Same request slot as F8; the engine still refuses a duplicate.
bool queueFactionByKey(const char* key)
{
    for (long i = 0; i < kFactionCount; ++i) {
        const char* f = kFactionChoices[i].faction;
        const char* tail = f + sizeof("3k_main_faction_") - 1;
        if (_stricmp(f, key) == 0 || _stricmp(tail, key) == 0) {
            InterlockedExchange(&g_factionRequest, i);
            diagLogf("SET FACTION queued: \"%s\" (asked for by key) — applied on the next game-thread hook.", f);
            return true;
        }
    }
    return false;
}
#endif

// ---- ★★★ AUTO-ASSIGN A FACTION TO A LOCAL PLAYER IN SLOT >= 2 (session 6k) ---------------------
//
// ❌ 6h/6i claimed "F8 is retired". **That was wrong**, and the user caught it: on a fresh host F8
// was still needed for a joining player, and the run that seemed to prove otherwise had re-hosted
// from a previous session, so that player already had a faction.
//
// The dropdown re-enable of 6h is real, but it only ever fires when a faction is APPLIED — so it
// fixes *changing* a faction and leaves the original circular dependency exactly where it was:
//
//     no faction -> the lobby never learns this player exists (it learns from the 0xCD message)
//               -> the panel holds no CcoFrontendFactionLeader context
//                 -> ContextVisibilitySetter keeps the panel hidden
//                   -> the dropdown that would set a faction is INSIDE that hidden panel
//
// Enabling a dropdown inside a hidden panel achieves nothing. Two separate gates, and 6h only opened
// the second one.
//
// This is the fix HANDOVER has described as "designed, not built" since session 5z: when the LOCAL
// player is seated in slot >= 2 and holds no faction, assign an unused one automatically — precisely
// what F8 does by hand. The panel then gains a real context, becomes visible on its own, and the
// re-enabled dropdown lets the player change it to whatever they actually wanted.
//
// ⚠ It must stay an ASSIGNMENT, not a choice imposed for good: the point is to break the loop, and
// the player picks properly a second later from a panel that now exists.
static volatile long      g_autoFactionTries   = 0;
static volatile long long g_autoFactionNextTry = 0;

void resetAutoFactionBudget()
{
    InterlockedExchange(&g_autoFactionTries, 0);
    InterlockedExchange64(&g_autoFactionNextTry, 0);
    InterlockedExchange(&g_factionNext, 0);
    InterlockedExchange(&g_factionRequest, -1);
    InterlockedExchange(&g_localSeat, -1);
}
static constexpr long     kAutoFactionMaxTries = 4;

void maybeAutoAssignFaction()
{
    if (g_factionRequest >= 0) return;                        // one already queued, let it drain

    // Rate-limit the WHOLE check, not just the attempt. The "gave up" test used to sit here, above
    // everything, so once it tripped the reset below could never be reached and the fix stayed dead
    // for the rest of the process. It now lives after the faction read instead.
    const long long now = (long long)GetTickCount64();
    if (now < g_autoFactionNextTry) return;
    g_autoFactionNextTry = now + 1000;                        // re-examine about once a second

    const uintptr_t mp = capturedMp();
    if (!mp) return;
    uint64_t slotObj = 0;
    if (!readAt(mp + OFF_MP_SLOTOBJ, slotObj) || !slotObj) return;

    void* service = resolvePlayerService();
    if (!service) return;                                     // not in the lobby yet
    uintptr_t svtbl = 0, fnLocalId = 0;
    if (!readAt((uintptr_t)service, svtbl) || svtbl <= 0x10000)   return;
    if (!readAt(svtbl + 0x20, fnLocalId) || fnLocalId <= 0x10000) return;

    uint32_t localId = 0xFFFFFFFF;
    __try { localId = ((GetLocalIdFn)fnLocalId)(service); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    // Publish the seat BEFORE the early returns below. This runs on the game thread about once a
    // second for EVERY seat, including the ones auto-faction leaves alone, and it is the only place
    // that reliably knows the id — so a player in slot 0 or 1 who presses F8 still gets a seat
    // anchor rather than falling back to entry 0 with everybody else.
    if (localId != 0xFFFFFFFF) InterlockedExchange(&g_localSeat, (long)localId);

    // Slots 0 and 1 are the stock path: their panels exist from the start, so they are never caught
    // by the loop and must be left entirely alone.
    if (localId == 0xFFFFFFFF || localId < 2 || localId > 3) return;

    const uintptr_t slot = (uintptr_t)slotObj + (uintptr_t)localId * SLOT_ENTRY_STRIDE;
    uint32_t flags = 0, curFaction = 0;
    if (!readAt(slot + SLOT_ENTRY_FLAGS, flags))  return;
    if (!readAt(slot + 0x60, curFaction))         return;

    // ★ Already has one — nothing to break out of.
    //
    // ❌ This used to latch the counter at MAX, which made the whole thing a **once per game
    // process** affair. User hit it immediately: they re-hosted within the same session, the joining
    // player was seated fresh with no faction, and auto-faction never fired again because it had
    // already succeeded in the *previous* lobby. Rehosting, returning to the menu and hosting again,
    // and a failed lobby you back out of are all ordinary things to do, and every one of them left
    // the fix disarmed.
    //
    // So reset instead of latching: a player who holds a faction needs no help, and if they are ever
    // factionless again — which is exactly what a new lobby looks like — they get a full set of
    // attempts. `g_factionNext` goes back too, or the next lobby's first pick would resume from
    // wherever the last one left off and could ask for a faction another player already holds.
    if (curFaction != 0xFFFFFFFF) {
        if (g_autoFactionTries != 0) {
            InterlockedExchange(&g_autoFactionTries, 0);
            InterlockedExchange(&g_factionNext, 0);
        }
        return;
    }

    // Exhausted for THIS factionless spell. Deliberately checked here, below the reset above, so a
    // new lobby always starts with a fresh allowance.
    if (g_autoFactionTries >= kAutoFactionMaxTries) return;   // F8 still available

    // The receiver drops the 0xCD message unless the slot exists and is neither vacant nor
    // spectating, so there is no point sending into a slot that would refuse it.
    if (!(flags & 0x40) || (flags & 0x480)) return;

    const long n = InterlockedIncrement(&g_autoFactionTries);
    g_autoFactionNextTry = now + 4000;
    diagLogf("AUTO-FACTION (try %ld/%d): this machine is player %u with no faction, so the lobby cannot "
         TW3K_MODE_TEXT("see it and its panel stays hidden. Assigning an unused faction — this is exactly what F8 ", "see it and its panel stays hidden. Assigning an unused faction ")
         TW3K_MODE_TEXT("did by hand. Change it afterwards with your own dropdown.", "automatically. Change it afterwards with your own dropdown."), n, kAutoFactionMaxTries,
         localId);
    queueFactionRequest(false);         // a retry asks for the SAME faction, not the next one
    if (n == kAutoFactionMaxTries)
        diagLogf(TW3K_MODE_TEXT("AUTO-FACTION: that was the last automatic attempt. If no panel appeared, press F8.", "AUTO-FACTION: that was the last automatic attempt. If no panel appeared, use your faction dropdown."));
}

// ---- lobby tick: the ONLY reliable place to drain queued work ---------------------------------
//
// The first F8 attempt queued correctly and then nothing happened: the request was drained from the
// MP-advertise hook, and that hook turns out to fire ONCE at session setup, not periodically. In an
// idle lobby no hook fires at all, so the queued request simply sat there — the log ended on the
// "queued" line.
//
// FUN_142CE4A80 is the lobby's per-frame tick (it is the sole caller of the refresh, which it gates
// behind a dirty flag at +0x91 — the classic shape of something called every frame). Hooking it gives
// a dependable game-thread heartbeat for draining queued work.
//
// Deliberately silent: this runs every frame, and logf reopens the log file on each call.


typedef void (*LobbyTickFn)(void*);
Detour       g_tickDetour;
static LobbyTickFn  g_origLobbyTick = nullptr;
static volatile long g_tickCalls    = 0;

static void lobbyTickHook(void* self)
{
#ifndef TW3K_RELEASE
    static volatile long announced = 0;
    if (InterlockedCompareExchange(&announced, 1, 0) == 0)
        logf("lobby tick: FIRST CALL (drain heartbeat is live)");

#endif
    TW3K_DIAGNOSTIC(InterlockedIncrement(&g_tickCalls));
#ifndef TW3K_RELEASE
    captureSaveLobbyWatch((uintptr_t)self);
#endif
    tickSaveLobby((uintptr_t)self);
    tickFreshLobbyFactions((uintptr_t)self);
    tickExtraLobbyPanels((uintptr_t)self);
    maybeAutoAssignFaction();           // break the no-faction/no-panel loop without a keypress
    drainFactionRequest();              // game thread, every frame — nothing can be left stranded

    if (g_origLobbyTick) g_origLobbyTick(self);
}

// ⚠ `PUSH RBX` here is the REX-prefixed 2-byte form `40 53`, not the bare `53`. Ghidra's disassembly
// listing shows only the mnemonic, so the first version of this table guessed `53` and the hook
// refused to install ("MISMATCH at byte 0 (want 53 got 40)") — costing a whole 3-machine run, during
// which the queued work was drained only by luck when the sporadic MP-advertise hook happened to
// fire. LESSON: always confirm expected prologue bytes with a byte search before trusting them; a
// mnemonic does not determine its encoding.

bool installTickHook()
{
    return detourInstall(g_tickDetour, g_base + RVA_LOBBY_TICK, TICK_STOLEN_LEN,
                         EXPECT_LOBBY_TICK, (uintptr_t)&lobbyTickHook,
                         (void**)&g_origLobbyTick, "lobby tick");
}

void removeTickHook() { detourRemove(g_tickDetour, "lobby tick"); }

// ------------------------------------------- STAGE 1: slot-cache overflow guard
//
// The lobby refresh FUN_142D57630 copies one player-name string per player into the INLINE slot
// cache at lobby+0xE8, which has exactly TWO 0x10-byte entries. Its loop bound is the live player
// count at +0xCC, so a third player makes it write over +0x108 onward — straight into the UI
// component pointers (+0x110 save_game_map, +0x118 campaign_selection_map, +0x120
// template_icon_army, +0x128 button_ready, +0x130 button_kick, +0x138 button_invite).
// That is a guaranteed crash, and it is exactly why raising the invite gate alone is unsafe.
//
// Guard: clamp +0xCC to the vanilla capacity for the duration of the original call, then restore
// it. The original then fills slots 0..1 as it always has and never runs off the end. Players 3+
// simply do not appear yet — displaying them is stage 2, and needs the extra panel lookups plus
// our own slot storage.

static constexpr uint32_t  VANILLA_SLOT_CACHE = 2;          // entries at lobby+0xE8

typedef void (*LobbyRefreshFn)(void*);
Detour            g_lobbyDetour;
static LobbyRefreshFn    g_origLobbyRefresh = nullptr;
static volatile uint32_t g_guardHits        = 0;
static volatile uint32_t g_guardMaxSeen     = 0;

// g_liveLobby is declared earlier (the faction picker needs it); the hooks below populate it.

// ---- player-vector watch --------------------------------------------------------------------
//
// The lobby's player list is a dynamic array {cap +0xC8, count +0xCC, ptr +0xD0} of 0x48-byte
// records. Run 1 read cap=4/count=3; run 2 read cap=2/count=2 — player 3 was seated in the SESSION
// but never added to the LOBBY, which is why it could not ready up.
//
// Rather than keep hunting statically for whatever pushes into this vector (the byte searches keep
// landing outside the lobby's address range), watch it: the refresh runs constantly, so logging the
// vector whenever it CHANGES shows exactly when a player is added — or added and then dropped again.
// Change-triggered so it costs nothing on the common path. Read-only.
static uint64_t g_lastVecState = ~0ull;

// ---- player display names, captured while the lobby still exists --------------------------------
//
// ★ WHERE THE NAME IS (2026-08-03). `FUN_142D4FB00(cb, groupData, playerId, name)` fills the panel's
// `dy_player_name` label, and its 4th parameter goes straight into `thunk_FUN_14689D340(wordList,
// out, in)` — which is a PROFANITY FILTER (it copies the string, then overwrites any listed word
// with '*'). So the 4th parameter is not a record to look a name up in: it IS the name, as a CA
// string. The DLL has been passing the lobby player record's own base address there since run 7 and
// the right name has been drawing on player 3's panel ever since ⇒ the name is the CA string at
// **record + 0x00**, and that is confirmed on screen rather than reasoned.
//
// Record layout, as far as it is known: +0x00 name (CaString) · +0x10 id · +0x18 faction (CaString).
//
// ⚠ NOT the `lobby+0xE8` slot cache, which would have been the obvious guess: it has exactly two
// entries and our own overflow guard clamps the refresh to filling those two, so it never holds a
// name for players 3 and 4 — the ones the gift panel most needs to name.
static char          g_playerNames[MAX_NAMED_PLAYERS][NAME_MAX] = { { 0 } };
static volatile long g_namesCaptured = 0;

// ---- BISECT INSTRUMENTATION (2026-08-04) --------------------------------------------------------
//
// ★★★ ATTRIBUTION WITHOUT RE-BREAKING THE RIG. The hang on campaign load is now known to be one of
// two backed-out changes, and the prime suspect is the name-move clear that used to live below: it
// made the next pass re-capture a record, which re-runs `RVA_CASTRING_COPY`, and that call **leaks**
// the previous copy. Two records carrying the same name at different ids would clear each other
// every pass — one allocation and one `logf` per lobby refresh, at frame rate.
//
// ⇒ Rather than reinstate a suspected bug to see it misbehave, COUNT WHAT IT WOULD HAVE DONE. These
// three numbers say whether that mechanism was ever reachable, and how hot:
//
//   captures   — how many times a record's name was written (each one is a CA-string copy, i.e. a
//                leak under the old code)
//   wouldClear — how many times a name was found at a DIFFERENT id, which is exactly when the
//                removed code would have fired
//   calls      — how many times the whole function ran, so the others have a denominator
//
// Reading it: `wouldClear` in the thousands after a normal load confirms the mechanism and measures
// it. `wouldClear = 0` exonerates it outright, and the cause is the other revert — which would be
// the more surprising answer, and worth knowing before `readTurnNumber` goes back in.
static volatile long g_nameCaptureCalls = 0;
static volatile long g_nameWouldClear   = 0;

// ★★ And a copy of the engine's own WIDE string, because that is what the UI wants back.
//
// A string-returning CCO query hands its value to `out->vtable[0x80]`, and FUN_142D59FA0 — the
// lobby's ReadyStatusText, the only worked example in the binary — shows exactly what type that is:
// it builds a NARROW CA string from the literal, uses it as a **loc key**, and hands over the WIDE
// result of the lookup. So vtable[0x80] takes UTF-16.
//
// ⚠ Building a wide CA string from a narrow literal is not something to guess at: the struct is
// 0x10 bytes with small-string optimisation, an inline marker in the top nibble of +0x08 and the
// length packed into its top byte, and the engine will later FREE whatever it is handed. A
// hand-built one that gets any of that wrong corrupts the game's heap rather than merely rendering
// badly.
//
// ⇒ Do not build one. **Copy the one the engine already made**, with the engine's own wide
// copy-constructor, while the record still exists. FUN_140663050(dest, src) is what both
// FUN_142D59FA0 and the profanity filter use, and it allocates its own buffer — which is why their
// callers free the source separately — so the copy outlives the lobby that is freed at campaign
// start. That is the whole trick, and it involves no assumption about the layout at all.
static constexpr uintptr_t RVA_CASTRING_COPY = 0x00663050;   // FUN_140663050(dest, src), wide

alignas(8) static uint8_t g_playerNameCa[MAX_NAMED_PLAYERS][16] = { { 0 } };
static bool               g_playerNameCaOk[MAX_NAMED_PLAYERS]   = { false };

// ---- #11: the capture had to move off the engine's clock, and that splits this in two ----------
//
// ★★★ THE MEASURED DEFECT (2026-08-10, six lobbies, three machines): name capture is a **single
// snapshot of a vector that is still filling**, and every machine learns a DIFFERENT SUBSET —
// player 0 captured by two machines and not the third, player 1 by one only, and so on. It is not
// "one unlucky player": each client happens to look at a different moment, and never looks again,
// because the lobby is freed at campaign start.
//
// ⇒ The same sentence `watchLobbyPlayers` was written for, one field along: **a watch wired to the
// wrong clock.** That function fixed it for the ID SET (2 Hz, probe thread) and the fix was never
// carried across to the NAMES, which are the half a player actually sees.
//
// ⚠⚠ BUT THE PROBE THREAD MAY NOT ALLOCATE. `RVA_CASTRING_COPY` calls the game's allocator, and
// this project's standing rule — paid for by the F1 slot-expansion race — is that engine calls
// which touch shared state happen on the GAME thread or not at all.
//
// ⇒ So the capture splits by what each half needs:
//
//   narrow half  reads the record and stores UTF-8 into g_playerNames[].     Any thread.
//                Leaf accessors only (str-len / str-data), SEH-guarded, no allocation.
//   wide half    builds the CA string the UI is handed.                      GAME THREAD ONLY.
//                Deferred via g_nameCaPending[] and drained in knownPlayerNameCa(), which is
//                the single place the wide string is ever consumed and is on the game thread.
//
// ★ Deferring is only possible because the deferred build does NOT read the lobby record — it
// rebuilds from our own UTF-8 copy. `readCaStringAuto` converts with WideCharToMultiByte(CP_UTF8),
// so MultiByteToWideChar(CP_UTF8) recovers the original UTF-16 exactly; the round trip is lossless
// for anything the engine can hold. That is what frees the wide build from the lobby's lifetime.
//
// ⚠ RISK POSTURE, deliberately conservative: when we are already on the game thread with a live
// record, the ORIGINAL path still runs — the engine's own copy-constructor, byte-identical to
// today. The rebuild path can therefore only ever execute where today there is **no name at all**,
// so it cannot regress a case that currently works.
static volatile long g_nameCaPending[MAX_NAMED_PLAYERS] = { 0 };
static volatile long g_namesLateBuilt = 0;    // wide strings built off the deferred path
static volatile long g_namesProbeSeen = 0;    // narrow names first seen by the 2 Hz watch

// Game thread only. Builds the wide CA string from our own UTF-8 copy when the capture that learned
// the name could not build one itself. Returns true if a usable wide string now exists.
static bool materialisePlayerNameCa(int id)
{
    if (id < 0 || id >= (int)MAX_NAMED_PLAYERS) return false;
    if (InterlockedCompareExchange(&g_nameCaPending[id], 0, 1) != 1)
        return g_playerNameCaOk[id];          // nothing pending — answer with what we have

    wchar_t wide[NAME_MAX] = { 0 };
    const int n = MultiByteToWideChar(CP_UTF8, 0, g_playerNames[id], -1, wide, NAME_MAX);
    if (n <= 1) return g_playerNameCaOk[id];  // empty or unconvertible — keep any earlier copy

    __try {
        // ⚠ Zero first, exactly as the engine's own constructor does: it writes +0x08 assuming it
        // owns the struct. A rename leaks the previous copy — bounded, because we only get here
        // when the narrow name actually CHANGED, which is once per player per lobby.
        memset(g_playerNameCa[id], 0, sizeof(g_playerNameCa[id]));
        ((MakeCaStringWFn)(g_base + RVA_MAKE_CASTRING_W))(g_playerNameCa[id], wide);
        g_playerNameCaOk[id] = true;
        TW3K_DIAGNOSTIC(InterlockedIncrement(&g_namesLateBuilt));
        diagLogf("PLAYER NAME (late build): player %d is \"%s\" — the narrow name was captured by the "
             "2 Hz watch and the wide CA string built here, on the game thread. This is the #11 "
             "path: without it this player's button would read \"Player %d\".",
             id, g_playerNames[id], id + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_playerNameCaOk[id] = false;
        logf("PLAYER NAME (late build): building the wide CA string for player %d FAULTED — the "
             "button falls back to \"Player %d\".", id, id + 1);
    }
    return g_playerNameCaOk[id];
}

void captureLobbyPlayerNames(uintptr_t lobby, bool onGameThread)
{
    if (!lobby) return;
    TW3K_DIAGNOSTIC(InterlockedIncrement(&g_nameCaptureCalls));

    uint32_t count = 0; uint64_t recs = 0;
    if (!readAt(lobby + OFF_PLAYER_COUNT, count) || !readAt(lobby + OFF_PLAYER_RECORDS, recs))
        return;
    if (!recs || count > 64) return;

    // ⚠ §6iii: in a three-player session NO machine ever captured player 2's name, every log read
    // `count=2` and never 3 — and the lobby drew that player's name on their panel regardless. So
    // either this vector does grow and we never sampled it at the right moment, or the engine has a
    // route to the name that is not this vector. The guard's own `highest player count seen` would
    // have told us which, and it is only printed on F3, which nobody pressed.
    //
    // ⇒ Say it here instead, once per new high-water mark, so the next lobby answers it by itself
    // rather than costing another session.
#ifndef TW3K_RELEASE
    {
        static volatile long seenHigh = 0;
        if ((long)count > seenHigh) {
            seenHigh = (long)count;
            if (count > 2)
                logf("★ LOBBY RECORD VECTOR reached count=%u — so it DOES grow past two, and a "
                     "missing name is a sampling problem rather than a missing route (§6iii).",
                     count);
        }
    }

#endif
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t rec = (uintptr_t)recs + (uintptr_t)i * 0x48;
        uint32_t id = 0xFFFFFFFF;
        if (!readAt(rec + 0x10, id)) continue;

        // ★★★ B4 DETECTOR (2026-08-04). Player ids are NOT stable across a save-load, and they are
        // NOT compact. Measured, all three machines agreeing: a 3-player lobby that was ids {0,1,2}
        // came back from a reload as ids **{0,1,3}** — the machine that was player 1 became player 3.
        //
        // ⇒ Everything this mod builds assumes ids 0..3: four panels, and `maybeAutoAssignFaction`
        // returns early on `localId > 3`. An id past 3 therefore has no panel and no auto-faction,
        // which is exactly B4's report — *a reload lobby loses players 3 and 4*. With four players a
        // reassignment only has to push one id to 4 for that player to vanish.
        //
        // ⚠ NOT yet shown to be B4's cause: the measured session had three players, ids stayed
        // inside 0..3, and nobody was lost. This says the premise is real and logs the moment the
        // dangerous case appears, rather than waiting for another session to end in a shrug.
        // ⚠⚠ THE BOUNDS GUARD IS UNCONDITIONAL, AND MUST STAY THAT WAY.
        //
        // ✗ The first version of this detector read `id != 0xFFFFFFFF && id >= MAX_NAMED_PLAYERS`,
        // so that the sentinel would not trigger the B4 shout. That let `0xFFFFFFFF` fall THROUGH
        // the guard into `g_playerNames[id]` and `memset(g_playerNameCa[id], ...)` — an out-of-bounds
        // write at a wild offset, in a function that runs on every lobby refresh. `0xFFFFFFFF` is
        // this codebase's empty-slot value (`curFaction != 0xFFFFFFFF`, `localId == 0xFFFFFFFF`), so
        // it is not a rare input. The version before the detector was safe:
        // `if (!readAt(...) || id >= MAX_NAMED_PLAYERS) continue;`
        //
        // ⇒ Bound first, then decide whether it is worth shouting about. Never the reverse.
        if (id >= MAX_NAMED_PLAYERS) {
            if (id != 0xFFFFFFFF) {            // a real id that is simply too large — the B4 case
                static volatile long shouted = 0;
                if (InterlockedCompareExchange(&shouted, 1, 0) == 0)
                    logf("★★★ LOBBY PLAYER id=%u IS PAST THE %u THIS MOD SUPPORTS. Panels are built "
                         "for ids 0..3 and auto-faction ignores anything above 3, so this player has "
                         "no panel and cannot be given one. This is the shape of B4.",
                         id, (unsigned)MAX_NAMED_PLAYERS - 1);
            }
            continue;                          // ← the guard, taken for the sentinel too
        }
        if (id > 3) {
            static volatile long shoutedHigh = 0;
            if (InterlockedCompareExchange(&shoutedHigh, 1, 0) == 0)
                diagLogf("★★ LOBBY PLAYER id=%u is above 3 — ids are not compact here. The four panels "
                     "are indexed 0..3, so check this player has one.", id);
        }

        // ⚠ The name is UTF-16 while the faction key two fields along is narrow, so this needs the
        // encoding-detecting reader. `wide` is logged because it is the one thing here that no test
        // on this machine can check — if a name ever comes out as "T" or as mojibake, that flag says
        // which way the detection went.
        char name[NAME_MAX] = { 0 };
        bool wide = false;
        if (!readCaStringAuto((void*)rec, name, sizeof(name), &wide) || !name[0]) continue;
        if (strcmp(g_playerNames[id], name) == 0) continue;      // unchanged — say nothing

        // ✂✂ REVERTED 2026-08-04 — BISECT. A "a name may exist at only one id" pass lived here, to
        // fix the gift buttons naming the wrong player after a save-load (ids are reassigned; the
        // table is keyed by id and was never cleaned). It is **cosmetic**, and it is one of only two
        // changes since the last known-good build that run AUTOMATICALLY rather than behind a
        // hotkey — so it comes out first while a hang is being chased.
        //
        // ⚠ WHY IT IS THE PRIME SUSPECT, and worth writing down before it is reinstated: clearing an
        // entry means the NEXT pass no longer matches `g_playerNames[id] == name`, so that record is
        // re-captured — which re-runs `RVA_CASTRING_COPY` below. That call ALLOCATES, and the note
        // beside it already says a rename **leaks the previous copy** ("a handful of short strings
        // per session", which was true when renames were rare). If two records ever carry the same
        // name at different ids, the two clear each other every pass and the leak becomes one
        // allocation plus one `logf` **per lobby refresh, at frame rate** — which is the shape of a
        // slow strangle ending in a black screen, not of a crash.
        //
        // ⇒ Reinstating it needs the leak fixed first, or a guard that cannot ping-pong.
        // Tracked on the backlog.
        //
        // Count what it WOULD have done, without doing it — see the note beside the counters.
#ifndef TW3K_RELEASE
        for (uint32_t o = 0; o < MAX_NAMED_PLAYERS; ++o)
            if (o != id && g_playerNames[o][0] && strcmp(g_playerNames[o], name) == 0)
                TW3K_DIAGNOSTIC(InterlockedIncrement(&g_nameWouldClear));

#endif
        strncpy(g_playerNames[id], name, NAME_MAX - 1);
        g_playerNames[id][NAME_MAX - 1] = '\0';
        TW3K_DIAGNOSTIC(InterlockedIncrement(&g_namesCaptured));

        // The wide copy the UI will actually be handed.
        //
        // ⚠⚠ ON THE PROBE THREAD THIS MUST NOT HAPPEN HERE — the engine's copy-constructor
        // allocates. Mark it pending instead and let the game thread build it out of our own UTF-8
        // copy (see materialisePlayerNameCa). The narrow name above is what makes that possible,
        // and storing it is the part that had to move to the faster clock.
        bool copied = false;
        if (!onGameThread) {
            InterlockedExchange(&g_nameCaPending[id], 1);
            TW3K_DIAGNOSTIC(InterlockedIncrement(&g_namesProbeSeen));
        } else {
            // Game thread, record still live: the ORIGINAL path, unchanged. Zero first, exactly as
            // FUN_140662DF0 does to its own destination — the constructor writes +0x08 assuming it
            // owns the struct.
            // ⚠ A rename leaks the previous copy. That is a handful of short strings per session,
            // and the alternative is replicating a release rule whose sentinels differ between the
            // narrow and wide families.
            __try {
                memset(g_playerNameCa[id], 0, sizeof(g_playerNameCa[id]));
                ((void(*)(void*, void*))(g_base + RVA_CASTRING_COPY))(g_playerNameCa[id], (void*)rec);
                g_playerNameCaOk[id] = true;
                copied = true;
                InterlockedExchange(&g_nameCaPending[id], 0);   // nothing left to defer
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_playerNameCaOk[id] = false;
                InterlockedExchange(&g_nameCaPending[id], 1);   // let the UI path retry the rebuild
            }
        }

        diagLogf("PLAYER NAME captured: player %u is \"%s\" (%s, wide copy %s) — kept for the rest of "
             "this process, because the lobby that knows it is freed when the campaign starts.",
             id, g_playerNames[id], wide ? "UTF-16" : "narrow",
             copied ? "ok" : (onGameThread ? "FAILED" : "deferred to the game thread"));
    }
}

const char* knownPlayerName(int id)
{
    if (id < 0 || id >= (int)MAX_NAMED_PLAYERS) return "";
    return g_playerNames[id];
}

// ⚠⚠ GAME THREAD ONLY — it may build a CA string, which allocates. That is not a new constraint:
// this is the UI's accessor and its only callers are the gift panel's CCO query and the recipient
// logging beside it, both of which already run on the game thread.
//
// The drain lives here rather than in a tick because this is the single place the wide string is
// ever consumed, so a name captured by the 2 Hz watch is materialised exactly when it is first
// needed and never speculatively.
const void* knownPlayerNameCa(int id)
{
    if (id < 0 || id >= (int)MAX_NAMED_PLAYERS) return nullptr;
    if (g_nameCaPending[id]) materialisePlayerNameCa(id);
    return g_playerNameCaOk[id] ? (const void*)g_playerNameCa[id] : nullptr;
}

#ifndef TW3K_RELEASE
void reportPlayerVectorIfChanged(uintptr_t lobby)
{
    // Names first, and NOT behind the change gate below: that gate fires only when (cap, count)
    // move, and a player's name can land after the vector has stopped changing size.
    captureLobbyPlayerNames(lobby);

    if (!lobby) return;

    uint32_t cap = 0, count = 0; uint64_t recs = 0;
    if (!readAt(lobby + 0xC8, cap) || !readAt(lobby + OFF_PLAYER_COUNT, count) ||
        !readAt(lobby + OFF_PLAYER_RECORDS, recs))
        return;
    if (count > 64) return;                       // garbage — not a populated lobby yet

    const uint64_t state = ((uint64_t)cap << 32) | count;
    if (state == g_lastVecState) return;
    g_lastVecState = state;

    logf("LOBBY PLAYER VECTOR CHANGED -> cap(+0xC8)=%u count(+0xCC)=%u ptr(+0xD0)=%016llX",
         cap, count, (unsigned long long)recs);

    // One line per player: id plus faction key, so a player present-but-factionless is obvious.
    for (uint32_t i = 0; i < count && recs; ++i) {
        const uintptr_t rec = (uintptr_t)recs + (uintptr_t)i * 0x48;
        uint32_t id = 0, flen = 0; uint64_t fdata = 0;
        char faction[40] = { 0 };
        readAt(rec + 0x10, id);
        if (readAt(rec + 0x18, flen) && readAt(rec + 0x20, fdata) &&
            flen && flen < 39 && fdata && fdata != EMPTY_STR_SENTINEL) {
            uint8_t raw[40] = { 0 };
            if (safeRead((void*)fdata, raw, flen))
                for (uint32_t c = 0; c < flen; ++c)
                    faction[c] = (raw[c] >= 32 && raw[c] < 127) ? (char)raw[c] : '.';
        }
        logf("    lobby player[%u] id=%u faction=\"%s\"", i, id, faction[0] ? faction : "(none)");
    }
}
#endif

// ---- campaign ready-check watch ---------------------------------------------------------------
//
// "START CAMPAIGN does nothing" has a precise cause the game already computes for itself. The
// campaign lobby's ready validator lives at DAT_1443B7238 -> +0x4E8 -> +0x70 and exposes:
//
//     vtable[0x30]() -> char  is this player a spectator
//     vtable[0x10]() -> uint  BITMASK of blocking reasons   (<= 1 means nothing is blocking)
//
// FUN_142D3F1B0 turns that mask into the tooltip text, one reason per bit — which is where the bit
// meanings below come from. Most reasons resolve through a numeric localisation id rather than a
// named key, so only a few can be named from the static image; the rest we identify by watching
// which bit clears when we change something.
//
// Reading it live beats guessing: it says exactly why the campaign will not start.
static constexpr uintptr_t RVA_FRONTEND_ROOT = 0x043B7238;   // DAT_1443B7238
uint32_t g_lastReadyMask = 0xFFFFFFFF;

#ifndef TW3K_RELEASE
void reportReadyMaskIfChanged()
{
    uintptr_t root = 0, obj = 0, validator = 0;
    if (!readAt(g_base + RVA_FRONTEND_ROOT, root) || !root)            return;
    if (!readAt(root + 0x4E8, obj) || !obj)                            return;
    if (!readAt(obj + 0x70, validator) || validator <= 0x10000)        return;

    uintptr_t vtbl = 0;
    if (!readAt(validator, vtbl) || vtbl <= 0x10000)                   return;
    uintptr_t fnMask = 0, fnSpec = 0;
    if (!readAt(vtbl + 0x10, fnMask) || !readAt(vtbl + 0x30, fnSpec))  return;
    if (fnMask <= 0x10000 || fnSpec <= 0x10000)                        return;

    uint32_t mask = 0; char spectator = 0;
    __try {
        mask      = ((uint32_t (*)(void*))fnMask)((void*)validator);
        spectator = ((char (*)(void*))fnSpec)((void*)validator);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;                                    // not a live validator yet — try again next tick
    }

    if (mask == g_lastReadyMask) return;
    g_lastReadyMask = mask;

    if (mask <= 1 && !spectator) {
        logf("READY CHECK: mask=%08X — NOTHING BLOCKING, the campaign should be startable", mask);
        return;
    }
    logf("READY CHECK CHANGED: mask=%08X spectator=%d  <-- why START CAMPAIGN is refused",
         mask, (int)spectator);

    // Only these three reasons carry a named key in the binary; the numeric ones are printed as
    // "bit N" so we can correlate them against what we changed rather than mislabel them.
    struct { int bit; const char* name; } known[] = {
        { 24, "team_has_no_players" },
        { 27, "ffa_needs_4_teams"   },
        { 28, "duplicate_characters (two players on the same faction)" },
    };
    for (int b = 0; b < 32; ++b) {
        if (!((mask >> b) & 1)) continue;
        const char* name = nullptr;
        for (const auto& k : known) if (k.bit == b) name = k.name;
        if (name) logf("    bit %-2d SET  -> %s", b, name);
        else      logf("    bit %-2d SET  -> (localised reason, id not resolvable statically)", b);
    }
}
#endif

static void lobbyRefreshHook(void* self)
{
    resetGiftPanelState("campaign lobby refresh");
    const uintptr_t p = (uintptr_t)self;
    uint32_t count   = 0;
    bool     clamped = false;

#ifndef TW3K_RELEASE
    // Announce the first invocation. With hooks installed one at a time, the last "first call"
    // line in the log before a crash identifies which hook was executing.
    static volatile long announced = 0;
    if (InterlockedCompareExchange(&announced, 1, 0) == 0)
        logf("lobby guard: FIRST CALL, this=%016llX", (unsigned long long)p);

#endif
    if (p) g_liveLobby = p;   // free capture of the live instance
    expandSlots();            // game thread — safe place to touch the slot list

    // Sanity-bound the count: a wild value means we are not looking at a real lobby.
    if (p && safeRead((void*)(p + OFF_PLAYER_COUNT), &count, sizeof(count)) &&
        count > VANILLA_SLOT_CACHE && count <= 64) {
        __try {
            *(volatile uint32_t*)(p + OFF_PLAYER_COUNT) = VANILLA_SLOT_CACHE;
            clamped = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { clamped = false; }
        if (clamped) {
            ++g_guardHits;
            if (count > g_guardMaxSeen) g_guardMaxSeen = count;
        }
    }

    const bool nestedRefresh = g_inLobbyCacheRefresh;
    g_inLobbyCacheRefresh = true;
    if (g_origLobbyRefresh) g_origLobbyRefresh(self);
    g_inLobbyCacheRefresh = nestedRefresh;

    if (clamped) {
        __try { *(volatile uint32_t*)(p + OFF_PLAYER_COUNT) = count; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }


#ifdef TW3K_RELEASE
    captureLobbyPlayerNames(p);
#else
    reportPlayerVectorIfChanged(p);
#endif
    TW3K_DIAGNOSTIC(reportReadyMaskIfChanged());
}


// FUN_142CEB060 allocates and constructs the live 0x190-byte MPCampaignLobby.
// Run after successful construction, before the caller can populate its records. Reused
// addresses still invoke this factory. FUN_14026E760 constructs the idle global decoy.
static Detour g_lobbyCreateDetour;
static uintptr_t (*g_origLobbyCreate)(uintptr_t, uintptr_t) = nullptr;
static unsigned long long g_lobbyGeneration = 0;
static constexpr uint8_t EXPECT_LOBBY_CREATE[15] = {
    0x48,0x89,0x5C,0x24,0x18,0x57,0x48,0x83,0xEC,0x20,0xB9,0x90,0x01,0x00,0x00
};

static uintptr_t lobbyCreateHook(uintptr_t ignored, uintptr_t context)
{
    const uintptr_t lobby = g_origLobbyCreate(ignored, context);
    if (lobby) {
        resetSlotExpansionBudget();
        resetAutoFactionBudget();
        resetPanelCompletionState();
        resetGiftPanelState("lobby construction");
        g_liveLobby = lobby;
        logf("lobby lifetime: constructed generation=%llu lobby=%016llX; recovery budgets reset",
             ++g_lobbyGeneration, (unsigned long long)lobby);
    }
    return lobby;
}

bool installLobbyGuard()
{
    const bool lifetime = detourInstall(g_lobbyCreateDetour, g_base + RVA_LOBBY_CREATE,
        sizeof(EXPECT_LOBBY_CREATE), EXPECT_LOBBY_CREATE, (uintptr_t)&lobbyCreateHook,
        (void**)&g_origLobbyCreate, "lobby lifetime");
    const bool guard = detourInstall(g_lobbyDetour, g_base + RVA_LOBBY_REFRESH, LOBBY_STOLEN_LEN,
                         EXPECT_LOBBY_REFRESH, (uintptr_t)&lobbyRefreshHook,
                         (void**)&g_origLobbyRefresh, "lobby guard");
    return guard && lifetime;
}

void removeLobbyGuard()
{
    detourRemove(g_lobbyDetour, "lobby guard");
    detourRemove(g_lobbyCreateDetour, "lobby lifetime");
}

// ---- B4: sample the lobby player vector from OUR thread, not the engine's -----------------------
//
// ★★★ WHY THIS EXISTS (2026-08-04). `reportPlayerVectorIfChanged` only runs when the engine's own
// refresh calls it — and the refresh ran **five times** in a whole session (measured; see the name
// capture counters). So the watch reports whatever the vector happened to hold at those five
// moments and is blind afterwards. A four-player lobby formed today and the last line logged was
// `count=2`, which is not what the lobby ended up holding — it is where the sampling stopped.
//
// ⇒ That is §6iii's "the record vector never reaches 3" in one sentence: it was never a missing
// route, and not really a sampling problem either — it is a watch wired to the wrong clock.
//
// This samples twice a second from the probe thread and logs when the ID SET changes, so B4's
// question — *does a four-player lobby, or a reload, push an id past the 0..3 this mod assumes* —
// is answered by the log rather than by catching the right instant.
//
// ⚠ Read-only, bounded, and gated on `lobbyLooksLive` — the lobby is freed when the campaign starts
// and reading records out of it afterwards hands back garbage faction keys (run 7).
void watchLobbyPlayers()
{
    const uintptr_t lobby = g_liveLobby;
    if (!lobby || !lobbyLooksLive(lobby)) return;

    uint32_t cap = 0, count = 0; uint64_t recs = 0;
    if (!readAt(lobby + 0xC8, cap) || !readAt(lobby + OFF_PLAYER_COUNT, count) ||
        !readAt(lobby + OFF_PLAYER_RECORDS, recs))
        return;
    if (!recs || count > 16) return;                 // not a populated lobby — say nothing

    // ★★★ #11: capture NAMES on this clock too, not just the id set.
    //
    // This is the whole fix. Name capture used to hang off the engine's own refresh, which was
    // measured running **five times in a session**, so each machine sampled a still-filling vector
    // once and kept whatever subset it happened to see — which is exactly the 2026-08-10 finding
    // that every machine learns a different subset. At 2 Hz for the lobby's whole life, a name that
    // lands late is picked up within half a second instead of never.
    //
    // ⚠ `false` = NOT the game thread: the narrow name is stored here and the allocating wide build
    // is deferred to knownPlayerNameCa(). See the note beside materialisePlayerNameCa.
    captureLobbyPlayerNames(lobby, false);

#ifndef TW3K_RELEASE
    uint32_t ids[16] = { 0 };
    for (uint32_t i = 0; i < count; ++i)
        readAt((uintptr_t)recs + (uintptr_t)i * 0x48 + 0x10, ids[i]);

    // Change detection on the SET, not just the count: a reload keeps the count and swaps the ids,
    // which is exactly the case B4 is about.
    static uint32_t lastIds[16] = { 0 };
    static uint32_t lastCount   = 0xFFFFFFFF;
    bool changed = (count != lastCount);
    if (!changed)
        for (uint32_t i = 0; i < count; ++i)
            if (ids[i] != lastIds[i]) { changed = true; break; }
    if (!changed) return;
    for (uint32_t i = 0; i < count; ++i) lastIds[i] = ids[i];
    lastCount = count;

    char line[256] = { 0 }; int n = 0;
    uint32_t highest = 0;
    for (uint32_t i = 0; i < count; ++i) {
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, "%u ", ids[i]);
        if (ids[i] > highest && ids[i] != 0xFFFFFFFF) highest = ids[i];
    }
    logf("LOBBY WATCH: cap=%u count=%u  ids={ %s}", cap, count, line);

    if (highest > 3)
        logf("  ★★★ AN ID IS PAST 3 (highest=%u). The four panels are indexed 0..3 and auto-faction "
             "ignores anything above 3, so that player has no panel and cannot be given one. "
             "**THAT IS B4**, caught in the act.", highest);
#endif

}

#ifndef TW3K_RELEASE
void reportLobbyGuard()
{
    logf("lobby guard: %s | interventions=%u | highest player count seen=%u",
         g_lobbyDetour.active ? "ACTIVE" : "inactive", g_guardHits, g_guardMaxSeen);
    if (g_guardHits == 0)
        logf("  (no interventions yet — that is expected until a 3rd player is actually in the lobby)");

    // ★ THE BISECT READING. See the counters' definition for what each one means.
    logf("name capture: calls=%ld  names written=%ld  WOULD-HAVE-CLEARED=%ld",
         g_nameCaptureCalls, g_namesCaptured, g_nameWouldClear);

    // ★★ #11: is the 2 Hz watch actually doing the work the engine's refresh could not?
    //
    // `deferred` counts names first stored by the probe-thread watch; `late-built` counts the wide
    // strings the UI then built from them. A late build is a name that, on the previous build,
    // would have rendered as "Player N" — so a non-zero here IS the fix firing, and this is the one
    // line to grep for when asking whether #11 is actually fixed rather than just not reproduced.
    logf("name capture (#11): deferred by the 2 Hz watch=%ld  wide strings late-built=%ld",
         g_namesProbeSeen, g_namesLateBuilt);
    {
        int named = 0, unnamed = 0;
        char missing[128] = { 0 }; int mn = 0;
        for (uint32_t i = 0; i < MAX_NAMED_PLAYERS; ++i) {
            if (g_playerNames[i][0]) ++named;
            else { ++unnamed;
                   mn += _snprintf_s(missing + mn, sizeof(missing) - mn, _TRUNCATE, "%u ", i); }
        }
        logf("  names known for %d id(s); no name for: %s", named, unnamed ? missing : "(none)");
    }
    if (g_nameWouldClear > 0)
        logf("  ★★★ the removed name-move clear WOULD have fired %ld time(s) — and each firing makes "
             "the next pass re-copy that name, which leaks. That is the hang, measured without "
             "reinstating it.", g_nameWouldClear);
    else
        logf("  ⇒ the removed name-move clear would NEVER have fired, so it is exonerated and the "
             "hang is the other revert (readTurnNumber in watchCampaignTurns). ⚠ Only trust this "
             "after a load that used to hang.");
}
#endif
