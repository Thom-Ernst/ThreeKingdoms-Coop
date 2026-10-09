#pragma once
#include "build_mode.h"
// tw3k.h - the shared internal header for the PoC DLL.
//
// LAYOUT. What was one 4678-line tw3k_coop.cpp is now src/*.cpp, split along the section
// boundaries the file already had. The split was pure code motion - no logic changed.
//
//   offsets.h    image RVAs, hook sites and struct offsets used by more than one module
//   log.cpp      per-machine per-run log file and the timestamped logf
//   util.cpp     focus gate, SEH-guarded reads, change detector, instance finder
//   detour.cpp   the register-free detour, the live-lobby pointer and its liveness check
//   session.cpp  MP session: advertise, capture, open slots, join handler, seat order
//   lobby.cpp    faction assignment, auto-faction, tick drain, guard, vector/ready watches
//   panels.cpp   panel expansion to four, panel population, slot-changed probe
//   battle.cpp   unit-ownership identity, rematch, spectator latch, HUD-as-participant
//   gift.cpp     share instrumentation, per-player targeting, census, CcoBattleRoot entries
//   probes.cpp   read-only probes and the one-key full capture
//   main.cpp     DllMain, main loop, hotkeys, arming, stage-3 patch
//
// ../tw3k_coop.ASBUILT.cpp is still the known-good single-file snapshot of the source the last
// field-tested DLL was built from, and `build.bat asbuilt` rebuilds from it. That is the fallback
// if anything about this tree is ever suspect.
//
// The original file header follows, unchanged.
//
// =================================================================================================
// tw3k_coop.cpp — proof-of-concept probe DLL for Total War: THREE KINGDOMS
//
// PURPOSE: retire risk before writing any real mod logic. Proves, in order:
//   Stage 1  DLL loads into the process at all (survives anti-tamper)
//   Stage 2  our reverse-engineered offsets are correct on the LIVE process
//   Stage 3  a write to game code actually takes effect (and is reversible)
//
// This ONLY reads state and toggles one already-verified comparison byte.
// It does not touch, defeat, or work around the game's copy protection.
//
// Findings this is based on (see ../NETCODE_NOTES.md):
//   MPCampaignLobby singleton @ 0x1443BD0F0   (.didata, RW)
//     +0xCC  uint    current lobby player count
//     +0xD0  ptr     player record array (stride 0x48; rec+0x18 = name string)
//     +0xE8  string[2] per-player slot strings (stride 0x10)  <-- only 2 entries!
//   FUN_142D71280 invite-button gate:
//     0x142D7BF5B  83 BB CC 00 00 00 01   CMP dword ptr [RBX+0xCC], 1
//     0x142D712E2  77 ..                  JA  -> disable invite
//   The immediate at 0x142D712E1 is the "max players - 1" constant.
//
// !! Flipping that byte alone does NOT give working 4-player coop. The slot
//    array at +0xE8 holds only 2 entries and is immediately followed by UI
//    component pointers (+0x110 save_game_map, +0x118 campaign_selection_map,
//    +0x120 template_icon_army, +0x128 button_ready, +0x130 button_kick,
//    +0x138 button_invite). A 3rd/4th player would overwrite those => crash.
//    This PoC is for OBSERVATION ONLY: verify the button enables, then revert.

#include <windows.h>
#include <intrin.h>     // _ReturnAddress — used to identify hook callers that have no static xrefs
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "offsets.h"

// ---------------------------------------------------------------- shared state
//
// Defined in main.cpp. g_base is the GAME's image base, so every RVA_* constant is applied as
// g_base + RVA_*; there is no ASLR on this binary but the arithmetic is done properly anyway.
extern HMODULE   g_selfModule;      // OUR dll (for clean FreeLibrary)
extern uintptr_t g_base;            // the GAME's image base (for RVA math)
extern bool      g_patched;
extern uint8_t   g_originalImm;
extern volatile bool    g_watch;
extern volatile uint8_t g_expectedImm;

