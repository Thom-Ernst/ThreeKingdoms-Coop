#ifndef TW3K_RELEASE
// prebattle.cpp - Who the battle is waiting on, and what each of them chose. Read-only.
//
// It installs a detour, but the hook logs and then calls the original unchanged; nothing below
// writes a byte of game state.
//
// ★ WHY THIS EXISTS. tester's question - "in a two-army battle a spectator is not even prompted to
// choose anything" - turns out to be answerable without a rig. Section 6hhh mapped the pre-battle
// choice end to end: it is the campaign command CCQ_SET_PENDING_BATTLE_READY_TO_START, applied by
// FUN_14191AED0, which stores the chosen role as an `int` at participant entry + 0x10:
//
//     role 0 = NOT CHOSEN YET   1 StartBattle   2 AutoResolve
//     role 3 = Spectate         4 PlayAsAI      5 WithdrawOrRetreat
//
// and whose tail then scans every participant for role 0 and refuses to launch while one remains.
// => THE PARTICIPANT LIST IS THE SET OF PLAYERS THE BATTLE WAITS ON, so "who gets prompted" is a
// question about membership of that list rather than about the UI. This prints it.
//
// ★ It is worth running in SINGLE PLAYER first, and that is half the point of it: one machine, one
// campaign, attack anything, and the log says whether the offsets are right before a multi-machine
// session spends a battle finding out. A one-entry list with a plausible faction key and a role of
// 1 or 2 is a pass.
//
// ✅ CONFIRMED LIVE (2026-08-03, 15:07, single player). One entry, role 0 -> role 1 across the
// original call, and `1 participant still on role 0` -> `every participant has chosen`. So the
// count, the entries pointer, the 0x28 stride and the role at +0x10 are all right on the running
// process, and the readiness gate reads exactly as §6hhh said it would. The faction key was the one
// thing wrong, and it was our reader rather than the offsets — see readParticipantKey.
//
// ⚠ IT PROVES NOTHING ABOUT `D + 0x10`, and must not be read as if it did. This is a u32 role of
// 0..5; `D + 0x10` is a byte observed as 0/1 with a second byte at +0x11. Section 6hhh says plainly
// that a translation step exists between them and has not been found. Do not let a familiar-looking
// offset close a question it does not answer.
//
// ⚠ Deliberately OUTSIDE the nine-hook health check, for the same reason as the HUD ctor hook: a
// machine must never be reported as unready because an experiment's signature did not match.

#include "tw3k.h"

static constexpr uintptr_t RVA_APPLY_BATTLE_ROLE = 0x0191AED0;   // FUN_14191AED0
static constexpr size_t    ROLE_STOLEN_LEN       = 16;           // through SUB RSP,0x30

// MOV [RSP+0x18],RBX / PUSH RBP,RSI,RDI,R12,R14 / SUB RSP,0x30 - register saves and one stack
// adjust, none RIP-relative, and nothing the body still expects in a register the jump could
// clobber. That last clause is what the run-24 crash was bought with: a volatile register is not
// free if the stolen prologue loaded it for the body.
static const uint8_t EXPECT_APPLY_BATTLE_ROLE[ROLE_STOLEN_LEN] = {
    0x48,0x89,0x5C,0x24,0x18,        // MOV [RSP+0x18], RBX
    0x55,                            // PUSH RBP
    0x56,                            // PUSH RSI
    0x57,                            // PUSH RDI
    0x41,0x54,                       // PUSH R12
    0x41,0x56,                       // PUSH R14
    0x48,0x83,0xEC,0x30              // SUB RSP, 0x30
};

typedef void (*ApplyBattleRoleFn)(void*, void*, uint32_t, uint8_t);
static Detour            g_roleDetour;
static ApplyBattleRoleFn g_origApplyBattleRole = nullptr;
static volatile long     g_roleCalls           = 0;

static const char* roleName(uint32_t r)
{
    switch (r) {
        case 0: return "NOT CHOSEN";
        case 1: return "StartBattle";
        case 2: return "AutoResolve";
        case 3: return "Spectate";
        case 4: return "PlayAsAI";
        case 5: return "WithdrawOrRetreat";
        default: return "unknown";
    }
}

