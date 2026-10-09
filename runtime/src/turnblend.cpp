// turnblend.cpp - Let a player keep managing their faction while somebody ELSE has the turn.
//
// Shogun 2 shaped partial simultaneity. NOT simultaneous turns: END TURN, movement and attacks stay
// exactly as they are. What this opens is the MANAGEMENT layer - buildings, character skill points,
// army stance - during another human player's turn.
//
// ARMS ITSELF once its sites decrypt (see the auto-arm section below). `blend off` reverts this
// machine to stock and stays off. Per machine: an un-armed client is byte-for-byte vanilla.
//
// ── THE GATE ──────────────────────────────────────────────────────────────────────────────────────
//
// `FUN_142F6BFA0(campaignRoot)` is a nullary "is it my turn", and it is two field reads:
//
//     currentFactionId = *(int *)( root->0x2188 ->0x78 ->0x3B68 ->0x48 + 8 );
//     localFactionId   = *(int *)( root->0x2188 ->0x1A8            + 8 );
//     return localFactionId == currentFactionId;
//
// No phase test, no movements-in-flight test - none of `FUN_1419B5CA0`'s nine clauses. It is the
// whole reason the build button is grey during another player's turn.
//
// 22 functions call it. 13 of them are registered CCO properties, and they are exactly the
// management layer (§6rrr, and `wiki/reference/generate/Find-CallersOf.py 142f636f0 3` reproduces
// the list):
//
//     CcoBuildingLevelRecord          ConstructInCurrentSlot, DismantleInCurrentSlot,
//                                     IssueCommandForBuildingBrowser, CanBuildInCurrentSlot,
//                                     IsActiveForBuildingBrowser,
//                                     ShouldShowConstructionDetailsInBuildingBrowser
//     CcoCampaignRegionSlot           Demolish, Dismantle, Repair
//     CcoCampaignCharacterSkill       AddPoint, RemovePoint, CanUpgrade
//     CcoCampaignMilitaryForceStance  CanBeActivated
//
// ★ Both halves are behind the same function - the queries that GREY the button and the action that
// does the work - so one change ungreys it AND makes it fire. `ConstructInCurrentSlot`
// (`FUN_14313B200`) reads, in full: `if (root != 0 && FUN_142F6BFA0()) { build the command; submit }`.
//
// ── WHY IT IS SAFE TO ACT OUT OF TURN AT ALL ─────────────────────────────────────────────────────
//
// The selection rule for this whole workstream, and it is checkable rather than argued: ONLY unlock
// an action class that submits a `CCQ_` command. Every action above does -
// `CCQ_REGION_BUILDING_CONSTRUCT` / `_DISMANTLE` / `_REPAIR`, the `CCQ_MILITARY_FORCE_BUILDING_*`
// family, `CCQ_ADD_CAMPAIGN_SKILL_POINT`, `CCQ_REMOVE_CAMPAIGN_SKILL_POINT`,
// `CCQ_CHANGE_MILITARY_FORCE_STANCE`.
//
// 3K campaign MP is a checksum-compared lockstep simulation and the command queue IS the order
// arbiter, so two players acting at once produce two entries in one ordered stream rather than a
// race. An action that mutated state locally WITHOUT a command would be a desync by construction -
// which is why the rule is about commands and not about how safe an action feels.
//
// ★ And the simulation half is already unguarded: of `FUN_1419B5CA0`'s eight callers, the only
// registered command handler is `CCQ_END_TURN`'s. None of the handlers above consults the turn.
//
// ── WHAT WE PATCH IT TO, AND WHY NOT SIMPLY `return 1` ───────────────────────────────────────────
//
// ★★★ There are THREE turn states, not two, and the engine branches on them itself. From the
// campaign tick `FUN_1419EEF90`, at the phase-2 transition:
//
//     if (currentFaction->0xCD0 == 0)   FUN_14199BBB0(model);        // AI  -> advance immediately
//     else                              container->0x8C = 3;         // human -> phase 3, and WAIT
//
//     | | another human is acting | the AI sweep |
//     | simulation | PARKED, waiting on a human   | ACTIVELY ADVANCING, faction by faction |
//     | the HUD    | present, some buttons greyed | removed wholesale |
//
// Issuing commands during the sweep means mutating state while the model is walking the faction
// array and resolving turns. `return 1` would open that window too. So instead the gate is made to
// answer "is the current faction HUMAN":
//
//     | state                  | original | patched | effect            |
//     | my turn                | true     | true    | none              |
//     | another human's turn   | false    | TRUE    | ★ the unlock      |
//     | the AI sweep           | false    | false   | none - vanilla    |
//
// ⇒ the dangerous window is excluded BY CONSTRUCTION, not by remembering not to arm the patch there.
//
// ★★★ AND WE DO NOT HAVE TO WRITE THAT PREDICATE - THE ENGINE ALREADY HAS IT, 32 BYTES EARLIER.
// `FUN_142F6BF80` is, in full:
//
//     142f636d0: MOV RAX, [RCX + 0x2188]        <- same argument, same register
//     142f636d7: MOV RCX, [RAX + 0x78]
//     142f636db: MOV RAX, [RCX + 0x3b68]
//     142f636e2: MOV RCX, [RAX + 0x48]          <- the current faction
//     142f636e6: MOVZX EAX, byte [RCX + 0xcd0]  <- its human flag
//     142f636ed: RET
//
// Identical signature (campaign root in RCX, bool in AL), leaf, no stack frame. So the patch is a
// TAIL JUMP from one engine predicate to its neighbour. We are not inventing behaviour; we are
// making one CA function answer another CA function's question. `faction+0xCD0` is the human flag
// this project has used since session 4b and confirmed live across 272 factions.
//
// ── METHOD: THE CALL SITES, NOT THE FUNCTION ─────────────────────────────────────────────────────
//
// ⚠ The first build patched `FUN_142F6BFA0` itself with a tail jump to the sibling. That worked in
// game — but it gives all 22 callers the same answer, and one of them turned out to need the real
// one back (the diplomacy button; see `kGateSites`). **The gate function is now left VANILLA** and
// each `CALL` is retargeted instead, four bytes of displacement at a time. See the section on
// `kGateSites` below for why that is strictly safer.
//
// Either way there is no trampoline, no stolen prologue, no `FF 25` absolute jump, and no analysis
// of which registers a body still expects — the machinery that has cost runs on this binary.
//
// ⇒ It can be armed and DISARMED LIVE, mid-session. That is what makes an A/B possible: sit in
// another player's turn, toggle, and watch the build button change state.
//
// Signatures are verified at ARM time, and these sites are deliberately NOT in kSites: a mismatch
// here must disable this feature only, never block the nine working hooks.
//
// ── ✅ CONFIRMED LIVE (2026-08-08, test-host) ────────────────────────────────────────────────────────
//
// `blend` read VANILLA, `blend on` armed it, and tester could then CONSTRUCT BUILDINGS during another
// player's turn. So the gate identification, the tail-jump, the arm path and the UI effect are all
// real, on the running process, first attempt.
//
// ⚠ WHAT THAT DOES **NOT** YET ESTABLISH, and the distinction is the one this file registered in
// advance rather than after the fact:
//
//   1. that the COMMAND LANDED. A button that responds is not a command in the queue. The proof is
//      `CCQ_REGION_BUILDING_CONSTRUCT` in the `commands` delta - mark, click, mark.
//   2. that the OTHER CLIENT AGREES. Lockstep means every peer executes the same queue; if the
//      building exists on one screen and not the other, that is a desync however good it looks.
//   3. that the SESSION SURVIVES the following turn.
//
// ── HONEST LIMITS ────────────────────────────────────────────────────────────────────────────────
// ⚠ 12 of the 22 callers are NOT registered properties - internal helpers, some of which may have
//   non-UI consumers. This changes those too, though now only during human turns. A per-property
//   swap would not, at the cost of reimplementing each action.
// ⚠ `cco_raw.tsv` only covers the FIRST CCO registration mechanism, so the list of 13 is a floor,
//   not a ceiling. Properties registered by the second mechanism cannot appear in it.
// ⚠ The state this opens only exists with TWO HUMANS, so single player cannot test it - during the
//   AI sweep the HUD is not on screen to be clicked.
//
// ── WHAT TO EXPECT WHEN IT IS ARMED ──────────────────────────────────────────────────────────────
//
//   button becomes clickable AND `CCQ_REGION_BUILDING_CONSTRUCT` appears in the `commands` delta
//        -> as designed. Mark with `commands`, click, `commands` again.
//   button ungreys but NO command lands
//        -> the action carries a check the query does not. A FINDING, not a failure.
//   button does not ungrey at all
//        -> `FUN_142F6BFA0` is not the whole gate; the 12 unregistered callers become the search space.

