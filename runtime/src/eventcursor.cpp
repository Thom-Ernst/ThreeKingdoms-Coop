// eventcursor.cpp — #63: give the 3rd and 4th human somewhere to keep their event-log cursor.
//
// ============================================================================================
// WHAT THIS IS FOR
//
// `mgr = *(model + 0x3D30)` keeps the campaign's HUMAN_FACTIONS list at {+0x228 cap, +0x22C count,
// +0x230 data} and, in parallel, one event-log watermark per registered faction at `+0x1F0`.
//
// ⚠⚠ THAT WATERMARK ARRAY IS `u32[2]`, AND `+0x1F8` IS A LIVE FIELD. Proven three ways (§6uuu.22,
// confirmed live 2026-08-13): the game's own saver writes EIGHT bytes with an element count of 2,
// range [+0x1F0, +0x1F8); the destructor zeroes `*(u32*)(mgr+0x1F8)` right after emptying the
// intrusive list at `+0x200`, the same size-at-head−8 idiom it uses for the list at `+0x268`; and
// in a fresh single-player turn 1 BOTH cursor slots read 0 while `+0x1F8` reads 235 — a value that
// has now been the same in four unrelated campaign states, which is not what a cursor does.
//
// And the writer has no bounds check at all:
//
//     1414976D9   MOV dword ptr [RBX + RCX*0x4 + 0x1F0],EAX      ; FUN_14149F350, end-turn path
//
// ⇒ The moment `humanFactionCount` becomes 3, the third human's every end turn writes their cursor
// over the list size, silently, on a client that is lockstep checksum-compared. That is why #63's
// "just stop clamping the count" is not shippable on its own, and it is what this file fixes.
//
// ============================================================================================
// THE SHAPE, AND WHY IT IS THIS ONE
//
// The three instructions that touch the array are 7-8 bytes each — too short for the 14-byte
// register-free `JMP [rip+0]` this project requires (`MOV RAX,imm64; JMP RAX` cost two crashed runs
// when the stolen prologue's body still wanted RAX). Mid-function surgery would mean hand-written
// stubs replicating prologue arithmetic, three times.
//
// ★ So we do not touch them. Both functions have ordinary 14+ byte PROLOGUES, so we hook those and
// let the engine keep using the slot it thinks it has — then put the field back:
//
//   the WRITE (FUN_14149F350)   save `+0x1F8`, call the original, harvest what it just wrote as
//                               this faction's cursor, restore `+0x1F8`
//   the READS (FUN_1414E0E40)   write our stored cursor INTO the slot, call the original, restore
//
// Same trick both directions, no new machine code, and it uses the detour idiom that has survived
// Denuvo nine times on this project.
//
// ============================================================================================
// ⚠ THE SAFETY PROPERTIES, because they are what make this shippable now
//
//  1. **Provably inert below 3 humans.** The index comes from a search bounded by
//     `humanFactionCount`; at count 2 it can only be 0 or 1, so every branch here is skipped and
//     the gate byte's `< 2` and `< 4` are the same test. This can ship, and be exercised by the
//     same session that tests everything else, without a separate risk window.
//  2. **Bounded at FOUR.** Slots 2 and 3 land on `+0x1F8` (list size) and `+0x1FC` (padding), both
//     of which we save and restore. Slot 4 would land on the POINTER at `+0x200`, so index >= 4 is
//     refused outright rather than repaired — see the log line, and raise this deliberately if the
//     project ever goes past four humans.
//  3. **Nothing here raises the count.** That is still the unlocated writer's job (§6uuu.23).
//  4. The save file carries two cursors. A third human's watermark therefore does NOT persist —
//     after a load they re-scan the event log from zero; the
//     exclusion list ("factions that have already had this event") is what actually dedups. That is
//     a known, deliberate behaviour difference, not an oversight.
// ============================================================================================

#include "tw3k.h"
#include "offsets.h"

// ------------------------------------------------------------------ sites, verified byte for byte

// FUN_14149F350 — the unbounded cursor WRITE. 17 bytes, 5 whole instructions, none RIP-relative.
//   40 53           PUSH RBX          (note the redundant REX prefix — it is really 2 bytes)
//   56              PUSH RSI
//   48 83 EC 38     SUB RSP,0x38
//   48 8B B1 30 02 00 00   MOV RSI,[RCX+0x230]
//   48 8B D9        MOV RBX,RCX
static constexpr uintptr_t RVA_CURSOR_WRITE  = 0x0149F350;
static constexpr size_t    CURSOR_WRITE_LEN  = 17;
static const uint8_t EXPECT_CURSOR_WRITE[CURSOR_WRITE_LEN] = {
    0x40,0x53, 0x56, 0x48,0x83,0xEC,0x38, 0x48,0x8B,0xB1,0x30,0x02,0x00,0x00, 0x48,0x8B,0xD9
};

// FUN_1414E0E40 — collect this faction's events; contains both cursor READS and the `< 2` gate.
// 15 bytes, 3 whole instructions, none RIP-relative. They only spill RBX/R9/RDX to the caller's
// home slots, so the trampoline replays them with nothing left dangling.
static constexpr uintptr_t RVA_COLLECT       = 0x014E0E40;
static constexpr size_t    COLLECT_LEN       = 15;
static const uint8_t EXPECT_COLLECT[COLLECT_LEN] = {
    0x48,0x89,0x5C,0x24,0x08, 0x4C,0x89,0x4C,0x24,0x20, 0x48,0x89,0x54,0x24,0x10
};

// The index gate inside FUN_1414E0E40. We verify the whole 16-byte run and rewrite ONE byte.
//   48 2B C2              SUB RAX,RDX
//   48 C1 F8 03           SAR RAX,0x3          -> the registry index
//   83 F8 02              CMP EAX,0x2          <- byte [9] is the 2
//   0F 83 E5 01 00 00     JNC <empty return>
static constexpr uintptr_t RVA_INDEX_GATE    = 0x014E0F02;
static constexpr size_t    INDEX_GATE_LEN    = 16;
static constexpr size_t    INDEX_GATE_IMM    = 9;     // offset of the literal inside that run
static const uint8_t EXPECT_INDEX_GATE[INDEX_GATE_LEN] = {
    0x48,0x2B,0xC2, 0x48,0xC1,0xF8,0x03, 0x83,0xF8,0x02, 0x0F,0x83,0xE5,0x01,0x00,0x00
};

// ------------------------------------------------------------------ the SEARCH BOUND (2026-08-15)
//
// The other half of `FUN_1414E0E40`, 96 bytes earlier, and the reason the count hold has to be
// re-armed for ever. The function looks the faction up in the registry before it ever reaches the
// index gate above, and it bounds that search by `humanFactionCount`:
//
//     1414D8FE8  48 8B 96 30 02 00 00   MOV RDX,[RSI+0x230]    ; data
//     1414D8FEF  8B 86 2C 02 00 00      MOV EAX,[RSI+0x22C]    ; count   <- byte [9] is the 0x2C
//     1414D8FF5  4C 8B F2               MOV R14,RDX
//     ...
//     1414D900C  LEA RCX,[RDX+RAX*8]                           ; end = data + count*8
//
// ⇒ Repointing that ONE displacement to +0x228 makes the search run to `humanFactionCap` instead,
// so a human registered past the count is FOUND. `cap` is the allocation size and is always >= the
// number of pushed entries, so nothing valid is ever excluded.
//
// ★ WHY THIS IS WORTH A BYTE: it writes no replicated model state at all. The count hold does, which
// is what forces `hfcount` to be armed on every machine before every start and every load, to die
// with the process, and to carry an every-machine-or-none rule. A code patch applied at attach is
// identical on every client by construction and invisible to a lockstep checksum.
//
// ⚠ IT READS THE UNINITIALISED TAIL. Slots [pushed, cap) are heap garbage — with three humans and
// cap 4 that is slot [3]. The loop only COMPARES the slot value against the faction it is looking
// for (`CMP qword ptr [R14],R15`) and never dereferences it, so this is a read of uninitialised
// memory, not a deref; a false match would need garbage to equal a live faction pointer exactly.
// That is the whole of the risk, and it is why this ships disarmed until a 4-human capture exists.
//
// ⚠⚠ IT DOES NOT FIX STORAGE. If the push really does write at data[count] (inferred, §6uuu.27; the
// push is unlocated), a fourth human overwrites the third and no reader-side change can recover a
// faction that was never stored. This widens what can be FOUND, nothing more.
static constexpr uintptr_t RVA_SEARCH_BOUND  = 0x014E0EA8;
static constexpr size_t    SEARCH_BOUND_LEN  = 16;
static constexpr size_t    SEARCH_BOUND_IMM  = 9;     // the 0x2C displacement byte
static constexpr uint8_t   SEARCH_BOUND_CAP  = 0x28;  // +0x228 = humanFactionCap
static const uint8_t EXPECT_SEARCH_BOUND[SEARCH_BOUND_LEN] = {
    0x48,0x8B,0x96,0x30,0x02,0x00,0x00, 0x8B,0x86,0x2C,0x02,0x00,0x00, 0x4C,0x8B,0xF2
};

