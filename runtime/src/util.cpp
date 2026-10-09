// util.cpp - Hotkey focus gate, SEH-guarded reads, the change detector, and the instance finder.
//
// Split out of the original single-file tw3k_coop.cpp. Behaviour is unchanged: this was pure
// code motion. ../tw3k_coop.ASBUILT.cpp remains the known-good single-file snapshot.

#include "tw3k.h"

// ---------------------------------------------------------------- hotkey focus gate
//
// GetAsyncKeyState reads GLOBAL key state, so it fires regardless of which window has focus. Over a
// remote-desktop session that means one keypress is seen by BOTH the local game and the remote one —
// which is how F8 got applied to the host as well as the intended client, and is how the host ended up
// switching to Sun Jian unintentionally.
//
// Gate every hotkey on our own process owning the foreground window. Then a keypress only ever affects
// the machine you are actually looking at, local or remote.
#ifndef TW3K_RELEASE
bool gameHasFocus()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

#endif
// ---------------------------------------------------------------- safe reads

// The lobby singleton is only populated while an MP campaign lobby is open,
// and pointers inside it are garbage before that. Guard every dereference.
bool safeRead(const void* addr, void* out, size_t len)
{
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;

    __try {
        memcpy(out, addr, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// readAt<T> is a template, so it lives in tw3k.h.

#ifndef TW3K_RELEASE
// ---------------------------------------------------------------- change detector

// We were not 100% sure +0xCC is the live player count (a solo host read 0).
// Instead of trusting one offset, snapshot a window of the lobby object and log
// the moment ANY byte changes — when a player joins we will SEE which field moves.
static constexpr uintptr_t WIN_OFF = 0xA0;   // covers +0xB0 session .. +0x140 UI ptrs
static constexpr size_t    WIN_LEN = 0xB0;

static uint8_t g_win[WIN_LEN] = { 0 };
static bool    g_haveWin      = false;

static void hexdumpWindow(const uint8_t* buf)
{
    const uintptr_t base = g_base + RVA_LOBBY_SINGLETON + WIN_OFF;
    for (size_t row = 0; row < WIN_LEN; row += 16) {
        char line[128]; int n = 0;
        n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE,
                         "    +0x%03zX:", WIN_OFF + row);
        for (size_t i = 0; i < 16 && row + i < WIN_LEN; ++i)
            n += _snprintf_s(line + n, sizeof(line) - n, _TRUNCATE, " %02X", buf[row + i]);
        logf("%s", line);
    }
    (void)base;
}

// Returns true and logs if the window changed since last snapshot.
bool detectChange(bool forceDump)
{
    const uintptr_t base = g_base + RVA_LOBBY_SINGLETON + WIN_OFF;
    uint8_t cur[WIN_LEN];
    if (!safeRead((void*)base, cur, WIN_LEN)) return false;

    bool changed = false;
    if (g_haveWin) {
        for (size_t i = 0; i < WIN_LEN; ++i) {
            if (cur[i] != g_win[i]) {
                logf("CHANGE @ +0x%03zX : %02X -> %02X",
                     WIN_OFF + i, g_win[i], cur[i]);
                changed = true;
            }
        }
    }
    if (changed || forceDump || !g_haveWin) {
        logf("lobby window snapshot:");
        hexdumpWindow(cur);
    }
    memcpy(g_win, cur, WIN_LEN);
    g_haveWin = true;
    return changed;
}

// ---------------------------------------------------------------- instance finder

// The global singleton turned out to be idle; the LIVE lobby is a separate
// (heap) instance. Every MPCampaignLobby object begins with a pointer to the
// same vtable, so scan committed memory for that pointer — each hit is an
// instance. Whichever one has a non-zero player count is the live lobby.
static void scanForLobby()
{
    const uintptr_t globalInst = g_base + RVA_LOBBY_SINGLETON;

    // Read the ACTUAL vtable pointer from the (constructed) global object rather
    // than hardcoding an address we might have wrong.
    uint64_t vtbl = 0;
    if (!safeRead((void*)globalInst, &vtbl, 8) || vtbl < 0x140000000ULL) {
        logf("SCAN: global object[0] unreadable/invalid (%016llX) — can't derive vtable",
             (unsigned long long)vtbl);
        return;
    }
    logf("SCAN: global vtable=%016llX ; scanning all committed private+image RW memory (no cap)...",
         (unsigned long long)vtbl);

    MEMORY_BASIC_INFORMATION mbi{};
    uint8_t* addr = nullptr;
    int  hits    = 0;
    uint64_t scanned = 0;
    uintptr_t liveCandidate = 0;   // best guess at the real active lobby
    bool capped = false;

    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint8_t* next = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        // Objects live in private heap (MEM_PRIVATE) or the exe's data (MEM_IMAGE).
        // Skip MEM_MAPPED (huge asset/file mappings) so the scan stays fast.
        const bool wantType = (mbi.Type == MEM_PRIVATE) || (mbi.Type == MEM_IMAGE);
        const DWORD rw = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && wantType &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (mbi.Protect & rw)) {
            __try {
                uint64_t*    p    = (uint64_t*)mbi.BaseAddress;
                const size_t n    = mbi.RegionSize / sizeof(uint64_t);
                for (size_t i = 0; i < n; ++i) {
                    if (p[i] != vtbl) continue;
                    uintptr_t inst = (uintptr_t)(p + i);
                    // Only read object fields if the whole object fits in-region.
                    if ((uint8_t*)inst + 0x1A0 > next) {
                        logf("  INSTANCE @ %016llX  (near region end, fields not read) %s",
                             (unsigned long long)inst,
                             inst == globalInst ? "(GLOBAL/idle)" : "<<< HEAP");
                    } else {
                        uint32_t cnt = *(uint32_t*)(inst + OFF_PLAYER_COUNT);
                        uint64_t rec = *(uint64_t*)(inst + OFF_PLAYER_RECORDS);
                        uint64_t s0  = *(uint64_t*)(inst + OFF_SLOT_STRINGS + 0x08);
                        uint64_t s1  = *(uint64_t*)(inst + OFF_SLOT_STRINGS + 0x18);
                        logf("  INSTANCE @ %016llX  +0xCC=%u  +0xD0=%016llX  slot0=%016llX slot1=%016llX %s",
                             (unsigned long long)inst, cnt, (unsigned long long)rec,
                             (unsigned long long)s0, (unsigned long long)s1,
                             inst == globalInst ? "(GLOBAL/idle)" : "<<< HEAP INSTANCE");
                        // Heuristic for the live lobby: sane count, real records ptr,
                        // at least one non-empty slot, and not the idle global.
                        if (inst != globalInst && cnt >= 1 && cnt <= 64 &&
                            rec > 0x10000 && s0 != 0 && s0 != EMPTY_STR_SENTINEL &&
                            liveCandidate == 0)
                            liveCandidate = inst;
                    }
                    if (++hits >= 128) { logf("  (128 hits, stopping)"); capped = true; break; }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            scanned += mbi.RegionSize;
        }
        if (capped) break;
        if (next <= addr) break; // overflow / no progress
        addr = next;
    }
    logf("SCAN done: %d hit(s), %llu MB scanned", hits, (unsigned long long)(scanned >> 20));
    if (liveCandidate) dumpInstanceDeep(liveCandidate);
    else logf("SCAN: no live-lobby candidate matched the heuristic "
              "(be in a populated MP campaign lobby before pressing F8)");
}


#endif
