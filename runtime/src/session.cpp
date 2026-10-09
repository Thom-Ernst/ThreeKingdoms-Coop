// session.cpp - MP session: advertise hook, session capture, open-slot injection, join handler, seat ordering.
//
// Split out of the original single-file tw3k_coop.cpp. Behaviour is unchanged: this was pure
// code motion. ../tw3k_coop.ASBUILT.cpp remains the known-good single-file snapshot.

#include "tw3k.h"

// ------------------------------------------------- MP session hook (join-accept)

// Goal: read the MP session's MAX-PLAYERS field live.
//   FUN_1404AB370 builds the lobby advertisement:
//     slotObj = *(this + 0xD3848)
//     count   = *(uint*)(slotObj + 0x1360)     // slot entries
//     max     = *(uint*)(slotObj + 0x1364)     // MAX PLAYERS  <-- the join-accept capacity
//     this->+0xD23D8 = max ; +0xD23D9 = max - occupied (FREE SLOTS) ; +0xD23DA = spectators
//   There is NO literal store to +0x1364 anywhere in the image, so max is a RUNTIME value
//   (copied between config objects by FUN_140438780) — raisable by a data write, no code patch.
//
// The EMPIRE_MP object is not reachable from any global (it is always passed as `this`), so we
// detour FUN_1404AB370 and capture RCX. Denuvo endurance passed, so a code detour is viable.


// The local-player object, and its vtable[0x20] returns THIS MACHINE'S player id. Derived from
// FUN_140471AB0, which reads it as cmdObj+0xD3880 where cmdObj = EMPIRE_MP+0x18 — the same +0x18 that
// makes slotObj land at EMPIRE_MP+0xD3848, so the two offsets corroborate each other.
// Without this a player has no way to know which slot in the dump is theirs, which made the
// "watch your own UNITS HELD" test unfollowable.


// ---- MP session capture -----------------------------------------------------

typedef void (*MpAdvertiseFn)(void*);
Detour             g_mpDetour;
static MpAdvertiseFn      g_origMpAdvertise = nullptr;
static volatile uintptr_t g_capturedMp      = 0;

uintptr_t capturedMp() { return g_capturedMp; }

// ---- open-slot injection ----------------------------------------------------
//
// "Failed to join game: Game full" comes from the advertised free-slot count going to zero.
// FUN_1404AB370 computes it as (slots that exist) - (slots occupied); the campaign lobby only ever
// pre-creates TWO slots, so with 2 players present free==0 and a 3rd is refused.
//
// FUN_140447780(slotObj) is the game's own "add an open slot": it builds a default player record
// (FUN_1404385D0) and appends it via FUN_1404477C0, which caps at 20 slots and recomputes the
// advertisement itself. Calling it is a normal engine operation — no patching, no struct surgery.
//
// It MUST run on a game thread: our probe thread could race the slot list mid-update. So a request
// is queued here and drained inside whichever hook fires next.

static constexpr uintptr_t RVA_ADD_OPEN_SLOT = 0x00447780; // FUN_140447780
typedef uint32_t (*AddOpenSlotFn)(void*);

void dumpMpSession();   // defined below

// Expansion runs AUTOMATICALLY. Relying on a hotkey pressed at the right moment cost a whole
// 3-machine session: the key was simply never pressed, and the lobby stayed at 2 slots. There is
// no timing to get right now — whenever the session has fewer slots than the target, we top it up.
static volatile long g_autoExpandTarget = 4;   // 0 disables
static volatile long g_expandRuns       = 0;   // runaway guard

static volatile long g_expandFaulted = 0;

void resetSlotExpansionBudget()
{
    InterlockedExchange(&g_expandRuns, 0);
    InterlockedExchange(&g_expandFaulted, 0);
}

