// panels.cpp - Panel expansion to four, the panel-population hook, and the slot-changed probe.
//
// Split out of the original single-file tw3k_coop.cpp. Behaviour is unchanged: this was pure
// code motion. ../tw3k_coop.ASBUILT.cpp remains the known-good single-file snapshot.

#include "tw3k.h"

// ================= PANEL EXPANSION: let the lobby accept slots 2 and 3 =========================
//
// PROVEN BLOCKER (session 5l). Once player 3 sends a faction message, the notification DOES reach
// the lobby — and is then thrown away:
//
//     slot-changed: slot=2 panelCount=2 panel=0000000000000000 -> IGNORED (no panel for this slot)
//
// FUN_142CE66D0 bounds-checks the slot index against the PANEL COUNT (lobby+0xDC) and bails when
// there is no panel. So the fix is to give the lobby four panels.
//
// THE TRAP: the per-panel faction cache at lobby+0xE8 holds only TWO 16-byte entries. Entry 2 spans
// +0x108..+0x117 and lands on the UI component pointers at +0x110 (save_game_map, +0x118, +0x120 …).
// Simply growing the panel count makes the engine's own loops write over those pointers and crash.
//
// DESIGN — grow the vector, but never let the engine touch +0xE8[2..]:
//
//   * the panel VECTOR at {cap 0xD8, count 0xDC, ptr 0xE0} is a normal heap CA array, so appending
//     to it is safe and needs no relocation;
//   * we append AFTER FUN_142D4FE10's own reset loop has run, so that loop only ever sees count 2
//     and never clears past entry 1;
//   * FUN_142D7CD10's panel loop keeps its hardcoded `CMP R15D,2`, so the engine never READS past
//     entry 1 either;
//   * the existing lobby guard already clamps +0xCC during the refresh, which stops the refresh loop
//     writing past entry 1 if the player vector ever reaches 3 — that is precisely what it is for;
//   * and for slot >= 2 we handle the change OURSELVES, using our own 4-entry cache, so the
//     engine's array is never indexed beyond 1 from that path either.
//
// SELF-GATING SAFETY: the extra panels only appear if `panel_player3` / `panel_player4` exist as
// widgets, which only happens with mod/coop_4player_ui.pack installed. Without the pack this code
// finds nothing, skips, and the DLL behaves exactly as before.

static constexpr uintptr_t RVA_FIND_WIDGET   = 0x005611E0; // FUN_1405611E0(parent, CaString*, char)
static constexpr uintptr_t RVA_SET_VISIBLE   = 0x005A8BB0; // FUN_1405A8BB0(widget, char)
static constexpr uintptr_t RVA_VEC_PUSH      = 0x00311500; // FUN_140311500(vec, void** item)
static constexpr uintptr_t RVA_STR_ASSIGN    = 0x00664020; // FUN_140664020(dst, src)
static constexpr uintptr_t RVA_STR_CLEAR     = 0x00664040; // FUN_140664040(dst, &"NULL")
static constexpr uintptr_t RVA_NULL_LITERAL  = 0x0331AA5C; // DAT_14331AA5C == "NULL"
static constexpr uintptr_t RVA_GET_PANEL_CB  = 0x02D03030; // FUN_142D03030(panel) -> MPCampaignPlayer cb
static constexpr uintptr_t RVA_POPULATE      = 0x02D7AA90; // FUN_142D7AA90(cb, cacheBase, session, isLocal, flag)

// ★ SESSION 5s: the two functions that actually make a player panel appear. Both were missing from
// every previous build, which is why player 3's panel stayed blank however many other things worked.
//
//   FUN_142D4FB00(cb, groupData /*lobby+0xB8*/, playerId, record)  -- the per-panel INITIALISER.
//       *(void**)(cb+0x78) = groupData;      <- the field FUN_142D7AA90's entire body is gated on
//       sets "dy_player_name" from the record; shows the "player" sub-widget;
//       *(uint*)(cb+0x70) = playerId;        <- THE PLAYER INDEX (init leaves it at -1, not 0)
//       shows/hides both dropdowns via FUN_142D46300 (= "is this the local player").
//
//   FUN_142D6DA20(cb, factionKey, partyKey, groupIdx, isLoadedSave) -- the DISPLAY path.
//       if (cb+0xA2 == 0) { setVisible(cb+8, 1); play "FadeIn"; cb+0xA2 = 1; }   <- makes it VISIBLE
//       sets the faction_dropdown text, the faction icon (mon_256.png), the group dropdown text.
//
// The engine reaches both through FUN_142CDDBD0 (vtable +0x18 on lobby+0x80) and FUN_142CE66D0. We
// swallow the latter for slot >= 2, so we must supply these ourselves.
static constexpr uintptr_t RVA_PANEL_INIT    = 0x02D4FB00; // FUN_142D4FB00(cb, groupData, playerId, rec)
static constexpr uintptr_t RVA_APPLY_FACTION = 0x02D6DA20; // FUN_142D6DA20(cb, faction, party, grp, loaded)
static constexpr uintptr_t RVA_SET_DROPDOWNS = 0x02D7B410; // FUN_142D7B410(cb, forceDisable)
// Native 142CE66D0 sets this around display/cache/record work; 142CE30A0
// refuses dropdown events while it is 1. UI selection setters emit those events.
static constexpr uintptr_t RVA_FACTION_UI_GUARD = 0x043B6FC1;
static constexpr uintptr_t RVA_PANEL_READY = 0x02CE6040; // listener+0x28: status + local ready controls
static const uint8_t EXPECT_PANEL_READY[15] = {
    0x4C,0x8B,0xDC,0x55,0x56,0x57,0x41,0x56,0x48,0x8B,0xEC,0x48,0x83,0xEC,0x48
};
static volatile long g_extraPanelRepair = 1;
static bool g_panelReadyVerified = false;
bool extraPanelRepairArmed() { return g_extraPanelRepair != 0 && g_panelReadyVerified; }
#ifndef TW3K_RELEASE
void setExtraPanelRepair(bool on) { InterlockedExchange(&g_extraPanelRepair, on ? 1 : 0); }
#endif


static constexpr size_t OFF_PANEL_VEC   = 0xD8;   // {cap 0xD8, count 0xDC, ptr 0xE0}
static constexpr size_t OFF_PANEL_COUNT = 0x5C;   // on lobby+0x80 => lobby+0xDC
static constexpr size_t OFF_LOBBY_ROOT  = 0x08;   // the lobby's root UI widget
static constexpr size_t OFF_GROUP_DATA  = 0xB8;   // faction-group data, built by the lobby refresh
static constexpr size_t OFF_SETTINGS    = 0xC0;   // +0x60 on it = is-loaded-save
static constexpr size_t OFF_PLAYER_CNT  = 0xCC;   // lobby player vector {cap 0xC8, count 0xCC, ptr 0xD0}
static constexpr size_t OFF_PLAYER_VEC  = 0xD0;
static constexpr size_t PLAYER_REC_SIZE = 0x48;   // rec+0x10 = player id, +0x18 = faction, +0x28 = party
static constexpr size_t CASTRING_SIZE   = 0x10;

// Fields on the MPCampaignPlayer callback (allocated 0xA8 bytes by FUN_142CDC3E0).
static constexpr size_t CB_WIDGET       = 0x08;   // the panel widget itself
static constexpr size_t CB_PLAYER_IDX   = 0x70;   // -1 until FUN_142D4FB00 assigns it
static constexpr size_t CB_GROUP_DATA   = 0x78;   // 0 until FUN_142D4FB00 assigns it
static constexpr size_t CB_DROPDOWN     = 0x90;   // "faction_dropdown", resolved by name at bind time
static constexpr size_t CB_GRP_DROPDOWN = 0x98;   // "faction_group_dropdown", ditto
static constexpr size_t CB_SHOWN_FLAG   = 0xA2;   // set to 1 by FUN_142D6DA20 when it reveals the panel
static constexpr size_t CB_DROPDOWN_OK  = 0xA0;   // FUN_142D7B410 refuses to ENABLE unless this is nonzero

typedef void*    (*FindWidgetFn)(void*, void*, char);
typedef void     (*SetVisibleFn)(void*, char);
typedef void     (*VecPushFn)(void*, void**);
typedef void     (*StrAssignFn)(void*, void*);
typedef void     (*StrClearFn)(void*, void*);
typedef void*    (*GetPanelCbFn)(void*);
typedef void     (*PopulateFn)(void*, void*, void*, char, char);
typedef void     (*PanelResetFn)(void*);
typedef void     (*PanelInitFn)(void*, void*, uint32_t, void*);
typedef void     (*ApplyFactionFn)(void*, void*, void*, uint32_t, char);

// Our own 4-entry replacement for the lobby's 2-entry cache. Entries are real CA strings,
// initialised with the engine's own clear function so they are byte-identical to what it makes.
static uint8_t       g_slotCache[4][CASTRING_SIZE] = { { 0 } };
static volatile long g_slotCacheReady = 0;

// ★ B4-S5: FUN_142CE5D90's player-leave tail still calls the narrow literal
// assignment at 142CE5FC0 with lobby+E8+id*10. For id=2, destination+8 is
// loaded_map: assigning "" copies ONE NUL byte into the WIDGET VTABLE, even
// though the string length is zero. The normal leave behaviour stays intact;
// only that exact call's destination is moved to our external cache.
static Detour g_leaveCacheDetour;
typedef uintptr_t (*AssignLiteralFn)(uintptr_t, const char*);
static AssignLiteralFn g_origAssignLiteral = nullptr;
static volatile long g_leaveCacheArmed = 1, g_leaveCacheRedirects = 0;
static constexpr uintptr_t RVA_LEAVE_CACHE_RETURN = 0x02CE5FC5;
static const uint8_t EXPECT_ASSIGN_LITERAL[15] = {
    0x41,0x56,0x48,0x83,0xEC,0x40,0x48,0xC7,0x44,0x24,0x20,0xFE,0xFF,0xFF,0xFF
};
static const uint8_t EXPECT_LEAVE_CACHE_CALL[5] = { 0xE8,0x7B,0xE0,0x97,0xFD };
bool leaveCacheFixArmed() { return g_leaveCacheArmed != 0 && g_leaveCacheDetour.active; }
void setLeaveCacheFix(bool on) { InterlockedExchange(&g_leaveCacheArmed, on ? 1 : 0); }

