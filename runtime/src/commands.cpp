// commands.cpp - What the campaign is actually being told to do. Read-only.
//
// ★★★ WHY THIS EXISTS: B1, the locked turn. tester's decisive observation is that the locked player
// CAN send diplomacy and CAN construct buildings, but CANNOT move armies or end turn. Two readings
// fit that, and they are different bugs:
//
//   (a) the UI refuses to submit the command  -> nothing ever enters the queue
//   (b) the command is submitted and applied  -> the refusal is below the command layer
//
// Nothing we had could tell those apart. This can, and the reading is SELF-CONTAINED — it does not
// need a healthy machine to compare against, because the working actions are the control group:
// `CCQ_DIPLOMACY_*` and `CCQ_REGION_BUILDING_CONSTRUCT` appearing while `CCQ_END_TURN` never does
// is (a), on one machine, in one capture.
//
// ⚠ THE HOOK IS NOT WHERE THE ANSWER IS PRINTED. Logging every command as it executes would bury a
// campaign, exactly as NETCODE_NOTES warned. So the hook COUNTS (cheap, unconditional) and `F3`
// PRINTS THE COUNTS. "CCQ_END_TURN: 0 this session" next to "CCQ_DIPLOMACY_BEGIN_NEGOTIATION: 7" is
// the whole finding, and it costs no log volume at all.
//
// ── HOW THE QUEUE IS READ ─────────────────────────────────────────────────────────────────────────
//
// `FUN_141B44840(queue, world)` is the executor, and its own loop was read instruction by
// instruction rather than guessed:
//
//     buffer     = queue + 0x08
//     usedBytes  = *(u32*)(queue + 0x5008)          // CMP cursor, [RDI+0x5008]
//     record at cursor:
//         +0x00  u16  size      (little-endian; the engine builds it as buf[c+1]<<8 | buf[c])
//         +0x02  u32  id        ⚠ BIG-ENDIAN — assembled a byte at a time, high byte first
//         +0x06  u8   version
//         +0x07       payload
//     cursor += size                                 // R11D = [RSP+0x38] + [RSP+0x3C]
//     size < 7 is the engine's own error exit, so it is ours too.
//
// ★ IDS ARE NAMED WITHOUT TOUCHING THE ENGINE'S REGISTRY. The executor resolves an id by hashing it
// (`id ^ 0x4A545EED`, modulo the bucket count) and walking a chain — re-implementing that against
// live engine data is precisely the kind of invention that has cost this project runs. It is also
// unnecessary: `wiki/reference/campaign-commands.md` records, for all 191 commands, the static
// address of the u32 the registrar writes the id into. Read those 191 slots once and the map is
// built. No hash, no bucket walk, no CA string constructed for the engine to free.
//
// ⚠ Deliberately OUTSIDE the nine-hook health check, for the same reason as the pre-battle hook: a
// machine must never be reported unready because an experiment's signature did not match.

#include "tw3k.h"
#ifndef TW3K_RELEASE
#include "command_ids.h"

static constexpr uintptr_t RVA_CMD_EXECUTOR  = 0x01B44840;   // FUN_141B44840
static constexpr size_t    CMD_STOLEN_LEN    = 16;           // 6 whole insns, none RIP-relative

// MOV [RSP+8],RBX / MOV [RSP+0x10],RBP / PUSH RSI,RDI,R12,R14 — register saves only, nothing
// RIP-relative, and nothing the body still expects in a volatile register that the jump could
// clobber. (The stack writes land in the caller's shadow space; the trampoline runs at the same RSP
// as the entry, so they land identically. Same shape as the pre-battle hook's prologue.)
static const uint8_t EXPECT_CMD_EXECUTOR[CMD_STOLEN_LEN] = {
    0x48,0x89,0x5C,0x24,0x08,   // MOV [RSP+0x08], RBX
    0x48,0x89,0x6C,0x24,0x10,   // MOV [RSP+0x10], RBP
    0x56,                       // PUSH RSI
    0x57,                       // PUSH RDI
    0x41,0x54,                  // PUSH R12
    0x41,0x56                   // PUSH R14
};

static constexpr size_t OFF_QUEUE_BUFFER = 0x0008;
static constexpr size_t OFF_QUEUE_USED   = 0x5008;
static constexpr size_t CMD_HEADER_LEN   = 7;      // the engine's own minimum; below it, it errors