void expandSlots()
{
    const long target = g_autoExpandTarget;
    if (target <= 0 || g_expandFaulted || g_expandRuns > 20) return;

    const uintptr_t mp = g_capturedMp;
    uintptr_t slotObj = 0;
    if (!mp || !readAt(mp + OFF_MP_SLOTOBJ, slotObj) || !slotObj) return;

    uint32_t count = 0;
    if (!readAt(slotObj + OFF_SLOT_COUNT, count)) return;
    // count==0 means the list is not built yet; >=target means nothing to do.
    if (count == 0 || count >= (uint32_t)target) return;

    InterlockedIncrement(&g_expandRuns);
    diagLogf("auto-expand: slot entries=%u, target=%ld -> adding %u open slot(s)",
         count, target, (uint32_t)target - count);

    auto addOpen = (AddOpenSlotFn)(g_base + RVA_ADD_OPEN_SLOT);
    for (uint32_t i = count; i < (uint32_t)target; ++i) {
        __try {
            const uint32_t idx = addOpen((void*)slotObj);
            diagLogf("  open slot -> returned index %u", idx);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            logf("  add open slot FAULTED — disabling auto-expand for this lobby");
            InterlockedExchange(&g_expandFaulted, 1);
            return;
        }
    }
    uint32_t after = 0;
    readAt(slotObj + OFF_SLOT_COUNT, after);
    logf(">>> AUTO-EXPAND DONE: slot entries %u -> %u", count, after);
#ifndef TW3K_RELEASE
    dumpMpSession();
#endif
}

static void mpAdvertiseHook(void* self)
{
#ifndef TW3K_RELEASE
    static volatile long announced = 0;
    if (InterlockedCompareExchange(&announced, 1, 0) == 0)
        logf("MP hook: FIRST CALL, this=%016llX", (unsigned long long)self);

#endif
    g_capturedMp = (uintptr_t)self;
    expandSlots();          // before the original, so it advertises the new free count immediately
    maybeAutoAssignFaction();  // break the no-faction/no-panel loop without a keypress
    drainFactionRequest();   // game thread — this is the most reliably-ticking hook we have
    if (g_origMpAdvertise) g_origMpAdvertise(self);
}


bool installMpHook()
{
    return detourInstall(g_mpDetour, g_base + RVA_MP_ADVERTISE, MP_STOLEN_LEN, EXPECT_MP_ADVERTISE,
                         (uintptr_t)&mpAdvertiseHook, (void**)&g_origMpAdvertise, "MP hook");
}

void removeMpHook() { detourRemove(g_mpDetour, "MP hook"); }

// ------------------------------------------- join handler: raise the advertised slot count
//
// THE ACTUAL 2-CAP. FUN_140474F10 is the client-side join handler. Every client builds its OWN
// local slot list from the host's advertised game description:
//
//     for (i = 0; i < *(uint*)(desc + 0x58); i++)     // 0x58 = NUMBER OF PLAYER SLOTS
//         FUN_1404477C0(slots, FUN_1404385D0(&rec, i % *(uint*)(desc+0x60), 0));
//     for (i = 0; i < *(uint*)(desc + 0x5C); i++) ... // 0x5C = spectator slots
//     ...
//     if (FUN_1404477C0(slots, &myRecord) == -1)
//         FUN_140412AA0(session, 0);                  // no slot for me -> SELF-DISCONNECT
//
// Reason 0 renders as "The host kicked you from the game" — nobody is actually kicking; the joiner
// drops itself. Growing only the HOST's slot array (which is what the earlier fix did) cannot help,
// because each client sizes its own list from this one advertised number.
//
// Correcting it here fixes every client from one place, and they all agree because they all derive
// from the same field.
//
// Only a value of exactly 2 is raised: that is the campaign-lobby case. Battle lobbies advertise
// their own (larger) counts and must be left alone.

static constexpr size_t    OFF_DESC_SLOTS    = 0x58;       // player slots
static constexpr size_t    OFF_DESC_SPECS    = 0x5C;       // spectator slots
static constexpr size_t    OFF_DESC_TEAMS    = 0x60;

typedef void (*JoinHandlerFn)(void*, void*, void*);
Detour        g_joinDetour;
static JoinHandlerFn g_origJoinHandler = nullptr;

static void joinHandlerHook(void* self, void* desc, void* p3)
{
#ifndef TW3K_RELEASE
    static volatile long announced = 0;
    if (InterlockedCompareExchange(&announced, 1, 0) == 0)
        logf("join hook: FIRST CALL, desc=%016llX", (unsigned long long)desc);

#endif
    if (desc && (uintptr_t)desc > 0x10000) {
        uint32_t slots = 0, specs = 0, teams = 0;
        if (readAt((uintptr_t)desc + OFF_DESC_SLOTS, slots) &&
            readAt((uintptr_t)desc + OFF_DESC_SPECS, specs) &&
            readAt((uintptr_t)desc + OFF_DESC_TEAMS, teams)) {
            diagLogf("join: advertised slots=%u spectators=%u teams=%u", slots, specs, teams);
            if (slots == 2 && g_autoExpandTarget > 2) {
                __try {
                    *(volatile uint32_t*)((uintptr_t)desc + OFF_DESC_SLOTS) =
                        (uint32_t)g_autoExpandTarget;
                    logf(">>> JOIN: raised advertised player slots 2 -> %ld", g_autoExpandTarget);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    logf("join: write to desc+0x58 FAULTED — leaving as-is");
                }
            }
        }
    }

    if (g_origJoinHandler) g_origJoinHandler(self, desc, p3);
}


