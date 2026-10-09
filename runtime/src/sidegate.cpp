// sidegate.cpp - #13/B10: which army a player with no force in the battle is given. ARMED at attach.
//
// ================================================================================================
// ★★★★★★★ THE BUG, AND IT IS ONE COMPARISON
// ================================================================================================
//
// `FUN_141864380(pendingBattleMgr, out, localFaction)` builds the battle DESCRIPTOR the whole battle
// is then constructed from. It answers one question - *which army is mine* - three ways:
//
//   1. am I in the ATTACKER list (`mgr+0x140`)?  -> take my entry, `idA = armies[entry+0x20]`
//   2. am I in the DEFENDER list (`mgr+0x148`)?  -> same
//   3. neither - I am a BYSTANDER:
//
//          for (i = 0; i < registry->count; i++)             // the human-faction registry,
//              humans += (registry->entries[i].+0x10 != 0);  // model+0x3B38, stride 0x50
//
//          if (humans == 2) { ...find the human who IS in this battle, take THEIR army entry... }
//          else             { idA = 0; }                     // <-- and 0 is armies[0]
//
// ⇒ `playerArmyContext = armies[idA]` (`setupObj+0xA8`), and **`armies[0]` is the attacker**.
//
// ★★★ THAT `== 2` IS THE WHOLE BUG, and it explains every observation this ticket collected:
//
// | | |
// |---|---|
// | 2 humans | the test passes, the branch finds the human participant and hands the spectator THEIR army ⇒ correct |
// | 3+ humans | the test fails, `idA` is set to a literal **0** ⇒ the spectator is built as the ATTACKER |
// | the human ATTACKS | `armies[0]` *is* the human, so the broken path looks right - which is why half of it "works" |
// | both spectators | land identically on `armies[0]`, because it is one constant, not a per-player computation |
//
// ✅ MEASURED live, 3 clients in one battle (§6uuu.53): host `idA = 1`, both spectators `idA = 0`,
// and both spectators' `myArmy(+0x160)` resolved to `3k_main_faction_ma_teng` - the AI attacker.
//
// ✗ It retires four candidates that each cost a session: the pending-battle vote (§6uuu.49 - a
// 2-player control produced a byte-identical key list, selector and vote landing, with the CORRECT
// side), `lend` (off, live, still `ma_teng`), `hfcount` (a no-op below 3 humans, and B10 predates it
// by a week), and the slot raise (it fires at 2 players too, which are fine).
//
// ---------------------------------------------------------------------------- what we write
//
//   1418644CB  83 FF 02        CMP EDI, 2          ; EDI = the human count
//   1418644CE  0F 85 74 01 ..  JNZ 141864648       ; -> idA = 0
//                 ^^ 85 -> 82  =  JB               ; -> idA = 0 only when humans < 2
//
// ★★ ONE BYTE, and at exactly two humans it is a **no-op by construction**: neither `JNZ` nor `JB`
// is taken when `EDI == 2`, so a 2-player game runs byte-for-byte vanilla. Above two the jump stops
// being taken and execution reaches the branch that already does the right thing - it walks the
// whole registry, finds whichever human is in the battle, and copies their army entry. The BODY was
// never two-shaped; only the guard was.
//
// ⇒ Same class as the event-cursor index gate (`02 -> 04`) and `feed icon`: widen a bound the engine
// already handles. No behaviour is invented and no model state is written.
//
// ⚠ ARMED AT ATTACH WITH NO SWITCH, deliberately (tester, 2026-08-18): this is a correctness fix, not
// an experiment, and an unpatched client puts its spectators on the enemy side. A build that has not
// applied it is the broken one. ⇒ There is no `on|off`; `panic`/`detach` restore it like any other
// patch, and that is the only way back.
//
// ⚠⚠ EVERY MACHINE OR NONE - but satisfied by construction rather than by anyone typing anything,
// exactly like the #63 count hold. The descriptor is built locally on each client from replicated
// model state, so every client on this build computes the same answer. A client WITHOUT the patch,
// in a lobby with clients that have it, disagrees about who owns which army - worse than the bug.
// `ping` stays the check that catches that.
//
// ⚠ WHAT THIS DOES NOT CLAIM. The widened path has a second sub-branch: with the count test passed
// it first calls `FUN_14191DC40` (the contentious-fight-battles gate, §6uuu.53) and, when that
// returns true, takes `idA = *(mgr+0x118)` instead of walking. At three players that could still
// hand back something wrong. It is the same code two players already run, so it is not NEW risk -
// but this may fix the common case and leave a rarer one, and a run that still misassigns should
// look there before concluding the patch failed.
//
// ★ And a lead worth keeping (tester, 2026-08-18): a `humans == 2` inside the descriptor builder is
// unlikely to be the only one. Anything else deriving per-player battle identity is worth re-reading
// now that this shape is known to exist here.