static uintptr_t leaveCacheDestination(uintptr_t caller, uintptr_t destination)
{
    if (caller != g_base + RVA_LEAVE_CACHE_RETURN || !leaveCacheFixArmed() || g_slotCacheReady != 1)
        return destination;
    for (uint32_t id = 2; id < 4; ++id) {
        const uintptr_t offset = 0xE8 + id * CASTRING_SIZE;
        if (destination < offset + 0x10000) continue;
        const uintptr_t lobby = destination - offset;
        uint32_t panels = 0;
        if (!lobbyLooksLive(lobby) || !readAt(lobby + 0xDC, panels) || panels <= id || panels > 4) continue;

#ifndef TW3K_RELEASE
        if (InterlockedIncrement(&g_leaveCacheRedirects) <= 24)
            logf("B4 leave-cache: id=%u redirected %016llX -> external cache; loaded_map/UI vtables preserved",
                 id, (unsigned long long)destination);
#endif

        return (uintptr_t)g_slotCache[id];
    }
    return destination;
}
static uintptr_t assignLiteralHook(uintptr_t destination, const char* literal)
{
    return g_origAssignLiteral(leaveCacheDestination((uintptr_t)_ReturnAddress(), destination), literal);
}
bool installLeaveCacheGuard()
{
    uint8_t call[sizeof(EXPECT_LEAVE_CACHE_CALL)] = {};
    if (!safeRead((void*)(g_base + RVA_LEAVE_CACHE_RETURN - sizeof(call)), call, sizeof(call)) ||
        memcmp(call, EXPECT_LEAVE_CACHE_CALL, sizeof(call)) != 0) {
        logf("B4 leave-cache: exact leave CALL SIGNATURE MISMATCH; refusing hook"); return false;
    }
    return detourInstall(g_leaveCacheDetour, g_base + RVA_STR_CLEAR, sizeof(EXPECT_ASSIGN_LITERAL),
        EXPECT_ASSIGN_LITERAL, (uintptr_t)&assignLiteralHook, (void**)&g_origAssignLiteral, "B4 leave-cache redirect");
}
void removeLeaveCacheGuard()
{
    setLeaveCacheFix(false);
    detourRemove(g_leaveCacheDetour, "B4 leave-cache redirect");
}

Detour        g_resetDetour;
static PanelResetFn  g_origPanelReset  = nullptr;
volatile long g_panelsAdded     = 0;   // how many extra panels are currently registered
volatile long g_panelPushFails  = 0;
volatile long g_extraSeats      = 0;   // slot>=2 changes we handled ourselves

static void initSlotCacheOnce()
{
    if (InterlockedCompareExchange(&g_slotCacheReady, 1, 0) != 0) return;
    auto clearStr = (StrClearFn)(g_base + RVA_STR_CLEAR);
    void* nullLit = (void*)(g_base + RVA_NULL_LITERAL);
    __try {
        for (int i = 0; i < 4; ++i) clearStr(g_slotCache[i], nullLit);
        diagLogf("panel expansion: 4-entry faction cache initialised at %016llX",
             (unsigned long long)(uintptr_t)g_slotCache);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("panel expansion: cache init FAULTED — expansion disabled");
        InterlockedExchange(&g_slotCacheReady, -1);
    }
}

// * #12 - set around our OWN widget lookup so the B9 guard's report can say WHICH lookup went
// wrong. The engine calls FUN_1405611E0 constantly; we call it a handful of times per lobby, and
// the ticket's open question is whether the corrupt subtree is reached from a name WE duplicated.
// A bare pointer to a string literal: every caller below passes one, and it outlives the call.
#ifndef TW3K_RELEASE
thread_local const char* t_ourLookupName = nullptr;
#endif
#ifndef TW3K_RELEASE
thread_local uintptr_t   t_ourLookupRoot = 0;
#endif
// Find a widget by name under the lobby's root.
static void* findLobbyWidget(uintptr_t lobby, const char* name)
{
    uintptr_t root = 0;
    if (!readAt(lobby + OFF_LOBBY_ROOT, root) || !root) return nullptr;
    alignas(16) uint8_t s[48] = { 0 };
    // ! Set OUTSIDE the __try, and cleared after it: MSVC forbids objects needing unwinding inside
    // a __try, so this is two plain stores rather than anything RAII-shaped.
    TW3K_DIAGNOSTIC(t_ourLookupName = name);
    TW3K_DIAGNOSTIC(t_ourLookupRoot = root);
    void* found = nullptr;
    __try {
        ((MakeCaStringFn)(g_base + RVA_MAKE_CASTRING))(s, name);
        found = ((FindWidgetFn)(g_base + RVA_FIND_WIDGET))((void*)root, s, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = nullptr;
    }
    TW3K_DIAGNOSTIC(t_ourLookupName = nullptr);
    TW3K_DIAGNOSTIC(t_ourLookupRoot = 0);
    return found;
}

// Runs AFTER the original reset. The original has just set panelCount = 2 and cleared entries 0..1;
// appending here means its loop never saw counts above 2.
static void panelResetHook(void* self)
{
    if (g_origPanelReset) g_origPanelReset(self);

    const uintptr_t lobby = (uintptr_t)self;
    if (!lobby) return;
    initSlotCacheOnce();
    if (g_slotCacheReady != 1) return;

    uint32_t before = 0;
    if (!readAt(lobby + OFF_PANEL_VEC + 4, before)) return;
    if (before != 2) return;              // only extend the stock 2-panel case

    // Plain array, not an initializer_list: this translation unit does not pull in <initializer_list>
    // and a ranged-for over a braced list needs it.
    static const char* const kExtraPanels[2] = { "panel_player3", "panel_player4" };

    long added = 0;
    for (int pi = 0; pi < 2; ++pi) {
        const char* name = kExtraPanels[pi];
        void* panel = findLobbyWidget(lobby, name);
        if (!panel) { InterlockedIncrement(&g_panelPushFails); continue; }
        __try {
            void* item = panel;
            ((VecPushFn)(g_base + RVA_VEC_PUSH))((void*)(lobby + OFF_PANEL_VEC), &item);
            ++added;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&g_panelPushFails);
        }
    }

    uint32_t after = 0;
    readAt(lobby + OFF_PANEL_VEC + 4, after);
    InterlockedExchange(&g_panelsAdded, added);

    static volatile long announced = 0;
    if (InterlockedCompareExchange(&announced, 1, 0) == 0) {
        if (added == 0)
            logf("panel expansion: panel_player3/4 NOT FOUND — is mod/tw3k_coop.pack "
                 "installed? (running unmodified, nothing changed)");
        else
            diagLogf("panel expansion: registered %ld extra panel(s); panelCount %u -> %u",
                 added, before, after);
    }
}


bool installPanelResetHook()
{
    uint8_t bytes[sizeof(EXPECT_PANEL_READY)] = {};
    g_panelReadyVerified = safeRead((void*)(g_base + RVA_PANEL_READY), bytes, sizeof(bytes)) &&
        memcmp(bytes, EXPECT_PANEL_READY, sizeof(bytes)) == 0;
    logf("B4 panel display: ready-handler signature=%s repair=%s",
         g_panelReadyVerified ? "matched" : "MISMATCH", extraPanelRepairArmed() ? "ARMED" : "off");
    return detourInstall(g_resetDetour, g_base + RVA_PANEL_RESET, RESET_STOLEN_LEN,
                         EXPECT_PANEL_RESET, (uintptr_t)&panelResetHook,
                         (void**)&g_origPanelReset, "panel reset");
}

void removePanelResetHook() { detourRemove(g_resetDetour, "panel reset"); }

// ⚠⚠ RE-ENTRANCY. FUN_142D7AA90 finishes by calling FUN_142D7B410, a notify that comes straight back
// round as another slot-change for the same slot. The first build called the populator every time and
// produced an unbounded feedback loop: thousands of "handled slot 2" lines per second on every client,
// and then a crash. Two independent brakes now:
//
//   1. a re-entrancy flag, so a notify raised from inside our own populate call cannot recurse;
//   2. change detection — the populate only runs when the faction actually DIFFERS from what we last
//      applied, which is what makes the loop converge instead of merely slowing it down.
//
// The incoming string is read with the engine's own accessors (FUN_140674190 length /
// FUN_140668CD0 data) because CA strings use small-string optimisation, so the {len,cap,ptr} layout
// cannot be assumed for short values like "NULL".
static constexpr uintptr_t RVA_STR_LEN  = 0x00674190; // FUN_140674190(CaString*) -> length
static constexpr uintptr_t RVA_STR_DATA = 0x00668CD0; // FUN_140668CD0(CaString*) -> char*

typedef uint32_t (*StrLenFn)(void*);
typedef char*    (*StrDataFn)(void*);

struct PanelCompletion {
    uintptr_t lobby = 0, panel = 0, callback = 0, group = 0;
    char faction[64] = {}, party[64] = {};
    uint32_t groupIndex = 0;
    bool complete = false;
};
static PanelCompletion g_panelCompletion[4];

static bool panelCompletionMatches(const PanelCompletion& c, uintptr_t lobby, uintptr_t panel, uintptr_t cb, uintptr_t group, uint32_t index, uint32_t slot, uint8_t shown, const char* faction, const char* party, uint32_t groupIndex)
{
    return c.complete && faction && faction[0] && group && shown && index == slot &&
           c.lobby == lobby && c.panel == panel && c.callback == cb && c.group == group &&
           strcmp(c.faction, faction) == 0 && party && strcmp(c.party, party) == 0 &&
           c.groupIndex == groupIndex;
}