// The LIVE lobby instance, captured for free by the guard/panel hooks (defined in detour.cpp).
// Always test it with lobbyLooksLive() first - it dangles once the campaign starts.
extern volatile uintptr_t g_liveLobby;

// ---------------------------------------------------------------- log.cpp

void initLogPath();
void resetLog();
void logf(const char* fmt, ...);

// ---------------------------------------------------------------- control.cpp
//
// The named-pipe command channel: every hotkey action, reachable without window focus and without
// a keyboard, returning its own output so a script can branch on the answer. Both are idempotent
// and are called from one site in the probe loop.
bool startControlServer();
// ⚠⚠ MANDATORY BEFORE FreeLibrary, exactly like removeCrashDumper. The pipe thread blocks in a
// SYNCHRONOUS ConnectNamedPipe, so it cannot see a flag on its own; this cancels its I/O and joins
// it. Without it, `detach` unmaps this module out from under a parked thread (#62).
void stopControlServer();
void drainControlCommand();

// ---------------------------------------------------------------- util.cpp

bool gameHasFocus();
bool safeRead(const void* addr, void* out, size_t len);
bool detectChange(bool forceDump);

// A template, so it has to live here rather than in util.cpp.
template <typename T>
static bool readAt(uintptr_t addr, T& out) { return safeRead((void*)addr, &out, sizeof(T)); }

// ---------------------------------------------------------------- detour.cpp

struct Detour {
    uintptr_t target   = 0;
    size_t    len      = 0;
    uint8_t   orig[32] = { 0 };
    uint8_t*  page     = nullptr;   // trampoline lives at page+0x40
    bool      active   = false;
    DWORD     removalProtection = 0; // original protection retained across a failed removal retry
};

bool detourInstall(Detour& d, uintptr_t target, size_t len, const uint8_t* expected,
                   uintptr_t dest, void** outOrig, const char* tag);
void detourRemove(Detour& d, const char* tag);

// ★ The 5-byte variant, for redirecting a `JMP rel32` THUNK rather than hooking a function.
//
// detourInstall needs 14 bytes and would smash whatever follows. A thunk is five, often with
// another thunk immediately after it, so this rewrites the relative displacement in place to reach
// a proximity stub and hands back the address the thunk USED to jump to — already absolute, so no
// trampoline is needed.
//
// ⚠ CURRENTLY UNUSED, and worth knowing why rather than deleting. Its one caller was plan E's CCO
// resolver hook, against a five-byte thunk at 1.7.1's 0x1405BC250 that jumped into the Denuvo
// .xcode blob. 1.7.2 removed Denuvo and the resolver body now sits at that address itself, so
// there is no thunk left to redirect and gift.cpp uses entryRedirect5 below instead. The shape
// recurs — the import tables and the CCO registration accessors are full of five-byte thunks —
// so this is kept for the next one, not retired.
bool thunkRedirect(Detour& d, uintptr_t thunk, uintptr_t dest, uintptr_t* outOriginal,
                   const char* tag);

// ★ Hook a real FUNCTION ENTRY with only five bytes, when detourInstall's fourteen would land
// inside a rel32 and its verbatim trampoline copy would carry a displacement that is only correct
// where it came from. Steals `stealLen` bytes (whole instructions, caller's responsibility, checked
// against `expected`), reaches `dest` via a proximity stub, and returns a trampoline through
// `outOriginal` that behaves exactly like the unhooked function. See detour.cpp for the worked
// case that forced it.
bool entryRedirect5(Detour& d, uintptr_t entry, size_t stealLen, const uint8_t* expected,
                    uintptr_t dest, uintptr_t* outOriginal, const char* tag);
// ★ A page within CALL rel32 reach (±2GB) of `anchor`. Our DLL is ~4.3GB from the image, so any
// patch that must be REACHED by a rel32 — a redirected thunk, or a rewritten call site — needs its
// stub here rather than in the module. Walks outwards at allocation granularity; first page wins.
void* allocNear(uintptr_t anchor);