#include "tw3k.h"

// The gate, and the engine's own is-current-faction-human predicate that we point it at.
static constexpr uintptr_t RVA_TURN_GATE     = 0x02F6BFA0;   // FUN_142F6BFA0
static constexpr uintptr_t RVA_IS_HUMAN_TURN = 0x02F6BF80;   // FUN_142F6BF80
static constexpr size_t    BLEND_WORD_LEN    = 8;

// Original first 8 bytes: MOV RDX,[RCX+0x2188] (7 bytes) then the first byte of MOV RAX,[RDX+0x78].
static const uint8_t EXPECT_TURN_GATE[BLEND_WORD_LEN] = {
    0x48,0x8B,0x91, 0x88,0x21,0x00,0x00,   0x48
};

bool g_turnBlendOn = false;

// ══════════════════════════════════════════════════════════════════════════════════════════════════
// ★★★ PER-CALL-SITE, not the shared function — so one caller can be left alone
// ══════════════════════════════════════════════════════════════════════════════════════════════════
//
// The first build patched `FUN_142F6BFA0` itself, which worked and gave all 22 of its callers the
// same answer. Then tester asked for one of them back (2026-08-08): *"I think it would be good to
// disable the diplomacy button again during this phase"* — diplomacy opens but cannot start a
// negotiation, because the events that begin one are withheld elsewhere and are deliberately not in
// the refresh table.
//
// A shared-function patch cannot express that: once the predicate lies, it lies to everyone. So the
// mechanism moved down one level. Every caller reaches the gate through
//
//     E8 <rel32>          CALL FUN_142F6BFA0
//
// and **only the displacement is rewritten**, so that site calls `FUN_142F6BF80` — the engine's own
// "is the current faction human" — instead. 4 bytes per site, one instruction, same length, same
// registers, same ABI. Which callers get the relaxed answer becomes a list.
//
// ★ The gate function itself is left VANILLA, which is why this is strictly safer than what it
// replaces: an un-listed caller is not merely excluded by policy, it is untouched code.
//
// The table below was generated by scanning for every `E8` whose target is the gate, then naming the
// containing function from `cco_raw.tsv` or, where it is not a registered property, from the string
// literals inside it. All 22 were identified or bounded that way.

struct GateCallSite {
    uint32_t rva;          // the CALL instruction
    bool     relax;        // false = leave calling the real predicate
    const char* what;
};