// The participant entry begins with a CA string - the faction key.
//
// ✗ The first version of this read the pointer from +0x00 and printed `(unreadable)` for a key that
// was there all along (the 15:07 SP run). A CA string is `{u32 len; u32 cap; char* data}`, so +0x00
// is `len` and the POINTER IS AT +0x08. `dumpRecordString` in probes.cpp had it right; this is the
// same read, and the lesson is the ordinary one - use the reader that is already proven rather than
// writing a second one from the shape of the struct.
//
// The encoding test comes with it: `len` counts CHARACTERS, and this record mixes narrow ASCII keys
// with UTF-16 names, so decide by looking for the interleaved zero bytes rather than assuming.
// A faction key is narrow, but assuming that is how "F.o.r.d.o." got printed once already.
static void readParticipantKey(uintptr_t entry, char* out, size_t outLen)
{
    out[0] = '\0';

    uint32_t len = 0, cap = 0;
    uint64_t data = 0;
    if (!readAt(entry, len) || !readAt(entry + 4, cap) || !readAt(entry + 8, data)) {
        strncpy_s(out, outLen, "(unreadable)", _TRUNCATE);
        return;
    }
    if (len == 0 || !data || data == EMPTY_STR_SENTINEL) {
        strncpy_s(out, outLen, "(empty)", _TRUNCATE);
        return;
    }

    const uint32_t n = len < 63 ? len : 63;
    uint8_t raw[128] = { 0 };
    if (!safeRead((void*)data, raw, n * 2) && !safeRead((void*)data, raw, n)) {
        // Say what could not be read, not just that something could not be. A bare "(unreadable)"
        // is exactly what cost this function a second look.
        _snprintf_s(out, outLen, _TRUNCATE, "(len=%u cap=%u data=%016llX unreadable)",
                    len, cap, (unsigned long long)data);
        return;
    }

    const bool wide = (n >= 2) && raw[1] == 0 && raw[3] == 0 && raw[0] != 0;
    char text[64] = { 0 };
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t c = wide ? raw[i * 2] : raw[i];
        text[i] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    strncpy_s(out, outLen, text, _TRUNCATE);
}

// list[0] = campaign model, list[2] = entries, (char*)list + 0xC = count. Read out of FUN_14191AED0
// itself, which indexes [R14+0x10] with [R14+0xC] at stride 0x28 in four separate places.
static void dumpParticipants(void* list, const char* when)
{
    if (!list) { logf("   participant list is null - nothing to dump."); return; }

    uint32_t  count   = 0;
    uintptr_t entries = 0;
    if (!readAt((uintptr_t)list + 0x0C, count) || !readAt((uintptr_t)list + 0x10, entries)) {
        logf("   participant list at %016llX is unreadable - the offsets are wrong, or this is not "
             "the object 6hhh described.", (unsigned long long)list);
        return;
    }

    // A refusal is worth more than a walk here: this is the first time anything has read this
    // object, so an implausible count means our reading is wrong, not that the game is odd.
    if (count > 64 || (count != 0 && entries <= 0x10000)) {
        logf("   participant list reads count=%u ptr=%016llX - REFUSING to walk it. Those numbers "
             "are not credible, so treat the offsets as wrong.",
             count, (unsigned long long)entries);
        return;
    }

    logf("   >>> PENDING-BATTLE PARTICIPANTS (%s): %u entr%s at %016llX",
         when, count, count == 1 ? "y" : "ies", (unsigned long long)entries);

    int unchosen = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uintptr_t e = entries + (uintptr_t)i * 0x28;
        uint32_t role = 0xFFFFFFFF;
        uint8_t  flag = 0xFF, second = 0xFF;
        readAt(e + 0x10, role);
        readAt(e + 0x14, flag);
        readAt(e + 0x15, second);

        char key[80] = { 0 };
        readParticipantKey(e, key, sizeof(key));
        if (role == 0) ++unchosen;

        logf("        [%u] %-32s role=%u (%s)  flag=%u  +0x15=%u",
             i, key[0] ? key : "(none)", role, roleName(role), flag, second);
    }

    if (unchosen)
        logf("        => %d participant(s) still on role 0, so the battle is WAITING on them. "
             "Anyone who should have been prompted and is NOT in this list never will be.", unchosen);
    else
        logf("        => every participant has chosen; this is the point at which the battle may "
             "launch.");
}