bool lobbyLooksLive(uintptr_t lobby);
uintptr_t capturedMp();

// Read a CA string (small-string-optimised, so the {len,cap,ptr} layout cannot be assumed) via the
// engine's own accessors. Defined in panels.cpp.
bool readCaString(void* str, char* out, size_t outSz);

// Same, but detects UTF-16 and converts. A lobby player record holds one of each: the faction key
// at +0x18 is narrow, the player name at +0x00 is wide.
bool readCaStringAuto(void* str, char* out, size_t outSz, bool* wasWide = nullptr);

// ---- player display names -----------------------------------------------------------------------
//
// The names exist only while the LOBBY does — it is freed when the campaign starts — so they are
// captured there and served from our own table for the rest of the process's life. Player ids are
// the same in the lobby, the session and a battle (slotObj+0x137C is the identity), so one table
// keyed by id serves all three. Defined in lobby.cpp.
static constexpr uint32_t MAX_NAMED_PLAYERS = 8;    // ids are slots; the mod tops out at 4
static constexpr size_t   NAME_MAX           = 64;

// ⚠ `onGameThread=false` stores the narrow name only and DEFERS the wide CA build, because that
// call allocates and the probe thread may not. The default is true so every existing game-thread
// call site keeps its original behaviour exactly. See the note beside materialisePlayerNameCa.
void        captureLobbyPlayerNames(uintptr_t lobby, bool onGameThread = true);
const char* knownPlayerName(int id);        // "" when we never saw one — for logs
const void* knownPlayerNameCa(int id);      // the engine's own WIDE CA string, or null — for the UI
                                            // ⚠⚠ GAME THREAD ONLY — it may build one on demand

// ============================================================ cross-module surface
//
// Generated from the definitions themselves during the split, so it cannot drift from them.
// The Detour objects are exposed only so status reporting and the F1 panic-unhook can see whether
// a hook is live; nothing outside a module installs another module's hook.

extern bool g_rematchUnlocked;

// turnblend.cpp — the CCO turn gate, relaxed so the management layer (buildings, skill points, army
// stance) works during another human player's turn. A byte patch, not a hook, so it can be armed and
// disarmed live mid-session. OFF by default; an un-armed client is byte-for-byte vanilla.
extern bool g_turnBlendOn;
bool setTurnBlend(bool on);
void reportTurnBlend();

// ★ Per-call-site control, for answering "which of these 21 does that button hang off?" in game
// instead of over a build-deploy-test cycle. Diagnostic: a bare `blend on`/`off` restores policy.
// `why` receives a human-readable outcome either way — including for the refusals, which are the
// interesting cases. index is 1-based, as printed by reportGateSites().
size_t gateSiteCount();
bool   setGateSite(size_t index, bool relax, char* why, size_t whySz);
void   reportGateSites();

// The second half: 20 UI event emitters that withhold their event out of turn, which is why a panel
// shows nothing new until it is closed and reopened. Cosmetic — they notify, they do not mutate.
extern bool g_refreshBlendOn;
bool setRefreshBlend(bool on);

// Arms both halves as soon as their sites have decrypted — nobody should have to press anything to
// play. Called once per main-loop tick; does nothing once settled. `blend off` cancels it, so a
// manual disarm stays disarmed.
void tickTurnBlendAutoArm(uint32_t tick);
void cancelTurnBlendAutoArm();

extern char g_hostName[64];
extern char g_logPath[MAX_PATH];
extern Detour             g_mpDetour;
extern Detour            g_lobbyDetour;
extern Detour          g_panelDetour;