// The return value is passed straight through rather than discarded: the single caller
// (0x1419E5692, inside the campaign tick) has not been read closely, and preserving RAX costs one
// line while assuming it is unused could cost a session.
typedef uint64_t (*CmdExecFn)(void*, void*);
static Detour     g_cmdDetour;
static CmdExecFn  g_origCmdExec = nullptr;

// ---- the id -> name map, built once from the registrar's own slots --------------------------------

static uint32_t g_cmdIds[COMMAND_ID_SLOT_COUNT] = { 0 };
static long     g_cmdCounts[COMMAND_ID_SLOT_COUNT] = { 0 };
static bool     g_cmdMapBuilt   = false;
static int      g_cmdMapFilled  = 0;

// ★★ THE DELTA IS THE EXPERIMENT, and it is why this probe is worth more than a session total.
//
// A cumulative count cannot separate "END TURN worked on turn 2" from "END TURN was refused on turn
// 3" — both leave the counter non-zero. So every capture also SNAPSHOTS the counters, and the next
// capture prints the difference. That turns a passive dump into a controlled test:
//
//     press F3  →  click the control that does nothing  →  press F3 again
//
// The second capture's delta is the answer, and it is about that click and nothing else.
static long g_cmdCountsAtMark[COMMAND_ID_SLOT_COUNT] = { 0 };
static bool g_cmdMarked = false;

// Anything the executor ran whose id is in no slot we know. Kept separately rather than dropped —
// a command we cannot name is a fact about our table, and silently discarding it would hide it.
static constexpr int MAX_UNKNOWN = 24;
static uint32_t g_unknownIds[MAX_UNKNOWN]    = { 0 };
static long     g_unknownCounts[MAX_UNKNOWN] = { 0 };
static int      g_unknownSeen                = 0;
static long     g_unknownDropped             = 0;

static volatile long g_cmdDrains   = 0;   // executor calls
static volatile long g_cmdExecuted = 0;   // records seen across all of them

static void resolveLogImmediatelySlots();   // defined below, beside the watchlist it fills

static void buildCommandIdMap()
{
    if (g_cmdMapBuilt) return;
    g_cmdMapBuilt = true;

    for (size_t i = 0; i < COMMAND_ID_SLOT_COUNT; ++i) {
        uint32_t id = 0;
        if (readAt(g_base + COMMAND_ID_SLOTS[i].rva, id) && id != 0) {
            g_cmdIds[i] = id;
            ++g_cmdMapFilled;
        }
    }

    resolveLogImmediatelySlots();

    logf("COMMAND ID MAP: %d of %zu registrar slots hold a non-zero id.",
         g_cmdMapFilled, COMMAND_ID_SLOT_COUNT);
    if (g_cmdMapFilled < (int)COMMAND_ID_SLOT_COUNT)
        logf("   ⚠ the rest read 0. The registrar fills these at startup, so a large shortfall means "
             "we are reading too early or the slot addresses are wrong — not that the commands are "
             "absent.");
}

static int slotForId(uint32_t id)
{
    if (id == 0) return -1;
    for (size_t i = 0; i < COMMAND_ID_SLOT_COUNT; ++i)
        if (g_cmdIds[i] == id) return (int)i;
    return -1;
}

