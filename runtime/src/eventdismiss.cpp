// eventdismiss.cpp — #105: the 3rd and 4th human can right-click an event-feed entry away.
//
// ============================================================================================
// THE BUG
//
// Right-clicking an entry runs the UI's MarkAsDismissed (FUN_142F6F440 -> FUN_1414CEA10): it adds
// the local faction to that one materialized message and submits
// CCQ_MARK_EVENT_FEED_MESSAGE_AS_DISMISSED with the message's (generation, log index). Every peer
// executes it through FUN_141B43010 -> FUN_1414CEDF0(mgr, key, faction), which stores the fact as
// bit 11+registryIndex of the 16-bit event-log word — and returns early for index 2 or 3:
//
//     1414CEE3A   CMP R9,0x2  /  JGE <return>       ; R9 = registry index of the faction
//
// The word has only two DISMISSED bits (11-12); bits 13-14 are READ. So the 3rd/4th human's
// dismissal is dropped, the next feed rebuild (FUN_142F76A80) re-materializes the message from the
// word without them, IsDismissed (FUN_14149F110) answers false, and the entry is back. Every turn
// adds more. Independent of mods: this is retail 1.7.2 behaviour once hfcount lets a 3rd human in.
//
// ============================================================================================
// THE FIX — three prologue hooks, no model write, no code-byte patch
//
//   FUN_1414CEDF0  the handler    call it; if it refused ONLY because the index was 2 or 3, keep
//                                 the dismissal in eventdismiss::Table and fire the same
//                                 notification the native path fires (model+0x78 -> +0x1860).
//   FUN_14149F110  IsDismissed    call it; if it said no, ask the table. Every UI reader of a
//                                 message's dismissed state goes through this leaf — the feed list
//                                 builder FUN_142F76A80 skips any entry it answers true for.
//   FUN_14149F970  the log erase  compaction memmoves the log down, renumbering every later index;
//                                 replay each erase of the manager's log onto the table.
//
// ★ Not lockstep state. The handler runs on every peer, so every peer's table agrees, but the only
// reader is the UI leaf: nothing in the model consults it, so a client without it cannot desync —
// it just keeps the vanilla symptom. The notification is the one the engine already sends for
// seats 0/1 from the same handler, on every peer, so it adds no new ordering either.
//
// ⚠ NOT SAVED. The save keeps the two-bit word; after a load the 3rd/4th human's dismissed entries
// return once and can be dismissed again. Persisting it would mean a save-format extension.
//
// ⚠ Lifetime: the table holds a manager pointer and reads `mgr+0x260` from the UI leaf without a
// VirtualQuery, which is only safe because eventcursor.cpp's destructor and HUMAN_FACTIONS-load
// hooks clear it (eventDismissReset) before that memory can go away. So this installs only after
// those hooks are live.
// ============================================================================================

#include "tw3k.h"
#include "offsets.h"
#include "eventdismiss.h"
#include <atomic>

// FUN_1414CEDF0(mgr, key{u32 generation, u32 index}*, faction). 16 bytes, 5 whole instructions,
// none relative, no branch lands inside them.
//   48 89 5C 24 08   MOV [RSP+8],RBX      57 PUSH RDI      48 83 EC 30 SUB RSP,0x30
//   48 8B D9         MOV RBX,RCX          4D 8B D8         MOV R11,R8
static constexpr uintptr_t RVA_DISMISS_HANDLER = 0x014CEDF0;
static const uint8_t EXPECT_DISMISS_HANDLER[] = {
    0x48,0x89,0x5C,0x24,0x08, 0x57, 0x48,0x83,0xEC,0x30, 0x48,0x8B,0xD9, 0x4D,0x8B,0xD8
};
// FUN_14149F110(msg, faction) -> AL. A 0x39-byte leaf: the JNZ at +7 rules out a 14-byte detour,
// so entryRedirect5 steals the first two instructions (7 bytes) and the trampoline returns to the
// JNZ with the CMP's flags intact.
//   48 8B 01         MOV RAX,[RCX]        80 78 42 00      CMP byte ptr [RAX+0x42],0
static constexpr uintptr_t RVA_IS_DISMISSED = 0x0149F110;
static const uint8_t EXPECT_IS_DISMISSED[] = { 0x48,0x8B,0x01, 0x80,0x78,0x42,0x00 };
// FUN_14149F970(log, start, len): the chunked-log erase. Its F350 caller passes the manager itself.
// 14 bytes, 4 whole instructions, none relative.
//   40 53 PUSH RBX   48 83 EC 30 SUB RSP,0x30   48 89 6C 24 40 MOV [RSP+0x40],RBP   48 8B D9 MOV RBX,RCX
static constexpr uintptr_t RVA_LOG_ERASE = 0x0149F970;
static const uint8_t EXPECT_LOG_ERASE[] = {
    0x40,0x53, 0x48,0x83,0xEC,0x30, 0x48,0x89,0x6C,0x24,0x40, 0x48,0x8B,0xD9
};

