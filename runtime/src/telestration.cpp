// telestration.cpp — #47 / #50: who actually gets a telestration slot, measured rather than deduced.
//
// ★★★ WHAT THIS IS FOR. Pings and battle field-lines/arrows are ONE subsystem, internally called
// **telestration**, and it admits a fixed set of participants chosen once at battle construction.
// Measured across three battles covering all three combatant positions (2026-08-15, §6uuu.38):
//
//     working set = {the combatant} u {the lowest-indexed spectator}.  Exactly two.
//
// Everyone else silently gets nothing: their ping fires locally and reaches no one, their brush is
// greyed out, and they cannot see anyone else's strokes either.
//
// ⚠⚠ THIS FILE MEASURES, IT DOES NOT FIX. Two mechanisms have already been proposed and killed:
//
//   ✗ "it is the machine / player"  — the failure moved when the roles moved.
//   ✗ "our +0x10=0,+0x11=1 half-state routes lending spectators down the participant branch"
//     — plausible, and TESTED WITH `lend off`: a natural spectator is still shut out. Both branches
//     fail for a third player, so the gate is in the key lookup they share, not in branch selection.
//
// ⇒ The open question is exactly one thing — **which keys does this client's map actually hold?** —
// and that is a log line rather than a session, which is the move `dumpEventMgrRegistry` made for B1.
//
// Everything here is READ-ONLY apart from the detour itself: the hook calls the original first and
// then walks memory. No game function is called, so nothing here can race the engine's own state.

#include "tw3k.h"

// The constructor takes five arguments (four in registers, the fifth on the stack) and returns the
// object. Matching the shape exactly matters: the trampoline replays three `MOV [RSP+n], reg` stores
// and the body then computes RBP from that same RSP, so the frame has to be ours and consistent.
typedef void* (*TelestrationCtorFn)(void*, void*, void*, void*, void*);

Detour                    g_telestrationDetour;
static TelestrationCtorFn g_origTelestrationCtor = nullptr;
static volatile long      g_telCtorCalls         = 0;

// The live manager, captured on the way past. The lobby guard does the same trick for `g_liveLobby`,
// and for the same reason: there is no global to find it in.
static volatile uintptr_t g_telestrationMgr = 0;

static constexpr int TEL_MAX_KEYS  = 32;   // a battle has a handful; this is a runaway guard
static constexpr int TEL_WALK_STOP = 256;  // ...and so is this, on a corrupt or cyclic list

// ---- the cap, and the lookup a fix has to drive -------------------------------------------------
//
// ★★★★★ `FUN_142E9DFC0(battleMgr, &out)` is the whole defect, and it is four lines:
//
//     svc->vt[0x248](svc, &vec);       // fetch the FULL share player list
//     if (vec.count == 0) *out = -1;
//     else *out = vec.data[0];         // <-- takes list[0] and discards the rest
//
// The constructor calls it ONCE and adds ONE entry, so participants = alliance members (measured: 1)
// + at most one extra = **2**, whoever else is in the battle.
//
// ★★ It is the same line as the gift bug: `FUN_142EA8580`, the share-units click handler at an
// ADJACENT address, also fetches `vt[0x248]` and takes `list[0]`. One idiom, used twice, and only one
// of them had ever been found. ⇒ Nothing here is bounded or short-allocated — the full list is
// already in hand — so this is a choice of value, not a feature to build.
//
// Both the list and the key lookup hang off the manager's own service pointer at `mgr+0x40`
// (`R12+0x40` in the constructor), so a probe needs no globals:
//
//     svcObj = *(void**)(mgr + 0x40) + 8
//     list   : svcObj->vt[0x248](svcObj, &CaVec32)      -- entries are SLOT indices, not player ids
//     key    : rec = FUN_14046F640(svcObj->vt[0x208](svcObj), slot);  key = *rec, skipped when 0
//
// ⚠ THESE ARE GAME CALLS. They run only from the constructor hook, which is the game thread. The
// `capture` path prints the cached result and calls nothing.
struct CaVec32 { uint32_t capacity; uint32_t count; int32_t* data; };
typedef void  (*GameFreeFn)(void*);

