// detour.cpp - The generic register-free detour, plus the live-lobby pointer and its liveness check.
//
// Split out of the original single-file tw3k_coop.cpp. Behaviour is unchanged: this was pure
// code motion. ../tw3k_coop.ASBUILT.cpp remains the known-good single-file snapshot.

#include "tw3k.h"

// ---------------------------------------------------------------- generic detour
//
// Overwrites `len` bytes at `target` with a REGISTER-FREE absolute jump to `dest`, but only after
// the bytes match `expected` byte-for-byte. Builds a trampoline (stolen bytes + absolute jump
// back) and publishes it through *outOrig BEFORE the detour goes live, so a call landing in the
// window between the two never sees a null original.
//
// The jump is `FF 25 00000000` (JMP qword ptr [rip+0]) followed by the 8-byte target — 14 bytes
// that clobber NO registers.
//
// This matters: an earlier version used `MOV RAX,imm64 ; JMP RAX` and crashed the game. The lobby
// refresh FUN_142D57630 starts `MOV RAX,RSP` and then, PAST the stolen bytes, spills all its
// non-volatile registers relative to RAX (`MOV [RAX-0x18],RSI` and friends). Destroying RAX in the
// jump-back made it write those registers into the game's own code section. Never assume a
// scratch register is free just because it is volatile at the ABI level — the stolen prologue may
// have loaded it for the body to use.
//
// Preconditions verified by hand at each call site: `len` covers whole instructions, and none of
// them is RIP-relative (those would need relocating).

// struct Detour and the prototypes below now live in tw3k.h.

bool detourInstall(Detour& d, uintptr_t target, size_t len, const uint8_t* expected,
                   uintptr_t dest, void** outOrig, const char* tag)
{
    if (d.active) { logf("%s: already installed", tag); return true; }
    if (len < 14 || len > sizeof(d.orig)) { logf("%s: bad stolen length %zu", tag, len); return false; }

    uint8_t cur[sizeof(d.orig)] = { 0 };
    if (!safeRead((void*)target, cur, len)) {
        logf("%s: target %016llX unreadable", tag, (unsigned long long)target); return false;
    }
    for (size_t i = 0; i < len; ++i) if (cur[i] != expected[i]) {
        logf("%s: SIGNATURE MISMATCH at byte %zu (want %02X got %02X) — refusing to hook",
             tag, i, expected[i], cur[i]);
        return false;
    }

    d.page = (uint8_t*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!d.page) { logf("%s: VirtualAlloc failed, err=%lu", tag, GetLastError()); return false; }
    memset(d.page, 0, 0x1000);

    // trampoline: <stolen bytes> ; JMP [rip+0] ; <8-byte target>
    uint8_t*        t    = d.page + 0x40;
    const uintptr_t back = target + len;
    memcpy(t, cur, len);
    t[len + 0] = 0xFF; t[len + 1] = 0x25;
    memset(t + len + 2, 0, 4);              // disp32 = 0 -> target sits directly after
    memcpy(t + len + 6, &back, 8);
    if (outOrig) *outOrig = (void*)t;       // publish BEFORE the detour is live

    // detour: JMP [rip+0] ; <8-byte dest> ; NOP-pad to `len`
    uint8_t det[sizeof(d.orig)];
    memset(det, 0x90, sizeof(det));
    det[0] = 0xFF; det[1] = 0x25;
    memset(det + 2, 0, 4);
    memcpy(det + 6, &dest, 8);

    memcpy(d.orig, cur, len);
    d.target = target;
    d.len    = len;

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)target, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("%s: VirtualProtect failed, err=%lu", tag, GetLastError()); return false;
    }
    memcpy((void*)target, det, len);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, len);
    DWORD tmp = 0; VirtualProtect((void*)target, len, oldProtect, &tmp);

    d.active = true;
    logf("%s: INSTALLED at %016llX (trampoline=%016llX)",
         tag, (unsigned long long)target, (unsigned long long)t);
    return true;
}

