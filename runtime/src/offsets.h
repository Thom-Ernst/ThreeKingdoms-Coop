// offsets.h - image RVAs and struct offsets shared across modules.
//
// Every constant here was derived in ../NETCODE_NOTES.md; that file is the source of truth and
// its section notes explain how each was found. Included via tw3k.h, which pulls in windows.h
// first. Offsets used by only one module stay in that module.
#pragma once

// ================================================================================================
//  ★★★ WHICH BUILD IS AN ADDRESS IN A COMMENT?  (rebased 1.7.1 -> 1.7.2 on 2026-09-18)
//
//  Read this before trusting any hex address written in a comment anywhere in runtime/src/.
//
//  Every RVA_* CONSTANT in this tree is 1.7.2. All 100 were re-derived against the 1.7.2 binary,
//  verified one at a time, and main.cpp refuses to hook at all if the running image is not that
//  build - so the numbers the code actually USES cannot silently be from the wrong game.
//
//  THE COMMENTS ARE A DIFFERENT MATTER, and they are deliberately not uniform:
//
//    * A FUN_/DAT_ token or bare address that corresponds to one of those 100 constants WAS
//      rewritten to 1.7.2. 260 of them. Those resolve in a 1.7.2 Ghidra database.
//
//    * A FUN_/DAT_ token naming any OTHER game function - a helper, a caller, something noticed
//      in passing during analysis - is STILL A 1.7.1 NAME. There are ~190 of them and they were
//      never RVA constants, so nothing had re-derived them and re-deriving all of them would be
//      a second rebase. They will NOT resolve in a 1.7.2 database. They are kept because the
//      prose around them is still true: the mechanism a note describes did not change just
//      because the address did.
//
//    * A handful are 1.7.1 ON PURPOSE and must stay that way, because they record what was
//      observed in a dated session: #75's account of which of three call sites crashed on
//      2026-08-19 (updguard.cpp, tw3k.h), the 2026-08-14 IconPath diagnoses, the dumps quoted
//      in probes.cpp and eventcursor.cpp, and the extra-digit typo anecdote in gift.cpp.
//      Rewriting those would falsify the history that makes them worth having.
//
//  ⇒ So: if a comment's address matches an RVA_* constant, it is current. Otherwise assume it is
//  1.7.1 and re-derive before relying on it. The mapping that did the rewrite, and the evidence
//  behind every one of the 100, is in E:KTW_Backup\_meta\postupdate\ (rva-map-172.tsv,
//  rebase-verified.tsv, rebase-recovered.tsv, rebase-recovered.md).
// ================================================================================================

// ---------------------------------------------------------------- constants

// RVAs relative to the preferred image base 0x140000000.
static constexpr uintptr_t RVA_LOBBY_SINGLETON = 0x043BD0F0; // DAT_1443BD0F0
static constexpr uintptr_t RVA_INVITE_CMP      = 0x02D7BF5B; // start of CMP insn

// ★★★ The BATTLE ROOT (session 6x). A global *pointer*, not a struct:
//
//     MOV RAX, qword ptr [0x1443c85e0]     ; battle root
//     MOV RCX, qword ptr [RAX + 0x160]     ; the LOCAL player's army
//
// Found through the CcoBattleRoot property table at FUN_1402B22D0, which registers every name the
// UI can ask for against its getter. Two of them answer the question run 19 left us with:
//
//   "PlayerArmyContext"  FUN_1430AB470  ->  *(battleRoot + 0x160)
//   "IsSpectator"        FUN_1430A50B0  ->  byte at +0x10 of FUN_142014A10(battleRoot + 8)
//
// and FUN_142014A10 is three instructions — `return *p + 0x350` — so the spectator flag inlines to
// a pure read with no call into the game:
//
//     isSpectator = *(uint8_t *)( *(uintptr_t *)(battleRoot + 8) + 0x350 + 0x10 )
static constexpr uintptr_t RVA_BATTLE_ROOT_PTR = 0x043C85E0; // DAT_1443C85E0
static constexpr size_t OFF_BR_LOCAL_PLAYER    = 0x008;      // -> object holding the spectator flag
static constexpr size_t OFF_BR_MY_ARMY         = 0x160;      // PlayerArmyContext
static constexpr size_t OFF_LP_SPECTATOR       = 0x360;      // 0x350 (resolve) + 0x10 (the byte)

// ★ B10, the inverted battle colours. The engine's own classifier `FUN_142E7BDA0(battleRoot, unit)`
// decides player / friendly / enemy from four field reads, and each accessor is one instruction:
//     armyOf(unit)     = *(unit + 0x590)   FUN_142379DD0
//     allianceOf(army) = *(army + 0x0A0)   FUN_14102E660
// §6jjj's `alliance(unit) = *(*(unit + 0x590) + 0x0A0)` is these two composed — same chain, now with
// both halves named. Everything the HUD colours hangs on `battleRoot + 0x160` being the right army.
static constexpr size_t OFF_UNIT_ARMY          = 0x590;      // battlefield unit -> its army
static constexpr size_t OFF_ARMY_ALLIANCE      = 0x0A0;      // army -> its alliance

static constexpr size_t OFF_PLAYER_COUNT   = 0xCC;
static constexpr size_t OFF_PLAYER_RECORDS = 0xD0;
static constexpr size_t OFF_SLOT_STRINGS   = 0xE8;
static constexpr size_t SLOT_STRIDE        = 0x10;
static constexpr size_t VANILLA_SLOTS      = 2;

