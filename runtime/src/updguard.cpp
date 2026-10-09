#ifndef TW3K_RELEASE
// updguard.cpp - #75: the null check that FUN_142509A40 omits at all three of its call sites.
// DISARMED at attach. `updguard on|off`.
//
// ================================================================================================
// ★★★★★★★ THE BUG, AND THE ENGINE ALREADY HANDLES IT EVERYWHERE ELSE
// ================================================================================================
//
// Three times in a row, the per-frame virtual update `FUN_142509A40` does this:
//
//     lea  rcx, [<owner> + 0x610]      ; the registry container
//     mov  rdx, <entity>               ; the key
//     call 0x142561EF0                 ; hash-map lookup  ->  RAX, and 0 ON A MISS
//     lea  rcx, [rax + 0x40]           ; RAX = 0  =>  RCX = 0x40
//     call 0x142575B00                 ; first body instruction is MOV RAX,[RCX]  -> CTD
//
// `FUN_142561EF0` (via `FUN_142562BE0`, FNV-1a) returns 0 on a miss. The other two callers of that
// lookup in the whole binary both `TEST RAX,RAX` and branch to a dedicated fallback:
//
//     142013ED6:  TEST RAX,RAX / JZ -> CALL 0x1425737A0   (fallback on the entity itself)
//     14225AD38:  the same shape
//
// => A miss on this map is a NORMAL, EXPECTED outcome that CA wrote a fallback for. It is not
// corruption. The defect is that `FUN_142509A40` is the one caller that never checks.
//
// A byte scan of the raw PE confirms the inventory the ticket asserts: `FUN_142561EF0` has exactly
// five direct call sites, at exactly the five addresses #75 lists.
//
// ================================================================================================
// !! THE ISSUE NAMES THE WRONG CALL SITE
// ================================================================================================
//
// #75 attributes the 2026-08-19 crash to `0x142509C27` and cites "three independent register
// matches". Two of the three do not discriminate between the sites at all:
//
// | evidence | site 1 `142509C27` | site 2 `142509EAD` | site 3 `14250A167` |
// |---|---|---|---|
// | `LEA RDX` target | `0x144340AA8` | `0x144340AA8` | `0x144340AA8` |
// | `RCX = 0x40` | yes | yes | yes |
// | **`R8 - RBP`** | `0x110` | `0x110` | * **`0x118`** |
// | **`R9`** | `RSP+0x60` | `RSP+0x60` | * **`RBP-0x78`** |
// | `RBX` at the call | a loop index (`MOV EBX,R14D`) | the container | the container |
//
// The crash logged `R8 - RBP = 0x118`, `R9 - RBP = -0x78`, and `RBX` = a live heap pointer on both
// machines. => **The crash was at site 3, `0x14250A167`.** The static `0x144340AA8` is loaded
// identically by all three sites, so it pins the FUNCTION and never the site.
//
// Read from the raw PE, not the decompiler: Ghidra was not running, so the bytes were taken out of
// `Three_Kingdoms.exe` directly and the three sites disassembled from disk.
//
// => * **That is why all three sites are guarded and not only the one #75 names.** Guarding
// `0x142509C27` alone would have shipped a fix that could not have prevented the crash that
// motivated it - and it would have looked correct until the next session.
//
// ---------------------------------------------------------------------------- what we write
//
// Nine contiguous bytes at each site, all three identical apart from the call's rel32:
//
//     48 8D 48 40      lea  rcx, [rax + 0x40]        ->   48 8B C8      mov  rcx, rax
//     E8 <rel32>       call 0x142575B00              ->   E8 <rel32>    call <our stub>
//                                                         90            nop
//
// `mov rcx, rax` hands our stub the RAW record - 0 on a miss - instead of `record + 0x40`, so the
// null is a value we can test rather than an address we have already formed. The stub re-adds the
// 0x40 on the found path.
//
// * NO TRAMPOLINE, DELIBERATELY. `FUN_1425805B0` opens `MOV R11,RSP` and then writes the caller's
// shadow store through R11 (`MOV [R11+0x20],R9`, `MOV [R11+8],RCX`). Stealing that prologue into a
// trampoline would re-run it against OUR stack and spill the caller's arguments into our frame -
// the exact failure detour.cpp's header describes for `MOV RAX,RSP`. Patching the CALL instead of
// the callee means the original is entered by an ordinary CALL, with a real frame, unmodified.
//
// ---------------------------------------------------------------------------- the instrument half
//
// The entity is the thing worth knowing, and it is not an argument - it lives in a non-volatile
// register that differs per site (`RSI` at site 1, `RDI` at sites 2 and 3, read off the
// `MOV RDX,<reg>` that feeds the lookup and confirmed by the `[<reg>+0x3308]` deref immediately
// after the guarded call). So each stub stashes it before tailing into the shared handler:
//
//     48 B8 <&g_lastEntity>    mov rax, imm64      ; RAX is dead here - the record is in RCX now
//     48 89 30 | 48 89 38      mov [rax], rsi|rdi  ; the entity, per site
//     FF 25 00000000 <dest>    jmp [rip+0]         ; tail into updGuardCommon, args untouched
//
// A tail JMP, not a CALL, so the return address the handler sees is the GAME's - which is what
// tells it which of the three sites it is standing in.
//
// ! We log the entity RAW - the pointer and a couple of fields read through `safeRead` - and work
// out what it was offline. Calling `FUN_142386970` for its type at the moment of the miss would be
// an engine call from inside a per-frame update on the game thread, which is a heavier risk class
// than this ticket needs. The pointer plus a dump names the entity just as well, a day later.
//
// ---------------------------------------------------------------------------- what the miss does
//
// `FUN_1425805B0` zeroes three outputs in its own prologue, BEFORE the faulting load:
//
//     142575B21  MOV dword ptr [RDX+4], EDI    ; EDI was just XORed to 0
//     142575B27  MOV byte  ptr [R8], DIL
//     142575B2D  MOV RAX, qword ptr [RCX]      ; <- the fault
//     142575B35  MOV dword ptr [R9], EDI       ; NOT reached
//
// So the guard performs all three and returns. That is what the function produces for an input it
// walks and finds nothing in: the empty-list path at `0x142575CDE` reads `[static+4]` - which B21
// has just zeroed - and falls straight through `CMP EDX,1 / JBE` to the epilogue at `0x142575F32`.
//
// * The return value is NOT used at any of the three sites, and `0x142575F32` does not set EAX
// either, so the engine's own empty path returns whatever the body happened to leave there. We
// return 0. The `TEST AL,AL` that follows site 1 belongs to `FUN_14079C970`, two instructions later,
// not to this call.
//
// ---------------------------------------------------------------------------- blast radius
//
// `FUN_1425805B0` has six callers, three more than #75 accounts for. The three outside
// `FUN_142509A40` (`142559007`, `142559085`, `142566744`) all pass RCX from a live object
// (`MOV RCX,RSI` / `MOV RCX,RDI`), never from a lookup result - so they cannot produce the null this
// guards. They are untouched regardless, because the patch is at the call site, not the callee.
//
// !! EVERY MACHINE OR NONE. `FUN_1425805B0` computes distances and angles and writes results the
// update consumes, so a machine that skips an entity and a machine that crashes on it are not
// running the same frame. In practice the unpatched machine is the one that dies - but that is not
// the same as "safe to run mixed", and `ping` stays the check.
//
// ! DISARMED AT ATTACH, deliberately, and not because it is doubted. #75's own running order is
// "dump first, patch second": a FULL dump taken inside that battle names the entity with no record,
// and a guard that is already swallowing the miss removes the crash that makes the battle findable.
// Arm it once that dump exists - or to survive an evening that would otherwise end.