// How many cursor slots we are prepared to serve. 2 are the engine's own; 3 and 4 are ours, and
// they live on +0x1F8/+0x1FC which we save and restore around each call. See safety note 2.
static constexpr uint32_t CURSOR_SLOTS_MAX = 4;
// The bytes we borrow: +0x1F8 (list size) and +0x1FC (padding). Never the pointer at +0x200.
static constexpr size_t   BORROW_OFF = OFF_EVMGR_LIST_SIZE;
static constexpr size_t   BORROW_LEN = 8;

// ------------------------------------------------------------------ the side table

// Keyed by event-manager ownership AND FACTION pointer, not by index: the index belongs to the
// registry's current ordering and this project has a standing rule against addressing a player by position
// (`tw-no-positional-player-ids`). Eight entries is twice the ceiling this file will serve.
static uintptr_t resolveEventMgr(uintptr_t* outModel);   // defined with the count hold, below

struct SideCursor { uintptr_t manager; uintptr_t faction; uint32_t cursor; bool used; };
static long g_sideResets = 0;
static long g_sideFull = 0;
static long g_sideOutOfRange = 0;
static SideCursor g_side[8]     = {};
static long g_repairs           = 0;   // writes harvested and undone
static long g_supplied          = 0;   // reads served from the side table
static long g_refusedTooHigh    = 0;   // index >= CURSOR_SLOTS_MAX — see safety note 2
static bool g_gatePatched       = false;
static uint8_t g_gateOrigByte   = 0x02;
static bool g_boundPatched      = false;   // the search bound — ships DISARMED, see the note above
static long g_foundPastCount    = 0;   // factions found ONLY because the bound was widened
static uint8_t g_boundOrigByte  = 0x2C;

static SideCursor* sideFind(uintptr_t mgr, uintptr_t faction)
{
    for (auto& s : g_side) if (s.used && s.manager == mgr && s.faction == faction) return &s;
    return nullptr;
}
static SideCursor* sideFindOrAdd(uintptr_t mgr, uintptr_t faction)
{
    if (SideCursor* s = sideFind(mgr, faction)) return s;
    for (auto& s : g_side) if (!s.used) { s.used = true; s.manager = mgr; s.faction = faction; s.cursor = 0; return &s; }
    return nullptr;
}

// These hooks share the existing campaign-thread assumption of the borrowed cursor window.
// The offline fixture validates lifetime behavior; live load-thread ordering still needs a run. Never keep a SideCursor pointer across an engine call. mgr==0 invalidates all
// entries while HUMAN_FACTIONS is deserialized; destruction invalidates only that owner.
static void resetSideCursors(uintptr_t mgr)
{
    unsigned cleared = 0;
    for (auto& s : g_side) if (s.used && (!mgr || s.manager == mgr)) {
        s = {};
        ++cleared;
    }
    ++g_sideResets;
    diagLogf("EVENT CURSOR lifetime: boundary #%ld (%s), manager=%016llX, cleared=%u, "
         "writes=%ld reads=%ld full=%ld out-of-range=%ld",
         g_sideResets, mgr ? "manager destruction" : "HUMAN_FACTIONS load",
         (unsigned long long)mgr, cleared, g_repairs, g_supplied, g_sideFull, g_sideOutOfRange);
}

// FUN_14198BAE0 is the verified event-manager destructor (ends by freeing mgr, size 0x298).
// Leave TEST RDX,RDX / JZ +0x1A5 in place. At +9 the stack is still at ABI entry and all
// arguments are intact. These four stolen instructions have no relative operands/branches:
// PUSH RDI; SUB RSP,40; MOV RCX,[RDX+288]; MOV RDI,RDX. No interior branch targets.
static constexpr uintptr_t RVA_EVENT_MGR_DTOR_BODY = 0x019953B9;
static const uint8_t EXPECT_EVENT_MGR_DTOR_BODY[] = {
    0x57, 0x48,0x83,0xEC,0x40, 0x48,0x8B,0x8A,0x88,0x02,0x00,0x00, 0x48,0x8B,0xFA
};
static Detour g_eventMgrDtorDetour;
using EventMgrDtorFn = void (*)(uintptr_t, uintptr_t);
static EventMgrDtorFn g_origEventMgrDtor = nullptr;
static void eventMgrDtorHook(uintptr_t arg, uintptr_t mgr)
{
    resetSideCursors(mgr); // before the allocator can hand this address to a new campaign
    g_origEventMgrDtor(arg, mgr);
}

static bool installEventMgrLifetimeHook()
{
    static const uint8_t prefix[] = {0x48,0x85,0xD2,0x0F,0x84,0x9C,0x01,0x00,0x00};
    uint8_t cur[sizeof(prefix)] = {};
    if (!safeRead((void*)(g_base + RVA_EVENT_MGR_DTOR_BODY - sizeof(prefix)), cur, sizeof(cur)) ||
        memcmp(cur, prefix, sizeof(cur)) != 0) {
        logf("event-manager lifetime: null-check signature mismatch — NOT installed");
        return false;
    }
    return detourInstall(g_eventMgrDtorDetour, g_base + RVA_EVENT_MGR_DTOR_BODY,
                         sizeof(EXPECT_EVENT_MGR_DTOR_BODY), EXPECT_EVENT_MGR_DTOR_BODY,
                         (uintptr_t)&eventMgrDtorHook, (void**)&g_origEventMgrDtor,
                         "event-manager lifetime");
}