// ---------------------------------------------------------------- 5-byte thunk redirect
//
// A `JMP rel32` reaches ±2GB, so the stub it points at has to live within that of the thunk. Our
// DLL does not: the trampolines above sit around 0x2E000000 while the image is at 0x140000000,
// roughly 4.3GB away, which is exactly why every other hook here uses a register-free ABSOLUTE
// jump and never needs to care where our code landed.
//
// So: allocate one page as close to the thunk as Windows will give us, and put the absolute jump
// there. 64KB granularity, walking outwards, both directions — the first page that takes wins.
void* allocNear(uintptr_t anchor)
{
    SYSTEM_INFO si = { 0 };
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t reach = 0x7FF00000ull;   // just inside 2GB, so the displacement always fits

    for (uintptr_t delta = gran; delta < reach; delta += gran) {
        for (int up = 0; up < 2; ++up) {
            const uintptr_t addr = (up ? anchor + delta : anchor - delta) & ~(gran - 1);
            if (addr < 0x10000) continue;
            void* p = VirtualAlloc((void*)addr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    }
    return nullptr;
}

bool thunkRedirect(Detour& d, uintptr_t thunk, uintptr_t dest, uintptr_t* outOriginal,
                   const char* tag)
{
    if (d.active) { logf("%s: already installed", tag); return true; }

    uint8_t cur[5] = { 0 };
    if (!safeRead((void*)thunk, cur, sizeof(cur))) {
        logf("%s: thunk %016llX unreadable", tag, (unsigned long long)thunk);
        return false;
    }
    if (cur[0] != 0xE9) {
        logf("%s: %016llX is not a JMP rel32 (first byte %02X) — refusing to patch. The thunk has "
             "moved or this build differs.", tag, (unsigned long long)thunk, cur[0]);
        return false;
    }

    // ★ DERIVED, NOT TYPED. The implementation address comes out of the instruction we are
    // replacing, so it cannot drift from it — the same lesson as the hijack RVA that carried an
    // extra digit and pointed 4GB outside the image.
    int32_t rel = 0;
    memcpy(&rel, cur + 1, 4);
    const uintptr_t original = thunk + 5 + (intptr_t)rel;
    if (outOriginal) *outOriginal = original;

    d.page = (uint8_t*)allocNear(thunk);
    if (!d.page) {
        logf("%s: no page available within 2GB of %016llX — cannot reach it with a rel32.",
             tag, (unsigned long long)thunk);
        return false;
    }
    memset(d.page, 0, 0x1000);

    // stub: JMP [rip+0] ; <8-byte dest>   — register-free, same shape as detourInstall's
    uint8_t* stub = d.page + 0x40;
    stub[0] = 0xFF; stub[1] = 0x25;
    memset(stub + 2, 0, 4);
    memcpy(stub + 6, &dest, 8);

    const int64_t disp = (int64_t)(uintptr_t)stub - (int64_t)(thunk + 5);
    if (disp < INT32_MIN || disp > INT32_MAX) {
        logf("%s: stub landed %lld bytes away, out of rel32 range", tag, (long long)disp);
        VirtualFree(d.page, 0, MEM_RELEASE);
        d.page = nullptr;
        return false;
    }

    memcpy(d.orig, cur, sizeof(cur));
    d.target = thunk;
    d.len    = sizeof(cur);

    uint8_t patch[5] = { 0xE9, 0, 0, 0, 0 };
    const int32_t d32 = (int32_t)disp;
    memcpy(patch + 1, &d32, 4);

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)thunk, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("%s: VirtualProtect failed, err=%lu", tag, GetLastError());
        return false;
    }
    memcpy((void*)thunk, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), (void*)thunk, sizeof(patch));
    DWORD tmp = 0; VirtualProtect((void*)thunk, sizeof(patch), oldProtect, &tmp);

    d.active = true;
    logf("%s: INSTALLED — thunk %016llX now reaches our stub at %016llX; the real implementation "
         "is %016llX (read out of the instruction, not typed).",
         tag, (unsigned long long)thunk, (unsigned long long)stub, (unsigned long long)original);
    return true;
}