// ---- the locked-turn probe (B1) ----------------------------------------------------------------
//
// ★ tester, 2026-08-04: in the locked state a player can send diplomatic requests and construct
// buildings, but cannot move units and cannot end turn. That rules out the turn never being given to
// them — the campaign model is taking their commands — and points at the army/character layer, with
// END TURN refused alongside movement because both consult something the engine still considers
// owed.
//
// ★★ This is a STATE, not an event, so it is a dump rather than a detour: press F3 while stuck and
// it reads the locked moment directly. No hot-path hook, nothing to rate-limit, and nothing that
// could desync a session — which matters, because `coop_skip_battle_prompts` is shelved for
// desyncing a campaign and anything touching this layer is suspect until proven otherwise.
//
// ⚠ The pointers are CACHED FROM THE HOOK rather than walked from a global. `FUN_14191AED0` is
// handed the participant list and the campaign model directly, so caching what the engine passes us
// avoids inventing a pointer chain — the failure mode that made the first version of the faction-key
// reader print "(unreadable)" for a key that was there all along.
static void* volatile g_lastParticipantList = nullptr;
static volatile long  g_participantDumps    = 0;

void dumpPendingBattleState()
{
    logf("---- PENDING-BATTLE STATE (B1: can the engine still be waiting on something?) ----");

    void* list = g_lastParticipantList;
    if (!list) {
        logf("  no participant list captured this session — nobody has made a pre-battle choice yet,");
        logf("  so there is no pending battle for this probe to be holding. That is itself a finding");
        logf("  if the turn is locked: whatever is owed, it is NOT a pre-battle decision.");
        return;
    }

    logf("  last participant list seen at %016llX (from %ld choice(s) this session):",
         (unsigned long long)list, g_participantDumps);
    dumpParticipants(list, "now");

    logf("  ⚠ Read this carefully. A participant still on role 0 means the engine is waiting for a");
    logf("     decision that may never be prompted — which would hold armies and END TURN while");
    logf("     leaving diplomacy and construction working, exactly the reported shape.");
    logf("     Every participant chosen means the pre-battle path is NOT what is holding this turn,");
    logf("     and the next place to look is the army layer itself.");
}

// ================================================================================================
// ★★★★★ #13 / B10 — WHICH RECORD DOES A BYSTANDER'S VOTE LAND ON?
// ================================================================================================
//
// The 2026-08-16 capture measured the landing and not the cause: a client whose own faction was Yuan
// Shao chose Spectate, and the choice was written into `3k_main_faction_ma_teng`'s participant record
// — the AI attacker's — after which the battle model held that client as Ma Teng, in the alliance
// opposite the human, commanding five of its units. "Joins the AI's team" is the symptom of that.
//
// ★ THE MECHANISM, read out of `FUN_141853A90` (tools/ghidra/dump/141853a90_*.c, lines 67-107).
// Every vote action does `FUN_141853A90(pendingBattleManager, localFaction, 0)` and puts
// `contents(record)` in the command as its target (§6ii, §6hhh). That function opens with the direct
// per-faction lookup, and then:
//
//     if (record == 0) {                                   // a bystander ALWAYS misses here
//         for (entry = mgr->keyList; entry != end; entry += 0x18)
//             if (entry->+0x12 == 0) {                     // the only filter in the loop
//                 faction = <resolve the faction named by THIS entry's key>;
//                 if (faction) return FUN_141853980(mgr, faction, x);   // ★ first hit wins
//             }
//     }
//
// The faction it was asked about is never referenced again on that path. So the answer to "whose
// record does a bystander's Spectate land on" is **the first key-list entry with +0x12 == 0**, and
// nothing about the asking player enters into it.
//
// ⇒ THIS PROBE PRINTS THAT LIST, so the next occurrence names the entry the engine would pick
// instead of leaving it to be inferred. It is READ-ONLY: a pointer walk with plausibility refusals
// and no engine call, so it is safe from the pipe thread and from inside the role hook alike.
//
// ⚠ It measures the CANDIDATE, it does not confirm it. Two things are still inference: that `+0x12`
// is a per-entry selector rather than something inside the key string, and that it is why the human
// defender was skipped on 08-16. A run where the first `+0x12 == 0` entry is the AI attacker
// confirms both; a run where it is the human kills the theory outright, which is worth as much.
//
// ⚠⚠ And it CANNOT be read as "the mod does this". Nothing here is hooked by the mod, and the
// fallback is vanilla code on a vanilla path. Whose bug it is stays open until a vanilla client
// produces the same reading — see #13.

