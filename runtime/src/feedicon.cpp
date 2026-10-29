// feedicon.cpp — #69: the event-feed `IconPath` getter faults on an empty feed, and the fix is to
// let it ask its own question in the right order.
//
// ============================================================================================
// THE DEFECT, WHICH IS CA'S AND NOT OURS
//
// `FUN_142F5EBA0` is the CCO getter for **`IconPath`** on an event-feed entry (registered at
// `0x14028C029`, next to `EventType`, `HasZoomLocation`, `ProvincesList`). It opens like this:
//
//     142F5EBCE  MOV  RCX,[R13+0x98]              ; the entry's tag object
//     142F5EBD5  MOV  RDI,[R13+0x90]              ; the entry's SOURCE ITEM  <- may be null
//     142F5EBDC  CALL 0x14038D0B0                 ; bag = get_property_bag_TIME_LINE_GROUP(tag)
//     142F5EBFC  MOV  RBX,[RAX+0x8]               ; ★ FAULT: RAX is null when the feed is empty
//     142F5EC00  MOV  [RBP+0x30],RBX              ; ...spilled...
//     142F5EC04  TEST RDI,RDI                     ; ...and only NOW is the item null-checked...
//     142F5EC07  JZ   0x142F5EFE8                 ; ...two instructions too late
//
// Three crashes on 2026-08-14, all at `142F5EBFC`, all with `RAX=0, RDI=0`, all on the machine at
// registry index 2. The compiler hoisted a load above the branch that decides whether the value is
// ever wanted — and on the null path it is **not** wanted: `[RBP+0x30]` is read exactly once, at
// `142F5EC2E`, which is inside the non-null branch, and `RBX` is rewritten at `142F5F007` before the
// null path uses it. ⇒ the load is DEAD on the path that faults.
//
// ⚠ And the null path is not an error path. `LAB_142F5EFE8` builds a *default* icon from the tag's
// own name (`"UI/Campaign UI/message_icons/" + name_FAST_XML_TAG(...)`). The game asks for that
// constantly and it works. The entry with no source item is meant to get the generic icon.
//
// ============================================================================================
// THE FIX: SEVENTEEN BYTES, REORDERED, SAME LENGTH
//
//     was                                  becomes
//     MOV  RBX,[RAX+8]      48 8B 58 08    TEST RDI,RDI          48 85 FF
//     MOV  [RBP+0x30],RBX   48 89 5D 30    JZ   <same target>    0F 84 (rel+8)
//     TEST RDI,RDI          48 85 FF       MOV  RBX,[RAX+8]      48 8B 58 08
//     JZ   +0x3DB           0F 84 DB030000 MOV  [RBP+0x30],RBX   48 89 5D 30
//
// 17 bytes in, 17 bytes out, no padding, and the branch keeps the identical target: the new `JZ`
// sits 8 bytes earlier in the stream, so its displacement is the old one **+8**, computed here
// rather than hard-coded.
//
// ⚠ WHY THIS IS A REORDER AND NOT A GUARD. A hook would have to reproduce `LAB_142F5EFE8` — CA
// string building with the shared-empty sentinel and the inline-tag test, three allocations and
// three conditional frees. Reordering asks the engine to do its own work in the order it meant to.
//
// ============================================================================================
// ⚠ WHAT IT DOES **NOT** FIX, said plainly
//
// It removes the crash, not the cause. Something is presenting an event-feed entry while the feed is
// empty, and after this patch that something renders a generic icon instead of killing the client.
// Two candidates are open and this changes neither: `blend`, which arms itself and makes 20 UI
// emitters fire out of turn, and our own event-cursor borrow, which parks a cursor in `+0x1F8` —
// the event list's SIZE — for the duration of every collect on the seat at registry index 2.
// ⇒ Keep #69 open on the cause. This is the airbag, not the brakes.
// ============================================================================================

#include "tw3k.h"
#include "offsets.h"

static constexpr uintptr_t RVA_ICONPATH_HOIST = 0x02F674AC;
static constexpr size_t    ICONPATH_LEN       = 17;
static constexpr size_t    ICONPATH_REL_AT    = 13;   // the JZ's rel32 inside the original run

static const uint8_t EXPECT_ICONPATH[ICONPATH_LEN] = {
    0x48,0x8B,0x58,0x08,             // MOV  RBX,[RAX+0x8]
    0x48,0x89,0x5D,0x30,             // MOV  [RBP+0x30],RBX
    0x48,0x85,0xFF,                  // TEST RDI,RDI
    0x0F,0x84,0xDB,0x03,0x00,0x00    // JZ   0x142F5EFE8
};