// ---- the few commands worth a line of their own, the moment they execute ------------------------
//
// ★ Counting is right for the bulk — 1,230 commands in twelve turns would bury a log. But a handful
// are rare AND load-bearing, and for those "it happened" is worth much less than "it happened HERE,
// at this moment, with this payload".
//
// `CCQ_FACTION_SWITCH_HUMAN_TO_AI` is the reason this exists. It fired twice in a healthy twelve-turn
// session (2026-08-04), and a human faction turned AI is a mechanism that would produce B1's exact
// report — that player stops being given turns as a human — and would carry into a save, which is
// what tester describes when a game saved in the bugged state cannot be restarted with everyone.
//
// ⚠ It is a LEAD, not a finding. Two switches fired and the human set was still 3 afterwards, so on
// that evidence they were benign — plausibly a reload re-seating players, since a save/load happened
// in the same window. This logs them so the next occurrence is not guesswork.
//
// ★★★ `CCQ_END_TURN` JOINS THEM FOR A SECOND REASON, AND IT IS tester'S (2026-08-04): *"I don't think
// we have ever reproduced the error local-only, perhaps it has to do with latency?"* — which is a
// hypothesis nothing we log could currently test.
//
// It can be tested for free, because EVERY PEER EXECUTES THE SAME QUEUE IN THE SAME ORDER. So the
// Nth `CCQ_END_TURN` is the same record on every machine, and the local clock beside it is the only
// thing that differs. Line the logs up on that line and the gaps ARE the per-machine lag, measured
// on the game's own traffic — no ping, no new hook, no assumption about the transport.
//
// ★ It is also the cleanest possible answer to B1's open question. `since` says a counter did not
// move; this says "END TURN was submitted at 16:47:03.112 and the stuck player's client never saw
// it", or that it never appeared anywhere. A timestamp beats a delta when the question is *when*.
//
// ⚠ ~4 lines per round in a four-player game. That is the reason for the cap, and the reason this
// list stays short: it is for the rare and load-bearing, never for the routine.
static const char* LOG_IMMEDIATELY[] = {
    "CCQ_FACTION_SWITCH_HUMAN_TO_AI",
    "CCQ_SAVE_CAMPAIGN_GAME_MULTIPLAYER",
    "CCQ_END_TURN",
    // ★ The multiplayer save BARRIER. Twelve of these in the 16:32 session, so it costs nothing —
    // and with END_TURN and the save either side of it, one log now carries the whole handover:
    // who ended the turn, when the save landed, and how many movements were in flight at each.
    "CCQ_PENDING_BATTLE_PLAYER_READY_TO_SAVE_GAME",
};
static constexpr size_t LOG_IMMEDIATELY_COUNT = sizeof(LOG_IMMEDIATELY) / sizeof(LOG_IMMEDIATELY[0]);
static constexpr long   LOG_IMMEDIATELY_CAP   = 80;   // per command, so nothing can flood

static int  g_logNowSlots[LOG_IMMEDIATELY_COUNT];
static long g_logNowSeen[LOG_IMMEDIATELY_COUNT] = { 0 };

static void resolveLogImmediatelySlots()
{
    for (size_t w = 0; w < LOG_IMMEDIATELY_COUNT; ++w) {
        g_logNowSlots[w] = -1;
        for (size_t i = 0; i < COMMAND_ID_SLOT_COUNT; ++i)
            if (strcmp(COMMAND_ID_SLOTS[i].name, LOG_IMMEDIATELY[w]) == 0) {
                g_logNowSlots[w] = (int)i;
                break;
            }
    }
}

// The payload is whatever the command's own class serialised. We do not know its shape, so this
// prints bytes rather than pretending to decode them — the faction this targets is presumably in
// here, and a hex dump is how that gets identified without inventing a layout first.
static void maybeLogImmediately(int slot, uintptr_t buf, uint32_t cursor, uint16_t size, uint8_t ver)
{
    for (size_t w = 0; w < LOG_IMMEDIATELY_COUNT; ++w) {
        if (g_logNowSlots[w] != slot) continue;
        if (g_logNowSeen[w] >= LOG_IMMEDIATELY_CAP) return;
        ++g_logNowSeen[w];

        const uint32_t payloadLen = (size > CMD_HEADER_LEN) ? (uint32_t)(size - CMD_HEADER_LEN) : 0;
        const uint32_t show = payloadLen > 48 ? 48 : payloadLen;
        uint8_t raw[48] = { 0 };
        char hex[160] = { 0 };
        if (show && safeRead((void*)(buf + cursor + CMD_HEADER_LEN), raw, show)) {
            int n = 0;
            for (uint32_t i = 0; i < show; ++i)
                n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", raw[i]);
        }
        // ★★★ THE MOVEMENT QUEUE, ON EVERY ONE OF THESE LINES. tester, from a vanilla 2-player run:
        // the autosave fires when the PREVIOUS player ends their turn, before the turn moves on —
        // and it is the next player who locks. Clause (B) refuses every army order and every END
        // TURN while a movement is in flight, so the number that matters is how many were in flight
        // AT THE MOMENT OF THE SAVE. A save taken over a moving army would serialise a movement
        // whose completion can never arrive, and the player after it would be locked out on restore.
        // ⇒ One integer, at exactly the right moment. `-1` means simply "not in a campaign".
        const int inFlight = movementsInFlight();

        const int pendingSave = autosavePending();

        logf("★ COMMAND: %s  (ver=%u, %u byte payload%s) #%ld   movementsInFlight=%d autosave=%d%s",
             COMMAND_ID_SLOTS[slot].name, ver, payloadLen,
             (show < payloadLen) ? ", first 48 shown" : "", g_logNowSeen[w], inFlight, pendingSave,
             (inFlight > 0) ? "   <-- ★★★ A MOVEMENT WAS IN FLIGHT WHEN THIS RAN" : "");
        if (hex[0]) logf("     payload: %s", hex);
        if (g_logNowSeen[w] == LOG_IMMEDIATELY_CAP)
            logf("     (that is %ld of these — no more will be logged individually, but the count "
                 "on F3 keeps rising.)", LOG_IMMEDIATELY_CAP);
        return;
    }
}

