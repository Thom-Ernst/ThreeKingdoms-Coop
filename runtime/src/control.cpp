#ifndef TW3K_RELEASE
// control.cpp - a named-pipe command channel, so the mod can be driven without a keyboard.
//
// WHY THIS EXISTS
//
// Every action this DLL can take is currently bound to a function key, and that has cost real
// sessions:
//
//   * RUN32 §2 "never ran (the game must be FOCUSED for a hotkey)"
//   * `GetAsyncKeyState` reads GLOBAL key state, so over remote desktop ONE keypress fired on TWO
//     machines - that is how the host once switched to sun_jian by itself, and why every hotkey is
//     now gated on gameHasFocus()
//   * F2 is unusable at all, because the game binds it to its own wiki
//   * F7 must be pressed on EVERY machine or none, and three humans pressing three keyboards is
//     the least reliable part of the whole procedure
//
// A pipe has none of those problems. It is addressed to ONE process by construction, needs no
// window focus, cannot collide with a game binding, and - unlike a keypress - it can RETURN
// something, so a script can branch on the answer instead of a person reading a log afterwards.
//
// ⇒ The point is not convenience. It is that a test can become a SCRIPT: RUN33's Tier 1 is
//   "capture, assert +0x48 == -1, answer, assert +0x48 == 1", and that is executable over this.
//
// DESIGN, and the two rules it follows
//
//  1. THE PIPE THREAD NEVER TOUCHES THE GAME. It reads a line, hands it to the probe thread, and
//     waits. The probe thread is the one that already runs the hotkeys, so a command executes on
//     exactly the same thread, at exactly the same point in the loop, as the key it mirrors. No new
//     concurrency is introduced, and nothing here needs its own reasoning about what is safe to
//     call from where - if the key was safe, the command is safe, for the same reason.
//
//  2. THE REPLY IS THE LOG TAIL. These functions report by calling logf() rather than returning
//     strings, so instead of rewriting them, this notes the log's size before running the command
//     and returns everything appended after. That means a command's answer is the same text the log
//     would have shown, from every module, with no changes to any of them.
//     ⚠ It also means a line logged by another thread during the command lands in the reply. That
//       is a cosmetic race, and preferable to duplicating a dozen report functions.

#include "tw3k.h"
#include "ui.h"
#include <cstdlib>      // _strtoui64 / atoi, for the `read` command

// Same generated stamp main.cpp prints at attach, and for the same reason: __DATE__/__TIME__ say
// WHEN a DLL was compiled and nothing about WHAT from. `ping` is how a script proves all three
// machines run one build, so it has to report the identifier that can actually answer that.
#if defined(__has_include)
#  if __has_include("build_info.h")
#    include "build_info.h"
#  endif
#endif
#ifndef TW3K_GIT
#  define TW3K_GIT "unknown - built without build.bat"
#endif

// A single slot is enough: commands are issued one at a time, and serialising them is a feature -
// two captures interleaving would produce a log nobody could read.
enum ControlState { CTRL_IDLE = 0, CTRL_PENDING, CTRL_DONE };

static volatile long  g_ctrlState = CTRL_IDLE;
static char           g_ctrlCmd[512];
static char           g_ctrlReply[64 * 1024];
static volatile bool  g_ctrlServerUp = false;
// Keep the worker handle for a bounded join. Failed waits retain it until process exit.
static volatile long  g_ctrlExit     = 0;
static HANDLE         g_ctrlThread   = nullptr;

// The pipe name is fixed rather than per-pid. Only one game runs per machine, and the armed mutex
// already refuses a second injection into one process - so a fixed name is unambiguous, and it
// means a caller never has to discover a pid first.
static const char* kPipeName = "\\\\.\\pipe\\tw3k_coop";

// ---------------------------------------------------------------- log tail capture

static long long logSizeNow()
{
    // Share WRITE as well as READ: logf holds the file with FILE_SHARE_READ while it appends, and
    // an opener that does not permit the existing writer is refused outright.
    for (int attempt = 0; attempt < 3; ++attempt) {
        HANDLE f = CreateFileA(g_logPath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz{};
            const BOOL ok = GetFileSizeEx(f, &sz);
            CloseHandle(f);
            if (ok) return (long long)sz.QuadPart;
            return -1;
        }
        Sleep(5);
    }
    return -1;
}

static void logTailSince(long long from, char* out, size_t outSz)
{
    out[0] = '\0';
    if (from < 0) return;

    HANDLE f = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3 && f == INVALID_HANDLE_VALUE; ++attempt) {
        f = CreateFileA(g_logPath, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) Sleep(5);
    }
    if (f == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= from) { CloseHandle(f); return; }

    long long len = sz.QuadPart - from;
    // Keep the TAIL when a capture overflows the buffer: the verdict lines a full capture ends with
    // are what a caller is reading for, and truncating the front loses the header instead.
    long long start = from;
    if (len > (long long)outSz - 1) { start = sz.QuadPart - ((long long)outSz - 1); len = (long long)outSz - 1; }

    LARGE_INTEGER mv{}; mv.QuadPart = start;
    if (SetFilePointerEx(f, mv, nullptr, FILE_BEGIN)) {
        DWORD got = 0;
        if (ReadFile(f, out, (DWORD)len, &got, nullptr)) out[got] = '\0';
    }
    CloseHandle(f);
}

// ---------------------------------------------------------------- the commands
//
// Everything below runs ON THE PROBE THREAD. Adding a command here is the same act as adding a
// hotkey, and carries the same obligations.

static void appendf(char* buf, size_t bufSz, const char* fmt, ...)
{
    const size_t used = strlen(buf);
    if (used + 2 >= bufSz) return;
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf + used, bufSz - used, _TRUNCATE, fmt, args);
    va_end(args);
}

// Word n of the command line, lowercased, or "" when absent.
static void argAt(const char* cmd, int n, char* out, size_t outSz)
{
    out[0] = '\0';
    const char* p = cmd;
    for (int i = 0; i <= n; ++i) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) return;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (i == n) {
            size_t len = (size_t)(p - start);
            if (len >= outSz) len = outSz - 1;
            for (size_t k = 0; k < len; ++k) {
                char c = start[k];
                out[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
            }
            out[len] = '\0';
            return;
        }
    }
}

static bool argIs(const char* cmd, int n, const char* want)
{
    char a[64];
    argAt(cmd, n, a, sizeof(a));
    return strcmp(a, want) == 0;
}

static void doRead(const char* cmd, char* reply, size_t replySz)
{
    char addrTxt[64], lenTxt[32];
    argAt(cmd, 1, addrTxt, sizeof(addrTxt));
    argAt(cmd, 2, lenTxt, sizeof(lenTxt));
    if (!addrTxt[0]) { appendf(reply, replySz, "ERR read needs an address, e.g. `read 0x1F47559B0 64`\n"); return; }

    // ❌★ ALWAYS BASE 16, and the previous `base 0` was a trap worth naming. Base 0 reads a leading
    // `0` as OCTAL — so `00000000F3C37270`, which is EXACTLY the form every address in our own logs
    // is printed in, parsed as octal, stopped at the first `F`, and returned 0. The reply then said
    // *"0x0 is not readable in this process"*: a confident, wrong, plausible answer to a correctly
    // typed address. Cost a round trip on a live B1 before anyone looked at the parse.
    // ⇒ Addresses here are hex by definition; `0x` is accepted and optional, decimal is not a thing.
    const char* ap = addrTxt;
    if (ap[0] == '0' && (ap[1] == 'x' || ap[1] == 'X')) ap += 2;
    char* aEnd = nullptr;
    unsigned long long addr = _strtoui64(ap, &aEnd, 16);
    if (aEnd == ap) {
        appendf(reply, replySz, "ERR `%s` is not a hex address. `0x` is optional; digits are hex.\n",
                addrTxt);
        return;
    }
    int n = lenTxt[0] ? atoi(lenTxt) : 64;
    if (n <= 0)  n = 64;
    if (n > 512) n = 512;   // a reply is a diagnostic, not a dump. Use a process dump for bulk.

    unsigned char buf[512];
    if (!safeRead((void*)(uintptr_t)addr, buf, (size_t)n)) {
        appendf(reply, replySz, "ERR 0x%llX is not readable in this process\n", addr);
        return;
    }
    for (int i = 0; i < n; i += 16) {
        char hex[80] = { 0 }, txt[24] = { 0 };
        const int row = (n - i < 16) ? (n - i) : 16;
        for (int k = 0; k < row; ++k) {
            char one[4];
            _snprintf_s(one, sizeof(one), _TRUNCATE, "%02X ", buf[i + k]);
            strcat_s(hex, sizeof(hex), one);
            txt[k] = (buf[i + k] >= 32 && buf[i + k] < 127) ? (char)buf[i + k] : '.';
        }
        appendf(reply, replySz, "  %016llX  %-48s %s\n", addr + i, hex, txt);
    }
}