// A guarded write. The targets here are ordinary heap the game already writes to, so no
// VirtualProtect is needed — but a bad pointer must not take the game down with it.
static bool safeWriteBytes(uintptr_t addr, const void* src, size_t len)
{
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    __try { memcpy((void*)addr, src, len); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The registry search, reproduced exactly as both hooked functions do it: linear over
// [data, data+count), comparing the faction POINTER. Returns the index, or -1.
// ⚠⚠ THIS MUST TRACK THE ENGINE'S OWN SEARCH BOUND, and getting it wrong is not a cosmetic bug.
// `FUN_1414E0E40` searches [0, count) normally and [0, cap) once `hfcount bound` has repointed its
// displacement. If this stayed on `count` while the engine ran to `cap`, then for a faction sitting
// past the count the engine would FIND it, we would return -1, `collectHook` would pass straight
// through without serving a cursor, and the engine would read `(&mgr->eventCursor0)[idx]` for idx 2
// or 3 — off the end of the two-slot array. That is precisely the corruption the side table exists
// to prevent, reintroduced by a reader-side fix. Caught before it ever ran; ⇒ the two must move
// together, which is why this reads the same flag the patch sets.
static int registryIndexOf(uintptr_t mgr, uintptr_t faction)
{
    uint32_t count = 0, cap = 0; uintptr_t data = 0;
    if (!mgr || !faction) return -1;
    if (!readAt(mgr + OFF_EVMGR_HF_COUNT, count)) return -1;
    if (!readAt(mgr + OFF_EVMGR_HF_PTR, data) || !data) return -1;

    uint32_t bound = count;
    if (g_boundPatched) {
        // cap < count cannot be true for a {cap,count,ptr} vector; if it reads that way the
        // offsets are wrong, so fall back to the narrow bound rather than walk off something.
        if (readAt(mgr + OFF_EVMGR_HF_CAP, cap) && cap >= count) bound = cap;
    }
    if (bound > 64) return -1;                       // cannot be true; say nothing, do nothing
    for (uint32_t i = 0; i < bound; ++i) {
        uintptr_t f = 0;
        if (!readAt(data + (uintptr_t)i * 8, f)) return -1;
        if (f == faction) {
            // ★ The proof-of-work metric. A hit at or past `count` is one the vanilla search would
            // have missed, so this counter is what separates "the bound patch did the job" from
            // "the count hold did it and the patch was along for the ride".
            if (i >= count) InterlockedIncrement(&g_foundPastCount);
            return (int)i;
        }
    }
    return -1;
}

// ------------------------------------------------------------------ hook 1: the unbounded write

typedef void (*CursorWriteFn)(uintptr_t mgr, uintptr_t faction);
static CursorWriteFn g_origCursorWrite = nullptr;
static Detour        g_cursorWriteDetour;

// `latest event index` — turnIdx[turnIdxCount-1], which is exactly the value FUN_14149F350 writes
// into a faction's cursor when it finishes. We write it ourselves when we skip that call.
static bool latestEventIndex(uintptr_t mgr, uint32_t& out)
{
    uint32_t n = 0; uintptr_t p = 0;
    if (!readAt(mgr + OFF_EVMGR_TURNIDX_CNT, n) || !readAt(mgr + OFF_EVMGR_TURNIDX_PTR, p)) return false;
    if (!p || n == 0 || n > 100000) return false;
    return readAt(p + (uintptr_t)(n - 1) * 4, out);
}

// ❌❌ REWRITTEN 2026-08-13 AFTER A LIVE FAILURE, and the correction is the important part.
//
// The first version let the original run and then repaired `+0x1F8`. That protected the list size
// and nothing else, because I had read this function as "the cursor writer" when it is a **garbage
// collector that happens to end with a cursor write**. What it actually spends its time doing is
// calling `FUN_141497AB0(mgr, start, len)` — a memmove-based **ERASE** that shifts the chunked event
// log down over `[start, start+len)`, shrinks the total at `+0x30` and frees a trailing chunk.
//
// ⚠⚠ And `FUN_14149F350` compensates only the turn-index array (`+0x1A8[n-2]`, `[n-1]`) and `+0x25C`
// for that shift. **It never adjusts any OTHER faction's cursor**, which is an index into the same
// log. So enrolling a third faction adds a third destructive pass per round over a log that seats 0
// and 1 still hold indices into.
//
// LIVE, 2026-08-13: with count raised to 3, Cao Cao — **seat 0**, the seat that has worked in every
// session this project has run — lost a dilemma box while owning the decision. That has never
// happened before, and this is the mechanism that best explains it.
//
// ⇒ So do not repair the compaction. **Do not run it.** For any faction past the engine's own two
// slots we skip the original entirely and set the cursor ourselves. The cost is that the event log
// is not pruned on that faction's behalf — growth, not correctness — and it buys three things at
// once: no extra deletion, no write to `+0x1F8` at all, and no window between the engine's write and
// our restore. It is the same reasoning already applied to index >= 4; failing to apply it to index
// >= 2 is what cost a session.
static void cursorWriteHook(uintptr_t mgr, uintptr_t faction)
{
    const int idx = registryIndexOf(mgr, faction);

    // The engine's own slots, untouched — and the only path that exists below three humans.
    if (idx < 0 || idx < (int)EVMGR_CURSOR_SLOTS) { g_origCursorWrite(mgr, faction); return; }

    if (idx >= (int)CURSOR_SLOTS_MAX) {
        if (++g_refusedTooHigh <= 3)
            diagLogf("EVENT CURSOR: registry index %d is past the %u slots this build serves — skipping "
                 "the compaction for faction %016llX. Raise CURSOR_SLOTS_MAX deliberately if this "
                 "project ever goes past four humans.",
                 idx, CURSOR_SLOTS_MAX, (unsigned long long)faction);
        return;
    }

    // Skip the engine's compaction for this faction and keep its watermark ourselves.
    uint32_t latest = 0;
    const bool ok = latestEventIndex(mgr, latest);
    bool stored = false;
    if (ok) {
        if (SideCursor* s = sideFindOrAdd(mgr, faction)) { s->cursor = latest; stored = true; }
        else if (++g_sideFull <= 5)
            logf("EVENT CURSOR: side table full; manager=%016llX faction=%016llX. "
                 "Collection will rescan from zero.", (unsigned long long)mgr, (unsigned long long)faction);
    }

    if (++g_repairs <= 5)
        diagLogf("EVENT CURSOR: faction %016llX is registry index %d — SKIPPED the engine's event-log "
             "compaction; stored=%d cursor=%u in the side table. Skipped rather than repaired "
             "because that call ERASES log entries other seats still index into (§6uuu.30).",
             (unsigned long long)faction, idx, stored ? 1 : 0, stored ? latest : 0u);
}

// ------------------------------------------------------------------ hook 2: the two reads

typedef uintptr_t (*CollectFn)(uintptr_t mgr, uintptr_t out, uintptr_t faction, uintptr_t p4);
static CollectFn g_origCollect = nullptr;
static Detour    g_collectDetour;

static uintptr_t collectHook(uintptr_t mgr, uintptr_t out, uintptr_t faction, uintptr_t p4)
{
    const int idx = registryIndexOf(mgr, faction);
    if (idx < 0 || idx < (int)EVMGR_CURSOR_SLOTS || idx >= (int)CURSOR_SLOTS_MAX)
        return g_origCollect(mgr, out, faction, p4);

    uint8_t saved[BORROW_LEN] = { 0 };
    const bool haveSaved = safeRead((void*)(mgr + BORROW_OFF), saved, BORROW_LEN);

    // Serve the cursor we kept. A faction we have never seen write one gets 0, which means "scan
    // the log from the start" — correct, just more work, because the exclusion list is what
    // actually prevents an event being delivered twice.
    const SideCursor* s = sideFind(mgr, faction);
    uint32_t v = s ? s->cursor : 0u;
    // The collector trusts this bound in its first pass. Compaction or an earlier save may
    // shorten the log; never lend a watermark beyond the current logical record count (+0x30).
    uint32_t end = 0;
    if (!readAt(mgr + 0x30, end) || v > end) {
        v = 0;
        ++g_sideOutOfRange;
        if (SideCursor* stale = sideFind(mgr, faction)) stale->cursor = 0;
    }
    const bool hadCursor = s != nullptr;
    const bool served = safeWriteBytes(mgr + OFF_EVMGR_CURSOR0 + (uintptr_t)idx * 4, &v, sizeof(v));

    const uintptr_t r = g_origCollect(mgr, out, faction, p4);

    if (haveSaved) safeWriteBytes(mgr + BORROW_OFF, saved, BORROW_LEN);
    if (served && ++g_supplied <= 5)
        diagLogf("EVENT CURSOR: served faction %016llX (index %d) cursor %u for its event collection%s.",
             (unsigned long long)faction, idx, v, hadCursor ? "" : " — first time, so from the start");
    return r;
}

// ------------------------------------------------------------------ the index gate

// ⚠ USELESS ALONE AND HARMLESS ALONE. It only widens what `FUN_1414E0E40` PERMITS; the walk is
// still bounded by `humanFactionCount`, so below three humans an index of 2 cannot occur and this
// changes nothing observable. It must nevertheless land WITH the hooks above and never without
// them — a widened gate over a two-slot array is exactly the corruption this file exists to stop.
static bool patchIndexGate()
{
    const uintptr_t addr = g_base + RVA_INDEX_GATE;
    uint8_t cur[INDEX_GATE_LEN] = { 0 };
    if (!safeRead((void*)addr, cur, INDEX_GATE_LEN)) {
        logf("event-cursor gate: %016llX unreadable — NOT patched", (unsigned long long)addr);
        return false;
    }
    for (size_t i = 0; i < INDEX_GATE_LEN; ++i) if (cur[i] != EXPECT_INDEX_GATE[i]) {
        logf("event-cursor gate: SIGNATURE MISMATCH at byte %zu (want %02X got %02X) — refusing",
             i, EXPECT_INDEX_GATE[i], cur[i]);
        return false;
    }

    const uintptr_t imm = addr + INDEX_GATE_IMM;
    g_gateOrigByte = cur[INDEX_GATE_IMM];
    const uint8_t want = (uint8_t)CURSOR_SLOTS_MAX;

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)imm, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("event-cursor gate: VirtualProtect failed, err=%lu — NOT patched", GetLastError());
        return false;
    }
    *(volatile uint8_t*)imm = want;
    FlushInstructionCache(GetCurrentProcess(), (void*)imm, 1);
    DWORD tmp = 0; VirtualProtect((void*)imm, 1, oldProtect, &tmp);

    g_gatePatched = true;
    logf("event-cursor gate: RVA %08llX  %02X -> %02X (index limit 2 -> %u). Inert until something "
         "raises humanFactionCount past 2.",
         (unsigned long long)RVA_INDEX_GATE + INDEX_GATE_IMM, g_gateOrigByte, want, CURSOR_SLOTS_MAX);
    return true;
}