// B4-S2, 1.7.2: the record writer asks the player service for teams 0 and 1 only.
// Service +0x148 -> 14046FE70 (MOV RCX,[RCX+40]; JMP) -> 14046FE80.
// That body scans slots, filtering (flags & 3) == team; teams 2/3 are valid values.
static constexpr uintptr_t RVA_SAVELOBBY_ENUM = 0x0046FE80;
static constexpr uintptr_t RVA_LOBBY_RECORD_WRITER = 0x02D7C810;
static constexpr uintptr_t RVA_LOBBY_ENUM_RETURN = 0x02D7C8B2;
static constexpr uint8_t EXPECT_SAVELOBBY_ENUM[15] = {
    0x48,0x89,0x74,0x24,0x18,0x57,0x8B,0xB1,0x60,0x13,0x00,0x00,0x45,0x33,0xDB
};
static constexpr uint8_t EXPECT_LOBBY_ENUM_CALL[6] = {0xFF,0x93,0x48,0x01,0x00,0x00};
static constexpr uint8_t EXPECT_LOBBY_RECORD_WRITER[18] = {
    0x40,0x55,0x41,0x55,0x41,0x56,0x48,0x8D,0x6C,0x24,0xF0,0x48,0x81,0xEC,0x10,0x01,0x00,0x00
};

// --- campaign-side human-faction vector (the F7 probe) ---------------------
// Chain: DAT_1443CFA50 -> +0x2188 -> +0x78 (campaign model) -> +0x3B68 (container)
static constexpr uintptr_t RVA_CAMPAIGN_ROOT = 0x043CFA50; // DAT_1443CFA50

static constexpr size_t OFF_ROOT_OBJ      = 0x2188;
static constexpr size_t OFF_MODEL         = 0x78;
static constexpr size_t OFF_CONTAINER     = 0x3B68;
static constexpr size_t OFF_LOCAL_FACTION = 0x1A8;  // on the +0x2188 object, not the container

// --- the PENDING-BATTLE manager, and the list a bystander's vote falls back to (#13) -------
//
// ★ WHY THESE ARE HERE. `FUN_141853A90(mgr, faction, x)` answers "which pending-battle record
// belongs to this faction". Its FIRST act is the direct per-faction lookup `FUN_141853980`; but when
// that MISSES — which is every bystander, since a player with no army in the fight has no record —
// it does NOT return 0. It walks the manager's faction-key list and returns the record of the FIRST
// entry whose `+0x12` byte is zero, and the faction it was asked about is never referenced again.
//
// ⇒ that is the leading candidate for #13: the Spectate vote of a player who is not in the battle is
// written into somebody ELSE's participant record, and on 2026-08-16 that somebody was the AI
// attacker. Static reading (tools/ghidra/dump/141853a90_*.c) — the probe below is what measures it.
//
// ⚠ `probes.cpp` carries its OWN copies of the 0x3B80 hop (`OFF_PB_MGR`, `OFF_GATE_STATEOBJ`) for
// the event-feed gate. They are the same offset; these are the names to use in new code.
static constexpr size_t OFF_PBM             = 0x3B80; // model -> the pending-battle manager (a POINTER)
static constexpr size_t OFF_PBM_PHASE       = 0x138;  // manager: battle phase; 0/1/8/9/0B/0E observed
static constexpr size_t OFF_PBM_KEYCOUNT    = 0x154;  // manager: entries in the faction-key list
static constexpr size_t OFF_PBM_KEYLIST     = 0x158;  // manager: the list itself
static constexpr size_t OFF_PBM_PARTICIPANTS= 0x188;  // manager: -> the list the battle waits on
static constexpr size_t PBM_KEY_STRIDE      = 0x18;   // key-list element; a CA string then three bytes
static constexpr size_t OFF_PBKEY_SELECTOR  = 0x12;   // ★ the byte the fallback loop tests for zero

// ★ The DILEMMA guard's vtable. The (D) list carries ~19 node types and only this one waits on a
// player decision; every other type pops on its own within seconds. Measured 2026-08-07: the same
// vptr on all three machines across four reproductions of B1, and the same one in a HEALTHY cycle.
//
// ⚠⚠ It exists so the item lookup can be GATED. `14345FD90` nodes were seen at list head during
// ordinary play carrying +0x28 values of -172425216, 275, 51, 275 and 35 — on those types the field
// is not an index at all, and one whose garbage happens to land inside the vector would otherwise
// print a confident "waits on item N" for a dilemma that does not exist. A false B1 report is worse
// than none: three sessions were already spent chasing a capture that pointed the wrong way.
static constexpr uintptr_t RVA_VPTR_DILEMMA_GUARD = 0x03464380; // 0x143464380

// --- ANSWERING a pending dilemma (plan B stage 2, #43) ---------------------
//
// Every one of these was read out of `FUN_142FA0D50` =
// `UIDLL::function_named_MakeDilemmaChoice_global_context_function` — the entry the game's own UI
// and Lua use. We replicate it minus the argument checking and minus the string→index lookup,
// because we already have the index.
//
// ★★★ THE FINDING THAT MAKES THIS SAFE: the submitted command carries ONLY the option index — no
// record id, no node pointer. The RECEIVER locates the pending dilemma itself, on each machine
// independently, and pops the guard through the engine's own destructor. So there is nothing to
// coordinate: unlike `unblock`, this does NOT have to be issued on every machine. One submission
// from the owning client resolves the dilemma everywhere, by the same path a mouse click would.
//
// ⚠ The engine reaches the guard list as `FUN_14140A920(model + 0x3B38)`, and that function does
// `FUN_1414907D0(arg)` — an interior-pointer→model helper — then returns `**(model + 0x3B88)`.
// ⇒ +0x3B38 and +0x3B88 are the SAME list, reached two ways. The probes' chain is the engine's.
static constexpr uintptr_t RVA_GUARD_LIST_HEAD = 0x0140A920; // FUN_14140A920(model + 0x3B38)
static constexpr size_t    OFF_GUARD_LIST_ARG  = 0x3B38;     // what the engine passes it
static constexpr uintptr_t RVA_GUARD_NEXT      = 0x00322EA0; // next(node) — CALL at 0x142FA9706.
                                                             // Ghidra mis-symbolises this as
                                                             // `_Env_State_LUA_UTILITYDLL_...`, so it
                                                             // can only be had from the disassembly.