static void executeControl(const char* cmd, char* reply, size_t replySz)
{
    reply[0] = '\0';

    if (argIs(cmd, 0, "ui")) {
        executeUiControl(cmd, reply, replySz);
        return;
    }

    if (argIs(cmd, 0, "ping") || !cmd[0]) {
        appendf(reply, replySz, "OK %s MODE=DEBUG pid=%lu BUILD=%s %s SOURCE=%s\n",
                g_hostName, GetCurrentProcessId(), __DATE__, __TIME__, TW3K_GIT);
        appendf(reply, replySz, "B4 save-lobby: hook=%s fix=%s\n",
                g_saveLobbyDetour.active ? "active" : "absent", saveLobbyFixArmed() ? "ARMED" : "off");
        appendf(reply, replySz, "B4 panel display: %s\n", extraPanelRepairArmed() ? "ARMED" : "off/signature-refused");
        appendf(reply, replySz, "B4 leave-cache: %s\n", leaveCacheFixArmed() ? "ARMED" : "off/signature-refused");
        return;
    }

    if (argIs(cmd, 0, "help")) {
        appendf(reply, replySz,
                "commands (read-only unless marked):\n"
                "  help                  this list. It is the AUTHORITY - it answers out of the DLL\n"
                "                        that is loaded, so unlike any doc it cannot be stale\n"
                "  ping                  build, machine, pid - use this to prove one build on all machines\n"
                "  ui arm|disarm|status  TEST ONLY; arm requires tw3k_ui_test.flag in working directory\n"
                "     tree <path> [0..8]|find <name>|click <path>|key esc|text <path> <text>|players|quit\n"
                "                        exact component Id paths; disarmed by default; engine input-phase queue\n"
                "     tree/find TSV: path visible disabled state text; synthetic #N adds escaped raw Id\n"
                "     #N is a zero-based direct-child index for unaddressable Ids; literal collisions refuse\n"
                "     TSV escapes: \\ \\t \\r \\n \\xHH; players: actual id, readable/captured name or no name\n"
                "     running timeout: never retry; completion disarms, inspect UI then explicitly arm again\n"
                "  status                hook signatures, local player id, whether a lobby is live\n"
                "  savelobby [on|off]    B4 saved-player records and panels; ON by default.\n"
                "  savelobby panels [on|off]  Initial human status + hidden-panel retry, ON by default.\n"
                "  savelobby close [on|off]   Extra-seat leave-cache redirect, ON by default.\n"
                "                        off restores the corrupting leave path; use only for controlled A/B.\n"
                "  savelobby watch [on|off]   Diagnostic hardware writer watch, OFF by default.\n"
                "                        Arm in a host-only save lobby before peers join; see B4 WATCH log.\n"
                "  crash [status]        the crash recorder: is the last-chance filter still ours,\n"
                "                        how many faults, where the last dump went. Read-only, and\n"
                "                        safe in MP - nothing is written on a handled fault\n"
                "  crash heap [on|off]   swap the crash dump between compact (few MB) and full\n"
                "                        memory (~8.6 GB). Per-process; dies with the game\n"
                "  crash test [av|overflow] --yes\n"
                "                        ⚠⚠ FAULTS ON PURPOSE and the game dies. The only way to\n"
                "                        prove the recorder in the GAME - a test host installs no\n"
                "                        competing filter. SINGLE PLAYER, one machine\n"
                "  capture               the full capture, exactly as F3 writes it\n"
                "  turn                  turn state, movements in flight, autosave pending\n"
                "  session               MP session dump (slots, flags, advertised counts)\n"
                "  lobby                 lobby state\n"
                "  commands              per-command traffic counts and the delta since last time\n"
                "  read <addr> [n]       hex dump up to 512 bytes of THIS process, SEH-guarded\n"
                "  faction [next]        request a faction. `next` advances to another one; bare\n"
                "                        re-asks for the SAME one, which is what a retry wants\n"
                "  faction set <key>     request that faction (full key or e.g. yuan_shao)\n"
                "  gift <player|off>     aim the next gift at a PLAYER id. It is converted to that\n"
                "                        player's SEAT, which is the space the command field is in;\n"
                "                        refuses if they have no seat. `session` prints the table.\n"
                "  dilemma               what decision is pending, how many options, whose it is\n"
                "  pending               #13: the pending-battle key list, and WHICH faction's record\n"
                "                        a player with no army in the fight would have their vote\n"
                "                        written into. Read-only. Run it while the PRE-BATTLE PROMPT\n"
                "                        is up — the list is empty at any other time.\n"
                "  feed [on|off|         B1: the event-feed ticker. No arg = the counts, and whether\n"
                "       dirty|refresh|   \n"
                "       rebuild|icon]    \n"
                "                        a box was ever ASKED for. `off` silences the per-transition\n"
                "                        lines; the counts accrue either way. `on`/`off` write nothing.\n"
                "                        ⚠ `dirty` WRITES one byte: it marks the feed's context stale\n"
                "                        (self+0x88), which is the flag the engine's own getters test\n"
                "                        before recomputing the list. B1's CANDIDATE FIX — the engine\n"
                "                        then rebuilds the list itself at its next query.\n"
                "                        ⚠⚠ `refresh` CALLS THE ENGINE's own FUN_1405B5BC0 on the UI\n"
                "                        thread. `dirty` alone is NOT enough on a stuck client: the\n"
                "                        refresh only runs inside a getter and a stuck client never\n"
                "                        calls one, so the flag just sits there. Run `dirty` first.\n"
                "                        ⚠⚠ `rebuild` is LEVEL 1 and upstream of both: it forces the\n"
                "                        SOURCE to be rebuilt from the campaign model for the LOCAL\n"
                "                        FACTION, via the engine's own unconditional FUN_14057B720.\n"
                "                        RUN IT FIRST — level 2 rebuilding from an empty source\n"
                "                        proves nothing. Source count before vs after is the answer.\n"
                "                        ★ `icon` is #69 and is ARMED AT ATTACH: 17 bytes of the\n"
                "                        IconPath getter are reordered so it null-checks its source\n"
                "                        item BEFORE dereferencing the property bag. `feed icon off`\n"
                "                        restores the VANILLA CRASH — it is the A/B, not the safer\n"
                "                        setting. It removes the crash, NOT the cause.\n"
                "  updguard [on|off]     #75. OFF by default. Restores the null check that\n"
                "                        FUN_142509A40 omits at all THREE of its call sites: a\n"
                "                        registry miss is a normal outcome its sibling callers\n"
                "                        handle, and this one dereferences 0x40 instead. Skips\n"
                "                        the entity, zeroes the three outputs the engine zeroes\n"
                "                        for an empty input, and LOGS the entity that had no\n"
                "                        record. ⚠⚠ EVERY MACHINE OR NONE. No arg = the counters.\n"
                "  lend [on|off|guard]   WRITES. Auto-arm a SPECTATOR to receive lent units. ON by\n"
                "                        default. `lend off` is the VANILLA control for #48, and it\n"
                "                        takes effect on the NEXT battle load, not immediately.\n"
                "                        ✂ `lend guard` is GONE -- its predicate was wrong. #13/B10\n"
                "                        is fixed in the descriptor builder instead, armed at attach\n"
                "                        with no switch; `status` reports it.\n"
                "  tel [status]          #47/#50 pings and battle field-lines - ONE subsystem, called\n"
                "                        telestration. No arg = the participant map, read live.\n"
                "  tel fix on --yes|off  ⚠ ALLOCATES + WRITES the map at battle construction, with the\n"
                "                        constructor's own calls, for every player it skipped -- plus\n"
                "                        this client's own entry at mgr+0x78, without which an excluded\n"
                "                        player still cannot draw. Ships OFF; NEXT battle, not this one.\n"
                "                        NOT replicated state (a UI object fed by chat), so it cannot\n"
                "                        desync -- but a client without it cannot see those strokes.\n"
                "  blend [on|off]        ⚠ WRITES. Management layer (buildings, skill points, army\n"
                "                        stance) usable during ANOTHER PLAYER's turn, AND the UI events\n"
                "                        that make panels update in place. No arg = report.\n"
                "                        `blend gate on|off` / `blend refresh on|off` move one half.\n"
                "                        `blend sites` lists the 22 call sites and what each points at;\n"
                "                        `blend site <n> on|off` moves exactly ONE - which is how you\n"
                "                        find out which site a given button hangs off, without a rebuild.\n"
                "                        PER MACHINE - it only affects the client it is sent to.\n"
                "  hfcount repair on --yes|off  ⚠ WRITES + MAY REALLOCATE. #68: puts back a human\n"
                "                        the save came back without. Armed SEPARATELY from the count\n"
                "                        hold so the two can be A/B'd. Ships OFF. EVERY MACHINE OR NONE.\n"
                "  hfcount bound on --yes|off   PATCHES ONE CODE BYTE, writes NO model state. The\n"
                "                        registry SEARCH in FUN_1414E0E40 runs to humanFactionCap\n"
                "                        instead of humanFactionCount, so a human registered past the\n"
                "                        count is found without raising it. Cannot desync; survives a\n"
                "                        load; needs no re-arming. Ships OFF. Does NOT fix storage.\n"
                "  hfcount [status|on --yes|off]  ⚠ WRITES, and ARMS ITSELF AT ATTACH since\n"
                "                        2026-08-15 — you do not need to run it. #63: raises\n"
                "                        humanFactionCount to the number of registered human\n"
                "                        factions, so a 3rd/4th player's events are collected at all.\n"
                "                        `off` stops future raises; the present count stays until load.\n"
                "                        EVERY MACHINE OR NONE still holds, and is now satisfied by\n"
                "                        construction — every client on this build arms identically.\n"
                "                        Requires the campaign tick and cursor hooks; no-op below 3 humans.\n"
                "  answer <n> --yes      ⚠ WRITES. ANSWERS the pending dilemma with option n (1-based).\n"
                "                        ONE MACHINE ONLY - the owning one. `dilemma` says which.\n"
                "  unblock --yes [--force]   ⚠ WRITES. Discards a pending decision.\n"
                "                            EVERY MACHINE OR NONE - one machine alone desyncs the session.\n"
                "                            ★ Prefer `answer`: it RESOLVES the decision instead of\n"
                "                            throwing it away, and a healthy hold of 187 s has been\n"
                "                            measured, so no timeout can tell the two apart.\n"
                "  panel [on|off]        Gift panel mode. `off` is the ESCAPE HATCH - the gift icon\n"
                "                        gifts directly again using `gift <id>`. No arg = report.\n"
                "  treewalk [on|off]     B9 (#12) guard: a corrupt widget child list makes the name\n"
                "                        lookup FAIL instead of crashing the client. ON by default.\n"
                "                        No arg = state + refusal count. ⚠ `off` restores the VANILLA\n"
                "                        CRASH - it is the A/B, not a safety setting.\n"
                "  panic --yes           ⚠ Removes EVERY hook on this machine, for the session.\n"
                "                        Prefer the narrow switch: `lend off`, `blend off`,\n"
                "                        `panel off`, `gift off`.\n"
                "  detach --yes          Restores hooks and stops diagnostics; DLL stays mapped until game exit.\n"
                "★ 2026-08-11: the function keys are gone and every action lives here instead. Only\n"
                "  Ctrl+F4 (the rematch experiment) is still a key. Across 72 logs the keys fired\n"
                "  twice, one of which was F1 pressed by accident.\n");
        return;
    }

    // ⚠⚠ DO NOT call verifyAllSignatures() here — this asked for it once, on 2026-08-07, and got
    //    an answer that means the OPPOSITE of what it reads like.
    //
    //    That check compares every hook site against its ORIGINAL bytes, which is only meaningful
    //    BEFORE the hooks are installed. Afterwards each site correctly begins `FF` — the first
    //    byte of our own register-free `FF 25` absolute jump — so it reported
    //    `mismatched=9 ... want 48 got FF` on a perfectly healthy process. Nine MISMATCH lines
    //    look like a broken or stale build and actually mean all nine hooks are live.
    //
    //    ⇒ Report what a caller is really asking: which detours are ACTIVE. The Detour objects are
    //      exposed in tw3k.h for exactly this.
    if (argIs(cmd, 0, "status")) {
        struct HookRow { const char* name; const Detour* d; };
        const HookRow hooks[] = {                 // a plain array, not a braced list: a ranged-for
            { "MP session",   &g_mpDetour },      // over an initializer_list needs a header this
            { "lobby guard",  &g_lobbyDetour },   // TU does not include.
            { "panel probe",  &g_panelDetour },
            { "hud ctor",     &g_hudCtorDetour },
            { "join",         &g_joinDetour },
            { "panel reset",  &g_resetDetour },
            { "seat order",   &g_seatDetour },
            { "share",        &g_shareDetour },
            { "slot-changed", &g_slotChDetour },
            { "lobby tick",   &g_tickDetour },
            { "B4 save-lobby", &g_saveLobbyDetour },
            { "B9 tree walk", &g_treeWalkDetour },
        };
        const int total = (int)(sizeof(hooks) / sizeof(hooks[0]));
        int live = 0;
        for (int i = 0; i < total; ++i) if (hooks[i].d->active) ++live;
        appendf(reply, replySz, "MODE=DEBUG hooks live: %d of %d\n", live, total);
        for (int i = 0; i < total; ++i)
            appendf(reply, replySz, "  %-14s %s\n", hooks[i].name, hooks[i].d->active ? "active" : "-");
        appendf(reply, replySz, "localPlayerId=%d   (-1 = no session yet, which is normal at the menu)\n",
                localPlayerId());
        appendf(reply, replySz, "B4 save-lobby: %s\n", saveLobbyFixArmed() ? "ARMED" : "off");
        appendf(reply, replySz, "B4 panel display: %s\n", extraPanelRepairArmed() ? "ARMED" : "off/signature-refused");
        appendf(reply, replySz, "B4 leave-cache: %s\n", leaveCacheFixArmed() ? "ARMED" : "off/signature-refused");
        reportSaveLobbyWatch(reply, replySz);
        // The two byte patches are not hooks and would otherwise be invisible here, which is exactly
        // how a machine ends up in an unexplained state that nobody can see from the outside.
        appendf(reply, replySz, "turn blend: gate=%s refresh=%s   rematch unlock: %s\n",
                g_turnBlendOn ? "ARMED" : "off", g_refreshBlendOn ? "ARMED" : "off",
                g_rematchUnlocked ? "ARMED" : "off");
        // #48: this changes battle behaviour on EVERY spectator, was on by default with no way back,
        // and could not be seen from outside the process at all — which is how a machine ends up in
        // a state nobody can account for.
        appendf(reply, replySz, "auto-lending spectator: %s\n",
                g_autoLendingSpectator ? "ARMED (a spectator can receive lent units)"
                                       : "off (stock spectator HUD on the next battle)");
        // ★ #13/B10 — no switch by design, so this line is the only way to see it from outside the
        // process, and it is the one patch whose absence makes this client disagree with its peers.
        appendf(reply, replySz, "#13 bystander side gate: %s\n",
                bystanderSideGateArmed()
                    ? "ARMED (spectators get the human participant's army)"
                    : "⚠⚠ NOT APPLIED — this client puts spectators on the ATTACKER's side above "
                      "two humans, and disagrees with any client that did apply it");
        // ★ #75 — ships DISARMED, so "off" is the expected reading and is not a warning. What
        // matters is that every machine reads the SAME thing: it changes what a frame does.
        appendf(reply, replySz, "#75 upd guard: %s\n",
                updateNullGuardArmed()
                    ? "ARMED (a registry miss is skipped, not a CTD) — ⚠ every machine or none"
                    : "off (default) — a registry miss in FUN_142509A40 crashes, as vanilla");
        return;
    }
    // ★★ `updguard` — #75. DISARMED at attach, deliberately: #75's running order is "dump first,
    // patch second", and a guard that is already swallowing the miss removes the crash that makes
    // that battle findable. Arm it once the FULL dump exists, or to survive an evening.
    // ⚠⚠ EVERY MACHINE OR NONE — it changes what a frame computes, not just what it logs.
    if (argIs(cmd, 0, "updguard")) {
        char w[32];
        argAt(cmd, 1, w, sizeof(w));
        if (!w[0]) {
            reportUpdateNullGuard();
            appendf(reply, replySz, "#75 upd guard: %s — see the log.\n",
                    updateNullGuardArmed() ? "ARMED" : "DISARMED");
            return;
        }
        const bool on = (strcmp(w, "on") == 0), off = (strcmp(w, "off") == 0);
        if (!on && !off) { appendf(reply, replySz, "usage: updguard [on|off]\n"); return; }
        const char* why = nullptr;
        if (!setUpdateNullGuard(on, &why)) {
            appendf(reply, replySz, "REFUSED: %s.\n", why ? why : "preconditions not met");
            return;
        }
        appendf(reply, replySz, "#75 upd guard: %s.%s\n", on ? "ARMED" : "DISARMED",
                on ? " ⚠⚠ arm it on EVERY machine or none." : "");
        return;
    }
    if (argIs(cmd, 0, "capture"))  { dumpEverything();                 return; }
    if (argIs(cmd, 0, "savelobby")) {
        if (argIs(cmd, 1, "panels") || argIs(cmd, 1, "close") || argIs(cmd, 1, "watch")) {
            char setting[16];
            argAt(cmd, 2, setting, sizeof(setting));
            if (setting[0] && strcmp(setting, "on") != 0 && strcmp(setting, "off") != 0) {
                appendf(reply, replySz, "usage: savelobby panels|close|watch [on|off]\n"); return;
            }
            if (argIs(cmd, 1, "watch")) {
                if (setting[0] && !setSaveLobbyWatch(strcmp(setting, "on") == 0))
                    appendf(reply, replySz, "REFUSED: writer-watch setup/cleanup failed or debugger present; see log\n");
                reportSaveLobbyWatch(reply, replySz);
            } else if (argIs(cmd, 1, "close")) {
                if (setting[0]) setLeaveCacheFix(strcmp(setting, "on") == 0);
                appendf(reply, replySz, "B4 leave-cache: %s\n", leaveCacheFixArmed() ? "ARMED" : "off/signature-refused");
            } else {
                if (setting[0]) setExtraPanelRepair(strcmp(setting, "on") == 0);
                appendf(reply, replySz, "B4 panel display: %s (extra seats only; existing panels retained)\n",
                    extraPanelRepairArmed() ? "ARMED" : "off/signature-refused");
            }
            return;
        }
        char state[16];
        argAt(cmd, 1, state, sizeof(state));
        if (state[0]) {
            if (strcmp(state, "on") != 0 && strcmp(state, "off") != 0) {
                appendf(reply, replySz, "usage: savelobby [on|off]\n");
                return;
            }
            if (strcmp(state, "on") == 0 && !g_saveLobbyDetour.active) {
                appendf(reply, replySz, "REFUSED: B4 save-lobby hook absent; restart with matching 1.7.2 signatures\n");
                return;
            }
            setSaveLobbyFix(strcmp(state, "on") == 0);
        }
        appendf(reply, replySz, "B4 save-lobby: hook=%s fix=%s; existing records retained\n",
                g_saveLobbyDetour.active ? "active" : "absent", saveLobbyFixArmed() ? "ARMED" : "off");
        return;
    }
    if (argIs(cmd, 0, "session"))  { dumpMpSession();                  return; }
    if (argIs(cmd, 0, "lobby"))    { dumpLobbyState();                 return; }
    if (argIs(cmd, 0, "commands")) { reportCommandTraffic();           return; }
    if (argIs(cmd, 0, "read"))     { doRead(cmd, reply, replySz);      return; }

    if (argIs(cmd, 0, "turn")) {
        dumpTurnState();
        dumpEndTurnNotifications();
        appendf(reply, replySz, "movementsInFlight=%d autosavePending=%d\n",
                movementsInFlight(), autosavePending());
        return;
    }

    if (argIs(cmd, 0, "faction")) {
        if (argIs(cmd, 1, "set")) {
            char key[64] = { 0 };
            argAt(cmd, 2, key, sizeof(key));
            appendf(reply, replySz, queueFactionByKey(key) ? "faction set: queued %s\n"
                                                           : "faction set: %s is not in the faction list\n", key);
            return;
        }
        queueFactionRequest(argIs(cmd, 1, "next"));
        return;
    }

    // ★★ `g_giftTargetIdx` IS A SLOT INDEX. Proven 2026-08-11 from the engine, not inferred:
    // `FUN_140471B30` bounds-checks the field against `+0x1360` (the slot ENTRY COUNT), scales it by
    // `0xF8` (the slot STRIDE) and indexes the slot array with it to read `+0xF4` flags and the
    // `+0x18`/`+0x14` unit list. Nothing in that path touches a player id.
    //
    // ✗ This command used to store whatever number was typed, RAW, while its own help called it a
    // "player id". On contiguous seating slot == id and the two coincide, which is why it never bit
    // — and non-contiguous seating is exactly when a human reaches for the manual override, so it
    // would have bitten at the worst moment. The gift PANEL already converts (`coopGiftToPlayer`
    // stores `slotForPlayer(playerId)`); this did not, so the two disagreed.
    //
    // ⇒ Take a PLAYER id, convert like the panel does, and REFUSE rather than guess when the seat
    // cannot be resolved — an unresolvable target written raw is how a unit reaches a stranger or an
    // empty slot, and the sender validates nothing about it.
    if (argIs(cmd, 0, "gift")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));
        if (!v[0]) { appendf(reply, replySz, "ERR gift needs a player id, or `off`\n"); return; }
        if (strcmp(v, "off") == 0) {
            g_giftTargetIdx = -1;
            appendf(reply, replySz, "gift target now off (vanilla)\n");
            return;
        }
        const int wantPlayer = (int)atol(v);
        const int slot       = slotForPlayerPublic(wantPlayer);
        if (slot < 0) {
            appendf(reply, replySz,
                    "REFUSED: player %d has no seat in the +0x137C table, so there is no slot to\n"
                    "aim at. The command field is a SLOT index, and writing an unresolved number\n"
                    "raw is how a unit reaches the wrong person or an empty slot — the sender\n"
                    "validates nothing about the target. `session` prints the player->slot table.\n",
                    wantPlayer);
            return;
        }
        g_giftTargetIdx = slot;
        appendf(reply, replySz, "gift target now player %d = slot %d%s\n", wantPlayer, slot,
                (slot == wantPlayer) ? "  (they coincide here — seating is contiguous)"
                                     : "  ★ THEY DIFFER — this is the case that used to send it wrong");
        return;
    }

    // ★ GIFT PANEL MODE — F6's action, and the last capability that had no pipe route (2026-08-11).
    //
    // ⚠ Do not read this as "the escape hatch nobody needed". Across 72 logs F6 was pressed ZERO
    // times, which says it has not been reached for lately, NOT that it never mattered — the panel
    // failing to appear is exactly the situation in which nobody is taking notes. It exists so the
    // proven `gift <id>` + gift-icon path can be restored without a rebuild.
    //
    // No arg reports, which is deliberate: every state-changing command here answers "what is it
    // now?" so a run sheet can assert instead of a person remembering.
    // ⚠ `panel probe` / `panel reset` appear in `status`, but they are DISPLAY LABELS for two
    // detours, not commands — so there is nothing here to collide with. Checked rather than assumed;
    // a guard against them would have implied commands that do not exist.
    if (argIs(cmd, 0, "panel")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));
        if (!v[0]) {
            appendf(reply, replySz, "gift panel mode: %s\n", g_giftPanelMode ? "ON" : "off");
            return;
        }
        const bool on = (strcmp(v, "on") == 0), off = (strcmp(v, "off") == 0);
        if (!on && !off) {
            appendf(reply, replySz, "ERR panel takes `on`, `off`, or nothing.\n");
            return;
        }
        g_giftPanelMode   = on;
        g_giftPanelWanted = false;      // never leave a stale "wanted" behind a mode change
        logf("GIFT PANEL MODE: %s by command.%s", on ? "ON" : "off",
             on ? "\n    The gift icon no longer gifts. It TOGGLES GiftPanelOpen and leaves the"
                  " selection alone, so the pack's panel can ask who. Needs"
                  " tw3k_coop.pack; without it the gift icon does nothing."
                : "\n    ★ ESCAPE HATCH: the gift icon gifts directly again, using `gift <id>`'s"
                  " target if one is armed. Use this if the panel never appears.");
        appendf(reply, replySz, "gift panel mode now %s\n", on ? "ON" : "off");
        return;
    }

    // ★ B9 (#12) — the widget-tree-walk guard, and its off switch.
    //
    // ON by default because it is a crash fix, not an experiment, and because the crash it prevents
    // lands while the FOURTH player is being seated — nobody is at a keyboard to arm anything then.
    // The switch exists anyway: #48 spent a session unable to answer "is this ours?" because
    // `g_autoLendingSpectator` was true with no way back, and that is not a mistake worth repeating.
    //
    // ⚠ `treewalk off` restores the VANILLA crash. That is the point of it — it is the A/B — but say
    // so, because "off" reads like "safer" and here it is the opposite.
    if (argIs(cmd, 0, "treewalk")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));
        if (!v[0]) {
            appendf(reply, replySz, "B9 tree-walk guard: %s   hook=%s   refusals=%ld\n",
                    g_treeWalkGuard ? "ON" : "off",
                    g_treeWalkDetour.active ? "active" : "NOT INSTALLED",
                    treeWalkRefusals());
            return;
        }
        const bool on = (strcmp(v, "on") == 0), off = (strcmp(v, "off") == 0);
        if (!on && !off) { appendf(reply, replySz, "ERR treewalk takes `on`, `off`, or nothing.\n"); return; }
        g_treeWalkGuard = on;
        logf(">>> B9 TREE-WALK GUARD: %s by command.%s", on ? "ON" : "off",
             on ? "  A corrupt widget child list makes the lookup fail instead of faulting."
                : "\n    ⚠ VANILLA BEHAVIOUR RESTORED — a corrupt child list will now CRASH this"
                  " client, which is B9. This is the A/B, not a safety setting.");
        appendf(reply, replySz, "tree-walk guard now %s\n", on ? "ON" : "off");
        return;
    }

    // ★★ PANIC and DETACH — F1 and F12's actions (2026-08-11).
    //
    // Both require `--yes` for the reason F1 was retired in the first place: F1 was pressed by
    // accident once in 72 logs and never on purpose, and its only symptom is grey buttons and a
    // player who thinks the mod is broken. A command you have to mean is the whole point of moving
    // it here — so DO NOT "helpfully" drop the confirmation later.
    //
    // Neither acts directly. They set a flag the probe loop drains, on the thread that used to run
    // these off a keypress; `main.cpp`'s drain site says why.
    if (argIs(cmd, 0, "panic")) {
        if (!argIs(cmd, 1, "--yes")) {
            appendf(reply, replySz,
                    "REFUSED: `panic` removes EVERY hook on this machine for the rest of the\n"
                    "session. Nothing re-installs them — the client keeps playing, without the mod,\n"
                    "and the only visible symptom is grey buttons.\n"
                    "⇒ For one feature, prefer the narrow switch: `lend off`, `blend off`,\n"
                    "  `panel off`, `gift off`. Say `panic --yes` when you mean all of it.\n");
            return;
        }
        stopUiControl();
        InterlockedExchange(&g_panicRequest, 1);
        appendf(reply, replySz, "panic queued — all hooks removed within ~100 ms.\n");
        return;
    }

    if (argIs(cmd, 0, "detach")) {
        if (!argIs(cmd, 1, "--yes")) {
            appendf(reply, replySz, "REFUSED: say `detach --yes` to restore hooks and stop the control server.\n"
                    "The DLL stays mapped until game exit. Restart to load another build.\n"
                    "Use `panic --yes` to disarm while keeping diagnostics available.\n");
            return;
        }
        uintptr_t root = 0;
        const bool inCampaign = readAt(g_base + RVA_CAMPAIGN_ROOT, root) && root > 0x10000;
        if (inCampaign && !argIs(cmd, 2, "--force")) {
            appendf(reply, replySz, "REFUSED: a campaign is loaded; detach is MAIN MENU ONLY.\n"
                    "Use `panic --yes` for diagnostics, or `detach --yes --force` to stop them too.\n"
                    "Both retain the DLL until game exit.\n");
            return;
        }
        InterlockedExchange(&g_detachRequest, 1);
        appendf(reply, replySz, "detach queued: restoring hooks and stopping workers; check the log for failures.\n"
                "DLL stays mapped. Restart the game before loading another build.\n");
        return;
    }

    // ★ #48 — the off switch that did not exist, and whose absence made the bug untestable.
    //
    // `g_autoLendingSpectator` has been true-with-no-way-back since the lending round trip was
    // proven, and battle.cpp says the cost out loud: "an un-armed client is no longer byte-for-byte
    // vanilla in a battle, which the project's own safety rule used to guarantee." On 2026-08-09 a
    // reinforcement battle put the spectators on the enemy side and the obvious A/B — run it once
    // with the latch off — could not be run at all, short of F1 wiping every hook on that machine.
    //
    // ⚠ It takes effect on the NEXT battle load, because the byte is cleared while the HUD is being
    // constructed. Flipping it mid-battle changes nothing, and saying so here is cheaper than
    // someone concluding the command does not work.
    if (argIs(cmd, 0, "lend")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));
        if (!v[0]) {
            appendf(reply, replySz,
                    "auto-lending spectator: %s (takes effect on the NEXT battle)\n",
                    g_autoLendingSpectator ? "ON" : "OFF");
            return;
        }
        const bool on  = (strcmp(v, "on")  == 0);
        const bool off = (strcmp(v, "off") == 0);
        if (!on && !off) { appendf(reply, replySz, "ERR lend takes `on`, `off`, or nothing.\n"); return; }

        // ✂ `lend guard` is GONE (2026-08-18). Its predicate compared `flags & 3` between session
        // slots, which is the lobby team at LOBBY time only — by battle time those bits are
        // rewritten and it was really separating participant from spectator, which is true in both
        // halves of #13. The real fix is `sidegate.cpp`, armed at attach, and it needs no switch.
        g_autoLendingSpectator = on;
        logf(">>> AUTO LENDING SPECTATOR: %s by command. %s",
             on ? "ARMED" : "DISARMED",
             on ? "A spectator will be built able to receive and command lent units."
                : "This client will build a STOCK spectator HUD on the next battle — the vanilla "
                  "control for #48. It cannot receive lent units while this is off.");
        appendf(reply, replySz,
                "auto-lending spectator now %s — takes effect on the NEXT battle load\n",
                on ? "ON" : "OFF");
        return;
    }

    // ★ #47/#50 — pings and battle field-lines, which are one subsystem (telestration). With no
    // argument this prints the participant map; `tel fix` inserts the entries the constructor never
    // asked for. Like `lend`, the set is decided once at battle construction, so arming mid-battle
    // does nothing until the next one.
    if (argIs(cmd, 0, "tel")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));
        if (!v[0] || strcmp(v, "status") == 0) {
            reportTelestration();
            appendf(reply, replySz, "telestration fix: %s — the map is in the log.\n",
                    telestrationFixArmed() ? "ARMED" : "disarmed");
            return;
        }
        if (strcmp(v, "fix") == 0) {
            char w[32];
            argAt(cmd, 2, w, sizeof(w));
            const bool on = (strcmp(w, "on") == 0), off = (strcmp(w, "off") == 0);
            if (!on && !off) { appendf(reply, replySz, "usage: tel fix on --yes | off\n"); return; }
            if (on && !argIs(cmd, 3, "--yes")) {
                // ⚠ The honest warning is about the heap, not about desync: this manager is a UI
                // object fed by chat_*_msg, so a client that runs it alone diverges from nobody.
                appendf(reply, replySz,
                        "REFUSED. `tel fix on --yes`. It ALLOCATES and writes to the telestration "
                        "manager's map at battle construction, using the constructor's own calls. It "
                        "is NOT replicated state, so unlike hfcount it cannot desync — but run it on "
                        "every machine anyway: a client without it still cannot see the strokes of a "
                        "player it has no entry for.\n");
                return;
            }
            const char* why = nullptr;
            if (!setTelestrationFix(on, &why)) {
                appendf(reply, replySz, "REFUSED: %s.\n", why ? why : "preconditions not met");
                return;
            }
            appendf(reply, replySz, "telestration fix: %s — takes effect on the NEXT battle.\n",
                    on ? "ARMED" : "disarmed");
            return;
        }
        appendf(reply, replySz, "usage: tel [status] | tel fix on --yes|off\n");
        return;
    }

    if (argIs(cmd, 0, "dilemma")) { reportPendingDilemma(); return; }

    // ★ #13 / B10 — read-only, and it is the one command worth running WHILE THE PRE-BATTLE PROMPT
    // IS UP, because that is the only moment the list is populated. The role hook prints it too, so
    // a reproduction is captured without anybody being at a keyboard.
    if (argIs(cmd, 0, "pending")) { dumpPendingBattleChoice(); return; }

    // ★ B1 — the event feed. Read-only; the on/off touches only OUR logging, never the game.
    //
    // ⚠ Worth knowing before reading its output: the feed's resting state and its stuck state are
    // the same three bytes, so `feed` deliberately reports COUNTS OF TRANSITIONS and not a state.
    // Two sessions were spent concluding things from a state sample that could not carry the
    // information — 2026-08-09 from three dumps, 2026-08-10 from a live read.
    if (argIs(cmd, 0, "feed")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));
        // Both halves of the same question, and they are only readable together: the ticker says
        // whether the event was FIRED, the listener says whether anything ANSWERED it. Reporting one
        // without the other is how "the event fires" was mistaken for "a box appears".
        if (!v[0]) {
            reportFeedTicker(); reportFeedGate(); reportNextAutoOpen(); reportFactionInList();
            reportFeedIconGuard();
            return;
        }

        // ★★ `feed icon` — #69. ARMED AT ATTACH; this is the revert, and the way to A/B the crash
        // against stock behaviour. Same species as `treewalk off`: "off" restores a vanilla crash,
        // it is not the safer setting.
        if (strcmp(v, "icon") == 0) {
            char w[32];
            argAt(cmd, 2, w, sizeof(w));
            if (!w[0]) {
                reportFeedIconGuard();
                appendf(reply, replySz, "feed icon guard: %s — see the log.\n",
                        feedIconGuardArmed() ? "ARMED" : "DISARMED");
                return;
            }
            const bool ion = (strcmp(w, "on") == 0), ioff = (strcmp(w, "off") == 0);
            if (!ion && !ioff) { appendf(reply, replySz, "usage: feed icon [on|off]\n"); return; }
            const char* why = nullptr;
            if (!setFeedIconGuard(ion, &why)) {
                appendf(reply, replySz, "REFUSED: %s.\n", why ? why : "preconditions not met");
                return;
            }
            appendf(reply, replySz, "feed icon guard: %s.%s\n", ion ? "ARMED" : "DISARMED",
                    ion ? "" : " ⚠ this client now has the vanilla crash back.");
            return;
        }

        // ★★★ `feed dirty` — ⚠ WRITES. B1's candidate fix: set the context's own stale flag and let
        // the engine recompute the list itself at its next query. No `--yes` gate, deliberately: it
        // writes ONE byte that the engine sets and clears constantly by itself, it cannot desync
        // (the feed is presentation, not model state), and the worst case is a recompute that finds
        // nothing. That is a different risk class from `answer`/`unblock`, which change a decision.
        if (strcmp(v, "dirty") == 0) {
            char why[320] = { 0 };
            const bool ok = markFeedDirty(why, sizeof(why));
            appendf(reply, replySz, "%s%s\n", ok ? "" : "ERR ", why);
            if (ok)
                appendf(reply, replySz,
                        "watch the log for `EVENT FEED DIRTY: 1 -> 0` then an `EVENT FEED LIST` "
                        "change. ⚠ flag clears but count does NOT move = the source is stale too, "
                        "and the target moves upstream of this context.\n");
            return;
        }

        // ★★★ `feed refresh` — ⚠ CALLS THE ENGINE, and it is the only command here that does.
        // `dirty` alone was measured insufficient on a live B1: the flag stayed set for 90 s because
        // the refresh only runs inside a getter and a stuck client never calls one. This pulls it.
        if (strcmp(v, "refresh") == 0) {
            char why[320] = { 0 };
            const bool ok = requestFeedRefresh(why, sizeof(why));
            appendf(reply, replySz, "%s%s\n", ok ? "" : "ERR ", why);
            if (ok)
                appendf(reply, replySz,
                        "⚠ run `feed dirty` FIRST or the helper short-circuits and tests nothing.\n");
            return;
        }

        // ★★★ `feed rebuild` — ⚠ CALLS THE ENGINE. LEVEL 1, upstream of `dirty`/`refresh`: rebuilds
        // the SOURCE itself from the campaign model for the local faction. Run this FIRST — forcing
        // level 2 from an empty source proves nothing, which is what happened on 2026-08-13.
        if (strcmp(v, "rebuild") == 0) {
            char why[320] = { 0 };
            const bool ok = requestSourceRebuild(why, sizeof(why));
            appendf(reply, replySz, "%s%s\n", ok ? "" : "ERR ", why);
            return;
        }

        const bool on = (strcmp(v, "on") == 0), off = (strcmp(v, "off") == 0);
        if (!on && !off) {
            appendf(reply, replySz,
                    "ERR feed takes `on`, `off`, `dirty`, `refresh`, `rebuild`, `icon`, or nothing.\n");
            return;
        }
        setFeedLogging(on);
        appendf(reply, replySz, "event feed per-transition logging now %s (counts unaffected)\n",
                on ? "ON" : "OFF");
        return;
    }

    // ★★★ TURN BLENDING (#44) — the management layer during another player's turn.
    //
    // ⚠ The rule here is neither `unblock`'s nor `answer`'s, and it is worth stating because this is
    // now the third writer with a different one:
    //
    //   unblock  DISCARDS a decision           -> EVERY MACHINE OR NONE
    //   answer   MAKES a decision              -> the OWNING machine only
    //   blend    changes what THIS CLIENT'S UI -> PER MACHINE, and mixing is legitimate
    //            will let its player do
    //
    // It needs no `--yes`. Nothing is decided, nothing is destroyed and nothing is sent: it flips a
    // local predicate that governs whether this client's own buttons are usable. Every action it
    // opens still travels as an ordinary `CCQ_` command that every peer executes in queue order, so
    // an armed client and an un-armed one do not disagree about the campaign — they disagree only
    // about what their own player may click. That is also what makes it a clean A/B: arm one machine
    // and leave the other vanilla.
    // #63. ⚠⚠ EVERY MACHINE OR NONE — it writes replicated model state, and one client holding a
    // different count is a desync by construction. It raises the count to a RULE computed from the
    // replicated array, so all clients reach the same answer; in a 2-player game that rule yields
    // the count it already has and nothing is written at all.
    if (argIs(cmd, 0, "hfcount")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));

        // ★ #68's repair, armed SEPARATELY. Fusing it to the count hold made a crash unattributable
        // — with one lever there is no A/B, and "probably ours" is not a measurement.
        if (strcmp(v, "repair") == 0) {
            char w[32];
            argAt(cmd, 2, w, sizeof(w));
            const bool on = (strcmp(w, "on") == 0), off = (strcmp(w, "off") == 0);
            if (!on && !off) {
                appendf(reply, replySz, "usage: hfcount repair on --yes | off\n");
                return;
            }
            if (on && !argIs(cmd, 3, "--yes")) {
                appendf(reply, replySz,
                        "REFUSED. `hfcount repair on --yes`, and run it on EVERY machine or none: it "
                        "writes replicated model state and may reallocate the registry through the "
                        "engine's allocator.\n");
                return;
            }
            const char* why = nullptr;
            if (!setHumanFactionRepair(on, &why)) {
                appendf(reply, replySz, "REFUSED: %s\n", why ? why : "preconditions not met");
                return;
            }
            appendf(reply, replySz, "registry repair: %s\n", on ? "ARMED" : "disarmed");
            return;
        }

        // ★ The reader-side alternative to holding the count (2026-08-15). One displacement byte,
        // no model write at all — so unlike the hold it cannot desync, does not need re-arming
        // after a load, and does not die with the campaign. It ships disarmed because it reads the
        // registry's uninitialised tail and because it does NOT fix storage: see eventcursor.cpp.
        if (strcmp(v, "bound") == 0) {
            char w[32];
            argAt(cmd, 2, w, sizeof(w));
            const bool on = (strcmp(w, "on") == 0), off = (strcmp(w, "off") == 0);
            if (!on && !off) {
                appendf(reply, replySz, "usage: hfcount bound on --yes | off\n");
                return;
            }
            if (on && !argIs(cmd, 3, "--yes")) {
                appendf(reply, replySz,
                        "REFUSED. `hfcount bound on --yes`. It writes NO model state, so it cannot "
                        "desync — but it patches a code byte and widens what the event feed will "
                        "look at, so say so deliberately.\n");
                return;
            }
            const char* why = nullptr;
            if (!setHumanFactionSearchBound(on, &why)) {
                appendf(reply, replySz, "REFUSED: %s.\n", why ? why : "preconditions not met");
                return;
            }
            appendf(reply, replySz, "search bound: %s%s\n", on ? "ARMED — the registry search now "
                    "runs to cap, not count" : "disarmed",
                    on ? ". No model state was written.\n" : ".\n");
            return;
        }

        if (!v[0] || strcmp(v, "status") == 0) {
            reportHumanFactionCountHold();
            appendf(reply, replySz, "count hold: %s | campaign tick: %s | search bound: %s — see the log.\n",
                    humanFactionCountHoldArmed() ? "ARMED" : "disarmed",
                    campaignTickHookActive() ? "LIVE" : "MISSING",
                    humanFactionSearchBoundArmed() ? "ARMED" : "disarmed");
            return;
        }
        if (strcmp(v, "off") == 0) {
            setHumanFactionCountHold(false, nullptr);
            reportHumanFactionCountHold();
            appendf(reply, replySz, "count hold: DISARMED. Present count left in place; the engine rebuilds it on load.\n");
            return;
        }
        if (strcmp(v, "on") == 0) {
            if (!argIs(cmd, 2, "--yes")) {
                appendf(reply, replySz,
                        "REFUSED. `hfcount on --yes`, and run it on EVERY machine or none: this "
                        "writes replicated model state and one client with a different count "
                        "desyncs by construction.\n");
                return;
            }
            const char* why = nullptr;
            if (!setHumanFactionCountHold(true, &why)) {
                appendf(reply, replySz, "REFUSED: %s.\n", why ? why : "preconditions not met");
                return;
            }
            reportHumanFactionCountHold();
            appendf(reply, replySz,
                    "count hold: ARMED. It raises the count only to the number of registered human "
                    "factions, never beyond, and never lowers it. Below 3 humans it is a no-op.\n");
            return;
        }
        appendf(reply, replySz,
                "usage: hfcount [status|on --yes|off] | hfcount bound on --yes|off | "
                "hfcount repair on --yes|off\n");
        return;
    }

    if (argIs(cmd, 0, "blend")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));
        if (!v[0]) { reportTurnBlend(); return; }

        // ★ PER-SITE. `blend sites` lists the 22 call sites and what each one currently points at;
        // `blend site <n> on|off` moves exactly one. This exists because the first attempt to put the
        // diplomacy button back excluded the site that carried the `diplomacy_panel` string, shipped,
        // and changed nothing — the button is bound to `IsPlayersTurn`, one of the other twenty. A
        // build and a live session to learn that; five flips of this to learn the next one.
        if (strcmp(v, "sites") == 0) {
            reportGateSites();
            appendf(reply, replySz, "%zu call sites listed in the log — `blend site <n> on|off`.\n",
                    gateSiteCount());
            return;
        }
        if (strcmp(v, "site") == 0) {
            char nArg[32], stateArg[32];
            argAt(cmd, 2, nArg, sizeof(nArg));
            argAt(cmd, 3, stateArg, sizeof(stateArg));
            const int n = atoi(nArg);
            const bool relax = (strcmp(stateArg, "on") == 0);
            if (!nArg[0] || n <= 0 || (!relax && strcmp(stateArg, "off") != 0)) {
                appendf(reply, replySz,
                        "ERR blend site <n> on|off   (1..%zu; `blend sites` lists them). `on` = that "
                        "caller is relaxed, `off` = it asks the real turn question.\n",
                        gateSiteCount());
                return;
            }
            char why[256] = { 0 };
            const bool ok = setGateSite((size_t)n, relax, why, sizeof(why));
            appendf(reply, replySz, "%s%s\n", ok ? "" : "ERR ", why);
            return;
        }

        // Two halves, separately armable so the A/B survives: `gate` is what lets a player ACT out of
        // turn, `refresh` is what lets their panels SHOW it without being closed and reopened. Bare
        // `on`/`off` moves both, because the gate without the refresh is what produced the
        // "i assign a general and nothing happens until i reopen the panel" report.
        char which[32];
        strcpy_s(which, sizeof(which), v);
        bool wantGate = true, wantRefresh = true;
        if (strcmp(which, "gate") == 0 || strcmp(which, "refresh") == 0) {
            wantGate    = (strcmp(which, "gate")    == 0);
            wantRefresh = (strcmp(which, "refresh") == 0);
            argAt(cmd, 2, v, sizeof(v));
            if (!v[0]) { appendf(reply, replySz, "ERR blend %s takes `on` or `off`.\n", which); return; }
        }

        const bool on  = (strcmp(v, "on")  == 0);
        const bool off = (strcmp(v, "off") == 0);
        if (!on && !off) {
            appendf(reply, replySz,
                    "ERR blend takes `on`, `off`, `gate on|off`, `refresh on|off`, or nothing at all "
                    "for a report.\n");
            return;
        }

        // An explicit `off` must STAY off — otherwise the auto-arm loop undoes it two seconds later,
        // which would make the A/B impossible and look like the patch re-applying itself.
        if (off) cancelTurnBlendAutoArm();

        bool didSomething = false;
        if (wantGate && on != g_turnBlendOn && setTurnBlend(on)) {
            didSomething = true;
            logf(">>> TURN BLEND: %s. FUN_142F6BFA0 now answers \"%s\".", on ? "ARMED" : "DISARMED",
                 on ? "is the current faction HUMAN" : "is the current faction MINE");
            if (on)
                logf("    Buildings, character skill points and army stance should be usable during "
                     "another player's turn. The AI sweep is unaffected. Mark with `commands`, "
                     "click, then `commands` again — CCQ_REGION_BUILDING_CONSTRUCT in the delta is "
                     "the proof it reached the queue rather than merely looking clickable.");
        }
        if (wantRefresh && on != g_refreshBlendOn && setRefreshBlend(on)) {
            didSomething = true;
            if (on)
                logf("    UI refresh events now fire out of turn — assignments, court posts, CEOs, "
                     "retinue and resource displays should update in place instead of needing the "
                     "panel closed and reopened. Diplomacy is deliberately NOT included.");
        }
        if (!didSomething)
            appendf(reply, replySz, "blend already %s — nothing written.\n", on ? "ARMED" : "off");
        return;
    }

    // ★★★ ANSWER a stuck dilemma (#43) — the surgical counterpart to `unblock` below.
    //
    // The distinction matters enough to be enforced in the refusal text, because the two commands
    // have OPPOSITE rules and confusing them is how a session gets desynced:
    //
    //   unblock  DISCARDS the decision  -> EVERY MACHINE OR NONE
    //   answer   MAKES the decision     -> ONE MACHINE, the owning one
    //
    // The asymmetry is not a convention, it is what the wire format dictates: the choice command
    // carries only an option index, so each receiver finds its own guard and pops it. Broadcasting
    // it would just mean the non-owners refuse (they fail the faction test) and, once the first has
    // landed, the record is answered so the rest refuse again. Safe, but pointless.
    //
    // `--yes` is required for the same reason `unblock` requires it: this makes a real, irreversible
    // in-game decision, and a mistyped option number cannot be taken back.
    if (argIs(cmd, 0, "answer")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));
        if (!v[0]) {
            appendf(reply, replySz,
                    "ERR answer needs an option number, 1-based, e.g. `answer 2 --yes`.\n"
                    "    Run `dilemma` first - it prints how many options there are and which\n"
                    "    machine owns the decision.\n");
            return;
        }
        if (!argIs(cmd, 2, "--yes")) {
            appendf(reply, replySz,
                    "REFUSED: `answer` MAKES a decision in the campaign and it cannot be undone.\n"
                    "Run `dilemma` to see the options and the owner, then re-issue as\n"
                    "`answer %s --yes`.\n"
                    "⚠ Send it to the OWNING machine only. This is the opposite of `unblock`:\n"
                    "  one submission resolves the dilemma on every client by itself.\n", v);
            return;
        }
        answerPendingDilemma(atoi(v));
        return;
    }

    // ★★ The one command that writes, and the one this channel most improves.
    //
    // F7 discards a pending decision, and the rule is EVERY MACHINE OR NONE: one machine dropping a
    // decision the others still hold makes their campaign models disagree, which is a desync by
    // construction. Three people pressing three keyboards cannot do that reliably; three pipe
    // writes issued from one script land within milliseconds of each other.
    //
    // It still refuses without --yes, because "every machine or none" is only safe when the caller
    // MEANT to fire it, and a typo that reaches three machines at once is worse than one that
    // reaches one.
    if (argIs(cmd, 0, "unblock")) {
        if (!argIs(cmd, 1, "--yes") && !argIs(cmd, 2, "--yes")) {
            appendf(reply, replySz,
                    "REFUSED: `unblock` DISCARDS a pending decision and must be issued to EVERY\n"
                    "machine or none. Check first whether an event box is open on ANY screen - if\n"
                    "one is, somebody is deciding and this would destroy it. Re-issue as\n"
                    "`unblock --yes` when you mean it.\n");
            return;
        }
        clearTurnGateBlockList(argIs(cmd, 1, "--force") || argIs(cmd, 2, "--force"));
        return;
    }

    // ★ `crash` — the recorder's status, answered OUT OF THE RUNNING PROCESS for the same reason
    // `ping` is. "Is the dumper armed" is not a question a file on disk can answer: the game
    // installs its own last-chance filter after ours and we re-take it on a timer, so the only
    // truthful answer comes from the process that would have to write the dump.
    if (argIs(cmd, 0, "crash")) {
        char v[32];
        argAt(cmd, 1, v, sizeof(v));

        if (strcmp(v, "heap") == 0) {
            char w[32];
            argAt(cmd, 2, w, sizeof(w));
            const bool on = (strcmp(w, "on") == 0), off = (strcmp(w, "off") == 0);
            if (!on && !off) { appendf(reply, replySz, "usage: crash heap [on|off]\n"); return; }
            setCrashDumpFullMemory(on);
            appendf(reply, replySz,
                    "crash dump size: %s\n", on
                    ? "FULL MEMORY. ⚠ ~8.6 GB and seconds to write, on a process that is already\n"
                      "dying — worth it when you are hunting a specific object, not otherwise. This\n"
                      "is per-process and dies with the game."
                    : "compact (stacks + indirectly referenced memory). A few MB, sub-second.");
            return;
        }

        // ★★ `crash test` — the only way to prove the recorder INSIDE THE GAME.
        //
        // The desk measurement (§6uuu.43) faulted a test host and got a valid dump, which proves the
        // write and nothing else. What it cannot contain is a GAME installing its own last-chance
        // filter over ours, and that displacement is the whole reason `rearmCrashDumper` exists. So
        // the remaining question — "is the filter still ours by the time it matters" — was answerable
        // only at the moment of a real crash, which is the worst possible moment to be asking: a real
        // CTD arrives unannounced, and if it leaves no dump there is no way to separate "the recorder
        // is broken" from "that death never reached an exception filter at all".
        //
        // ⇒ This makes the fault the CONTROL rather than the experiment. Run it first, and a real
        //   crash that then leaves no dump is telling you something about the CRASH, not about this.
        //
        // ⚠⚠ It kills the game. Single player, one machine, a campaign nobody cares about.
        if (strcmp(v, "test") == 0) {
            char w[32];
            argAt(cmd, 2, w, sizeof(w));

            // `crash test --yes` with no kind is the common case, so --yes is accepted in either
            // position rather than being a syntax puzzle at the moment somebody means it.
            long kind = 0;
            if (!w[0] || strcmp(w, "--yes") == 0 || strcmp(w, "av") == 0) kind = CRASH_TEST_AV;
            else if (strcmp(w, "overflow") == 0)                          kind = CRASH_TEST_OVERFLOW;
            else {
                appendf(reply, replySz, "usage: crash test [av|overflow] --yes\n");
                return;
            }

            if (!argIs(cmd, 2, "--yes") && !argIs(cmd, 3, "--yes")) {
                appendf(reply, replySz,
                        "REFUSED: `crash test` FAULTS ON PURPOSE and the game dies. That is what it is\n"
                        "for — it proves the crash recorder in the running game, which no test host can\n"
                        "do, because only the game installs a competing last-chance filter.\n"
                        "⚠ SINGLE PLAYER, one machine, a campaign you do not mind losing. In a session\n"
                        "  it drops this client, and unsaved progress goes with it.\n"
                        "⇒ `crash test --yes`           access violation — the shape of a real CTD\n"
                        "  `crash test overflow --yes`  stack overflow — the case the dumper thread\n"
                        "                               exists for, so a dump here proves that design\n"
                        "Nothing is written to model state and no byte is patched. Read `crash` first if\n"
                        "you only wanted the status.\n");
                return;
            }

            InterlockedExchange(&g_crashTestRequest, kind);

            // ⚠ The two kinds do NOT produce the same log, and saying they do sent a reader looking
            // for a FAULT block that cannot exist (2026-08-17). On a stack overflow the witness
            // returns without printing — deliberately, because `logf` costs 2.2 KB of a stack that
            // has about one page left, and trying to print there is what produced NO output at all
            // the first time. So the reply describes the path the caller actually asked for.
            appendf(reply, replySz,
                    "queued: %s, on the probe thread, within ~100 ms.\n"
                    "⇒ The game is about to die. Then, beside the log:\n"
                    "    tw3k_crash_<HOST>_<stamp>.dmp   the dump — this is the result\n"
                    "  and IN the log, in this order:\n"
                    "    DELIBERATE FAULT   the recorder's state at the instant of the fault,\n"
                    "                       re-arm count included — read that number\n",
                    kind == CRASH_TEST_OVERFLOW ? "stack overflow" : "access violation (null store)");

            if (kind == CRASH_TEST_OVERFLOW)
                appendf(reply, replySz,
                        "    (NO `FAULT` block — expected. The witness returns without printing on\n"
                        "     this code; on an overflowed stack `logf` is what kills the report.)\n"
                        "    FATAL             written from the DUMPER thread, and it carries the\n"
                        "                      module attribution the witness could not print\n"
                        "    DUMP WRITTEN      path and size\n"
                        "⚠ If the log stops at `Going down now.` with no FATAL, the fatal path still\n"
                        "  costs too much stack — that is the §6uuu.45 failure, not a lost race.\n");
            else
                appendf(reply, replySz,
                        "    FAULT             the witness's line\n"
                        "    FATAL             the last-chance filter, i.e. the filter was still ours\n"
                        "    DUMP WRITTEN      path and size\n"
                        "⚠ No FATAL block means something displaced us and we lost the race — which is\n"
                        "  the finding, and it is what the re-arm exists to prevent.\n");
            return;
        }

        if (v[0] && strcmp(v, "status") != 0) {
            appendf(reply, replySz,
                    "usage: crash [status] | crash heap [on|off] | crash test [av|overflow] --yes\n");
            return;
        }

        appendf(reply, replySz, "---- crash recorder ----\n");
        appendf(reply, replySz,
                "  witness (VEH, first-chance) : installed at attach. Prints FAULT for every\n"
                "                                fatal-CLASS exception, handled or not. Microseconds.\n");
        appendf(reply, replySz, "  dumper  (UEF, last-chance)  : %s\n",
                crashDumperInstalled()
                    ? "INSTALLED - writes a dump only once nothing handled the fault"
                    : "NOT INSTALLED - a crash leaves the FAULT lines and nothing else");
        appendf(reply, replySz, "  faults seen                 : %ld distinct, %ld total\n",
                faultKindsSeen(), faultsSeenTotal());
        if (faultsSeenTotal() > faultKindsSeen() * 4 && faultKindsSeen() > 0)
            appendf(reply, replySz,
                    "                                ^ a large total against few kinds is the game\n"
                    "                                  handling a fault in a loop - normal traffic.\n");

        if (crashDumperInstalled()) {
            appendf(reply, replySz, "  re-arms                     : %ld %s\n", crashDumperRearms(),
                    crashDumperRearms() == 0
                        ? "(nothing has displaced us yet)"
                        : "(something installs its own filter; we re-take it and chain to it)");
            appendf(reply, replySz, "  chained to                  : %s\n",
                    crashDumperChained() ? "yes - the game's own filter still runs after ours"
                                         : "nothing - we are the only filter in this process");
            appendf(reply, replySz, "  dump size mode              : %s\n",
                    crashDumpFullMemory() ? "FULL MEMORY (~8.6 GB) - `crash heap off` to revert"
                                          : "compact (a few MB, sub-second)");
            appendf(reply, replySz, "  dumps written this run      : %ld\n", crashDumpsWritten());
            appendf(reply, replySz, "  last dump                   : %s\n",
                    crashDumperLastDump() ? crashDumperLastDump()
                                          : "(none - this process has not died yet)");
        }
        appendf(reply, replySz,
                "  ⚠ SAFE IN A LOCKSTEP SESSION. Nothing is written on a first-chance fault, so\n"
                "  there is no stall to desync on; the dump happens only when this client is\n"
                "  already lost. Neither handler changes execution - the witness returns\n"
                "  CONTINUE_SEARCH and the dumper hands over to the filter it displaced.\n");
        return;
    }

    appendf(reply, replySz, "ERR unknown command. Try `help`.\n");
}