// The engine's own small-string rule, inlined rather than called (`CaString_Length` /
// `CaString_Data`): a tag of 0x8… in the top nibble of +0x08 means the text is INLINE at +0x00 with
// its length in the tag's second byte. Reading {len,cap,ptr} unconditionally is wrong for exactly
// the short values this list is full of, and has printed nonsense on this project before.
static bool readKeyString(uintptr_t s, char* out, size_t outLen)
{
    out[0] = '\0';
    uint64_t tag = 0;
    if (!readAt(s + 8, tag)) { strncpy_s(out, outLen, "(unreadable)", _TRUNCATE); return false; }

    uint32_t    len = 0;
    const void* src = nullptr;
    char        inl[16] = { 0 };

    if ((tag & 0xF000000000000000ull) == 0x8000000000000000ull) {
        len = (uint32_t)((tag >> 56) & 0xF);
        if (!safeRead((void*)s, inl, len)) { strncpy_s(out, outLen, "(inline unreadable)", _TRUNCATE); return false; }
        src = inl;
    } else {
        if (!readAt(s, len)) { strncpy_s(out, outLen, "(unreadable)", _TRUNCATE); return false; }
        if (len == 0 || tag == EMPTY_STR_SENTINEL) { strncpy_s(out, outLen, "(empty)", _TRUNCATE); return true; }
        if (len > 63) { _snprintf_s(out, outLen, _TRUNCATE, "(len=%u implausible)", len); return false; }
        src = (const void*)tag;
    }

    char raw[64] = { 0 };
    if (src != inl && !safeRead(src, raw, len)) {
        _snprintf_s(out, outLen, _TRUNCATE, "(data unreadable @%016llX)", (unsigned long long)tag);
        return false;
    }
    const char* p = (src == inl) ? inl : raw;
    char text[64] = { 0 };
    for (uint32_t i = 0; i < len && i < sizeof(text) - 1; ++i)
        text[i] = (p[i] >= 32 && p[i] < 127) ? p[i] : '.';
    strncpy_s(out, outLen, text, _TRUNCATE);
    return true;
}

// The role command's second argument. ⚠ The corpus renders it as a faction pointer in one decompile
// and a faction KEY in another, and on the receive path it arrives off the wire as whatever the
// sender's `contents(record)` returned — which is a `const char*`. So this does not assume: it tries
// the CA-string shape, then a plain C string, and falls back to hex rather than inventing a name.
// (#13's own next-step list asked for this: the hook logged the POINTER, and both allocations had
// been reused by the time anyone read it.)
static void describeRoleTarget(const void* p, char* out, size_t outLen)
{
    if (!p) { strncpy_s(out, outLen, "(null)", _TRUNCATE); return; }

    char ca[80] = { 0 };
    if (readKeyString((uintptr_t)p, ca, sizeof(ca)) && strncmp(ca, "3k_", 3) == 0) {
        _snprintf_s(out, outLen, _TRUNCATE, "\"%s\" (CA string)", ca);
        return;
    }

    char raw[64] = { 0 };
    if (safeRead(p, raw, sizeof(raw) - 1)) {
        size_t n = 0;
        while (n < sizeof(raw) - 1 && raw[n] >= 32 && raw[n] < 127) ++n;
        if (n >= 4 && raw[n] == '\0') {
            _snprintf_s(out, outLen, _TRUNCATE, "\"%.*s\" (narrow C string)", (int)n, raw);
            return;
        }
        int w = _snprintf_s(out, outLen, _TRUNCATE, "not a key here; first 16 bytes ");
        for (int i = 0; i < 16 && w > 0 && (size_t)w < outLen - 4; ++i)
            w += _snprintf_s(out + w, outLen - w, _TRUNCATE, "%02X ", (uint8_t)raw[i]);
        return;
    }
    strncpy_s(out, outLen, "(unreadable)", _TRUNCATE);
}