#include "tw3k.h"
#include "offsets.h"
#include <intrin.h>

// ---------------------------------------------------------------- the sites
//
// RVAs of the CALL. The nine-byte patch span starts four bytes earlier, at the LEA.
static constexpr uintptr_t RVA_SITE_CALL[3] = { 0x02514687, 0x0251490D, 0x02514BC7 };

// Which non-volatile register holds the entity at each site, as the ModRM byte for `MOV [RAX],<reg>`.
static constexpr uint8_t   ENT_RSI = 0x30;
static constexpr uint8_t   ENT_RDI = 0x38;
static constexpr uint8_t   SITE_ENT_MODRM[3] = { ENT_RSI, ENT_RDI, ENT_RDI };
static const char* const   SITE_ENT_NAME[3]  = { "RSI", "RDI", "RDI" };
static const char* const   SITE_LABEL[3]     = { "1", "2", "3" };

static constexpr size_t    PATCH_LEN            = 9;          // LEA(4) + CALL(5)
static constexpr size_t    STUB_LEN             = 27;
static constexpr uintptr_t RVA_FUN_1425805B0    = 0x025805B0;

// 48 8D 48 40 E8 <rel32>. The rel32 differs per site, so it is checked by DESTINATION instead.
static const uint8_t EXPECT_LEA_CALL[5] = { 0x48, 0x8D, 0x48, 0x40, 0xE8 };