static const GateCallSite kGateSites[] = {
    // ★★ REBASED TO 1.7.2 on 2026-09-20. Commit 49004b1 moved RVA_TURN_GATE / RVA_IS_HUMAN_TURN to
    // 1.7.2 and left THIS TABLE on 1.7.1 addresses. Measured against both shipped binaries:
    //     1.7.1 → 22/22 of the old RVAs are a valid E8 reaching the old gate (they were correct)
    //     1.7.2 →  0/22, and 21 of them do not even begin with an E8 opcode
    // So looksLikeGateCall() refused every site, and auto-arm spent 300 s waiting for a decrypt
    // that can no longer happen — Denuvo is gone, the bytes are final at load — before giving up.
    // That is the "!! TURN BLENDING: gave up auto-arming" line on all three machines that day.
    //
    // ★ WHY THIS LIST IS COMPLETE, not a selection: in BOTH builds the table is exactly "every
    // caller of the stock turn gate". 1.7.1 has 22 callers of TURN_GATE and 5 of IS_HUMAN_TURN,
    // and the old table was precisely those 22; 1.7.2 has 22 and 5 likewise. Derived by scanning
    // .text for every E8 rel32 reaching FUN_142F6BFA0; owners taken from .pdata, which 1.7.2 has
    // and 1.7.1 did not. All 22 below re-pass looksLikeGateCall against the shipped exe.
    //
    // ★ HOW THE LABELS WERE CARRIED OVER: address order is preserved across the rebuild, so the
    // two sorted lists pair off index by index. That pairing is not assumed — it is TESTED, and
    // 14 of the 22 are independently confirmed, with zero contradictions:
    //     • 10 by body comparison (same call offset into the function, bodies differing only in
    //       their rip-relative displacements)
    //     • 4 more by distinctive string sets referenced from the function
    // The 8 marked ~ are positional only. A ~ label may be wrong ABOUT ITS NAME; it is still a
    // real gate call site, and its policy is true like 21 of the 22.
    //
    // ⚠ THE LABELS ARE DOCUMENTATION. Only the policy column changes behaviour, and the single
    // policy=false entry is confirmed twice over (see its comment below). So a wrong ~ name
    // cannot cause a wrong patch. To settle one, use `blend site <n> on|off` in game — flip it,
    // look at the screen, flip it back — which is what that command was built for.
    //
    // Old 1.7.1 addresses, in this same order, so the next rebase can re-derive rather than
    // re-discover: 02E32790 02E38E30 02E7A415 02E7FB83 02E8462A 02EA77A0 02EBC719 02ECCA5D
    //              02ED1636 02EE855F 02FFA553 02FFC2AD 03008F99 0300AE79 030424E9 031374B6
    //              0313B21D 0313ED90 03144F89 03145863 031459C2 03145D78
    { 0x02E393A0, true,  "CcoClickHoldContext" },
    { 0x02E42400, true,  "campaign hotkeys (escape_menu, quick_save, toggle_labels, …)" },
    { 0x02E82955, true,  "CcoCampaignCharacterSkill.AddPoint" },
    { 0x02E880C3, true,  "BlockingNotificationIcon (CCO)  — NOT the agent HUD; old label was wrong" },
    { 0x02E8CB6A, true,  "CcoCampaignCharacterSkill.CanUpgrade  (CCO: \"CanUpgrade\")" },

    // ★★★ THE BIG ONE, and it was filed as "unidentified" for a week. `FUN_142EAFCD0` (1.7.1:
    // `FUN_142EA7790`) is three lines — `if (root && FUN_142F6BFA0()) return 1;` — and it has NO
    // string literals because it does not name itself in code: it is REGISTERED (1.7.1 call site
    // `0x1402A6030`, NOT rebased) under the name stored at `0x14383FA38` — 1.7.1 `0x14383BFA0`,
    // which moved .sbss → .rdata like every other literal — and that name reads **`IsPlayersTurn`**.
    //
    // ⇒ it is not one button's gate. It is a GENERIC DATABINDING PROPERTY, and every UI element bound
    // to it flips together. That is why relaxing the set of 21 opened diplomacy, court, character
    // cards and assignments all at once, and why excluding the site below changed nothing.
    //
    // ⚠ It is absent from `cco_raw.tsv` — issue #28's second, un-enumerated registration mechanism,
    // showing up as a concrete cost rather than a footnote. The generator could not name it, so it
    // was relaxed blind. `blend sites` now exists so this one can be flipped ALONE, in game, and the
    // blast radius measured instead of guessed at.
    //
    // ★ Its 1.7.2 identification is CONCLUSIVE rather than positional: the two bodies are identical
    // instruction for instruction — sub rsp,28 / mov rcx,[rip+d] / test rcx,rcx / jz / call gate /
    // test al,al / ret — differing ONLY in their two rip-relative displacements, 39 bytes, call at
    // +0x10 in both builds. The entry it would be most expensive to get wrong is the most certain.
    { 0x02EAFCE0, true,  "CCO property IsPlayersTurn  ★ generic — most of the HUD hangs off this" },

    { 0x02EC4C59, true,  "CcoCampaignCharacterSkill.RemovePoint" },
    { 0x02ED4F9D, true,  "annexation / victory popups (campaign_victory_defeat, annexation_popup)" },
    { 0x02ED9B76, true,  "~ panel tab state (sets +0x9D/+0x9E)" },

    // ⚠ THE ONE DELIBERATE EXCLUSION, and ⚠⚠ IT IS NOT THE HUD DIPLOMACY BUTTON. It was labelled
    // that on 2026-08-08 from two strings in `FUN_142EF05C0` (1.7.1: `FUN_142EE7FE0`), and tester's next test disproved it: the
    // exclusion armed correctly (log: "21 call sites"), and the button behaved exactly as before.
    //
    // Decompiling it says why. The call sits in a block that first resolves a FACTION, then builds
    // `diplomacy_panel`, then guards on three things together — so this is the
    // **double-click-a-faction-to-open-diplomacy** path, which is where `double_click_diplomacy`
    // came from. The HUD button is bound to `IsPlayersTurn` above.
    //
    // It stays excluded: opening a negotiation out of turn still leads nowhere, because
    // `DiplomacyNegotiationStarted` and its siblings are deliberately kept out of the refresh table.
    // But excluding it is NOT what re-greys the button, and this comment used to claim it was.
    //
    // ★ Its 1.7.2 identification is DOUBLY confirmed, which matters because this is the only entry
    // whose policy differs: the same distinctive string pair {diplomacy_panel, double_click_diplomacy}
    // is referenced at this site in BOTH builds, and it sits at the same index in the two sorted
    // address lists AND at the same offset (+0x57F) into its containing function. Since exactly one
    // entry is policy=false and this is it, the policy column is correct even where a ~ name is not.
    { 0x02EF0B3F, false, "double-click-faction → diplomacy (NOT the HUD button; left vanilla)" },

    { 0x03000973, true,  "CcoCampaignMilitaryForceStance.CanBeActivated  (CCO: \"CanBeActivated\")" },
    { 0x030026CD, true,  "CanResearch (CCO)  — 1.7.1 label said \"movement-point test\"; registration disagrees" },
    { 0x0300F3B9, true,  "CcoCampaignRegionSlot.Demolish" },
    { 0x03011299, true,  "CcoCampaignRegionSlot.Dismantle" },
    { 0x03048909, true,  "CcoCampaignRegionSlot.Repair" },
    { 0x0313C3A6, true,  "region-slot helper (CanBuildInCurrentSlot)" },
    { 0x0314010D, true,  "CcoBuildingLevelRecord.ConstructInCurrentSlot" },
    { 0x03143C80, true,  "CcoBuildingLevelRecord.DismantleInCurrentSlot" },
    { 0x03149E79, true,  "~ building-browser helper (IsActiveForBuildingBrowser)" },
    { 0x0314A753, true,  "~ building-browser helper" },
    { 0x0314A8B2, true,  "IsRecruitmentPossible (CCO)" },
    { 0x0314AC68, true,  "CcoBuildingLevelRecord.IssueCommandForBuildingBrowser" },
};
static constexpr size_t kGateCount = sizeof(kGateSites) / sizeof(kGateSites[0]);