static void unpatchIndexGate()
{
    if (!g_gatePatched) return;
    const uintptr_t imm = g_base + RVA_INDEX_GATE + INDEX_GATE_IMM;
    DWORD oldProtect = 0;
    if (VirtualProtect((void*)imm, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *(volatile uint8_t*)imm = g_gateOrigByte;
        FlushInstructionCache(GetCurrentProcess(), (void*)imm, 1);
        DWORD tmp = 0; VirtualProtect((void*)imm, 1, oldProtect, &tmp);
        diagLogf("event-cursor gate: restored to %02X", g_gateOrigByte);
    }
    g_gatePatched = false;
}

// ------------------------------------------------------------------ the search bound

#ifndef TW3K_RELEASE
static bool patchSearchBound()
{
    if (g_boundPatched) return true;
    const uintptr_t addr = g_base + RVA_SEARCH_BOUND;
    uint8_t cur[SEARCH_BOUND_LEN] = { 0 };
    if (!safeRead((void*)addr, cur, SEARCH_BOUND_LEN)) {
        logf("search bound: %016llX unreadable — NOT patched", (unsigned long long)addr);
        return false;
    }
    for (size_t i = 0; i < SEARCH_BOUND_LEN; ++i) if (cur[i] != EXPECT_SEARCH_BOUND[i]) {
        logf("search bound: SIGNATURE MISMATCH at byte %zu (want %02X got %02X) — refusing",
             i, EXPECT_SEARCH_BOUND[i], cur[i]);
        return false;
    }

    const uintptr_t imm = addr + SEARCH_BOUND_IMM;
    g_boundOrigByte = cur[SEARCH_BOUND_IMM];

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)imm, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("search bound: VirtualProtect failed, err=%lu — NOT patched", GetLastError());
        return false;
    }
    *(volatile uint8_t*)imm = SEARCH_BOUND_CAP;
    FlushInstructionCache(GetCurrentProcess(), (void*)imm, 1);
    DWORD tmp = 0; VirtualProtect((void*)imm, 1, oldProtect, &tmp);

    g_boundPatched = true;
    logf("search bound: RVA %08llX  %02X -> %02X (FUN_1414E0E40 now searches the registry to "
         "humanFactionCap instead of humanFactionCount). No model state was written.",
         (unsigned long long)RVA_SEARCH_BOUND + SEARCH_BOUND_IMM, g_boundOrigByte, SEARCH_BOUND_CAP);
    return true;
}
#endif


#ifndef TW3K_RELEASE
static void unpatchSearchBound()
{
    if (!g_boundPatched) return;
    const uintptr_t imm = g_base + RVA_SEARCH_BOUND + SEARCH_BOUND_IMM;
    DWORD oldProtect = 0;
    if (VirtualProtect((void*)imm, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        *(volatile uint8_t*)imm = g_boundOrigByte;
        FlushInstructionCache(GetCurrentProcess(), (void*)imm, 1);
        DWORD tmp = 0; VirtualProtect((void*)imm, 1, oldProtect, &tmp);
        logf("search bound: restored to %02X", g_boundOrigByte);
    }
    g_boundPatched = false;
}
#endif


bool humanFactionSearchBoundArmed() { return g_boundPatched; }

// ⚠ Refuses unless the cursor hooks are live, for the same reason the index gate does: widening
// what the search can FIND is only safe while something is serving cursors for the slots it can now
// reach. Without them a found index of 2 reads past the two-slot array.
#ifndef TW3K_RELEASE
bool setHumanFactionSearchBound(bool on, const char** why)
{
    if (!on) { unpatchSearchBound(); return true; }
    // Same three preconditions the write/read paths test elsewhere in this file. All of them:
    // widening the SEARCH is pointless without the index gate (the find is thrown away at
    // `CMP EAX,2`) and unsafe without the cursor hooks (index 2 would read past the two slots).
    if (!g_cursorWriteDetour.active || !g_collectDetour.active || !g_gatePatched) {
        if (why) *why = "the event-cursor hooks and index gate are not both live, and a widened "
                        "search without them either finds nothing or reads off the end of the "
                        "two-slot cursor array";
        return false;
    }
    if (!patchSearchBound()) {
        if (why) *why = "the site did not match its expected bytes — wrong build, nothing written";
        return false;
    }
    return true;
}
#endif


// =================================================================================================
//  ★★★★★★ #68 — WHY THE THIRD HUMAN IS MISSING AFTER A RELOAD
//
//  Measured 2026-08-14 on three clients at once: a coop save whose registry held THREE humans while
//  live comes back holding TWO, and faction 5 — the third seat — is the one that does not return.
//  Identical on host and both guests, so it is replicated state, not a client-local glitch.
//
//  `FUN_1414D0780` = `CampaignSave_ReadHumanFactionsBlock(vector, campaign, stream)` **caps nothing**
//  (its own plate comment: reserves the stream count, otherwise doubles). But it drops silently:
//
//      uVar2 = FUN_141457760(*(model+0x3C18) + 0x20, &savedId);   // resolve id -> faction*
//      humanFactions->count = uVar5 + 1;
//      if (data[count - 1] == 0) { uVar6 = uVar5; }                // NULL -> roll the count BACK
//      humanFactions->count = uVar6;
//
//  ⇒ Any id that fails to resolve is un-appended with **no log and no error**, and the count simply
//  comes back short — exactly the symptom. Two candidates remain and they need different fixes:
//
//      the stream held 2 ids            -> the WRITER (FUN_1414D3C50) is the culprit
//      the stream held 3, one gave NULL -> a LOAD-ORDER bug: the id is saved but the lookup table at
//                                          model+0x3C18 -> +0x20 cannot resolve it yet
//
//  ⚠ The writer could not be read: it timed out in Ghidra at 280 s, and the save stream is compressed
//  (no literal `HUMAN_FACTIONS` in any of 89 save files), so neither route closed at the desk.
//
//  ★ THIS HOOK ANSWERS IT IN ONE RELOAD. It sits on the resolver and **counts the calls made from the
//  loader**: that count IS the number of ids in the stream, and each result says whether that id
//  survived. One line per entry; also invalidates mod-owned cursor and gift state on load.
//  It does not write engine state.
//
//  ⚠ FILTERED BY RETURN ADDRESS. `FUN_141457760` is a shared id->pointer lookup with many callers;
//  only the call at `1414D090B` (returning to `1414D0910`) is this question. Same technique as
//  `factionInListHook` and `feedGateHook`.
// =================================================================================================

static constexpr uintptr_t RVA_FACTION_BY_ID   = 0x01457760;   // FUN_141457760(table, &id) -> faction*
static constexpr uintptr_t RVA_HF_RESOLVE_RET  = 0x014D87D0;   // the CALL's return address in the loader
static constexpr size_t    FACTION_BY_ID_LEN   = 14;

// 14 bytes = 4 whole instructions, ending exactly on the `TEST R11D,R11D` boundary. None is
// RIP-relative and none is a branch; the function's own jump targets are 0x…8C0/8E0/8F0/90E/916, all
// past the stolen range, so nothing can land inside it. Read out of a process image with
// `twdump.py d base+0x144F890`, NOT transcribed from a listing — the `40 57` lesson from earlier today.
static const uint8_t EXPECT_FACTION_BY_ID[FACTION_BY_ID_LEN] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,   // MOV [RSP+8],RBX
    0x44, 0x8B, 0x1A,               // MOV R11D,[RDX]
    0x4C, 0x8B, 0xD1,               // MOV R10,RCX
    0x45, 0x85, 0xDB,               // TEST R11D,R11D
};