#include "tw3k.h"
#include "offsets.h"

static constexpr uintptr_t RVA_BYSTANDER_SIDE = 0x0186CCCB;
static constexpr size_t    SIDEGATE_LEN       = 9;
static constexpr size_t    SIDEGATE_JCC_AT    = 4;    // the 0F 85 -> 0F 82 byte
static constexpr size_t    SIDEGATE_REL_AT    = 5;    // the JNZ's rel32

static const uint8_t EXPECT_SIDEGATE[SIDEGATE_LEN] = {
    0x83,0xFF,0x02,                  // CMP EDI, 0x2
    0x0F,0x85,0x74,0x01,0x00,0x00    // JNZ 0x141864648
};

// ★ The same self-check `feed icon` uses, and it is what makes a wrong write essentially impossible:
// the branch target must still begin with `MOV R9B,1` - `LAB_141864648`'s first instruction, the
// "not a participant, and not two humans" tail. Signature AND destination, or we refuse.
static const uint8_t EXPECT_SIDEGATE_TARGET[3] = { 0x41, 0xB1, 0x01 };

static bool    g_sideGatePatched = false;
static uint8_t g_sideGateOrig[SIDEGATE_LEN] = { 0 };

static bool writeSideGate(uintptr_t addr, const void* src, size_t len)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        logf("#13 side gate: VirtualProtect failed, err=%lu - NOT patched", GetLastError());
        return false;
    }
    memcpy((void*)addr, src, len);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
    DWORD tmp = 0; VirtualProtect((void*)addr, len, oldProtect, &tmp);
    return true;
}

bool patchBystanderSideGate()
{
    if (g_sideGatePatched) return true;
    const uintptr_t addr = g_base + RVA_BYSTANDER_SIDE;

    uint8_t cur[SIDEGATE_LEN] = { 0 };
    if (!safeRead((void*)addr, cur, SIDEGATE_LEN)) {
        logf("#13 side gate: %016llX unreadable - NOT patched", (unsigned long long)addr);
        return false;
    }
    for (size_t i = 0; i < SIDEGATE_LEN; ++i) if (cur[i] != EXPECT_SIDEGATE[i]) {
        logf("#13 side gate: SIGNATURE MISMATCH at byte %zu (want %02X got %02X) - refusing. The "
             "game has probably been updated; re-derive the site rather than forcing anything.",
             i, EXPECT_SIDEGATE[i], cur[i]);
        return false;
    }

    int32_t rel = 0;
    memcpy(&rel, cur + SIDEGATE_REL_AT, sizeof(rel));
    const uintptr_t target = addr + SIDEGATE_LEN + (intptr_t)rel;

    uint8_t tgt[sizeof(EXPECT_SIDEGATE_TARGET)] = { 0 };
    if (!safeRead((void*)target, tgt, sizeof(tgt)) ||
        memcmp(tgt, EXPECT_SIDEGATE_TARGET, sizeof(tgt)) != 0) {
        logf("#13 side gate: the branch target %016llX does not begin with MOV R9B,1 - refusing. "
             "Nothing was written.", (unsigned long long)target);
        return false;
    }

    memcpy(g_sideGateOrig, cur, SIDEGATE_LEN);

    uint8_t patch[SIDEGATE_LEN] = { 0 };
    memcpy(patch, cur, SIDEGATE_LEN);
    patch[SIDEGATE_JCC_AT] = 0x82;              // JNZ -> JB   (humans != 2  ->  humans < 2)

    if (!writeSideGate(addr, patch, SIDEGATE_LEN)) return false;

    g_sideGatePatched = true;
    logf(">>> #13/B10 BYSTANDER SIDE GATE: ARMED. RVA %08llX, one byte (JNZ -> JB), so the battle "
         "descriptor's `humans == 2` test becomes `humans >= 2`. A player with no army in the fight "
         "is now given the HUMAN participant's army instead of armies[0], which is the attacker.",
         (unsigned long long)RVA_BYSTANDER_SIDE);
    logf("    At exactly two humans this changes NOTHING - neither JNZ nor JB is taken when EDI==2, "
         "so a 2-player game runs byte-for-byte vanilla. ⚠ EVERY MACHINE OR NONE: a client without "
         TW3K_MODE_TEXT("this patch disagrees with the others about who owns which army. Check `ping`.", "this patch disagrees with the others about who owns which army. Check the local logs."));
    return true;
}

void unpatchBystanderSideGate()
{
    if (!g_sideGatePatched) return;
    if (writeSideGate(g_base + RVA_BYSTANDER_SIDE, g_sideGateOrig, SIDEGATE_LEN))
        logf("#13 side gate: restored - above two humans, spectators go back to the attacker's side.");
    g_sideGatePatched = false;
}

bool bystanderSideGateArmed() { return g_sideGatePatched; }