static const char* phaseName(uint32_t p)
{
    switch (p) {
        case 0x00: return "map";
        case 0x01: return "PRE-BATTLE";
        case 0x08: return "post-battle";
        case 0x09: case 0x0B: return "in flight";
        case 0x0E: return "map";
        default: return "not a phase this project has observed";
    }
}

void dumpPendingBattleChoice()
{
    uintptr_t root = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) {
        logf("   PENDING-BATTLE KEY LIST: no campaign root — it reads 0 in a battle and at the main "
             "menu, so there is nothing campaign-side to walk. Not a fault.");
        return;
    }
    uintptr_t obj = 0, model = 0, mgr = 0;
    if (!readAt(root + OFF_ROOT_OBJ, obj)   || !obj)   { logf("   chain broke at +0x2188"); return; }
    if (!readAt(obj + OFF_MODEL, model)     || !model) { logf("   chain broke at +0x78");   return; }
    if (!readAt(model + OFF_PBM, mgr)       || !mgr)   { logf("   chain broke at model+0x3B80"); return; }

    uint32_t  phase = 0, keyCount = 0;
    uintptr_t keyList = 0, participants = 0;
    readAt(mgr + OFF_PBM_PHASE, phase);
    readAt(mgr + OFF_PBM_KEYCOUNT, keyCount);
    readAt(mgr + OFF_PBM_KEYLIST, keyList);
    readAt(mgr + OFF_PBM_PARTICIPANTS, participants);

    logf("   >>> PENDING-BATTLE KEY LIST (#13: which record a bystander's vote falls back to)");
    logf("       manager=%016llX  phase(+0x138)=%u (%s)  participants(+0x188)=%016llX",
         (unsigned long long)mgr, phase, phaseName(phase), (unsigned long long)participants);

    // Who is asking. The id->faction map is in wiki/campaign.md; printing the id keeps this probe
    // free of any engine call, which is what makes it safe to run from the pipe thread.
    uintptr_t localFaction = 0;
    if (readAt(obj + OFF_LOCAL_FACTION, localFaction) && localFaction) {
        uint32_t id = 0; uint8_t human = 0;
        readAt(localFaction + 0x08, id);
        readAt(localFaction + 0xCD0, human);
        logf("       this client's own faction = %016llX  id=%u  isHuman=%u",
             (unsigned long long)localFaction, id, human);
    }

    // A refusal beats a walk: an implausible count means our reading is wrong, not that the game is
    // odd, and a number printed without that check is what gets quoted back later as a measurement.
    if (keyCount == 0) {
        logf("       key list is EMPTY (count=0, ptr=%016llX) — no battle is pending, so the "
             "fallback would find nothing and return 0.", (unsigned long long)keyList);
        return;
    }
    if (keyCount > 64 || keyList <= 0x10000) {
        logf("       key list reads count=%u ptr=%016llX — REFUSING to walk it. Those numbers are "
             "not credible, so treat the offsets as wrong rather than the game.",
             keyCount, (unsigned long long)keyList);
        return;
    }

    logf("       %u entr%s at %016llX, stride 0x18:", keyCount, keyCount == 1 ? "y" : "ies",
         (unsigned long long)keyList);

    int  firstIdx = -1;
    char firstKey[80] = { 0 };
    for (uint32_t i = 0; i < keyCount; ++i) {
        const uintptr_t e = keyList + (uintptr_t)i * PBM_KEY_STRIDE;
        char key[80] = { 0 };
        readKeyString(e, key, sizeof(key));

        uint8_t tail[8] = { 0 };
        char    hex[32] = { 0 };
        int     w = 0;
        if (safeRead((void*)(e + 0x10), tail, sizeof(tail)))
            for (size_t b = 0; b < sizeof(tail); ++b)
                w += _snprintf_s(hex + w, sizeof(hex) - w, _TRUNCATE, "%02X ", tail[b]);

        const bool selected = safeRead((void*)(e + OFF_PBKEY_SELECTOR), tail, 1) && tail[0] == 0;
        if (selected && firstIdx < 0) {
            firstIdx = (int)i;
            strncpy_s(firstKey, sizeof(firstKey), key, _TRUNCATE);
        }
        logf("        [%u] %-34s  +0x10..17 = %s%s", i, key[0] ? key : "(none)",
             hex[0] ? hex : "<unreadable>",
             (firstIdx == (int)i) ? "   <== FIRST +0x12==0" : "");
    }

    if (firstIdx < 0) {
        logf("       => no entry has +0x12 == 0, so the fallback loop finds nothing and returns 0. "
             "A bystander voting now would get no record at all — which is a DIFFERENT outcome from "
             "the one #13 describes, and worth recording as such.");
        return;
    }
    logf("       => a player with NO record of their own gets entry [%d]: %s", firstIdx, firstKey);
    logf("          If this client is not in the battle and that key is not this client's faction, "
             "its Spectate/AutoResolve vote lands in ANOTHER faction's record — #13, with a name.");
}