static void countUnknown(uint32_t id)
{
    for (int i = 0; i < g_unknownSeen; ++i)
        if (g_unknownIds[i] == id) { ++g_unknownCounts[i]; return; }
    if (g_unknownSeen < MAX_UNKNOWN) {
        g_unknownIds[g_unknownSeen] = id;
        g_unknownCounts[g_unknownSeen] = 1;
        ++g_unknownSeen;
    } else {
        ++g_unknownDropped;
    }
}

// ---- the hook --------------------------------------------------------------------------------

static uint64_t cmdExecHook(void* queue, void* world)
{
    // Counting only. Nothing is logged from here: the executor runs from the campaign tick, so it
    // is called every frame, and a campaign's worth of per-command lines is the failure mode
    // NETCODE_NOTES specifically warned about.
    if (queue) {
        buildCommandIdMap();

        uint32_t used = 0;
        uintptr_t buf = (uintptr_t)queue + OFF_QUEUE_BUFFER;
        if (readAt((uintptr_t)queue + OFF_QUEUE_USED, used) && used > 0 && used < 0x5000) {
            InterlockedIncrement(&g_cmdDrains);

            uint32_t cursor = 0;
            int guard = 0;
            while (cursor + CMD_HEADER_LEN <= used && ++guard < 512) {
                uint16_t size = 0;
                uint8_t  idb[4] = { 0 };
                if (!readAt(buf + cursor, size) || !safeRead((void*)(buf + cursor + 2), idb, 4))
                    break;
                if (size < CMD_HEADER_LEN) break;          // the engine's own error exit

                // ⚠ big-endian: the executor assembles this a byte at a time, high byte first.
                const uint32_t id = ((uint32_t)idb[0] << 24) | ((uint32_t)idb[1] << 16) |
                                    ((uint32_t)idb[2] << 8)  |  (uint32_t)idb[3];

                uint8_t ver = 0;
                readAt(buf + cursor + 6, ver);

                const int slot = slotForId(id);
                if (slot >= 0) {
                    InterlockedIncrement(&g_cmdCounts[slot]);
                    maybeLogImmediately(slot, buf, cursor, size, ver);
                } else {
                    countUnknown(id);
                }

                InterlockedIncrement(&g_cmdExecuted);
                cursor += size;
            }
        }
    }

    return g_origCmdExec ? g_origCmdExec(queue, world) : 0;
}

#endif
// ---- the campaign tick, which is where the answer drain actually belongs -----------------------
//
// ❌ The drain was first put in `cmdExecHook` above, on the reasoning that the executor "runs from
// the campaign tick, so it is called every frame". **Measured wrong, 2026-08-07**: `answer` queued a
// request in a live single-player campaign with a dilemma box open, and nothing drained — no submit
// line, no refusal, silence.
//
// The bytes say why. The executor's only caller is `0x1419E5692`, inside this function, and it sits
// inside a count test near the TOP of the tick:
//
//     uVar17 = param_3[0x19015];                       // pending command blocks
//     if (uVar17 != 0) { do { ... FUN_141B44840(...); } while (...); }
//
// ⇒ **an idle campaign never calls the executor at all.** Sitting on a modal dilemma box submitting
// nothing is exactly that state. Note it is NOT clause (D) starving it — the executor runs before
// any `+0x3B88` test, which is also why a normal click still works.
//
// ★ THE TELL THAT SHOULD HAVE CAUGHT THIS EARLIER: `reportCommandTraffic` already had a branch for
// "hook is live but NO command has executed yet". A drain hung off something that can legitimately
// never run is the session-5k failure — F8 queued against a hook that fired once, 52 s before the
// keypress — and it was rebuilt here in a different costume.
//
// This tick runs every frame regardless: it holds the (D) evaluation, the per-faction update and the
// turn-advance decision, so it cannot be gated by them.
static constexpr uintptr_t RVA_CAMPAIGN_TICK = 0x019EEF90;   // FUN_1419EEF90
static constexpr size_t    CAMP_TICK_STOLEN_LEN   = 14;           // 5 whole insns, none RIP-relative