// Displacements are computed from the RVAs, so they are correct at any image base and cannot drift
// if a constant is edited.
static int32_t relTo(uint32_t siteRva, uintptr_t targetRva)
{
    return (int32_t)((intptr_t)targetRva - (intptr_t)siteRva - 5);
}

// Is this site an `E8` reaching one of the TWO predicates we know about? That is the build check —
// anything else means the bytes are not what this table was generated against.
//
// ★ Deliberately NOT "is it in the state I expect": per-site flipping (`blend site`) means a site can
// legitimately be in either state when a bulk arm or disarm arrives. Demanding an exact state made
// `blend off` — and with it the F1 panic and the F12 detach — FAIL WHOLESALE after a single site had
// been flipped by hand, leaving a machine patched with no way to unpatch it. A recovery path that
// stops working once you have used the diagnostic is worse than no diagnostic.
static bool gateSiteIsOurs(size_t i, int32_t* relOut = nullptr)
{
    uint8_t op = 0; int32_t rel = 0;
    const uintptr_t addr = g_base + kGateSites[i].rva;
    if (!safeRead((void*)addr, &op, 1) || !safeRead((void*)(addr + 1), &rel, 4)) return false;
    if (relOut) *relOut = rel;
    return op == 0xE8 && (rel == relTo(kGateSites[i].rva, RVA_TURN_GATE) ||
                          rel == relTo(kGateSites[i].rva, RVA_IS_HUMAN_TURN));
}

// Acquire all writable ranges before changing any byte. Ranges may share a page, so unwind
// protections in REVERSE acquisition order (later calls then restore RWX before the first
// call restores the original page protection). A failed acquisition leaves all bytes intact.
struct BlendWrite {
    uintptr_t address;
    size_t length;
    uint8_t bytes[7];
    DWORD protection;
};
static bool applyBlendWrites(BlendWrite* writes, size_t count, bool& committed)
{
    committed = false;
    size_t acquired = 0;
    for (; acquired < count; ++acquired) {
        auto& w = writes[acquired];
        if (!VirtualProtect((void*)w.address, w.length, PAGE_EXECUTE_READWRITE, &w.protection)) {
            logf("blend: could not make site %zu/%zu writable, err=%lu — no code bytes changed",
                 acquired + 1, count, GetLastError());
            break;
        }
    }
    bool ok = acquired == count;
    if (ok) {
        for (size_t i = 0; i < count; ++i) {
            auto& w = writes[i];
            memcpy((void*)w.address, w.bytes, w.length);
            if (!FlushInstructionCache(GetCurrentProcess(), (void*)w.address, w.length)) {
                logf("blend: instruction-cache flush FAILED at site %zu, err=%lu", i, GetLastError());
                ok = false;
            }
        }
        committed = true; // every requested byte was written, even if cache/protection failed
    }
    while (acquired) {
        auto& w = writes[--acquired];
        DWORD ignored = 0;
        if (!VirtualProtect((void*)w.address, w.length, w.protection, &ignored)) {
            logf("blend: protection restore FAILED at site %zu, err=%lu", acquired, GetLastError());
            ok = false;
        }
    }
    return ok;
}

// Verify every site we intend to touch, then write them. Never half.
bool setTurnBlend(bool on)
{
    for (size_t i = 0; i < kGateCount; ++i) {
        if (!kGateSites[i].relax) continue;
        int32_t rel = 0;
        if (!gateSiteIsOurs(i, &rel)) {
            logf("turn blend: SIGNATURE MISMATCH at %016llX (%s): rel=%d reaches neither "
                 "FUN_142F6BFA0 nor FUN_142F6BF80 — nothing written. All %zu sites are checked "
                 "before any is patched.",
                 (unsigned long long)(g_base + kGateSites[i].rva), kGateSites[i].what, rel,
                 kGateCount);
            return false;
        }
    }

    BlendWrite writes[kGateCount] = {};
    size_t done = 0;
    for (size_t i = 0; i < kGateCount; ++i) {
        if (!kGateSites[i].relax) continue;
        auto& w = writes[done++];
        w.address = g_base + kGateSites[i].rva + 1;
        w.length = 4;
        const int32_t rel = relTo(kGateSites[i].rva, on ? RVA_IS_HUMAN_TURN : RVA_TURN_GATE);
        memcpy(w.bytes, &rel, sizeof(rel));
    }
    bool committed = false;
    const bool ok = applyBlendWrites(writes, done, committed);
    if (committed) g_turnBlendOn = on;
    if (!ok) {
        logf(TW3K_MODE_TEXT("turn blend: FAILED (%s); inspect `blend sites` before continuing", "turn blend: FAILED (%s); inspect the local log before continuing"),
             committed ? "all bytes written but cache/protection incomplete" : "code unchanged");
        return false;
    }
    logf(TW3K_MODE_TEXT("turn blend: %s — %zu call sites now reach \"%s\". `blend sites` lists them individually; ", "turn blend: %s — %zu call sites now reach \"%s\". ")
         TW3K_MODE_TEXT("`blend site <n> off` puts one back without touching the rest.", ""),
         on ? "ARMED" : "restored", done,
         on ? "is the current faction HUMAN" : "is the current faction MINE");
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
// ★★ PER-SITE CONTROL — because "which of these 21 does X hang off?" is not a desk question
// ══════════════════════════════════════════════════════════════════════════════════════════════════
//
// tester asked for the diplomacy button back. The site that carried the string was excluded, the DLL
// armed correctly, and NOTHING CHANGED — because the button is bound to `IsPlayersTurn`, which is one
// of the other twenty. The cost of learning that was a build, a deploy and a live session.
//
// ⇒ the same question about any other button costs the same again, and there are twenty of them. So
// the table becomes individually addressable at runtime: flip one site, look at the screen, flip it
// back. A binary search over 21 sites is five flips in one sitting and no rebuild at all.
//
// ⚠ These are DIAGNOSTIC, not configuration. A subsequent bare `blend on`/`blend off` moves every
// `relax` site back to the table's policy, deliberately — the table is the shipped state and a
// half-remembered manual flip must not survive as one.

size_t gateSiteCount() { return kGateCount; }

// index is 1-based as printed by `blend sites`. relax = point this caller at the human predicate.
#ifndef TW3K_RELEASE
bool setGateSite(size_t index, bool relax, char* why, size_t whySz)
{
    if (index < 1 || index > kGateCount) {
        _snprintf_s(why, whySz, _TRUNCATE, "index %zu is out of range 1..%zu", index, kGateCount);
        return false;
    }
    const size_t i = index - 1;

    int32_t rel = 0;
    if (!gateSiteIsOurs(i, &rel)) {
        _snprintf_s(why, whySz, _TRUNCATE,
                    "site %zu (%s) does not read as a CALL to either predicate — refusing to write",
                    index, kGateSites[i].what);
        return false;
    }

    const int32_t want = relTo(kGateSites[i].rva, relax ? RVA_IS_HUMAN_TURN : RVA_TURN_GATE);
    if (rel == want) {
        _snprintf_s(why, whySz, _TRUNCATE, "site %zu (%s) is already %s — nothing written",
                    index, kGateSites[i].what, relax ? "relaxed" : "stock");
        return false;
    }

    const uintptr_t addr = g_base + kGateSites[i].rva + 1;   // the rel32 only
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        _snprintf_s(why, whySz, _TRUNCATE, "VirtualProtect failed at %016llX, err=%lu",
                    (unsigned long long)addr, GetLastError());
        return false;
    }
    memcpy((void*)addr, &want, 4);
    FlushInstructionCache(GetCurrentProcess(), (void*)(addr - 1), 5);
    DWORD tmp = 0;
    VirtualProtect((void*)addr, 4, oldProtect, &tmp);

    _snprintf_s(why, whySz, _TRUNCATE, "site %zu (%s) is now %s",
                index, kGateSites[i].what, relax ? "RELAXED (asks: is the current faction human)"
                                                 : "STOCK (asks: is the turn mine)");
    logf("turn blend: %s", why);
    return true;
}
#endif