static constexpr size_t OFF_EVMGR_GENERATION = 0x260;  // u32, ++ at the end of FUN_14149F350
static constexpr size_t OFF_EVMGR_OWNER      = 0x00;   // -> object whose +0x78 is the model
static constexpr size_t OFF_OWNER_MODEL      = 0x78;
static constexpr size_t OFF_MODEL_DISMISS_SINK = 0x1860; // vtable slot +0x10 takes {faction, gen}
static constexpr size_t OFF_MSG_GENERATION   = 0x88;    // materialized message: u32 generation
static constexpr size_t OFF_MSG_INDEX        = 0x8C;    // u32 log index
static constexpr int    NATIVE_DISMISS_SLOTS = 2;       // bits 11-12
static constexpr int    DISMISS_SLOTS_MAX    = 4;       // eventcursor's ceiling

using DismissHandlerFn = void (*)(uintptr_t mgr, const uint32_t* key, uintptr_t faction);
using IsDismissedFn    = uint8_t (*)(uintptr_t msg, uintptr_t faction);
using LogEraseFn       = void (*)(uintptr_t log, uint32_t start, uint32_t len);
using DismissSinkFn    = void (*)(uintptr_t sink, void* payload);

static Detour g_handlerDetour, g_isDismissedDetour, g_eraseDetour;
static DismissHandlerFn g_origHandler = nullptr;
static IsDismissedFn    g_origIsDismissed = nullptr;
static LogEraseFn       g_origErase = nullptr;

static SRWLOCK g_lock = SRWLOCK_INIT;
static eventdismiss::Table g_table;
static std::atomic<bool> g_enabled{false};
static std::atomic<bool> g_any{false};     // the UI leaf's lock-free early out
static long g_stored = 0, g_duplicate = 0, g_stale = 0, g_full = 0, g_answered = 0, g_erases = 0;
static long g_notifyRefused = 0;

// The handler's own search, bound and all: linear over [data, data+count).
static int registryIndexOf(uintptr_t mgr, uintptr_t faction)
{
    uint32_t count = 0; uintptr_t data = 0;
    if (!readAt(mgr + OFF_EVMGR_HF_COUNT, count) || !readAt(mgr + OFF_EVMGR_HF_PTR, data) || !data)
        return -1;
    if (count > DISMISS_SLOTS_MAX) count = DISMISS_SLOTS_MAX;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t f = 0;
        if (!readAt(data + i * 8, f)) return -1;
        if (f == faction) return int(i);
    }
    return -1;
}

// The native accept path ends by telling model+0x78 -> +0x1860 {faction, generation}. Same call,
// same payload shape ([RSP+0x20] faction, [RSP+0x28] u32 generation).
static void notifyDismissed(uintptr_t mgr, uintptr_t faction, uint32_t generation)
{
    uintptr_t owner = 0, model = 0, vtable = 0, fn = 0;
    if (!readAt(mgr + OFF_EVMGR_OWNER, owner) || !readAt(owner + OFF_OWNER_MODEL, model) || !model ||
        !readAt(model + OFF_MODEL_DISMISS_SINK, vtable) || !readAt(vtable + 0x10, fn) ||
        fn < g_base || fn >= g_base + 0x05000000) {
        ++g_notifyRefused;
        return;
    }
    struct { uintptr_t faction; uint32_t generation; uint32_t pad; } payload{ faction, generation, 0 };
    ((DismissSinkFn)fn)(model + OFF_MODEL_DISMISS_SINK, &payload);
}

static void dismissHandlerHook(uintptr_t mgr, const uint32_t* key, uintptr_t faction)
{
    g_origHandler(mgr, key, faction);
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    const int idx = registryIndexOf(mgr, faction);
    if (idx < NATIVE_DISMISS_SLOTS || idx >= DISMISS_SLOTS_MAX) return;   // native, or not a human

    uint32_t generation = 0, index = 0, current = 0;
    if (!readAt((uintptr_t)key, generation) || !readAt((uintptr_t)key + 4, index) ||
        !readAt(mgr + OFF_EVMGR_GENERATION, current))
        return;
    eventdismiss::Table::Record r;
    AcquireSRWLockExclusive(&g_lock);
    r = g_table.record(mgr, faction, generation, index, current);
    g_any.store(!g_table.empty(), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_lock);

    using R = eventdismiss::Table::Record;
    switch (r) {
    case R::Stored:    ++g_stored; break;
    case R::Duplicate: ++g_duplicate; break;
    case R::Stale:     ++g_stale; return;      // the engine would have refused this one too
    case R::Full:
        if (++g_full == 1)
            logf("Event dismissal: storage full; further 3rd/4th-player dismissals will return");
        return;
    }
    diagLogf("Event dismissal #105: registry index %d, generation %u, log index %u -> %s",
             idx, generation, index, r == R::Stored ? "stored" : "already stored");
    notifyDismissed(mgr, faction, generation);
}