// MOV [RSP+0x18],R8 / MOV [RSP+0x10],RDX / PUSH RBP,RSI,R12. The two stack writes are the caller's
// shadow space and the trampoline is CALLED at a proper frame, so they land where the body expects.
// ⚠ Byte-searched before use, not read off the listing — `40 53` vs `53` cost a run in 5l. Exactly
// one match in the image.
static const uint8_t EXPECT_CAMPAIGN_TICK[CAMP_TICK_STOLEN_LEN] = {
    0x4C,0x89,0x44,0x24,0x18,   // MOV [RSP+0x18], R8
    0x48,0x89,0x54,0x24,0x10,   // MOV [RSP+0x10], RDX
    0x55,                       // PUSH RBP
    0x56,                       // PUSH RSI
    0x41,0x54                   // PUSH R12
};

// undefined8* FUN_1419EEF90(longlong campaign, undefined8* out, uint* commandSource)
// ⚠ Three arguments and a POINTER return. Forward all of them exactly; the return is the caller's
// out-pointer and dropping it would corrupt the tick's own result.
typedef void* (*CampaignTickFn)(void*, void*, void*);
static Detour         g_tickCampDetour;
static CampaignTickFn g_origCampaignTick = nullptr;

static void* campaignTickHook(void* campaign, void* out, void* cmdSource)
{
    void* r = g_origCampaignTick ? g_origCampaignTick(campaign, out, cmdSource) : nullptr;

    // AFTER the original, so a submission lands on a tick that has finished its own walk. Costs one
    // volatile read per frame when nothing is queued, which is the overwhelmingly common case.
#ifndef TW3K_RELEASE
    drainAnswerRequest();
#endif
    // #63. Automatically armed once this tick and the cursor lifetime hooks are installed.
    tickHumanFactionCountHold();

    return r;
}

bool installCampaignTickHook()
{
    return detourInstall(g_tickCampDetour, g_base + RVA_CAMPAIGN_TICK, CAMP_TICK_STOLEN_LEN,
                         EXPECT_CAMPAIGN_TICK, (uintptr_t)&campaignTickHook,
                         (void**)&g_origCampaignTick, "campaign tick");
}

void removeCampaignTickHook() { detourRemove(g_tickCampDetour, "campaign tick"); }

bool campaignTickHookActive() { return g_tickCampDetour.active; }

#ifndef TW3K_RELEASE
bool installCommandExecHook()
{
    return detourInstall(g_cmdDetour, g_base + RVA_CMD_EXECUTOR, CMD_STOLEN_LEN,
                         EXPECT_CMD_EXECUTOR, (uintptr_t)&cmdExecHook,
                         (void**)&g_origCmdExec, "command executor");
}

void removeCommandExecHook() { detourRemove(g_cmdDetour, "command executor"); }

// This hook stopped being purely diagnostic when it became the game-thread drain for a queued
// dilemma answer (#43). Anything that queues work for it has to be able to ask whether it is live,
// or a failed install turns into a request that sits in the queue forever — which is exactly how
// session 5k was lost, with F8 queued against a hook that was barely being called.
bool commandExecHookActive() { return g_cmdDetour.active; }

// ---- the report, which is where the answer actually appears ------------------------------------