// ---------------------------------------------------------------- state

typedef char (*Fn575B00)(void* rec, void* outStruct, unsigned char* outFlag, unsigned int* outCount,
                         unsigned char mode);

static Fn575B00  g_orig575B00 = nullptr;
static bool      g_armed      = false;
static uint8_t   g_origBytes[3][PATCH_LEN] = { { 0 } };
static uint8_t*  g_stub[3]    = { nullptr, nullptr, nullptr };
static uintptr_t g_retAddr[3] = { 0, 0, 0 };      // what _ReturnAddress() reads, per site

// * Written by the stubs, read by the handler. One slot and not three: the stub runs immediately
// before the handler on the same thread, so there is no window in which another site overwrites it.
extern "C" void* g_lastEntity = nullptr;

// ! Plain increments, for the reason the #73 counters are plain (see gift.cpp): this answers "did
// this fire at all, and roughly how often", and a lost count does not change that answer.
// Magnitudes only - never a small difference between two of them.
static unsigned long long g_calls  = 0;   // every entry, both paths
static unsigned long long g_misses = 0;   // the record was null - the crash that did not happen
static unsigned int       g_logged = 0;   // bounded log output

static constexpr unsigned int LOG_FIRST_N = 8;

// ---------------------------------------------------------------- the handler
//
// Reached by a tail JMP from a stub, so RCX/RDX/R8/R9 and the 5th argument at [RSP+0x20] are exactly
// what the game's call site set up, and the return address on the stack is the game's.
extern "C" char updGuardCommon(void* record, void* outStruct, unsigned char* outFlag,
                               unsigned int* outCount, unsigned char mode)
{
    ++g_calls;

    if (record)                                    // the ordinary case, and the whole hot path:
        return g_orig575B00((char*)record + 0x40,  // re-add the 0x40 the LEA used to add
                            outStruct, outFlag, outCount, mode);

    ++g_misses;

    // What the engine does for an input it finds nothing in - the three writes out of its own
    // prologue, two of which it had already performed before it faulted.
    if (outStruct) *(unsigned int*)((char*)outStruct + 4) = 0;   // 142575B21
    if (outFlag)   *outFlag  = 0;                                // 142575B27
    if (outCount)  *outCount = 0;                                // 142575B35

    if (g_logged < LOG_FIRST_N) {
        ++g_logged;
        const uintptr_t ret = (uintptr_t)_ReturnAddress();
        int site = -1;
        for (int i = 0; i < 3; ++i) if (g_retAddr[i] == ret) { site = i; break; }

        void* const ent = g_lastEntity;
        uintptr_t vt = 0, sub = 0;
        const bool gotVt = ent && safeRead(ent, &vt, sizeof(vt));
        // The site itself dereferences this two instructions later (`MOV RCX,[<ent>+0x3308]`), so it
        // is the one field we know the engine considers live on this object.
        const bool gotSub = ent && safeRead((char*)ent + 0x3308, &sub, sizeof(sub));

        logf(">>> #75 UPD GUARD: MISS #%llu SWALLOWED - the registry had no record for this entity, "
             "and vanilla would have read [0x40] and died here.", (unsigned long long)g_misses);
        logf("    site %s (RVA %08llX, entity in %s), return %016llX",
             site >= 0 ? SITE_LABEL[site] : "UNKNOWN",
             site >= 0 ? (unsigned long long)RVA_SITE_CALL[site] : 0ull,
             site >= 0 ? SITE_ENT_NAME[site] : "?", (unsigned long long)ret);
        logf("    entity %016llX  vtable %016llX%s  [+0x3308] %016llX%s",
             (unsigned long long)(uintptr_t)ent,
             (unsigned long long)vt,  gotVt  ? "" : " (unreadable)",
             (unsigned long long)sub, gotSub ? "" : " (unreadable)");
        if (g_logged == LOG_FIRST_N)
            logf("    ! that is %u - no more will be logged this run. The counters keep going; read "
                 "them with `capture`.", LOG_FIRST_N);
    }
    return 0;
}