// ★ The self-check that makes a wrong write essentially impossible: the branch target must begin
// with `MOV RAX,[R12]`, which is `LAB_142F5EFE8`'s first instruction. Signature plus destination —
// if either has moved in a game patch, we refuse rather than reorder something else.
static const uint8_t EXPECT_TARGET[4] = { 0x49, 0x8B, 0x04, 0x24 };

static bool    g_iconPatched = false;
static uint8_t g_iconOrig[ICONPATH_LEN] = { 0 };

static bool writeCode(uintptr_t addr, const void* src, size_t len)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("feed icon guard: VirtualProtect failed, err=%lu — NOT patched", GetLastError());
        return false;
    }
    memcpy((void*)addr, src, len);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    DWORD tmp = 0; VirtualProtect((void*)addr, len, oldProtect, &tmp);
    return true;
}

bool patchFeedIconGuard()
{
    if (g_iconPatched) return true;
    const uintptr_t addr = g_base + RVA_ICONPATH_HOIST;

    uint8_t cur[ICONPATH_LEN] = { 0 };
    if (!safeRead((void*)addr, cur, ICONPATH_LEN)) {
        logf("feed icon guard: %016llX unreadable — NOT patched", (unsigned long long)addr);
        return false;
    }
    for (size_t i = 0; i < ICONPATH_LEN; ++i) if (cur[i] != EXPECT_ICONPATH[i]) {
        logf("feed icon guard: SIGNATURE MISMATCH at byte %zu (want %02X got %02X) — refusing",
             i, EXPECT_ICONPATH[i], cur[i]);
        return false;
    }

    int32_t rel = 0;
    memcpy(&rel, cur + ICONPATH_REL_AT, sizeof(rel));
    const uintptr_t target = addr + ICONPATH_LEN + (intptr_t)rel;   // next insn after the JZ + rel

    uint8_t tgt[sizeof(EXPECT_TARGET)] = { 0 };
    if (!safeRead((void*)target, tgt, sizeof(tgt)) || memcmp(tgt, EXPECT_TARGET, sizeof(tgt)) != 0) {
        logf("feed icon guard: the branch target %016llX does not start with the expected "
             "MOV RAX,[R12] — refusing. Nothing was written.", (unsigned long long)target);
        return false;
    }

    memcpy(g_iconOrig, cur, ICONPATH_LEN);

    // The new JZ starts 8 bytes earlier in the instruction stream, so it must jump 8 bytes further.
    const int32_t relNew = rel + 8;
    uint8_t patch[ICONPATH_LEN] = {
        0x48,0x85,0xFF,                  // TEST RDI,RDI      <- the check, now FIRST
        0x0F,0x84,0x00,0x00,0x00,0x00,   // JZ   <target>
        0x48,0x8B,0x58,0x08,             // MOV  RBX,[RAX+0x8] <- the load, now second
        0x48,0x89,0x5D,0x30              // MOV  [RBP+0x30],RBX
    };
    memcpy(patch + 5, &relNew, sizeof(relNew));

    if (!writeCode(addr, patch, ICONPATH_LEN)) return false;

    g_iconPatched = true;
    logf(">>> #69 FEED ICON GUARD: ARMED. RVA %08llX, 17 bytes reordered so the IconPath getter "
         "null-checks its source item BEFORE dereferencing the property bag. Branch target "
         "%016llX unchanged and verified. An entry with no item now renders the generic icon, "
         "which is what the engine's own null path does.",
         (unsigned long long)RVA_ICONPATH_HOIST, (unsigned long long)target);
    return true;
}

void unpatchFeedIconGuard()
{
    if (!g_iconPatched) return;
    if (writeCode(g_base + RVA_ICONPATH_HOIST, g_iconOrig, ICONPATH_LEN))
        logf("feed icon guard: restored — the vanilla instruction order is back, crash included.");
    g_iconPatched = false;
}

bool feedIconGuardArmed() { return g_iconPatched; }

bool setFeedIconGuard(bool on, const char** why)
{
    if (on) {
        if (g_iconPatched) return true;
        if (!patchFeedIconGuard()) {
            if (why) *why = "the 17-byte signature or its branch target did not verify — the getter "
                            "is not the one this build knows";
            return false;
        }
        return true;
    }
    unpatchFeedIconGuard();
    return true;
}