static void recordPanelCompletion(PanelCompletion& c, uintptr_t lobby, uintptr_t panel, uintptr_t cb, uintptr_t group, uint32_t index, uint32_t slot, uint8_t shown, const char* faction, const char* party, uint32_t groupIndex, bool displayed)
{
    c.complete = false;
    if (!displayed || !faction || !faction[0] || !party || !group || !shown || index != slot) return;
    c.lobby = lobby; c.panel = panel; c.callback = cb; c.group = group;
    strcpy_s(c.faction, sizeof(c.faction), faction);
    strcpy_s(c.party, sizeof(c.party), party);
    c.groupIndex = groupIndex;
    c.complete = true;
}
static volatile long g_inExtraHandler     = 0;

void resetPanelCompletionState()
{
    for (auto& completion : g_panelCompletion) completion = {};
    InterlockedExchange(&g_panelsAdded, 0);
    // Never clear the re-entry lock: an outer handler still owns it if construction nests.
}
static volatile long g_extraLogs          = 0;

// Copy a CA string's text out via the engine's accessors. Returns false if unreadable.
//
// ★ Shared (declared in tw3k.h) since 2026-08-03: the lobby captures player NAMES with it, and
// those are CA strings in exactly the same SSO shape. It was already the only correct reader in the
// codebase; making it reachable was cheaper than growing a second one.
bool readCaString(void* str, char* out, size_t outSz)
{
    if (!str) return false;
    __try {
        const uint32_t len = ((StrLenFn)(g_base + RVA_STR_LEN))(str);
        char* data = ((StrDataFn)(g_base + RVA_STR_DATA))(str);
        if (!data || len >= outSz) return false;
        memcpy(out, data, len);
        out[len] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ★★ Same string, but it may be UTF-16 — and in a lobby player record the two live side by side.
//
// The header is shared between the encodings, which is why one reader nearly served both:
//   FUN_140674190 returns a CHARACTER count either way (SSO packs it into the top byte of +0x08,
//   otherwise it is the u32 at +0x00), and FUN_140668CD0 returns the base pointer — the same address
//   FUN_140663AB0(str, 0) gives. Only the STEP differs, and FUN_140663AB0 multiplies the index by
//   **2**, which is what proves the element size. The profanity filter that runs over the player
//   name writes '*' through it as a 2-byte store.
//
// ⇒ In one 0x48-byte record, `+0x18` (faction key) is narrow ASCII and `+0x00` (player name) is
// UTF-16. Reading either with the other's step gives "3" or "F.o.r.d.o.".
//
// ★ So decide by LOOKING rather than by trusting a type: an ASCII string held as UTF-16 has a NUL
// as its second byte, and a narrow string of length > 1 cannot. That makes this correct whichever
// the field turns out to be, which matters for a value no test on this machine can check.
bool readCaStringAuto(void* str, char* out, size_t outSz, bool* wasWide)
{
    if (!str || outSz < 2) return false;
    if (wasWide) *wasWide = false;
    __try {
        const uint32_t len  = ((StrLenFn)(g_base + RVA_STR_LEN))(str);
        char*          data = ((StrDataFn)(g_base + RVA_STR_DATA))(str);
        if (!data || len == 0 || len >= outSz) return false;

        const bool wide = (len > 1) && (data[1] == '\0');
        if (wasWide) *wasWide = wide;

        if (!wide) {
            memcpy(out, data, len);
            out[len] = '\0';
            return true;
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)data, (int)len,
                                          out, (int)outSz - 1, nullptr, nullptr);
        if (n <= 0) return false;
        out[n] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Handle a slot >= 2 change. Returns true if handled (caller must then NOT run the original itself).
//
// ★ THE ORIGINAL IS NEVER RUN FOR SLOT >= 2, and cannot be (session 5p). It writes a CA string to
// lobby+0xE8+slot*0x10, which for slot 2 lands on the UI component pointers at +0x110 — and then, in
// the same call, `142CDEA26 MOV RBX,[R14+0x90]` reads lobby+0x110 back and uses it as a parent widget.
// The read happens DURING the call, so snapshot/restore around it cannot help (5o tried; it crashed
// every client).
//
// Instead we reproduce the parts of it that matter, against our own 4-entry cache:
//
//   FUN_142D4FB00  — initialise the panel for this player (+0x70 index, +0x78 group data, name,
//                    sub-widgets). Only needed if the engine's FUN_142CDDBD0 never reached this panel.
//   FUN_142D6DA20  — the display path: reveal the panel (+0xA2 / FadeIn) and set the faction text+icon.
//
// Everything else the original does is either lobby-wide and unsafe for us (FUN_142D27260, which reads
// the +0x110 pointer) or bounded to panels 0/1 anyway (FUN_142D7CD10's hardcoded `CMP R15D,2`).
//
// ★ B4-S2 correction to the old registration comment: 1.7.2 FUN_142CE5970 calls
// FUN_142D7C810 to BUILD the records, then FUN_142D5AE70 to LOOK UP one record.
// The lookup never appends. The builder enumerates teams 0/1 only; saveload.cpp
// adds the other occupied teams outside the unsafe inline-cache refresh. Run 5p's
// count=3 was possible with three people on those two teams and did not refute the bound.
typedef void (*RunOrigFn)(void*);

static bool handleExtraSlotChange(uintptr_t self, uint32_t slotIdx, void* newFaction,
                                  void* newParty, int groupIdx, RunOrigFn runOrig, void* ctx)
{
    if (slotIdx < 2 || slotIdx >= 4)   return false;
    if (g_slotCacheReady != 1)         return false;
    if (g_panelsAdded <= 0)            return false;   // no extra panels -> stock behaviour

    // Brake 1: never re-enter. FUN_142D7CD10 (called by the original) and FUN_142D7AA90 both end in a
    // notify that arrives back here for the same slot. Swallow it, still returning true so the caller
    // does not run the original a second time.
    // NB explicit unlock at every exit — MSVC forbids __try in a function that needs object unwinding.
    if (InterlockedCompareExchange(&g_inExtraHandler, 1, 0) != 0)
        return true;

    PanelCompletion& completion = g_panelCompletion[slotIdx];

    const uintptr_t lobby = self - 0x80;               // `self` is the lobby+0x80 sub-object
    // ★ A four-panel lobby can have only three records, in order {0,2,1}.
    // Revalidate membership BEFORE convergence, cache writes or engine calls: a
    // callback still showing id 3 is not evidence that id 3 remains in the roster.
    // Always match record+0x10; vector position is never a player id.
    uintptr_t rec = 0, recs = 0;
    uint32_t recCount = 0, recCap = 0;
    bool rosterReadable = readAt(lobby + OFF_PLAYER_CNT, recCount) &&
        readAt(lobby + 0xC8, recCap) && recCount <= recCap && recCap <= 32 &&
        readAt(lobby + OFF_PLAYER_VEC, recs) && (!recCount || recs);
    for (uint32_t i = 0; rosterReadable && i < recCount; ++i) {
        const uintptr_t candidate = recs + (uintptr_t)i * PLAYER_REC_SIZE;
        uint32_t id = ~0u;
        if (!readAt(candidate + 0x10, id)) { rosterReadable = false; break; }
        if (id == slotIdx) rec = candidate;
    }
    if (!rosterReadable || !rec) {
        completion.complete = false;
        if (InterlockedIncrement(&g_extraLogs) <= 24)
            logf("panel expansion: REFUSED id=%u — %s (records=%u cap=%u); stale callback not used",
                 slotIdx, rosterReadable ? "player record absent" : "roster unreadable/invalid",
                 recCount, recCap);
        InterlockedExchange(&g_inExtraHandler, 0);
        return true;
    }
    char incoming[64] = { 0 };
    const bool haveText = readCaString(newFaction, incoming, sizeof(incoming));
    char incomingParty[64] = {};
    const bool haveParty = readCaString(newParty, incomingParty, sizeof(incomingParty));
    uint32_t panelCount = 0; uint64_t panelsPtr = 0; void* panel = nullptr;
    if (!readAt(self + OFF_PANEL_COUNT, panelCount) || slotIdx >= panelCount ||
        !readAt(self + 0x60, panelsPtr) || !panelsPtr ||
        !readAt((uintptr_t)panelsPtr + (uintptr_t)slotIdx * 8, panel) || !panel) {
        InterlockedExchange(&g_inExtraHandler, 0);
        return false;
    }

    uint64_t session = 0;
    readAt(lobby + 0xB0, session);

    // --- ❌ DO NOT run the original for slot >= 2. Snapshot/restore CANNOT make this safe. ---
    //
    // Session 5o tried it and crashed every client. The reason is in FUN_142CE66D0 itself:
    //
    //     142CDEA26  MOV RBX, [R14 + 0x90]    ; R14 = lobby+0x80  ->  lobby+0x110
    //     142CDEA3D  CALL 0x1405611E0         ; then uses RBX as the PARENT WIDGET
    //
    // `lobby+0x110` is a UI component pointer, and it is inside the 16 bytes our slot-2 cache entry
    // overlaps. So the original READS the pointer we just clobbered, in the same call — it consumes a
    // fragment of a CA string as a widget and dies. Restoring afterwards is far too late.
    //
    // It also turns out not to be needed: session 5o proved the lobby player vector reaches count=3
    // (with player id=2 present) purely from a SLOT-0 change, which runs the original on a safe slot.
    // Registration does not depend on running the original for slot 2 at all.
    (void)runOrig; (void)ctx;

    __try {
        uint8_t priorGuard = 0;
        if (!readAt(g_base + RVA_FACTION_UI_GUARD, priorGuard)) {
            logf("S21 faction display refused: engine dropdown-event guard unreadable");
            InterlockedExchange(&g_inExtraHandler, 0);
            return true;
        }
        // ★ The re-entry brake cannot stop an emitted NETWORK command.
        // Mirror the native display guard so selecting accepted text does not
        // queue another 0xCD from the group/faction picker. Restore nested native
        // state even on early convergence, missing callback or SEH unwind.
        *(volatile uint8_t*)(g_base + RVA_FACTION_UI_GUARD) = 1;
        __try {
        if (newFaction) ((StrAssignFn)(g_base + RVA_STR_ASSIGN))(g_slotCache[slotIdx], newFaction);

        // ❌ NO unconditional setVisible here (removed 5z). Forcing a panel visible before it owns a
        // CcoFrontendFactionLeader context is one of the two ways to produce the "player 1's controls
        // are dead" failure: an unoccupied panel then renders its ANCESTOR's context (5y) and, worse,
        // sits over the vanilla panel intercepting its input (5z). FUN_142D6DA20 reveals the panel
        // itself, at the moment it sets the context — which is the only moment it is safe to do.
        void* cb = ((GetPanelCbFn)(g_base + RVA_GET_PANEL_CB))(panel);
        if (!cb) {
            if (InterlockedIncrement(&g_extraLogs) <= 24)
                logf("panel expansion: slot %u has NO MPCampaignPlayer callback on panel %016llX "
                     "— pack widget is missing the callback declaration", slotIdx,
                     (unsigned long long)panel);
            InterlockedExchange(&g_inExtraHandler, 0);
            return true;
        }

        // ★ THE DECISIVE DIAGNOSTIC (session 5s). FUN_142CDB950 (the callback's widget-bind init)
        // leaves +0x70 at -1 and +0x78 at 0; only FUN_142D4FB00 sets them. So these two values say
        // whether the engine's own registration path (FUN_142CDDBD0, vtable +0x18 on lobby+0x80,
        // gated on the panel count) ever ran for THIS panel:
        //
        //   +0x70 == -1 and +0x78 == 0  ->  it never did; we must initialise the panel ourselves
        //   +0x70 == slot and +0x78 != 0 ->  it did, and only the display call was missing
        //
        // Note this corrects session 5r, which predicted "+0x70 was 0". The init writes -1, so 0 was
        // never the expected uninitialised value.
        uint32_t idxBefore = 0xFFFFFFFF;
        uint64_t grpBefore = 0, ddBefore = 0, gddBefore = 0;
        uint8_t  shownBefore = 0;
        readAt((uintptr_t)cb + CB_PLAYER_IDX,   idxBefore);
        readAt((uintptr_t)cb + CB_GROUP_DATA,   grpBefore);
        TW3K_DIAGNOSTIC(readAt((uintptr_t)cb + CB_DROPDOWN, ddBefore));
        TW3K_DIAGNOSTIC(readAt((uintptr_t)cb + CB_GRP_DROPDOWN, gddBefore));
        readAt((uintptr_t)cb + CB_SHOWN_FLAG,   shownBefore);
        uintptr_t currentGroup = 0;
        readAt(lobby + OFF_GROUP_DATA, currentGroup);
        uint8_t ownVisible = 0, effectiveVisible = 0;
        const bool widgetVisible = readAt((uintptr_t)panel + 0x2C4, ownVisible) &&
            readAt((uintptr_t)panel + 0x2C5, effectiveVisible) && ownVisible == 1 && effectiveVisible == 1;
        // Brake 2: convergence requires a completed display on this current callback.
        // A new callback, hidden/reset callback or failed previous display must retry.
        // ★ A real 0xCD acknowledgement must reconcile the dropdown even when
        // the service refused the pick and reports the SAME accepted faction.
        // The user-facing text may already show the attempted duplicate and the
        // picker has disabled the controls. Only timer retries may converge here.
        if (!runOrig && (!extraPanelRepairArmed() || widgetVisible) &&
            (!saveLobbyFixArmed() || !currentGroup || grpBefore == currentGroup) &&
            panelCompletionMatches(completion, lobby, (uintptr_t)panel, (uintptr_t)cb,
                                   grpBefore, idxBefore, slotIdx, shownBefore, haveText ? incoming : nullptr,
                                   haveParty ? incomingParty : nullptr, (uint32_t)groupIdx)) {
            InterlockedExchange(&g_inExtraHandler, 0);
            return true;
        }
        completion.complete = false;

#ifndef TW3K_RELEASE
        if (InterlockedIncrement(&g_extraLogs) <= 24)
            logf("panel expansion: slot %u cb=%016llX BEFORE +0x70(playerIdx)=%d +0x78(groupData)=%016llX "
                 "+0x90(dropdown)=%016llX +0x98(grpDropdown)=%016llX +0xA2(shown)=%u",
                 slotIdx, (unsigned long long)cb, (int)idxBefore, (unsigned long long)grpBefore,
                 (unsigned long long)ddBefore, (unsigned long long)gddBefore, (unsigned)shownBefore);
#endif


        // ---- (1) initialise the panel for this player, if the engine never did ------------------
        // Mirrors FUN_142CDDBD0: find the lobby player record whose +0x10 matches this id, then call
        // FUN_142D4FB00(cb, lobby+0xB8, playerId, record). That sets +0x70 AND +0x78, fills in
        // "dy_player_name" from the record, reveals the "player" sub-widget and shows the dropdowns.
        uint64_t groupData = 0;
        readAt(lobby + OFF_GROUP_DATA, groupData);

#ifndef TW3K_RELEASE
        char recordFaction[64] = {};
        const bool recordReadable = rec && readCaString((void*)(rec + 0x18), recordFaction, sizeof(recordFaction));

#ifndef TW3K_RELEASE
        if (InterlockedIncrement(&g_extraLogs) <= 24)
            logf("panel expansion: slot %u record=%s faction=\"%s\" callback=%016llX group=%016llX shown=%u cached=\"%s\"",
                 slotIdx, !rec ? "absent" : !recordReadable ? "unreadable" : recordFaction[0] ? "populated" : "empty",
                 recordReadable ? recordFaction : "<unavailable>", (unsigned long long)cb,
                 (unsigned long long)grpBefore, (unsigned)shownBefore, completion.faction);
#endif

#endif
        // The archived test-host capture has record faction empty while callback shown=1.
        // The incoming notification supplies display text independently of that record.
        if (grpBefore == 0 || (saveLobbyFixArmed() && groupData && grpBefore != groupData) ||
            (extraPanelRepairArmed() && (!widgetVisible || idxBefore != slotIdx))) {
            // +0x78 is dereferenced twice inside (`**(char**)(cb+0x78)`), so groupData must be a
            // valid pointer-to-pointer. It is built by the lobby refresh; refuse to call without it.
            if (groupData && rec) {
                ((PanelInitFn)(g_base + RVA_PANEL_INIT))(cb, (void*)groupData, slotIdx, (void*)rec);

#ifndef TW3K_RELEASE
                if (InterlockedIncrement(&g_extraLogs) <= 24)
                    logf("panel expansion: slot %u ran FUN_142D4FB00(cb, groupData=%016llX, id=%u, "
                         "rec=%016llX) — panel initialised for this player",
                         slotIdx, (unsigned long long)groupData, slotIdx, (unsigned long long)rec);
#endif

            } else if (InterlockedIncrement(&g_extraLogs) <= 24) {
                logf("panel expansion: slot %u CANNOT init — groupData=%016llX rec=%016llX "
                     "(recCount=%u) — panel will stay blank",
                     slotIdx, (unsigned long long)groupData, (unsigned long long)rec, recCount);
            }
        }

        // ---- (2) belt and braces: the index must name this slot ---------------------------------
        uint32_t idxNow = 0xFFFFFFFF;
        readAt((uintptr_t)cb + CB_PLAYER_IDX, idxNow);
        if (idxNow != slotIdx) {
            __try {
                *(volatile uint32_t*)((uintptr_t)cb + CB_PLAYER_IDX) = slotIdx;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                logf("panel expansion: could not write callback+0x70 for slot %u", slotIdx);
            }

#ifndef TW3K_RELEASE
            if (InterlockedIncrement(&g_extraLogs) <= 24)
                logf("panel expansion: slot %u forced +0x70 %d -> %u", slotIdx, (int)idxNow, slotIdx);
#endif

        }

        // ---- (3) the display path — what FUN_142CE66D0 would have called -----------------------
        // FUN_142D6DA20 is what actually reveals the panel (+0xA2 / FadeIn) and writes the faction
        // dropdown text and icon. The original only calls it when the new faction is non-empty; an
        // empty one means "no faction yet", which FUN_142D4FB00 has already rendered correctly.
        //
        // Deliberately NOT called: FUN_142D27260 (it reads lobby+0x110, the UI pointer our slot-2
        // cache entry overlaps) and FUN_142D7CD10 (repopulates panels 0/1 only, and its notify is
        // what caused the 5n feedback loop).
        uint64_t settings = 0;
        char     loaded   = 0;
        readAt(lobby + OFF_SETTINGS, settings);
        if (settings) readAt((uintptr_t)settings + 0x60, loaded);

        // ★★★★ AND THE PANEL HAS TO BE INITIALISED, NOT MERELY EXIST (2026-08-04 — B4/B9's cause).
        //
        // `FUN_142D6DA20` dereferences `cb + 0x78` (the group data) TWICE, and only null-checks it
        // the first time:
        //
        //     142d62fef  MOV RBX,[RDI+0x78] / TEST RBX,RBX / JZ ...   ← checked here
        //     142d63248  MOV RDX,[RDI+0x78]
        //     142d6325d  MOV RDX,[RDX+0x8]                            ← and NOT here
        //
        // so with a null group pointer it reads address 0x8 and the process dies. That is a latent
        // fault in the engine's own code, but vanilla never reaches it because vanilla never calls
        // this function on an uninitialised panel. **We do.**
        //
        // ✅ MEASURED, not deduced (2026-08-04, the save-load lobby that lost players 3 and 4). Our
        // own line and the crash witness line up to the address and the millisecond:
        //
        //     16:52:08.334  slot 2 cb=BE2C8830  +0x70(playerIdx)=-1  +0x78(groupData)=0
        //     16:52:08.336  FAULT at 142D6325D, READ from 0x8, RDI=BE2C8830
        //     16:52:08.959  slot 3 cb=BE2DB210  +0x70(playerIdx)=-1  +0x78(groupData)=0
        //     16:52:08.959  FAULT at 142D6325D, READ from 0x8, RDI=BE2DB210
        //
        // ⇒ `playerIdx == -1` with `groupData == 0` is a panel the engine has created and never
        // populated — exactly what a **save lobby** leaves behind, because the factions come from the
        // save and no `0xCD` faction-change message is ever sent to initialise the panel.
        //
        // ⇒ So B4's *"a reload lobby loses players 3 and 4"* and this crash are the same event: the
        // panels are not initialised, and our attempt to display them kills the client.
        // ⚠ Re-read AFTER the init attempt above, not `grpBefore` — the initialiser sets `+0x78`
        // when it runs, and the whole question is whether the panel ended up with one.
        uint64_t grpNow = 0;
        readAt((uintptr_t)cb + 0x78, grpNow);

        const bool haveFaction = haveText && incoming[0] != '\0' && strcmp(incoming, "NULL") != 0;
        if (haveFaction && !grpNow) {
            static volatile long shouted = 0;
            if (InterlockedCompareExchange(&shouted, 1, 0) == 0)
                logf("panel expansion: slot %u REFUSING the display call — cb+0x78 (group data) is "
                     "NULL, so FUN_142D6DA20 would read address 0x8 and kill this client. The panel "
                     "exists but was never populated, which is what a save lobby leaves behind. "
                     "The panel stays hidden instead, which is B4's symptom without the crash.",
                     slotIdx);
        } else if (haveFaction) {
            ((ApplyFactionFn)(g_base + RVA_APPLY_FACTION))(cb, newFaction, newParty,
                                                           (uint32_t)groupIdx, loaded);
        }

        uint8_t shownAfter = 0;
        readAt((uintptr_t)cb + CB_SHOWN_FLAG, shownAfter);

        // ---- (4) ★★★ RE-ENABLE THE FACTION DROPDOWNS (found 2026-07-30, session 6h) -------------
        //
        // Reported from the first 4-player lobby: "changing character disables the faction button;
        // changing faction disables both; pressing start campaign enables them again." That is not a
        // side effect of anything we do — it is the engine's own interlock, and the half that undoes
        // it is bounded by 2.
        //
        //   FUN_142CE30A0 (1.7.2) is the dropdown-pick handler. BOTH branches end:
        //       FUN_1404A6580(session, playerId, ...);   // send the faction-set command 0xCD
        //       FUN_142D7B410(cb, 1);                    // 1 = FORCE DISABLE both dropdowns
        //   so picking anything deliberately greys the controls until the change is confirmed.
        //
        //   FUN_142D7B410(cb, force) is the only thing that sets their state. It picks the ENABLED
        //   constant only when: force == 0, cb+0x70 == the LOCAL player id, a session flag
        //   (vtable[0x50]) is clear, and cb+0xA0 is nonzero. The `cb+0x70 == local id` term is what
        //   keeps you from editing another player's faction, so calling it on every machine is safe:
        //   it enables your own panel and disables everyone else's, which is correct.
        //
        //   The re-enable — FUN_142D7B410(cb, 0) — is reached from exactly one place: the
        //   "nothing actually changed" tail of FUN_142CE66D0. And that tail is
        //       local_58 = 2; do { ... panelArray[i] ... } while (--local_58);
        //   a hardcoded TWO. It searches panels 0 and 1 for a callback whose +0x70 equals the slot
        //   that changed, so for slot 2 or 3 it matches nothing and the dropdowns are never
        //   re-enabled. A fourth hardcoded 2 in the lobby UI, and the one that governs input.
        //
        // We already own the slot >= 2 path, so we do what the loop would have done. This may also
        // retire F8: those dropdowns were disabled from the player's first use of them, not missing.
        if (haveFaction) {
            uint8_t dropdownOk = 0;
            TW3K_DIAGNOSTIC(readAt((uintptr_t)cb + CB_DROPDOWN_OK, dropdownOk));
            __try {
                ((void(*)(void*, char))(g_base + RVA_SET_DROPDOWNS))(cb, 0);

#ifndef TW3K_RELEASE
                if (InterlockedIncrement(&g_extraLogs) <= 24)
                    logf("panel expansion: slot %u dropdowns re-enabled via FUN_142D7B410(cb,0) "
                         "(+0xA0=%u — must be nonzero on the OWNING machine or the engine refuses "
                         "to enable)", slotIdx, (unsigned)dropdownOk);
#endif

            } __except (EXCEPTION_EXECUTE_HANDLER) {
                logf("panel expansion: slot %u FUN_142D7B410 FAULTED — dropdowns left as they were",
                     slotIdx);
            }
        }

        // ★ FUN_142CE5970 normally follows initialisation/display with listener+0x28,
        // passing record+0x38. Recovered records never took that registration route, so
        // their status kept the XML default "AI" until an actual Ready notification.
        // Use the engine handler: it also keeps LOCAL ready/dropdown controls consistent,
        // without changing a ready bit or sending a network command. No inline-cache access.
        if (extraPanelRepairArmed() && rec && haveFaction && grpNow && shownAfter) {
            uint8_t ready = 0;
            if (readAt(rec + 0x38, ready) && ready <= 1) {
                ((void(*)(void*, uint32_t, uint8_t))(g_base + RVA_PANEL_READY))((void*)self, slotIdx, ready);

#ifndef TW3K_RELEASE
                if (InterlockedIncrement(&g_extraLogs) <= 48)
                    logf("B4 panel display: id=%u status=%s via FUN_142CE6040; prior visible=%u/%u",
                         slotIdx, ready ? "ready" : "not_ready", ownVisible, effectiveVisible);
#endif

            }
        }

        uint32_t completedIndex = 0xFFFFFFFF;
        readAt((uintptr_t)cb + CB_PLAYER_IDX, completedIndex);
        recordPanelCompletion(completion, lobby, (uintptr_t)panel, (uintptr_t)cb, grpNow,
                              completedIndex, slotIdx, shownAfter, haveText ? incoming : nullptr,
                              haveParty ? incomingParty : nullptr, (uint32_t)groupIdx,
                              haveFaction && grpNow);
        TW3K_DIAGNOSTIC(InterlockedIncrement(&g_extraSeats));

#ifndef TW3K_RELEASE
        if (InterlockedIncrement(&g_extraLogs) <= 24)
            logf(">>> panel expansion: slot %u DISPLAY ATTEMPT faction=\"%s\" (panel=%016llX cb=%016llX, "
                 "display=%s, +0xA2(shown) now %u, original skipped — unsafe for slot>=2, see 5p)",
                 slotIdx, haveText ? incoming : "<unreadable>",
                 (unsigned long long)panel, (unsigned long long)cb,
                 (haveFaction && grpNow) ? "FUN_142D6DA20 ran"
                 : haveFaction              ? "REFUSED (cb+0x78 null — would have crashed)"
                                            : "skipped (no faction yet)",
                 (unsigned)shownAfter);
#endif

        } __finally {
            *(volatile uint8_t*)(g_base + RVA_FACTION_UI_GUARD) = priorGuard;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedIncrement(&g_extraLogs) <= 24)
            logf("panel expansion: FAULTED on slot %u panel work — slot left undisplayed (safe)",
                 slotIdx);
    }

    InterlockedExchange(&g_inExtraHandler, 0);
    return true;
}

// Saved-lobby repair calls this on the game thread, with the session's current slots.
void refreshSavedExtraPanels(uintptr_t lobby, uintptr_t slots)
{
    // No guessed faction and no network message: the record writer supplies both keys.
    // Enumerate the records actually present; their +0x10 is the service's slot id,
    // not the record vector position or the session's player->slot table index.
    uint32_t count = 0, slotCount = 0;
    uintptr_t records = 0;
    if ((!saveLobbyFixArmed() && !extraPanelRepairArmed()) || !lobbyLooksLive(lobby) ||
        !readAt(lobby + OFF_PLAYER_COUNT, count) || count > 4 ||
        !readAt(lobby + OFF_PLAYER_RECORDS, records) || !records ||
        !readAt(slots + OFF_SLOT_COUNT, slotCount) || slotCount > 4) return;
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t record = records + i * PLAYER_REC_SIZE;
        uint32_t id = ~0u, flags = 0, faction = ~0u, group = 0;
        if (!readAt(record + 0x10, id) || id < 2 || id >= 4 || id >= slotCount ||
            !readAt(slots + id * SLOT_ENTRY_STRIDE + SLOT_ENTRY_FLAGS, flags) ||
            !(flags & 0x40) || (flags & (SLOT_FLAG_VACANT | SLOT_FLAG_SPEC)) ||
            !readAt(slots + id * SLOT_ENTRY_STRIDE + 0x60, faction) || faction == ~0u ||
            !readAt(record + 0x3C, group)) continue;
        handleExtraSlotChange(lobby + 0x80, id, (void*)(record + 0x18),
                              (void*)(record + 0x28), (int)group, nullptr, nullptr);
    }
}

// Fresh lobbies need the same retry when the faction notification preceded the
// record or when reset hid a callback whose +0xA2 still says "shown". Saved lobby
// enumeration stays in saveload.cpp; this consumes only records already present.
void tickExtraLobbyPanels(uintptr_t lobby)
{
    if (!extraPanelRepairArmed() || !lobbyLooksLive(lobby)) return;
    uint8_t leaving = 0;
    uintptr_t setup = 0, session = 0, slots = 0;
    uint8_t loaded = 0;
    if (!readAt(lobby + 0x90, leaving) || leaving == 1 ||
        !readAt(lobby + 0xC0, setup) || !setup || !readAt(setup + 0x60, loaded) || loaded ||
        !readAt(lobby + 0xB0, session) || !session ||
        !readAt(session + OFF_MP_SLOTOBJ, slots) || !slots) return;
    static uintptr_t lastLobby = 0;
    static ULONGLONG next = 0;
    const auto now = GetTickCount64();
    if (lobby == lastLobby && now < next) return;
    lastLobby = lobby; next = now + 500;
    refreshSavedExtraPanels(lobby, slots);
}

// ------------------------------------------- panel-population hook (probe carrier)
//
// WHY THIS EXISTS: the lobby refresh FUN_142D57630 turns out to run essentially ONCE, when the
// lobby opens — not when players join. That is why the overflow guard has always reported
// `interventions=0`, and why in run 3 the player-vector and ready-check probes each logged a single
// sample taken before anyone had joined. Hanging diagnostics off the refresh was the wrong choice.
//
// FUN_142D7CD10 is the panel population function. It is called from the lobby's slot-change event
// handlers (FUN_142CE66D0 / FUN_142CDF250), so it runs on EVERY client every time a player's state
// changes — exactly the moment we want to sample. It takes the lobby as its only argument, on the
// game thread, which is also where the ready-check virtual call has to happen.
//
// This hook changes NO behaviour. It calls the original and then samples. The hardcoded 2-panel
// bound inside it (`CMP R15D,2` at 0x142D7231F) is deliberately left alone for now.


typedef void (*PanelPopulateFn)(void*);
Detour          g_panelDetour;
static PanelPopulateFn g_origPanelPopulate = nullptr;
volatile long   g_panelCalls        = 0;

static void panelPopulateHook(void* lobby)
{
#ifndef TW3K_RELEASE
    static volatile long announced = 0;
    if (InterlockedCompareExchange(&announced, 1, 0) == 0)
        logf("panel populate: FIRST CALL, lobby=%016llX", (unsigned long long)lobby);

#endif
    TW3K_DIAGNOSTIC(InterlockedIncrement(&g_panelCalls));
    if (lobby) g_liveLobby = (uintptr_t)lobby;   // also a more reliable live-instance capture

    if (g_origPanelPopulate) g_origPanelPopulate(lobby);


#ifdef TW3K_RELEASE
    captureLobbyPlayerNames((uintptr_t)lobby);
#else
    reportPlayerVectorIfChanged((uintptr_t)lobby);
#endif
    TW3K_DIAGNOSTIC(reportReadyMaskIfChanged());
}


bool installPanelHook()
{
    return detourInstall(g_panelDetour, g_base + RVA_PANEL_POPULATE, PANEL_STOLEN_LEN,
                         EXPECT_PANEL_POPULATE, (uintptr_t)&panelPopulateHook,
                         (void**)&g_origPanelPopulate, "panel probe");
}

void removePanelHook() { detourRemove(g_panelDetour, "panel probe"); }

// ============================================================ B9 GUARD: the widget-tree walk (#12)
//
// B9 is the FOURTH player crashing to desktop while joining. It faults inside FUN_14057BEB0, the
// recursive descent that resolves a DUPLICATED widget name — a path reachable ONLY for duplicated
// names, which is what `coop_4player_ui.pack` creates by cloning panel_player1/2 and every child
// inside them, so `faction_dropdown` and friends exist four times over.
//
// The chain from our code to the faulting instruction is complete: FUN_1405611E0 picks the
// descending walk when its third argument is non-zero, and `findWidget()` above passes 1.
//
// ✗ NOT the fix: passing 0 instead. That selects FUN_140557BC0, which walks UP the parent chain —
//   "is this widget above me", not "find my named descendant". A different question, not a safer
//   version of the same one. Recorded because it is the obvious wrong move.
//
// ★ WHAT THIS DOES: the walk validates nothing, so we validate for it. The recursion target is the
// function's own entry, so this hook is re-entered at EVERY level of the descent — one detour
// covers the whole tree. On a bad node we return 0, which means "not found" and which every caller
// already handles. ⇒ A corrupt subtree makes the lookup FAIL instead of killing the process; the
// panel stays blank, which is B4's accepted shape and strictly the better failure.
//
// ★★ WHY THE TEST IS A RANGE CHECK AND NOT A READ. Both captured B9 faults had pointers ABOVE the
// user-mode ceiling:
//
//     0x00FFFFFF00FFFFFF   the child ARRAY (fault at 14057BD41)
//     0x0065004400202022   the NODE — UTF-16 "• De…" (fault at 14057BD22)
//
// Three compares catch both. That matters on this path: it is hot (every duplicated-name lookup,
// once per node of a subtree), and the alternatives are worse than they look —
//   * `safeRead` costs a VirtualQuery per node;
//   * a bare SEH read is free until it faults, and then it raises a FIRST-CHANCE exception that the
//     crash witness prints as a FAULT block. On 2026-08-10 five of those were read as "our DLL
//     crashed five times" when the game ran on for forty minutes. A guard whose normal operation
//     manufactures crash-looking log entries is a bad guard.
// ⇒ An implausible node still needs no engine-memory dereference. S20 additionally
// bounds depth/work and detects active-chain cycles in BOTH builds. Accepted nodes
// validate the complete candidate/child array spans, so hot-path VirtualQuery cost
// must be checked in the next live run; a plausible base alone was insufficient.
//
// ⚠ DELIBERATELY NOT DONE: a vtable test for "is this really a widget". B8's probe saw
// `0x1437FDE20` on three widgets, and whether that covers every widget type in a panel subtree is
// NOT established — a too-strict test would refuse lookups that work today. Readability-shaped
// checks only until that is measured. (This project has three retractions from tests built on what
// a value looked like rather than what it was.)
volatile bool g_treeWalkGuard = true;    // ★ #48's lesson: ship the off switch WITH the guard

Detour g_treeWalkDetour;
typedef uintptr_t (*TreeWalkFn)(uintptr_t node, uintptr_t candidates);
static TreeWalkFn g_origTreeWalk = nullptr;

static volatile long g_treeWalkRefusals = 0;
static volatile long g_treeWalkLogged   = 0;

// User-mode pointers only, and every widget pointer seen is 8-aligned. No dereference.
static inline bool plausibleNode(uintptr_t p)
{
    return p >= 0x10000 && p < 0x00007FFFFFFFFFFFULL && (p & 7) == 0;
}

// ================================================================================================
// ★★★ #12's REMAINING QUESTION, AND WHAT THIS INSTRUMENT ANSWERS
// ================================================================================================
//
// The guard below already turns the crash into a "not found". What the ticket still asks for is the
// SOURCE of the string: *"the two faults are one instruction apart in the same walk, so a third
// reproduction adds little. What is wanted is which widget's child list holds it."*
//
// Historical PRE-1.7.2 instruction labels below came from the raw PE 2026-09-07.
// ★ S20 verified /v172/Three_Kingdoms_172.exe read-only in Ghidra: the entry is
// 14057BEB0, node-count read 14057BEF2, child load 14057BF11, recursive CALL
// 14057BF15. The old BD22/BD41 labels in historical logs name the same operations.
//
//     BCEF  rax = [rdx+8]            ; candidates.ptr    — RDX is a {..,count@+4,ptr@+8} vector
//     BCFA  rsi = rdx                ; ← the crash dump's RSI IS the candidate list
//     BD06  cmp [rax], rdi           ; linear search: is this node one of the candidates?
//     BD22  cmp [rdi+0x124], ebx     ; ← FAULT 2. rdi is the NODE, handed in by the caller
//     BD35  rax = [rdi+0x128]        ; the children array
//     BD41  rcx = [rax + rcx*8]      ; ← FAULT 1. children[i]
//     BD45  call FUN_14057BEB0       ; recurse with (child, candidates)
//
// Two things follow, and both are free:
//
//  1. ★★ **`candidates` is already an argument of this hook.** "Dump RSI" needs no new plumbing at
//     all — the crash's `RSI=0x89830C38` is exactly this pointer, and its count and entries name
//     the duplicated widget whose resolution went wrong.
//
//  2. ★★ **For FAULT 2 the parent is simply the PREVIOUS FRAME.** The recursion target is this
//     function's own entry, so our detour is re-entered at every level; a depth counter and a small
//     per-thread array recover the whole chain, and `chain[depth-1]` is the widget whose child list
//     handed us the garbage. That is the thing #12 has been asking for by name.
//
// ⚠ The chain costs two thread-local stores per accepted node. Real, but small next to what the
// accept path already pays — one `safeRead`, and so one VirtualQuery, per node (see the comment at
// that call). The REJECT path is unchanged: three compares, no dereference, no syscall.
//
// ★ The report is cold-path and deliberately extravagant: it fires at most four times per run, and
// each field is there because its absence cost something. In particular it dumps the parent's RAW
// BYTES around `+0x120` — if a CA string's inline buffer overlaps the child array, the LAYOUT is
// what shows it, and a list of decoded pointers would not.

static constexpr int B9_MAX_DEPTH = 96;
static thread_local uintptr_t t_walkChain[B9_MAX_DEPTH] = { 0 };
static thread_local int       t_walkDepth = 0;
static thread_local uint32_t  t_walkVisits = 0;
static constexpr uint32_t B9_MAX_VISITS = 8192;

// Check the ENTIRE span before handing an array to the unguarded engine loop.
// A plausible base alone does not prove that count*8 bytes remain mapped.
static bool readableWalkArray(uintptr_t array, uint32_t count)
{
    if (!count) return true;
    if (count > 4096 || !plausibleNode(array)) return false;
    const uintptr_t end = array + (uintptr_t)count * sizeof(uintptr_t);
    while (array < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((void*)array, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= array) return false;
        array = next;
    }
    return true;
}
#ifndef TW3K_RELEASE
static thread_local uintptr_t t_walkCaller = 0;

static volatile long  g_b9Reports    = 0;
static constexpr long B9_MAX_REPORTS = 4;   // each is ~25 lines

// A pointer value that is really text decodes here. Both captured faults were that shape, and
// `0x0065004400202022` was decoded BY HAND on 2026-08-11 — which is the step this removes.
static void decodePointerAsText(uintptr_t v, char* out, size_t n)
{
    uint8_t b[8];
    memcpy(b, &v, sizeof(b));
    size_t o = 0;
    o += (size_t)_snprintf_s(out + o, n - o, _TRUNCATE, "ascii=\"");
    for (int k = 0; k < 8 && o + 12 < n; ++k)
        o += (size_t)_snprintf_s(out + o, n - o, _TRUNCATE, "%c",
                                 (b[k] >= 0x20 && b[k] < 0x7F) ? (char)b[k] : '.');
    o += (size_t)_snprintf_s(out + o, n - o, _TRUNCATE, "\"  utf16=\"");
    for (int k = 0; k + 1 < 8 && o + 8 < n; k += 2) {
        const uint16_t w = (uint16_t)(b[k] | (b[k + 1] << 8));
        o += (size_t)_snprintf_s(out + o, n - o, _TRUNCATE, "%c",
                                 (w >= 0x20 && w < 0x7F) ? (char)w : '.');
    }
    _snprintf_s(out + o, n - o, _TRUNCATE, "\"");
}

// Everything #12 asked for, in one block, at the moment the guard refuses.
static void reportB9Culprit(const char* why, uintptr_t bad, uintptr_t parent, uintptr_t candidates,
                            int depth)
{
    if (InterlockedIncrement(&g_b9Reports) > B9_MAX_REPORTS) return;

    char txt[192] = { 0 };
    decodePointerAsText(bad, txt, sizeof(txt));

    logf("=================== #12/B9 CULPRIT REPORT %ld ===================", g_b9Reports);
    logf("  %s", why);
    logf("  bad value : %016llX   %s", (unsigned long long)bad, txt);
    logf("  depth     : %d   root of this walk: %016llX",
         depth,
         (unsigned long long)(depth > 0 ? t_walkChain[0] : bad));
    logf("  root caller: %016llX (RVA_%08llX)", (unsigned long long)t_walkCaller,
         (unsigned long long)(t_walkCaller >= g_base ? t_walkCaller - g_base : t_walkCaller));
    // ★ The old "not ours" marker only excludes findLobbyWidget, not an engine
    // lookup called indirectly by our panel work. A bounded cold-path trace lets
    // the next shallow refusal name that initiating callback without a dump.
    static thread_local void* frames[16];
    const USHORT frameCount = CaptureStackBackTrace(0, 16, frames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        const uintptr_t pc = (uintptr_t)frames[i];
        logf("      caller[%u] = %016llX (game RVA_%08llX)", (unsigned)i,
             (unsigned long long)pc, (unsigned long long)(pc >= g_base ? pc - g_base : pc));
    }

    // ---- WHICH LOOKUP. The engine calls the name lookup constantly; we call it rarely.
    if (t_ourLookupName)
        logf("  ★ THIS IS OUR OWN LOOKUP: name=\"%s\" root=%016llX — the duplicated name is one of "
             "ours, from tw3k_coop.pack.",
             t_ourLookupName, (unsigned long long)t_ourLookupRoot);
    else
        logf("  ★ NOT an explicit findLobbyWidget lookup — the engine was resolving a duplicated "
             "name. This does not identify the initiating callback or the corrupting writer; "
             "use the caller trace above to distinguish indirect panel work.");

    // ---- THE CANDIDATE LIST — the crash dump's RSI, and it names the duplicated widget.
    uint32_t  cCount = 0;
    uintptr_t cPtr   = 0;
    if (candidates && safeRead((void*)(candidates + 4), &cCount, sizeof(cCount)) &&
        safeRead((void*)(candidates + 8), &cPtr, sizeof(cPtr))) {
        logf("  candidates@%016llX : count=%u ptr=%016llX   (the same-named widgets)",
             (unsigned long long)candidates, cCount, (unsigned long long)cPtr);
        const uint32_t show = cCount > 16 ? 16u : cCount;
        for (uint32_t k = 0; k < show; ++k) {
            uintptr_t e = 0;
            if (safeRead((void*)(cPtr + k * 8), &e, sizeof(e)))
                logf("      cand[%u] = %016llX%s", k, (unsigned long long)e,
                     e == bad ? "   ⇐ the BAD value is IN the candidate list, so the NAME TABLE is "
                                "the source, not a child array" : "");
        }
        if (cCount > show) logf("      ... %u more", cCount - show);
    } else {
        logf("  candidates@%016llX : unreadable", (unsigned long long)candidates);
    }

    // ---- THE PARENT, AND WHICH SLOT OF ITS CHILD ARRAY HOLDS THE GARBAGE.
    if (!parent) {
        logf("  parent    : NONE — this was the ROOT call, so the bad pointer came from the caller "
             "of FUN_1405611E0 and not from any widget's child list. ⇒ That is a DIFFERENT bug from "
             "the one #12 describes, and worth saying out loud rather than filing as the same one.");
    } else {
        uint32_t  pCount = 0;
        uintptr_t pArr   = 0;
        const bool okC = safeRead((void*)(parent + OFF_WIDGET_CHILD_COUNT), &pCount, sizeof(pCount));
        const bool okA = safeRead((void*)(parent + OFF_WIDGET_CHILD_ARRAY), &pArr, sizeof(pArr));
        logf("  parent    : %016llX   childCount=%s%u  childArray=%s%016llX",
             (unsigned long long)parent, okC ? "" : "unreadable:", pCount,
             okA ? "" : "unreadable:", (unsigned long long)pArr);

        if (okC && okA && pArr && pCount && pCount <= 4096) {
            const uint32_t show = pCount > 24 ? 24u : pCount;
            for (uint32_t k = 0; k < show; ++k) {
                uintptr_t e = 0;
                if (!safeRead((void*)(pArr + k * 8), &e, sizeof(e))) {
                    logf("      child[%u] unreadable", k);
                    continue;
                }
                if (e == bad)
                    logf("      child[%u] = %016llX   ⇐ ★★ THIS SLOT. The parent above is the "
                         "widget whose child list holds it. ★★", k, (unsigned long long)e);
                else
                    // A neighbour that IS a plausible widget is the useful contrast: it says the
                    // array was partly overwritten rather than pointing at the wrong object.
                    logf("      child[%u] = %016llX%s", k, (unsigned long long)e,
                         plausibleNode(e) ? "" : "   (also not a plausible node)");
            }
            if (pCount > show) logf("      ... %u more", pCount - show);
        }

        // ---- THE LAYOUT. If a CA string's inline buffer overlaps the child array, only the raw
        // bytes show it — a list of decoded pointers cannot.
        for (uintptr_t off = 0x100; off < 0x140; off += 0x10) {
            uint8_t raw[16] = { 0 };
            if (!safeRead((void*)(parent + off), raw, sizeof(raw))) {
                logf("      parent+%03llX  unreadable", (unsigned long long)off);
                continue;
            }
            char hex[56] = { 0 }, asc[20] = { 0 };
            for (int k = 0; k < 16; ++k) {
                _snprintf_s(hex + k * 3, sizeof(hex) - k * 3, _TRUNCATE, "%02X ", raw[k]);
                asc[k] = (raw[k] >= 0x20 && raw[k] < 0x7F) ? (char)raw[k] : '.';
            }
            logf("      parent+%03llX  %s|%s|%s", (unsigned long long)off, hex, asc,
                 (off == 0x120) ? "   ← childCount at +0x124, childArray at +0x128" : "");
        }
    }
    logf("================================================================");
}

#endif
static uintptr_t treeWalkHook(uintptr_t node, uintptr_t candidates)
{
    if (!g_treeWalkGuard || !g_origTreeWalk)
        return g_origTreeWalk ? g_origTreeWalk(node, candidates) : 0;

    // The frames above us, captured BEFORE we push ourselves. At depth 0 this is the root call and
    // there is no parent — which is itself a finding, so the report says so rather than printing 0.
    const int       depth  = t_walkDepth;
#ifndef TW3K_RELEASE
    const uintptr_t parent = (depth > 0 && depth <= B9_MAX_DEPTH) ? t_walkChain[depth - 1] : 0;
#endif
    if (depth == 0) {
        t_walkVisits = 0;
        TW3K_DIAGNOSTIC(t_walkCaller = (uintptr_t)_ReturnAddress());
    }
    bool cycle = false;
    for (int i = 0; i < depth; ++i) if (t_walkChain[i] == node) { cycle = true; break; }
    if (depth >= B9_MAX_DEPTH || ++t_walkVisits > B9_MAX_VISITS || cycle) {
        const long n = InterlockedIncrement(&g_treeWalkRefusals);
        if (InterlockedIncrement(&g_treeWalkLogged) <= 8)
            logf("B9 GUARD: REFUSED — %s node=%016llX depth=%d visits=%u refusals=%ld",
                 cycle ? "cycle in active chain" : depth >= B9_MAX_DEPTH ? "depth limit" : "visit limit",
                 (unsigned long long)node, depth, t_walkVisits, n);
        TW3K_DIAGNOSTIC(reportB9Culprit("Cycle/depth/work bound: recursive descent refused.",
                                      node, parent, candidates, depth));
        return 0;
    }
    if (!plausibleNode(node)) {
        const long n = InterlockedIncrement(&g_treeWalkRefusals);
        // ⚠ Rate-limited, and it says so. A crash-adjacent path that spams is a path nobody reads.
        if (InterlockedIncrement(&g_treeWalkLogged) <= 8)
            logf("★★★ B9 GUARD: REFUSED a widget-tree walk — node=%016llX is not a plausible "
                 "pointer.\n"
                 "    This is the B9 crash, caught. The engine would have read its child count at "
                 "+0x124 and faulted (14057BD22).\n"
                 "    Returning 0 = \"not found\": the lookup fails, the widget stays unresolved, "
                 "the process lives. refusals=%ld", (unsigned long long)node, n);
        TW3K_DIAGNOSTIC(reportB9Culprit("A child list handed the walk something that is not a widget "
                        "(the 14057BD22 fault).", node, parent, candidates, depth));
        return 0;
    }

    // The original reads the candidate vector even before it examines this node.
    uint32_t candidateCount = 0;
    uintptr_t candidateArray = 0;
    if (!plausibleNode(candidates) ||
        !readAt(candidates + 4, candidateCount) || !readAt(candidates + 8, candidateArray) ||
        !readableWalkArray(candidateArray, candidateCount)) {
        const long n = InterlockedIncrement(&g_treeWalkRefusals);
        if (InterlockedIncrement(&g_treeWalkLogged) <= 8)
            logf("B9 GUARD: REFUSED — candidate vector unreadable/invalid at %016llX refusals=%ld",
                 (unsigned long long)candidates, n);
        return 0;
    }

    // The node itself is plausible; its CHILD ARRAY is the other thing the original dereferences
    // without checking. Read both under the same no-fault discipline.
    uint32_t  count = 0;
    uintptr_t arr   = 0;
    if (!safeRead((void*)(node + OFF_WIDGET_CHILD_COUNT), &count, sizeof(count)) ||
        !safeRead((void*)(node + OFF_WIDGET_CHILD_ARRAY), &arr,   sizeof(arr))) {
        // ⚠ safeRead (VirtualQuery) is used HERE and not on `node` above deliberately: this runs
        // only once we already believe the node is real, so it is off the fast rejection path, and
        // being wrong here is what costs a process.
        const long n = InterlockedIncrement(&g_treeWalkRefusals);
        if (InterlockedIncrement(&g_treeWalkLogged) <= 8)
            logf("★★★ B9 GUARD: REFUSED — node=%016llX is plausible but its child count/array at "
                 "+0x124/+0x128 could not be read. refusals=%ld", (unsigned long long)node, n);
        TW3K_DIAGNOSTIC(reportB9Culprit("A plausible node whose own child count/array is unreadable.",
                        node, parent, candidates, depth));
        return 0;
    }

    if (!readableWalkArray(arr, count)) {
        const long n = InterlockedIncrement(&g_treeWalkRefusals);
        if (InterlockedIncrement(&g_treeWalkLogged) <= 8)
            logf("★★★ B9 GUARD: REFUSED — node=%016llX claims %u children at array=%016llX.\n"
                 "    ★ THIS NAMES THE CULPRIT, which is what #12 has been asking for: the parent "
                 "whose child list is corrupt, rather than the register dump of whoever followed "
                 "it. The engine would have faulted at 14057BD41.\n"
                 "    (4096 is a sanity bound, not a measured maximum — if a legitimate widget ever "
                 "exceeds it this line is the evidence, and the bound is what to change.)\n"
                 "    refusals=%ld",
                 (unsigned long long)node, count, (unsigned long long)arr, n);
        // ★ Here the bad thing is the ARRAY POINTER and the culprit is `node` ITSELF, so `node` is
        // passed as the parent — which is what puts the raw layout dump on the right object.
        TW3K_DIAGNOSTIC(reportB9Culprit("A widget claims children but its child ARRAY pointer is garbage "
                        "(the 14057BD41 fault).", arr, node, candidates, depth));
        return 0;
    }

    // Accepted. Push ourselves so a deeper level can name us as its parent, and pop on the way out.
    t_walkChain[depth] = node;
    ++t_walkDepth;
    uintptr_t r = 0;
    // ★ Our surrounding panel lookup can catch an engine SEH fault. Restore TLS
    // during unwinding too, or its next independent lookup inherits a stale chain.
    __try { r = g_origTreeWalk(node, candidates); }
    __finally { --t_walkDepth; }
    return r;
}

// ★ #12 — the counters, so `capture` says whether the instrument has ever had anything to report.
#ifndef TW3K_RELEASE
void reportTreeWalkGuard()
{
    logf("#12/B9 tree-walk guard (%s): refusals=%ld  culprit reports=%ld",
         g_treeWalkGuard ? "ARMED" : "off", g_treeWalkRefusals, g_b9Reports);
    if (g_treeWalkGuard && g_treeWalkRefusals == 0)
        logf("    nothing refused this run ⇒ every widget-tree walk this session was clean, and the "
             "B9 corruption did not occur. A 4-player join that did NOT crash and shows 0 here is "
             "the control #12 has never had.");
}
#endif


bool installTreeWalkGuard()
{
    return detourInstall(g_treeWalkDetour, g_base + RVA_TREE_WALK, TREE_WALK_STOLEN,
                         EXPECT_TREE_WALK, (uintptr_t)&treeWalkHook,
                         (void**)&g_origTreeWalk, "B9 tree-walk guard");
}

void removeTreeWalkGuard() { detourRemove(g_treeWalkDetour, "B9 tree-walk guard"); }

long treeWalkRefusals() { return g_treeWalkRefusals; }

// ------------------------------------------- slot-changed probe (the suspected real blocker)
//
// FUN_142CE66D0 is the lobby's slot-change handler. Its very first act is:
//
//     142CDE848  CMP EDX, [RCX+0x5C]   ; slotIndex  vs  PANEL COUNT (lobby+0xDC)
//     142CDE84B  JNC 142CDE85D
//     142CDE85D  XOR EDI, EDI          ; "no panel for this slot"
//     ...
//     142CDE91F  TEST RDI,RDI / JZ     ; -> skips the faction-cache update AND FUN_142D7CD10
//
// With two panels and player 3 in slot 2, every update about player 3 is silently discarded. That
// is the leading explanation for the lobby player list stopping at 2 while the session happily
// seats three — which is the actual thing blocking the campaign (the ready check reads
// "nothing blocking", so readiness is NOT the problem).
//
// This probe is READ-ONLY: it logs the slot index and the panel count, then calls the original
// unchanged. If the log shows `slot=2 panelCount=2 -> IGNORED`, the hypothesis is confirmed and the
// fix is to grow the panel vector (which needs the +0xE8 relocation first — see NETCODE_NOTES 5c/5d).
//
// The function takes EIGHT arguments (4 register + 4 stack: [RSP+0x28..0x40] at entry), so the
// typedef must match exactly or the forwarded call corrupts the stack.


typedef void (*SlotChangedFn)(void*, uint32_t, void*, void*, void*, void*, int, int);
Detour        g_slotChDetour;
static SlotChangedFn g_origSlotChanged = nullptr;
static volatile long g_slotChLogs = 0;

// Lets the panel-expansion code invoke the original with all eight arguments after it has snapshotted
// the bytes that call is about to clobber.
struct SlotChArgs { void* self; uint32_t slot; void* a3; void* a4; void* a5; void* a6; int a7; int a8; };
static void runOrigSlotChanged(void* ctx)
{
    SlotChArgs* p = (SlotChArgs*)ctx;
    if (g_origSlotChanged) g_origSlotChanged(p->self, p->slot, p->a3, p->a4, p->a5, p->a6, p->a7, p->a8);
}

static void slotChangedHook(void* self, uint32_t slotIdx, void* a3, void* a4,
                            void* a5, void* a6, int a7, int a8)
{
    // A nested UI notification must not rebuild/free the record held by the
    // outer extra-panel handler, even before its own re-entry brake is reached.
    if (slotIdx >= 2 && g_inExtraHandler) return;
#ifndef TW3K_RELEASE
    if (self && slotIdx >= 2 && slotIdx < 4) {
        static volatile long changes = 0;
        if (InterlockedIncrement(&changes) <= 96) {
            char accepted[128] = {}, previous[128] = {}, party[128] = {}, oldParty[128] = {};
            readCaString(a3, accepted, sizeof(accepted)); readCaString(a4, previous, sizeof(previous));
            readCaString(a5, party, sizeof(party)); readCaString(a6, oldParty, sizeof(oldParty));
            logf("S21 faction acknowledgement: id=%u accepted=\"%s\" previous=\"%s\" party=\"%s\" previousParty=\"%s\" group=%d previousGroup=%d",
                 slotIdx, accepted, previous, party, oldParty, a7, a8);
        }
    }
#endif
    // S17: restore the record-only half of the stock notification before the
    // replacement skips its unsafe inline cache. Use accepted service state.
    if (self && slotIdx >= 2 && slotIdx < 4)
        tickFreshLobbyFactions((uintptr_t)self - 0x80, true);
#ifndef TW3K_RELEASE
    if (self && InterlockedIncrement(&g_slotChLogs) <= 48) {
        uint32_t panelCount = 0;
        void*    panel      = nullptr;
        uint64_t panelsPtr  = 0;
        readAt((uintptr_t)self + OFF_PANEL_COUNT, panelCount);
        if (readAt((uintptr_t)self + 0x60, panelsPtr) && panelsPtr && slotIdx < panelCount)
            readAt((uintptr_t)panelsPtr + (uintptr_t)slotIdx * 8, panel);

        const bool ignored = (slotIdx >= panelCount) || !panel;

        // Run 5 showed this callback fires for slots 0 and 1 ONLY — never for slot 2, even though
        // player 3 is seated there. So the panel-count check inside is NOT what drops player 3; the
        // callback is simply never invoked for that slot, and the decision is made upstream by
        // whoever notifies us. This is a virtual (slot +0x50 of the interface at lobby+0x80, vtable
        // 0x1437DC0A8) with no static callers, so the notifier cannot be found by xref.
        //
        // The detour is entered by JMP with the stack untouched, so the return address here IS the
        // notifier's. Logging it as an RVA points straight at the code that decides which slots get
        // reported — which is the thing actually capping the lobby at 2 players.
        const uintptr_t ret = (uintptr_t)_ReturnAddress();
        logf("slot-changed: slot=%u panelCount=%u panel=%016llX caller=RVA_%08llX -> %s",
             slotIdx, panelCount, (unsigned long long)panel,
             (unsigned long long)(ret >= g_base ? ret - g_base : ret),
             ignored ? "IGNORED (no panel for this slot)" : "processed");
    }

#endif
    // Slots 0/1 keep stock behaviour. Slots 2+ are handled with OUR cache so the engine's 2-entry
    // array at lobby+0xE8 is never indexed past entry 1 — running the original for those would write
    // straight over the UI component pointers at +0x110.
    // a3 = new faction key, a5 = new political-party key, a7 = faction-group index. FUN_142D6DA20
    // needs all three (party is appended to the faction as "%s:%s", the index becomes the group
    // dropdown's text), which is why they are forwarded rather than just the faction.
    SlotChArgs oargs = { self, slotIdx, a3, a4, a5, a6, a7, a8 };
    if (self && handleExtraSlotChange((uintptr_t)self, slotIdx, a3, a5, a7, &runOrigSlotChanged, &oargs))
        return;

    // Never fall back to the engine's two-entry inline cache for extra ids,
    // including when a shrinking/reset lobby has lost its panel/cache readiness.
    if (slotIdx >= 2) return;

    if (g_origSlotChanged) g_origSlotChanged(self, slotIdx, a3, a4, a5, a6, a7, a8);
}


bool installSlotChangedHook()
{
    return detourInstall(g_slotChDetour, g_base + RVA_SLOT_CHANGED, SLOTCH_STOLEN_LEN,
                         EXPECT_SLOT_CHANGED, (uintptr_t)&slotChangedHook,
                         (void**)&g_origSlotChanged, "slot-changed probe");
}

void removeSlotChangedHook() { detourRemove(g_slotChDetour, "slot-changed probe"); }