// ---------------------------------------------------------------- install
//
// The stub has to sit within a CALL rel32 of the site, which our DLL is not: the image is at
// 0x140000000 and our pages land roughly 4.3GB away. allocNear walks outwards from the site.

static bool writeCode(uintptr_t addr, const void* src, size_t len, const char* what)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("#75 upd guard: VirtualProtect failed for %s, err=%lu - NOT patched",
             what, GetLastError());
        return false;
    }
    memcpy((void*)addr, src, len);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    DWORD tmp = 0; VirtualProtect((void*)addr, len, oldProtect, &tmp);
    return true;
}

// Build the 27-byte stub for one site into `page`.
static void buildStub(uint8_t* page, uint8_t entModrm)
{
    size_t o = 0;
    void** const slot = &g_lastEntity;
    const void*  dest = (const void*)&updGuardCommon;

    page[o++] = 0x48; page[o++] = 0xB8;                          // mov rax, imm64
    memcpy(page + o, &slot, 8); o += 8;
    page[o++] = 0x48; page[o++] = 0x89; page[o++] = entModrm;    // mov [rax], rsi|rdi
    page[o++] = 0xFF; page[o++] = 0x25;                          // jmp [rip+0]
    memset(page + o, 0, 4); o += 4;
    memcpy(page + o, &dest, 8); o += 8;
}