typedef void* (*FactionByIdFn)(void* table, uint32_t* pId);

static Detour        g_hfResolveDetour;
static FactionByIdFn g_origFactionById = nullptr;

static volatile long g_hfrSeen = 0;   // ids the loader read out of the HUMAN_FACTIONS stream
static volatile long g_hfrNull = 0;   // ...that resolved to nothing and were therefore dropped

static void* factionByIdHook(void* table, uint32_t* pId)
{
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    void* const     r   = g_origFactionById ? g_origFactionById(table, pId) : nullptr;

    // Every other caller of this leaf is a different question. Answer them and say nothing.
    if (ret != g_base + RVA_HF_RESOLVE_RET) return r;

    // This exact caller deserializes HUMAN_FACTIONS, including in-place/earlier-save loads.
    // Each entry is a load notification, not a separate campaign generation. Clearing more
    // than once within deserialization is harmless; no loaded cursor exists in the side table.
    resetSideCursors(0);
    resetGiftPanelState("campaign load");

#ifndef TW3K_RELEASE
    const long n  = InterlockedIncrement(&g_hfrSeen);
    uint32_t   id = 0xFFFFFFFFu;
    __try { if (pId) id = *pId; } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0xFFFFFFFFu; }

    if (r) {
        logf("HUMAN_FACTIONS LOAD: [%ld] saved id=%u -> faction %016llX  ok",
             n, id, (unsigned long long)r);
    } else {
        InterlockedIncrement(&g_hfrNull);
        logf("HUMAN_FACTIONS LOAD: [%ld] saved id=%u -> ★★★ UNRESOLVED. The loader rolls the count "
             "back, so this human is SILENTLY DROPPED from the registry and the campaign can never "
             "reach them — #68, caught in the act.", n, id);
    }
#endif
    return r;
}

bool installHumanFactionLoadHook()
{
    return detourInstall(g_hfResolveDetour, g_base + RVA_FACTION_BY_ID, FACTION_BY_ID_LEN,
                         EXPECT_FACTION_BY_ID, (uintptr_t)&factionByIdHook,
                         (void**)&g_origFactionById, "HUMAN_FACTIONS load resolver");
}

void removeHumanFactionLoadHook()
{
    detourRemove(g_hfResolveDetour, "HUMAN_FACTIONS load resolver");
}

#ifndef TW3K_RELEASE
void reportHumanFactionLoad()
{
    logf("---- HUMAN_FACTIONS LOAD (#68: what did the save actually contain?) ----");
    if (!g_hfResolveDetour.active) { logf("  hook NOT installed on this machine."); return; }
    logf("  ids read from the stream=%ld  |  unresolved and dropped=%ld", g_hfrSeen, g_hfrNull);

    if (g_hfrSeen == 0) {
        logf("  ⇒ no campaign has been LOADED since this hook was installed. It fires only on the "
             "load path, so a fresh campaign shows nothing here — that is correct, not a fault.");
    } else if (g_hfrNull == 0 && g_hfrSeen < 3) {
        logf("  ⇒ ★★★ the stream held only %ld id(s) and every one resolved. Nothing was dropped at "
             "load ⇒ **the WRITER did not persist the third human**, and FUN_1414D3C50 is the target.",
             g_hfrSeen);
    } else if (g_hfrNull > 0) {
        logf("  ⇒ ★★★ %ld id(s) were written but did NOT resolve at load ⇒ **a LOAD-ORDER bug**: the "
             "save is complete and the lookup table at model+0x3C18 -> +0x20 cannot yet answer for "
             "them. The fix belongs at load, not at save.", g_hfrNull);
    } else {
        logf("  ⇒ the stream held %ld id(s) and all resolved. If the registry is still short, the "
             "loss is DOWNSTREAM of this function and both #68 candidates are wrong.", g_hfrSeen);
    }
}
#endif


// ------------------------------------------------------------------ install / remove / report

bool installEventCursorHooks()
{
    // Storage lifetime must be installed before widening collection. No pointer-only fallback.
    if (!installEventMgrLifetimeHook() || !installHumanFactionLoadHook()) return false;
    // Order matters: storage first, then the permission. If the second fails we are left with two
    // hooks that cannot fire, which is exactly the state the machine was already in.
    const bool okWrite = detourInstall(g_cursorWriteDetour, g_base + RVA_CURSOR_WRITE,
                                       CURSOR_WRITE_LEN, EXPECT_CURSOR_WRITE,
                                       (uintptr_t)&cursorWriteHook, (void**)&g_origCursorWrite,
                                       "event-cursor write");
    if (!okWrite) return false;

    const bool okRead = detourInstall(g_collectDetour, g_base + RVA_COLLECT, COLLECT_LEN,
                                      EXPECT_COLLECT, (uintptr_t)&collectHook,
                                      (void**)&g_origCollect, "event-cursor read");
    if (!okRead) { detourRemove(g_cursorWriteDetour, "event-cursor write"); return false; }

    if (!patchIndexGate()) {
        detourRemove(g_collectDetour, "event-cursor read");
        detourRemove(g_cursorWriteDetour, "event-cursor write");
        return false;
    }
    return true;
}

void removeEventCursorHooks()
{
    // ⚠ Order matters: put the search bound back BEFORE the gate and the hooks go, or there is a
    // window where the search still runs to `cap` with nothing serving the cursors it can reach.
#ifndef TW3K_RELEASE
    unpatchSearchBound();
#endif
    unpatchIndexGate();
    detourRemove(g_collectDetour, "event-cursor read");
    detourRemove(g_cursorWriteDetour, "event-cursor write");
    detourRemove(g_eventMgrDtorDetour, "event-manager lifetime");
}

#ifndef TW3K_RELEASE
void reportEventCursors()
{
    unsigned sideUsed = 0;
    for (const auto& slot : g_side) if (slot.used) ++sideUsed;
    logf("EVENT CURSOR lifetime: destructor=%s load=%s boundaries=%ld entries=%u/8 full=%ld "
         "out-of-range=%ld", g_eventMgrDtorDetour.active ? "LIVE" : "MISSING",
         g_hfResolveDetour.active ? "LIVE" : "MISSING", g_sideResets, sideUsed,
         g_sideFull, g_sideOutOfRange);

    logf("---- EVENT CURSORS (#63: storage for the 3rd and 4th human) ----");
    if (!g_cursorWriteDetour.active || !g_collectDetour.active) {
        logf("  NOT installed on this machine — a 3rd human here would corrupt +0x1F8 on every end "
             "turn. Do not run a 3-player campaign against this build.");
        return;
    }
    logf("  hooks live, gate %s, slots served: %u (2 the engine's, %u ours)",
         g_gatePatched ? "patched to 4" : "NOT patched", CURSOR_SLOTS_MAX,
         CURSOR_SLOTS_MAX - EVMGR_CURSOR_SLOTS);
    logf("  compactions skipped=%ld  reads served=%ld  refused (index >= %u)=%ld",
         g_repairs, g_supplied, CURSOR_SLOTS_MAX, g_refusedTooHigh);
    logf("  search bound: %s  |  factions found ONLY past the count=%ld",
         g_boundPatched ? "WIDENED to cap" : "engine's own (count)", g_foundPastCount);
    if (g_boundPatched)
        logf("    ⇒ that last number is the proof-of-work: it counts lookups the vanilla search "
             "would have MISSED. Zero of them while a 3rd/4th human is live means the bound patch "
             "is not what is carrying the fix — the count hold is.");
    // ⚠ Say what the count IS, never what it is assumed to be. The previous wording asserted
    // "humanFactionCount is 2" and kept saying it after the hold had raised it to 3 — a correct
    // number with a false explanation attached, which is the most expensive kind of log line.
    if (g_repairs == 0 && g_supplied == 0) {
        uintptr_t model = 0; uint32_t cnt = 0;
        const uintptr_t mgr = resolveEventMgr(&model);
        const bool okCnt = mgr && readAt(mgr + OFF_EVMGR_HF_COUNT, cnt);
        if (!okCnt)
            logf("  ⇒ nothing has fired: not in a campaign, so there is nothing for it to do yet.");
        else if (cnt <= EVMGR_CURSOR_SLOTS)
            logf("  ⇒ nothing has fired, and humanFactionCount is %u — at or below the engine's own "
                 "two slots, so the registry index cannot reach 2. Inert, not broken.", cnt);
        else {
            // ⚠ FALSE POSITIVE FIXED 2026-08-15, caught in the first 4-human capture. This used to
            // say "read this as a problem" whenever the count was past 2 and nothing had fired —
            // but collection runs for the LOCAL player's own feed only (§6uuu.30), so a machine
            // whose own faction sits at registry index 0 or 1 uses the ENGINE's cursor slots and is
            // supposed to never touch the side table. It reported a healthy client as broken.
            uintptr_t root = 0, obj = 0, mine = 0;
            int myIdx = -1;
            if (readAt(g_base + RVA_CAMPAIGN_ROOT, root) && root &&
                readAt(root + OFF_ROOT_OBJ, obj) && obj &&
                readAt(obj + OFF_LOCAL_FACTION, mine) && mine)
                myIdx = registryIndexOf(mgr, mine);

            if (myIdx >= 0 && myIdx < (int)EVMGR_CURSOR_SLOTS)
                logf("  ⇒ nothing has fired, and that is CORRECT here: humanFactionCount is %u, but "
                     "this machine's own faction is registry index %d, which uses the engine's own "
                     "cursor slot. The side table is for indices %u and up.",
                     cnt, myIdx, EVMGR_CURSOR_SLOTS);
            else if (myIdx < 0)
                logf("  ⇒ nothing has fired, humanFactionCount is %u, and this machine's own faction "
                     "is NOT IN the registry at all. That is worth a look — it is the shape a "
                     "starved seat has.", cnt);
            else
                logf("  ⇒ ⚠ nothing has fired, BUT humanFactionCount is %u and this machine's own "
                     "faction is registry index %d — past the engine's slots, so it SHOULD have "
                     "fired. Read this as a problem, not as quiet success.", cnt, myIdx);
        }
    }
    else
        logf("  ⇒ ★★★ a faction past the engine's two slots is live in this campaign. That means "
             "humanFactionCount got past 2 — capture the registry block and say so on #63.");
    for (const auto& s : g_side)
        if (s.used) logf("    manager %016llX  faction %016llX  cursor=%u",
                        (unsigned long long)s.manager, (unsigned long long)s.faction, s.cursor);
}
#endif