static constexpr uintptr_t RVA_TEL_KEY_LOOKUP = 0x0046F640;  // FUN_14046F640(svc, slot) -> record
static constexpr uintptr_t RVA_TEL_GAME_FREE  = 0x00670570;  // FUN_140670570
static constexpr size_t    OFF_TEL_SERVICE    = 0x40;        // manager+0x40 = the share service
static constexpr size_t    VT_TEL_KEY_SVC     = 0x208;       // virtual: the object keys resolve on
static constexpr size_t    VT_TEL_PLAYER_LIST = 0x248;       // virtual: the full share player list

// Cached at construction (game thread), printed by `capture` (probe thread). One row per player the
// battle actually holds, with the key the engine would use and whether the map has it.
struct TelParticipant { int32_t slot; uint64_t key; bool hasSlot; };
static TelParticipant g_telRoster[TEL_MAX_KEYS] = { { 0, 0, false } };
static volatile long  g_telRosterN  = 0;
static volatile long  g_telRosterOk = 0;

// Walk the participant map. Returns the number of entries, or -1 if the manager is unreadable.
// `out` receives up to TEL_MAX_KEYS keys.
//
// ⚠ The sentinel is an ADDRESS (mgr+0x50), not a node to dereference — the engine's own lookup ends
// by comparing against `LEA RAX,[R12+0x50]`. Reading it as a node would be reading the manager's own
// fields as a key.
static int telestrationKeys(uintptr_t mgr, uint64_t* out, int* truncated)
{
    if (truncated) *truncated = 0;
    if (!mgr) return -1;

    const uint64_t sentinel = (uint64_t)(mgr + OFF_TEL_SENTINEL);
    uint64_t node = 0;
    if (!readAt(mgr + OFF_TEL_HEAD, node)) return -1;

    int n = 0;
    for (int guard = 0; guard < TEL_WALK_STOP; ++guard) {
        if (!node || node == sentinel) return n;          // clean end of list
        if (node < 0x10000) return n;                     // not a pointer — stop, do not deref

        uint64_t key = 0;
        if (!readAt((uintptr_t)node + OFF_TEL_NODE_KEY, key)) return n;
        if (n < TEL_MAX_KEYS) out[n] = key;
        else if (truncated)   *truncated = 1;
        ++n;

        uint64_t next = 0;
        if (!readAt((uintptr_t)node + OFF_TEL_NODE_NEXT, next)) return n;
        node = next;
    }
    return n;   // hit the runaway guard — reported as such by the caller
}