// ---------------------------------------------------------------- the probe-thread drain
//
// Called once per pass of the probe loop, right where the hotkeys are read.

void drainControlCommand()
{
    if (g_ctrlState != CTRL_PENDING) return;

    // UI rows are machine-readable TSV, not a log tail. Keep the full capped reply and do not
    // mix in asynchronous logs or shrink it to the ordinary 8 KB direct-response buffer.
    if (argIs(g_ctrlCmd, 0, "ui")) {
        executeControl(g_ctrlCmd, g_ctrlReply, sizeof(g_ctrlReply));
        InterlockedExchange(&g_ctrlState, CTRL_DONE);
        return;
    }

    const long long before = logSizeNow();

    char cmd[512];
    strcpy_s(cmd, sizeof(cmd), g_ctrlCmd);

    char direct[8192];
    direct[0] = '\0';
    executeControl(cmd, direct, sizeof(direct));

    // The log tail first (that is where the report functions wrote), then anything the command
    // returned directly.
    logTailSince(before, g_ctrlReply, sizeof(g_ctrlReply) - sizeof(direct) - 8);
    if (direct[0]) strcat_s(g_ctrlReply, sizeof(g_ctrlReply), direct);
    if (!g_ctrlReply[0]) strcpy_s(g_ctrlReply, sizeof(g_ctrlReply), "(no output)\n");

    InterlockedExchange(&g_ctrlState, CTRL_DONE);
}