// ============================================================================================
// THE COUNT HOLD — #63's actual fix, automatically armed after tick/cursor installation
//
// `humanFactionCount` reaches 3 in a 3-player campaign and something puts it back to 2 (§6uuu.20,
// §6uuu.25). The writer is still unlocated, so this does not fight it at its source — it re-asserts
// the correct value from the campaign tick, which is also the experiment that tells us whether the
// clamp is a one-shot or a standing rewrite ("are we debugging a ghost?", #63 step 2).
//
// ★★★ IT IS A RULE, NOT A VALUE, and that is the whole safety argument.
//
//     count := the number of LEADING entries in [0, cap) that are non-null and carry the engine's
//              own human flag (+0xCD0) — capped at what we have cursor storage for, and only ever
//              RAISED, never lowered.
//
// Every client computes that from array contents which are **replicated model state**, so every
// client arrives at the same answer without any of them being told a number. That is what makes it
// safe in lockstep, and it is why this is not `set it to 4`:
//
//   * in a genuine 2-player game the rule yields 2, which equals the current count ⇒ NO WRITE. A
//     provable no-op, which is what tester asked for on #63 ("stop clamping, never set it to 4").
//   * it can never point a consumer at an uninitialised slot, because it counts only entries that
//     are actually there — the B1 dump's `[3]` held `0x0015A5C8`, and this rule stops at `[2]`.
//   * it is capped by CURSOR_SLOTS_MAX, so it can never outrun the cursor storage above.
//
// ⚠ AND IT REFUSES TO ARM unless the cursor hooks are live. A raised count without them is exactly
// the corruption this file exists to prevent.
// ============================================================================================

static volatile long g_holdArmed  = 0;
static long          g_holdWrites = 0;
static uint32_t      g_holdLast   = 0;   // the last value we wrote
static uint32_t      g_holdSeen   = 0;   // the last value we found BEFORE writing — the clamp's trace
static uintptr_t resolveEventMgr(uintptr_t* outModel)
{
    uintptr_t root = 0, obj = 0, model = 0, mgr = 0;
    if (outModel) *outModel = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) return 0;
    if (!readAt(root + OFF_ROOT_OBJ, obj) || !obj) return 0;
    if (!readAt(obj + OFF_MODEL, model) || !model) return 0;
    if (!readAt(model + OFF_MODEL_EVENT_MGR, mgr)) return 0;
    if (outModel) *outModel = model;
    return mgr;
}

// ⚠⚠ MEMBERSHIP, NOT A HEURISTIC — and the 3-player baseline of 2026-08-13 is why.
//
// The first version of this asked only `pointer > 0x10000 && *(fac+0xCD0) != 0`. Slot `[3]` of a
// cap-4 registry is uninitialised heap, and on two of the three machines it read `0x0015A5C8` with
// a `+0xCD0` byte of **84** and **144** — non-zero, so it passed, so the rule would have returned 4
// on those two clients and 3 on the third. Two different counts on two clients is a desync **by
// construction**, and a count of 4 would have pointed the engine at a pointer that is not a faction.
//
// ⇒ Caught in the baseline capture, before anything was armed. The lesson is the project's own: a
// plausibility test on garbage is not a test. So this asks the only question with a real answer —
// **is this pointer actually one of the campaign's factions?** — by looking it up in the all-factions
// array, which is replicated model state, so every client computes the same answer.
static bool isRealHumanFaction(uintptr_t model, uintptr_t fac)
{
    if (fac <= 0x10000) return false;

    uint8_t human = 0;
    // A bool, not a bitfield. `!= 0` let 84 and 144 through; the engine only ever writes 0 or 1.
    if (!readAt(fac + OFF_FACTION_IS_HUMAN, human) || human != 1) return false;

    uintptr_t cont = 0; uint32_t n = 0; uintptr_t arr = 0;
    if (!readAt(model + OFF_CONTAINER, cont) || !cont) return false;
    if (!readAt(cont + OFF_FACTIONS_COUNT, n) || !readAt(cont + OFF_FACTIONS_PTR, arr)) return false;
    if (!arr || n == 0 || n > 512) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uintptr_t f = 0;
        if (!readAt(arr + (uintptr_t)i * 8, f)) return false;
        if (f == fac) return true;
    }
    return false;
}

// The rule. Returns 0 if it cannot be computed safely — in which case we do nothing at all.
static uint32_t humansActuallyRegistered(uintptr_t model, uintptr_t mgr)
{
    uint32_t cap = 0; uintptr_t data = 0;
    if (!readAt(mgr + OFF_EVMGR_HF_CAP, cap) || !readAt(mgr + OFF_EVMGR_HF_PTR, data)) return 0;
    if (!data || cap == 0 || cap > 64) return 0;              // cannot be true; refuse
    uint32_t n = 0;
    for (uint32_t i = 0; i < cap && i < CURSOR_SLOTS_MAX; ++i) {
        uintptr_t fac = 0;
        if (!readAt(data + (uintptr_t)i * 8, fac)) break;
        if (!isRealHumanFaction(model, fac)) break;           // stop at the first non-member
        ++n;
    }
    return n;
}