static void applyBattleRoleHook(void* list, void* factionKey, uint32_t role, uint8_t flag)
{
    g_lastParticipantList = list;
    InterlockedIncrement(&g_participantDumps);

    const long n = InterlockedIncrement(&g_roleCalls);
    logf("=== PRE-BATTLE ROLE #%ld: role=%u (%s) flag=%u ===", n, role, roleName(role), flag);

    // ★ The key as TEXT, not as an address. #13's capture could not answer "which faction did this
    // client identify itself with" because the pointer's allocation had been reused four minutes
    // later. A string costs one line and survives the session.
    char target[160] = { 0 };
    describeRoleTarget(factionKey, target, sizeof(target));
    logf("   list=%016llX  factionKey=%016llX  = %s",
         (unsigned long long)list, (unsigned long long)factionKey, target);

    // ★★ And the list the sender's own lookup would have fallen back to. This is the half that was
    // missing on 2026-08-16: the landing was measured, the choice of landing site was not.
    dumpPendingBattleChoice();

    // Before, so the entry this command is about still reads whatever it held previously...
    dumpParticipants(list, "before");

    if (g_origApplyBattleRole) g_origApplyBattleRole(list, factionKey, role, flag);

    // ...and after, which is the write landing. Two readings rather than one, because "the role was
    // stored" and "the role already held that value" look identical from a single dump.
    dumpParticipants(list, "after");
}

bool installBattleRoleHook()
{
    return detourInstall(g_roleDetour, g_base + RVA_APPLY_BATTLE_ROLE, ROLE_STOLEN_LEN,
                         EXPECT_APPLY_BATTLE_ROLE, (uintptr_t)&applyBattleRoleHook,
                         (void**)&g_origApplyBattleRole, "pre-battle role");
}

void removeBattleRoleHook() { detourRemove(g_roleDetour, "pre-battle role"); }

void reportBattleRoles()
{
    if (!g_roleDetour.active) { logf("PRE-BATTLE ROLES: hook not installed."); return; }
    logf("PRE-BATTLE ROLES: hook live, %ld choice(s) seen this session.", g_roleCalls);
    if (g_roleCalls == 0)
        logf("   => nobody has picked a pre-battle option yet. Attack something and choose fight or "
             "auto-resolve; in SINGLE PLAYER that is enough to exercise it.");
}

#endif // developer facilities