// The list, with what each site ACTUALLY points at right now — not what the policy column says.
#ifndef TW3K_RELEASE
void reportGateSites()
{
    logf("=== TURN BLEND CALL SITES — `blend site <n> on|off` flips ONE ===");
    logf("  relaxed = this caller asks \"is the current faction HUMAN\" (true during another");
    logf("  player's turn). stock = it asks \"is the turn MINE\" (vanilla).");
    for (size_t i = 0; i < kGateCount; ++i) {
        int32_t rel = 0;
        const char* state = "?? UNRECOGNISED";
        if (gateSiteIsOurs(i, &rel))
            state = (rel == relTo(kGateSites[i].rva, RVA_IS_HUMAN_TURN)) ? "relaxed" : "stock  ";
        logf("  %2zu  %s  %08X  %s%s", i + 1, state, (unsigned)kGateSites[i].rva,
             kGateSites[i].what, kGateSites[i].relax ? "" : "   [policy: excluded]");
    }
    logf("  ★ To find what a button hangs off: flip one site, look at the screen, flip it back.");
    logf("  ⚠ A bare `blend on`/`blend off` afterwards restores the table's own policy to every site.");
}
#endif


// ══════════════════════════════════════════════════════════════════════════════════════════════════
// ★★★ THE REFRESH HALF — why the panel does not update until you close and reopen it
// ══════════════════════════════════════════════════════════════════════════════════════════════════
//
// tester, with the gate armed (2026-08-08): *"i could update items but the UI would not show it
// updated, only after closing and opening up again would it show the new item"* — court positions,
// character tools, and assignments: *"i can assign a general but the UI does not update yet, i close
// it, i open it, now it works"*.
//
// The cause was already mapped, before the symptom was reported. §6rrr.4's chokepoint scan turned up
// ~28 functions in `0x142FA…0x142FB` with ZERO callers — the signature of code reached through a
// table — each of which fires a named UI event, gated on the same turn test, INLINE:
//
//     if (localFaction.id == currentFaction.id) { ... fire "CharacterAssignmentChanged" ... }
//
// They inline the comparison, so the gate patch above does not touch them. ⇒ the model changes, the
// UI is never told, and reopening the panel forces a rebuild that reads current state. Extracting
// the event names out of those functions produced tester's symptom list one for one.
//
// ── THE TRICK, and it is why this is one uniform patch rather than 20 hand-cut ones ──────────────
//
// Each guard is the same eight instructions, but register allocation and branch SENSE both vary
// (some `JNZ skip`, some `JZ body`), so patching the branch would need per-site analysis:
//
//     MOV rCUR,[rX+0x48]        <- the current faction
//     MOV rLOC,[rY+0x1A8]       <- the local faction        ★ we rewrite THIS one
//     MOV eCUR,[rCUR+8]         <- current faction id
//     CMP [rLOC+8],eCUR         <- compare
//     Jcc                       <- sense varies
//
// Rewrite the second load as `MOV rLOC, rCUR` and the comparison becomes **current == current**,
// which is unconditionally true. ⇒ every site reads "it is my turn" whichever way its branch points,
// and no branch is touched at all. The replacement is 3 bytes into a 7-byte instruction, so it is
// padded with a 4-byte `NOP` and the instruction boundary is preserved exactly.
//
// The table below is GENERATED and every entry was decoded out of the executable — the original
// bytes, the two registers, and the encoding of the replacement. ⚠ They do NOT all want the same
// replacement: eighteen decode to `RCX <- RAX`, the two `CeoEquipped` sites to `R8 <- RAX`.
//
// ── WHY THIS CANNOT DESYNC ───────────────────────────────────────────────────────────────────────
//
// These emit UI events. They tell the presentation layer to re-read state the simulation has ALREADY
// changed — the assignment landed, the money left the bank, the building went up. Nothing here
// submits a command, mutates campaign state, or travels over the wire. Worst case is a panel
// refreshing at a moment it otherwise would not have.
//
// ⚠ That argument is exactly why DIPLOMACY IS EXCLUDED. `FUN_142FB1510`
// (`DiplomacyNegotiationStarted`, `DiplomacyQuickDealNegotiationStarted`), `FUN_142FB08F0` and
// `FUN_142FB1810` (`DiplomacyProposalAccepted` / `Rejected`) look like the same bug and are not: they
// gate ENTRY INTO a stateful, networked negotiation backed by CCQ_DIPLOMACY_BEGIN_NEGOTIATION. Two
// players negotiating at once is a design question, not a refresh. tester, 2026-08-08: *"if we can't
// have diplomacy at once i don't think it's a deal breaker"* — so it stays out.
//
// Also excluded, deliberately: `CampaignOverlayDisabled`, `multiplayer_time_up`,
// `CampaignAreaOfInterestMouseOver`, `CampaignTurnEnd`/`CampaignHUDHiderPanelCallback`, the 4KB
// `PanelCallback` dispatcher — none is a stale-panel symptom, and each does more than notify.
// `CharacterMoveEnd` is excluded too: movement is Tier D, and during another player's turn it would
// fire constantly.
//
// ⚠ THE WRITE IS NOT ATOMIC. 7 bytes at an unaligned address cannot be one store, unlike the gate.
// This is the same exposure the nine detours already carry (14 bytes each, likewise non-atomic), and
// these are event emitters called on discrete state changes rather than per frame, so the window is
// small — but it is not zero. ⇒ EVERY SITE IS VERIFIED BEFORE ANY IS WRITTEN, so a build mismatch
// can never leave the table half-applied.