static constexpr uintptr_t RVA_GUARD_IS_TYPE   = 0x00A658B0; // FUN_140A658B0(node, &token) -> bool
static constexpr uintptr_t RVA_DILEMMA_TOKEN   = 0x03464994; // &DAT_143464994, the dilemma type token
// ★ `FUN_141429440(node)` returns THE RECORD — it is the factored-out form of the applier's own
// first two lines: `FUN_1414907D0(*(node+0x20) + 0x288)` (faction -> model) then
// `FUN_1414C6B40(model+0x3D30, *(node+0x28))` (item vector -> record). It ends in `JMP 0x1414C6B40`,
// a tail call, so RAX carries the record; Ghidra rendering it `void` is the prototype-not-locked
// artifact, not an ABI. ⇒ it re-derives what our own chain already resolved, by a DIFFERENT route
// (via the guard's faction rather than the global root), which makes it worth calling as a
// CROSS-CHECK and worthless as a source of data.
static constexpr uintptr_t RVA_GUARD_RECORD    = 0x01429440; // FUN_141429440(node) -> record
// The count the SUBMIT path iterates, on the RECORD. ⚠ Measured 2026-08-07 from two dumped records
// (local captures, test-host mp-live): a PENDING 3-option record read +0x28/+0x2C/+0x38/+0x3C all
// = 3, and an ANSWERED 2-option one read all = 2. So the four counts agree in practice — but this
// is the one the engine bounds the choice against, so it is the one used, and a disagreement is
// logged rather than assumed impossible.
static constexpr size_t    OFF_ITEM_OPT_SUBMIT = 0x2C;

// --- STAGE 3: the option KEYS, so a choice is not made blind (#43) -----------
//
// ★★★ Derived from the engine's own accessor, not from a hexdump. `FUN_1417AD740(record, index)`:
//
//     if (index < *(uint*)(record + 0x3C))
//         return *(u64*)(*(longlong*)(record + 0x40) + index * 8);
//     return 0;
//
// ⇒ `+0x3C` is the count it bounds against and `+0x40` points at an array of option POINTERS.
// Each option opens with a vtable and then embeds an ordinary CA string.
//
// ⚠ Reading the hexdump first was the wrong move and cost two passes: the bytes around the record
// are full of unrelated heap neighbours ("_TEMPORARY W:3", "pooled_", "Campaign::Undercover::…")
// that read like fields and are not. One decompile of the accessor gave the layout exactly.
//
// Measured live AND from a dump of the same moment, 2026-08-08 — both options of the Yue Jin
// dilemma ("AN ADEPT RECRUITER"):
//     3k_cp01_char_historical_yue_jin_joins_pc_dilemmaFIRST     (len 53)
//     3k_cp01_char_historical_yue_jin_joins_pc_dilemmaSECOND    (len 54)
// The choice index is named in the key itself, which is what makes a blind pick unnecessary.
static constexpr size_t OFF_ITEM_OPT_COUNT = 0x3C;   // the count FUN_1417AD740 bounds against
static constexpr size_t OFF_ITEM_OPT_ARRAY = 0x40;   // -> array of option pointers, 8 bytes each
static constexpr size_t OFF_OPTION_KEY_LEN = 0x08;   // option: CA string length
static constexpr size_t OFF_OPTION_KEY_PTR = 0x10;   // option: -> the key text
static constexpr size_t MAX_OPTION_KEY     = 160;    // longest seen is 54; refuse anything absurd
static constexpr uintptr_t RVA_MAKE_CHOICE_CMD = 0x01B3F760; // FUN_141B3F760(&buf, index)
static constexpr uintptr_t RVA_SUBMIT_COMMAND  = 0x02F25B60; // FUN_142F25B60(*(rootObj+0x80), &buf)
static constexpr size_t    OFF_COMMAND_QUEUE   = 0x80;       // on the +0x2188 object
// The command buffer is a 16-byte POD — `{ void* vtable; u32 index; }` — built on the stack and
// never freed by the engine's own path, because the submit serialises it and takes no ownership.
static constexpr size_t    CHOICE_CMD_SIZE     = 16;

// On the faction container:
static constexpr size_t OFF_FACTIONS_COUNT = 0x64;  // all factions (turn order) — sanity check
static constexpr size_t OFF_FACTIONS_PTR   = 0x68;
// ⚠⚠ THIS IS THE VESTIGIAL ONE. Measured cap=0 count=0 ptr=NULL in single player AND in a live
// 2-player coop campaign (2026-07-29), i.e. never populated in normal play. It is kept only so the
// log keeps saying so. **The real human-faction list is the registry below** — see #63 / §6uuu.22.
static constexpr size_t OFF_HUMAN_CAP      = 0x180; // standard CA vector {cap,count,ptr}
static constexpr size_t OFF_HUMAN_COUNT    = 0x184;
static constexpr size_t OFF_HUMAN_PTR      = 0x188;