void reportCommandTraffic()
{
    logf("---- COMMAND TRAFFIC (B1: was the command ever submitted at all?) ----");

    if (!g_cmdDetour.active) {
        logf("  executor hook not installed — no reading available.");
        return;
    }
    if (g_cmdExecuted == 0) {
        logf("  hook is live but NO command has executed yet (%ld drain(s) seen). In a campaign that "
             "means either the campaign has not started or the offsets are wrong; outside one it is "
             "simply normal.", g_cmdDrains);
        return;
    }

    logf("  %ld command(s) executed across %ld non-empty drain(s). Every peer executes the same "
         "queue, so a command that is here was submitted and applied.", g_cmdExecuted, g_cmdDrains);

    // ★ The four that decide B1, printed first and printed even when zero — a zero is the finding.
    static const char* WATCH[] = {
        "CCQ_END_TURN",                    // refused, per the report
        "CCQ_CHARACTER_LOCOMOTE_TO",       // refused, per the report
        "CCQ_DIPLOMACY_BEGIN_NEGOTIATION", // works, per the report — the control group
        "CCQ_REGION_BUILDING_CONSTRUCT",   // works, per the report — the control group
    };
    logf("  --- the four that decide it (the last two are the CONTROL: tester reports these work) ---");
    logf("      %-34s %8s %8s", "command", "since", "session");
    for (size_t w = 0; w < sizeof(WATCH) / sizeof(WATCH[0]); ++w) {
        long n = -1, d = -1;
        for (size_t i = 0; i < COMMAND_ID_SLOT_COUNT; ++i)
            if (strcmp(COMMAND_ID_SLOTS[i].name, WATCH[w]) == 0) {
                n = g_cmdCounts[i];
                d = g_cmdMarked ? (g_cmdCounts[i] - g_cmdCountsAtMark[i]) : -1;
                break;
            }
        if (g_cmdMarked) logf("      %-34s %8ld %8ld", WATCH[w], d, n);
        else             logf("      %-34s %8s %8ld", WATCH[w], "n/a", n);
    }

    logf("  --- everything else that executed (since = change from the previous capture) ---");
    for (size_t i = 0; i < COMMAND_ID_SLOT_COUNT; ++i)
        if (g_cmdCounts[i] > 0) {
            // ⚠ On the FIRST capture there is no previous one, so print n/a rather than 0 — a 0
            // there reads as "this did not happen since the mark", which is a different claim.
            if (!g_cmdMarked) { logf("      %-52s %8s %8ld", COMMAND_ID_SLOTS[i].name, "n/a",
                                     g_cmdCounts[i]); continue; }
            const long d = g_cmdCounts[i] - g_cmdCountsAtMark[i];
            logf("      %-52s %8ld %8ld%s", COMMAND_ID_SLOTS[i].name, d, g_cmdCounts[i],
                 (d > 0) ? "   <-- since the last capture" : "");
        }

    if (g_unknownSeen) {
        logf("  --- ids that matched no registrar slot (a fact about OUR table) ---");
        for (int i = 0; i < g_unknownSeen; ++i)
            logf("      id=%08X  %ld", g_unknownIds[i], g_unknownCounts[i]);
        if (g_unknownDropped)
            logf("      (+%ld more distinct ids not tracked — the table holds %d)",
                 g_unknownDropped, MAX_UNKNOWN);
    }

    logf("  >>> HOW TO RUN IT — the `since` column is the experiment:");
    logf("      1. press F3        (this capture marks the counters)");
    logf("      2. click END TURN, and try to move an army");
    logf("      3. press F3 again  — `since` now covers those clicks and nothing else");
    logf("  >>> READING IT (on the SECOND capture's `since` column):");
    logf("      END_TURN / LOCOMOTE 0 while the control pair moved  ⇒ the UI never SUBMITS them. The");
    logf("          command layer is fine and the refusal is a client-side gate — read the TURN STATE");
    logf("          section above, which is where such a gate reads its answer.");
    logf("      END_TURN / LOCOMOTE non-zero  ⇒ the commands ARE submitted and applied, and the hold");
    logf("          is below the command layer. That moves the target to the army layer proper.");
    logf("      ⚠ The `session` column cannot answer this on its own: END TURN working on turn 2 and");
    logf("        being refused on turn 3 both leave it non-zero. Use `since`.");

    // Snapshot last, so this capture's own printing is not counted into its own delta.
    for (size_t i = 0; i < COMMAND_ID_SLOT_COUNT; ++i) g_cmdCountsAtMark[i] = g_cmdCounts[i];
    g_cmdMarked = true;
}

#endif