struct RefreshSite {
    uint32_t rva;
    uint8_t  orig[7];
    uint8_t  patched[7];
    const char* event;
};

// GENERATED — do not hand-edit. Every `orig` was read out of Three_Kingdoms.exe and every `patched`
// is `MOV <local>,<current>` + a 4-byte NOP, encoded from the registers decoded at that site.
//
// ★ The last two came from a LATER and STRICTLY BETTER scan — `Find-UiEventEmitters.py`. The first
// eighteen were found by sweeping one region, 0x142FA0000–0x142FB0000, chosen because §6rrr.4's
// zero-caller scan happened to land there; it was never a proven boundary, and `CeoEquipped` sits
// 0xE7 bytes past the last of them. The new scan anchors on `CALL FUN_1406A86A0` — the intern
// function EVERY named event goes through — so it enumerates the whole binary: 3,592 named events,
// 46 of them turn-guarded. There is no window left to be outside of.
static const RefreshSite kRefreshSites[] = {
    // ★★ REBASED TO 1.7.2 on 2026-09-20, in the same pass that fixed kGateSites above — and this
    // table is WHY the first fix did not work. blendSitesReadyAndVanilla() checks three things:
    // the gate's own prologue, the 22 gate call sites, AND these 20. Fixing only the first two
    // left the arm still refusing, and the log line is identical either way, which is what made
    // it look like the gate fix had failed when it had not.
    //
    // Measured: 20/20 of the 1.7.1 addresses carry their recorded 7-byte pattern in 1.7.1 and
    // 0/20 do in 1.7.2. The pattern populations are IDENTICAL across builds (A 28/28, B 14/14,
    // C 5/5), so every site still exists and only moved.
    //
    // Each row was resolved three ways, and all three had to agree:
    //   1. the new address carries that row's own 7-byte pattern
    //   2. the row's event-name string is within +/-700 bytes of it
    //   3. the old->new delta is one of exactly two piecewise shifts, +0x88B0 or +0x8950
    // ⚠ Note there are TWO shifts, not one: CharacterApReplenished, GovernmentPoliticsChanged and
    // Character::AttributeChanged moved +0x88B0 and the other seventeen +0x8950. A single delta
    // applied to the whole table would have put those three on wrong-but-plausible addresses.
    //
    // PooledResourceTier/Transaction passed (1) and (3) but not (2) — and it also fails (2) in
    // 1.7.1, where the answer is known, so that is this label not being a literal event string
    // rather than a bad address. It was confirmed separately in Ghidra: 0x142FB3DC4 disassembles
    // to MOV RCX,[R8+0x1A8] inside the exact current-faction-vs-local-faction CMP/JNZ guard this
    // table exists to neutralise.
    //
    // 1.7.1 addresses, in this row order, for the next rebase:
    //   02FA58DA 02FAA0AA 02FAA3BA 02FAA42A 02FAA49A 02FAB2CA 02FAB33A 02FAB3AF 02FAB60F 02FAF96F
    //   02FA4404 02FAB474 02FA4569 02FA7639 02FA86F1 02FA8531 02FAC421 02FA8E81 02FAFA56 02FAFB56

    { 0x02FAE18A, {0x48,0x8B,0x8A,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "CharacterApReplenished" },
    { 0x02FB29FA, {0x48,0x8B,0x8A,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "FamilyTreeUpdated" },
    { 0x02FB2D0A, {0x48,0x8B,0x8A,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "RetinueContainerOrderChanged" },
    { 0x02FB2D7A, {0x48,0x8B,0x8A,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "RetinueAddedToForce" },
    { 0x02FB2DEA, {0x48,0x8B,0x8A,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "RetinueSlotChanged" },
    { 0x02FB3C1A, {0x48,0x8B,0x8A,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "RetinueSlotChanged (2)" },
    { 0x02FB3C8A, {0x48,0x8B,0x8A,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "RetinueSlotRecruitmentComplete" },
    { 0x02FB3CFF, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "PooledResourceEffectChanged" },
    { 0x02FB3F5F, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "PooledResourceValueChanged" },
    { 0x02FB82BF, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "CeoAdded/Removed/NodeChanged" },
    { 0x02FACCB4, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "GovernmentPoliticsChanged" },
    { 0x02FB3DC4, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "PooledResourceTier/Transaction" },
    { 0x02FACE19, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "Character::AttributeChanged" },
    { 0x02FAFF89, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "CharacterRecruitmentPoolChanged" },
    { 0x02FB1041, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "CharacterAssignmentStateChanged" },
    { 0x02FB0E81, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "CharacterAssignmentChanged" },
    { 0x02FB4D71, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "FactionSupportUpdated" },
    { 0x02FB17D1, {0x49,0x8B,0x88,0xA8,0x01,0x00,0x00}, {0x48,0x8B,0xC8,0x0F,0x1F,0x40,0x00}, "GovernmentPostChanged/CharacterRankUp" },

    // ★★ EQUIPPING — tester, 2026-08-08: *"character screens still don't have their UI updating for
    // selecting new weapons and whatnot"*. Two emitters, byte-identical, each firing `CeoEquipped`
    // or `CeoUnequipped` off an internal branch, so one guard per site covers both directions.
    //
    // ⚠ NOTE THE DIFFERENT REGISTER PAIR: `MOV R8,[R9+0x1A8]` with the current faction in RAX, so the
    // replacement is `MOV R8,RAX` (4C 8B C0), not the `MOV RCX,RAX` all eighteen above wanted.
    // Hand-copying a row would have got that wrong; the scanner decodes the ModRM.
    //
    // ★ Checked, because "the comparison is now trivially true" is only half the requirement: the
    // body past the branch reads RDX, RDI, RCX, RBX and RAX and NEVER READS R8 — which is also
    // forced, since R8 is volatile and the first thing the body does is CALL. So handing R8 the
    // current faction instead of the local one cannot mislead anything downstream.
    { 0x02FB83A6, {0x4D,0x8B,0x81,0xA8,0x01,0x00,0x00}, {0x4C,0x8B,0xC0,0x0F,0x1F,0x40,0x00}, "CeoEquipped / CeoUnequipped" },
    { 0x02FB84A6, {0x4D,0x8B,0x81,0xA8,0x01,0x00,0x00}, {0x4C,0x8B,0xC0,0x0F,0x1F,0x40,0x00}, "CeoEquipped / CeoUnequipped (2)" },
};
static constexpr size_t kRefreshCount = sizeof(kRefreshSites) / sizeof(kRefreshSites[0]);
static constexpr size_t REFRESH_LEN   = 7;

bool g_refreshBlendOn = false;

// Verify EVERY site, then write every site. Never half of them: a partially applied table is a
// machine in a state no log explains.
bool setRefreshBlend(bool on)
{
    for (size_t i = 0; i < kRefreshCount; ++i) {
        uint8_t cur[REFRESH_LEN] = { 0 };
        const uintptr_t addr = g_base + kRefreshSites[i].rva;
        if (!safeRead((void*)addr, cur, REFRESH_LEN)) {
            logf("refresh blend: cannot read %016llX (%s) — nothing written",
                 (unsigned long long)addr, kRefreshSites[i].event);
            return false;
        }
        if (memcmp(cur, kRefreshSites[i].orig, REFRESH_LEN) != 0 &&
            memcmp(cur, kRefreshSites[i].patched, REFRESH_LEN) != 0) {
            logf("refresh blend: SIGNATURE MISMATCH at %016llX (%s) — nothing written. All %zu sites "
                 "are checked before any is patched, so the table cannot be half-applied.",
                 (unsigned long long)addr, kRefreshSites[i].event, kRefreshCount);
            return false;
        }
    }

    BlendWrite writes[kRefreshCount] = {};
    for (size_t i = 0; i < kRefreshCount; ++i) {
        writes[i].address = g_base + kRefreshSites[i].rva;
        writes[i].length = REFRESH_LEN;
        memcpy(writes[i].bytes, on ? kRefreshSites[i].patched : kRefreshSites[i].orig, REFRESH_LEN);
    }
    bool committed = false;
    const bool ok = applyBlendWrites(writes, kRefreshCount, committed);
    if (committed) g_refreshBlendOn = on;
    if (!ok) {
        logf(TW3K_MODE_TEXT("refresh blend: FAILED (%s); inspect `blend` before continuing", "refresh blend: FAILED (%s); inspect the local log before continuing"),
             committed ? "all bytes written but cache/protection incomplete" : "code unchanged");
        return false;
    }
    const size_t done = kRefreshCount;
    logf("refresh blend: %s — %zu of %zu UI event emitters now fire out of turn",
         on ? "ARMED" : "restored", done, kRefreshCount);
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════════════════════════
// ★★ AUTO-ARM — because this stopped being an experiment
// ══════════════════════════════════════════════════════════════════════════════════════════════════
//
// It was armed by hand while it was being proved. It is a FEATURE now (tester, 2026-08-08: *"for me
// this is already feature-complete"*), and this project's standing rule for a feature is that
// **nobody should have to press anything to play** — the same rule that made slot count, seating and
// the joining players' factions automatic, each after a session was lost to a keypress nobody made.
//
// ⚠ IT CANNOT SIMPLY ARM BESIDE THE HOOKS. Denuvo decrypts lazily and these sites live in the CCO
// region, which is not necessarily readable at the moment the nine hook sites are. So this follows
// `armWhenReady`'s discipline instead of assuming: look, and if the bytes are not there yet, look
// again. The pre-check is SILENT — a failed attempt every two seconds must not fill the log with
// mismatch lines that mean nothing more than "not yet".
//
// ★ Restarting the game is what makes this matter. The patch is process memory, so it survives a
// save/load inside one session (tester saw exactly that) and does NOT survive a restart — and a
// restart is precisely when nobody remembers to re-arm.
//
// Turning it off by hand cancels the auto-arm, so `blend off` stays off rather than being undone two
// seconds later by a helpful loop.

static bool  g_blendAutoWanted = true;      // until it succeeds, gives up, or is switched off by hand
static DWORD g_blendAutoStart  = 0;
static int   g_blendAutoLast   = -1;
static constexpr int kBlendAutoTimeoutSec = 300;

#ifndef TW3K_RELEASE
void cancelTurnBlendAutoArm() { g_blendAutoWanted = false; }
#endif


// Silent: is every site readable AND still vanilla? Used only to decide whether arming can be
// attempted, so it must not log — this runs every two seconds.
static bool blendSitesReadyAndVanilla()
{
    uint8_t g[BLEND_WORD_LEN] = { 0 };
    if (!safeRead((void*)(g_base + RVA_TURN_GATE), g, BLEND_WORD_LEN)) return false;
    if (memcmp(g, EXPECT_TURN_GATE, BLEND_WORD_LEN) != 0) return false;   // the gate stays vanilla
    for (size_t i = 0; i < kGateCount; ++i) {
        if (!kGateSites[i].relax) continue;
        uint8_t op = 0; int32_t rel = 0;
        const uintptr_t a = g_base + kGateSites[i].rva;
        if (!safeRead((void*)a, &op, 1) || !safeRead((void*)(a + 1), &rel, 4)) return false;
        if (op != 0xE8 || rel != relTo(kGateSites[i].rva, RVA_TURN_GATE)) return false;
    }
    for (size_t i = 0; i < kRefreshCount; ++i) {
        uint8_t r[REFRESH_LEN] = { 0 };
        if (!safeRead((void*)(g_base + kRefreshSites[i].rva), r, REFRESH_LEN)) return false;
        if (memcmp(r, kRefreshSites[i].orig, REFRESH_LEN) != 0) return false;
    }
    return true;
}

// Called once per main-loop tick (100 ms). Does nothing at all once settled.
void tickTurnBlendAutoArm(uint32_t tick)
{
    if (!g_blendAutoWanted) return;
    if (g_turnBlendOn && g_refreshBlendOn) { g_blendAutoWanted = false; return; }
    if (tick % 20 != 0) return;                       // every ~2 s

    if (g_blendAutoStart == 0) g_blendAutoStart = GetTickCount();
    const int elapsed = (int)((GetTickCount() - g_blendAutoStart) / 1000);

    if (blendSitesReadyAndVanilla()) {
        const bool gate    = setTurnBlend(true);
        const bool refresh = setRefreshBlend(true);
        g_blendAutoWanted = false;
        if (gate && refresh)
            logf(">>> TURN BLENDING: ARMED AUTOMATICALLY%s. Buildings, character skill points, army "
                 "stance, assignments and court posts are usable during ANOTHER PLAYER's turn, and "
                 TW3K_MODE_TEXT("the panels update in place. The AI sweep is untouched. `blend off` reverts this ", "the panels update in place. The AI sweep is untouched. This ")
                 TW3K_MODE_TEXT("machine to stock.", "machine uses automatic blending."), elapsed ? " (after a wait)" : "");
        else
            logf("!! TURN BLENDING: partial arm — gate=%s refresh=%s. See the lines above.",
                 gate ? "ok" : "FAILED", refresh ? "ok" : "FAILED");
        return;
    }

    if (elapsed >= kBlendAutoTimeoutSec) {
        g_blendAutoWanted = false;
        logf("!! TURN BLENDING: gave up auto-arming after %d s — its sites never read as vanilla. "
             "Either this machine's game build differs, or those pages never decrypted. Everything "
             TW3K_MODE_TEXT("else on this machine is unaffected; `blend` reports what the bytes actually say.", "else on this machine is unaffected; inspect the local log."),
             kBlendAutoTimeoutSec);
        return;
    }

    const int bucket = elapsed / 30;                  // one line per 30 s, not one per attempt
    if (bucket != g_blendAutoLast) {
        g_blendAutoLast = bucket;
        if (elapsed)
            diagLogf("turn blending: waiting for its sites to decrypt (%d s) — normal before a campaign "
                 "is loaded.", elapsed);
    }
}

// Read-only. Prints the state, the bytes as they actually are, and what the gate will answer — so a
// reply says whether the patch is really in place rather than what a flag remembers.
#ifndef TW3K_RELEASE
void reportTurnBlend()
{
    logf("=== TURN BLEND (management layer during another player's turn) ===");
    logf("  flag says: %s", g_turnBlendOn ? "ARMED" : "off");

    // Count the call sites by what they ACTUALLY point at, rather than trusting the flag. The gate
    // function is never modified now, so the question is which callers reach the human predicate.
    size_t relaxed = 0, stock = 0, odd = 0, excluded = 0;
    for (size_t i = 0; i < kGateCount; ++i) {
        if (!kGateSites[i].relax) { ++excluded; continue; }
        uint8_t op = 0; int32_t rel = 0;
        const uintptr_t a = g_base + kGateSites[i].rva;
        if (!safeRead((void*)a, &op, 1) || !safeRead((void*)(a + 1), &rel, 4) || op != 0xE8) { ++odd; continue; }
        if      (rel == relTo(kGateSites[i].rva, RVA_IS_HUMAN_TURN)) ++relaxed;
        else if (rel == relTo(kGateSites[i].rva, RVA_TURN_GATE))     ++stock;
        else                                                          ++odd;
    }
    logf("  CALL SITES: %zu relaxed / %zu stock / %zu unrecognised, plus %zu deliberately excluded",
         relaxed, stock, odd, excluded);
    logf("    relaxed = that caller asks \"is the current faction HUMAN\" (true on my turn AND on "
         "another player's turn, false during the AI sweep). stock = it asks \"is it MINE\".");
    for (size_t i = 0; i < kGateCount; ++i)
        if (!kGateSites[i].relax)
            logf("    ✗ EXCLUDED, and left as untouched code: %s", kGateSites[i].what);
    if (odd)
        logf("    ⚠ %zu site(s) point somewhere we do not recognise. Do not arm or disarm from here.", odd);

    // The gate function itself must still read vanilla — we stopped patching it, and if it does not,
    // something else has.
    uint8_t g[BLEND_WORD_LEN] = { 0 };
    if (safeRead((void*)(g_base + RVA_TURN_GATE), g, BLEND_WORD_LEN) &&
        memcmp(g, EXPECT_TURN_GATE, BLEND_WORD_LEN) != 0)
        logf("  ⚠ FUN_142F6BFA0 itself is NOT vanilla. Nothing here writes to it any more — an older "
             "build of this mod did, so a stale DLL is the first thing to suspect.");

    logf("  ✅ 2026-08-08, test-host: buildings constructed, money left the bank, an assignment applied, "
         "and a commanding general changed — all during another player's turn.");
    logf("  ⚠ STILL UNPROVEN: that the OTHER client agrees, and that the session survives long-term.");
    logf("  ⚠ needs TWO humans to exercise — in single player the AI sweep hides the HUD entirely.");

    // The refresh table, counted rather than asserted: how many sites actually read as patched.
    size_t patchedN = 0, vanillaN = 0, otherN = 0;
    for (size_t i = 0; i < kRefreshCount; ++i) {
        uint8_t c[REFRESH_LEN] = { 0 };
        if (!safeRead((void*)(g_base + kRefreshSites[i].rva), c, REFRESH_LEN)) { ++otherN; continue; }
        if      (memcmp(c, kRefreshSites[i].patched, REFRESH_LEN) == 0) ++patchedN;
        else if (memcmp(c, kRefreshSites[i].orig,    REFRESH_LEN) == 0) ++vanillaN;
        else                                                            ++otherN;
    }
    logf("  UI REFRESH events: flag says %s — %zu patched / %zu vanilla / %zu unrecognised, of %zu",
         g_refreshBlendOn ? "ARMED" : "off", patchedN, vanillaN, otherN, kRefreshCount);
    logf("    (this is what makes a panel show an assignment, a court post or a new item WITHOUT "
         "closing and reopening it. Diplomacy is deliberately NOT in the table — it gates entry to a "
         "networked negotiation, not a redraw.)");
    if (otherN)
        logf("    ⚠ %zu site(s) are NEITHER vanilla nor ours. Do not arm or disarm from here.", otherN);
}
#endif