// ============================================================================================
// #105 — the same defect one getter over: `EventType` (FUN_142F5B4F0, registered at 0x14028B7AC)
//
//     142F5B500  CALL 0x14038BF30                 ; bag = get_property_bag_TIME_LINE_GROUP(tag)
//     142F5B505  MOV  R8,[RBX]                    ; the result sink's vtable
//     142F5B508  MOV  RCX,RBX
//     142F5B50B  MOV  RDX,[RAX+0x8]               ; ★ FAULT: no bag, no null check
//     142F5B50F  ADD  RSP,0x20 / POP RBX / JMP [R8+0x70]
//
// A 3rd-seat player's dump from 2026-10-14 (190 Expanded mod set, issue #105) faults here reading
// 0x8: an event-feed entry whose tag has no property bag — the "empty landmine event" that crashes
// when touched. Its sibling getter at 0x142F5B4D0 shows the engine's own idiom for "nothing to
// report": return without setting the sink. So the tail becomes exactly that, in place:
//
//     MOV RCX,RBX / LEA RSP,[RSP+0x20] / POP RBX     ; LEA, not ADD: the flags are needed next
//     TEST RAX,RAX / JZ ret
//     MOV R8,[RCX] / MOV RDX,[RAX+0x8] / JMP [R8+0x70]
//   ret:
//     RET
//
// 26 bytes over the original 19 and 7 of the 8 INT3 pad bytes before the next function at
// 0x142F5B520; the remaining byte stays INT3. Non-null path: same registers, same tail call.
// ⚠ Again an airbag. Why such an entry exists is not answered by this.
// ============================================================================================
static constexpr uintptr_t RVA_EVENTTYPE_TAIL = 0x02F5B505;
static constexpr size_t    EVENTTYPE_LEN      = 27;
static const uint8_t EXPECT_EVENTTYPE[EVENTTYPE_LEN] = {
    0x4C,0x8B,0x03,                  // MOV  R8,[RBX]
    0x48,0x8B,0xCB,                  // MOV  RCX,RBX
    0x48,0x8B,0x50,0x08,             // MOV  RDX,[RAX+0x8]
    0x48,0x83,0xC4,0x20,             // ADD  RSP,0x20
    0x5B,                            // POP  RBX
    0x49,0xFF,0x60,0x70,             // JMP  [R8+0x70]
    0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC   // pad up to 0x142F5B520
};
static const uint8_t PATCH_EVENTTYPE[EVENTTYPE_LEN] = {
    0x48,0x8B,0xCB,                  // MOV  RCX,RBX
    0x48,0x8D,0x64,0x24,0x20,        // LEA  RSP,[RSP+0x20]
    0x5B,                            // POP  RBX
    0x48,0x85,0xC0,                  // TEST RAX,RAX
    0x74,0x0B,                       // JZ   ret
    0x4C,0x8B,0x01,                  // MOV  R8,[RCX]
    0x48,0x8B,0x50,0x08,             // MOV  RDX,[RAX+0x8]
    0x49,0xFF,0x60,0x70,             // JMP  [R8+0x70]
    0xC3,                            // ret: RET
    0xCC
};
// ============================================================================================
// #105, round two: the same unchecked bag read in two more event-feed getters. The bag accessor
// FUN_14038BF30 is only `MOV RAX,[RCX+0x28] / RET`, so "no bag" is a message whose +0x28 is null.
// A scan of every call to it in 0x142F40000-0x142FA0000 found five unchecked reads: IconPath
// (#69), EventType (above), these two, and 0x142F783B0, which FUN_142F77F20 only reaches after
// null-checking the bag of the same message, so it needs nothing.
//
// (1) FUN_142F5B470 — 2026-10-28 dump (exe+0x2F5B493, read of 0x50), same 3rd-seat player:
//
//     142F5B48B  CALL 0x14038BF30 / MOV RCX,RDI / MOV RDX,[RAX+0x50]   ; ★ FAULT
//     ...        MOV RAX,RBX / MOV RBX,[RSP+0x30] / ADD RSP,0x20 / POP RDI / JMP RAX
//
//   Same idiom as EventType, in place over the 23-byte tail and all 9 pad bytes to 0x142F5B4B0:
//
//     MOV RCX,RDI / MOV RDX,RAX / MOV RAX,RBX / MOV RBX,[RSP+0x30] / ADD RSP,0x20 / POP RDI
//     TEST RDX,RDX / JZ ret / MOV RDX,[RDX+0x50] / JMP RAX
//   ret:
//     RET
//
// (2) FUN_142F6DCC0 — not yet seen crashing. It already has a default: when an earlier check
//   fails it keeps RBX = 0x1443D5BC8 (a static it initialised above) and skips the bag. Its
//   16-byte bag run cannot fit a null test in place, so the CALL at 0x142F6DD2B is pointed at a
//   stub that returns the real bag, or a fake one whose +0x48 holds that same default — the
//   following MOV RBX,[RAX+0x48] then loads exactly what the default path would have kept.
// ============================================================================================
static constexpr uintptr_t RVA_SITE50_TAIL = 0x02F5B490;
static constexpr size_t    SITE50_LEN      = 32;
static const uint8_t EXPECT_SITE50[SITE50_LEN] = {
    0x48,0x8B,0xCF,                  // MOV  RCX,RDI
    0x48,0x8B,0x50,0x50,             // MOV  RDX,[RAX+0x50]
    0x48,0x8B,0xC3,                  // MOV  RAX,RBX
    0x48,0x8B,0x5C,0x24,0x30,        // MOV  RBX,[RSP+0x30]
    0x48,0x83,0xC4,0x20,             // ADD  RSP,0x20
    0x5F,                            // POP  RDI
    0x48,0xFF,0xE0,                  // JMP  RAX
    0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC   // pad up to 0x142F5B4B0
};
static const uint8_t PATCH_SITE50[SITE50_LEN] = {
    0x48,0x8B,0xCF,                  // MOV  RCX,RDI
    0x48,0x8B,0xD0,                  // MOV  RDX,RAX       ; the bag
    0x48,0x8B,0xC3,                  // MOV  RAX,RBX
    0x48,0x8B,0x5C,0x24,0x30,        // MOV  RBX,[RSP+0x30]
    0x48,0x83,0xC4,0x20,             // ADD  RSP,0x20
    0x5F,                            // POP  RDI
    0x48,0x85,0xD2,                  // TEST RDX,RDX
    0x74,0x07,                       // JZ   ret
    0x48,0x8B,0x52,0x50,             // MOV  RDX,[RDX+0x50]
    0x48,0xFF,0xE0,                  // JMP  RAX
    0xC3                             // ret: RET
};