// ============================================================ the REAL human-faction registry (#63)
//
// `mgr = *(model + 0x3D30)` — one 0x298-byte object (size from its destructor FUN_14198BAE0 →
// FUN_140663220(mgr, 0x298)). It carries the pending-item vector AND the campaign's HUMAN_FACTIONS
// list, which is what B1 turns on. The name is not ours: FUN_1414D3C50 writes a save block literally
// called "HUMAN_FACTIONS" by walking {+0x22C, +0x230}, and FUN_1414D0780 reads it back.
//
// ⚠ THE CURSOR ARRAY IS TWO ENTRIES, AND +0x1F8 IS LIVE. Proven three ways (§6uuu.22): the saver
// writes 8 bytes with element count 2, range [+0x1F0, +0x1F8); the destructor zeroes *(u32*)+0x1F8
// after emptying the list at +0x200 (size-at-head−8, same idiom as +0x268); and in a single-player
// dump the never-written slot +0x1F4 reads 0 while +0x1F8 reads 0xEB.
// ⇒ Raising the count past 2 makes FUN_14149F350 (end-turn path, NO bounds check, RVA 0x014976D9)
//    write cursor[2] over +0x1F8. Do not raise the count until cursors 2+ live somewhere we own.
static constexpr size_t OFF_MODEL_EVENT_MGR  = 0x3D30; // model -> the event/pending-item manager
// The per-turn index array: `latest event index` = turnIdx[turnIdxCount-1]. It is what
// FUN_14149F350 writes into a faction's cursor when it finishes compacting, so it is also what we
// write when we SKIP that compaction.
static constexpr size_t OFF_EVMGR_TURNIDX_CNT = 0x1A4;
static constexpr size_t OFF_EVMGR_TURNIDX_PTR = 0x1A8;
static constexpr size_t OFF_EVMGR_CURSOR0    = 0x1F0;  // u32 eventCursor[2] — EXACTLY TWO
static constexpr size_t OFF_EVMGR_LIST_SIZE  = 0x1F8;  // u32, size of the list at +0x200 — LIVE
static constexpr size_t OFF_EVMGR_HF_CAP     = 0x228;  // HUMAN_FACTIONS {cap, count, ptr}
static constexpr size_t OFF_EVMGR_HF_COUNT   = 0x22C;
static constexpr size_t OFF_EVMGR_HF_PTR     = 0x230;
static constexpr uint32_t EVMGR_CURSOR_SLOTS = 2;      // what the struct actually has room for

// On a faction object: the is-human flag, per the IsHuman getter @0x14301CD60.
static constexpr size_t OFF_FACTION_IS_HUMAN = 0xCD0;

// Full instruction we expect to find. If ANY byte differs we refuse to patch:
// that means a different game build, wrong address, or bad analysis.

// ============================================================ hook sites and shared offsets
//
// Gathered here when the single file was split. Every hook is byte-verified before it is written,
// so the expected prologue bytes belong beside the address they are checked against.

// from battle.cpp
static constexpr int  REMATCH_KEY        = VK_F4;

// ★★ TRUE since 2026-08-11, and it was measured wrong first. This read `false`, so BARE F4 armed
// the rematch patch — while the comment beside it, the banner, the help text and three docs all
// said "Ctrl+F4, modifier-gated, cannot be hit in passing". Every one of those was written from the
// CONSTANT'S NAME rather than its value.
//
// It was caught the only way it could be: the smoke test for the hotkey retirement pressed every
// function key to prove none did anything, and F4 armed a live code patch. ⇒ The one key left was
// doing exactly what F1 was retired for.
//
// ★ Same species as `TIME_METRIC::set` (a name read as a measurement) and "FREE KEYS: F6, F7"
// (a comment read as a fact about the code twelve lines below it). **A name is not a value.**
static constexpr bool REMATCH_NEEDS_CTRL = true;

// from gift.cpp
static constexpr size_t    OFF_MODE_SPECTATOR = 0x10;

// from gift.cpp
static constexpr size_t    OFF_MODE_DIRECTION = 0x11;

// from lobby.cpp
static constexpr uintptr_t RVA_MAKE_CASTRING  = 0x00662DF0; // FUN_140662DF0

// from lobby.cpp
typedef void (*MakeCaStringFn)(void*, const char*);

// ★ The WIDE one-shot, the exact mirror of the narrow one above (2026-08-05).
//
// FUN_140663120(CaStringW* dst, const wchar_t* sz) -> dst. Same body as FUN_140662DF0 with
// `strlen` replaced by a `short*` walk and the assign-range call going to FUN_14065F9A0 (wide)
// instead of FUN_14065F840 (narrow). It zeroes dst+8 itself, so it needs no pre-zeroed destination
// and no builder dance — the three-step builder recorded on the backlog is not needed for this.
//
// ⚠ It is WIDE because of what its call site does, not because of its name: at its call site in
// FUN_142D57630 the destination is later destroyed against the **wide** empty sentinel
// `0x143C5CB90`, where the narrow family destroys against `0x143C5CB8E`. Those two sentinels are
// what actually distinguishes the families in this ABI.
//
// ⚠ Unlike the narrow one it can ALLOCATE. The wide small-string threshold is tiny, so anything
// longer than a couple of characters lands on the game heap — so build once and keep it, never per
// query. See `ccoReturnPlayerName` in gift.cpp.
static constexpr uintptr_t RVA_MAKE_CASTRING_W = 0x00663120; // FUN_140663120
typedef void (*MakeCaStringWFn)(void*, const wchar_t*);