// B9 (#12): the widget-tree-walk guard. On by default — it is a crash fix, not an experiment — but
// switchable, because #48 established that a guard with no off switch cannot be A/B'd.
extern volatile bool   g_treeWalkGuard;
extern Detour          g_treeWalkDetour;
bool installTreeWalkGuard();
void removeTreeWalkGuard();
void reportTreeWalkGuard();   // #12 - refusals + culprit reports, for `capture`
long treeWalkRefusals();
extern Detour        g_hudCtorDetour;
extern Detour        g_joinDetour;
extern Detour        g_resetDetour;
extern Detour        g_seatDetour;
extern Detour        g_shareDetour;
extern Detour        g_slotChDetour;
extern Detour       g_tickDetour;
extern uint32_t g_lastReadyMask;
extern volatile bool g_autoLendingSpectator;   // on by default: spectators can receive lent units
extern volatile bool g_giftPanelMode;
extern volatile bool g_giftPanelWanted;

// ★ Pipe -> probe-loop requests. The pipe thread sets these; the loop in main.cpp drains and acts.
// Set from anywhere, acted on in one place: `panic` tears down fourteen detours and `detach` breaks
// the loop that runs the restore-everything tail, and neither is safe to do from the pipe thread
// while the loop is mid-pass. Same shape as `queueFactionRequest`.
extern volatile long g_panicRequest;
extern volatile long g_detachRequest;
extern PVOID         g_crashWitnessHandle;   // ⚠ needed to REMOVE the VEH before unload — #62
// ⚠⚠ DELIBERATELY KILLS THE GAME. `crash test <kind> --yes` sets this to a CrashTestKind; the probe
// loop drains it and faults on purpose, so the recorder can be proven inside the running GAME rather
// than in a synthetic test host. It is drained on the probe thread for the same reason `panic` is —
// and for one more: a fault raised on the pipe thread would die with the reply half-written, so the
// evidence would be the one thing missing. See performCrashTest in crashdump.cpp.
extern volatile long g_crashTestRequest;
extern volatile long   g_panelCalls;
extern volatile long g_extraSeats;
// ⚠ A SLOT INDEX, not a player id — proven from FUN_140471B30, which bounds-checks it against the
// slot entry count (+0x1360) and scales it by the slot stride (0xF8). Arm it through
// slotForPlayerPublic(), never with a raw number a human typed.
extern volatile long g_giftTargetIdx;
int slotForPlayerPublic(int playerId);
extern volatile long g_panelPushFails;
extern volatile long g_panelsAdded;