// ★ The 5-byte ENTRY redirect, for hooking a real function whose opening bytes cannot be
// relocated.
//
// detourInstall writes 14 bytes and copies the span it stole into the trampoline VERBATIM. That is
// correct only while the stolen bytes hold no rel32 and no RIP-relative operand — a copied
// displacement is right at the address it came from and nowhere else.
//
// The 1.7.2 CCO resolver is exactly the case it is wrong for. Its entry reads
//     40 53           PUSH RBX
//     48 83 EC 20     SUB  RSP,0x20
//     48 8B D9        MOV  RBX,RCX
//     48 8B CA        MOV  RCX,RDX
//     E8 <rel32>      CALL <the CaString accessor>
// which is TWELVE bytes before a rel32. Fourteen lands inside that CALL and the trampoline would
// carry a displacement pointing somewhere arbitrary.
//
// So steal SIX — the first two instructions, neither relocatable — and reach the hook the way
// thunkRedirect does, with a 5-byte JMP to a proximity stub, because our DLL sits ~4.3GB from the
// image and cannot be reached by a rel32 directly. The sixth byte is NOP-padded. The trampoline is
// the stolen pair followed by an ABSOLUTE jump back to entry+stealLen, so calling it does exactly
// what calling the function did — which is what the hook needs, since plan E only acts on the
// answers the engine itself could not supply.
//
// ⚠ stealLen must cover WHOLE instructions. Nothing here decodes x86; the signature check is what
// stands in for that, so `expected` has to be the exact opening bytes and the caller is the one
// asserting where the instruction boundary falls.
bool entryRedirect5(Detour& d, uintptr_t entry, size_t stealLen, const uint8_t* expected,
                    uintptr_t dest, uintptr_t* outOriginal, const char* tag)
{
    if (d.active) { logf("%s: already installed", tag); return true; }
    if (stealLen < 5 || stealLen > sizeof(d.orig)) {
        logf("%s: bad steal length %zu (need 5..%zu)", tag, stealLen, sizeof(d.orig));
        return false;
    }

    uint8_t cur[sizeof(d.orig)] = { 0 };
    if (!safeRead((void*)entry, cur, stealLen)) {
        logf("%s: entry %016llX unreadable", tag, (unsigned long long)entry);
        return false;
    }
    for (size_t i = 0; i < stealLen; ++i) if (cur[i] != expected[i]) {
        logf("%s: SIGNATURE MISMATCH at byte %zu (want %02X got %02X) — refusing to hook. The "
             "function moved or this build differs; re-derive it rather than forcing this.",
             tag, i, expected[i], cur[i]);
        return false;
    }

    d.page = (uint8_t*)allocNear(entry);
    if (!d.page) {
        logf("%s: no page available within 2GB of %016llX — cannot reach it with a rel32.",
             tag, (unsigned long long)entry);
        return false;
    }
    memset(d.page, 0, 0x1000);

    // stub: JMP [rip+0] ; <8-byte dest>  — near enough for the entry's rel32 to reach, absolute
    // from there on, so where our DLL landed stops mattering.
    uint8_t* stub = d.page + 0x40;
    stub[0] = 0xFF; stub[1] = 0x25;
    memset(stub + 2, 0, 4);
    memcpy(stub + 6, &dest, 8);

    // trampoline: <the stolen instructions> ; JMP [rip+0] ; <8-byte entry+stealLen>
    uint8_t*        tramp = d.page + 0x80;
    const uintptr_t back  = entry + stealLen;
    memcpy(tramp, cur, stealLen);
    tramp[stealLen + 0] = 0xFF; tramp[stealLen + 1] = 0x25;
    memset(tramp + stealLen + 2, 0, 4);
    memcpy(tramp + stealLen + 6, &back, 8);
    if (outOriginal) *outOriginal = (uintptr_t)tramp;   // publish BEFORE the detour is live

    const int64_t disp = (int64_t)(uintptr_t)stub - (int64_t)(entry + 5);
    if (disp < INT32_MIN || disp > INT32_MAX) {
        logf("%s: stub landed %lld bytes away, out of rel32 range", tag, (long long)disp);
        VirtualFree(d.page, 0, MEM_RELEASE);
        d.page = nullptr;
        return false;
    }

    memcpy(d.orig, cur, stealLen);
    d.target = entry;
    d.len    = stealLen;

    uint8_t patch[sizeof(d.orig)];
    memset(patch, 0x90, sizeof(patch));          // anything past the JMP is NOP, not left behind
    patch[0] = 0xE9;
    const int32_t d32 = (int32_t)disp;
    memcpy(patch + 1, &d32, 4);

    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)entry, stealLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("%s: VirtualProtect failed, err=%lu", tag, GetLastError());
        return false;
    }
    memcpy((void*)entry, patch, stealLen);
    FlushInstructionCache(GetCurrentProcess(), (void*)entry, stealLen);
    DWORD tmp = 0; VirtualProtect((void*)entry, stealLen, oldProtect, &tmp);

    d.active = true;
    logf("%s: INSTALLED at %016llX (stole %zu bytes; stub=%016llX, trampoline=%016llX — the "
         "trampoline IS the original, so the hook calls it and not the patched entry).",
         tag, (unsigned long long)entry, stealLen,
         (unsigned long long)stub, (unsigned long long)tramp);
    return true;
}