// from lobby.cpp
static constexpr uintptr_t RVA_LOBBY_TICK   = 0x02CE4A80; // FUN_142CE4A80

// from lobby.cpp
static constexpr size_t    TICK_STOLEN_LEN  = 16;         // 4 whole insns, none RIP-relative

// from lobby.cpp
static const uint8_t EXPECT_LOBBY_TICK[TICK_STOLEN_LEN] = {
    0x40,0x53,                     // PUSH RBX   (REX-prefixed form)
    0x48,0x83,0xEC,0x60,           // SUB RSP, 0x60
    0x48,0x8B,0xD9,                // MOV RBX, RCX
    0x48,0x8B,0x89,0xB0,0x00,0x00,0x00  // MOV RCX, [RCX+0xB0]
};

// from lobby.cpp
static constexpr uintptr_t RVA_LOBBY_REFRESH  = 0x02D57630; // FUN_142D57630

// from lobby.cpp. Allocates and constructs the live 0x190-byte MPCampaignLobby.
//
// ⚠ This was a BARE HEX LITERAL inline in installLobbyGuard() until 2026-09-18, and that is why
// the 1.7.2 rebase missed it: the sweep remapped RVA_ constants and comment tokens, and a literal
// sitting in a call argument is neither. It was the ONE hook that refused on the first 1.7.2 run --
// "lobby lifetime: SIGNATURE MISMATCH at byte 1 (want 89 got 85)" -- because 1.7.1's 0x02CE3120
// lands mid-function in this build. It is NAMED here so the next rebase cannot miss it the same way.
//
// Relocated by its 15-byte prologue: unique in BOTH builds, and the 1.7.1 hit lands exactly on the
// address the source had hardcoded. 98 of the first 112 body bytes are identical and every
// difference sits inside a displacement field. Corroborated from outside the match -- the
// `lea rdx,[rip+disp32]` at +40 points at the interned string "MPCampaignLobby" in both builds,
// and `mov ecx,0x190` is the documented object size.
static constexpr uintptr_t RVA_LOBBY_CREATE   = 0x02CEB060; // FUN_142CEB060  (1.7.1: 0x02CE3120)

// from lobby.cpp
static constexpr size_t    LOBBY_STOLEN_LEN   = 16;         // 5 whole insns, none RIP-relative

// from lobby.cpp
static const uint8_t EXPECT_LOBBY_REFRESH[LOBBY_STOLEN_LEN] = {
    0x48,0x8B,0xC4,                        // MOV RAX, RSP
    0x48,0x89,0x48,0x08,                   // MOV [RAX+8], RCX
    0x55,                                  // PUSH RBP
    0x53,                                  // PUSH RBX
    0x48,0x8D,0xA8,0x48,0xFF,0xFF,0xFF     // LEA RBP, [RAX-0xB8]
};

// from panels.cpp
static constexpr uintptr_t RVA_PANEL_RESET   = 0x02D4FE10; // FUN_142D4FE10

// from panels.cpp
static constexpr size_t    RESET_STOLEN_LEN  = 17;         // 7 whole insns, none RIP-relative

// from panels.cpp
static const uint8_t EXPECT_PANEL_RESET[RESET_STOLEN_LEN] = {
    0x48,0x89,0x5C,0x24,0x20,   // MOV [RSP+0x20], RBX
    0x56,                       // PUSH RSI
    0x57,                       // PUSH RDI
    0x41,0x54,                  // PUSH R12
    0x41,0x55,                  // PUSH R13
    0x41,0x57,                  // PUSH R15
    0x48,0x83,0xEC,0x40         // SUB RSP, 0x40
};

// from panels.cpp
static constexpr uintptr_t RVA_PANEL_POPULATE = 0x02D7CD10; // FUN_142D7CD10

// from panels.cpp
static constexpr size_t    PANEL_STOLEN_LEN   = 15;         // 8 whole insns, none RIP-relative

// ---------------------------------------------------------------- B9: the widget-tree walk (#12)
//
// FUN_14057BEB0(node, candidates) — the recursive descent that resolves a DUPLICATED widget name to
// whichever same-named widget is a descendant of `node`. Reached only from FUN_1405611E0's second
// table, and only when its third argument is non-zero — which is what panels.cpp:131 passes.
//
// It validates NOTHING: it reads the child count at +0x124 and the child array at +0x128 off
// whatever pointer it is handed, and recurses into every entry. Both captured B9 faults are that
// one defect a level apart — a child list holding something that is not a widget.
//
// ★ The recursion target is the function's OWN ENTRY (`CALL 0x14057BEB0`), so a detour here is
// re-entered at every level of the descent: one hook validates the whole tree.
//
// ⚠ Ghidra types this `void`. The disassembly returns the found widget in RAX. Do not believe the
// decompiled prototype — same trap as REMATCH_NEEDS_CTRL, one file over.
static constexpr uintptr_t RVA_TREE_WALK    = 0x0057BEB0;
static constexpr size_t    TREE_WALK_STOLEN = 15;   // 4 insns; >= the 14 an FF25 jump needs

