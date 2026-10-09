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