bool installCcoHijack();      // plan D: hijack DevCycleArmy inside the vanilla array
void removeCcoHijack();
bool installCcoResolverHook();
// #73 — also reports CALL VOLUME through the resolver, which is what says whether a slow UI panel
// is ours or vanilla's.
void reportCcoResolver();  // plan E: answer for names the engine's own scan cannot find
void removeCcoResolverHook();
void reportCcoHijack();
bool installHudCtorHook();
bool installBattleRoleHook();   // §6hhh: read-only, who the battle waits on and what they chose
void removeBattleRoleHook();
void reportBattleRoles();
void dumpPendingBattleState();  // B1: press F3 while a turn is locked
// ★★★ #13 / B10 — read-only. The pending-battle manager's faction-key list, and which entry
// `FUN_141853A90` falls back to when the asking faction has no record of its own (every bystander).
// That fallback ignores the faction it was asked about and takes the FIRST entry with `+0x12 == 0`,
// which is the leading candidate for a spectator's vote landing in the AI attacker's record. Fires
// from the pre-battle role hook, from `capture`, and on demand as `pending`.
void dumpPendingBattleChoice();
void dumpTurnState();           // B1: whose turn does this client think it is? (probes.cpp)
void dumpEndTurnNotifications();// B1: is an end-turn notification holding the turn? (probes.cpp)
int  movementsInFlight();       // B1 clause (B): movements the engine still has in flight, or -1
int  autosavePending();         // B1: rootObj+0x19E — an autosave requested and not yet taken
void clearTurnGateBlockList(bool force);  // B1 RECOVERY: empty clause (D)'s list. EVERY machine, or none.
// B1 RECOVERY, the surgical one (#43): submit the choice instead of abandoning it. ONE machine only
// — the command carries no record id, so every client resolves its own guard from it.
void reportPendingDilemma();    // read-only: what is pending, how many options, whose it is
void answerPendingDilemma(int option);   // 1-based, as read off the screen. Queues; drains on tick.
void drainAnswerRequest();      // called from the command executor hook — GAME THREAD ONLY
void watchCampaignTurns();      // B1: logs turn handovers and human-set changes, no keypress needed
void dumpLobbyMapWidgets(uintptr_t lobby);   // B8: is save_game_map a widget, or a smashed pointer?
bool installCommandExecHook();  // B1: read-only, what the campaign is being told to do
void removeCommandExecHook();
bool commandExecHookActive();
// ★ The campaign tick — the game-thread drain for `answer`. The command executor was tried first and
// does NOT tick: it is called only when there are commands to execute, so an idle campaign (a modal
// dilemma box, nothing submitted) never reaches it. Measured 2026-08-07.
bool installCampaignTickHook();
void removeCampaignTickHook();
bool campaignTickHookActive();
void reportCommandTraffic();    // B1: the per-command counts, and the delta since the last capture
// ---------------------------------------------------------------- eventcursor.cpp
// #63: `mgr+0x1F0` is `u32[2]` and `+0x1F8` is a live field, so a 3rd human's end turn would write
// its cursor over the list size. These two prologue detours borrow and restore that field, and the
// index-gate byte goes with them — a widened gate over a two-slot array is the corruption itself.
// ⚠ PROVABLY INERT below 3 humans: the index is bounded by humanFactionCount, so it cannot reach 2.
bool installEventCursorHooks();
void removeEventCursorHooks();
void reportEventCursors();
// ---------------------------------------------------------------- telestration.cpp
// #47/#50: pings and battle field-lines are ONE subsystem — internally **telestration** — and it
// admits a fixed participant set chosen once at battle construction. Measured rule (§6uuu.38):
// {the combatant} u {the lowest-indexed spectator}, exactly two, everyone else silently gets nothing.
// The probe half is READ-ONLY and always on: it reports WHICH KEYS this client holds, and it is what
// killed the two earlier mechanisms ("it's that machine"; "our +0x10/+0x11 half-state picks the wrong
// branch", tested with `lend off`).
// ★ The cause is `FUN_142E9DFC0`: it fetches the whole share player list and returns `list[0]` — the
// same idiom as the gift bug at an adjacent address. So `tel fix` inserts the entries the constructor
// never asked for, using the constructor's own calls. ⚠ It WRITES (heap, at battle construction) and
// therefore ships DISARMED — but it is a UI object fed by chat, not lockstep state, so a client that
// misses it fails locally and cannot desync anyone.
extern Detour g_telestrationDetour;
bool installTelestrationHook();
void removeTelestrationHook();
void reportTelestration();
bool setTelestrationFix(bool on, const char** why);
bool telestrationFixArmed();
// ---------------------------------------------------------------- feedicon.cpp
// #69: `FUN_142F5EBA0` (`IconPath` on an event-feed entry) dereferences the property bag BEFORE the
// null check on its source item — three crashes on 2026-08-14, all at `exe+0x2F5EBFC`, all with
// RAX=0/RDI=0. 17 bytes are reordered so the check comes first; the load it skips is provably dead
// on that path. ARMED AT ATTACH, because it changes behaviour only where the game currently dies.
// ⚠ It removes the crash, not the cause — see the report.
bool patchFeedIconGuard();
void unpatchFeedIconGuard();
bool feedIconGuardArmed();

// ---------------------------------------------------------------- sidegate.cpp
//
// ★ #13/B10. ARMED AT ATTACH, no switch: the battle descriptor's `humans == 2` test is widened to
// `humans >= 2`, so a player with no army in the fight is given the HUMAN participant's army instead
// of `armies[0]` — which is the attacker. One byte, a no-op at two humans by construction.
// ⚠⚠ EVERY MACHINE OR NONE. See sidegate.cpp for the whole argument and §6uuu.53 for the measurement.
bool patchBystanderSideGate();
void unpatchBystanderSideGate();
bool bystanderSideGateArmed();