// ⚠ The epilogue restores RBX/RSI from [RSP+0x30]/[RSP+0x38], which is only correct AFTER the
// stolen PUSH RDI + SUB RSP,0x20 have run. The trampoline therefore re-enters at 0x14057BCEF.
static const uint8_t EXPECT_TREE_WALK[TREE_WALK_STOLEN] = {
    0x48,0x89,0x5C,0x24,0x08,   // MOV [RSP+0x8], RBX
    0x48,0x89,0x74,0x24,0x10,   // MOV [RSP+0x10], RSI
    0x57,                       // PUSH RDI
    0x48,0x83,0xEC,0x20,        // SUB RSP, 0x20
};

// Widget node layout, used by the guard. Read out of FUN_14057BEB0 itself.
static constexpr size_t OFF_WIDGET_CHILD_COUNT = 0x124;
static constexpr size_t OFF_WIDGET_CHILD_ARRAY = 0x128;

// ------------------------------------------------- #47/#50: TELESTRATION, the two-participant cap
//
// ★ The feature's internal name is **telestration** — the broadcast term for drawing over a live
// picture. Nothing in the exe says arrow, line or draw, which is why it went unfound: same lesson as
// `Share`-not-`gift`. Pings ride the same subsystem (`chat_ping_msg` / `chat_telestration_msg`, and
// `ping_attack`/`ping_defend` against `button_attack_brush`/`button_defend_brush`), which is why the
// live data shows them failing and recovering together.
//
// `FUN_142DCDAD0` is the manager's constructor. It attaches to the "radar" widget and builds a hash
// map of **participant key -> 10 stroke objects**, and *which keys exist is decided here, once, at
// battle construction*. Three branches, selected by `+0x10` of `FUN_142014A10(battleMgr+8)` — the
// same mode struct whose `+0x11` battle.cpp writes:
//
//   +0x10 == 0   participant : enumerates ITS OWN ALLIANCE'S members; key = svc->vt[0x220](allianceId, i)
//   +0x10 != 0   spectator   : enumerates a u32 id list via vt[0x208] -> vt[0x188](.., 0x80);
//                              key = *FUN_14046F640(svc, id), and **skipped silently when that is 0**
//   no service   fallback    : one entry, key 0
//
// Measured live 2026-08-15, all three clients: alliance id 0, member count (`alliance+0x1C`) = **1**.
// ❌ And the obvious reading — that the mod's `+0x10=0, +0x11=1` half-state routes lending spectators
// down the participant branch — was TESTED WITH `lend off` AND KILLED: a natural spectator is still
// shut out. Both branches fail for a third player, so the gate is in the key lookup they share.
//
// ⇒ This probe stops the guessing: it reports **which keys this client actually holds**.
static constexpr uintptr_t RVA_TELESTRATION_CTOR = 0x02DCDAD0;   // FUN_142DCDAD0
static constexpr size_t    TELESTRATION_STOLEN   = 15;           // 3 whole insns, none RIP-relative

static const uint8_t EXPECT_TELESTRATION_CTOR[TELESTRATION_STOLEN] = {
    0x48,0x89,0x5C,0x24,0x18,   // MOV [RSP+0x18], RBX
    0x48,0x89,0x54,0x24,0x10,   // MOV [RSP+0x10], RDX
    0x48,0x89,0x4C,0x24,0x08    // MOV [RSP+0x08], RCX
};

// The map, read straight out of the constructor's own tail lookup (0x142DC2F12..0x142DC2F7D):
//     MOV ECX,[R12+0x64]  bucket count   ·  MOV R8,[R12+0x68]  bucket array
//     CMP R9,[RAX+0x10]   node key       ·  MOV RAX,[RAX+0x8]  node next
//     LEA RAX,[R12+0x50]  not-found sentinel
// and 0x142DC30FB (`MOV RAX,[RSI+0x58]`) shows +0x58 is the first node. ⇒ walking it is
// head = *(mgr+0x58), follow +0x08, stop at the sentinel address (mgr+0x50). No game calls needed.
static constexpr size_t OFF_TEL_MAP       = 0x48;   // the map object the inserts are passed
static constexpr size_t OFF_TEL_SENTINEL  = 0x50;   // end-of-list address, not a node to read
static constexpr size_t OFF_TEL_HEAD      = 0x58;   // first node
static constexpr size_t OFF_TEL_NBUCKETS  = 0x64;   // u32
static constexpr size_t OFF_TEL_BUCKETS   = 0x68;   // node**
static constexpr size_t OFF_TEL_NODE_NEXT = 0x08;
static constexpr size_t OFF_TEL_NODE_KEY  = 0x10;

// Alliance, from FUN_141F719A0 -> BattleArmy_GetAlliance and FUN_14057EB50.
static constexpr size_t OFF_ALLIANCE_ID    = 0x08;
static constexpr size_t OFF_ALLIANCE_COUNT = 0x1C;