static constexpr uintptr_t RVA_SITE48_CALL   = 0x02F6DD2B;
static constexpr uintptr_t RVA_BAG_ACCESSOR  = 0x0038BF30;
static constexpr uintptr_t RVA_SITE48_DEFAULT = 0x043D5BC8;   // LEA RBX,[RIP+..] at 0x142F6DCDA
static const uint8_t EXPECT_SITE48_NEXT[] = { 0x48,0x8B,0x58,0x48 };   // MOV RBX,[RAX+0x48]

static bool g_eventTypePatched = false, g_site50Patched = false, g_site48Patched = false;
static uint8_t  g_site48Orig[5] = { 0 };
static uint8_t* g_site48Page = nullptr;

static bool patchInPlace(uintptr_t rva, const uint8_t* expect, const uint8_t* patch, size_t len)
{
    uint8_t cur[64] = { 0 };
    if (len > sizeof(cur) || !safeRead((void*)(g_base + rva), cur, len) || memcmp(cur, expect, len) != 0)
        return false;
    return writeCode(g_base + rva, patch, len);
}

static bool patchSite48()
{
    const uintptr_t call = g_base + RVA_SITE48_CALL;
    uint8_t cur[9] = { 0 };
    if (!safeRead((void*)call, cur, sizeof(cur)) || cur[0] != 0xE8 ||
        memcmp(cur + 5, EXPECT_SITE48_NEXT, sizeof(EXPECT_SITE48_NEXT)) != 0)
        return false;
    int32_t rel = 0; memcpy(&rel, cur + 1, 4);
    if (call + 5 + (intptr_t)rel != g_base + RVA_BAG_ACCESSOR) return false;
    uint8_t lea[3] = { 0 };   // the default's LEA RBX,[RIP+disp] must still be where it was read
    int32_t disp = 0;
    if (!safeRead((void*)(g_base + 0x02F6DCDA), lea, 3) || lea[0] != 0x48 || lea[1] != 0x8D ||
        lea[2] != 0x1D || !safeRead((void*)(g_base + 0x02F6DCDD), &disp, 4) ||
        g_base + 0x02F6DCE1 + (intptr_t)disp != g_base + RVA_SITE48_DEFAULT)
        return false;
    if (!g_site48Page) g_site48Page = (uint8_t*)allocNear(call);
    if (!g_site48Page) return false;
    // page+0x40: MOV RAX,[RCX+0x28] / TEST RAX,RAX / JZ +1 / RET / LEA RAX,[RIP+fake] / RET
    // page+0x80: the fake bag; only its +0x48 is ever read, and it holds the getter's own default.
    uint8_t* stub = g_site48Page + 0x40;
    uint8_t* fake = g_site48Page + 0x80;
    const uint8_t head[] = { 0x48,0x8B,0x41,0x28, 0x48,0x85,0xC0, 0x74,0x01, 0xC3, 0x48,0x8D,0x05 };
    memcpy(stub, head, sizeof(head));
    const int32_t toFake = (int32_t)((intptr_t)fake - (intptr_t)(stub + sizeof(head) + 4));
    memcpy(stub + sizeof(head), &toFake, 4);
    stub[sizeof(head) + 4] = 0xC3;
    memset(fake, 0, 0x50);
    const uintptr_t dflt = g_base + RVA_SITE48_DEFAULT;
    memcpy(fake + 0x48, &dflt, 8);
    FlushInstructionCache(GetCurrentProcess(), stub, 0x40);
    const int64_t d = (int64_t)(uintptr_t)stub - (int64_t)(call + 5);
    if (d < INT32_MIN || d > INT32_MAX) return false;
    uint8_t patch[5] = { 0xE8 }; const int32_t d32 = (int32_t)d; memcpy(patch + 1, &d32, 4);
    memcpy(g_site48Orig, cur, 5);
    return writeCode(call, patch, 5);
}