static uint8_t isDismissedHook(uintptr_t msg, uintptr_t faction)
{
    const uint8_t native = g_origIsDismissed(msg, faction);
    if (native || !g_any.load(std::memory_order_relaxed) || !g_enabled.load(std::memory_order_relaxed))
        return native;
    bool answer = false;
    AcquireSRWLockShared(&g_lock);
    if (g_table.knowsFaction(faction)) {
        // The engine just dereferenced msg; mgr is kept alive by the lifetime reset (header note).
        const uint32_t generation = *(const uint32_t*)(msg + OFF_MSG_GENERATION);
        const uint32_t index      = *(const uint32_t*)(msg + OFF_MSG_INDEX);
        const uint32_t current    = *(const uint32_t*)(g_table.mgr + OFF_EVMGR_GENERATION);
        answer = g_table.query(g_table.mgr, faction, generation, index, current);
    }
    ReleaseSRWLockShared(&g_lock);
    if (answer) InterlockedIncrement(&g_answered);
    return answer ? 1 : native;
}

static void logEraseHook(uintptr_t log, uint32_t start, uint32_t len)
{
    g_origErase(log, start, len);
    if (!g_any.load(std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_lock);
    if (log && log == g_table.mgr) {
        g_table.erase(log, start, len, *(const uint32_t*)(log + OFF_EVMGR_GENERATION));
        g_any.store(!g_table.empty(), std::memory_order_relaxed);
        ++g_erases;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void eventDismissReset(uintptr_t mgr)
{
    AcquireSRWLockExclusive(&g_lock);
    if (!mgr || mgr == g_table.mgr) {
        g_table.reset();
        g_any.store(false, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

bool installEventDismissHooks()
{
    // Bookkeeping first, the writer last: an erase or a query with an empty table is a no-op.
    if (!detourInstall(g_eraseDetour, g_base + RVA_LOG_ERASE, sizeof(EXPECT_LOG_ERASE),
                       EXPECT_LOG_ERASE, (uintptr_t)&logEraseHook, (void**)&g_origErase,
                       "event dismissal erase"))
        return false;
    uintptr_t original = 0;
    if (!entryRedirect5(g_isDismissedDetour, g_base + RVA_IS_DISMISSED, sizeof(EXPECT_IS_DISMISSED),
                        EXPECT_IS_DISMISSED, (uintptr_t)&isDismissedHook, &original,
                        "event dismissal query")) {
        detourRemove(g_eraseDetour, "event dismissal erase");
        return false;
    }
    g_origIsDismissed = (IsDismissedFn)original;
    if (!detourInstall(g_handlerDetour, g_base + RVA_DISMISS_HANDLER, sizeof(EXPECT_DISMISS_HANDLER),
                       EXPECT_DISMISS_HANDLER, (uintptr_t)&dismissHandlerHook,
                       (void**)&g_origHandler, "event dismissal handler")) {
        detourRemove(g_isDismissedDetour, "event dismissal query");
        detourRemove(g_eraseDetour, "event dismissal erase");
        return false;
    }
    g_enabled.store(true);
    return true;
}

void removeEventDismissHooks()
{
    // Stop storing, then stop answering; the erase bookkeeping goes last.
    g_enabled.store(false);
    detourRemove(g_handlerDetour, "event dismissal handler");
    detourRemove(g_isDismissedDetour, "event dismissal query");
    detourRemove(g_eraseDetour, "event dismissal erase");
    eventDismissReset(0);
}

#ifndef TW3K_RELEASE
bool eventDismissEnabled() { return g_enabled.load(); }
// Off stops storing and answering but keeps the table, so `on` resumes with what was stored.
bool setEventDismissEnabled(bool on)
{
    if (!g_handlerDetour.active || !g_isDismissedDetour.active || !g_eraseDetour.active) return false;
    g_enabled.store(on);
    logf("#105 event dismissal: switched %s", on ? "ON" : "OFF (vanilla)");
    return true;
}

void reportEventDismiss()
{
    logf("---- EVENT DISMISSAL (#105: right-click for the 3rd and 4th human) ----");
    if (!g_handlerDetour.active || !g_isDismissedDetour.active || !g_eraseDetour.active) {
        logf("  NOT installed on this machine — a 3rd/4th player's dismissed entries return.");
        return;
    }
    size_t entries = 0; uintptr_t mgr = 0;
    AcquireSRWLockShared(&g_lock);
    entries = g_table.entries; mgr = g_table.mgr;
    ReleaseSRWLockShared(&g_lock);
    logf("  manager=%016llX entries=%zu/%zu stored=%ld duplicate=%ld stale=%ld full=%ld",
         (unsigned long long)mgr, entries, eventdismiss::maxEntries, g_stored, g_duplicate,
         g_stale, g_full);
    logf("  answered=%ld erases replayed=%ld notify refused=%ld", g_answered, g_erases,
         g_notifyRefused);
}
#endif