// ---- the INSERT, read instruction by instruction out of the spectator branch --------------------
//
// ★★★ The constructor already contains the exact code a fix needs, once per participant, at
// `0x142DC2DFA..0x142DC2E8E`. Nothing here is invented; these are its own calls in its own order:
//
//     vec = {cap=0, count=0, data=nullptr}                     ; 16 bytes, CaVec32 shape
//     FUN_142E30A20(&vec, 10)                                  ; reserve
//     for (i = 0; i < 10; ++i) {
//         obj = FUN_142DD56E0(&tmp, strokeIndex++, ctorArg2)    ; build one stroke, 0x150 bytes
//         FUN_142E2F8F0(&vec, obj)                             ; push_back, deep copy
//         if (*(void**)(tmp+0x138)) FUN_140670570(that)         ; ⚠ points buffer FIRST...
//         FUN_141F26CB0(tmp+8)                                  ; ...then the sub-object
//     }
//     pair.key = key                                            ; [RSP+0x48]
//     FUN_142DCD8A0(&pair.value, &vec)                          ; [RSP+0x50] — deep-copies the vector
//     FUN_142DCB810(mgr+0x48, &iter, &pair)                     ; insert
//     FUN_142DD67B0(&pair.value)                                ; destroy what the move left behind
//     ...then destroy vec's elements the same way and free vec.data
//
// ✓ **The insert is idempotent.** `FUN_142DCB810` hashes the key, walks the bucket, and on a hit
// returns `{existing node, inserted=false}` *without* consuming the value — so running it over every
// player, including the two the engine already added, cannot double-insert and cannot leak: the
// value it did not take is destroyed by the `FUN_142DD67B0` that follows either way.
//
// ✓ And `FUN_14046F640(keySvc, slot)` is just the MP session slot record — `*(svc+0x40) + slot*0xF8`,
// bounds-checked against the slot count at `+0x1360` and the `exists` bit 6 of the flags at `+0xF4`,
// falling back to a shared all-zero record. ⇒ **every occupied slot has a real key.** The excluded
// players were never anonymous; nobody ever asked for their key.
static constexpr uintptr_t RVA_TEL_VEC_RESERVE = 0x02E30A20;  // FUN_142E30A20(&vec, n)
static constexpr uintptr_t RVA_TEL_VEC_PUSH    = 0x02E2F8F0;  // FUN_142E2F8F0(&vec, obj)
static constexpr uintptr_t RVA_TEL_STROKE_CTOR = 0x02DD56E0;  // FUN_142DD56E0(&tmp, index, ctorArg2)
static constexpr uintptr_t RVA_TEL_STROKE_DTOR = 0x01F26CB0;  // FUN_141F26CB0(obj + 8)
static constexpr uintptr_t RVA_TEL_VALUE_MAKE  = 0x02DCD8A0;  // FUN_142DCD8A0(&value, &vec)
static constexpr uintptr_t RVA_TEL_MAP_INSERT  = 0x02DCB810;  // FUN_142DCB810(map, &iter, &pair)
static constexpr uintptr_t RVA_TEL_VALUE_FREE  = 0x02DD67B0;  // FUN_142DD67B0(&value)

static constexpr size_t   TEL_STROKE_SIZE   = 0x150;  // element stride, three call sites agree
static constexpr size_t   TEL_STROKE_POINTS = 0x138;  // the point buffer the temp owns
static constexpr size_t   TEL_STROKE_SUBOBJ = 0x08;   // what FUN_141F26CB0 is handed
static constexpr uint32_t TEL_STROKES_PER_PLAYER = 10;

// `mgr+0x78` is THIS client's own entry — the constructor's tail looks the local key up and stores
// `node+0x18` there (`0x142DC2F79: ADD RAX,0x18 / MOV [R12+0x78],RAX`), and leaves it alone when the
// lookup misses. ⇒ inserting a key without re-running that lookup would let everyone see everyone
// else and still leave the excluded client unable to draw. Local record: `svcObj->vt[0x110]()`,
// key = `*(u64*)record`, exactly as the tail reads it.
static constexpr size_t OFF_TEL_LOCAL_ENTRY = 0x78;
static constexpr size_t VT_TEL_LOCAL_RECORD = 0x110;
static constexpr size_t OFF_TEL_NODE_VALUE  = 0x18;

// from panels.cpp
static const uint8_t EXPECT_PANEL_POPULATE[PANEL_STOLEN_LEN] = {
    0x48,0x89,0x4C,0x24,0x08,   // MOV [RSP+8], RCX
    0x53,                       // PUSH RBX
    0x55,                       // PUSH RBP
    0x56,                       // PUSH RSI
    0x57,                       // PUSH RDI
    0x41,0x54,                  // PUSH R12
    0x41,0x56,                  // PUSH R14
    0x41,0x57                   // PUSH R15
};

// from panels.cpp
static constexpr uintptr_t RVA_SLOT_CHANGED = 0x02CE66D0; // FUN_142CE66D0

// from panels.cpp
static constexpr size_t    SLOTCH_STOLEN_LEN = 15;        // 4 whole insns, none RIP-relative

// from panels.cpp
static const uint8_t EXPECT_SLOT_CHANGED[SLOTCH_STOLEN_LEN] = {
    0x48,0x89,0x5C,0x24,0x18,   // MOV [RSP+0x18], RBX
    0x89,0x54,0x24,0x10,        // MOV [RSP+0x10], EDX
    0x48,0x89,0x4C,0x24,0x08,   // MOV [RSP+0x08], RCX
    0x55                        // PUSH RBP
};

// from probes.cpp
static constexpr uint64_t EMPTY_STR_SENTINEL = 0x143C5CB8E; // &DAT_143c5cb8e

// from session.cpp
static constexpr uintptr_t RVA_MP_ADVERTISE = 0x004AB370; // FUN_1404AB370

// from session.cpp
static constexpr size_t    MP_STOLEN_LEN    = 17;         // 4 whole instructions, none RIP-relative

// from session.cpp
static constexpr size_t OFF_MP_SLOTOBJ  = 0xD3848;

// from session.cpp
static constexpr size_t OFF_MP_ADV_MAX  = 0xD23D8;

// from session.cpp
static constexpr size_t OFF_MP_LOCAL_PLAYER = 0xD3898;

// from session.cpp
static constexpr size_t OFF_MP_ADV_FREE = 0xD23D9;