bool patchEventTypeGuard()
{
    if (!g_eventTypePatched) {
        if (patchInPlace(RVA_EVENTTYPE_TAIL, EXPECT_EVENTTYPE, PATCH_EVENTTYPE, EVENTTYPE_LEN))
            g_eventTypePatched = true;
        else
            logf("event type guard: signature mismatch or unreadable — NOT patched");
    }
    if (!g_site50Patched) {
        if (patchInPlace(RVA_SITE50_TAIL, EXPECT_SITE50, PATCH_SITE50, SITE50_LEN))
            g_site50Patched = true;
        else
            logf("event-feed bag guard +0x50: signature mismatch or unreadable — NOT patched");
    }
    if (!g_site48Patched) {
        if (patchSite48()) g_site48Patched = true;
        else logf("event-feed bag guard +0x48: signature mismatch or unreadable — NOT patched");
    }
    if (g_eventTypePatched && g_site50Patched && g_site48Patched)
        logf(">>> #105 EVENT-FEED BAG GUARDS: ARMED (EventType, +0x50, +0x48). An event-feed entry "
             "with no property bag reports nothing instead of crashing this client.");
    return g_eventTypePatched && g_site50Patched && g_site48Patched;
}

void unpatchEventTypeGuard()
{
    if (g_eventTypePatched && writeCode(g_base + RVA_EVENTTYPE_TAIL, EXPECT_EVENTTYPE, EVENTTYPE_LEN))
        g_eventTypePatched = false;
    if (g_site50Patched && writeCode(g_base + RVA_SITE50_TAIL, EXPECT_SITE50, SITE50_LEN))
        g_site50Patched = false;
    // The stub page stays mapped: a getter already inside the redirected CALL may return into it.
    if (g_site48Patched && writeCode(g_base + RVA_SITE48_CALL, g_site48Orig, 5))
        g_site48Patched = false;
}

#ifndef TW3K_RELEASE
void reportFeedIconGuard()
{
    logf("---- #69 FEED ICON GUARD (the IconPath crash at exe+0x2F674AC) ----");
    logf("  %s", g_iconPatched
         ? "ARMED — the null check happens before the deref, so an empty feed renders the generic "
           "icon instead of taking the client down."
         : "DISARMED — vanilla order. An event-feed entry with a null source item AND a null "
           "property bag crashes this client.");
    logf("  ⚠ It removes the CRASH, not the cause. Something is still presenting an entry while the "
         "feed is empty — `blend` and our own +0x1F8 cursor borrow are both open suspects, and this "
         "changes neither. #69 stays open on the cause.");
}
#endif