bool patchUpdateNullGuard()
{
    if (g_armed) return true;

    g_orig575B00 = (Fn575B00)(g_base + RVA_FUN_1425805B0);

    // ---- verify all three BEFORE writing any of them. A half-patched function is worse than an
    // unpatched one, and the signature is the only thing between us and a game update.
    uint8_t cur[3][PATCH_LEN] = { { 0 } };
    for (int i = 0; i < 3; ++i) {
        const uintptr_t span = g_base + RVA_SITE_CALL[i] - 4;
        if (!safeRead((void*)span, cur[i], PATCH_LEN)) {
            logf("#75 upd guard: site %d span %016llX unreadable - nothing written.",
                 i + 1, (unsigned long long)span);
            return false;
        }
        for (size_t b = 0; b < sizeof(EXPECT_LEA_CALL); ++b) if (cur[i][b] != EXPECT_LEA_CALL[b]) {
            logf("#75 upd guard: site %d SIGNATURE MISMATCH at byte %zu (want %02X got %02X) - "
                 "refusing. Re-derive the sites against the new binary rather than forcing this.",
                 i + 1, b, EXPECT_LEA_CALL[b], cur[i][b]);
            return false;
        }
        int32_t rel = 0;
        memcpy(&rel, cur[i] + 5, sizeof(rel));
        const uintptr_t dest = g_base + RVA_SITE_CALL[i] + 5 + (intptr_t)rel;
        if (dest != g_base + RVA_FUN_1425805B0) {
            logf("#75 upd guard: site %d calls %016llX, not FUN_1425805B0 - refusing. Nothing was "
                 "written.", i + 1, (unsigned long long)dest);
            return false;
        }
    }

    // ---- stubs first, so a failure here leaves the game untouched.
    for (int i = 0; i < 3; ++i) {
        const uintptr_t site = g_base + RVA_SITE_CALL[i];
        g_stub[i] = (uint8_t*)allocNear(site);
        if (!g_stub[i]) {
            logf("#75 upd guard: no page within CALL reach of site %d (%016llX) - nothing written.",
                 i + 1, (unsigned long long)site);
            for (int j = 0; j < i; ++j) { VirtualFree(g_stub[j], 0, MEM_RELEASE); g_stub[j] = nullptr; }
            return false;
        }
        memset(g_stub[i], 0xCC, 0x1000);
        buildStub(g_stub[i], SITE_ENT_MODRM[i]);
        FlushInstructionCache(GetCurrentProcess(), g_stub[i], STUB_LEN);
    }

    // ---- and now the nine bytes, per site.
    for (int i = 0; i < 3; ++i) {
        const uintptr_t span = g_base + RVA_SITE_CALL[i] - 4;
        memcpy(g_origBytes[i], cur[i], PATCH_LEN);

        // ! The rel32 is relative to the END OF THE CALL, which is span+8 - NOT span+PATCH_LEN.
        // The patch is `mov rcx,rax`(3) + `call rel32`(5) + `nop`(1): the CALL occupies span+3..+7
        // and ends at span+8, and the trailing NOP is not part of it. Computing against span+9 puts
        // the destination one byte below the stub, which disassembles as garbage and is an instant
        // CTD. Caught by disassembling the emitted bytes rather than by reading them.
        static constexpr size_t CALL_AT  = 3;                    // offset of the E8 within the patch
        static constexpr size_t CALL_END = CALL_AT + 5;          // = 8, where the rel32 is measured from
        const intptr_t rel = (intptr_t)g_stub[i] - (intptr_t)(span + CALL_END);
        if (rel > 0x7FFFFFFF || rel < -0x7FFFFFFF) {      // allocNear should make this impossible
            logf("#75 upd guard: site %d stub is out of CALL range (%lld) - nothing written.",
                 i + 1, (long long)rel);
            return false;
        }
        const int32_t rel32 = (int32_t)rel;

        uint8_t patch[PATCH_LEN];
        patch[0] = 0x48; patch[1] = 0x8B; patch[2] = 0xC8;    // mov rcx, rax
        patch[CALL_AT] = 0xE8;                                // call rel32
        memcpy(patch + CALL_AT + 1, &rel32, 4);
        patch[CALL_END] = 0x90;                               // nop, to fill the ninth byte

        if (!writeCode(span, patch, PATCH_LEN, "a call site")) return false;
        g_retAddr[i] = span + CALL_END;                        // the NOP - what _ReturnAddress() reads
    }

    g_armed = true;
    logf(">>> #75 UPD GUARD: ARMED. Three call sites in FUN_142509A40 (RVA %08llX, %08llX, %08llX) "
         "now test the registry lookup for null before using it, which is the check its two sibling "
         "callers already do. A miss is skipped and logged instead of dereferencing 0x40.",
         (unsigned long long)RVA_SITE_CALL[0], (unsigned long long)RVA_SITE_CALL[1],
         (unsigned long long)RVA_SITE_CALL[2]);
    logf("    ! EVERY MACHINE OR NONE - a machine that skips an entity and a machine that crashes on "
         "it are not running the same frame. `ping` is the check.");
    return true;
}

void unpatchUpdateNullGuard()
{
    if (!g_armed) return;
    for (int i = 0; i < 3; ++i)
        writeCode(g_base + RVA_SITE_CALL[i] - 4, g_origBytes[i], PATCH_LEN, "a call site (restore)");

    // ! The stub pages are NOT freed. A thread parked inside one would return into unmapped memory,
    // which is #62's whole lesson. Three pages is a cheap price for never having to prove that
    // nothing is standing in them.
    g_armed = false;
    logf("#75 upd guard: restored - a registry miss in FUN_142509A40 crashes again, as vanilla.");
}

bool updateNullGuardArmed() { return g_armed; }

bool setUpdateNullGuard(bool on, const char** why)
{
    if (on == g_armed) return true;
    if (on) {
        if (!patchUpdateNullGuard()) {
            if (why) *why = "the sites did not verify - see the log";
            return false;
        }
        return true;
    }
    unpatchUpdateNullGuard();
    return true;
}

void reportUpdateNullGuard()
{
    logf("#75 upd guard (%s): calls=%llu  misses=%llu  logged=%u%s",
         g_armed ? "ARMED" : "DISARMED", (unsigned long long)g_calls,
         (unsigned long long)g_misses, g_logged,
         g_armed ? "" : "  - nothing is patched, so these are whatever the last armed run left");
    if (g_armed && g_misses == 0)
        logf("    no miss yet. => every entity this update touched had a registry record, so the "
             "#75 crash could not have fired in what has been played so far.");
}

#endif // developer facilities