// ---------------------------------------------------------------- the pipe server

static DWORD WINAPI controlServerThread(LPVOID)
{
    // ⚠⚠ THIS LOOP USED TO HAVE NO EXIT AT ALL, and `startControlServer` threw the thread handle
    // away, so it could not even be joined. `detach` therefore ran FreeLibraryAndExitThread while
    // this thread sat blocked in the SYNCHRONOUS ConnectNamedPipe below — inside the module being
    // unmapped, with its return address in a page about to be freed. See stopControlServer.
    while (!g_ctrlExit) {
        HANDLE pipe = CreateNamedPipeA(kPipeName, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }

        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            continue;
        }

        // Woken by the shutdown poke rather than by a real client: leave without serving anything.
        if (g_ctrlExit) { DisconnectNamedPipe(pipe); CloseHandle(pipe); break; }

        char in[512];
        DWORD got = 0;
        if (ReadFile(pipe, in, sizeof(in) - 1, &got, nullptr) && got > 0) {
            in[got] = '\0';
            for (char* p = in; *p; ++p) if (*p == '\r' || *p == '\n') { *p = '\0'; break; }

            strcpy_s(g_ctrlCmd, sizeof(g_ctrlCmd), in);
            InterlockedExchange(&g_ctrlState, CTRL_PENDING);

            // Wait for the probe thread. It polls at 100 ms, and a full capture takes a while, so
            // this is generous - but it is BOUNDED, because a caller hanging forever on a pipe is
            // its own kind of failure.
            const DWORD deadline = GetTickCount() + 30000;
            while (g_ctrlState != CTRL_DONE && GetTickCount() < deadline) Sleep(10);

            const char* out;
            if (g_ctrlState == CTRL_DONE) {
                out = g_ctrlReply;
            } else {
                out = "ERR timed out waiting for the probe thread - is the game still running?\n";
                InterlockedExchange(&g_ctrlState, CTRL_IDLE);
            }

            DWORD written = 0;
            WriteFile(pipe, out, (DWORD)strlen(out), &written, nullptr);
            FlushFileBuffers(pipe);
            if (g_ctrlState == CTRL_DONE) InterlockedExchange(&g_ctrlState, CTRL_IDLE);
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

// Called from the detach tail after the reply can be sent. Cancel/poke synchronous I/O,
// then join. Only WAIT_OBJECT_0 proves the worker has exited; failed waits retain its handle.
void stopControlServer()
{
    stopUiControl();
    if (!g_ctrlServerUp) return;
    InterlockedExchange(&g_ctrlExit, 1);

    // 1. The API that exists for exactly this — and the reason the thread handle is now KEPT instead
    //    of being CloseHandle'd the moment the thread starts.
    if (g_ctrlThread) CancelSynchronousIo(g_ctrlThread);

    // 2. Connect as a client, which makes ConnectNamedPipe return normally. Belt and braces: if the
    //    thread was between CreateNamedPipeA and ConnectNamedPipe, (1) cancels nothing.
    HANDLE poke = CreateFileA(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);

    if (g_ctrlThread) {
        const DWORD wait = WaitForSingleObject(g_ctrlThread, 3000);
        if (wait != WAIT_OBJECT_0) {
            logf("control channel: shutdown incomplete (wait=%lu error=%lu); retaining thread handle and DLL; restart required",
                 wait, wait == WAIT_FAILED ? GetLastError() : 0);
            return;
        }
        logf("control channel: pipe thread exited");
        CloseHandle(g_ctrlThread);
        g_ctrlThread = nullptr;
    }
    g_ctrlServerUp = false;
}

bool startControlServer()
{
    if (g_ctrlServerUp) return true;
    // ⚠ The handle is KEPT. It used to be closed immediately, which made the thread unstoppable and
    // unjoinable — see stopControlServer and #62.
    g_ctrlThread = CreateThread(nullptr, 0, controlServerThread, nullptr, 0, nullptr);
    if (!g_ctrlThread) {
        logf("control channel: FAILED to start (CreateThread %lu). Hotkeys still work.", GetLastError());
        return false;
    }
    g_ctrlServerUp = true;
    // ✗ Said "every hotkey is now reachable" until 2026-08-11 — printed by a build that has no
    // hotkeys left. Caught by reading the smoke-test log, one line above the AUTO-CAPTUREs.
    // ⚠ `unblock` is no longer the only writer: `answer`, `blend`, `lend`, `panic` and `detach` all
    // write, and naming one of them is worse than naming none.
    logf("control channel: listening on %s — this is the ONLY way to drive the mod; the function "
         "keys were retired 2026-08-11. `help` lists the commands and marks the ones that WRITE.",
         kPipeName);
    return true;
}

#endif // developer facilities