// ---------------------------------------------------------------- updguard.cpp
//
// ★ #75. DISARMED at attach — `updguard on|off`. `FUN_142509A40` is the only caller of the registry
// lookup `FUN_142561EF0` that does not test its result for null, at all THREE of its call sites, and
// a miss is a normal outcome the engine's other callers have a dedicated fallback for. Nine bytes
// per site turn `lea rcx,[rax+0x40]` into `mov rcx,rax` + a call to our stub, which skips the entity
// (writing the three zeroes the engine writes for an empty input) and logs it.
// ⚠ #75 names site `0x142509C27`; the crash registers say site `0x14250A167`. All three are guarded.
// ⚠⚠ EVERY MACHINE OR NONE. See updguard.cpp for the whole argument.
bool patchUpdateNullGuard();
void unpatchUpdateNullGuard();
bool updateNullGuardArmed();
bool setUpdateNullGuard(bool on, const char** why);
void reportUpdateNullGuard();
bool setFeedIconGuard(bool on, const char** why);
void reportFeedIconGuard();
// ---------------------------------------------------------------- crashdump.cpp
// The half `crashWitness` (main.cpp) cannot do: a minidump written from inside the dying process.
// The witness is a VEH and sees FIRST-CHANCE faults, so it must stay cheap — a dump there would
// stall the process on exceptions the game was about to handle, which in a lockstep session is a
// desync risk. So the dump hangs off SetUnhandledExceptionFilter instead, which runs only once
// nothing has handled the fault and the process is already dying.
// ⚠ rearmCrashDumper() is NOT optional — the game installs its own filter after ours and displaces
// it. The probe loop re-takes it every ~2 s and chains to whoever displaced us, so the game's own
// handler still runs and still decides the outcome. Changes no execution.
bool        installCrashDumper();
void        rearmCrashDumper();
// ⚠⚠ MANDATORY BEFORE FreeLibrary — see the detach tail in main.cpp and #62. Restores the filter we
// displaced (only if it is still ours) and gets the parked dumper thread out of this module. Leaving
// either behind points the OS's own handler chain at an unmapped page, which kills the game on the
// next exception rather than at detach — and takes the recorder with it, so nothing records it.
// ★ `panic` deliberately does NOT call this: it removes hooks but does not UNLOAD, so the handlers
//   stay valid, and a read-only recorder is exactly what you want still armed when things go wrong.
//   That asymmetry with the panic list is intentional, not drift.
void        removeCrashDumper();
// ⚠⚠ FAULTS ON PURPOSE AND DOES NOT RETURN. The one thing a test host cannot prove is that our
// last-chance filter is still ours once the GAME has installed its own — so this provides a fault on
// demand, inside the game, at a moment somebody chose. It logs the recorder's state first, so the log
// says what the re-arm count was at the instant of the fault rather than whenever anyone last asked.
enum CrashTestKind { CRASH_TEST_AV = 1, CRASH_TEST_OVERFLOW = 2 };
void        performCrashTest(long kind);
void        setCrashDumpFullMemory(bool on);
bool        crashDumperInstalled();
bool        crashDumperChained();
bool        crashDumpFullMemory();
long        crashDumperRearms();
long        crashDumpsWritten();
const char* crashDumperLastDump();
// Defined in main.cpp beside `crashWitness`, which owns the per-signature fault table. Distinct
// (code, address) pairs vs every fault seen — a large total against a small kind count is a fault
// the game is handling in a loop, which is normal traffic and not a crash.
long        faultKindsSeen();
long        faultsSeenTotal();
// gift.cpp — slot index -> player id. ⚠ The two spaces diverge the moment anyone rejoins (#45), so
// cross between them with this rather than indexing by position. Returns -1 when no player claims it.
int  playerForSlot(int slot);
// ★★★ #68 — the third human is absent from HUMAN_FACTIONS after a save reload, on every client.
// The loader `FUN_1414D0780` caps nothing but SILENTLY rolls the count back for any saved id that
// fails to resolve. This sits on the resolver `FUN_141457760`, filtered to the loader's call site,
// so the number of calls IS the number of ids in the stream and each result says whether it
// survived. Read-only, one line per entry; fires only on the load path.
bool installHumanFactionLoadHook();
void removeHumanFactionLoadHook();
void reportHumanFactionLoad();
// ⚠ WRITES replicated model state. Ships DISARMED, refuses to arm without the cursor hooks, and
// raises `humanFactionCount` to a RULE — the number of leading registered human factions — so every
// client computes the same answer from replicated data rather than being told a number.
bool setHumanFactionCountHold(bool on, const char** why);
// ★ #68 — re-register a human the save came back without. ⚠ WRITES replicated model state and may
// REALLOCATE the registry through the engine's allocator. Ships DISARMED and armed SEPARATELY from
// the count hold, so the two can be A/B'd: shipping them fused made a crash unattributable.
bool setHumanFactionRepair(bool on, const char** why);