bool installJoinHook()
{
    return detourInstall(g_joinDetour, g_base + RVA_JOIN_HANDLER, JOIN_STOLEN_LEN,
                         EXPECT_JOIN_HANDLER, (uintptr_t)&joinHandlerHook,
                         (void**)&g_origJoinHandler, "join hook");
}

void removeJoinHook() { detourRemove(g_joinDetour, "join hook"); }

// ------------------------------------------- seat ordering: take the FIRST free slot, not the last
//
// With the advertised slot count raised to 4, all three clients finally agreed on a 4-slot list —
// but the players landed in slots 2, 3 and 0 rather than 0, 1, 2. The lobby UI maps its panels by
// slot order, so the host and player 2 sat outside the two panels and were invisible, while player
// 3 held slot 0 and rendered as "player 1" complete with the host's controls.
//
// The cause is in FUN_1404477C0, which seats a record into a slot. When it is seating a REAL player
// (slots+0x1374 != 0 and the record's bit7 clear) it scans every slot and remembers each candidate:
//
//     if (vacant && spectatorClassMatches && teamMatches)
//         chosen = i;          // 0x14044792B `MOV EDI,EDX` — no break, so the LAST match wins
//
// The join handler creates slots with team `i % teams`, and teams is 2, so the four slots carry
// teams 0,1,0,1. Players receive teams in join order (0,1,0), so the host matched slots 0 and 2 and
// took 2, player 2 matched 1 and 3 and took 3, and player 3 was left with 0.
//
// Making the choice first-match yields 0,1,2,3 instead. Patching the loop itself is awkward — there
// is no room for an early exit, because the two bytes after `MOV EDI,EDX` are the `INC EDX` that
// the "no match" path jumps to. So instead we let the engine's own scan run and hide the later
// candidates from it: clear bit7 ("vacant") on every match after the first, call the original,
// restore. The engine then finds exactly one match, and its "last" is our "first".
//
// This is safe because nothing that runs inside the original call reads bit7. Before returning it
// recounts slots (reading bits 6 and 10) and calls FUN_1404AC430 to rebuild the player-id -> slot
// table (matching `flags & 0x140`, i.e. bits 6 and 8). The masked slots are never the one selected,
// so the record is written into the untouched first match. The mask lives only for the duration of
// one call on the game's own thread, which is the same thread that mutates this list anyway.

static constexpr size_t    OFF_SLOTS_MODE   = 0x1374;     // 0 = building the list, != 0 = seating
// ✂ SLOT_MAX_ENTRIES / SLOT_FLAG_VACANT / SLOT_FLAG_SPEC / SLOT_TEAM_MASK moved to offsets.h
//   2026-08-17 — battle.cpp's #13 side gate reads the same bits and must not carry its own copy.

typedef uint32_t (*SlotAppendFn)(void*, void*);
Detour        g_seatDetour;
static SlotAppendFn  g_origSlotAppend = nullptr;
static volatile long g_seatSeatings = 0;   // real players seated
static volatile long g_seatReorders = 0;   // times we actually changed the outcome
static volatile long g_seatNoSlot   = 0;   // times a real player found no seat (=> self-disconnect)

// logf reopens the log file on every call, so it must not run on a hot path. Seating is rare in
// principle, but if a network resync replays it we would rather lose log lines than frames.
static volatile long g_seatLogs = 0;
static bool seatMayLog() { return InterlockedIncrement(&g_seatLogs) <= 64; }