// Build the roster: every player the battle holds, the key the engine would compute for them, and
// whether the map actually has it. ⚠⚠ GAME THREAD ONLY — it calls three engine functions.
//
// This is deliberately the read-only rehearsal of the fix: it drives the *exact* lookup an inserting
// version would drive, so if the call path is wrong we find out from a log line rather than from a
// heap write. Nothing is inserted and nothing is modified.
#ifndef TW3K_RELEASE
static void buildTelestrationRoster(uintptr_t mgr, const uint64_t* keys, int nKeys)
{
    InterlockedExchange(&g_telRosterN, 0);
    InterlockedExchange(&g_telRosterOk, 0);
    if (!mgr) return;

    __try {
        const uintptr_t svcPtr = *(uintptr_t*)(mgr + OFF_TEL_SERVICE);
        if (!svcPtr || svcPtr < 0x10000) return;

        void* const     svcObj = (void*)(svcPtr + 8);
        const uintptr_t vt     = *(uintptr_t*)svcObj;
        if (!vt || vt < 0x10000) return;

        CaVec32 list = { 0, 0, nullptr };
        ((void(*)(void*, void*))(*(uintptr_t*)(vt + VT_TEL_PLAYER_LIST)))(svcObj, &list);
        if (!list.data || !list.count || list.count > 64) {
            if (list.data) ((GameFreeFn)(g_base + RVA_TEL_GAME_FREE))(list.data);
            return;
        }

        void* const keySvc = ((void*(*)(void*))(*(uintptr_t*)(vt + VT_TEL_KEY_SVC)))(svcObj);
        auto        lookup = (uint64_t*(*)(void*, uint32_t))(g_base + RVA_TEL_KEY_LOOKUP);

        int n = 0;
        for (uint32_t i = 0; i < list.count && n < TEL_MAX_KEYS; ++i) {
            const int32_t slot = list.data[i];
            uint64_t      key  = 0;
            if (keySvc) {
                uint64_t* rec = lookup(keySvc, (uint32_t)slot);
                if (rec) key = *rec;      // the engine skips this player entirely when it reads 0
            }
            bool has = false;
            for (int k = 0; k < nKeys && k < TEL_MAX_KEYS; ++k)
                if (keys[k] == key && key != 0) { has = true; break; }

            g_telRoster[n].slot    = slot;
            g_telRoster[n].key     = key;
            g_telRoster[n].hasSlot = has;
            ++n;
        }

        ((GameFreeFn)(g_base + RVA_TEL_GAME_FREE))(list.data);
        InterlockedExchange(&g_telRosterN, (long)n);
        InterlockedExchange(&g_telRosterOk, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A fault here means the call path a fix would use is WRONG, which is exactly what this
        // rehearsal exists to discover — and far better learned now than from an insert.
        InterlockedExchange(&g_telRosterOk, 0);
    }
}
#endif

// ================================================================================================
// THE FIX — insert the entries the constructor never asked for. Ships DISARMED.
//
// ★ Everything below is the constructor's own code, in its own order, driven for the players it
// skipped. See `offsets.h` for the instruction-level transcript it was read from. What makes this
// the cheap class of fix rather than the expensive one:
//
//   ✓ the full player list is already in hand (`vt[0x248]`) — nothing is bounded or short-allocated
//   ✓ every occupied slot already has a key (`FUN_14046F640` = the MP session slot record)
//   ✓ the insert is idempotent — a key already present returns its node and consumes nothing
//   ✓ ⚠ **none of this is replicated simulation state.** The manager is a UI object on the "radar"
//     widget fed by `chat_*_msg`; it is not lockstep-checksummed. So the every-machine-or-none rule
//     that governs `hfcount` does NOT apply here, and a client that misses this just fails locally.
//
// ⚠ It runs INSIDE the constructor hook, on the game thread, with the service live and `ctorArg2`
// still the argument the engine itself would have passed. Arming mid-battle does nothing until the
// next battle builds a manager — which is the same constraint the engine has: the participant set is
// decided once, at construction, and nothing re-reads it.
// ================================================================================================

static bool          g_telFixArmed  = false;
static volatile long g_telFixRuns   = 0;   // constructions the fix ran on
static volatile long g_telInserted  = 0;   // entries it added
static volatile long g_telLocalSet  = 0;   // times it repaired this client's own +0x78
static volatile long g_telFixFaults = 0;   // ⚠ any non-zero value retires this switch

typedef void  (*TelVecReserveFn)(void* vec, uint32_t n);
typedef void  (*TelVecPushFn)(void* vec, void* obj);
typedef void* (*TelStrokeCtorFn)(void* buf, uint32_t index, void* ctorArg2);
typedef void  (*TelStrokeDtorFn)(uintptr_t subObject);
typedef void* (*TelValueMakeFn)(void* value, void* vec);
typedef void* (*TelMapInsertFn)(void* map, void* iter, void* pair);
typedef void  (*TelValueFreeFn)(void* value);

// {cap, count, data} again — the same CaVec32 shape as the share list, stride 0x150.
struct TelStrokeVec { uint32_t capacity; uint32_t count; void* data; };
// The insert reads key at +0, cap at +8, count at +0xC, data at +0x10 — which is exactly how the
// constructor lays them out on its own stack ([RSP+0x48] and [RSP+0x50] are adjacent).
struct TelPair { uint64_t key; TelStrokeVec value; };
struct TelIter { void* node; unsigned char inserted; unsigned char pad[7]; };

static void telDestroyStrokeVec(TelStrokeVec& vec)
{
    auto gameFree = (GameFreeFn)(g_base + RVA_TEL_GAME_FREE);
    auto dtor     = (TelStrokeDtorFn)(g_base + RVA_TEL_STROKE_DTOR);
    if (vec.data) {
        for (uint32_t i = 0; i < vec.count; ++i) {
            uint8_t* const e   = (uint8_t*)vec.data + (size_t)i * TEL_STROKE_SIZE;
            void* const    pts = *(void**)(e + TEL_STROKE_POINTS);
            if (pts) gameFree(pts);                             // ⚠ buffer first, sub-object second
            dtor((uintptr_t)e + TEL_STROKE_SUBOBJ);
        }
        gameFree(vec.data);
    }
    vec.capacity = 0; vec.count = 0; vec.data = nullptr;
}

// One participant, built and inserted exactly as 0x142DC2DFA..0x142DC2E8E does it.
// Returns the map node (existing or new), or nullptr if nothing was touched.
static void* telInsertParticipant(uintptr_t mgr, uint64_t key, uint32_t strokeIndexBase,
                                  void* ctorArg2, bool* outInserted)
{
    if (outInserted) *outInserted = false;
    if (!mgr || !key) return nullptr;

    auto reserve  = (TelVecReserveFn)(g_base + RVA_TEL_VEC_RESERVE);
    auto push     = (TelVecPushFn)(g_base + RVA_TEL_VEC_PUSH);
    auto ctor     = (TelStrokeCtorFn)(g_base + RVA_TEL_STROKE_CTOR);
    auto dtor     = (TelStrokeDtorFn)(g_base + RVA_TEL_STROKE_DTOR);
    auto makeVal  = (TelValueMakeFn)(g_base + RVA_TEL_VALUE_MAKE);
    auto insert   = (TelMapInsertFn)(g_base + RVA_TEL_MAP_INSERT);
    auto freeVal  = (TelValueFreeFn)(g_base + RVA_TEL_VALUE_FREE);
    auto gameFree = (GameFreeFn)(g_base + RVA_TEL_GAME_FREE);

    TelStrokeVec vec = { 0, 0, nullptr };
    reserve(&vec, TEL_STROKES_PER_PLAYER);

    // The stroke temp is a 0x150-byte stack object the constructor rebuilds for every stroke. It is
    // fully initialised by its own constructor; zeroing first only makes a fault deterministic.
    alignas(16) uint8_t tmp[TEL_STROKE_SIZE];
    for (uint32_t i = 0; i < TEL_STROKES_PER_PLAYER; ++i) {
        memset(tmp, 0, sizeof(tmp));
        void* const obj = ctor(tmp, strokeIndexBase + i, ctorArg2);
        push(&vec, obj ? obj : (void*)tmp);
        void* const pts = *(void**)(tmp + TEL_STROKE_POINTS);
        if (pts) gameFree(pts);
        dtor((uintptr_t)tmp + TEL_STROKE_SUBOBJ);
    }

    TelPair pair;
    pair.key = key;
    makeVal(&pair.value, &vec);        // deep-copies the ten strokes into the pair

    TelIter iter = { nullptr, 0, { 0 } };
    insert((void*)(mgr + OFF_TEL_MAP), &iter, &pair);

    // On a hit the insert consumed nothing, so this frees the copy; on a miss it was moved out and
    // zeroed, so this is a no-op. Either way it is the call the constructor makes here.
    freeVal(&pair.value);
    telDestroyStrokeVec(vec);

    if (outInserted) *outInserted = (iter.inserted != 0);
    return iter.node;
}

// The map's node list, walked for one key. Same walk as `telestrationKeys`, returning the node.
static void* telFindNode(uintptr_t mgr, uint64_t key)
{
    const uint64_t sentinel = (uint64_t)(mgr + OFF_TEL_SENTINEL);
    uint64_t node = 0;
    if (!key || !readAt(mgr + OFF_TEL_HEAD, node)) return nullptr;

    for (int guard = 0; guard < TEL_WALK_STOP; ++guard) {
        if (!node || node == sentinel || node < 0x10000) return nullptr;
        uint64_t k = 0;
        if (!readAt((uintptr_t)node + OFF_TEL_NODE_KEY, k)) return nullptr;
        if (k == key) return (void*)node;
        uint64_t next = 0;
        if (!readAt((uintptr_t)node + OFF_TEL_NODE_NEXT, next)) return nullptr;
        node = next;
    }
    return nullptr;
}

// ⚠⚠ GAME THREAD ONLY, and only from the constructor hook: every call below is an engine call and
// `ctorArg2` is the constructor's own argument.
static void telestrationApplyFix(uintptr_t mgr, void* ctorArg2, const uint64_t* have, int nHave)
{
    if (!g_telFixArmed || !mgr) return;
    InterlockedIncrement(&g_telFixRuns);

    __try {
        const uintptr_t svcPtr = *(uintptr_t*)(mgr + OFF_TEL_SERVICE);
        if (!svcPtr || svcPtr < 0x10000) {
            diagLogf("  ⚠ #47/#50 fix: no share service on this manager — nothing to insert.");
            return;
        }
        void* const     svcObj = (void*)(svcPtr + 8);
        const uintptr_t vt     = *(uintptr_t*)svcObj;
        if (!vt || vt < 0x10000) return;

        CaVec32 list = { 0, 0, nullptr };
        ((void(*)(void*, void*))(*(uintptr_t*)(vt + VT_TEL_PLAYER_LIST)))(svcObj, &list);
        void* const keySvc = ((void*(*)(void*))(*(uintptr_t*)(vt + VT_TEL_KEY_SVC)))(svcObj);
        auto        lookup = (uint64_t*(*)(void*, uint32_t))(g_base + RVA_TEL_KEY_LOOKUP);

        // ★ The stroke index is a single running counter in the constructor — entry 0 gets 0..9,
        // entry 1 gets 10..19. Continuing from 10 x (entries already present) is what the engine
        // itself would have produced had it enumerated everybody.
        uint32_t nextStroke = (uint32_t)(nHave > 0 ? nHave : 0) * TEL_STROKES_PER_PLAYER;

        uint64_t added[TEL_MAX_KEYS] = { 0 };
        int      nAdded = 0;

        if (list.data && list.count && list.count <= 64) {
            for (uint32_t i = 0; i < list.count && nAdded < TEL_MAX_KEYS; ++i) {
                uint64_t        key = 0;
                uint64_t* const rec = keySvc ? lookup(keySvc, (uint32_t)list.data[i]) : nullptr;
                if (rec) key = *rec;
                if (!key) continue;                  // an empty slot: the engine skips these too

                bool seen = false;
                for (int k = 0; k < nHave && k < TEL_MAX_KEYS; ++k) if (have[k] == key) seen = true;
                for (int k = 0; k < nAdded; ++k)     if (added[k] == key) seen = true;
                if (seen) continue;

                bool  inserted = false;
                void* node     = telInsertParticipant(mgr, key, nextStroke, ctorArg2, &inserted);
                if (node && inserted) {
                    added[nAdded++] = key;
                    nextStroke += TEL_STROKES_PER_PLAYER;
                    InterlockedIncrement(&g_telInserted);
                    const int owner = playerForSlot(list.data[i]);
                    diagLogf("  ★ #47/#50 fix: inserted a telestration entry for slot %d, key %016llX%s%s"
                         " — that player can now ping, draw, and be seen drawing.",
                         list.data[i], (unsigned long long)key,
                         (owner >= 0 && knownPlayerName(owner)[0]) ? " — " : "",
                         (owner >= 0) ? knownPlayerName(owner) : "");
                } else if (node) {
                    diagLogf("  #47/#50 fix: slot %d (key %016llX) was already in the map after all.",
                         list.data[i], (unsigned long long)key);
                }
            }
        } else {
            diagLogf("  ⚠ #47/#50 fix: the share player list read empty (count=%u) — nothing to do.",
                 list.count);
        }
        if (list.data) ((GameFreeFn)(g_base + RVA_TEL_GAME_FREE))(list.data);

        // ★★ And then this client's OWN entry, which the constructor's tail sets only when its
        // lookup hits. Insert-then-point, using the same local record the tail reads. Without this
        // an excluded client would see everyone else's strokes and still not be able to draw.
        uint64_t* const localRec =
            ((uint64_t*(*)(void*))(*(uintptr_t*)(vt + VT_TEL_LOCAL_RECORD)))(svcObj);
        const uint64_t localKey = localRec ? *localRec : 0;
        if (localKey) {
            bool  inserted = false;
            void* node     = telFindNode(mgr, localKey);
            if (!node) {
                node = telInsertParticipant(mgr, localKey, nextStroke, ctorArg2, &inserted);
                if (node && inserted) InterlockedIncrement(&g_telInserted);
            }
            if (node) {
                void** const slot = (void**)(mgr + OFF_TEL_LOCAL_ENTRY);
                void* const  want = (void*)((uintptr_t)node + OFF_TEL_NODE_VALUE);
                if (*slot != want) {
                    *slot = want;
                    InterlockedIncrement(&g_telLocalSet);
                    diagLogf("  ★★ #47/#50 fix: this client's own entry (key %016llX) is now wired to "
                         "mgr+0x78 — the constructor's tail had left it unset, which is what stops "
                         "an excluded player drawing at all.", (unsigned long long)localKey);
                }
            } else {
                logf("  ⚠ #47/#50 fix: this client's own key %016llX could not be placed.",
                     (unsigned long long)localKey);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_telFixFaults);
        logf("  ⚠⚠ #47/#50 fix: FAULTED and was caught. The insert path is wrong somewhere — treat "
             "this switch as retired until that is understood, and read the roster line above for "
             "how far it got.");
    }
}

bool telestrationFixArmed() { return g_telFixArmed; }

bool setTelestrationFix(bool on, const char** why)
{
    if (on && !g_telestrationDetour.active) {
        if (why) *why = "the telestration constructor hook is not installed on this client, so "
                        "nothing would ever apply the fix";
        return false;
    }
    g_telFixArmed = on;
    logf(">>> #47/#50 TELESTRATION FIX: %s. It applies at the NEXT battle — the participant set is "
         "decided once, at construction, and nothing re-reads it.", on ? "ARMED" : "disarmed");
    return true;
}

#ifndef TW3K_RELEASE
void reportTelestration()
{
    logf("---- TELESTRATION (#47/#50: who holds a ping/arrow slot) ----");

    if (!g_telestrationDetour.active) {
        logf("  hook NOT installed — this client can say nothing about #47/#50 this run.");
        return;
    }

    const uintptr_t mgr = (uintptr_t)g_telestrationMgr;
    if (!mgr) {
        logf("  hook live, ctor calls=%ld, but no manager captured yet.", g_telCtorCalls);
        logf("  ⇒ that is CORRECT outside a battle — the manager is built at battle construction "
             "and this is the only place its pointer ever appears.");
        return;
    }

    uint32_t nbuckets = 0;
    uint64_t buckets  = 0;
    readAt(mgr + OFF_TEL_NBUCKETS, nbuckets);
    readAt(mgr + OFF_TEL_BUCKETS,  buckets);

    uint64_t keys[TEL_MAX_KEYS] = { 0 };
    int truncated = 0;
    const int n = telestrationKeys(mgr, keys, &truncated);

    logf("  manager=%016llX  ctor calls=%ld  buckets=%u @ %016llX",
         (unsigned long long)mgr, g_telCtorCalls, nbuckets, (unsigned long long)buckets);

    if (n < 0) {
        logf("  ⚠ the manager pointer does not read — treat the offsets as wrong, not the game.");
        return;
    }

    logf("  participant slots on THIS client: %d%s", n,
         truncated ? "  (list longer than we print)" : "");
    for (int i = 0; i < n && i < TEL_MAX_KEYS; ++i)
        logf("      slot[%d] key=%016llX", i, (unsigned long long)keys[i]);
    if (n == 0)
        logf("      (none — this client can neither draw nor be drawn to, which IS the #47/#50 "
             "symptom seen from the inside)");

    // ---- the roster: who the battle holds, against who actually got a slot ----------------------
    const long rn = g_telRosterN;
    if (!g_telRosterOk) {
        logf("  roster: NOT built — the share service or its player list did not read at "
             "construction. ⚠ A fix cannot use this call path until that is understood.");
    } else {
        int missing = 0;
        logf("  battle holds %ld player(s) — key computed through the SAME lookup a fix would use "
             "(FUN_14046F640):", rn);
        for (long i = 0; i < rn && i < TEL_MAX_KEYS; ++i) {
            // ⚠ The list entries are SLOT indices, not player ids — the two spaces diverge the
            // moment anyone rejoins (#45). Cross back explicitly rather than indexing by position.
            const int   owner = playerForSlot(g_telRoster[i].slot);
            const char* who   = (owner >= 0) ? knownPlayerName(owner) : "";
            logf("      slot %-2d key=%016llX  %s%s%s",
                 g_telRoster[i].slot, (unsigned long long)g_telRoster[i].key,
                 g_telRoster[i].hasSlot ? "HAS a telestration slot" : "★ MISSING — cannot ping or draw",
                 (who && who[0]) ? "  — " : "", (who && who[0]) ? who : "");
            if (!g_telRoster[i].hasSlot) ++missing;
        }
        if (missing)
            logf("  ⇒ ★★★ %d player(s) shut out. That is #47/#50, and the cause is FUN_142E9DFC0 "
                 "returning list[0] instead of every entry.", missing);
        else
            logf("  ⇒ every player in this battle holds a slot — #47/#50 should NOT reproduce here. "
                 "If it does, the map is not the whole story.");
    }

    logf(TW3K_MODE_TEXT("  fix (`tel fix`): %s  runs=%ld  entries inserted=%ld  own entry repaired=%ld%s", "  automatic fix: %s  runs=%ld  entries inserted=%ld  own entry repaired=%ld%s"),
         g_telFixArmed ? "ARMED" : "disarmed",
         g_telFixRuns, g_telInserted, g_telLocalSet,
         g_telFixFaults ? "  ⚠⚠ AND IT FAULTED — see above" : "");

    // ★ The reading, stated here so a log is self-explaining at 1am and nobody has to hold the
    // ticket in their head. What matters is the COMPARISON ACROSS MACHINES, not any one number.
    logf("  >>> READING IT: collect this line from every client in the same battle.");
    logf("      · a client whose own key is ABSENT is the one that cannot ping or draw.");
    logf("      · if every client shows the SAME small set, the map is replicated and the cap is in");
    logf("        the key lookup both branches share (svc vt[0x220] / FUN_14046F640).");
    logf("      · if the sets DIFFER per client, it is built locally and the cap is per-client.");
    logf("      ⚠ Do NOT read a count of 2 as 'the engine supports two'. It may be two because the");
    logf("        alliance had two members, which is a different bug with the same number.");
}
#endif

// The hook itself. Calls the original FIRST — the map does not exist until it returns — then records
// the instance and reports once. Nothing is modified.
static void* telestrationCtorHook(void* self, void* a2, void* a3, void* a4, void* a5)
{
    void* r = self;
    if (g_origTelestrationCtor) r = g_origTelestrationCtor(self, a2, a3, a4, a5);

    TW3K_DIAGNOSTIC(InterlockedIncrement(&g_telCtorCalls));
    g_telestrationMgr = (uintptr_t)self;

    // Build the roster HERE, on the game thread, while the service is live — `capture` runs on the
    // probe thread and must never make these calls.
    uint64_t keys[TEL_MAX_KEYS] = { 0 };
    int      trunc = 0;
    int      nk    = telestrationKeys((uintptr_t)self, keys, &trunc);
    TW3K_DIAGNOSTIC(buildTelestrationRoster((uintptr_t)self, keys, nk < 0 ? 0 : nk));

#ifndef TW3K_RELEASE
    static volatile long announced = 0;
    if (InterlockedCompareExchange(&announced, 1, 0) == 0)
        logf(">>> TELESTRATION: FIRST CALL, manager=%016llX — #47/#50's participant map exists from "
             TW3K_MODE_TEXT("here on, and `capture` can read it.", "here on, and the local log records its state."), (unsigned long long)self);

    // Report every construction, not just the first: a battle builds one, and comparing battle to
    // battle is exactly how the working set was characterised in the first place.
    reportTelestration();

#endif
    // ★ And then, if armed, the fix — followed by a SECOND reading of the same map. The log then
    // carries before and after from one battle, which is what makes a claim about it checkable
    // instead of remembered.
    if (g_telFixArmed) {
        telestrationApplyFix((uintptr_t)self, a2, keys, nk < 0 ? 0 : nk);
#ifndef TW3K_RELEASE
        nk = telestrationKeys((uintptr_t)self, keys, &trunc);
        TW3K_DIAGNOSTIC(buildTelestrationRoster((uintptr_t)self, keys, nk < 0 ? 0 : nk));
        logf("---- and the same map AFTER the #47/#50 fix ran ----");
        reportTelestration();
#endif

    }
    return r;
}

bool installTelestrationHook()
{
    return detourInstall(g_telestrationDetour, g_base + RVA_TELESTRATION_CTOR,
                         TELESTRATION_STOLEN, EXPECT_TELESTRATION_CTOR,
                         (uintptr_t)&telestrationCtorHook,
                         (void**)&g_origTelestrationCtor, "telestration probe");
}

void removeTelestrationHook() { detourRemove(g_telestrationDetour, "telestration probe"); }