// =================================================================================================
//  ★★★★★★ #68 — RE-REGISTER A HUMAN THE SAVE CAME BACK WITHOUT
//
//  Measured 2026-08-14. A save written while `humanFactionCount` was clamped to 2 persists only two
//  ids: the writer walks the registry bounded by the count. Reloading it restores a two-entry
//  registry while all three factions still carry `isHuman`, so the third player is a human the
//  campaign cannot reach — no events collected, no dilemma, nothing in the sidebar, everyone's turn
//  locks. ⚠ The writer itself is FINE: a save made with the count already at 3 round-trips all three,
//  proven by the manual-save test the same evening. The clamp is upstream of everything.
//
//  ⇒ Raising the count cannot fix a reloaded save, because the entry is absent rather than hidden.
//  This puts it back.
//
//  ★ THE ORDER RULE, and it is what makes this safe to replicate. The registry INDEX selects the
//  cursor slot (§63), so two clients appending in different orders desync by construction. So we do
//  not choose an order: existing entries keep the order the save gave them (identical everywhere,
//  same file), and the missing ones are appended in **all-factions array order**, which is replicated
//  model state. Every client computes the same array from the same inputs — the same discipline
//  `humansActuallyRegistered` already uses, for the same reason.
//
//  ⚠⚠ THIS GROWS AN ENGINE-OWNED BUFFER, which is the riskiest write this mod makes. After loading a
//  two-id save the vector is `cap=2, count=2` — the loader reserves exactly the stream count — so
//  there is no spare slot and appending means reallocating. The buffer MUST come from the engine's
//  allocator, because the engine's own destructor and the loader's growth path will free it.
//  `FUN_1406705A0(bytes, 0)` / `FUN_140670570(ptr)` are the pair the loader itself uses.
//
//  ★ WRITE ORDER, chosen so the vector is valid at every intermediate step:
//      1. allocate 4 slots, copy the existing entries, ZERO the rest
//      2. publish `data`  (cap still 2 -> readers see 2 valid entries in a valid buffer)
//      3. publish `cap`   (count still 2 -> the spare slots are zeroed, never garbage)
//      4. write the missing factions into the spare slots
//      5. publish `count` LAST — nothing observes a member before it is fully written
//      6. free the old buffer
//  There is no point in that sequence where a reader sees count > valid entries.
// =================================================================================================

#ifndef TW3K_RELEASE
static constexpr uintptr_t RVA_ENGINE_ALLOC = 0x06705A0;   // FUN_1406705A0(bytes, 0) -> void*
static constexpr uintptr_t RVA_ENGINE_FREE  = 0x0670570;   // FUN_140670570(ptr)

// Read out of a process image, not transcribed. ⚠ `PUSH RDI` here is `40 57` — the redundant-REX
// form, the same encoding that made a hook refuse itself earlier today.
static const uint8_t EXPECT_ENGINE_ALLOC[15] = {
    0x40, 0x57,                                             // PUSH RDI
    0x48, 0x83, 0xEC, 0x50,                                 // SUB RSP,0x50
    0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF,   // MOV [RSP+0x20],-2
};
static const uint8_t EXPECT_ENGINE_FREE[13] = {
    0x48, 0x85, 0xC9,               // TEST RCX,RCX
    0x74, 0x2A,                     // JZ  +0x2A
    0x53,                           // PUSH RBX
    0x48, 0x83, 0xEC, 0x20,         // SUB RSP,0x20
    0x48, 0x8B, 0xD9,               // MOV RBX,RCX
};

typedef void* (*EngineAllocFn)(size_t bytes, int zero);
typedef void  (*EngineFreeFn)(void* p);

// The faction's own id, for the log line only — nothing here decides anything on it. Same field the
// registry dump prints as `id(+0x8)`.
static constexpr size_t OFF_FACTION_ID_LOCAL = 0x8;

static volatile long g_repairWrites = 0;
static volatile long g_repairAdded  = 0;

// ⚠⚠ SEPARATE FROM THE COUNT HOLD, AND OFF BY DEFAULT — and the reason is a mistake worth recording.
//
// The first version of this shipped inside `hfcount` with no switch of its own. Two clients then
// crashed within an hour of deploying it, during an autosave at a turn switch, and there was **no way
// to test the repair apart from the count raise** — the only lever disabled both. That made
// attribution impossible, which is worse than either bug.
//
// ⇒ Arm them independently: `hfcount on --yes` raises the count, `hfcount repair on --yes` puts back
// a missing member. Run the first alone to reproduce the afternoon's known-good behaviour; add the
// second only to test it. Same replicated-state rule as everything else here — every machine or none.
static volatile long g_repairArmed = 0;

static bool engineAllocatorsVerified()
{
    uint8_t a[sizeof(EXPECT_ENGINE_ALLOC)] = { 0 };
    uint8_t f[sizeof(EXPECT_ENGINE_FREE)]  = { 0 };
    if (!safeRead((void*)(g_base + RVA_ENGINE_ALLOC), a, sizeof(a)) ||
        memcmp(a, EXPECT_ENGINE_ALLOC, sizeof(a)) != 0) return false;
    if (!safeRead((void*)(g_base + RVA_ENGINE_FREE), f, sizeof(f)) ||
        memcmp(f, EXPECT_ENGINE_FREE, sizeof(f)) != 0) return false;
    return true;
}