// from session.cpp
static constexpr size_t OFF_MP_ADV_SPEC = 0xD23DA;

// from session.cpp
static constexpr size_t OFF_SLOT_COUNT    = 0x1360;

// from session.cpp
// ⚠ NOT a max-players field, despite having been named one here and printed as one for weeks. It is
// a DERIVED count of OCCUPIED slots, recomputed on every update (wiki/multiplayer.md). Renamed
// because the old name was read back as evidence on 2026-08-04.
static constexpr size_t OFF_SLOT_OCCUPIED = 0x1364;

// from session.cpp
// ★★★ THE PLAYER-INDEX -> SLOT-INDEX TABLE. There are two index spaces in this session object and
// they are NOT the same number: a player index (what several network messages carry) and a slot
// index (what the slot array, the share list and the gift payload use). Four engine message
// handlers convert between them with the identical line, which is the definition of the
// relationship:
//
//     if (playerIdx < 0x14) slot = *(uint *)(slotObj + 0x137C + playerIdx*4); else slot = -1;
//
// ⚠ They coincide whenever seats are contiguous, which is every session where nobody rejoins — so
// code that confuses them tests clean for months. Issue #45 is what that costs: a gift addressed by
// player id landed on whoever happened to hold that SLOT.
static constexpr size_t   OFF_SLOT_PLAYER_TABLE = 0x137C;
static constexpr uint32_t MAX_SESSION_PLAYERS   = 0x14;   // the engine's own bound on that table

// from session.cpp
static constexpr size_t SLOT_ENTRY_STRIDE = 0xF8;

// from session.cpp
static constexpr size_t SLOT_ENTRY_FLAGS  = 0xF4;

// from session.cpp — the flag bits inside SLOT_ENTRY_FLAGS, and the engine's own bound on the array.
// ★ Moved here from session.cpp's file scope 2026-08-17: battle.cpp's #13 side gate reads the same
// bits, and two private copies of a bit mask is how the two halves of a project start to disagree.
//
// ⚠ `SLOT_TEAM_MASK` is the LOBBY TEAM, and it is also what the engine's share-target search calls
// the ALLIANCE (`alliance = flags & 3`, `army = (flags >> 2) & 0xF` — FUN_14046FBC0, §6iii). Same
// bits, two names, and nothing has established a mapping from either to the battle's alliance
// POINTER (`army + 0xA0`). Compare flags with flags or pointers with pointers; never across.
static constexpr size_t   SLOT_MAX_ENTRIES = 20;      // the engine's own cap (`if (0x13 < count)`)
static constexpr uint32_t SLOT_FLAG_VACANT = 0x80;    // bit7  — an open seat
static constexpr uint32_t SLOT_FLAG_SPEC   = 0x400;   // bit10 — a spectator seat, not a player seat
static constexpr uint32_t SLOT_TEAM_MASK   = 0x3;     // low 2 bits — see the warning above

// from session.cpp
static constexpr size_t SLOT_ENTRY_UNITS  = 0x14;   // how many units this player holds (FUN_140471B30)

// from session.cpp
static constexpr size_t SLOT_ENTRY_LIST   = 0x18;   // -> array of unit entries, stride 0x78

// from session.cpp
static constexpr size_t UNIT_REC_STRIDE   = 0x78;

// from session.cpp
static constexpr size_t UNIT_REC_OBJ      = 0x50;   // entry -> the BATTLEFIELD unit object, or null

// from session.cpp
static constexpr size_t UNIT_OBJ_ID       = 0x3D94; // unitObj -> unit id  (both from FUN_1404A9EB0)

// from session.cpp
static const uint8_t EXPECT_MP_ADVERTISE[MP_STOLEN_LEN] = {
    0x48,0x89,0x5C,0x24,0x08,            // MOV [RSP+8], RBX
    0x57,                                // PUSH RDI
    0x48,0x83,0xEC,0x20,                 // SUB RSP, 0x20
    0x4C,0x8B,0x99,0x48,0x38,0x0D,0x00   // MOV R11, [RCX+0xD3848]
};

// from session.cpp
static constexpr uintptr_t RVA_JOIN_HANDLER  = 0x00474F10; // FUN_140474F10

// from session.cpp
static constexpr size_t    JOIN_STOLEN_LEN   = 15;         // 6 whole insns, none RIP-relative

// from session.cpp
static const uint8_t EXPECT_JOIN_HANDLER[JOIN_STOLEN_LEN] = {
    0x48,0x8B,0xC4,             // MOV RAX, RSP
    0x4C,0x89,0x40,0x18,        // MOV [RAX+0x18], R8
    0x55,                       // PUSH RBP
    0x53,                       // PUSH RBX
    0x56,                       // PUSH RSI
    0x48,0x8D,0x6C,0x24,0x80    // LEA RBP, [RSP-0x80]
};

// from session.cpp
static constexpr uintptr_t RVA_SLOT_APPEND  = 0x004477C0; // FUN_1404477C0

// from session.cpp
static constexpr size_t    SEAT_STOLEN_LEN  = 15;         // 4 whole insns, none RIP-relative

// from session.cpp
static const uint8_t EXPECT_SLOT_APPEND[SEAT_STOLEN_LEN] = {
    0x48,0x89,0x5C,0x24,0x08,   // MOV [RSP+0x08], RBX
    0x48,0x89,0x74,0x24,0x10,   // MOV [RSP+0x10], RSI
    0x57,                       // PUSH RDI
    0x48,0x83,0xEC,0x20         // SUB RSP, 0x20
};