static uint32_t slotAppendHook(void* slotsPtr, void* recPtr)
{
    const uintptr_t slots = (uintptr_t)slotsPtr;
    const uintptr_t rec   = (uintptr_t)recPtr;

    uint32_t maskedIdx[SLOT_MAX_ENTRIES]   = { 0 };
    uint32_t maskedFlags[SLOT_MAX_ENTRIES] = { 0 };
    size_t   nMasked  = 0;
    uint32_t firstIdx = 0xFFFFFFFF;
    bool     seating  = false;

    uint8_t  mode     = 0;
    uint32_t recFlags = 0;
    uint32_t count    = 0;

    if (slots > 0x10000 && rec > 0x10000 &&
        readAt(slots + OFF_SLOTS_MODE,   mode)     &&
        readAt(rec   + SLOT_ENTRY_FLAGS, recFlags) &&
        readAt(slots + OFF_SLOT_COUNT,   count)    &&
        mode != 0 && (recFlags & SLOT_FLAG_VACANT) == 0 &&
        count > 0 && count <= SLOT_MAX_ENTRIES)
    {
        seating = true;
        const uint32_t recSpec = recFlags & SLOT_FLAG_SPEC;

        // Replicates the engine's own candidate test exactly — see the decompilation above.
        for (uint32_t i = 0; i < count; ++i) {
            const uintptr_t fp = slots + (uintptr_t)i * SLOT_ENTRY_STRIDE + SLOT_ENTRY_FLAGS;
            uint32_t f = 0;
            if (!readAt(fp, f))                                     continue;
            if ((f & SLOT_FLAG_VACANT) == 0)                        continue; // not an open seat
            if ((f & SLOT_FLAG_SPEC) != recSpec)                    continue; // player vs spectator
            if (!recSpec && ((f ^ recFlags) & SLOT_TEAM_MASK) != 0) continue; // different team

            if (firstIdx == 0xFFFFFFFF) { firstIdx = i; continue; }   // keep the first, hide the rest

            __try {
                *(volatile uint32_t*)fp = f & ~SLOT_FLAG_VACANT;
                maskedIdx[nMasked]   = i;
                maskedFlags[nMasked] = f;
                ++nMasked;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                if (seatMayLog()) logf("seat: mask write FAULTED at slot %u — stock order for this one", i);
            }
        }
    }

    const uint32_t chosen = g_origSlotAppend ? g_origSlotAppend(slotsPtr, recPtr) : 0xFFFFFFFF;

    for (size_t m = 0; m < nMasked; ++m) {
        const uintptr_t fp = slots + (uintptr_t)maskedIdx[m] * SLOT_ENTRY_STRIDE + SLOT_ENTRY_FLAGS;
        __try {
            *(volatile uint32_t*)fp = maskedFlags[m];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (seatMayLog()) logf("seat: restore FAULTED at slot %u (flags left masked!)", maskedIdx[m]);
        }
    }

    if (seating) {
        InterlockedIncrement(&g_seatSeatings);
        if (chosen == 0xFFFFFFFF) {
            InterlockedIncrement(&g_seatNoSlot);
            if (seatMayLog())
                logf("!! seat: NO FREE SLOT for player (flags=%08X team=%u spectator=%u) among %u slots"
                     " — this client will disconnect itself and report being kicked",
                     recFlags, recFlags & SLOT_TEAM_MASK,
                     (recFlags & SLOT_FLAG_SPEC) ? 1u : 0u, count);
        } else {
            if (nMasked) InterlockedIncrement(&g_seatReorders);
            if (seatMayLog())
                logf("seat: player (flags=%08X team=%u) -> slot %u of %u%s",
                     recFlags, recFlags & SLOT_TEAM_MASK, chosen, count,
                     nMasked ? "   <<< REORDERED (stock would have taken a later slot)" : "");
        }
    }

    return chosen;
}


bool installSeatHook()
{
    return detourInstall(g_seatDetour, g_base + RVA_SLOT_APPEND, SEAT_STOLEN_LEN,
                         EXPECT_SLOT_APPEND, (uintptr_t)&slotAppendHook,
                         (void**)&g_origSlotAppend, "seat order");
}

void removeSeatHook() { detourRemove(g_seatDetour, "seat order"); }

#ifndef TW3K_RELEASE
void reportSeatHook()
{
    logf("seat order: %s | players seated=%ld | reordered=%ld | no-slot failures=%ld",
         g_seatDetour.active ? "ACTIVE" : "not installed",
         g_seatSeatings, g_seatReorders, g_seatNoSlot);
    if (g_seatDetour.active && g_seatSeatings == 0)
        logf("  (nothing seated yet — this fires as each player is placed into the lobby)");
}
#endif