static void* engineAlloc(size_t bytes)
{
    __try { return ((EngineAllocFn)(g_base + RVA_ENGINE_ALLOC))(bytes, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void engineFree(void* p)
{
    __try { ((EngineFreeFn)(g_base + RVA_ENGINE_FREE))(p); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Returns the number of factions appended. 0 means "nothing to do", which is the normal case and
// the case in every fresh campaign.
static uint32_t repairHumanFactionRegistry(uintptr_t model, uintptr_t mgr)
{
    uint32_t cap = 0, count = 0; uintptr_t data = 0;
    if (!readAt(mgr + OFF_EVMGR_HF_CAP, cap) ||
        !readAt(mgr + OFF_EVMGR_HF_COUNT, count) ||
        !readAt(mgr + OFF_EVMGR_HF_PTR, data)) return 0;
    if (!data || cap == 0 || cap > 64 || count > cap) return 0;      // cannot be true; refuse

    uintptr_t cont = 0; uint32_t nFac = 0; uintptr_t arr = 0;
    if (!readAt(model + OFF_CONTAINER, cont) || !cont) return 0;
    if (!readAt(cont + OFF_FACTIONS_COUNT, nFac) || !readAt(cont + OFF_FACTIONS_PTR, arr)) return 0;
    if (!arr || nFac == 0 || nFac > 512) return 0;

    // Who is human, a real member, and NOT already registered — in all-factions order.
    uintptr_t missing[CURSOR_SLOTS_MAX] = { 0 };
    uint32_t  nMissing = 0;
    for (uint32_t i = 0; i < nFac && nMissing < CURSOR_SLOTS_MAX; ++i) {
        uintptr_t fac = 0;
        if (!readAt(arr + (uintptr_t)i * 8, fac)) return 0;
        if (!isRealHumanFaction(model, fac)) continue;

        bool already = false;
        for (uint32_t j = 0; j < count; ++j) {
            uintptr_t have = 0;
            if (!readAt(data + (uintptr_t)j * 8, have)) return 0;
            if (have == fac) { already = true; break; }
        }
        if (!already) missing[nMissing++] = fac;
    }
    if (nMissing == 0) return 0;                                     // the normal case

    const uint32_t want = count + nMissing;
    if (want > CURSOR_SLOTS_MAX) {
        logf("HUMAN_FACTIONS REPAIR: ⚠ REFUSING — %u registered + %u missing = %u, past the %u cursor "
             "slots this build can cover. Registering them would enrol a faction in the event-log "
             "compaction with nowhere to keep its cursor.", count, nMissing, want, CURSOR_SLOTS_MAX);
        return 0;
    }

    uintptr_t oldData = data;
    if (want > cap) {
        if (!engineAllocatorsVerified()) {
            logf("HUMAN_FACTIONS REPAIR: ⚠ REFUSING — the engine allocator prologues do not match on "
                 "this build, so the registry cannot be grown safely. Nothing written.");
            return 0;
        }
        void* fresh = engineAlloc((size_t)CURSOR_SLOTS_MAX * 8);
        if (!fresh) {
            logf("HUMAN_FACTIONS REPAIR: ⚠ the engine allocator returned nothing. Nothing written.");
            return 0;
        }
        // Copy what is there, zero the rest, and only then publish — see the write-order note above.
        for (uint32_t j = 0; j < CURSOR_SLOTS_MAX; ++j) {
            uintptr_t v = 0;
            if (j < count && !readAt(data + (uintptr_t)j * 8, v)) { engineFree(fresh); return 0; }
            if (!safeWriteBytes((uintptr_t)fresh + (uintptr_t)j * 8, &v, sizeof(v))) {
                engineFree(fresh); return 0;
            }
        }
        const uintptr_t freshAddr = (uintptr_t)fresh;
        const uint32_t  newCap    = CURSOR_SLOTS_MAX;
        if (!safeWriteBytes(mgr + OFF_EVMGR_HF_PTR, &freshAddr, sizeof(freshAddr))) {
            engineFree(fresh); return 0;
        }
        if (!safeWriteBytes(mgr + OFF_EVMGR_HF_CAP, &newCap, sizeof(newCap))) return 0;
        data = freshAddr;
        cap  = newCap;
    }

    for (uint32_t k = 0; k < nMissing; ++k)
        if (!safeWriteBytes(data + (uintptr_t)(count + k) * 8, &missing[k], sizeof(missing[k])))
            return 0;

    if (!safeWriteBytes(mgr + OFF_EVMGR_HF_COUNT, &want, sizeof(want))) return 0;

    if (oldData != data && oldData) engineFree((void*)oldData);

    InterlockedIncrement(&g_repairWrites);
    InterlockedExchangeAdd(&g_repairAdded, (long)nMissing);
    for (uint32_t k = 0; k < nMissing; ++k) {
        uint32_t id = 0;
        readAt(missing[k] + OFF_FACTION_ID_LOCAL, id);
        logf("HUMAN_FACTIONS REPAIR: ★★★ re-registered faction id=%u at index %u — it carries "
             "isHuman but the save came back without it (#68). Count %u -> %u%s.",
             id, count + k, count, want,
             (oldData != data) ? ", registry grown to 4 slots via the engine's own allocator" : "");
    }
    return nMissing;
}

#endif

// Called from the campaign tick on the game thread. The hold auto-arms at attach; registry
// repair remains an explicit operator choice.
void tickHumanFactionCountHold()
{
#ifdef TW3K_RELEASE
    if (!g_holdArmed) return;
#else
    if (!g_holdArmed && !g_repairArmed) return;
#endif

    uintptr_t model = 0;
    const uintptr_t mgr = resolveEventMgr(&model);
    if (!mgr || !model) return;

    // #68 first: a member that is absent cannot be reached by raising a count. This is a no-op in
    // every fresh campaign, and fires at most once per load. ⚠ Armed separately — see the note above.
#ifndef TW3K_RELEASE
    if (g_repairArmed) repairHumanFactionRegistry(model, mgr);
#endif

    if (!g_holdArmed) return;

    uint32_t count = 0;
    if (!readAt(mgr + OFF_EVMGR_HF_COUNT, count)) return;

    const uint32_t want = humansActuallyRegistered(model, mgr);
    if (want == 0 || want <= count) return;                   // only ever raises; 2-player = no-op

    if (!safeWriteBytes(mgr + OFF_EVMGR_HF_COUNT, &want, sizeof(want))) return;

    g_holdSeen = count;
    g_holdLast = want;
    if (++g_holdWrites <= 10 || (g_holdWrites % 100) == 0)
        diagLogf("HUMAN_FACTION COUNT HOLD: found %u, %u factions are actually registered and human — "
             "raised it. (write #%ld) %s", count, want, g_holdWrites,
             g_holdWrites == 1 ? "★ If this fires ONCE the clamp is a one-shot at campaign start; if "
                                 "it keeps firing, something rewrites it every tick and THAT is the "
                                 "writer we could not find statically."
                               : "");
#ifdef TW3K_RELEASE
    // One support milestone, not a per-tick diagnostic. No output on steady turns.
    if (g_holdWrites == 1)
        logf("campaign: human count %u -> %u (automatic count hold)", count, want);
#endif
}

bool setHumanFactionCountHold(bool on, const char** why)
{
    if (on) {
        if (!campaignTickHookActive()) {
            if (why) *why = "the campaign tick is not live, so maintenance cannot run";
            return false;
        }
        if (!g_cursorWriteDetour.active || !g_collectDetour.active || !g_gatePatched) {
            if (why) *why = "the event-cursor hooks are not all live on this machine — a raised "
                            "count without them enrols this faction in the event-log compaction, "
                            "which erases entries the other seats still index into";
            return false;
        }
        InterlockedExchange(&g_holdArmed, 1);
        return true;
    }

    // A saved process-wide count does not belong to the current campaign, even if its
    // manager address was reused. Stop future raises; let the engine rebuild on load.
    InterlockedExchange(&g_holdArmed, 0);
    diagLogf("HUMAN_FACTION COUNT HOLD: disarmed; current count LEFT IN PLACE. The engine rebuilds "
         "it on the next load. Every machine or none, exactly as arming was.");
    return true;
}

bool humanFactionCountHoldArmed() { return g_holdArmed != 0; }

// #68's repair, armed independently of the count hold. Same preconditions: it writes replicated model
// state and it enrols a faction in the event-log machinery, so the cursor hooks must be live.
#ifndef TW3K_RELEASE
bool setHumanFactionRepair(bool on, const char** why)
{
    if (on) {
        if (!campaignTickHookActive()) {
            if (why) *why = "the campaign tick is not live, so maintenance cannot run";
            return false;
        }
        if (!g_cursorWriteDetour.active || !g_collectDetour.active || !g_gatePatched) {
            if (why) *why = "the event-cursor hooks are not all live on this machine — re-registering "
                            "a third human without them enrols it in the event-log compaction, which "
                            "erases entries the other seats still index into";
            return false;
        }
        InterlockedExchange(&g_repairArmed, 1);
        return true;
    }
    InterlockedExchange(&g_repairArmed, 0);
    // ⚠ Deliberately does NOT un-register anything. Removing a member would change every later index
    // and therefore every cursor slot, on one client only — a desync far worse than the bug. Disarming
    // stops further repairs; what is already registered stays, and a reload starts clean.
    logf("HUMAN_FACTIONS REPAIR: disarmed. Entries already re-registered are LEFT IN PLACE — removing "
         "one would renumber every index after it on this client alone. Reload for a clean slate.");
    return true;
}
#endif


#ifndef TW3K_RELEASE
void reportHumanFactionCountHold()
{
    logf("---- HUMAN_FACTION COUNT HOLD (#63: the fix, and the ghost test) ----");
    logf("  campaign tick: %s", campaignTickHookActive() ? "LIVE" : "MISSING — maintenance unavailable");
    // ⚠ "disarmed" is now the ABNORMAL reading — the hold arms itself at attach (2026-08-15), so a
    // disarmed client either had `hfcount off` typed at it or could not meet the preconditions.
    // Saying "ships this way" here would have been actively misleading after the default flipped.
    logf("  %s | raises performed=%ld | last: found %u -> wrote %u",
         g_holdArmed ? "ARMED (automatic at attach)" : "DISARMED — ⚠ not the default any more",
         g_holdWrites, g_holdSeen, g_holdLast);
    logf("  #68 registry repair: %s | performed=%ld  factions re-registered=%ld",
         g_repairArmed ? "ARMED" : "disarmed (ships this way)", g_repairWrites, g_repairAdded);
    if (g_repairAdded > 0)
        logf("  ⇒ ★★★ this campaign was loaded from a save written while the count was clamped: %ld "
             "human(s) were absent from the registry and have been put back. Without this they are "
             "unreachable and their dilemmas can never be presented — B1 on a reloaded save.",
             g_repairAdded);
    if (!g_holdArmed)
        logf("  ⇒ ⚠⚠ THIS CLIENT IS THE ODD ONE OUT. The hold arms itself at attach, so every other "
             "machine on this build has it ARMED and will raise the count while this one does not — "
             "and a differing count in replicated model state is a desync by construction. Either "
             "`hfcount on --yes` here, or `hfcount off` everywhere else. Check `ping` first: a build "
             "mismatch is the usual reason one client is out of step.");
    else if (g_holdWrites == 0)
        logf("  ⇒ armed and has never needed to fire: the count already equals the number of "
             "registered human factions. Below three humans that is the expected reading.");
    else if (g_holdWrites == 1)
        logf("  ⇒ ★★★ fired ONCE. The clamp is a one-shot — it happens at campaign start and does "
             "not stand. That is the good case and the fix is simply this.");
    else
        logf("  ⇒ ★★★ fired %ld times. Something rewrites the count repeatedly; this hold is racing "
             "it, and the real writer still has to be found. Capture the interval between writes.",
             g_holdWrites);
}
#endif