// The search bound (2026-08-15). Repoints ONE displacement byte in FUN_1414E0E40 so the registry
// search runs to humanFactionCap instead of humanFactionCount. Writes no model state — see the long
// note in eventcursor.cpp. Ships DISARMED.
bool setHumanFactionSearchBound(bool on, const char** why);
bool humanFactionSearchBoundArmed();
bool humanFactionCountHoldArmed();
void tickHumanFactionCountHold();   // called from the campaign tick, game thread
void reportHumanFactionCountHold();
// ★ The event-feed ticker, FUN_142FCC8E0 — a DETOUR since 2026-08-10, replacing the 10 s sampler
// that hunted the object with an 11.8 GB sweep of committed private memory. Read-only. It exists
// because the feed's resting state and its stuck state are THE SAME BYTES, so only a transition
// discriminates and only a hook sees one. Full reasoning above feedTickHook in probes.cpp.
bool installFeedTickHook();
void removeFeedTickHook();
// ★★★ #60 — the NOTIFICATION gate. `FUN_142FCC780` (the observer that rebuilds the feed context and
// raises +0x14C) runs only if the faction holding the turn is HUMAN. In single player that is the
// same test as the listener's guard; in coop they come apart. Hooked on the predicate and filtered
// by return address, because 142FC0450 itself cannot be stolen from (RIP-relative 4th instruction).
bool installFeedGateHook();
void removeFeedGateHook();
void reportFeedGate();
bool installFactionInListHook();
void removeFactionInListHook();
void reportFactionInList();
bool installNextAutoOpenHook();
void removeNextAutoOpenHook();
void reportNextAutoOpen();
void reportFeedTicker();        // B1: was a box ever ASKED for, and which half of the cycle stalled
void setFeedLogging(bool on);   // `feed on|off` — the counts accrue regardless
// ★★★ `feed dirty` — ⚠ WRITES one byte. Sets the CCO context's own stale flag (`self+0x88`), which
// FUN_1405B5BC0 tests at the top of both list getters; the engine then recomputes the list through
// its own virtual at its next query. B1's candidate fix, and the reason `repop` never worked —
// firing a named event never marked the context stale. Full reasoning above markFeedDirty.
bool markFeedDirty(char* why, size_t cap);
// ★★★ `feed refresh` — ⚠ CALLS THE ENGINE. Measured 2026-08-13: marking the context stale is not
// enough, because the refresh is a PULL and a stuck client never queries the property (the flag sat
// at 1 for 90 s untouched). This queues a call to the engine's own `FUN_1405B5BC0(self+0x48)`,
// drained on the UI thread by the ticker hook. Byte-verified, one-shot, SEH-wrapped.
bool requestFeedRefresh(char* why, size_t cap);
// ★★★ `feed rebuild` — ⚠ CALLS THE ENGINE. LEVEL 1: forces FUN_142FC0770 to rebuild the SOURCE from
// model+0x3D30 for the local faction, via the engine's own UNCONDITIONAL refresh FUN_14057B720 — the
// same call FUN_142FBFE20 makes on the same sub-object, so no flag has to be forged. The source count
// before vs after is the answer: it fills (stale level-1 cache) or it does not (FUN_1414E0E40 is
// rejecting this faction's own dilemma, and B1 is model-side).
bool requestSourceRebuild(char* why, size_t cap);
bool installJoinHook();
void resetSlotExpansionBudget();
void resetAutoFactionBudget();
void resetPanelCompletionState();
bool installLobbyGuard();
bool installMpHook();
bool installPanelHook();
bool installPanelResetHook();
bool installSeatHook();
bool installShareHook();
bool installSlotChangedHook();
bool installTickHook();
extern Detour g_saveLobbyDetour;
bool installSaveLobbyHook();
void removeSaveLobbyHook();
bool saveLobbyFixArmed();
void setSaveLobbyFix(bool on);
void tickSaveLobby(uintptr_t lobby);
// S17: fresh-lobby registration uses the same guarded native record writer.
bool refreshFreshLobbyRecords(uintptr_t lobby);
bool installFreshFactionHook();
void removeFreshFactionHook();
void tickFreshLobbyFactions(uintptr_t lobby, bool notification = false);
bool extraPanelRepairArmed();
bool installLeaveCacheGuard();
void removeLeaveCacheGuard();
bool leaveCacheFixArmed();
void setLeaveCacheFix(bool on);
void setExtraPanelRepair(bool on);
void tickExtraLobbyPanels(uintptr_t lobby);
bool setSaveLobbyWatch(bool on);
void captureSaveLobbyWatch(uintptr_t lobby);
void reportSaveLobbyWatch(char* reply, size_t size);
void refreshSavedExtraPanels(uintptr_t lobby, uintptr_t slots);
// The refresh owns a TWO-entry inline cache. Never grow its records inside that call.
extern thread_local bool g_inLobbyCacheRefresh;
bool setRematchUnlock(bool on);
bool verifyAllSignatures(bool verbose, int* unreadable = nullptr, int* mismatched = nullptr);
int localPlayerId();
void drainFactionRequest();
void dumpEverything();
void dumpInstanceDeep(uintptr_t inst);
void dumpLobbyState();
void dumpMpSession();
void dumpBattleSides();         // B10: whose units does this client think are friendly? (battle.cpp)
void dumpUnitCensusOnDemand();
void reportCcoQueryCounts();   // has the UI ever asked for our CcoBattleRoot entries?
void expandSlots();
void maybeAutoAssignFaction();
// advance=true  : a person pressed F8 after watching it not work — try the NEXT unused faction.
// advance=false : an automatic retry — ask for the SAME faction again. See lobby.cpp for why the
//                 distinction matters (a retry means "the message has not landed", not "refused").
void queueFactionRequest(bool advance);
#ifndef TW3K_RELEASE
bool queueFactionByKey(const char* key);   // debug pipe `faction set <key>`
#endif
void removeHudCtorHook();
void removeJoinHook();
void removeLobbyGuard();
void removeMpHook();
void removePanelHook();
void removePanelResetHook();
void removeSeatHook();
void removeSlotChangedHook();
void removeTickHook();
void reportLobbyGuard();
void reportPlayerVectorIfChanged(uintptr_t lobby);
void watchLobbyPlayers();       // B4: samples the lobby id set on OUR clock, not the engine's
void reportReadyMaskIfChanged();
void reportSeatHook();

// Game-thread lifecycle boundaries; clears only mod-owned gift UI state.
void resetGiftPanelState(const char* reason);