void detourRemove(Detour& d, const char* tag)
{
    if (!d.active) return;
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)d.target, d.len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("%s: REMOVE FAILED (VirtualProtect error=%lu); hook remains active; restart required",
             tag, GetLastError());
        return;
    }
    if (!d.removalProtection) d.removalProtection = oldProtect;
    memcpy((void*)d.target, d.orig, d.len);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), (void*)d.target, d.len) != 0;
    DWORD tmp = 0;
    const bool protectedAgain = VirtualProtect((void*)d.target, d.len, d.removalProtection, &tmp) != 0;
    if (protectedAgain) d.removalProtection = 0;
    if (!flushed || !protectedAgain) {
        logf("%s: REMOVE INCOMPLETE (cache=%d protection=%d); retaining active state for retry; restart required",
             tag, flushed, protectedAgain);
        return;
    }
    d.active = false;
    logf("%s: removed (original prologue restored)", tag);
    // Keep the trampoline and DLL mapped: restoration does not prove callback rundown.
}

// g_liveLobby: the LIVE lobby instance, captured for free by the guard/panel hooks. The global
// at RVA_LOBBY_SINGLETON is a permanent idle decoy, so live lobby state must use this one.
// Declared in tw3k.h, as is capturedMp(), which session.cpp defines.
volatile uintptr_t g_liveLobby = 0;

// ★ RUN 7 (2026-07-30): the lobby object is FREED once the campaign starts, and its allocation is
// reused. An F3 capture taken in-battle dumped the stale pointer and printed nonsense — a player
// vector of cap=800450064 count=0, and an object body containing
// "terrain\tiles\campaign\canal_links\canal_link_1_mirror\custom_me". It happened not to fault, but
// dereferencing freed memory is luck, not safety, and the output is actively misleading.
//
// Cheap exact check: the lobby carries the listener interface at +0x80, whose vtable is the fixed
// image address 0x1437DC0A8. A live lobby always has it; the terrain data that replaced it does not
// (that dump read D0 EA 7E 63 there). Anything that walks g_liveLobby must pass this first.
//
// ~~⚠ 1.7.2: the ONE rebased address in the tree that is not independently corroborated.~~
// ★ CORROBORATED 2026-09-20, and the second source is the thing itself: a vtable is an array of
// code pointers, so it can simply be read. Both builds hold ten, all ten resolve into code, and
// the SHAPE is identical slot for slot - [0] unique, [1]=[2] a shared stub, [3][4][5] unique and
// ascending, [6..9] the same stub again - with the three unique entries 0x420 and 0x2B0 apart in
// BOTH builds. A positional read that landed on the wrong object would not reproduce that.
//
// The original caution was still right to be written: it was read positionally, out of a
// byte-matched function, and the sibling vtables around it grew from 0x18 to 0x48 apart, which is
// exactly when a positional read is least safe. It simply is not the weakest row any more.
// If lobbyLooksLive() ever starts refusing every lobby, this remains worth re-reading first.
static constexpr uintptr_t RVA_LISTENER_VTABLE = 0x037DC0A8;   // = 0x1437DC0A8

bool lobbyLooksLive(uintptr_t lobby)
{
    if (!lobby || lobby < 0x10000) return false;
    uintptr_t vt = 0;
    if (!readAt(lobby + 0x80, vt)) return false;
    return vt == g_base + RVA_LISTENER_VTABLE;
}

