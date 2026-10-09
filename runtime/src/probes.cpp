#ifndef TW3K_RELEASE
// probes.cpp - Read-only probes: stage 2 offset check, deep dump, human-faction vector, full capture.
//
// Split out of the original single-file tw3k_coop.cpp. Behaviour is unchanged: this was pure
// code motion. ../tw3k_coop.ASBUILT.cpp remains the known-good single-file snapshot.

#include "tw3k.h"

// ---------------------------------------------------------------- stage 2

// Dump what we believe the lobby object contains. Correct values here mean our
// static analysis maps onto the running game.
void dumpLobbyState()
{
    // ★ B4-S2 dumps: the global is an idle decoy (count=0), while the dynamic
    // lobby captured by our hooks holds count=2. Make `lobby` report that object.
    const uintptr_t lobby = g_liveLobby;
    if (!lobbyLooksLive(lobby)) {
        logf("lobby: no live MPCampaignLobby (captured object absent or already torn down)");
        return;
    }

    uint32_t count = 0;
    if (!readAt(lobby + OFF_PLAYER_COUNT, count)) {
        logf("lobby: player count unreadable (lobby not constructed yet?)");
        return;
    }

    uintptr_t records = 0;
    readAt(lobby + OFF_PLAYER_RECORDS, records);

    // +0xC0 = game-setup object; the refresh loop only runs when it is non-null.
    // Its +0x60 = is-loaded-save flag, +0xAC != 1 is required for invite.
    uintptr_t setup = 0;
    readAt(lobby + 0xC0, setup);
    uint8_t setupFlag60 = 0; uint32_t setupAC = 0;
    if (setup) { readAt(setup + 0x60, setupFlag60); readAt(setup + 0xAC, setupAC); }

    logf("lobby @ %016llX | players(+0xCC)=%u | records(+0xD0)=%016llX | setup(+0xC0)=%016llX (+0x60=%02X +0xAC=%u)",
         (unsigned long long)lobby, count, (unsigned long long)records,
         (unsigned long long)setup, setupFlag60, setupAC);

    // Sanity flag: a count above the vanilla slot capacity means the refresh
    // loop is about to write past the slot array and into the UI pointers.
    if (count > VANILLA_SLOTS)
        logf("  !! count %u EXCEEDS %zu vanilla slots — overflow territory",
             count, VANILLA_SLOTS);

    // Slot strings are {uint64 len; char* data} — log raw so we don't guess at
    // the string encoding / small-string tagging.
    for (size_t i = 0; i < VANILLA_SLOTS; ++i) {
        const uintptr_t slot = lobby + OFF_SLOT_STRINGS + i * SLOT_STRIDE;
        uint64_t qw[2] = { 0, 0 };
        if (safeRead((void*)slot, qw, sizeof(qw)))
            logf("  slot[%zu] @ +0x%zX  len/tag=%016llX  data=%016llX",
                 i, OFF_SLOT_STRINGS + i * SLOT_STRIDE,
                 (unsigned long long)qw[0], (unsigned long long)qw[1]);
    }

    // Player records: stride 0x48, +0x10 service id, +0x18 faction key.
    if (records && count && count <= 8) {
        for (uint32_t i = 0; i < count; ++i) {
            const uintptr_t rec = records + (uintptr_t)i * 0x48;
            uint32_t id = ~0u;
            char faction[128] = {};
            readAt(rec + 0x10, id);
            readCaString((void*)(rec + 0x18), faction, sizeof(faction));
            logf("  record[%u] @ %016llX  id=%u  faction=\"%s\"",
                 i, (unsigned long long)rec, id, faction);
        }
    }
}

// ---------------------------------------------------------------- deep dump (read-only)


// Hex + ASCII dump of a memory range, 16 bytes per row, offset-labelled.
static void dumpBytesAscii(uintptr_t addr, size_t len, const char* tag)
{
    for (size_t row = 0; row < len; row += 16) {
        uint8_t b[16] = { 0 };
        if (!safeRead((void*)(addr + row), b, 16)) {
            logf("  %s +0x%03zX: <unreadable>", tag, row);
            continue;
        }
        char hx[56] = { 0 }; char as[20] = { 0 }; int hn = 0;
        for (int i = 0; i < 16; ++i) {
            hn += _snprintf_s(hx + hn, sizeof(hx) - hn, _TRUNCATE, "%02X ", b[i]);
            as[i] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
        }
        logf("  %s +0x%03zX: %s|%s|", tag, row, hx, as);
    }
}

// A slot / CA-String looks like { qword tag ; char* data }. Decode the text at
// data (shown as both narrow and wide, since we don't yet know the encoding).
static void dumpSlotString(uintptr_t slotAddr, int idx)
{
    uint64_t tag = 0, data = 0;
    safeRead((void*)slotAddr, &tag, 8);
    safeRead((void*)(slotAddr + 8), &data, 8);

    if (!data || data == EMPTY_STR_SENTINEL) {
        logf("    slot[%d] tag=%016llX data=%016llX  (EMPTY)",
             idx, (unsigned long long)tag, (unsigned long long)data);
        return;
    }
    uint8_t raw[48] = { 0 };
    char narrow[25] = { 0 }, wide[25] = { 0 };
    if (safeRead((void*)data, raw, sizeof(raw))) {
        for (int i = 0; i < 24; ++i)
            narrow[i] = (raw[i] >= 32 && raw[i] < 127) ? (char)raw[i] : '.';
        for (int i = 0; i < 24; ++i)             // wide = every other byte
            wide[i] = (raw[i * 2] >= 32 && raw[i * 2] < 127) ? (char)raw[i * 2] : '.';
    }
    logf("    slot[%d] tag=%016llX data=%016llX  narrow=\"%s\" wide=\"%s\"",
         idx, (unsigned long long)tag, (unsigned long long)data, narrow, wide);
}

// A CA string is { uint32 len ; uint32 cap ; char* data } — 16 bytes. An empty one has len 0 and
// points at a shared sentinel in .rdata rather than null.
//
// The run-1 logs let us decode the 0x48-byte player record without another test session. Three of
// these strings sit inside it, and the middle one is the FACTION KEY: on both the host's and player
// 3's dumps, exactly one record carried a string of length 0x17 = 23 = len("3k_main_faction_cao_cao")
// at +0x18, and it was the *same* player on both machines, while the records for players who had
// not picked a faction held the empty sentinel there. The lobby's own +0xE8 cache corroborates it:
// it stores tag=0x0000001700000017 pointing at that same text.
//
// Printing it decoded turns the next capture from "compare hex by eye" into a direct read.
static void dumpRecordString(uintptr_t rec, size_t off, const char* label)
{
    uint32_t len = 0, cap = 0; uint64_t data = 0;
    if (!readAt(rec + off, len) || !readAt(rec + off + 4, cap) || !readAt(rec + off + 8, data)) {
        logf("      %-12s +0x%02zX <unreadable>", label, off);
        return;
    }
    if (len == 0 || !data || data == EMPTY_STR_SENTINEL) {
        logf("      %-12s +0x%02zX (empty)", label, off);
        return;
    }

    // The record mixes encodings: the faction key and political-party key are narrow ASCII, but the
    // player NAME is UTF-16 (it has to hold arbitrary Steam names). `len` counts CHARACTERS either
    // way, so decode 2*len bytes and decide by looking for the interleaved zero bytes. Getting this
    // wrong is what printed "P.l.a.y.e.r. .N.a.m." for "Player Name".
    const uint32_t n = len < 63 ? len : 63;
    uint8_t raw[128] = { 0 };
    if (!safeRead((void*)data, raw, n * 2)) {                 // wide needs 2n; narrow tolerates the
        if (!safeRead((void*)data, raw, n)) {                 // over-read, so fall back if it faults
            logf("      %-12s +0x%02zX len=%u cap=%u <text unreadable>", label, off, len, cap);
            return;
        }
    }
    const bool wide = (n >= 2) && raw[1] == 0 && raw[3] == 0 && raw[0] != 0;

    char text[64] = { 0 };
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t c = wide ? raw[i * 2] : raw[i];
        text[i] = (c >= 32 && c < 127) ? (char)c : '.';
    }
    logf("      %-12s +0x%02zX len=%u cap=%u %-5s \"%s\"",
         label, off, len, cap, wide ? "utf16" : "ascii", text);
}

// ---------------------------------------- B8: the white rectangle in a save-load lobby
//
// ★★★ WHAT THE REFRESH ACTUALLY DOES, read out of `FUN_142D57630` (2026-08-04). Two map widgets
// hang off the lobby, and a loaded save swaps which one is shown:
//
//     lobby + 0x110   save_game_map            shown  when setup+0x60 (is-loaded-save) is set
//     lobby + 0x118   campaign_selection_map   hidden when it is
//
// and near the top of the SAME function, the block that puts a picture into `+0x110` at all builds
// `campaign_maps/<key>/campaign_map_multiplayer.png` and is gated on a campaign record resolving —
// **not** on the save flag. So an empty-but-visible widget is a perfectly ordinary outcome if a
// loaded save does not resolve that record, and an empty image widget is a white rectangle.
//
// ⇒ TWO CANDIDATES, and they are distinguishable by ONE READ:
//
//   (a) OURS.  The `+0xE8` slot cache has two 0x10-byte entries, so entry 2 spans +0x108..+0x117 and
//       OVERLAPS `+0x110`. The refresh's write loop is bounded by the player count at `+0xCC`, and
//       the show-the-save-map block runs AFTER it — so an overrun corrupts the pointer and then it
//       is dereferenced. This is exactly what the lobby guard exists to prevent.
//       ⚠ Weak on the evidence so far: every capture to date reports `interventions=0`, including
//       the four-player session, so the overrun has never even been ATTEMPTED.
//
//   (b) VANILLA. The widget is shown with no image because the campaign record did not resolve.
//       Nothing to do with this mod, and nobody has ever checked whether stock 3K does it too.
//
// ⇒ **If `+0x110` holds a plausible widget pointer, (a) is dead.** If it holds ASCII — a fragment of
// a faction key — then (a) is proven outright and the guard has a hole. Printing the bytes decides
// it from a single save lobby, with no uninjected A/B needed.
void dumpLobbyMapWidgets(uintptr_t lobby)
{
    if (!lobby) return;

    logf("  --- B8: the save-lobby map widgets ---");

    uintptr_t setup = 0; uint8_t isLoadedSave = 0xFF;
    if (readAt(lobby + 0xC0, setup) && setup) readAt(setup + 0x60, isLoadedSave);
    logf("      setup(+0xC0)=%016llX  is-loaded-save(+0x60)=%u%s",
         (unsigned long long)setup, isLoadedSave,
         isLoadedSave == 1 ? "   <-- SAVE LOBBY: save_game_map is shown, campaign_selection_map hidden"
                           : "");

    // ★ THE INPUT TO THE GATE. The picture block runs only when
    // `FUN_142D589C0(campaignDB, setup + 0x28)` resolves, and the path it then builds is
    // `campaign_maps/<record+0x58>/campaign_map_multiplayer.png`. So `setup+0x28` is the campaign
    // key the whole thing hangs on — and an empty one in a save lobby would explain the white
    // rectangle completely, with no bug anywhere except a lobby that never says which campaign it is.
    //
    // ⇒ Capture this in a SAVE lobby and in a FRESH one. The difference between the two strings is
    // the finding; neither on its own says much.
    if (setup) {
        char key[80] = { 0 };
        if (readCaStringAuto((void*)(setup + 0x28), key, sizeof(key)) && key[0])
            logf("      campaign key (setup+0x28) = \"%s\"", key);
        else
            logf("      campaign key (setup+0x28) = (EMPTY or unreadable)   <-- ★★★ if this is empty "
                 "in a save lobby and set in a fresh one, THAT IS B8: the picture block is gated on "
                 "this key resolving, so the widget is shown with no image.");
    }

    static const struct { size_t off; const char* name; } kWidgets[] = {
        { 0x110, "save_game_map" },
        { 0x118, "campaign_selection_map" },
        { 0x120, "template_icon_army" },
    };

    for (size_t i = 0; i < sizeof(kWidgets) / sizeof(kWidgets[0]); ++i) {
        const uintptr_t at = lobby + kWidgets[i].off;
        uintptr_t p = 0;
        if (!readAt(at, p)) { logf("      %-24s +0x%03zX <unreadable>", kWidgets[i].name,
                                   kWidgets[i].off); continue; }

        // A widget is a heap object with a vtable in the image. A smashed entry is a CA string
        // fragment, so it reads as small integers or as text — both obvious once printed.
        uintptr_t vt = 0;
        const bool plausible = (p > 0x10000) && readAt(p, vt) && vt > 0x140000000ull &&
                               vt < 0x150000000ull;

        // The eight bytes AT the slot, not at what it points to — a smashed entry is a CA string
        // fragment, so the faction key shows up as text right here.
        char ascii[9] = { 0 };
        uint8_t raw[8] = { 0 };
        safeRead((void*)at, raw, 8);
        for (int b = 0; b < 8; ++b) ascii[b] = (raw[b] >= 32 && raw[b] < 127) ? (char)raw[b] : '.';

        logf("      %-24s +0x%03zX = %016llX  vtable=%016llX  bytes=\"%s\"  %s",
             kWidgets[i].name, kWidgets[i].off, (unsigned long long)p,
             (unsigned long long)vt, ascii,
             plausible ? "looks like a widget" : "★★★ NOT A WIDGET — see below");

        if (!plausible && kWidgets[i].off == 0x110)
            logf("      ★★★ save_game_map is NOT a widget pointer. The +0xE8 slot cache HAS "
                 "overrun into it (entry 2 spans +0x108..+0x117), the guard has a hole, and B8 is "
                 "ours. The refresh shows this pointer for a loaded save — hence the white area.");
    }

    logf("      ⇒ If save_game_map above LOOKS LIKE A WIDGET while the map still renders white, the "
         "overflow is exonerated and B8 is the image never being loaded — the picture block in "
         "FUN_142D57630 is gated on a campaign record resolving, not on the save flag.");
}

// Full read-only anatomy of the live lobby object.
void dumpInstanceDeep(uintptr_t inst)
{
    uint32_t cnt = 0; uint64_t recs = 0, setup = 0;
    safeRead((void*)(inst + OFF_PLAYER_COUNT), &cnt, 4);
    safeRead((void*)(inst + OFF_PLAYER_RECORDS), &recs, 8);
    safeRead((void*)(inst + 0xC0), &setup, 8);

    logf("=== DEEP DUMP live lobby @ %016llX  count(+0xCC)=%u records(+0xD0)=%016llX setup(+0xC0)=%016llX ===",
         (unsigned long long)inst, cnt, (unsigned long long)recs, (unsigned long long)setup);

    // The player list is a standard CA dynamic array {cap +0xC8, count +0xCC, ptr +0xD0} — NOT a
    // fixed pair. Run 1 read cap=4/count=3 (three players registered); run 2 read cap=2/count=2,
    // i.e. player 3 was seated in the SESSION but never added to the LOBBY. Printing the capacity
    // alongside the count is what makes that distinction visible at a glance.
    uint32_t vcap = 0;
    safeRead((void*)(inst + 0xC8), &vcap, 4);
    logf("  player vector: cap(+0xC8)=%u count(+0xCC)=%u ptr(+0xD0)=%016llX%s",
         vcap, cnt, (unsigned long long)recs,
         (cnt < 3 && vcap <= 2) ? "   <-- only 2 players registered in the LOBBY" : "");

    logf("  --- full object (0x190 bytes) ---");
    dumpBytesAscii(inst, 0x190, "obj");

    // Slots 0..3: 0/1 are the real 2-slot cache; 2/3 land on the UI pointers
    // (+0x110/+0x120) — dumping them shows exactly what a >2 write would clobber.
    logf("  --- slot cache @ +0xE8 (idx 2,3 overlap UI ptrs) ---");
    for (int i = 0; i < 4; ++i) dumpSlotString(inst + OFF_SLOT_STRINGS + (uintptr_t)i * 0x10, i);

    // B8 — printed right after the cache it might have been overrun by, so the two read together.
    dumpLobbyMapWidgets(inst);

    // Player records @ +0xD0, stride 0x48. Are these dynamically sized (=> engine
    // already handles N players) or fixed? Raw dump lets us read names + ids.
    if (recs && cnt && cnt <= 16) {
        logf("  --- player records @ +0xD0 (stride 0x48) ---");
        for (uint32_t i = 0; i < cnt; ++i) {
            uintptr_t rec = recs + (uintptr_t)i * 0x48;
            uint32_t id = 0; readAt(rec + 0x10, id);
            logf("  record[%u] @ %016llX  id=%u:", i, (unsigned long long)rec, id);
            dumpBytesAscii(rec, 0x48, "rec");
            logf("    decoded strings:");
            dumpRecordString(rec, 0x00, "name");
            dumpRecordString(rec, 0x18, "FACTION?");
            dumpRecordString(rec, 0x28, "string3");
        }
    }
    logf("=== end deep dump ===");
}

// ------------------------------------------------- human-faction vector probe

// THE question this probe exists to answer: is the campaign-side human-faction
// vector a FIXED 2-slot preallocation (a real campaign cap we would have to
// enlarge) or a lazily-grown dynamic vector (=> the 2-player limit is purely the
// lobby, which is already fully mapped)?
//
// SINGLE PLAYER IS DECISIVE — no second machine needed:
//   ptr == 0 && cap == 0   -> lazily grown   -> NO campaign-side cap      (good)
//   ptr != 0 && cap == 2   -> fixed 2 slots  -> real campaign-side cap    (must enlarge)
// With exactly 2 humans the two cases are indistinguishable (doubling gives
// cap==2 as well), which is why we want the single-player reading.

// Best-effort allocation size. The game uses its own allocator (FUN_1406705A0),
// so the CRT/Win32 heap almost certainly does not own this block — we therefore
// treat HeapSize as a bonus and rely mainly on the raw header bytes, which most
// allocators use to store the block size just below the returned pointer.
static void dumpAllocationEvidence(uintptr_t ptr)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)ptr, &mbi, sizeof(mbi)))
        logf("  region: base=%016llX size=0x%zX state=%lX protect=%lX type=%lX",
             (unsigned long long)mbi.BaseAddress, (size_t)mbi.RegionSize,
             mbi.State, mbi.Protect, mbi.Type);

    HANDLE heaps[64] = { nullptr };
    DWORD  n = GetProcessHeaps(64, heaps);
    for (DWORD i = 0; i < n; ++i) {
        SIZE_T sz = (SIZE_T)-1;
        __try { if (HeapValidate(heaps[i], 0, (void*)ptr)) sz = HeapSize(heaps[i], 0, (void*)ptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { sz = (SIZE_T)-1; }
        if (sz != (SIZE_T)-1) {
            logf("  HeapSize: block is 0x%zX bytes (= %zu entries of 8) in heap %u",
                 (size_t)sz, (size_t)sz / 8, (unsigned)i);
            return;
        }
    }
    logf("  HeapSize: block not owned by any Win32 heap (custom allocator, as expected)");
    logf("  --- 32 bytes BELOW the pointer (allocator header; look for a size field) ---");
    dumpBytesAscii(ptr - 32, 32, "hdr");
}

static void dumpHumanFactionVector()
{
    // ⚠ NAME IT FOR WHAT IT IS. This vector has read empty in every campaign ever probed, and #63
    // established that the list the engine consults is the registry on *(model+0x3D30) — printed
    // just above by dumpEventMgrRegistry(). Keep this reading: an empty vector here is a standing
    // confirmation, and the day it is NOT empty is worth knowing about immediately.
    logf("=== F7: VESTIGIAL container human-faction vector (+0x180) — the real one is the "
         "HUMAN_FACTIONS registry above ===");

    uintptr_t root = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) {
        logf("  DAT_1443CFA50 = 0 -> not in a campaign. Load a campaign first, then press F7.");
        return;
    }
    uintptr_t obj = 0, model = 0, cont = 0;
    if (!readAt(root + OFF_ROOT_OBJ, obj) || !obj) { logf("  chain broke at +0x2188"); return; }
    if (!readAt(obj + OFF_MODEL, model) || !model)  { logf("  chain broke at +0x78");   return; }
    if (!readAt(model + OFF_CONTAINER, cont) || !cont) { logf("  chain broke at +0x3B68"); return; }
    logf("  chain: root=%016llX -> +0x2188=%016llX -> +0x78=%016llX -> +0x3B68 container=%016llX",
         (unsigned long long)root, (unsigned long long)obj,
         (unsigned long long)model, (unsigned long long)cont);

    // Sanity: the all-factions turn-order array should hold ~40-60 factions in a
    // real 3K campaign. If this looks sane we know we are on the right object.
    uint32_t facCount = 0; uintptr_t facPtr = 0;
    readAt(cont + OFF_FACTIONS_COUNT, facCount);
    readAt(cont + OFF_FACTIONS_PTR, facPtr);
    logf("  SANITY all-factions: count(+0x64)=%u  ptr(+0x68)=%016llX  %s",
         facCount, (unsigned long long)facPtr,
         (facCount >= 5 && facCount <= 512 && facPtr) ? "(looks like the faction container OK)"
                                                      : "!! UNEXPECTED — offsets may be wrong");

    // The measurement.
    uint32_t cap = 0, count = 0; uintptr_t hptr = 0;
    readAt(cont + OFF_HUMAN_CAP, cap);
    readAt(cont + OFF_HUMAN_COUNT, count);
    readAt(cont + OFF_HUMAN_PTR, hptr);
    logf("  >>> HUMAN VECTOR: capacity(+0x180)=%u  count(+0x184)=%u  data(+0x188)=%016llX",
         cap, count, (unsigned long long)hptr);

    if (hptr == 0 && cap == 0) {
        logf("  >>> VERDICT: NOTHING PREALLOCATED -> lazily grown dynamic vector.");
        logf("  >>> No campaign-side cap. The 2-player limit is purely the LOBBY.");
    } else if (hptr != 0 && cap == 2) {
        logf("  >>> VERDICT: FIXED 2-SLOT PREALLOCATION -> a real campaign-side cap.");
        logf("  >>> This vector must be enlarged/relocated for >2 humans.");
    } else {
        logf("  >>> VERDICT: inconclusive shape (cap=%u ptr=%016llX) — see raw dumps below.",
             cap, (unsigned long long)hptr);
    }

    if (hptr) {
        dumpAllocationEvidence(hptr);
        const uint32_t show = (cap > count ? cap : count) + 2; // peek past the end too
        logf("  --- vector entries (showing %u, incl. 2 past the end) ---",
             show > 16 ? 16u : show);
        for (uint32_t i = 0; i < show && i < 16; ++i) {
            uintptr_t fac = 0;
            if (!readAt(hptr + (uintptr_t)i * 8, fac)) { logf("    [%u] <unreadable>", i); continue; }
            uint8_t human = 0;
            if (fac) readAt(fac + OFF_FACTION_IS_HUMAN, human);
            logf("    [%u] faction=%016llX  isHuman(+0xCD0)=%u%s",
                 i, (unsigned long long)fac, human,
                 (i >= count) ? "   <-- past count" : "");
        }
    }

    // Independent cross-check: count humans by walking ALL factions and testing
    // the per-faction flag. This is how WH3 derives its human set, and it tells
    // us what the engine really thinks regardless of the vector's contents.
    if (facPtr && facCount && facCount <= 512) {
        uint32_t humans = 0;
        for (uint32_t i = 0; i < facCount; ++i) {
            uintptr_t fac = 0;
            if (!readAt(facPtr + (uintptr_t)i * 8, fac) || !fac) continue;
            uint8_t human = 0;
            if (readAt(fac + OFF_FACTION_IS_HUMAN, human) && human) {
                ++humans;
                logf("    human faction #%u: %016llX (index %u of %u)",
                     humans, (unsigned long long)fac, i, facCount);
            }
        }
        logf("  CROSS-CHECK: %u faction(s) have isHuman set, vs vector count=%u", humans, count);
    }

    uintptr_t localFac = 0;
    if (readAt(obj + OFF_LOCAL_FACTION, localFac))
        logf("  local faction (+0x1A8 on the +0x2188 obj) = %016llX",
             (unsigned long long)localFac);

    logf("=== end F7 probe ===");
}

// ------------------------------------------- B1: whose turn does THIS client think it is?
//
// ★★★ WHY THIS IS THE FIRST READ. tester, 2026-08-04: in the locked state a player CAN send
// diplomatic requests and CAN construct buildings, and CANNOT move armies or end turn. That rules
// out the client being cut off — the campaign model is taking its commands — so what is left is a
// gate that some actions consult and others do not.
//
// `wiki/campaign.md` already says where such a gate lives, and it was written long before this bug:
// **3K's turn permission is distributed and mostly in the UI**, unlike WH3's single chokepoint. So
// "does this client believe it is my turn" is not a background detail here — it is the question, and
// it is three field reads.
//
// The engine's own predicate is `FUN_1419B5CA0(campaign, faction, allowEndPhase)`:
//
//     container->0x8C == 1                           // phase == acting
//  && FUN_1417BD660(campaign + 0x3B70)                // a mode check — NOT decompiled
//  && container->0x48->id(+8) == faction->id(+8)      // the current faction IS this faction
//
// ⚠ Two of the three clauses are plain reads and are evaluated below. The middle one is a CALL and
// is deliberately not made: F3 runs on the probe thread and calling into game objects from here
// could race the game thread — the same rule the ready-check virtual is already skipped under. Its
// argument pointer is printed instead, so two machines can at least be compared on it.
//
// ★ COMPARE MACHINES ON IDS AND INDICES, NEVER ON POINTERS. Faction objects are separate
// allocations per process; the id at +0x8 and the position in the faction array are the simulation's
// own values and must agree across a lockstep campaign. If they do not, that is a desync, and this
// dump is how it would first become visible to us.

// ★★★★★ THE PREDICATE HAS NINE CLAUSES, NOT THREE (read out in full 2026-08-04, §6mmm).
//
// tester, correcting the reading that produced the three-clause version: *"in this game armies cannot
// move unless it is your turn... what is different is that here the player was able to RECRUIT,
// DEPLOY and SPLIT armies, just not move them."*
//
// ⇒ That is a much finer symptom than "the army layer". Recruiting, deploying and splitting are all
// army-layer actions and they WORKED. Only the two things this predicate gates were refused. So the
// predicate is not one candidate among several — it is the thing, and it was only ever evaluated two
// thirds of the way.
//
// `FUN_1419B5CA0(campaignModel, faction, allowEndPhase)` in full:
//
//   (A)  *(u32*)(model + 0x3D84) != 0                       -> refuse          ← never read before
//        allowEndPhase && container->0x8C == 3              -> ALLOW (early)
//        container->0x8C != 1                               -> refuse            phase
//   (B)  *(void**)(mode + 0x38) != mode + 0x30              -> refuse          ← the "call"
//        container->0x48->id != faction->id                 -> refuse            the turn is mine
//   (C)  FUN_141844B60(model+0x3B80) && !FUN_141869E30(…)   -> refuse            pending battle
//   (D)  FUN_1403ECB80(model + 0x3B88)                      -> refuse
//   (E)  *(u8*)(model + 0x3C41) != 0                        -> refuse          ← never read before
//   (F)  *(u8*)(model + 0x3C42) != 0                        -> refuse          ← never read before
//        …then faction-side clauses on faction+0xE58, and ALLOW
//
// ★★★ (B) IS NOT A MODE CHECK, AND IT IS NOT EXPENSIVE. `FUN_1417BD660` is one line:
//
//     return *(void**)(p + 0x38) == (p + 0x30);
//
// — the canonical **"is this intrusive linked list EMPTY?"** test, where an empty list's head points
// at itself. With `p = *(model + 0x3B70)`, the clause reads: **something must be empty, or no army
// may move and no turn may end.** It was skipped for a year of this project's life because it looked
// like a call into the engine; it is two loads and a compare, and it can be evaluated from the probe
// thread as safely as any other read.
//
// ⇒ ★ **A queue that should drain and does not is exactly the shape of a lag-sensitive bug**, which
// is where B1's evidence already points (LAN-only sessions clean, remote-peer sessions broken).
//
// ★★★★★ AND THE LIST IS NOW IDENTIFIED: IT IS MOVEMENTS IN PROGRESS.
//
// `*(model + 0x3B70)` is allocated `0x120` bytes at `0x14196B87F` and constructed by
// `FUN_14174E8F0`, which stores a vtable at `0x143483F28` and a data pointer to `0x143483F40`.
// Immediately after that data pointer sits the subsystem's own string table:
//
//     walk · flee · navy_embark · navy_disembark · agent_join_force · agent_leave_force · teleport
//
// ⇒ Those are **campaign movement types**. The object is the movement/locomotion controller, and
// clause (B) therefore reads, in plain words:
//
//     ★ NO ARMY MAY MOVE AND NO TURN MAY END WHILE A MOVEMENT IS STILL IN FLIGHT.
//
// That closes three open questions at once:
//   * why movement and END TURN refuse together while recruit / deploy / split / build / diplomacy
//     keep working — those never consult this predicate;
//   * why `CCQ_CHARACTER_LOCOMOTE_TO` counts **zero** through a session of constant movement, and no
//     movement command appears in the queue at all — movement is carried by THIS subsystem, not by
//     the command queue we hook;
//   * why it looks like latency — a movement whose completion never arrives leaves this list
//     non-empty **forever**, and that client can never move again and never end another turn. That
//     is B1's report exactly, including its permanence and its survival into a save.
//
// ⚠ **Hypothesis, and the gap is stated:** nothing yet shows a movement actually stuck. That is what
// the walk below is for. ★ It is also tester's own theory from the chair — *"the game does not allow
// you to end turns [when] moving units"* — arrived at from play, months before the address was read.
//
// The list is a circular doubly-linked list with its sentinel AT `mode + 0x30`:
//     sentinel.next = *(mode + 0x38)     sentinel.prev = *(mode + 0x40)
//     both point back at `mode + 0x30` when empty, which is precisely FUN_1417BD660's test
// so a node N has its next at `*(N + 0x8)`. What the node is embedded in is NOT known, so the walk
// prints the node address and its first bytes rather than inventing a layout.
static constexpr size_t OFF_CUR_FACTION   = 0x48;    // container: whose turn it is
static constexpr size_t OFF_WRAP_FACTION  = 0x50;    // container: round-end / wrap faction
static constexpr size_t OFF_TURN_PHASE    = 0x8C;    // container: turn-phase state machine, 0..3
// ★★★★★ (C) AND (D) ARE PLAIN READS TOO (2026-08-04, §6qqq). They were left unevaluated because
// they are calls; all three turn out to be one-liners, exactly like (B) did:
//
//   FUN_141844B60(pb)  =  s != 0 && s != 0xE        where s = *(int*)(pb + 0x138)
//   FUN_141869E30(pb)  =  s == 0xE                          (same field)
//   FUN_1403ECB80(t)   =  *(u64*)t != 0             — Ghidra calls it `TIME_METRIC::set()`
//
// and the predicate combines the first two as `if (C1 && !C2) refuse`, which collapses to:
//
//   (C) REFUSES  iff  *(int*)( *(model + 0x3B80) + 0x138 )  is neither 0 nor 14
//   (D) REFUSES  iff  *(u64*)( *(model + 0x3B88) )          is non-zero
//
// ⇒ (C) is the pending-battle manager's **state machine**: 0 idle, 14 explicitly permitted, anything
// else means a battle is outstanding and NOTHING may act.
//
// ============================================================================================
// ✗✗✗ (D) IS NOT A TIMER. RETRACTED 2026-08-05, and it changes what B1 IS.
// ============================================================================================
//
// The symbol said `TIME_METRIC::set`, so this file said "a timer which, left set, refuses every
// action forever", and the bug board said the raw unit was microseconds and the stuck value a timestamp
// left behind. **All of that was wrong**, and it was wrong in the project's most familiar way: a
// NAME was read as a MEASUREMENT.
//
// What `*(model + 0x3B88)` actually holds is a **head-cell pointer for an intrusive singly-linked
// LIFO of "campaign is blocked" guard nodes**, and (D) refuses while that list is NON-EMPTY:
//
//   model + 0x3B88  ->  an 8-byte heap cell, allocated once in the model ctor at 0x14196C3AD
//                        *cell  =  the head node, or 0 when the list is empty
//
//   node + 0x00   vptr
//   node + 0x08   back-pointer to the CELL (so a node can unlink itself without the model)
//   node + 0x10   next
//   node + 0x18   a per-frame COUNTDOWN
//
// Three independent reads kill the timestamp story outright:
//   * the qword is **dereferenced and indexed** at two separate sites — `FUN_1419C4E10` and
//     `FUN_1419EEF90` both do `MOV RAX,[model+0x3B88] / MOV RCX,[RAX] / CMP dword [RCX+0x18],0`.
//     A microsecond count cannot be a pointer with a field at +0x18.
//   * every one of the **17** values this project has ever logged, across two machines, is
//     **16-byte aligned**. For a microsecond counter that is (1/16)^17. They are heap blocks.
//   * the logged values jump **backwards by billions** between samples 23 seconds apart.
//
// ★★★ SO B1 IS A GUARD NODE PUSHED AND NEVER POPPED — a campaign operation that started and never
// finished — not a stale timestamp. And the normal case is not "this never happens": in the naming
// session (D) set and cleared **ten times on one machine and seven on the other**, with holds of
// 0, 5, 6, 7, 9, 17 and **27** seconds, all of them healthy. Only the last one stuck.
//
// ⇒ THE POP, and the hazard hiding in it: `FUN_14145BA50` is called every frame from the campaign
// update loop `FUN_1419EEF90` and ticks **only the head node's** +0x18. At zero it virtual-calls
// slot +0x28 (`FUN_14140C500`, self-unlink and delete); if it is already zero it polls slot +0x38
// instead. ⚠ **A node buried under a newer push is never ticked at all**, so a LIFO ordering
// inversion is a candidate deadlock shape beside "the completion never arrived".
//
// ⚠ "19 push sites" is a proxy count, NOT an enumeration, and must not be repeated as one. Every
// write to the head cell is a zero-displacement `MOV [reg], val` on a standalone 8-byte allocation,
// so no byte search on `+0x3B88` can see one; the 19 were found by "loads the slot, then stores
// into a node". `FUN_14140C500` itself writes the cell that way and is not in the 19.
static constexpr size_t OFF_PB_MGR        = 0x3B80;  // model: -> the pending-battle manager
static constexpr size_t OFF_PB_STATE      = 0x138;   // manager: its state machine (0 / 14 = fine)
static constexpr size_t OFF_GATE_D_LIST   = 0x3B88;  // model: -> the blocked-list HEAD CELL
static constexpr size_t OFF_BLK_OWNER     = 0x08;    // node: back-pointer to the head cell
static constexpr size_t OFF_BLK_NEXT      = 0x10;    // node: next node
static constexpr size_t OFF_BLK_COUNTDOWN = 0x18;    // node: per-frame countdown; 0 = polling
static constexpr size_t OFF_BLK_FACTION   = 0x20;    // node: the faction the guard was pushed for
static constexpr size_t OFF_BLK_INDEX     = 0x28;    // node: WHICH item of model+0x3D30 it waits on
static constexpr size_t BLK_WALK_CAP      = 16;      // a corrupt list must not spin the probe
static constexpr size_t OFF_GATE_A        = 0x3D84;  // model: u32, non-zero refuses everything
static constexpr size_t OFF_GATE_E        = 0x3C41;  // model: u8
static constexpr size_t OFF_GATE_F        = 0x3C42;  // model: u8
static constexpr size_t OFF_LIST_HEAD     = 0x30;    // on *(model+0x3B70): the list's own head node
static constexpr size_t OFF_LIST_TAIL     = 0x38;    // ...points back at the head when EMPTY
static constexpr size_t OFF_NODE_NEXT     = 0x08;    // node -> next node (sentinel is at +0x30)
static constexpr size_t MOVEQ_WALK_CAP    = 32;      // a corrupt list must not spin the probe thread

// Walks the movement list and returns how many nodes are on it, or -1 if it could not be read.
// `verbose` prints each node; the watcher wants the count only.
static int walkMovementQueue(uintptr_t modeObj, bool verbose)
{
    if (!modeObj) return -1;

    const uintptr_t sentinel = modeObj + OFF_LIST_HEAD;
    uintptr_t node = 0;
    if (!readAt(modeObj + OFF_LIST_TAIL, node)) return -1;

    int n = 0;
    while (node && node != sentinel && n < (int)MOVEQ_WALK_CAP) {
        ++n;
        if (verbose) {
            uint8_t raw[0x20] = { 0 };
            char hex[112] = { 0 };
            if (safeRead((void*)node, raw, sizeof(raw))) {
                int w = 0;
                for (size_t b = 0; b < sizeof(raw); ++b)
                    w += _snprintf_s(hex + w, sizeof(hex) - w, _TRUNCATE, "%02X ", raw[b]);
            }
            logf("            node %d @ %016llX  %s", n, (unsigned long long)node,
                 hex[0] ? hex : "<unreadable>");
        }
        uintptr_t next = 0;
        if (!readAt(node + OFF_NODE_NEXT, next)) break;
        node = next;
    }
    if (n == (int)MOVEQ_WALK_CAP && verbose)
        logf("            (stopped at %zu — either a very long queue or a broken list)",
             MOVEQ_WALK_CAP);
    return n;
}
static constexpr size_t OFF_MODE_OBJ      = 0x3B70;  // campaign model: the middle clause's argument
static constexpr size_t OFF_FACTION_ID    = 0x08;    // faction: the field the predicate compares
static constexpr size_t OFF_FACTION_SKIP1 = 0xEE8;   // faction: the two flags the turn advance
static constexpr size_t OFF_FACTION_SKIP2 = 0xF68;   //          steps over when it round-robins

// ★ THE GAME'S OWN TURN NUMBER — the one on screen and the one bug reports are phrased in.
//
// Found from the Lua binding rather than guessed: the campaign-model table built by FUN_14011D4A0
// pairs the name string `turn_number` (@0x143472390) with `FUN_141642660`, which is eleven useful
// instructions:
//
//     MOV RAX,[RBX+0x18]      ; -> the campaign model
//     MOV RCX,[RAX+0x3B78]    ; -> the turn manager (NETCODE_NOTES' `mgr2`, already known)
//     MOV EBX,[RCX+0x5C]      ; the counter
//     LEA EDX,[RBX+1]         ; ...and Lua returns it PLUS ONE
//
// ⇒ `+0x5C` is ZERO-BASED and the displayed turn is `+0x5C + 1`. Worth being exact about, because
// the whole point of reading it is to line logs up with "it broke on turn 4".
static constexpr size_t OFF_TURN_MANAGER = 0x3B78;   // on the campaign model
static constexpr size_t OFF_TURN_COUNTER = 0x5C;     // zero-based; add 1 for the displayed number

// When the gate's clause (D) was last seen to shut, so the unblock key can refuse to fire on a
// block that has only just started — see clearTurnGateBlockList().
static uint64_t g_gateBlockedSince = 0;

// ============================================================================================
// ★★★★★ WHAT THE GUARD IS WAITING ON — the item vector at model+0x3D30 (§6qqq, 2026-08-06)
// ============================================================================================
//
// The vptr says what TYPE of guard is on the list. It does not say what that guard is waiting for,
// and for three sessions that was the whole gap: the log said `vptr=14345FC70` and nothing else, so
// B1 was a pointer nobody could act on. It was closed not by adding more logging but by reading the
// hung process from outside with `ReadProcessMemory` — and every value that mattered turned out to
// be a plain pointer walk this DLL is already inside the process for. So it prints them itself now.
//
// `FUN_1419D4C60` is the per-faction pending-event drain. When it has an entry to raise it does:
//
//     if (!blockedListNonEmpty(model + 0x3B88)) {      // only one guard outstanding at a time
//         FUN_14194AEC0(model + 0x3D30, entry);        // register the item in the vector
//         id = FUN_1414C8130(model + 0x3D30);          // == count - 1, the item's index
//         FUN_14141C930(faction, id);                  // push the guard carrying that index
//     }
//
// so the guard node's `+0x28` is an INDEX INTO THAT VECTOR, and the node's completion method
// (vtable `+0x40` = `0x141441DA0`) re-looks-up the record and does:
//
//     CMP dword ptr [RAX + 0x48], -1
//     JZ  ...                       ; -1 -> return, do nothing, STAY ON THE LIST
//
// ⇒ `record + 0x48` is the answer slot: **-1 = unanswered, 0 = answered**. A guard that never pops
// is a guard whose record was never answered, and the engine is behaving correctly when it refuses.
//
// The vector itself is plain: `count` at `+0x10C`, the pointer array at `+0x110`
// (`FUN_1414C6B40(coll,i) { return coll->array[i]; }`).
//
// ⚠⚠ THAT LOOKUP HAS NO BOUNDS CHECK. This probe does not get to inherit that: a bad index read out
// of a half-torn-down model would turn a diagnostic into a crash in the game we are diagnosing. The
// index is checked against `count` here, every time, before anything is indexed.
//
// ★ Cross-check that costs nothing: `record + 0x10` held the same faction pointer as the guard's
// `+0x20` in the live read. They are printed side by side so a mismatch is visible rather than
// assumed away.
static constexpr size_t OFF_ITEM_VEC       = 0x3D30;  // model: -> the pending-item vector
static constexpr size_t OFF_ITEM_COUNT     = 0x10C;   // vector: int count
static constexpr size_t OFF_ITEM_ARRAY     = 0x110;   // vector: -> array of record pointers
static constexpr size_t OFF_ITEM_FACTION   = 0x10;    // record: the faction, cross-checks node+0x20

// ★★★ `record + 0x48` IS THE CHOSEN OPTION INDEX, ZERO-BASED — not a flag.
//
// ✗✗ This was written as "-1 = unanswered, 0 = answered" from the broken state, before anything had
// been watched through a HEALTHY cycle. Corrected 2026-08-07 by answering a three-option dilemma
// with the THIRD option and reading **2** — and the player said so before the value was read, which
// is what makes it a measurement rather than a fit.
//
//     -1  no choice made yet
//      0  the first option was chosen ... N-1  the Nth
//
// ⚠⚠ AND -1 IS THE ORDINARY STATE, NOT A FAULT. Every dilemma reads -1 from the moment it is raised
// until the player clicks. Healthy holds of 112 s and 187 s have been measured. Wording that treats
// -1 as evidence of a defect is precisely the B13 error, and it must not be reintroduced here: what
// distinguishes B1 is that NOTHING WAS EVER PUT ON SCREEN, which this probe cannot see. It can say a
// decision is outstanding; only a human can say no box was offered.
static constexpr size_t OFF_ITEM_ANSWER    = 0x48;    // record: chosen option index, -1 = pending

// The record's own option count. Two independent vectors sit at +0x28 and +0x38, each {count, cap,
// ptr}, and both held the same value in every capture — 2 for a two-option dilemma, 3 for a
// three-option one, cross-checked against what was on screen. ⚠ count and capacity cannot be told
// apart while the vectors are exactly full, which they have been every time.
static constexpr size_t OFF_ITEM_OPTIONS   = 0x38;    // record: number of options offered

// ✗✗ NOT A B1 MARKER. It tracks the OPTION COUNT. Kept, and kept printed, so the correlation is
// not rediscovered and chased a second time.
//
// A pointer to a polymorphic object (vtable 0x1434A7A50, a second vptr at its +0x28). For a few
// hours on 2026-08-07 it looked like the thing that distinguishes "stuck" from "somebody is
// deciding" — which would have let the DLL detect B1 by itself and retired #43's rule that only a
// human can answer "is a box open on any screen?". The readings, in the order they arrived:
//
//     2-option, healthy pending, box on screen, SINGLE PLAYER   -> 0
//     2-option, healthy pending, box on screen, MP 3 machines   -> 0
//     2-option, answered                                        -> 0
//     3-option, STUCK (B1), no box anywhere, MP, twice          -> POPULATED   <- looked decisive
//     3-option, HEALTHY pending, box ON SCREEN, MP 3 machines   -> POPULATED   <- killed it
//
// ⇒ populated on a 3-option dilemma whether healthy or stuck; null on a 2-option one. The stated
// confound — every healthy sample was 2-option and both stuck ones 3-option — WAS the explanation.
//
// ⚠ The lesson is not "we were wrong", it is that the confound was named before the result was
// acted on, and the one sample that could settle it was taken instead of more of the same. Four
// readings of 0 added nothing; one 3-option healthy reading ended it in minutes.
//
// SUSPECTED, not chased: probably a target/character reference — the 3-option char_conflict family
// carries target_character_1/2 and the 2-option dilemmas seen so far do not.
static constexpr size_t OFF_ITEM_STATEOBJ  = 0x20;    // record: correlates with option count

// ⚠ A LOOSE CAP ON PURPOSE. The vector is append-only — §6qqq found no pop anywhere in the drain —
// so a long campaign legitimately accumulates entries, and a tight cap would refuse the real answer
// on the machine that finally reproduces this. 65536 still catches every shape of garbage the
// captures have actually produced (`count=168364553`, `ptr=09090909090A0909`) without ever refusing
// something plausible.
static constexpr int32_t ITEM_COUNT_MAX    = 65536;
static constexpr int32_t ITEM_PRINT_MAX    = 16;      // ...but only ever print a tail this long

// Reads the vector header. Returns false — and logs the refusal in the house idiom — if the numbers
// are not credible, because when they are not, the offsets are wrong rather than the game broken.
static bool readItemVector(uintptr_t model, uintptr_t& coll, int32_t& count, uintptr_t& arr)
{
    coll = 0; count = 0; arr = 0;
    if (!readAt(model + OFF_ITEM_VEC, coll) || !coll) {
        logf("      (D) item vector at model+0x3D30 = %016llX — did not read; not resolving the "
             "index.", (unsigned long long)coll);
        return false;
    }
    if (!readAt(coll + OFF_ITEM_COUNT, count) || !readAt(coll + OFF_ITEM_ARRAY, arr)) {
        logf("      (D) item vector %016llX: count/array unreadable — not resolving the index.",
             (unsigned long long)coll);
        return false;
    }
    // ⚠ EMPTY IS NOT INCREDIBLE. `count == 0 && ptr == 0` is exactly what a campaign in which no
    // decision has ever been raised looks like — a lazily-grown vector that has never been pushed
    // to — and a fresh single-player turn 1 hits it every time (measured 2026-08-13). The old test
    // lumped it in with the garbage cases and printed "treat the offsets as wrong", which is a
    // false alarm in the one place a false alarm is most expensive: a capture read at speed during
    // a session. The sibling probe below already words this correctly; match it.
    if (count == 0 && arr == 0) {
        logf("      (D) item vector %016llX is EMPTY (count=0, ptr=0) — no decision has been raised "
             "this campaign. Nothing to resolve, and nothing wrong.",
             (unsigned long long)coll);
        return false;
    }
    if (count < 0 || count > ITEM_COUNT_MAX || arr <= 0x10000) {
        logf("      (D) item vector %016llX reads count=%d ptr=%016llX — REFUSING to read further. "
             "Those numbers are not credible, so treat the offsets as wrong rather than the game as "
             "broken.",
             (unsigned long long)coll, count, (unsigned long long)arr);
        return false;
    }
    return true;
}

// One entry of the vector. `index` is checked against `count` by the caller AND here, because the
// engine's own accessor checks it nowhere and this is the read that would fault.
static bool readItemRecord(uintptr_t arr, int32_t count, int32_t index,
                           uintptr_t& rec, int32_t& answer, int32_t& options)
{
    rec = 0; answer = 0; options = -1;
    if (index < 0 || index >= count) return false;
    if (!readAt(arr + (uintptr_t)(uint32_t)index * 8, rec) || rec <= 0x10000) return false;
    readAt(rec + OFF_ITEM_OPTIONS, options);   // best-effort: absence must not fail the record
    return readAt(rec + OFF_ITEM_ANSWER, answer);
}

// Describes `+0x48` without editorialising. `-1` is a state, not a verdict: it is what every
// dilemma reads while its box is open, so this says a decision is OUTSTANDING and stops there.
// Whether that decision was ever offered to anyone is not something this process can see.
static void answerText(int32_t answer, int32_t options, char* out, size_t cap)
{
    if (answer == -1) {
        _snprintf_s(out, cap, _TRUNCATE, "no choice made yet — a decision is outstanding");
        return;
    }
    if (answer < 0) {
        // Not -1 and still negative: that is not a state we have a reading for. Say so.
        _snprintf_s(out, cap, _TRUNCATE, "%d — negative but not -1, never observed, not interpreting it",
                    answer);
        return;
    }
    if (options > 0 && answer < options)
        _snprintf_s(out, cap, _TRUNCATE, "answered: option %d of %d", answer + 1, options);
    else if (options > 0)
        _snprintf_s(out, cap, _TRUNCATE,
                    "answered: option index %d, but the record offers %d — ⚠ out of range", answer, options);
    else
        _snprintf_s(out, cap, _TRUNCATE, "answered: option index %d (0-based)", answer);
}

// Set by reportItemVector so a capture prints the vector exactly once — the guard path prints it
// with the waited-on row highlighted, and the capture prints it anyway when no guard is up. Cleared
// at the top of each capture. Single-threaded probe path; a stale true would only cost one listing.
static bool g_itemVectorPrinted = false;

// The whole item vector, one row per record. `highlight` marks the row a guard is waiting on, or -1.
//
// ★ Printed on EVERY capture, not only while a guard is up. The first version only ran from the
// guard path, so the moment a dilemma was ANSWERED the guard popped and nothing logged the result —
// `+0x48 = 1 (answered: option 2 of 2)` was correct in memory and absent from the log. A probe that
// can only describe the broken state cannot show what the healthy one looks like to compare against.
static void reportItemVector(uintptr_t model, int32_t highlight)
{
    uintptr_t coll = 0, arr = 0;
    int32_t   count = 0;
    if (!readItemVector(model, coll, count, arr)) return;

    g_itemVectorPrinted = true;

    if (count == 0) {
        logf("      item vector at model+0x3D30: EMPTY — no decision has been raised this session.");
        return;
    }

    const int32_t lo = (count > ITEM_PRINT_MAX) ? (count - ITEM_PRINT_MAX) : 0;
    logf("      item vector at model+0x3D30: %d item(s)%s", count,
         (lo > 0) ? "  (showing the last few)" : "");
    for (int32_t i = lo; i < count; ++i) {
        uintptr_t r = 0; int32_t a = 0, o = -1;
        if (!readItemRecord(arr, count, i, r, a, o)) {
            logf("          [%d] unreadable", i);
            continue;
        }
        uintptr_t f = 0, stateObj = 0; uint32_t fid = 0;
        char facTxt[24] = "faction ?";
        if (readAt(r + OFF_ITEM_FACTION, f) && f > 0x10000 && readAt(f + OFF_FACTION_ID, fid))
            _snprintf_s(facTxt, sizeof(facTxt), _TRUNCATE, "faction %u", fid);
        readAt(r + OFF_ITEM_STATEOBJ, stateObj);

        char t[160];
        answerText(a, o, t, sizeof(t));
        logf("          [%d] rec=%016llX %-11s options=%-2d +0x20=%016llX%s +0x48=%-3d %s%s",
             i, (unsigned long long)r, facTxt, o, (unsigned long long)stateObj,
             stateObj ? " <-- POPULATED" : "", a, t,
             (i == highlight) ? "   <== THIS NODE WAITS ON THIS ONE" : "");
    }
}

// ★★★ THE LINE THAT WOULD HAVE REPLACED THREE SESSIONS OF WORK.
//
// Given one guard node's stored index, name the record it waits on and say whether that record has
// been answered. Prints nothing and returns quietly when the vector is not readable — the caller has
// already printed the node itself, and a missing resolution must not look like a resolved "fine".
static void reportGuardItem(uintptr_t model, uint64_t vptr, int32_t index)
{
    uintptr_t coll = 0, arr = 0;
    int32_t   count = 0;
    if (!readItemVector(model, coll, count, arr)) return;

    if (index < 0 || index >= count) {
        logf("      ⚠ (D) node vptr=%016llX carries index %d, but the vector holds %d item(s) "
             "(valid 0..%d). REFUSING to index it — the engine's own FUN_1414C6B40 would not have, "
             "and that is exactly the bug we are not going to reproduce inside the probe.",
             (unsigned long long)vptr, index, count, count - 1);
        return;
    }

    uintptr_t rec = 0; int32_t answer = 0, options = -1;
    if (!readItemRecord(arr, count, index, rec, answer, options)) {
        logf("      (D) node vptr=%016llX waits on item %d of %d — record unreadable (arr=%016llX).",
             (unsigned long long)vptr, index, count, (unsigned long long)arr);
        return;
    }

    char what[160];
    answerText(answer, options, what, sizeof(what));
    logf("      ★ (D) node vptr=%016llX waits on item %d of %d — record %016llX  options=%d  "
         "+0x48 = %d (%s)",
         (unsigned long long)vptr, index, count, (unsigned long long)rec, options, answer, what);

    if (answer == -1)
        logf("        ⇒ A decision is outstanding, and the campaign is CORRECT to hold. This is the "
             "ordinary state while a dilemma box is open — healthy holds of 112 s and 187 s are on "
             "record. It is NOT evidence of a defect. **The question only a human can answer: is a "
             "box open on ANY player's screen?** A box somewhere ⇒ somebody is deciding, leave it "
             "alone. No box anywhere ⇒ that is B1.");
    else
        logf("        ⇒ ⚠ The record is ANSWERED and the node is STILL on the list. That is a "
             "different shape from B1 — write it down rather than filing it as one.");

    // The faction cross-check, and the rest of the vector so pending-vs-answered is visible at a
    // glance rather than needing a second capture to compare against. The id is what the logs and
    // the external probes speak in; the pointer alone cost a session's worth of guesswork.
    uintptr_t recFac = 0;
    if (readAt(rec + OFF_ITEM_FACTION, recFac)) {
        uint32_t facId = 0;
        const bool okId = recFac > 0x10000 && readAt(recFac + OFF_FACTION_ID, facId);
        if (okId)
            logf("        record+0x10 faction=%016llX  id=%u  (compare with the node's +0x20 above)",
                 (unsigned long long)recFac, facId);
        else
            logf("        record+0x10 faction=%016llX  (id unreadable; compare with node +0x20)",
                 (unsigned long long)recFac);
    }

    // The guard's own item is highlighted in the listing, which is printed once per capture.
    reportItemVector(model, index);
}

// ★★★ WALK THE BLOCKED LIST. This is the probe B1 has needed since it was named.
//
// Knowing (D) is a list rather than a timer only helps if we can see WHICH node is on it. Each of
// the ~19 node types has its own derived vtable, so the vptr is the type — and the type is the
// operation that started and never finished. One session with this running maps the vptrs of every
// healthy 0-27 s episode as a by-product, so the leaked one arrives already named.
//
// Returns the depth (0 = empty, -1 = unreadable). Prints one line per node when `verbose`.
//
// ⚠ Prints DEPTH, not just the head, on purpose: only the head node is ticked, so a guard buried
// under a newer push can never self-pop. A depth of 2+ while stuck is a different bug from a depth
// of 1, and the two would look identical from the head alone.
//
// ★ `next` is on the node line for the same reason DEPTH is: `next=0` says this is a LONE LEAK, and
// a non-zero one says there is a stack. The 2026-08-06 read had to establish that by hand.
//
// ★ The waited-on item is resolved for the HEAD node only. That is not a shortcut — the head's
// resolution prints the whole vector, so a buried node's `index=` on its own line can be read
// straight off that same table without a second walk of it.
static int walkBlockList(uintptr_t model, bool verbose)
{
    uintptr_t cell = 0;
    if (!readAt(model + OFF_GATE_D_LIST, cell) || !cell) return -1;

    uintptr_t node = 0;
    if (!readAt(cell, node)) return -1;
    if (!node) return 0;

    int n = 0;
    while (node > 0x10000 && n < (int)BLK_WALK_CAP) {
        uintptr_t vptr = 0, owner = 0, next = 0, faction = 0;
        uint32_t  countdown = 0;
        // ⚠ +0x28 is read as 4 bytes deliberately. `FUN_1414C8130` returns `count - 1` as an int and
        // the ctor stores that; whether the field is padded to 8 is not established, and the low
        // dword is the index either way on this little-endian target.
        int32_t   index = 0;
        const bool okV = readAt(node, vptr);
        readAt(node + OFF_BLK_OWNER,     owner);
        readAt(node + OFF_BLK_NEXT,      next);
        readAt(node + OFF_BLK_COUNTDOWN, countdown);
        readAt(node + OFF_BLK_FACTION,   faction);
        // ⚠ Kept, because a FAILED read leaves `index` at 0 and "waits on item 0" is a sentence this
        // project would then quote. An unread index has to say so, not default to a plausible one.
        const bool okIdx = readAt(node + OFF_BLK_INDEX, index);

        if (verbose) {
            char idxTxt[24];
            if (okIdx) _snprintf_s(idxTxt, sizeof(idxTxt), _TRUNCATE, "%d", index);
            else       _snprintf_s(idxTxt, sizeof(idxTxt), _TRUNCATE, "unreadable");
            logf("      blocked[%d] node=%016llX vptr=%016llX countdown=%u owner=%016llX "
                 "next=%016llX faction=%016llX index=%s%s%s%s",
                 n, (unsigned long long)node, (unsigned long long)vptr, countdown,
                 (unsigned long long)owner, (unsigned long long)next,
                 (unsigned long long)faction, idxTxt,
                 (owner && owner != cell) ? "  ⚠ owner is NOT this model's head cell" : "",
                 (n == 0 && !next) ? "  (LONE — nothing under it)" : "",
                 (n > 0) ? "  ⚠ BURIED — only the head is ticked, so this one cannot self-pop" : "");
        }

        // ★★★ The head node's index, resolved into the record it waits on. This is the whole point
        // of the walk; everything above is context.
        //
        // ⚠⚠ GATED ON THE VPTR, and that gate is the difference between a probe and a liar.
        // `+0x28` is an item index ONLY on the dilemma guard. On `14345FD90` nodes — which appear at
        // list head constantly during ordinary play — the same field read -172425216, 275, 51, 275
        // and 35. Resolving those would print "★ waits on item 275" whenever the junk happened to
        // fall inside the vector, i.e. a confident B1 report for a dilemma that does not exist. The
        // capture that pointed away from B1 for three sessions was exactly this class of mistake.
        const bool isDilemmaGuard = okV && (uintptr_t)vptr == g_base + RVA_VPTR_DILEMMA_GUARD;

        if (verbose && n == 0 && okV && okIdx) {
            if (isDilemmaGuard) {
                reportGuardItem(model, (uint64_t)vptr, index);
            } else {
                // Say why nothing was resolved. Silence here would read as "resolved, and fine".
                logf("      (head node is type %016llX, not the dilemma guard %016llX — +0x28 is "
                     "not an item index on this type, so it is NOT being resolved. This is normal: "
                     "most guards pop on their own within seconds.)",
                     (unsigned long long)vptr,
                     (unsigned long long)(g_base + RVA_VPTR_DILEMMA_GUARD));
            }
        }

        if (!okV) break;
        ++n;
        if (!next || next == node) break;
        node = next;
    }
    if (verbose && n == (int)BLK_WALK_CAP)
        logf("      (stopped after %d nodes — the list is longer than anything sane, or circular)",
             (int)BLK_WALK_CAP);
    return n;
}

// Returns the DISPLAYED turn number, or 0 if it cannot be read.
static uint32_t readTurnNumber(uintptr_t model)
{
    uintptr_t mgr = 0; uint32_t raw = 0;
    if (!readAt(model + OFF_TURN_MANAGER, mgr) || !mgr) return 0;
    if (!readAt(mgr + OFF_TURN_COUNTER, raw)) return 0;
    if (raw > 100000) return 0;                       // implausible => we are reading the wrong thing
    return raw + 1;
}

// The phase names are read out of the campaign tick's behaviour (`FUN_1419EEF90` drives 0→1 once
// battles resolve; at 2 it either advances via `FUN_14199BBB0` or goes to 3 for a human, and the
// advance `FUN_1419B4FD0` clears it to 0). ⚠ Only phase 1 is *named* by anything — it is the value
// the predicate requires. The other three are described from what drives them, not from a string.
static const char* turnPhaseName(uint32_t p)
{
    switch (p) {
        case 0: return "just advanced / reset";
        case 1: return "ACTING — the only phase the predicate permits action in";
        case 2: return "end of turn: advance or hand to a human";
        case 3: return "waiting on a human";
        default: return "unexpected — not a value the tick produces";
    }
}

// Index of a faction in the turn-order array, or -1. This is the value to compare across machines.
static int factionIndex(uintptr_t fac, uintptr_t facPtr, uint32_t facCount)
{
    if (!fac || !facPtr || facCount == 0 || facCount > 512) return -1;
    for (uint32_t i = 0; i < facCount; ++i) {
        uintptr_t p = 0;
        if (readAt(facPtr + (uintptr_t)i * 8, p) && p == fac) return (int)i;
    }
    return -1;
}

// One line per faction of interest: everything the turn advance and the predicate consult about it.
static void describeFaction(const char* label, uintptr_t fac, uintptr_t facPtr, uint32_t facCount)
{
    if (!fac) { logf("  %-16s (null)", label); return; }

    uint64_t id = 0; uint8_t human = 0, skip1 = 0, skip2 = 0;
    readAt(fac + OFF_FACTION_ID, id);
    readAt(fac + OFF_FACTION_IS_HUMAN, human);
    readAt(fac + OFF_FACTION_SKIP1, skip1);
    readAt(fac + OFF_FACTION_SKIP2, skip2);

    logf("  %-16s ptr=%016llX  id(+0x8)=%08X  arrayIndex=%d  isHuman(+0xCD0)=%u  "
         "skip(+0xEE8)=%u skip(+0xF68)=%u%s",
         label, (unsigned long long)fac, (unsigned)(id & 0xFFFFFFFF),
         factionIndex(fac, facPtr, facCount), human, skip1, skip2,
         (skip1 || skip2) ? "   <-- THE TURN ADVANCE STEPS OVER THIS FACTION" : "");
}

// ---------------------------------------- #63: the REAL human-faction registry, and its two cursors
//
// WHY THIS EXISTS. `dumpHumanFactionVector()` above reads `container+0x180/0x184/0x188` and has
// returned `cap=0 count=0 ptr=NULL` in every campaign it has ever seen — single player and live
// 2-player coop alike. That reading was correct and the conclusion drawn from it ("vestigial") was
// correct; the mistake was leaving it as the only thing in the capture called "human factions",
// because the list the engine actually consults is somewhere else entirely:
//
//     mgr = *(model + 0x3D30);   HUMAN_FACTIONS = { cap +0x228, count +0x22C, data +0x230 }
//
// B1 is `FUN_1414E0E40` searching that array over `[0, count)` and, additionally, requiring the
// found index to be `< 2`. Three humans really are registered — the dumps show three valid faction
// pointers in a cap-4 array — and `count` reads 2, so the third is never examined.
//
// ⚠⚠ AND THIS PROBE IS READ-ONLY ON PURPOSE. Raising `count` here would be a wild write: the
// per-faction cursor array at `+0x1F0` has room for exactly TWO entries and `+0x1F8` is the size of
// a live intrusive list, while `FUN_14149F350` (end-turn path) writes `cursor[idx]` with no bounds
// check at all. Cursors 2+ need storage we own before anything raises the count. §6uuu.22.
//
// What we want out of a session, in one line per capture:
//   * does `count` EVER read 3 — at lobby, at campaign load, and across turns
//   * do the entries past `count` hold real factions (they did in the B1 dumps)
//   * do the two cursors move, and does anything ever disturb `+0x1F8`
static void dumpEventMgrRegistry()
{
    logf("=== HUMAN_FACTIONS registry (*(model+0x3D30)) — the list B1 turns on ===");

    uintptr_t root = 0, obj = 0, model = 0, mgr = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) {
        logf("  not in a campaign (root is 0) — nothing to read.");
        return;
    }
    if (!readAt(root + OFF_ROOT_OBJ, obj)  || !obj)   { logf("  chain broke at +0x2188"); return; }
    if (!readAt(obj  + OFF_MODEL,    model)|| !model) { logf("  chain broke at +0x78");   return; }
    if (!readAt(model + OFF_MODEL_EVENT_MGR, mgr) || !mgr) {
        logf("  chain broke at model+0x3D30 (event manager not built yet)");
        return;
    }
    logf("  mgr = %016llX", (unsigned long long)mgr);

    uint32_t cap = 0, count = 0; uintptr_t data = 0;
    const bool okCap   = readAt(mgr + OFF_EVMGR_HF_CAP,   cap);
    const bool okCount = readAt(mgr + OFF_EVMGR_HF_COUNT, count);
    const bool okData  = readAt(mgr + OFF_EVMGR_HF_PTR,   data);
    if (!okCap || !okCount || !okData) { logf("  registry fields unreadable — offsets are wrong, "
                                              "not the game. Stopping."); return; }

    // A value that cannot be true is reported as such, and the walk stops.
    if (cap > 64 || count > cap) {
        logf("  >>> cap=%u count=%u data=%016llX — THAT CANNOT BE TRUE for a {cap,count,ptr} "
             "vector. Offsets are wrong, not the game. Stopping.",
             cap, count, (unsigned long long)data);
        return;
    }
    logf("  >>> HUMAN_FACTIONS: cap(+0x228)=%u  count(+0x22C)=%u  data(+0x230)=%016llX",
         cap, count, (unsigned long long)data);

    // Walk the whole CAPACITY, not just the count: the entire point is that entries past `count`
    // held real, registered factions in both B1 dumps. Anything found there is the finding.
    uint32_t liveBeyondCount = 0;
    // ★ 4-PLAYER PREP (2026-08-15): remember every VALID entry, so the analysis at the end can ask
    // the question a 3-human registry never had to — whether two humans are sharing one slot.
    uintptr_t seen[16] = { 0 };
    uint32_t  nSeen = 0;
    if (data) {
        for (uint32_t i = 0; i < cap && i < 16; ++i) {
            uintptr_t fac = 0;
            if (!readAt(data + (uintptr_t)i * 8, fac)) { logf("    [%u] <unreadable>", i); continue; }
            if (!fac) { logf("    [%u] (null)%s", i, i >= count ? "   past count" : ""); continue; }
            uint64_t id = 0; uint8_t human = 0;
            const bool okId    = readAt(fac + OFF_FACTION_ID, id);
            const bool okHuman = readAt(fac + OFF_FACTION_IS_HUMAN, human);
            // ⚠⚠ `human == 1`, NOT `human != 0`, and the second attempt is why this comment is long.
            // Slot [3] of a cap-4 registry is uninitialised heap. `fac > 0x10000` passed it, `+0x8`
            // read, and `human != 0` ALSO passed because the garbage byte read **84** on one machine
            // and **144** on another. A plausibility test applied to garbage is not a test. The flag
            // is a bool the engine only ever writes 0 or 1 to, so demand exactly that — and note the
            // count hold uses a stronger check still (membership in the all-factions array), because
            // it WRITES and this only prints. Measured across three machines, 2026-08-13.
            const bool looksReal = okId && okHuman && human == 1 && fac > 0x10000;
            if (looksReal && i >= count) ++liveBeyondCount;
            if (looksReal && nSeen < 16) seen[nSeen++] = fac;
            logf("    [%u] faction=%016llX  id(+0x8)=%08X  isHuman(+0xCD0)=%u%s%s",
                 i, (unsigned long long)fac, (unsigned)(id & 0xFFFFFFFF), human,
                 i >= count ? "   <-- PAST count" : "",
                 (looksReal && i >= count) ? " — REGISTERED BUT UNREACHABLE (this is B1)" : "");
        }
    }
    if (liveBeyondCount)
        logf("  >>> %u faction(s) are registered past the count. B1 is live in this campaign.",
             liveBeyondCount);
    else if (count >= 3)
        logf("  >>> count is %u — THE CLAMP DID NOT HAPPEN THIS TIME. Capture everything.", count);

    // The two cursors, and the live field immediately after them. `listSize` is printed so that a
    // build which ever does raise the count has a witness for the corruption: it belongs to the
    // list at +0x200 and has no business changing when a turn ends.
    uint32_t cur[EVMGR_CURSOR_SLOTS] = { 0 }, listSize = 0;
    for (uint32_t i = 0; i < EVMGR_CURSOR_SLOTS; ++i)
        readAt(mgr + OFF_EVMGR_CURSOR0 + i * 4, cur[i]);
    readAt(mgr + OFF_EVMGR_LIST_SIZE, listSize);
    logf("  cursors(+0x1F0): [0]=%u [1]=%u   |   listSize(+0x1F8)=%u  <-- NOT a third cursor; it is "
         "the size of the list at +0x200, and it is what cursor[2] would land on",
         cur[0], cur[1], listSize);

    // Independent cross-check, deliberately from a different mechanism: the per-faction flag. If
    // this disagrees with `count`, the disagreement is the whole bug in one line.
    uintptr_t cont = 0; uint32_t facCount = 0; uintptr_t facPtr = 0;
    uint32_t  humans = 0;
    uintptr_t humanFac[16] = { 0 };
    uint32_t  nHuman = 0;
    if (readAt(model + OFF_CONTAINER, cont) && cont &&
        readAt(cont + OFF_FACTIONS_COUNT, facCount) && readAt(cont + OFF_FACTIONS_PTR, facPtr) &&
        facPtr && facCount && facCount <= 512) {
        for (uint32_t i = 0; i < facCount; ++i) {
            uintptr_t fac = 0; uint8_t human = 0;
            if (!readAt(facPtr + (uintptr_t)i * 8, fac) || !fac) continue;
            // ⚠ Deliberately `human` truthy here, NOT `human == 1` as the slot walk above demands.
            // These come out of the all-factions array and are real objects, so the flag is a real
            // bool — and keeping the test identical to every historical log is what makes this
            // number comparable across captures. The strict test belongs where garbage can appear.
            if (readAt(fac + OFF_FACTION_IS_HUMAN, human) && human) {
                ++humans;
                if (nHuman < 16) humanFac[nHuman++] = fac;
            }
        }
        logf("  CROSS-CHECK: %u faction(s) carry isHuman(+0xCD0), registry count says %u%s",
             humans, count,
             humans > count ? "   <-- MISMATCH: humans the campaign cannot reach" : "");
    }

    // ★★★ THE FOUR-PLAYER ANALYSIS — added 2026-08-15, BEFORE the first 4-human capture exists.
    //
    // Every registry ever recorded held at most three humans (cap never exceeded 4), so none of
    // this has been exercised. It is written now, in advance, because the failure it looks for is
    // one that a raw dump does NOT make obvious: the push writes at data[count] and the rollback
    // leaves `count` where it was, so a fourth human would land on the SAME slot the third used and
    // the third would disappear with no error logged anywhere. Three valid entries alongside four
    // humans looks almost exactly like a healthy three-player capture.
    //
    // ⚠ That model is INFERRED (§6uuu.27 asserts the push shares FUN_1414D0780's idiom; the push
    // itself is still unlocated), which is precisely why the outcomes are enumerated here instead
    // of being reasoned out afterwards from whatever the numbers happen to be.

    uint32_t dupes = 0;
    for (uint32_t i = 0; i < nSeen; ++i)
        for (uint32_t j = i + 1; j < nSeen; ++j)
            if (seen[i] == seen[j]) {
                ++dupes;
                logf("  >>> DUPLICATE SLOT: entries %u and %u both hold faction %016llX — two "
                     "pushes landed on one slot.", i, j, (unsigned long long)seen[i]);
            }

    uint32_t missing = 0;
    for (uint32_t h = 0; h < nHuman; ++h) {
        bool found = false;
        for (uint32_t s = 0; s < nSeen && !found; ++s) if (seen[s] == humanFac[h]) found = true;
        if (found) continue;
        ++missing;
        uint64_t mid = 0; readAt(humanFac[h] + OFF_FACTION_ID, mid);
        logf("  >>> ABSENT FROM THE REGISTRY: faction %016llX id(+0x8)=%08X carries isHuman but "
             "occupies no slot up to cap. Nothing can ever be collected for this player.",
             (unsigned long long)humanFac[h], (unsigned)(mid & 0xFFFFFFFF));
    }

    logf("  >>> slots holding a real faction: %u   duplicates: %u   humans absent: %u",
         nSeen, dupes, missing);

    if (humans >= 4) {
        logf("  >>> ★★★ FOUR-HUMAN CAPTURE — the one every earlier session lacked. Reading:");
        if (dupes)
            logf("      ✗ OVERWRITE CONFIRMED. The push reuses data[count] while the rollback pins "
                 "the count, so humans past the second share a slot. THE FIX BELONGS AT "
                 "REGISTRATION: neither the reader patch nor a count raise can recover a faction "
                 "that was never stored.");
        else if (missing)
            logf("      ✗ %u HUMAN(S) NEVER STORED, and no duplicate slot — so this is not the "
                 "overwrite, it is the push declining them outright. Different mechanism, same "
                 "conclusion: the fix belongs at registration.", missing);
        else
            logf("      ✓ STORAGE IS SOUND — every human holds its own slot. The count is then the "
                 "only defect, and the reader fix plus a count advance are sufficient.");
    } else if (nHuman) {
        logf("  >>> only %u human(s) here, so the four-player question is still open. The capture "
             "that settles it needs FOUR humans in one campaign.", humans);
    }

    logf("=== end HUMAN_FACTIONS registry ===");
}

// ★★★ THE READ tester'S AUTOSAVE OBSERVATION ASKS FOR (2026-08-04, confirmed by him in a vanilla
// 2-player run): the autosave happens when the PREVIOUS player ends their turn, *before* the turn
// moves to the next player — who is then the one that locks.
//
// ⇒ Pair that with clause (B) and there is a chain worth measuring rather than arguing:
//
//     a movement is still in flight  →  the autosave serialises the campaign model with it
//     →  the completion never arrives after the restore  →  the list is occupied forever
//     →  the next player can never move an army and never end a turn
//
// It would also explain why the *save* is unusable rather than just the session, why the stuck
// player is always the one AFTER the save, and why remote peers matter: a peer whose movement is
// still travelling is exactly the one caught mid-flight when the save barrier lands.
//
// ⚠ FIRST SESSION WITH THIS BUILD, AND IT FOUND A HOLE IN THE MEASUREMENT (23:18, three players):
// `CCQ_SAVE_CAMPAIGN_GAME_MULTIPLAYER` executed **zero** times, and every
// `CCQ_PENDING_BATTLE_PLAYER_READY_TO_SAVE_GAME` triple follows a `PRE-BATTLE ROLE` block by 0.4 s —
// they are the *battle* save handshake, not the turn autosave. ⇒ **The turn autosave is not a queued
// command at all**, so watching the command stream could never catch the moment tester named.
//
// ★★★ It is a FLAG, and it is readable (§6ppp). The Lua binding `autosave_at_next_opportunity`
// (@0x143471868) pairs with `FUN_1415940D0`, which calls `FUN_1414C84D0(model + 0x3B38)`, which is
// one line: `*(*(model+0x3B38)) + 0x19E = 1`. And `*(model + 0x3B38)` is the campaign root object —
// the same one this file already reaches as `root + 0x2188` (it carries `+0x78` = model and
// `+0x1A8` = local faction).
//
//     autosavePending = *(u8*)(rootObj + 0x19E)
//
// ★ The other setter, `FUN_1414C9500`, says WHEN it is requested, and it matches tester's description
// of the sequence exactly:
//
//     if (rootObj->0x138 == 0
//      && container->0x48->id == rootObj->0x1A8->id     ← the turn has arrived at MY faction
//      && (autosaves are enabled))
//          rootObj->0x19E = 1;
//
// ⇒ The autosave is requested **at the handover**, on the machine the turn just reached — which is
// the player who then locks.
//
// So: how deep is the movement queue at the moment a save is requested? Two integers, at a known
// moment. Callable from the command hook (game thread) — guarded reads, no allocation.
int movementsInFlight()
{
    uintptr_t root = 0, obj = 0, model = 0, mode = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) return -1;
    if (!readAt(root + OFF_ROOT_OBJ, obj)   || !obj)   return -1;
    if (!readAt(obj + OFF_MODEL, model)     || !model) return -1;
    if (!readAt(model + OFF_MODE_OBJ, mode) || !mode)  return -1;
    return walkMovementQueue(mode, false);
}

// ★★★★★ THE UNBLOCK — B1's recovery, proven in game on 2026-08-05.
//
// tester, from a live stuck session: clearing the timer on ONLY the blocked client let that client
// act and **desynced the session** — recovered, but a real desync. Then every machine cleared it and
// *"we were able to end the turn successfully, also move armies."*
//
// ⇒ Two things are established by that, and the second is as important as the first:
//   1. Clause (D) IS the block. Zeroing it restores play, and the control reading (0 while healthy,
//      set while stuck, one turn apart on the same process) rules out coincidence.
//   2. (D) gates the SIMULATION, not the presentation. An unblocked client generates commands the
//      gated ones do not, and the campaigns diverge. ⇒ **It must be cleared on every machine or on
//      none.** That is not a style preference; doing it on one desynced a live game.
//
// ⚠ THIS IS A RECOVERY, NOT A FIX. What pushes the node that never pops is still unknown, so this
// treats the symptom. It is worth having anyway: it turns a dead session into a playable one, and
// the alternative was running a PowerShell script on three machines mid-game.
//
// ⚠⚠ **AND IT IS BLUNTER THAN IT LOOKS, now that (D) is known to be a LIST.** Zeroing the head cell
// does not pop anything — it ORPHANS the whole chain. Each orphaned node still runs its destructor
// later (it takes the "not found" early-out in `FUN_14140C500` and destroys itself anyway), so this
// does not corrupt the heap; but whatever operation each node was guarding is silently abandoned.
// With a depth of 1 that is one abandoned operation. With a depth of 2 it is more, and the log says
// which, because `walkBlockList` prints the chain before anything is written.
//
// ✗✗ **THE 20-SECOND SAFETY RULE WAS WRONG, and it was wrong in the dangerous direction.** It read
// "a real save or battle transition holds it for far less than 20 s" — asserted, never measured.
// Measured, in the very logs that named B1: the naming session recorded **HEALTHY** holds of 17 s
// and 27 s, on both machines simultaneously, that cleared by themselves. So `F7` pressed at second
// 21 of a legitimate operation would have cut it short — precisely the desync the rule exists to
// prevent. The threshold is now 60 s.
// ★ The real rule wants to be "the head node has not changed in N seconds" rather than a wall
// clock, and that is now buildable because the head is readable. Not done yet; the threshold move
// is the part that stops the rule from causing the harm it was written to avoid.
static constexpr uint64_t kUnblockMinHeldMs = 60000;

void clearTurnGateBlockList(bool force)
{
    uintptr_t root = 0, obj = 0, model = 0, cell = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) {
        logf("UNBLOCK: not in a campaign — nothing to clear.");
        return;
    }
    if (!readAt(root + OFF_ROOT_OBJ, obj) || !obj ||
        !readAt(obj + OFF_MODEL, model)   || !model ||
        !readAt(model + OFF_GATE_D_LIST, cell) || !cell) {
        logf("UNBLOCK: the chain to model+0x3B88 did not read — refusing to write anything.");
        return;
    }

    uint64_t val = 0;
    if (!readAt(cell, val)) {
        logf("UNBLOCK: the blocked-list head cell at %016llX is unreadable — refusing to write.",
             (unsigned long long)cell);
        return;
    }
    if (val == 0) {
        logf("UNBLOCK: clause (D) is already clear (the blocked list is EMPTY). Nothing written. If "
             "the game is still refusing, the block is NOT this clause — press F3 and read the "
             "other clauses.");
        return;
    }

    const uint64_t heldMs = g_gateBlockedSince ? (GetTickCount64() - g_gateBlockedSince) : 0;
    if (!force && g_gateBlockedSince && heldMs < kUnblockMinHeldMs) {
        logf("UNBLOCK: REFUSED — the list has only been non-empty for %llu ms. ⚠ Healthy holds of "
             "17 s AND 27 s were MEASURED on both machines in the session that named B1, so a "
             "short one is very likely a real operation in progress and cutting it short is how a "
             "desync starts. Wait past %llu s, or press Ctrl+F7 to force it.",
             (unsigned long long)heldMs, (unsigned long long)(kUnblockMinHeldMs / 1000));
        return;
    }

    // Print the chain BEFORE writing. This is the only moment the leaked node is guaranteed to
    // still be readable, and its vptr is its type — which is the operation that never finished.
    logf("UNBLOCK: the blocked list, as it stands right now:");
    const int depth = walkBlockList(model, true);
    if (depth > 1)
        logf("      ⚠ DEPTH %d. Only the HEAD node is ticked, so the ones under it could never have "
             "self-popped whatever happens. Clearing abandons all %d.", depth, depth);

    logf("################################################################");
    logf("★★★ UNBLOCK: emptying the turn gate's blocked list. Head cell %016llX held node %016llX; "
         "the list has been non-empty for %llu s.",
         (unsigned long long)cell, (unsigned long long)val,
         (unsigned long long)(heldMs / 1000));

    bool wrote = false;
    __try { *(volatile uint64_t*)cell = 0; wrote = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { wrote = false; }

    uint64_t after = 1;
    readAt(cell, after);

    if (!wrote) {
        logf("    the write FAULTED. Nothing changed.");
    } else if (after == 0) {
        logf("    cleared. This client can act again.");
        logf("    ⚠⚠ EVERY MACHINE MUST DO THIS, NOW. Clearing it on one client only has already");
        logf("       desynced a live session (2026-08-05): the unblocked client acts, the gated ones");
        logf("       cannot, and the campaigns diverge. All of you, or none of you.");
        logf("    ⚠ The node(s) above are now ORPHANED, not popped. Each still destroys itself later");
        logf("      (it takes the not-found early-out in FUN_14140C500), so nothing corrupts — but");
        logf("      whatever operation each was guarding is abandoned rather than completed.");
    } else {
        logf("    the write reported success but the head reads back %016llX — something is PUSHING "
             "again immediately. That is a finding in itself: the node is not merely left behind, "
             "something is still producing them.", (unsigned long long)after);
    }
    logf("################################################################");
}

// ★★★★★ PLAN B STAGE 2 — ANSWER the dilemma rather than discard it (#43).
//
// `clearTurnGateBlockList` above is the blunt instrument: it orphans the guard, the decision is
// abandoned, `+0x48` stays -1 forever and rides through saves, and it MUST be issued on every
// machine or the campaigns diverge. This is the surgical one. It submits the choice the player never
// got to make, through the exact path a mouse click takes.
//
// ★★★ THE PROPERTY THAT MAKES IT SAFE, and it is the opposite of `unblock`'s: the wire payload is
// ONLY the option index. No record id, no node pointer. Each receiver locates its own pending
// dilemma and pops its own guard through the engine's destructor. ⇒ ONE MACHINE SUBMITS AND EVERY
// MACHINE RESOLVES. There is no "every machine or none" rule here, because there is nothing to keep
// in step — the engine was always going to do this itself when somebody clicked.
//
// Read out of `FUN_142FA0D50` = `UIDLL::function_named_MakeDilemmaChoice_global_context_function`,
// the UI/Lua entry, with two things dropped: its Lua argument checking, and its string→index lookup
// (it is handed a choice KEY and walks the option list matching `option+0x30` to find the index —
// we already have the index, so we skip straight to the submit it ends in).
//
// ⚠⚠ GAME THREAD ONLY. `FUN_142F25B60` appends to the campaign command queue, bumping a non-atomic
// cursor at `queue+0x5008` and calling the payload's serialiser through its vtable. Doing that from
// the probe thread races the campaign tick. This is the same rule that put F8's faction message in a
// hook (session 5i) — so the key/command only QUEUES, and the drain runs from the command executor
// hook, which is called from the campaign tick every frame. Nothing is submitted anywhere else.
typedef uintptr_t (*GuardHeadFn)(uintptr_t);
typedef uintptr_t (*GuardNextFn)(uintptr_t);
typedef char      (*GuardIsTypeFn)(uintptr_t, const void*);
typedef uintptr_t (*GuardRecordFn)(uintptr_t);
typedef void*     (*MakeChoiceFn)(void*, uint32_t);
typedef uint8_t   (*SubmitFn)(uintptr_t, void*);

static volatile long g_answerRequest = -1;   // queued option index; -1 = nothing pending
static volatile long g_inAnswerDrain = 0;    // re-entrancy brake (the 5n lesson: two brakes, always)

// ★★ THE RECORD THE REQUEST WAS AIMED AT, captured when it is queued.
//
// A request is validated when it DRAINS, not when it is made, and those are different moments —
// ~35 ms apart in the measured case, but the gap is real. Without this, `answer 2` aimed at the
// dilemma on screen could land on a DIFFERENT dilemma that arrived in between, and answer with an
// option nobody chose for it. The drain refuses unless the pending record is still the same object.
// ⚠ Aligned 64-bit reads/writes are atomic on x64, so a plain volatile is sufficient here; it is
// only ever written before the request is published and read after it is claimed.
static volatile uintptr_t g_answerRecord = 0;

// Each engine call gets its own SEH wrapper. These stay free of C++ objects on purpose — MSVC
// refuses `__try` in any function that would need object unwinding, which cost a build in 5n.
static uintptr_t callGuardHead(uintptr_t model)
{
    uintptr_t r = 0;
    __try { r = ((GuardHeadFn)(g_base + RVA_GUARD_LIST_HEAD))(model + OFF_GUARD_LIST_ARG); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}
static uintptr_t callGuardNext(uintptr_t node)
{
    uintptr_t r = 0;
    __try { r = ((GuardNextFn)(g_base + RVA_GUARD_NEXT))(node); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}
static int callGuardIsDilemma(uintptr_t node)
{
    int r = -1;   // -1 = the call itself faulted, which is not the same as "no"
    __try {
        r = ((GuardIsTypeFn)(g_base + RVA_GUARD_IS_TYPE))(
                node, (const void*)(g_base + RVA_DILEMMA_TOKEN)) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = -1; }
    return r;
}
static uintptr_t callGuardRecord(uintptr_t node)
{
    uintptr_t r = 0;
    __try { r = ((GuardRecordFn)(g_base + RVA_GUARD_RECORD))(node); }
    __except (EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}

// Everything the decision needs, read before anything is written. POD by design.
struct DilemmaView {
    uintptr_t root, obj, model, node, rec;
    uint64_t  vptr;
    int32_t   itemIndex, options, answer, recOptions;
    uint64_t  myFactionId, recFactionId;
    bool      haveFactions, owned;
};

// Locates the pending dilemma using NOTHING BUT MEMORY READS — no engine calls at all.
//
// ★ Deliberate, and it is the safer of the two available searches. The engine's own path
// (`FUN_14140A920` → `FUN_140A658B0` → `FUN_140322EA0`, kept in offsets.h) would be more faithful,
// but calling engine code from the probe thread is the mistake this project keeps writing down and
// then making: sessions 5i and 5k both ended with work queued to the game thread for exactly this
// reason. This walk uses the chain the probes have read across four reproductions of B1 on three
// machines — `*(*(model+0x3B88))`, `next` at `+0x10`, gated on the FC70 vptr — so it is both safe on
// any thread AND better evidenced than the engine path.
//
// ⚠ Both option counts are read here as ordinary fields of the record. `FUN_141429440` is NOT the
// way to get them — it returns the record itself, so calling it would only re-derive what we have.
// The drain calls it anyway, once, purely to cross-check that derivation.
static bool resolveDilemma(DilemmaView& v, char* why, size_t whySz)
{
    memset(&v, 0, sizeof(v));
    v.itemIndex = -1; v.options = -1; v.answer = 0; v.recOptions = -1;

    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, v.root) || !v.root) {
        _snprintf_s(why, whySz, _TRUNCATE, "not in a campaign");
        return false;
    }
    if (!readAt(v.root + OFF_ROOT_OBJ, v.obj) || !v.obj ||
        !readAt(v.obj + OFF_MODEL, v.model)   || !v.model) {
        _snprintf_s(why, whySz, _TRUNCATE, "the chain to the campaign model did not read");
        return false;
    }

    uintptr_t cell = 0, node = 0;
    if (!readAt(v.model + OFF_GATE_D_LIST, cell) || !cell || !readAt(cell, node)) {
        _snprintf_s(why, whySz, _TRUNCATE, "the (D) guard list head cell did not read");
        return false;
    }

    int hops = 0;
    while (node > 0x10000 && hops < (int)BLK_WALK_CAP) {
        uint64_t vptr = 0;
        if (!readAt(node, vptr)) break;
        if (vptr == (uint64_t)(g_base + RVA_VPTR_DILEMMA_GUARD)) { v.vptr = vptr; break; }
        uintptr_t next = 0;
        if (!readAt(node + OFF_BLK_NEXT, next) || !next || next == node) { node = 0; break; }
        node = next;
        ++hops;
    }
    if (node <= 0x10000 || !v.vptr) {
        _snprintf_s(why, whySz, _TRUNCATE,
                    "no dilemma guard is pending (walked %d node(s); the other guard types pop on "
                    "their own within seconds)", hops + 1);
        return false;
    }
    v.node = node;                // selected ON the vptr, so no separate "does it agree" flag

    readAt(v.node + OFF_BLK_INDEX, v.itemIndex);

    // The record, for the owner test and for reporting what is actually being answered.
    uintptr_t coll = 0, arr = 0; int32_t count = 0;
    if (readAt(v.model + OFF_ITEM_VEC, coll) && coll &&
        readAt(coll + OFF_ITEM_COUNT, count) && readAt(coll + OFF_ITEM_ARRAY, arr) &&
        count > 0 && count <= ITEM_COUNT_MAX && arr > 0x10000) {
        readItemRecord(arr, count, v.itemIndex, v.rec, v.answer, v.recOptions);
    }
    // The count the submit path bounds against — a plain field of the record, so it is available
    // here rather than only at submit time.
    if (v.rec) readAt(v.rec + OFF_ITEM_OPT_SUBMIT, v.options);

    // Owner: the record's faction against this machine's own. Same test the DLL already applies
    // before sending `0xCD`.
    uintptr_t myFac = 0, recFac = 0;
    if (readAt(v.obj + OFF_LOCAL_FACTION, myFac) && myFac &&
        v.rec && readAt(v.rec + OFF_ITEM_FACTION, recFac) && recFac) {
        uint64_t a = 0, b = 0;
        if (readAt(myFac + OFF_FACTION_ID, a) && readAt(recFac + OFF_FACTION_ID, b)) {
            v.myFactionId  = a & 0xFFFFFFFF;
            v.recFactionId = b & 0xFFFFFFFF;
            v.haveFactions = true;
            v.owned = (v.myFactionId == v.recFactionId);
        }
    }
    return true;
}

// ★★★ STAGE 3 — print the option KEYS, so nobody answers blind.
//
// Pure reads, so this is safe on the probe thread and needs no engine call. Chain and evidence are
// documented beside the offsets; the short version is `record+0x40` is an array of option pointers
// bounded by `record+0x3C`, and each option embeds a CA string holding its key.
//
// A key looks like `3k_cp01_char_historical_yue_jin_joins_pc_dilemmaFIRST` — the choice is named in
// it, which is the whole point: `answer 2` stops being a coin flip.
//
// ⚠ Prints nothing rather than something wrong. Every failure here is silent-with-a-reason: a bad
// count, an unreadable array, a length that is not credible. A confidently-wrong option label is
// worse than none at all, because it would be *acted* on.
static void reportOptionKeys(const DilemmaView& v)
{
    if (!v.rec) return;

    int32_t   count = 0;
    uintptr_t arr   = 0;
    if (!readAt(v.rec + OFF_ITEM_OPT_COUNT, count) || !readAt(v.rec + OFF_ITEM_OPT_ARRAY, arr))
        return;
    if (count <= 0 || count > 64 || arr <= 0x10000) {
        logf("    (option keys: count=%d array=%016llX — not credible, so not resolving them)",
             count, (unsigned long long)arr);
        return;
    }
    // The engine bounds the SUBMIT on this count. If it disagrees with the one `answer` uses, say
    // so — two counts on the same record that differ is a finding, not a rounding error.
    if (v.options > 0 && count != v.options)
        logf("    ⚠⚠ option count disagreement: +0x3C says %d, +0x2C says %d. `answer` bounds on "
             "+0x2C — report this.", count, v.options);

    for (int32_t i = 0; i < count; ++i) {
        uintptr_t opt = 0;
        if (!readAt(arr + (uintptr_t)i * 8, opt) || opt <= 0x10000) {
            logf("      option %d: <unreadable>", i + 1);
            continue;
        }
        uint32_t  len = 0;
        uintptr_t txt = 0;
        if (!readAt(opt + OFF_OPTION_KEY_LEN, len) || !readAt(opt + OFF_OPTION_KEY_PTR, txt) ||
            len == 0 || len > MAX_OPTION_KEY || txt <= 0x10000) {
            logf("      option %d: key unreadable (len=%u ptr=%016llX)",
                 i + 1, len, (unsigned long long)txt);
            continue;
        }
        char key[MAX_OPTION_KEY + 1];
        if (!safeRead((void*)txt, key, len)) { logf("      option %d: key did not read", i + 1); continue; }
        key[len] = '\0';
        for (uint32_t c = 0; c < len; ++c) if (key[c] < 0x20 || key[c] > 0x7E) key[c] = '.';
        logf("      option %d: %s", i + 1, key);
    }
}

// Read-only: says what is pending and what answering it would do. Safe at any time, any thread.
static void reportDilemmaView(const DilemmaView& v)
{
    char ans[160]; answerText(v.answer, v.recOptions, ans, sizeof(ans));
    logf("DILEMMA: guard node=%016llX vptr=%016llX  item=%d  record=%016llX",
         (unsigned long long)v.node, (unsigned long long)v.vptr,
         v.itemIndex, (unsigned long long)v.rec);
    // Both counts, always, and labelled. They agreed on every record measured so far (two dumped
    // 2026-08-07, one pending and one answered) — but +0x2C is the one the engine bounds a choice
    // against, so it is the one `answer` uses, and printing both is how a divergence gets noticed
    // rather than assumed away.
    logf("    options offered: %d  (+0x2C, the bound `answer` uses)%s",
         v.options,
         (v.recOptions >= 0 && v.recOptions != v.options)
             ? "   ⚠⚠ DISAGREES with +0x38 — report this" : "   — agrees with +0x38");
    reportOptionKeys(v);
    logf("    state: %s", ans);
    if (v.haveFactions)
        logf("    belongs to faction %u; this machine plays faction %u -> %s",
             (unsigned)v.recFactionId, (unsigned)v.myFactionId,
             v.owned ? "MINE, this machine may answer it"
                     : "NOT mine — answer it on the machine that plays that faction");
    else
        // No override is offered here on purpose. `unblock` has --force because a healthy hold is
        // legitimate and the operator can see the screens; ownership is not something a person can
        // overrule usefully, and answering another faction's dilemma is not a thing to make easy.
        logf("    ⚠ could not read both factions, so ownership is UNKNOWN — `answer` will refuse. "
             "There is deliberately no override for this one.");

    if (v.answer != -1)
        logf("    ⚠ this record is ALREADY answered (+0x48=%d). The guard is still up, which means "
             "the block is not an unanswered decision — do not answer it again.", v.answer);
    else if (v.options > 0)
        logf("    ⇒ `answer <1..%d> --yes` submits a choice. ONE machine only — the command carries "
             "no record id, so every client resolves its own guard from it.", v.options);
}

void reportPendingDilemma()
{
    DilemmaView v; char why[192] = { 0 };
    if (!resolveDilemma(v, why, sizeof(why))) {
        logf("DILEMMA: nothing to answer — %s.", why);
        return;
    }
    reportDilemmaView(v);
}

// The write. Queued by `answerPendingDilemma`, executed here on the game thread.
static void drainAnswerRequestInner(long idx)
{
    DilemmaView v; char why[192] = { 0 };
    if (!resolveDilemma(v, why, sizeof(why))) {
        logf("ANSWER: REFUSED — %s. Nothing submitted.", why);
        return;
    }
    // ★ Still the same decision we were asked about? Between queueing and draining, the pending
    // dilemma can change — the one on screen gets answered by hand, or another arrives. Answering
    // whatever happens to be pending NOW with an index chosen for something else is precisely the
    // wrong-option hazard this whole design is careful about everywhere else.
    if (g_answerRecord && v.rec != g_answerRecord) {
        logf("ANSWER: REFUSED — the pending dilemma CHANGED between the request and this tick "
             "(asked about record %016llX, now %016llX). The option number was chosen for the "
             "other one. Run `dilemma` and ask again.",
             (unsigned long long)g_answerRecord, (unsigned long long)v.rec);
        return;
    }
    // ★★★ THE CROSS-CHECK, and it is the reason this runs on the game thread rather than being
    // folded into the read above. We found the node by vptr; the RECEIVER will find its node with
    // the engine's own predicate. If those two ever disagree we would be bounds-checking against
    // one dilemma and answering another. Asking the engine directly is the only way to know, and it
    // is safe here because this is the campaign tick.
    const uintptr_t engineHead = callGuardHead(v.model);
    uintptr_t engineNode = engineHead; int hops = 0; bool engineFaulted = false;
    while (engineNode > 0x10000 && hops < (int)BLK_WALK_CAP) {
        const int isD = callGuardIsDilemma(engineNode);
        if (isD < 0) { engineFaulted = true; break; }
        if (isD == 1) break;
        engineNode = callGuardNext(engineNode);
        ++hops;
    }
    if (engineFaulted) {
        logf("ANSWER: REFUSED — the engine's own guard-type predicate faulted while confirming the "
             "node. Nothing submitted.");
        return;
    }
    if (engineNode != v.node) {
        logf("ANSWER: REFUSED — WE picked guard %016llX (by vptr) but the engine's own predicate "
             "picks %016llX. Those must agree, because the receiver uses the engine's. This has "
             "never been observed and is a finding to report, not something to answer through.",
             (unsigned long long)v.node, (unsigned long long)engineNode);
        return;
    }

    // ★★ SECOND CROSS-CHECK: the engine derives the record from the GUARD'S OWN FACTION
    // (`node+0x20 → +0x288 → model → +0x3D30 → [node+0x28]`); we derived it from the global root
    // chain. Two independent routes to the same object. If they disagree, one of the two models is
    // not the one the receiver will use, and nothing below this line would mean what it says.
    const uintptr_t engineRec = callGuardRecord(v.node);
    if (!engineRec || engineRec != v.rec) {
        logf("ANSWER: REFUSED — the record derived from the guard's own faction is %016llX but ours "
             "is %016llX. Two routes to the same record must agree; report this rather than "
             "answering through it.",
             (unsigned long long)engineRec, (unsigned long long)v.rec);
        return;
    }

    if (v.options <= 0 || v.options > 64) {
        logf("ANSWER: REFUSED — record %016llX reads %d options at +0x2C. Submitting an index into "
             "a list we cannot size is how a wrong choice gets made.",
             (unsigned long long)v.rec, v.options);
        return;
    }
    if (v.recOptions >= 0 && v.recOptions != v.options)
        logf("ANSWER: ⚠ the record says %d options and the submit path's list says %d. Using %d — "
             "it is the one the engine bounds the choice against. Worth reporting.",
             v.recOptions, v.options, v.options);
    if (idx < 0 || idx >= v.options) {
        logf("ANSWER: REFUSED — option %ld is out of range; this dilemma offers %d (1..%d).",
             idx + 1, v.options, v.options);
        return;
    }
    if (v.answer != -1) {
        logf("ANSWER: REFUSED — record %016llX is already answered (+0x48=%d). Answering twice is "
             "not something the engine's own path can do, so it is not something we will do either.",
             (unsigned long long)v.rec, v.answer);
        return;
    }
    if (!v.haveFactions) {
        logf("ANSWER: REFUSED — could not read both this machine's faction and the record's, so "
             "whether this decision is ours is unknown.");
        return;
    }
    if (!v.owned) {
        logf("ANSWER: REFUSED — this dilemma belongs to faction %u and this machine plays faction "
             "%u. Answer it on that machine; one submission resolves it everywhere.",
             (unsigned)v.recFactionId, (unsigned)v.myFactionId);
        return;
    }

    uintptr_t queue = 0;
    if (!readAt(v.obj + OFF_COMMAND_QUEUE, queue) || queue <= 0x10000) {
        logf("ANSWER: REFUSED — the command queue at +0x80 reads %016llX.",
             (unsigned long long)queue);
        return;
    }

    logf("################################################################");
    logf("★★★ ANSWER: submitting option %ld of %d for faction %u (record %016llX, guard %016llX).",
         idx + 1, v.options, (unsigned)v.recFactionId,
         (unsigned long long)v.rec, (unsigned long long)v.node);

    // The engine's own two calls. The buffer is a 16-byte POD — `{ vtable, u32 index }` — that
    // `FUN_141B3F760` fills and `FUN_142F25B60` serialises into the queue; the game's own path never
    // frees it, because the submit takes no ownership.
    unsigned char buf[CHOICE_CMD_SIZE];
    memset(buf, 0, sizeof(buf));

    uint8_t rc = 0; bool faulted = false;
    __try {
        ((MakeChoiceFn)(g_base + RVA_MAKE_CHOICE_CMD))(buf, (uint32_t)idx);
        rc = ((SubmitFn)(g_base + RVA_SUBMIT_COMMAND))(queue, buf);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; }

    if (faulted) {
        logf("    FAULTED inside the engine's own build/submit. Nothing was queued — or a partial "
             "record was, which the executor would reject on its length check.");
    } else {
        logf("    submitted, submit returned %u. The command is CCQ_MAKE_DILEMMA_CHOICE (0x0405BF78) "
             "carrying only the index; every client will locate its own guard and pop it.",
             (unsigned)rc);
        logf("    ⇒ WATCH FOR: the guard list going empty, and record +0x48 changing from -1 to %ld. "
             "Press F3 to confirm. If the guard is still up a few seconds from now, the submission "
             "was accepted but not acted on, and THAT is the next thing to chase.", idx);
    }
    logf("################################################################");
}

// =================================================================================================
//  B1 THEORY 5 — IS THE EVENT-FEED AUTO-OPEN GATE REFUSING?  (2026-08-09)
//
//  Nothing PUSHES a dilemma onto the screen. A UI ticker (`FUN_142FCC8E0`) polls every 200 ms and
//  fires `EventFeedAutoOpen` by name — but only `if (FUN_142F49430())`. If that gate is false the
//  ticker fires nothing, forever, and the decision is never presented: B1's exact description.
//
//  ★ THE WHOLE FUNCTION WAS DISASSEMBLED OUT OF A DUMP (2026-08-09,
//  `PWL12_20260809-133253_healthy-dilemma-displayed.dmp`). It is exactly NINE PANEL CHECKS followed
//  by FOUR NUMERIC CONDITIONS, and nothing else — that is now counted, not assumed:
//
//      142F4966A  MOV RAX, [0x1443CFA50]          ; ⚠ the root is RELOADED HERE. Earlier write-ups
//      142F49671  CMP qword [RAX+0x27B0], 0       ;   omitted this line and read the next two as
//      142F4967B  CMP byte  [0x1443BFA29], 0      ;   model-relative. They are ROOT-relative.
//      142F49684  CMP byte  [RAX+0x2D35], 0
//
//      FUN_141844B60:  MOV EAX,[RCX+0x138] · TEST · returns 0 iff state ∈ {0, 0x0E}
//                      and the gate proceeds only when it returns 0.
//
//  ✗✗ RETRACTED — "THE FOUR NUMERIC CONDITIONS ARE RULED OUT, so a refusal must be one of the nine
//  panels." That was read off ONE B1 dump, and it does not discriminate anything. A **healthy,
//  displayed, unanswered dilemma** was captured on 2026-08-09 with the gate REFUSING continuously
//  either side of the capture, and its four numerics read:
//
//      state = 0x0E ✓   root+0x27B0 = 0 ✓   DAT_1443BFA29 = 0 ✓   root+0x2D35 = 0 ✓
//
//  — i.e. IDENTICAL to the B1 dump. Since the four permit and there is no fifth condition, a panel
//  was open in BOTH cases. ⇒ **The numerics cannot tell B1 from healthy; only WHICH PANEL can.**
//
//  ⚠⚠ AND THAT IS WHY THIS PROBE USED TO ANSWER NOTHING. Attribution ran only on the falling edge,
//  so it reported whatever was open at the instant the gate turned false and never looked again — it
//  said "no panel open" at 13:25:15 while a panel was demonstrably open at 13:32:53. A snapshot was
//  being read as if it described a duration. `post_battle_screen` had therefore NEVER been reported
//  in any log on disk: the gate is already refusing (on `pre_battle_screen`) by the time it appears,
//  so no edge ever fired.
//
//  WHAT IT DOES NOW: calls the gate — nullary, returns bool in AL — on the campaign tick, and while
//  it is REFUSING re-runs the engine's own per-panel lookup on a timer, logging whenever the set of
//  open panels CHANGES. That is what turns `pre_battle_screen` -> `post_battle_screen` into a
//  visible event. The four numerics are printed beside it, so a refusal nothing explains is
//  distinguishable from one the panels account for.
//
//  ⚠ GAME THREAD ONLY. It allocates CA strings and reads UI state. `watchCampaignTurns()` — the
//  obvious home for it — runs on the PROBE thread, which is exactly the mistake that cost session 5k
//  and the one `answer` was redesigned around. It therefore hangs off the campaign tick drain below.
//
//  ⚠ It lives INSIDE `drainAnswerRequest` rather than getting its own call site because a second
//  call would mean editing `commands.cpp` and `tw3k.h`, and another session is holding files in this
//  tree. The name is now slightly wrong for what it does; that is the lesser harm and it is said out
//  loud here rather than left to be discovered.
// =================================================================================================

static constexpr uintptr_t RVA_EVENTFEED_GATE = 0x02F49430;   // FUN_142F49430() -> bool
static constexpr uintptr_t RVA_PANEL_MGR_GET  = 0x02D504E0;   // FUN_142D504E0()
// ⚠ (mgr, out, CaString*) -> out. The first argument IS the manager and the callee DOES use it —
// the gate does `MOV RCX,RBX` to reload it right before every one of the nine calls. A comment here
// used to say it was ignored; the disassembly says otherwise.
static constexpr uintptr_t RVA_PANEL_BY_NAME  = 0x02D51210;

// ★★ The manager is a lazy singleton in ONE global, and both of the above read it:
//     FUN_142D504E0:  MOV RAX,[0x1443B7F18]   ... if null, allocate 0x88 and construct
//     FUN_142D51210:  MOV RAX,[0x1443B7F18]   ... same, inlined
// Reading the global directly is strictly safer than calling the getter for a read-only probe: no
// call on the game thread, and no chance of CONSTRUCTING a manager that did not exist yet just by
// asking whether a panel is open. A null here simply means nothing has opened a panel.
static constexpr uintptr_t RVA_PANEL_MGR_SINGLETON = 0x043B7F18;

// ★★★ The manager's open-panel stack, read out of the engine's own lookup (FUN_142D83BE0):
//     R8D = [mgr+0x5C]                  ; how many are open
//     RAX = [mgr+0x60]                  ; the array of panel pointers
//     out[8] = ([RAX + R8D*8 - 8] == panel)
// ⚠ Note what that last line means: the bool the by-name lookup returns is "is this panel the LAST
// entry", i.e. the TOPMOST one — NOT "is this panel open". With one panel open the two coincide,
// which is why the nine-name probe looked correct; with two open it reports only the top.
// ⇒ enumerating the array is both more truthful and strictly more informative, because it also
// sees panels that are not in kGatePanels at all.
static constexpr size_t OFF_PANELMGR_COUNT = 0x5C;   // uint32
static constexpr size_t OFF_PANELMGR_ARRAY = 0x60;   // void**
static constexpr int    MAX_OPEN_PANELS    = 32;     // the array is a UI stack; 32 is absurdly deep

// The four numeric conditions, by the chain the gate's own tail walks. Every one is a plain read.
// ★ The head of that chain is already in offsets.h — `RVA_CAMPAIGN_ROOT` -> `OFF_ROOT_OBJ` ->
// `OFF_MODEL` is the same walk the F7 human-faction probe makes, so it is reused rather than
// restated. ⚠ Only the tail is new: the gate hangs off `model+0x3B80`, NOT `OFF_CONTAINER`
// (`+0x3B68`) — two different objects 0x18 apart, which is exactly the kind of pair this codebase
// has confused before.
static constexpr uintptr_t RVA_GATE_GLOBAL_B  = 0x043BFA29;   // DAT_1443BFA29, permits on 0
static constexpr size_t    OFF_GATE_STATEOBJ  = 0x3B80;       // on the model
static constexpr size_t    OFF_GATE_STATE     = 0x0138;       // permits on 0 or 0x0E
static constexpr size_t    OFF_GATE_ROOT_Q    = 0x27B0;       // qword on the ROOT, permits on 0
static constexpr size_t    OFF_GATE_ROOT_B    = 0x2D35;       // byte  on the ROOT, permits on 0

struct GateNumerics {
    bool     read;        // every dereference in the chain succeeded
    uint32_t state;       // *(u32*)(*(model+0x3B80) + 0x138)
    uint64_t q27B0;
    uint8_t  global;
    uint8_t  b2D35;

    bool statePermits()  const { return state == 0 || state == 0x0E; }
    bool allPermit()     const { return statePermits() && q27B0 == 0 && global == 0 && b2D35 == 0; }
};

// ⚠ READ-ONLY, and deliberately does NOT call FUN_141844B60 — the predicate is two compares and
// re-implementing it costs nothing, while calling it would put an engine call on this path for no
// information. Its body was disassembled to be sure the reimplementation is exact:
//     MOV EAX,[RCX+0x138] ; TEST EAX,EAX ; JZ ret0 ; CMP EAX,0x0E ; JZ ret0 ; MOV AL,1 ; RET
static bool readGateNumerics(GateNumerics& n)
{
    n = GateNumerics{};
    uintptr_t root = 0, rootObj = 0, model = 0, stateObj = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root)     || root     <= 0x10000) return false;
    if (!readAt(root + OFF_ROOT_OBJ, rootObj)         || rootObj  <= 0x10000) return false;
    if (!readAt(rootObj + OFF_MODEL, model)           || model    <= 0x10000) return false;
    if (!readAt(model + OFF_GATE_STATEOBJ, stateObj)  || stateObj <= 0x10000) return false;

    if (!readAt(stateObj + OFF_GATE_STATE, n.state))    return false;
    if (!readAt(root + OFF_GATE_ROOT_Q, n.q27B0))       return false;
    if (!readAt(g_base + RVA_GATE_GLOBAL_B, n.global))  return false;
    if (!readAt(root + OFF_GATE_ROOT_B, n.b2D35))       return false;
    n.read = true;
    return true;
}

static void logGateNumerics(const GateNumerics& n)
{
    if (!n.read) {
        logf("    numerics: UNREADABLE — the root/model chain did not resolve, so nothing is claimed "
             "about them.");
        return;
    }
    logf("    numerics: state=0x%02X %s · root+0x27B0=%llu %s · DAT_1443BFA29=%u %s · "
         "root+0x2D35=%u %s%s",
         n.state,  n.statePermits() ? "(permits)" : "(REFUSES — not 0 or 0x0E)",
         (unsigned long long)n.q27B0, n.q27B0 == 0 ? "(permits)" : "(REFUSES)",
         n.global, n.global == 0 ? "(permits)" : "(REFUSES)",
         n.b2D35,  n.b2D35 == 0 ? "(permits)" : "(REFUSES)",
         n.allPermit()
             ? "   ⇒ all four permit, so the refusal is a PANEL."
             : "   ⇒ a numeric condition is refusing; the panels below need not explain anything.");
}

// =================================================================================================
//  THE EVENT-FEED TICKER — a DETOUR, not a sampler (§6sss, rewritten 2026-08-10)
//
//  `FUN_142FCC8E0(self, nowMs)` decompiled in full on 2026-08-10. It is a two-phase state machine:
//
//      +0x14D != 0  -> branch A: throttle(200ms) -> gate -> listener(+0x118) ->
//                                fire "EventFeedRepopulate" -> SET +0x14C
//      +0x14D == 0  -> branch B: needs +0x14C -> throttle -> gate -> listener(+0x128) ->
//                                CLEAR +0x14C -> fire "EventFeedAutoOpen"   <- puts the box up
//
//  ★★★ WHY THIS IS A HOOK AND NOT A SAMPLE — the lesson that has now been learned twice.
//
//  At `+0x14C == 0 && +0x14D == 0` NEITHER branch has a reachable body. That is the state a HEALTHY
//  feed RESTS in between cycles: branch B leaves it there on every successful consume. It is also
//  exactly what a stuck one looks like. Measured on test-host 2026-08-10 21:50, minutes after a
//  confirmed B1 had been recovered and the campaign was running normally — three reads one second
//  apart, `+0x148` frozen at 0x0017AFCD, `14C=0`, `14D=0`.
//
//  ⇒ NO SAMPLE OF ANY KIND DISCRIMINATES. Not a dump (three were taken, 2026-08-09). Not a live
//  read (2026-08-10: B1 was declared "located" on one, and that was wrong). Not "is the throttle
//  advancing", because an idle healthy feed does not advance it either. The healthy state and the
//  stuck state are THE SAME BYTES; only the TRANSITION differs, and only a hook sees a transition.
//
//  ★ It also deletes the object hunt entirely. `self` arrives as the first argument. The previous
//  build searched for it by sweeping every committed private region — 11.8 GB across 20,128 regions
//  on a live game — every 10 s for as long as its cached pointer was stale, and did so SILENTLY
//  after the first three attempts. That is the load-time regression reported on 2026-08-10, and it
//  was introduced by the fix that stopped the scan latching a ghost: the buggy version stopped at
//  the first match in "1 regions walked", so being wrong was what had been keeping it cheap.
//
//  ⚠ THERE IS MORE THAN ONE INSTANCE. A static one lives at 0x143C45E40, built by an inlined
//  constructor at 14029B0C0, and is inert — throttle 0, still carrying the ctor's `14C=0, 14D=1`.
//  The old scan's "exactly ONE instance" comment was simply false, and it had two heap candidates
//  it could not tell apart, picking by address order. A hook cannot have that problem: it reports
//  whichever object the engine actually ticks, and says so if that ever changes.
//
//  ⚠⚠ CLASS IDENTITY IS BY VTABLE (`0x14383C5B0`, installed by the ctor at 142FC11C8), NEVER by
//  field layout. Three separate candidates have now been wrongly accepted because they had bytes at
//  +0x14C/+0x14D — including `1410BE550`, which B1-d proposed as the re-armer and which is in fact
//  a constructor for an unrelated ~370 KB class whose fields run out to +0x5A578.
// =================================================================================================

static constexpr uintptr_t RVA_FEED_TICK   = 0x02FCC8E0;   // FUN_142FCC8E0(self, nowMs)
static constexpr uintptr_t RVA_FEED_STATIC = 0x03C45E40;   // the inert static instance
static constexpr size_t    OFF_FEED_T148   = 0x148;        // u32 throttle stamp, set on branch entry
static constexpr size_t    OFF_FEED_B14C   = 0x14C;        // pending: branch B's precondition
static constexpr size_t    OFF_FEED_B14D   = 0x14D;        // branch select / "repopulate subscribed"

// 17 bytes = 4 whole instructions, none RIP-relative, ending exactly on the `MOV RDI,RCX` boundary.
//
// ⚠ The 4th stolen instruction is `CMP byte [RCX+0x14D],0`, and the `JZ` two instructions LATER
// consumes its ZF. detourInstall's trampoline tail is `JMP qword ptr [rip+0]`, which does not touch
// flags, so the branch still reads the right answer. This is the one thing that would have made a
// register-free jump insufficient, and it is fine.
//
// Verified byte-for-byte against the LIVE process as well as the static image, because this region
// is inside a Denuvo-protected binary and only the running bytes are authoritative.
static constexpr size_t FEEDTICK_STOLEN_LEN = 17;
static const uint8_t EXPECT_FEED_TICK[FEEDTICK_STOLEN_LEN] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,               // MOV qword ptr [RSP+0x10],RBX
    0x57,                                       // PUSH RDI
    0x48, 0x83, 0xEC, 0x40,                     // SUB RSP,0x40
    0x80, 0xB9, 0x4D, 0x01, 0x00, 0x00, 0x00,   // CMP byte ptr [RCX+0x14D],0
};

typedef void (*FeedTickFn)(uintptr_t self, int nowMs);
static FeedTickFn g_origFeedTick = nullptr;
static Detour     g_feedTickDetour;

static volatile long g_feedCalls   = 0;   // every call, including the resting ones
static volatile long g_feedInert   = 0;   // 14C==0 && 14D==0 -> no branch body reachable
static volatile long g_feedThrott  = 0;   // a branch was selected but the 200 ms throttle said "not yet"
static volatile long g_feedRaised  = 0;   // branch A completed: EventFeedRepopulate fired, 14C set
static volatile long g_feedOpened  = 0;   // branch B completed: EventFeedAutoOpen fired  <- a box
static volatile long g_feedRefused = 0;   // a branch was entered and produced nothing
static volatile long g_feedSpentOOT= 0;   // branch B fired while it was NOT our turn -> §6uuu
static uintptr_t     g_feedSelf    = 0;
static bool          g_feedVerbose = true;

// ✗ REGRESSION FIXED 2026-08-10, same evening, caught by its own first log: while the gate refuses,
// branch A runs EVERY 200 ms and produced one line each time — 37 lines in 8 s, and log.cpp reopens
// the file per line. That is a probe making the thing it measures worse, which is the failure the
// old sampler's "a probe that shouts while saying nothing" note already warned about.
// ⇒ A refusal RUN is one line when it starts and one when it ends, and the closing line carries the
//   count and the duration — strictly more information than the 37 it replaces.
static long     g_refuseRun   = 0;    // consecutive refused attempts in the current episode
static uint64_t g_refuseStart = 0;
static int      g_seen14D     = -1;   // last value SEEN, across calls — see below

// ★ The cross-call gap, also found by that first log: 40 branch-A refusals were followed by a
// branch-B consume, which requires +0x14D to have become 0 — but nothing logged the change, because
// the original comparison was before-vs-after WITHIN one call and +0x14D is written by
// FUN_1430076B0, from outside. A flag that only ever moves between our calls was therefore invisible
// to the probe built to watch it.
// Returns true if it printed, so the caller does not ALSO print its plain success line.
static bool noteFeedRefusalCleared(const char* what)
{
    if (g_refuseRun <= 0) return false;
    const double secs = (double)(GetTickCount64() - g_refuseStart) / 1000.0;
    const long   n    = g_refuseRun;
    g_refuseRun = 0;
    if (!g_feedVerbose) return false;   // `feed off` means silent; the counts still moved
    logf("EVENT FEED: %s after %ld refused attempt(s) over %.1f s — that run of refusals is over.",
         what, n, secs);
    return true;
}

// =================================================================================================
//  ★★★ #57 — WHAT PRECEDED THE FILL
//
//  The watch below reports a CHANGE in the feed's list count. On its own that is a timestamp, and
//  the job #57 actually sets is to *name what inserts* rather than infer it. So every feed-relevant
//  moment this file already recognises drops a marker here, and the change line quotes the most
//  recent one with its age — turning "the list grew at 01:29:41.844" into "the list grew 0.012 s
//  after the dilemma push", which is the sentence the issue is asking for.
//
//  ⚠ A marker is a CORRELATION and the line never says otherwise. Two markers in the same
//  millisecond is exactly the case this cannot resolve, which is why the age is printed rather than
//  just the name: 0.000 s reads as "these are simultaneous, pick neither" and 0.400 s does not.
//
//  ⚠ String LITERALS only. The marker outlives its caller's frame by design, so anything with a
//  lifetime shorter than the process must not be passed here.
static const char* g_feedMarker   = nullptr;
static uint64_t    g_feedMarkerAt = 0;

static void noteFeedMarker(const char* what)
{
    g_feedMarker   = what;
    g_feedMarkerAt = GetTickCount64();
}

static void formatFeedMarker(char* out, size_t cap)
{
    if (!g_feedMarker) {
        _snprintf_s(out, cap, _TRUNCATE,
                    ", and NOTHING this probe recognises preceded it — which is itself the finding: "
                    "the insert is reached by a path none of the feed hooks is on");
        return;
    }
    _snprintf_s(out, cap, _TRUNCATE, ", %.3f s after `%s`",
                (double)(GetTickCount64() - g_feedMarkerAt) / 1000.0, g_feedMarker);
}

// The list watch itself lives with the reader it uses, in the listener section below, because the
// offsets it depends on are documented there. These two are the forward halves.
static void noteFeedListSample(void* self, const char* where);
static void sampleFeedListFromCache(const char* where);
static void drainFeedRefresh();   // `feed refresh`, drained on the UI thread by the ticker hook
static void drainSourceRebuild(); // `feed rebuild` — level 1, same drain point

// ★★★ WHOSE TURN IS IT? — the question the ticker never asks, and §6uuu says it is the one that
// decides whether the fired event can produce anything.
//
// `EventFeedAutoOpen` is published to whatever subscribes; the subscriber that matters is
// `CcoCampaignEventFeed.NextAutoOpenEventContext` = `FUN_142F72090`, and its ENTIRE body sits inside
// one inlined guard, evaluated before it looks at a single event:
//
//     root->0x2188->0x1A8->id(+8)   ==   root->0x2188->0x78->0x3B68->0x48->id(+8)
//     (LOCAL faction)                     (CURRENT faction)
//
// — the same two reads as `FUN_142F6BFA0`, the nullary "is it my turn" predicate. If it fails, the
// function returns having produced NO context, so there is nothing to put on screen.
//
// ⇒ THE TICKER CANNOT SEE THAT. It clears `+0x14C` and publishes regardless, so a fire that lands
// out of turn is SPENT and never retried: the ticker retries against the GATE, never against the
// LISTENER. That is the shape B1 has always had — the event fires, no box appears, and nothing tries
// again.
//
// ⚠ This is a HYPOTHESIS under test, not a finding. It was reached statically; the 2026-08-12 log
// timed the stuck dilemma to within a second of a turn change but has no sample at the instant that
// matters, which is exactly the gap this closes. `SPENT OUT OF TURN` during a confirmed B1 confirms
// it; its absence kills it, as it killed the five mechanisms before it.
static bool feedTurnIds(uint32_t* curOut, uint32_t* mineOut)
{
    uintptr_t root = 0, obj = 0, model = 0, cont = 0, cur = 0, mine = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root)      return false;
    if (!readAt(root + OFF_ROOT_OBJ, obj) || !obj)               return false;
    if (!readAt(obj + OFF_MODEL, model) || !model)               return false;
    if (!readAt(model + OFF_CONTAINER, cont) || !cont)           return false;
    if (!readAt(cont + OFF_CUR_FACTION, cur) || cur <= 0x10000)  return false;
    if (!readAt(obj + OFF_LOCAL_FACTION, mine) || mine <= 0x10000) return false;
    return readAt(cur + OFF_FACTION_ID, *curOut) &&
           readAt(mine + OFF_FACTION_ID, *mineOut);
}

// =================================================================================================
// Read-only. Samples the three bytes either side of the original call and reports only what the
// call CHANGED — which is the whole point, since the state itself carries no information.
static void feedTickHook(uintptr_t self, int nowMs)
{
    InterlockedIncrement(&g_feedCalls);

    uint8_t  b14C = 0, b14D = 0;
    uint32_t bT = 0;
    const bool before = readAt(self + OFF_FEED_B14C, b14C) &&
                        readAt(self + OFF_FEED_B14D, b14D) &&
                        readAt(self + OFF_FEED_T148, bT);

    if (g_origFeedTick) g_origFeedTick(self, nowMs);
    if (!before) return;

    uint8_t  a14C = 0, a14D = 0;
    uint32_t aT = 0;
    if (!readAt(self + OFF_FEED_B14C, a14C) ||
        !readAt(self + OFF_FEED_B14D, a14D) ||
        !readAt(self + OFF_FEED_T148, aT)) return;

    // Which object does the engine actually tick? Free here, and it settles the multi-instance
    // question the scan could never answer.
    if (self != g_feedSelf) {
        const uintptr_t was = g_feedSelf;
        g_feedSelf = self;
        logf("EVENT FEED: the ticked object is %016llX%s. The static instance at %016llX is a "
             "decoy and is never ticked.",
             (unsigned long long)self,
             was ? " — CHANGED; the previous one is gone, so anything caching it was stale" : "",
             (unsigned long long)(g_base + RVA_FEED_STATIC));
    }

    // ⚠ Compared against the last value SEEN, not against `b14D` from this same call. FUN_1430076B0
    // writes it from outside, so the change lands BETWEEN our calls and an in-call comparison never
    // sees it. Measured 2026-08-10: branch A refused 40 times, then branch B consumed — which is
    // only possible if +0x14D went 1 -> 0, and the first build logged nothing at all.
    if ((int)a14D != g_seen14D) {
        const int was = g_seen14D;
        g_seen14D = a14D;
        noteFeedMarker(a14D ? "+0x14D armed (a subscription was set up)"
                            : "+0x14D torn down (FUN_1430076B0)");
        if (was >= 0)
            logf("EVENT FEED: +0x14D %d -> %u  (%s)", was, (unsigned)a14D,
                 a14D ? "repopulate ARMED — and nothing in the image sets this except a constructor"
                      : "repopulate subscription torn down; FUN_1430076B0 is its only writer of 0");
    }

    // ★★★ `feed refresh` drains HERE — UI thread, frame rate, and still running on a stuck client
    // when everything else has stopped. Ordered before the sample so the result is visible on this
    // same tick rather than the next one.
    drainFeedRefresh();
    drainSourceRebuild();


    // ★ #57 — the frame-rate half of the list watch. The ticker runs whenever the feed exists, so
    // this keeps watching after the listener stops being polled — and the list filling WHILE the
    // listener is silent is precisely the case worth catching, since it would mean the insert has
    // nothing to do with the query path we have spent three sessions inside.
    sampleFeedListFromCache("ticker");

    // The resting state. Extremely common and NOT a fault — see the header.
    if (b14D == 0 && b14C == 0) { InterlockedIncrement(&g_feedInert); return; }

    // A branch was selected, but the throttle stamp did not move, so its body never ran.
    if (aT == bT) { InterlockedIncrement(&g_feedThrott); return; }

    if (b14D != 0) {                                     // ---- branch A ----
        if (b14C == 0 && a14C != 0) {
            InterlockedIncrement(&g_feedRaised);
            noteFeedMarker("branch A raised +0x14C (the engine's own `EventFeedRepopulate`)");
            const bool told = noteFeedRefusalCleared(
                "branch A RAISED +0x14C (`EventFeedRepopulate` fired)");
            if (g_feedVerbose && !told)
                logf("EVENT FEED: branch A RAISED +0x14C — `EventFeedRepopulate` fired. Branch B "
                     "should consume it within ~200 ms and put a box up.");
        } else {
            InterlockedIncrement(&g_feedRefused);
            // ONE line per run, not one per attempt: this fires every 200 ms while it lasts.
            if (g_feedVerbose && g_refuseRun == 0)
                logf("EVENT FEED: branch A is being REFUSED — the gate said no, or the +0x118 "
                     "listener vetoed. ★ One of the two halves that leaves a dilemma unshown. "
                     "Repeats are suppressed; a line with the count and duration follows when it clears.");
            if (g_refuseRun == 0) g_refuseStart = GetTickCount64();
            ++g_refuseRun;
        }
    } else {                                             // ---- branch B ----
        if (b14C != 0 && a14C == 0) {
            InterlockedIncrement(&g_feedOpened);
            noteFeedMarker("branch B consumed +0x14C (`EventFeedAutoOpen` fired)");

            // The event has just been published and `+0x14C` is already clear, so THIS is the moment
            // that decides whether it produced anything — see feedTurnIds above.
            uint32_t   curId = 0, myId = 0;
            const bool okTurn    = feedTurnIds(&curId, &myId);
            const bool outOfTurn = okTurn && curId != myId;
            if (outOfTurn) InterlockedIncrement(&g_feedSpentOOT);

            const bool told = noteFeedRefusalCleared(
                outOfTurn ? "★★★ branch B CONSUMED — but SPENT OUT OF TURN, so no box can appear"
                          : "★★★ branch B CONSUMED — `EventFeedAutoOpen` fired, a box is going up");
            if (g_feedVerbose && !told)
                logf("EVENT FEED: ★★★ branch B CONSUMED — `EventFeedAutoOpen` fired.%s",
                     outOfTurn ? "" : " A box is going up.");

            if (outOfTurn)
                logf("EVENT FEED: ⚠⚠ SPENT OUT OF TURN — current faction id=%08X, mine=%08X. "
                     "`NextAutoOpenEventContext` refuses unless those match, so this fire produced "
                     "NOTHING and will not be retried (+0x14C is already clear). ★ If a dilemma is "
                     "outstanding for this faction, THIS LINE IS B1's MECHANISM.",
                     curId, myId);
            else if (!okTurn && g_feedVerbose)
                logf("EVENT FEED: (turn ownership unreadable at the consume — outside a campaign "
                     "this is normal, and the line is here so a silent gap is not read as a match.)");
        } else {
            InterlockedIncrement(&g_feedRefused);
            if (g_feedVerbose && g_refuseRun == 0)
                logf("EVENT FEED: branch B is being REFUSED — the gate said no, or the +0x128 "
                     "listener vetoed. ★ A raised event is sitting there unshown. Repeats suppressed.");
            if (g_refuseRun == 0) g_refuseStart = GetTickCount64();
            ++g_refuseRun;
        }
    }
}

// =================================================================================================
//  ★★★ THE LISTENER ITSELF — `CcoCampaignEventFeed.NextAutoOpenEventContext` = FUN_142F72090
//
//  §6uuu found this is what `EventFeedAutoOpen` reaches, and proposed that its inlined
//  "is it my turn" guard was B1. ❌ REFUTED the same night: on 2026-08-13 the fire that followed the
//  stuck dilemma's own push carried NO `SPENT OUT OF TURN`, and the live `turn` read
//  `current faction == my faction: YES`. The guard PASSED and there was still no box. That is the
//  sixth mechanism to die to a measurement, and it died in one session because the probe was built
//  before the fix.
//
//  ⇒ So the refusal is INSIDE the walk, past the guard. Every event it iterates must satisfy three
//  clauses before it is handed back (142F6992E..142F69963):
//
//      cVar5 = *(char*)(recordDef + 0x40);          // a flag on the event TYPE
//      cVar6 = FUN_1414D2A80(record);
//      cVar7 = FUN_1414D8610(record, localFaction);
//      if (cVar5 != 0 && (cVar7 == 0 || cVar6 != 0))  -> build a context and return it
//
//  ✗ NOT chasing `+0x40` in the DB. tester, 2026-08-13: **multiple DIFFERENT events have been blocked
//  by this bug** — today's repeat of yesterday's key is coincidence, not signal — so a per-event-type
//  property cannot be the explanation and the DB hunt it invited is not worth the session.
//
//  ★ WHAT THIS PROBE READS, and it needs no engine calls. The accept path pushes into the vector at
//  `self+0xA8` and the tail then indexes `count@+0xAC` / `data@+0xB0` (142F699E9, 142F69A43). So the
//  COUNT AT +0xAC GROWING is exactly "this call produced a context". Sampling it either side of the
//  original splits the failure three ways, and the third outcome would move the target entirely:
//
//      turn guard refused        -> curId != myId, and §6uuu's mechanism is alive after all
//      guard passed, count same  -> the walk accepted NOTHING: the three-clause filter refused
//      count grew                -> a context WAS produced, so the failure is DOWNSTREAM of here
//
//  ⚠ A CCO query is polled, so every line is change-triggered; the counters carry the volume.
// =================================================================================================

static constexpr uintptr_t RVA_NEXT_AUTO_OPEN = 0x02F72090;   // FUN_142F72090(self, sink)
static constexpr size_t    OFF_AO_ACCEPTED    = 0xAC;         // count of contexts this call produced

// ★★★ THE FEED'S OWN EVENT LIST — and reading its size needs no hook at all.
//
// The walk iterates `*(self + 0x90)` through begin/end helpers, and `FUN_141C10C50` (end) gives the
// container away in one line: it reads a u32 at `container + 0x0C` and computes
// `container[2] + count * 0x18`. ⇒ count@+0x0C, data@+0x10, stride 0x18 — the same shape as the
// pre-battle participant list, so the reader is the one already proven rather than a second one.
//
// ⇒ `begin == end` exactly when that count is ZERO, which is what separates the two readings of an
// `exclusion calls=0`:
//
//     count == 0  ->  the feed's list is EMPTY: the event never reached the feed at all
//     count >  0  ->  entries exist and every one was skipped BEFORE the clause tests
//                     (FUN_141C115E0's skip, or the FUN_141C0DCA0 compare)
//
// ⚠ Those are very different bugs, and on 2026-08-13 the first was asserted from `calls=0` when the
// evidence only supported "no candidate reached the test". This is that correction, made readable.
static constexpr size_t OFF_AO_COLLECTION = 0x90;
static constexpr size_t OFF_COLL_COUNT    = 0x0C;
static constexpr size_t OFF_COLL_DATA     = 0x10;
static constexpr size_t COLL_STRIDE       = 0x18;

// ★★★★★ THE DIRTY FLAG — why the list is recomputed once per turn, read out of the binary 2026-08-13.
//
// Both of this context's list getters (`NextAutoOpenEventContext` = FUN_142F72090 and
// `GroupEventList` = FUN_142F5BC20) begin with the same call, `FUN_1405B5BC0(self + 0x48)`:
//
//     1405B5926  CMP byte ptr [RCX + 0x40],0x0    ; RCX = self+0x48  =>  self+0x88
//     1405B592D  JZ  <return>                     ; CLEAN -> do nothing at all
//     1405B592F  MOV byte ptr [RCX + 0x40],0x0    ; DIRTY -> clear, then call virtual +0x10
//                                                 ;          which is the RECOMPUTE
//
// ⇒ the list is not rebuilt on a timer. It is rebuilt **on query, if and only if this byte is set**,
// and the "once per turn, ~1 s in" §6uuu.8 measured is the flag being set at turn start and the
// first query arriving a beat later.
//
// ★ That helper has 100+ callers across the whole CCO layer, so this is the engine's STANDARD lazy
// refresh for every databinding context — not a feed quirk. It is also the complete explanation for
// `repop`: firing a named event never sets this byte, so every query short-circuited on the JZ and
// handed back the same stale list. Nothing we ever did marked the context stale.
//
// ⚠ NOT read: the recompute itself (virtual slot 0x10 on the +0x48 sub-object). That it rebuilds
// `+0x90` is inference from the flag clearing and the list changing in the same window. THIS PROBE IS
// WHAT CONFIRMS OR KILLS THAT — sampling either side of the original inside the listener hook puts
// the clear between two reads: `entry dirty=1` -> `exit dirty=0` + the count moved is the whole model
// in one line pair.
static constexpr size_t OFF_FEED_DIRTY = 0x88;

// ★★★ #61 — `+0x90` IS THE INPUT. THESE ARE THE OUTPUTS.
//
// `FUN_142FE62D0` builds this context with `+0x90` **passed in as an argument**, and the recompute
// `FUN_142F6E1D0` READS it and writes its results to two vectors of {cap, count, data}. So the thing
// this file has been calling "the feed's own list" all day is the recompute's SOURCE.
//
// ⇒ That mattered, not just as wording: `feed refresh`'s result line reported *"the count did not
// move, therefore the recompute produced nothing"* while watching the input, which the recompute is
// not supposed to move. The conclusion survived for a different reason (the input was genuinely
// empty), but the evidence offered for it measured the wrong vector. #61.
//
// ⇒ Sampling BOTH is what separates the two readings that matter:
//     source 0, outputs 0  ->  nothing to show. The failure is UPSTREAM of this context.
//     source N, outputs 0  ->  the recompute REJECTED all N. The failure is in its two filters.
static constexpr size_t OFF_OUT_A_COUNT = 0x9C;   // recompute output 1 (cap 0x98, data 0xA0)
static constexpr size_t OFF_OUT_B_COUNT = 0xAC;   // recompute output 2 (cap 0xA8, data 0xB0)

// Both output counts. Returns false only if neither is readable, so "unknown" never prints as 0.
static bool feedOutputCounts(void* self, uint32_t* aOut, uint32_t* bOut)
{
    uint32_t a = 0, b = 0;
    const bool okA = readAt((uintptr_t)self + OFF_OUT_A_COUNT, a);
    const bool okB = readAt((uintptr_t)self + OFF_OUT_B_COUNT, b);
    if (!okA && !okB) return false;
    if (a > 4096) a = 0xFFFFFFFFu;      // implausible reads are flagged, not printed as data
    if (b > 4096) b = 0xFFFFFFFFu;
    *aOut = a;
    *bOut = b;
    return true;
}

// Returns false when the collection pointer is not readable/credible, so "unknown" is never printed
// as "empty" — the distinction this whole probe exists to make.
static bool feedListSize(void* self, uint32_t* countOut, uintptr_t* dataOut)
{
    uintptr_t coll = 0;
    if (!readAt((uintptr_t)self + OFF_AO_COLLECTION, coll) || coll <= 0x10000) return false;
    uint32_t  n = 0;
    uintptr_t d = 0;
    if (!readAt(coll + OFF_COLL_COUNT, n) || !readAt(coll + OFF_COLL_DATA, d)) return false;
    if (n > 4096 || (n && d <= 0x10000)) return false;
    *countOut = n;
    *dataOut  = d;
    return true;
}

// =================================================================================================
//  ★★★ #57 — THE WATCH: every CHANGE of the feed's list count, and what preceded it
//
//  The point is the HEALTHY path. test-host read `holds 1 entry` on 2026-08-13, so the fill demonstrably
//  happens and the moment has never been observed on any machine — every probe so far has sampled
//  the list at a moment chosen by something else (the listener being called), which is why the one
//  ZERO reading we have landed 420 ms before the push and proved nothing.
//
//  ⚠ WHY THIS IS A POLL AND NOT A HOOK, deliberately: hooking the insert would name it outright, but
//  we do not know where it is — and the two attempts to find such a writer by byte-searching offsets
//  (`+0x14C`/`+0x14D`, twice) are the trap B1-d died in. So this samples a pointer the engine hands
//  us, from two sites that between them cover UI-poll rate and frame rate, and correlates each change
//  against the markers above. Naming the inserter is then a reading job, not a search.
//
//  Sampled from:
//    · the listener, before AND after the original — the object arrives as an argument, so no cached
//      pointer can be stale, and a fill that happens DURING the walk is visible as before != after
//    · the ticker, from the pointer the listener last handed us — frame rate, and it keeps running
//      when the listener stops being polled
//
//  Read-only throughout: two loads behind `feedListSize`'s credibility checks, and a line only when
//  the number moves. The baseline/last-count pair is plain rather than interlocked, on the same
//  footing as `g_seen14D` and `g_aoLastSelf` beside it: both sample sites are UI-thread, and the
//  worst a race could cost is one duplicated line.
//
//  ⚠ BOUNDED, because this file has already shipped a probe that made the thing it measured worse:
//  branch A refusing every 200 ms produced 37 lines in 8 s, and log.cpp reopens the file per line.
//  Two shapes could do it again here — a context that is rebuilt constantly (a baseline line each
//  time), and a list that churns (a change line each time) — so both have a cap, and the counters
//  keep accruing after it so `feed` still reports the volume.
static constexpr long FL_MAX_BASELINES = 12;
static constexpr long FL_MAX_CHANGES   = 200;

static uintptr_t     g_flWatchSelf  = 0;    // the object the baseline belongs to
static int64_t       g_flWatchCount = -1;   // last count seen on it
static volatile long g_flFills      = 0;    // SOURCE count went UP
static volatile long g_flDrains     = 0;    // SOURCE count went DOWN
static long          g_flBaselines  = 0;    // objects watched, for the cap
static long          g_flLines      = 0;    // change lines printed, for the cap

// ★ The dirty flag, change-triggered on its own. It moves without the count moving — that is the
// whole point of watching it: `0 -> 1` names the moment the engine decided the feed was stale, which
// is the event §6uuu.8 could only infer from a turn-start timestamp.
static int g_flDirty = -1;

static void noteFeedDirty(void* self, const char* where)
{
    uint8_t b = 0;
    if (!readAt((uintptr_t)self + OFF_FEED_DIRTY, b)) return;
    if ((int)b == g_flDirty) return;
    const int was = g_flDirty;
    g_flDirty = b;
    if (was < 0 || !g_feedVerbose) return;   // first sight is a baseline, not a transition

    char marker[256];
    formatFeedMarker(marker, sizeof(marker));
    logf("EVENT FEED DIRTY: %d -> %u  (at the %s)%s  — %s", was, (unsigned)b, where, marker,
         b ? "★ MARKED STALE. The next query will recompute the list; nothing else will."
           : "cleared by FUN_1405B5BC0, so a recompute JUST RAN and the list should have moved.");
}

static void noteFeedListSample(void* self, const char* where)
{
    if (!self) return;

    noteFeedDirty(self, where);

    uint32_t  n = 0;
    uintptr_t d = 0;
    // Unreadable says nothing. The reports print "unknown" for that, and the whole reason this
    // reader returns a bool is that 'unknown' and 'empty' are the two answers B1 turns on.
    if (!feedListSize(self, &n, &d)) return;

    // A different object is a new baseline, never a change. The context is rebuilt when the binding
    // is torn down and set up again — which is the very machinery B1 is about — so this will happen
    // in normal running and must not be reported as the list growing.
    if ((uintptr_t)self != g_flWatchSelf) {
        const uintptr_t was = g_flWatchSelf;
        g_flWatchSelf  = (uintptr_t)self;
        g_flWatchCount = (int64_t)n;
        if (++g_flBaselines == FL_MAX_BASELINES + 1)
            logf("EVENT FEED SOURCE: %ld different context objects have been watched — no longer "
                 "printing a baseline for each. ⚠ That many rebuilds is itself worth knowing: the "
                 "watch is following a context the engine keeps replacing, so a `fills=0` reading "
                 "below may mean the fill lands on an object we only ever see afterwards.",
                 FL_MAX_BASELINES);
        if (g_feedVerbose && g_flBaselines <= FL_MAX_BASELINES) {
            char marker[256];
            formatFeedMarker(marker, sizeof(marker));
            logf("EVENT FEED SOURCE: baseline %u entr%s on context %016llX (seen at the %s)%s%s",
                 n, n == 1 ? "y" : "ies", (unsigned long long)self, where,
                 was ? " — a DIFFERENT object from the one being watched, so the context was rebuilt "
                       "and anything caching the old one was stale" : "",
                 marker);
            if (n)
                logf("   ⚠ it is ALREADY non-empty at the first sample, so this run did not see it "
                     "fill. The next change is the one to read.");
        }
        return;
    }

    if ((int64_t)n == g_flWatchCount) return;

    const int64_t was = g_flWatchCount;
    g_flWatchCount = (int64_t)n;
    if ((int64_t)n > was) InterlockedIncrement(&g_flFills);
    else                  InterlockedIncrement(&g_flDrains);

    if (!g_feedVerbose) return;
    if (++g_flLines == FL_MAX_CHANGES + 1)
        logf("EVENT FEED SOURCE: %ld change lines printed — the rest are counted but not logged, so a "
             "churning list cannot drown the session it was built to measure. `feed` has the totals.",
             FL_MAX_CHANGES);
    if (g_flLines > FL_MAX_CHANGES) return;

    char marker[256];
    formatFeedMarker(marker, sizeof(marker));
    logf("EVENT FEED SOURCE: ★★★ count %lld -> %u%s  (at the %s, data=%016llX)%s",
         (long long)was, n, (int64_t)n > was ? "  <<< FILLED" : "  (drained)", where,
         (unsigned long long)d, marker);

    // The entries themselves, on a FILL only — a drain has nothing new to identify and this is the
    // part that costs eight lines. Two words each is enough to tell one event from another across
    // samples, which is what says whether a fill is the dilemma we are watching or unrelated traffic.
    if ((int64_t)n < was) return;
    for (uint32_t i = 0; i < n && i < 8; ++i) {
        uint64_t w0 = 0, w1 = 0;
        readAt(d + (uintptr_t)i * COLL_STRIDE, w0);
        readAt(d + (uintptr_t)i * COLL_STRIDE + 8, w1);
        logf("      [%u] %016llX %016llX", i, (unsigned long long)w0, (unsigned long long)w1);
    }
}

// The production eventread module owns the detour; this module only observes its callback.
static bool g_autoOpenObserverRegistered = false;

static volatile long g_aoCalls     = 0;
static volatile long g_aoNoTurn    = 0;   // refused at the inlined turn guard
static volatile long g_aoEmpty     = 0;   // guard passed, walk accepted nothing
static volatile long g_aoProduced  = 0;   // a context was produced
static int           g_aoLastKind  = -1;  // change detection, so a polled query does not flood
static uintptr_t     g_aoLastSelf  = 0;   // the object the engine last ticked, for the report's live read

// The ticker's half of the #57 watch. The ticker is handed a DIFFERENT object (the feed's ticker and
// its `CcoCampaignEventFeed` context are separate — measured 2026-08-13), so it has no context
// pointer of its own and borrows the last one the listener was called on.
static void sampleFeedListFromCache(const char* where)
{
    if (g_aoLastSelf) noteFeedListSample((void*)g_aoLastSelf, where);
}

// =================================================================================================
//  ★★★★★ `feed dirty` — MARK THE FEED STALE, WHICH IS B1's CANDIDATE FIX
//
//  One byte, and it is the engine's OWN signal. `FUN_1405B5BC0` tests `self+0x88` at the top of both
//  list getters and recomputes only when it is set, so setting it makes the engine rebuild the list
//  **through its own virtual, on its own thread, at its own next query** — no engine call from us, no
//  code written, nothing to unwind. That is as close to "cause a re-evaluation rather than announce
//  one" as this layer allows, and it is a different and far better-evidenced byte than #57's dropped
//  `+0x14D` plan: right object, and the flag the engine actually reads.
//
//  ⚠⚠ WHAT THIS DOES NOT SETTLE, and it is the question that decides whether B1 is fixed: if the
//  recompute re-reads a MODEL-side list that is itself only built at turn start, marking the context
//  stale just rebuilds from the same stale source and no box appears. One armed use on a live B1
//  answers it — a `count 0 -> 1` line means the source had the dilemma all along and only the cache
//  was behind; no change means the staleness is upstream of this context entirely.
//
//  SAFETY: refuses unless the listener has handed us a context AND that context still reads as one
//  (`feedListSize` succeeds on it — the same credibility check the watch uses). A byte write cannot
//  tear, so it needs no game-thread marshalling; the recompute it provokes happens inside the getter
//  where the engine already expects it. OFF by default in the sense that nothing calls this but a
//  person typing `feed dirty`.
static volatile long g_flForced = 0;

static bool writeByteGuarded(uintptr_t addr, uint8_t v)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    __try {
        *(volatile uint8_t*)addr = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// =================================================================================================
//  ★★★★★ `feed refresh` — PULL THE REFRESH OURSELVES, because nothing else is going to
//
//  Measured on a live B1, 2026-08-13: `feed dirty` set `self+0x88` and **the byte was still 1 ninety
//  seconds later**, with the listener's call count frozen at 4. The lazy refresh is a PULL — it runs
//  only inside a getter — and on a stuck client the binding is torn down, so the property is never
//  evaluated and the flag sits there forever. Marking it stale is necessary and not sufficient.
//
//  So call the engine's own helper. `FUN_1405B5BC0(sub)` is eleven instructions, fully disassembled:
//  test the byte, clear it, call virtual `+0x10`. One argument, and it is a pointer we already hold
//  and re-validate. Nothing is patched and nothing is written except by the engine itself.
//
//  ⚠ THIS IS THE FIRST TIME WE CALL INTO THIS PATH rather than reading or flagging it, and the
//  virtual it dispatches to is UNREAD. Hence: one-shot, off unless asked, re-entrancy guarded,
//  SEH-wrapped, byte-verified, and drained on the UI THREAD from inside the ticker hook — which is
//  where the engine calls it from, at frame rate, and which is still running on a stuck client
//  (calls were climbing past 38,000 while everything else was frozen).
//
//  ⇒ Reading the outcome: the list FILLS and the cache was the whole story · the flag clears and the
//  count stays 0 and the source it recomputes FROM is stale too, which moves the target off the UI
//  layer entirely. Both are decisive; only one is a fix.
static constexpr uintptr_t RVA_CCO_REFRESH  = 0x005B5BC0;   // FUN_1405B5BC0(sub)
static constexpr size_t    OFF_FEED_CCO_SUB = 0x48;         // the CCO base sub-object on the context

// The prologue through `MOV RBX,RCX`, stopping short of the JZ so the check does not depend on a
// branch displacement. Same discipline as every detour here: verify the site, never assume the RVA.
//
// ❌★ TAKEN FROM THE LIVE PROCESS, NOT FROM A DISASSEMBLY LISTING — the first version was
// hand-assembled from Ghidra's text and refused on a live B1, costing a deploy cycle. `PUSH RBX`
// here is `40 53`, a **redundant REX prefix** plus the opcode; Ghidra renders it "PUSH RBX" exactly
// like the bare `53`, so transcribing the listing silently drops a byte and shifts everything.
// ⇒ The address was right and the check was wrong: a FALSE refusal, which is the safe direction but
//   is still a bug. **Read the bytes; never re-encode the rendering.** Same species as `offset` vs
//   `dock_offset` and "gift" vs "Share" — reasoning from what a tool DISPLAYS instead of what the
//   artefact CONTAINS.
static const uint8_t EXPECT_CCO_REFRESH[13] = {
    0x40, 0x53,                                 // PUSH RBX          (REX prefix + 53)
    0x48, 0x83, 0xEC, 0x20,                     // SUB RSP,0x20
    0x80, 0x79, 0x40, 0x00,                     // CMP byte ptr [RCX+0x40],0x0
    0x48, 0x8B, 0xD9,                           // MOV RBX,RCX
};

typedef void (*CcoRefreshFn)(void* sub);

static volatile long g_feedRefreshReq = 0;
static volatile long g_inFeedRefresh  = 0;
static volatile long g_feedRefreshes  = 0;

// Its own function so the __try needs no object unwinding — the 5n lesson.
static bool callCcoRefresh(uintptr_t sub)
{
    __try {
        ((CcoRefreshFn)(g_base + RVA_CCO_REFRESH))((void*)sub);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Called from feedTickHook — UI thread, frame rate. Never from the pipe thread: the recompute
// rebuilds UI-facing state and the engine only ever runs it from a getter on this thread.
static void drainFeedRefresh()
{
    if (g_feedRefreshReq == 0) return;
    if (InterlockedCompareExchange(&g_inFeedRefresh, 1, 0) != 0) return;   // brake 1: re-entrancy
    const long want = InterlockedExchange(&g_feedRefreshReq, 0);           // brake 2: one-shot
    if (want) {
        uint32_t n = 0; uintptr_t d = 0;
        uint8_t  dirty = 0;
        if (!g_aoLastSelf || !feedListSize((void*)g_aoLastSelf, &n, &d)) {
            logf("FEED REFRESH: REFUSING — the cached context no longer reads as one. Nothing called.");
        } else {
            readAt(g_aoLastSelf + OFF_FEED_DIRTY, dirty);
            logf("★★★★★ FEED REFRESH: calling FUN_1405B5BC0(self+0x48) on the UI thread. Before: "
                 "dirty=%u, list holds %u entr%s. ⚠ If dirty=0 the helper will do NOTHING by design — "
                 "run `feed dirty` first.", dirty, n, n == 1 ? "y" : "ies");

            const bool ok = callCcoRefresh(g_aoLastSelf + OFF_FEED_CCO_SUB);
            InterlockedIncrement(&g_feedRefreshes);

            uint32_t n2 = 0; uintptr_t d2 = 0;
            uint8_t  dirty2 = 0;
            const bool okAfter = feedListSize((void*)g_aoLastSelf, &n2, &d2);
            readAt(g_aoLastSelf + OFF_FEED_DIRTY, dirty2);

            if (!ok) {
                logf("FEED REFRESH: ⚠⚠ THE CALL FAULTED and was swallowed. The recompute virtual is "
                     "not safe to drive from here — record this and do not retry blind.");
            } else if (!okAfter) {
                logf("FEED REFRESH: returned, but the context no longer reads as one afterwards. "
                     "Report that — it is worse than a fault, because it is silent.");
            } else if (n2 != n) {
                logf("★★★★★ FEED REFRESH: ✓✓ THE LIST CHANGED — %u -> %u entr%s (dirty %u -> %u). "
                     "⇒ THE CACHE WAS THE WHOLE STORY. The recompute had the event available all "
                     "along and only the stale flag stood between it and the screen.",
                     n, n2, n2 == 1 ? "y" : "ies", dirty, dirty2);
            } else {
                logf("FEED REFRESH: the call returned and the count did NOT move (%u, dirty %u -> "
                     "%u). ⇒ %s", n, dirty, dirty2,
                     dirty != 0 && dirty2 == 0
                         ? "★★★ the refresh RAN — the flag was consumed — and produced nothing. The "
                           "source it recomputes FROM is stale too, and the target moves OFF the UI "
                           "layer. That is decisive and it is not a fix."
                         : "the flag was already clear, so the helper short-circuited and nothing "
                           "was tested. Run `feed dirty` first, then this.");
            }
        }
    }
    InterlockedExchange(&g_inFeedRefresh, 0);
}

bool requestFeedRefresh(char* why, size_t cap)
{
    if (!g_aoLastSelf) {
        _snprintf_s(why, cap, _TRUNCATE,
                    "no event-feed context has been seen yet — nothing to refresh.");
        return false;
    }
    uint8_t site[sizeof(EXPECT_CCO_REFRESH)] = { 0 };
    if (!safeRead((void*)(g_base + RVA_CCO_REFRESH), site, sizeof(site)) ||
        memcmp(site, EXPECT_CCO_REFRESH, sizeof(site)) != 0) {
        _snprintf_s(why, cap, _TRUNCATE,
                    "REFUSING — the bytes at RVA 0x%05llX are not FUN_1405B5BC0's prologue on this "
                    "build. Wrong version, or the address is wrong; either way do not call it.",
                    (unsigned long long)RVA_CCO_REFRESH);
        return false;
    }
    InterlockedExchange(&g_feedRefreshReq, 1);
    _snprintf_s(why, cap, _TRUNCATE,
                "queued — the ticker hook will call FUN_1405B5BC0(self+0x48) on the UI thread within "
                "a frame. Signature verified. Read the log for the `FEED REFRESH:` result line.");
    return true;
}

// =================================================================================================
//  ★★★★★★ `feed rebuild` — LEVEL 1: force the SOURCE to be rebuilt from the campaign model
//
//  §6uuu.15. There are TWO levels of the same lazy-dirty-flag pattern, and everything measured so far
//  has been level 2:
//
//    level 1   ticker+0x50   dirty ticker+0x90   recompute FUN_142FC0770  -> builds the SOURCE
//    level 2   feedCtx+0x48  dirty feedCtx+0x88  recompute FUN_142F6E1D0  -> builds the display lists
//
//  `FUN_142FC0770` is the one that matters: it calls
//  `FUN_1414E0E40(model+0x3D30, &out, LOCAL FACTION, …)` and publishes the result as
//  `*(ticker+0x138)`. That is the first point in the whole chain that takes the local faction, so it
//  is the first place a per-client divergence can legitimately arise.
//
//  On the stuck client BOTH levels read clean with an owned dilemma outstanding, and level 2 has
//  already been forced (it rebuilt correctly, from an empty source). **Level 1 has never been run.**
//
//  ★★★ AND THE LEVER IS BETTER THAN `feed dirty` + `feed refresh`. `FUN_14057B720` is the
//  UNCONDITIONAL twin of `FUN_1405B5BC0` — it clears the dirty byte and calls the recompute
//  regardless of it:
//
//      14057B559  MOV  byte ptr [RCX+0x40],0x0
//      14057B56D  JMP  qword ptr [RAX+0x10]      ; tail-call the recompute
//
//  ⇒ no flag to forge, and no two-step. **`FUN_142FBFE20` already calls exactly this on exactly this
//  sub-object**, so we are doing what the engine's own setup path does, with its own function.
//
//  ⇒ READING THE RESULT: the source count before and after is the entire answer.
//      grows   -> B1 is a STALE LEVEL-1 CACHE, and the fix is to invalidate it on a decision push
//      stays 0 -> `FUN_1414E0E40` is rejecting this faction's OWN dilemma, and that function is the
//                 whole remaining question
static constexpr size_t    OFF_TICKER_SRC      = 0x138;        // ticker -> the SOURCE container
static constexpr uintptr_t RVA_CCO_REFRESH_NOW = 0x0057B720;   // FUN_14057B720(sub) — unconditional
static constexpr size_t    OFF_TICKER_CCO_SUB  = 0x50;         // the ticker's level-1 sub-object

// Through `MOV byte [RCX+0x40],0` — which is the semantic signature, not just a prologue. Read out
// of the image as bytes; `PUSH RBX` here is `40 53`, the redundant-REX form that cost a deploy today.
static const uint8_t EXPECT_CCO_REFRESH_NOW[13] = {
    0x40, 0x53,                                 // PUSH RBX
    0x48, 0x83, 0xEC, 0x20,                     // SUB RSP,0x20
    0x48, 0x8B, 0xD9,                           // MOV RBX,RCX
    0xC6, 0x41, 0x40, 0x00,                     // MOV byte ptr [RCX+0x40],0x0
};

typedef void (*CcoRefreshNowFn)(void* sub);

static volatile long g_srcRebuildReq = 0;
static volatile long g_inSrcRebuild  = 0;
static volatile long g_srcRebuilds   = 0;

// The source container and its count, straight off the ticker. Separate from feedListSize because
// this reads the TICKER, not the feed context — the two hold the same pointer but only while a
// context exists, and level 1 is meaningful even when level 2 has not been built.
static bool tickerSourceCount(uintptr_t ticker, uintptr_t* contOut, uint32_t* countOut)
{
    uintptr_t cont = 0;
    if (!readAt(ticker + OFF_TICKER_SRC, cont) || cont <= 0x10000) return false;
    uint32_t n = 0;
    if (!readAt(cont + OFF_COLL_COUNT, n) || n > 4096) return false;
    *contOut = cont;
    *countOut = n;
    return true;
}

static bool callCcoRefreshNow(uintptr_t sub)
{
    __try {
        ((CcoRefreshNowFn)(g_base + RVA_CCO_REFRESH_NOW))((void*)sub);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Drained from feedTickHook — UI thread, frame rate, and the ticker is by definition alive there.
static void drainSourceRebuild()
{
    if (g_srcRebuildReq == 0) return;
    if (InterlockedCompareExchange(&g_inSrcRebuild, 1, 0) != 0) return;
    const long want = InterlockedExchange(&g_srcRebuildReq, 0);
    if (want) {
        uintptr_t contBefore = 0, contAfter = 0;
        uint32_t  nBefore = 0, nAfter = 0;
        const bool okBefore = tickerSourceCount(g_feedSelf, &contBefore, &nBefore);
        uint8_t    dirty = 0;
        readAt(g_feedSelf + 0x90, dirty);

        logf("★★★★★ FEED REBUILD (level 1): calling FUN_14057B720(ticker+0x50) on the UI thread. "
             "ticker=%016llX, level-1 dirty +0x90=%u, source %s%u entr%s. This forces "
             "FUN_142FC0770 to rebuild the source from model+0x3D30 for the LOCAL FACTION.",
             (unsigned long long)g_feedSelf, dirty, okBefore ? "" : "UNREADABLE, ",
             nBefore, nBefore == 1 ? "y" : "ies");

        const bool ok = callCcoRefreshNow(g_feedSelf + OFF_TICKER_CCO_SUB);
        InterlockedIncrement(&g_srcRebuilds);
        const bool okAfter = tickerSourceCount(g_feedSelf, &contAfter, &nAfter);

        if (!ok)
            logf("FEED REBUILD: ⚠⚠ THE CALL FAULTED and was swallowed. FUN_142FC0770 is not safe to "
                 "drive from here — record it and do not retry blind.");
        else if (!okAfter)
            logf("FEED REBUILD: returned, but the source is unreadable afterwards. Report that — it "
                 "is worse than a fault because it is silent.");
        else if (nAfter > nBefore)
            logf("★★★★★ FEED REBUILD: ✓✓ THE SOURCE FILLED — %u -> %u entr%s (container %016llX -> "
                 "%016llX). ⇒ **B1 IS A STALE LEVEL-1 CACHE.** The model had this faction's event all "
                 "along and nothing invalidated the source. The fix is to invalidate on the decision "
                 "push. Now force level 2 (`feed dirty` + `feed refresh`) and expect a box.",
                 nBefore, nAfter, nAfter == 1 ? "y" : "ies",
                 (unsigned long long)contBefore, (unsigned long long)contAfter);
        else
            logf("FEED REBUILD: the rebuild RAN and the source is still %u (container %016llX -> "
                 "%016llX). ⇒ ★★★ `FUN_1414E0E40(model+0x3D30, out, localFaction, …)` looked at the "
                 "campaign's items and gave this faction NOTHING — while a dilemma it owns is "
                 "outstanding. That function is then the whole remaining question, and B1 is a "
                 "MODEL-side membership decision, not a UI cache.",
                 nAfter, (unsigned long long)contBefore, (unsigned long long)contAfter);
    }
    InterlockedExchange(&g_inSrcRebuild, 0);
}

bool requestSourceRebuild(char* why, size_t cap)
{
    if (!g_feedSelf) {
        _snprintf_s(why, cap, _TRUNCATE,
                    "the event-feed ticker has not been seen yet — no campaign, or the ticker hook "
                    "is not installed. Nothing to rebuild.");
        return false;
    }
    uint8_t site[sizeof(EXPECT_CCO_REFRESH_NOW)] = { 0 };
    if (!safeRead((void*)(g_base + RVA_CCO_REFRESH_NOW), site, sizeof(site)) ||
        memcmp(site, EXPECT_CCO_REFRESH_NOW, sizeof(site)) != 0) {
        _snprintf_s(why, cap, _TRUNCATE,
                    "REFUSING — the bytes at RVA 0x%05llX are not FUN_14057B720's prologue on this "
                    "build. Do not call it.", (unsigned long long)RVA_CCO_REFRESH_NOW);
        return false;
    }
    InterlockedExchange(&g_srcRebuildReq, 1);
    _snprintf_s(why, cap, _TRUNCATE,
                "queued — the ticker hook will call FUN_14057B720(ticker+0x50) on the UI thread "
                "within a frame. Signature verified. Read the log for the `FEED REBUILD:` line: the "
                "source count before vs after IS the answer.");
    return true;
}

// =================================================================================================
bool markFeedDirty(char* why, size_t cap)
{
    if (!g_aoLastSelf) {
        _snprintf_s(why, cap, _TRUNCATE,
                    "no event-feed context has been seen yet — the listener has never been called on "
                    "this machine, so there is nothing to mark. Load a campaign first.");
        return false;
    }

    uint32_t n = 0; uintptr_t d = 0;
    if (!feedListSize((void*)g_aoLastSelf, &n, &d)) {
        _snprintf_s(why, cap, _TRUNCATE,
                    "the cached context %016llX no longer reads as one — REFUSING to write. Treat "
                    "this as the pointer being stale, not the game being odd.",
                    (unsigned long long)g_aoLastSelf);
        return false;
    }

    uint8_t before = 0;
    readAt(g_aoLastSelf + OFF_FEED_DIRTY, before);
    if (!writeByteGuarded(g_aoLastSelf + OFF_FEED_DIRTY, 1)) {
        _snprintf_s(why, cap, _TRUNCATE, "the write to %016llX+0x88 faulted — nothing changed.",
                    (unsigned long long)g_aoLastSelf);
        return false;
    }

    InterlockedIncrement(&g_flForced);
    noteFeedMarker("our `feed dirty` (self+0x88 set by hand)");
    logf("★★★★★ FEED DIRTY: set self+0x88 on context %016llX (was %u), list currently holds %u "
         "entr%s. B1 CANDIDATE FIX. The next query to either list getter should now recompute "
         "instead of short-circuiting. WATCH FOR: an `EVENT FEED DIRTY: 1 -> 0` line, then an "
         "`EVENT FEED LIST` change dated against this. ⚠ If the flag clears and the count does NOT "
         "move, the cache was not the problem — the source this recomputes FROM is also stale, and "
         "the target moves upstream of this context.",
         (unsigned long long)g_aoLastSelf, before, n, n == 1 ? "y" : "ies");

    _snprintf_s(why, cap, _TRUNCATE,
                "marked context %016llX stale (self+0x88 was %u, now 1; list holds %u). The next "
                "query recomputes. Forced %ld time(s) this session.",
                (unsigned long long)g_aoLastSelf, before, n, g_flForced);
    return true;
}

static void nextAutoOpenHook(void* self, void* sink, NextAutoOpenFn original)
{
    InterlockedIncrement(&g_aoCalls);
    g_aoLastSelf = (uintptr_t)self;
    noteFeedListSample(self, "listener entry");

    uint32_t   before = 0, after = 0, curId = 0, myId = 0;
    const bool okB    = readAt((uintptr_t)self + OFF_AO_ACCEPTED, before);
    const bool okTurn = feedTurnIds(&curId, &myId);

    if (original) original(self, sink);

    const bool okA = readAt((uintptr_t)self + OFF_AO_ACCEPTED, after);
    noteFeedListSample(self, "listener exit");

    // 0 = turn guard refused · 1 = walked, accepted nothing · 2 = produced · 3 = unreadable
    int kind = 3;
    if (okTurn && curId != myId)      kind = 0;
    else if (okB && okA)              kind = (after > before) ? 2 : 1;
    if (kind == 0) InterlockedIncrement(&g_aoNoTurn);
    if (kind == 1) InterlockedIncrement(&g_aoEmpty);
    if (kind == 2) InterlockedIncrement(&g_aoProduced);

    if (kind == g_aoLastKind || !g_feedVerbose) return;
    g_aoLastKind = kind;

    switch (kind) {
        case 0:
            logf("AUTO-OPEN: refused at the TURN GUARD — current faction id=%08X, mine=%08X. The "
                 "walk was never entered.", curId, myId);
            break;
        case 1: {
            uint32_t  n = 0;
            uintptr_t d = 0;
            const bool okList = feedListSize(self, &n, &d);
            logf("AUTO-OPEN: ★★★ the guard PASSED and the walk accepted NOTHING (accepted count "
                 "stayed %u).", before);
            if (!okList) {
                logf("   the recompute's SOURCE is UNREADABLE — reported rather than guessed, because "
                     "'unknown' and 'empty' are different answers here.");
            } else if (n == 0) {
                logf("   ★★★ the recompute's SOURCE at self+0x90 holds ZERO entries. ⇒ there was "
                     "nothing to walk, so this is NOT a filter refusing the dilemma — the event "
                     "NEVER REACHED THE FEED on this client. The target is upstream of this "
                     "function entirely.");
            } else {
                // ⚠ CORRECTED for #57. This used to claim NONE of the entries survived to the clause
                // tests, which it cannot know: an entry refused *at* the exclusion test looks
                // identical from here, and test-host's `exclusion calls=2` says entries did reach it.
                // Say what was measured — nothing was accepted — and leave the WHERE to the counter
                // that can actually answer it.
                logf("   ★★★ the recompute's SOURCE holds %u entr%s at %016llX, and NOTHING WAS "
                     "ACCEPTED. ⇒ the list was not empty, so this is a refusal rather than an absent "
                     "event — but WHERE it refused is not visible from here. With #56 containment "
                     "installed, the shared-leaf probe cannot observe this routed IsRead call.",
                     n, n == 1 ? "y" : "ies", (unsigned long long)d);
                for (uint32_t i = 0; i < n && i < 8; ++i) {
                    uint64_t w0 = 0, w1 = 0;
                    readAt(d + (uintptr_t)i * COLL_STRIDE, w0);
                    readAt(d + (uintptr_t)i * COLL_STRIDE + 8, w1);
                    logf("      [%u] %016llX %016llX", i,
                         (unsigned long long)w0, (unsigned long long)w1);
                }
            }
            break;
        }
        case 2:
            logf("AUTO-OPEN: produced a context (accepted count %u -> %u). ⇒ this function did its "
                 "job; if no box appeared, the failure is DOWNSTREAM of it and the target moves to "
                 "whatever consumes the returned context.", before, after);
            break;
        default:
            logf("AUTO-OPEN: state unreadable (self=%016llX) — reported rather than left silent, so "
                 "a gap is never read as a match.", (unsigned long long)self);
            break;
    }
}

// =================================================================================================
//  ★★★ THE PREDICATE THAT REFUSES — `FUN_1414D8610(record, faction)`
//
//  §6uuu.1's probe measured the split and it came back `produced=0` on the stuck client against
//  `produced=1` on a healthy one in the same minutes: the guard passed, the walk ran, and it accepted
//  NOTHING with a dilemma outstanding. So the refusal is in the three clauses, and of those exactly
//  one takes the LOCAL FACTION — which is the only way a per-client failure can arise from a record
//  every client holds identically.
//
//  The whole function, 43 bytes:
//
//      data = *(void**)(record + 0x98);
//      end  = data + *(uint*)(record + 0x94);
//      return <faction pointer is present in that array>;
//
//  A plain membership test. And the accept condition around it is
//
//      if (cVar5 != 0 && (cVar7 == 0 || cVar6 != 0))       // cVar7 = this function
//
//  ⇒ the event is accepted only when the local faction is **NOT** in the record's array. ⚠ INFERRED,
//  and the inference is the thing this probe exists to test: an array of faction pointers used as an
//  exclusion reads like "factions that have already had this", but nothing names it and this codebase
//  has been wrong six times about mechanisms that read just as naturally.
//
//  ★ WHY IT FITS ALL THREE OF tester'S OBSERVATIONS: the array lives on the RECORD, so it is model
//  state and is serialised — which is exactly why reloading the previous turn reproduces B1 rather
//  than re-rolling it ("predetermined by the time the game is saved"); membership is per faction, so
//  it fails on one client; and nothing in it is type-specific, so different events can be affected.
//
//  ⚠ FILTERED BY RETURN ADDRESS. There are five callers and only the auto-open walk is interesting,
//  so the hook compares `_ReturnAddress()` against the call site's return address (142F69958) and
//  ignores everything else — the session-5l technique, used here to keep a shared leaf quiet rather
//  than to identify an unknown caller.
// =================================================================================================

static constexpr uintptr_t RVA_FACTION_IN_LIST = 0x014D8610;   // FUN_1414D8610(record, faction)
static constexpr uintptr_t RVA_AO_CALLSITE_RET = 0x02F72208;   // the CALL's return address in the walk
static constexpr size_t    OFF_REC_EXCL_COUNT  = 0x94;
static constexpr size_t    OFF_REC_EXCL_DATA   = 0x98;

// 17 bytes = 3 whole instructions, ending on the `LEA R8,[RAX+RCX*8]` boundary. None RIP-relative,
// and — checked, because the function is only 43 bytes long — **no branch target lands inside the
// stolen range**: the three jumps go to 0x766, 0x774 and 0x777, all past 0x761.
static constexpr size_t FIL_STOLEN_LEN = 17;
static const uint8_t EXPECT_FACTION_IN_LIST[FIL_STOLEN_LEN] = {
    0x48, 0x8B, 0x81, 0x98, 0x00, 0x00, 0x00,   // MOV RAX, [RCX+0x98]
    0x8B, 0x89, 0x94, 0x00, 0x00, 0x00,         // MOV ECX, [RCX+0x94]
    0x4C, 0x8D, 0x04, 0xC8,                     // LEA R8, [RAX+RCX*8]
};

typedef uint64_t (*FactionInListFn)(void* record, void* faction);
static FactionInListFn g_origFactionInList = nullptr;
static Detour          g_filDetour;

static volatile long g_filCalls    = 0;   // calls from the auto-open walk only
static volatile long g_filExcluded = 0;   // ...that answered "yes, this faction is in the list"
static uintptr_t     g_filLastRec  = 0;
static int           g_filLastAns  = -1;

static uint64_t factionInListHook(void* record, void* faction)
{
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    const uint64_t  r   = g_origFactionInList ? g_origFactionInList(record, faction) : 0;

    // Four of the five callers are not this question. Answer them and say nothing.
    if (ret != g_base + RVA_AO_CALLSITE_RET) return r;

    InterlockedIncrement(&g_filCalls);
    const int ans = (int)(r & 1);
    if (ans) InterlockedIncrement(&g_filExcluded);

    // Change-triggered on (record, answer): the walk asks once per candidate event per query, and a
    // polled query would otherwise flood. A NEW record is always worth a line even if the answer
    // matches the last one.
    if ((uintptr_t)record == g_filLastRec && ans == g_filLastAns) return r;
    g_filLastRec = (uintptr_t)record;
    g_filLastAns = ans;

    if (!g_feedVerbose) return r;

    uint32_t myId = 0;
    readAt((uintptr_t)faction + OFF_FACTION_ID, myId);

    logf("EXCLUSION: record=%016llX  my faction=%016llX id=%08X  ->  %s",
         (unsigned long long)record, (unsigned long long)faction, myId,
         ans ? "IN THE LIST — this event will be SKIPPED unless the override says otherwise"
             : "not in the list — this event may be shown");

    // Who else is on it. This is the line that says whether the list is what it looks like.
    uint32_t  n   = 0;
    uintptr_t arr = 0;
    if (readAt((uintptr_t)record + OFF_REC_EXCL_COUNT, n) &&
        readAt((uintptr_t)record + OFF_REC_EXCL_DATA, arr)) {
        if (n > 64 || (n && arr <= 0x10000)) {
            logf("   list reads count=%u ptr=%016llX — REFUSING to walk it; those are not credible, "
                 "so treat the offsets as wrong rather than the game as odd.",
                 n, (unsigned long long)arr);
            return r;
        }
        logf("   list holds %u faction(s):", n);
        for (uint32_t i = 0; i < n && i < 16; ++i) {
            uintptr_t f = 0; uint32_t fid = 0;
            if (!readAt(arr + (uintptr_t)i * 8, f) || !f) continue;
            readAt(f + OFF_FACTION_ID, fid);
            logf("      [%u] %016llX id=%08X%s", i, (unsigned long long)f, fid,
                 f == (uintptr_t)faction ? "   <<< ME" : "");
        }
    }
    return r;
}

bool installFactionInListHook()
{
    return detourInstall(g_filDetour, g_base + RVA_FACTION_IN_LIST, FIL_STOLEN_LEN,
                         EXPECT_FACTION_IN_LIST, (uintptr_t)&factionInListHook,
                         (void**)&g_origFactionInList, "auto-open exclusion test");
}

void removeFactionInListHook() { detourRemove(g_filDetour, "auto-open exclusion test"); }

void reportFactionInList()
{
    if (eventReadInstalled())
        logf("  #56 routes the auto-open CALL through a helper: this leaf probe's return-address "
             "filter misses it; zero calls means UNOBSERVED, not absent candidates.");
    logf("---- AUTO-OPEN EXCLUSION TEST (B1: is MY faction on the record's list?) ----");
    if (!g_filDetour.active) { logf("  hook NOT installed on this machine."); return; }
    logf("  calls from the walk=%ld  |  answered EXCLUDED=%ld", g_filCalls, g_filExcluded);
    if (eventReadInstalled()) {
        logf("  The counts above cover unrouted observations only; current auto-open eligibility "
             "cannot be inferred from this return-address-filtered probe.");
        return;
    }
    if (g_filCalls == 0)
        logf("  ⇒ the walk never reached this test. Either it had no candidate event at all, or it "
             "refused earlier — `feed`'s listener split says which, and that is a DIFFERENT finding "
             "from the one this hook was built for.");
    else if (g_filExcluded == 0)
        logf("  ⇒ ★ the walk asked and was never told to exclude, so the refusal is one of the OTHER "
             "two clauses — `*(recordDef+0x40)` or `FUN_1414D2A80`. The inference this hook tests is "
             "then WRONG, which is worth as much as confirming it.");
    else
        // ❌★ #58 — THIS USED TO CLAIM B1. It said: "excluded N times, with a dilemma outstanding
        // and the listener accepting nothing, that is B1's refusal named."
        //
        // It is not. The exclusion array is *"factions that have already had this event"* — proven
        // 2026-08-13 by a before/after on one record — so a non-zero count is the ORDINARY reading on
        // any machine that has been playing, and it grows all session. Worse, the state it fired on
        // (decision outstanding · listener accepting nothing · non-zero exclusions) is exactly the
        // state of a HEALTHY machine sitting on an open dilemma box. It was measured naming B1 on a
        // healthy single-player campaign the same day it was filed.
        //
        // ⇒ The distinguishing fact is whether an excluded event was ever PRESENTED here, and
        //   nothing tracks that. So report the count and stop; do not dress a tally as a diagnosis.
        logf("  ⇒ excluded %ld time(s) — ⚠ EXPECTED, and not by itself a finding: the array is "
             "\"factions that have already had this event\", so it grows all session on any machine "
             "that is playing normally. It becomes evidence only for an event that was NEVER SHOWN "
             "here, which nothing currently records.", g_filExcluded);
}

// =================================================================================================
//  ★★★★★★ #60 — THE NOTIFICATION MOMENT, which no probe has ever seen
//
//  `FUN_142FCC780` is the observer callback at `ticker+0xD8`. It rebuilds the feed context from
//  `*(ticker+0x138)` and raises `ticker+0x14C` — and its ENTIRE body is gated on one predicate:
//
//      142FCC78C  MOV  RCX,[0x1443CFA50]
//      142FCC793  CALL 0x142F6BF80          <- the gate
//      142FCC798  TEST AL,AL                <- our return address filter
//      142FCC79A  JZ   <skip everything>
//
//      FUN_142F6BF80(root) = *(u8*)( root->0x2188->0x78->0x3B68->0x48 + 0xCD0 )
//                                   //   the CURRENT faction ──────────┘   └── the HUMAN flag
//
//  ⇒ the feed is notified ONLY while the faction holding the turn is human, and nothing retries.
//
//  ★★★ WHY THIS IS THE ONE TO MEASURE: **in single player that gate is the same test as the
//  listener's own guard.** `FUN_142F72090` refuses unless `local == current`; with one human,
//  "current is human" implies "current is mine". They come apart ONLY IN COOP — another human
//  holding the turn passes the gate and fails the guard, so `+0x14C` is set, `EventFeedAutoOpen`
//  fires, the listener refuses, and the event is spent. Coop-specific by construction, which none of
//  the six dead mechanisms were.
//
//  ⚠⚠ AND IT CONTRADICTS §6uuu.1, which measured no `SPENT OUT OF TURN` on the fire after the stuck
//  dilemma's push with `current == mine` live. This hook is what settles that, either way.
//
//  ✗ WHY NOT HOOK `FUN_142FCC780` ITSELF: its 4th instruction is `MOV RCX,[rip+…]`, and stopping
//  before it yields only 12 bytes — under the 14-byte minimum. Everything past it is RIP-relative or
//  a rel32 branch, both of which break when relocated into a trampoline. The gate is a 30-byte leaf
//  with no branches at all, so we hook that and filter on the CALL's return address — the 5l
//  technique, already used by `factionInListHook`.
// =================================================================================================

static constexpr uintptr_t RVA_FEED_GATE  = 0x02F6BF80;   // FUN_142F6BF80(root)
static constexpr uintptr_t RVA_NOTIFY_RET = 0x02FCC798;   // the CALL's return address inside 142FCC780

// 18 bytes = 3 whole instructions, none RIP-relative, and the function contains NO branches at all,
// so nothing can land inside the stolen range. Read out of the image as bytes, not transcribed from
// a listing — the `40 53` lesson from earlier today.
static constexpr size_t GATE_STOLEN_LEN = 18;
static const uint8_t EXPECT_FEED_GATE[GATE_STOLEN_LEN] = {
    0x48, 0x8B, 0x81, 0x88, 0x21, 0x00, 0x00,   // MOV RAX,[RCX+0x2188]
    0x48, 0x8B, 0x48, 0x78,                     // MOV RCX,[RAX+0x78]
    0x48, 0x8B, 0x81, 0x68, 0x3B, 0x00, 0x00,   // MOV RAX,[RCX+0x3B68]
};

typedef uint8_t (*FeedGateFn)(uintptr_t root);
static FeedGateFn g_origFeedGate = nullptr;
static Detour     g_feedGateDetour;

static volatile long g_notifyCalls   = 0;   // times the observer callback reached its gate
static volatile long g_notifyPassed  = 0;   // ...and was allowed to notify the feed
static volatile long g_notifyRefused = 0;   // ...and was NOT — the event is dropped, nothing retries
static int           g_notifyLastAns = -1;

static uint8_t feedGateHook(uintptr_t root)
{
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    const uint8_t   r   = g_origFeedGate ? g_origFeedGate(root) : 0;

    // This predicate is a shared leaf. Only the call from the observer callback is this question.
    if (ret != g_base + RVA_NOTIFY_RET) return r;

    InterlockedIncrement(&g_notifyCalls);
    const int ans = r ? 1 : 0;
    if (ans) InterlockedIncrement(&g_notifyPassed);
    else     InterlockedIncrement(&g_notifyRefused);

    if (ans == g_notifyLastAns || !g_feedVerbose) return r;
    g_notifyLastAns = ans;

    // Everything that decides the answer, at the instant it was decided.
    uint32_t curId = 0, myId = 0;
    const bool okTurn = feedTurnIds(&curId, &myId);
    uint8_t    b14C   = 0;
    if (g_feedSelf) readAt(g_feedSelf + OFF_FEED_B14C, b14C);

    uint32_t  n = 0, oa = 0, ob = 0;
    uintptr_t d = 0;
    const bool okSrc = g_aoLastSelf && feedListSize((void*)g_aoLastSelf, &n, &d);
    const bool okOut = g_aoLastSelf && feedOutputCounts((void*)g_aoLastSelf, &oa, &ob);

    logf("★★★★★ FEED NOTIFY: the observer fired and the gate said %s. current faction id=%08X, "
         "mine=%08X%s  |  ticker+0x14C=%u  |  source=%s  outputs=%s",
         ans ? "YES" : "NO", curId, myId,
         okTurn ? (curId == myId ? " (MINE)" : " (ANOTHER FACTION'S TURN)") : " (unreadable)",
         b14C, okSrc ? "readable" : "unreadable", okOut ? "readable" : "unreadable");
    if (okSrc || okOut)
        logf("      source holds %u · outputs +0x9C=%u +0xAC=%u", n, oa, ob);

    if (!ans)
        logf("      ⇒ ★★★ REFUSED. The feed is NOT being told this event exists, no context is "
             "rebuilt, `+0x14C` is not raised, and NOTHING RETRIES — the observer is edge-driven. "
             "If a dilemma arrived in this window it is lost until something else marks the context "
             "stale. **That is B1's candidate mechanism caught in the act.**");
    else if (okTurn && curId != myId)
        logf("      ⇒ ⚠⚠ PASSED, BUT IT IS NOT OUR TURN. This is the coop-only divergence: the gate "
             "asks 'is the current faction HUMAN', the listener asks 'is it MINE'. Expect "
             "`EventFeedAutoOpen` to fire and produce nothing — watch for SPENT OUT OF TURN next.");
    else
        logf("      ⇒ passed on our own turn — the ordinary healthy path, and in single player the "
             "only one that exists.");
    return r;
}

bool installFeedGateHook()
{
    return detourInstall(g_feedGateDetour, g_base + RVA_FEED_GATE, GATE_STOLEN_LEN,
                         EXPECT_FEED_GATE, (uintptr_t)&feedGateHook,
                         (void**)&g_origFeedGate, "event-feed notify gate");
}

void removeFeedGateHook() { detourRemove(g_feedGateDetour, "event-feed notify gate"); }

void reportFeedGate()
{
    logf("---- FEED NOTIFY GATE (#60: is the feed even TOLD the event exists?) ----");
    if (!g_feedGateDetour.active) { logf("  hook NOT installed on this machine."); return; }
    logf("  observer reached the gate=%ld  |  PASSED=%ld  REFUSED=%ld",
         g_notifyCalls, g_notifyPassed, g_notifyRefused);
    if (g_notifyCalls == 0)
        logf("  ⇒ ★★★ the observer NEVER FIRED. The model is not notifying this client at all, and "
             "the target moves further upstream — to the four registrations in FUN_142FBFE20 "
             "(model+0x29B8 / +0x528 / +0x1450 / +0x2DC8). That is a DIFFERENT finding from a "
             "refusing gate, and a bigger one.");
    else if (g_notifyRefused == 0)
        logf("  ⇒ the gate has never refused here. Against a session with a confirmed B1 that "
             "WEAKENS the hypothesis — the notification was delivered and the loss is downstream.");
    else
        logf("  ⇒ ★★★ refused %ld time(s). Each refusal is an event the feed was never told about. "
             "Against a dilemma that arrived in the same window, that is B1 named.", g_notifyRefused);
}

bool installNextAutoOpenHook()
{
    if (!eventReadInstalled()) return false;
    setNextAutoOpenObserver(&nextAutoOpenHook);
    g_autoOpenObserverRegistered = true;
    return true;
}

void removeNextAutoOpenHook() {
    setNextAutoOpenObserver(nullptr);
    g_autoOpenObserverRegistered = false;
}

void reportNextAutoOpen()
{
    logf("---- AUTO-OPEN LISTENER (B1: did the box get ASKED for, and did anything answer?) ----");
    if (!g_autoOpenObserverRegistered || !eventReadInstalled()) { logf("  hook NOT installed on this machine."); return; }
    logf("  calls=%ld  |  refused-at-turn-guard=%ld  accepted-nothing=%ld  produced=%ld",
         g_aoCalls, g_aoNoTurn, g_aoEmpty, g_aoProduced);

    // ★ #57 — the watch's own tally. `fills=0` on a machine that has shown event boxes all session
    // means the watch is not seeing the object the fill happens on, which is a defect in the probe
    // and not a finding about the game. Printed first so that reading is available before the rest.
    logf("  SOURCE changes: grew=%ld  shrank=%ld  |  contexts watched=%ld, now %016llX at %lld "
         "entr%s", g_flFills, g_flDrains, g_flBaselines, (unsigned long long)g_flWatchSelf,
         (long long)g_flWatchCount, g_flWatchCount == 1 ? "y" : "ies");
    if (g_flFills == 0 && g_aoCalls > 0)
        // ❌ Also corrected for #58's class. This used to call `grew=0` a PROBE DEFECT. It is not:
        // `+0x90` is a constructor ARGUMENT (§6uuu.11), so it is normally already populated by the
        // time we first sample it, and a session can legitimately see it never change. Say what the
        // counter means and leave the diagnosis to the source/output pair below.
        logf("  ⇒ the SOURCE has not been seen to change on this machine. ⚠ Ordinary — `+0x90` is "
             "handed to the context at construction, so it is usually already populated before the "
             "first sample. Read the source/output pair below instead; that is the line that "
             "distinguishes an empty source from a recompute that rejected everything.");

    // The list size RIGHT NOW, from the last object the engine handed us. This is the line that
    // separates "the feed never got the event" from "the feed had it and skipped it", and it is a
    // plain read, so it costs nothing to print even when nothing is wrong.
    if (g_aoLastSelf) {
        uint32_t  n = 0;
        uintptr_t d = 0;
        if (feedListSize((void*)g_aoLastSelf, &n, &d)) {
            // ★ The dirty flag, live. Added after a session spent reading it with `read <addr>`
            // by hand: this report is where somebody looks, and the flag is now the first thing
            // that matters about this object.
            uint8_t dirty = 0;
            const bool okD = readAt(g_aoLastSelf + OFF_FEED_DIRTY, dirty);
            logf("  SOURCE (+0x90) holds %u entr%s right now (self=%016llX), stale flag "
                 "+0x88 = %s", n, n == 1 ? "y" : "ies", (unsigned long long)g_aoLastSelf,
                 !okD ? "UNREADABLE"
                      : dirty ? "1 — MARKED STALE, and the next query would recompute"
                              : "0 — CLEAN, so every query short-circuits and the list cannot change");
            // ❌★ CORRECTED WITHIN THE HOUR. The first version of this line said "clean AND empty
            // ⇒ that pair is B1". It is NOT: measured on all three machines at once, every one of
            // them reads clean-and-empty at rest — including a HEALTHY client that had just produced
            // an auto-open context. An empty feed list is the ordinary resting state between events,
            // so the pair carries no information by itself and the line named B1 on healthy
            // machines. Exactly the defect filed as #58, introduced by me while filing it.
            // ⇒ The discriminator needs the other two facts: a decision outstanding, and OURS.
            // ★ #61 — the outputs, beside the input. `source N / outputs 0` and `source 0 /
            // outputs 0` are completely different findings and nothing sampled them until now.
            uint32_t oa = 0, ob = 0;
            if (feedOutputCounts((void*)g_aoLastSelf, &oa, &ob))
                logf("  recompute OUTPUTS: +0x9C=%u  +0xAC=%u%s", oa, ob,
                     (n > 0 && oa == 0 && ob == 0)
                         ? "   ⇒ ★★★ the SOURCE HAS ENTRIES AND BOTH OUTPUTS ARE EMPTY: the "
                           "recompute rejected every one of them. The failure is in its two "
                           "filters, not upstream."
                     : (n == 0 && oa == 0 && ob == 0)
                         ? "   ⇒ source empty too — nothing to show, so the failure is UPSTREAM of "
                           "this context (what fills *(ticker+0x138))."
                         : "");
            else
                logf("  recompute OUTPUTS: unreadable — 'unknown', not 'zero'.");

            if (okD && n == 0) {
                DilemmaView dv; char why[192] = { 0 };
                const bool pending = resolveDilemma(dv, why, sizeof(why));
                if (pending && dv.owned && dv.answer < 0)
                    logf("  ⇒ ★★★ empty (flag %u) WHILE THIS MACHINE OWNS AN OUTSTANDING DECISION "
                         "(faction %llu, %d options). THAT is B1 — an empty feed is only meaningful "
                         "against a dilemma this client should be showing.",
                         dirty, (unsigned long long)dv.recFactionId, dv.options);
                else
                    logf("  ⇒ empty and flag %u, with %s. ⚠ ORDINARY — an empty list at rest is the "
                         "resting state on every machine, healthy ones included. Not a finding.",
                         dirty, pending ? (dv.owned ? "a decision already answered"
                                                    : "a decision owned by another machine")
                                        : why);
            }
        }
        else
            logf("  the SOURCE is unreadable right now (self=%016llX) — 'unknown', not "
                 "'empty'.", (unsigned long long)g_aoLastSelf);
    }
    if (g_aoCalls == 0)
        logf("  ⇒ never called. Outside a campaign that is normal; inside one it means "
             "`EventFeedAutoOpen` is not reaching this listener at all, which is its own finding.");
    else if (g_aoProduced == 0)
        logf("  ⇒ ★ this client has NEVER produced an auto-open context. Against a known count of "
             "dilemmas that arrived, that is B1 — and the split above says which half.");
}

bool installFeedTickHook()
{
    return detourInstall(g_feedTickDetour, g_base + RVA_FEED_TICK, FEEDTICK_STOLEN_LEN,
                         EXPECT_FEED_TICK, (uintptr_t)&feedTickHook,
                         (void**)&g_origFeedTick, "event feed ticker");
}

void removeFeedTickHook() { detourRemove(g_feedTickDetour, "event feed ticker"); }

void setFeedLogging(bool on)
{
    g_feedVerbose = on;
    logf("EVENT FEED: per-transition logging %s. The counts accrue either way — `feed` reports them.",
         on ? "ON" : "OFF");
}

void reportFeedTicker()
{
    logf("---- EVENT FEED TICKER (B1: was the box ever ASKED for?) ----");
    if (!g_feedTickDetour.active) {
        logf("  hook NOT installed — no reading available on this machine.");
        return;
    }
    logf("  ticked object=%016llX   per-transition logging=%s",
         (unsigned long long)g_feedSelf, g_feedVerbose ? "on" : "off");
    logf("  calls=%ld  inert=%ld  throttled=%ld  |  raised(A)=%ld  opened(B)=%ld  refused=%ld  "
         "spent-out-of-turn=%ld",
         g_feedCalls, g_feedInert, g_feedThrott, g_feedRaised, g_feedOpened, g_feedRefused,
         g_feedSpentOOT);
    if (g_feedSpentOOT)
        logf("  ★ %ld of those %ld opens fired while ANOTHER faction held the turn, so "
             "`NextAutoOpenEventContext` refused and they produced nothing. Harmless when no decision "
             "was outstanding; when one was, that is the B1 event being burnt (§6uuu).",
             g_feedSpentOOT, g_feedOpened);

    uint8_t  c14C = 0, c14D = 0;
    uint32_t cT = 0;
    if (g_feedSelf && readAt(g_feedSelf + OFF_FEED_B14C, c14C) &&
        readAt(g_feedSelf + OFF_FEED_B14D, c14D) && readAt(g_feedSelf + OFF_FEED_T148, cT))
        logf("  right now: +0x14C=%u +0x14D=%u throttle=%u  ⚠ these three bytes look IDENTICAL in a "
             "healthy feed and a stuck one; they are logged for the record, not as evidence.",
             (unsigned)c14C, (unsigned)c14D, cT);

    if (g_feedCalls == 0) {
        logf("  ⇒ the ticker has not run at all. Outside a campaign that is simply normal.");
        return;
    }
    if (g_feedOpened == 0)
        logf("  ⇒ ★ NO box has ever been opened by this client this session. Against a known count "
             "of dilemmas that arrived, that is B1 — and raised(A) says WHICH HALF: 0 means nothing "
             "ever asked for a box, non-zero means it asked and branch B never delivered.");
    else
        logf("  ⇒ this client has opened %ld box(es), so the cycle does complete here.", g_feedOpened);
    logf("  ⚠ `inert` is the HEALTHY resting state (both flags zero, no branch body reachable), so a "
         "large count means nothing by itself. Read it against raised/opened, never alone.");
}

// The nine names the gate tests, in its own order, with the bit it tags each with. Read out of the
// decrypted image, not from a header — every RVA below was decoded from the LEA at that call site.
struct GatePanel { uintptr_t rva; const char* name; unsigned bit; };
static const GatePanel kGatePanels[] = {
    // ★ REBASED to 1.7.2 on 2026-09-20. The 1.7.1 values read 0/9 correctly on the new build, so
    // gateStringsVerified() disabled the probe on every machine in the first 3-player session.
    // Recovered by locating each literal in the 1.7.2 image: every one occurs EXACTLY ONCE, and
    // all nine moved from .sbss into .rdata.
    // ⚠ The shift is NOT uniform — eight moved by +0x3530 and `quest_details` by +0x30F8. Applying
    //   one delta to the table would have left that entry wrong, and a single wrong entry disables
    //   the whole probe, which is exactly the failure this comment exists to stop repeating.
    // Previous (1.7.1) values, kept so the next rebase can re-derive rather than re-discover:
    //   037E1048 037E0F10 037E0F50 037E1950 037E0C08 037E0C50 037E0ED0 0382FAE8 037E0B48
    { 0x037E4578, "undercover_network_panel", 0x001 },
    { 0x037E4440, "diplomacy_panel",          0x002 },
    { 0x037E4480, "diplomacy_popup",          0x004 },
    { 0x037E4E80, "faction_council_panel",    0x008 },
    { 0x037E4138, "pre_battle_screen",        0x010 },
    { 0x037E4180, "post_battle_screen",       0x020 },
    { 0x037E4400, "campaign_victory_defeat",  0x040 },
    { 0x03832BE0, "quest_details",            0x080 },
    { 0x037E4078, "unit_exchange",            0x100 },
};

// ⚠ VERIFY THE LITERAL BEFORE USING IT, exactly as every hook here verifies its prologue bytes. A
// wrong RVA would not fail loudly — it would hand the engine some other string and quietly report
// that no panel is open, which is the answer we are least able to distinguish from the truth.
static bool gateStringsVerified()
{
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 1;
    __try {
        for (const GatePanel& p : kGatePanels) {
            const char* s = (const char*)(g_base + p.rva);
            if (strcmp(s, p.name) != 0) {
                logf("EVENTFEED GATE: string at RVA_%08llX reads \"%.32s\", expected \"%s\" — this "
                     "build is not the one those RVAs were decoded from. Probe DISABLED.",
                     (unsigned long long)p.rva, s, p.name);
                cached = 0;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("EVENTFEED GATE: faulted reading the panel name literals — probe DISABLED.");
        cached = 0;
    }
    return cached != 0;
}

// One panel, through the engine's own lookup — the same three calls the gate makes for itself.
// `ok` distinguishes "closed" from "could not tell", which are not the same answer.
// `panelPtr` receives out[0], the panel object the name resolved to, or 0. That is what makes a
// name comparable with an entry enumerated out of the manager's own array — the return value alone
// cannot do it, because it answers "topmost", not "open".
static bool panelIsOpen(const GatePanel& p, bool& ok, uintptr_t* panelPtr = nullptr)
{
    ok = false;
    if (panelPtr) *panelPtr = 0;
    __try {
        alignas(8) uint8_t ca[16]  = { 0 };
        alignas(8) uint8_t out[16] = { 0 };   // the frame spacing at the call sites says 0x10
        ((MakeCaStringFn)(g_base + RVA_MAKE_CASTRING))(ca, (const char*)(g_base + p.rva));

        auto  getMgr = (void*(*)())(g_base + RVA_PANEL_MGR_GET);
        auto  lookup = (void*(*)(void*, void*, void*))(g_base + RVA_PANEL_BY_NAME);
        void* mgr    = getMgr();                      // its result is passed but unused by the
        void* r      = lookup(mgr, out, ca);          // callee; called anyway, as the engine does
        if (!r) return false;
        ok = true;
        if (panelPtr) *panelPtr = *(uintptr_t*)r;     // out[0] = the panel, out[8] = topmost
        return *((uint8_t*)r + 8) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // ⚠ The CA string is deliberately NOT freed — same rule as gift.cpp's fallback labels. The
    // engine's free has never had its parameters committed and a wrong one corrupts the process
    // heap. This path runs at most nine times per episode, so the leak is bounded and cheap.
}

// How often the nine are re-checked while the gate STAYS shut, and how many times per refusal.
//
// ⚠ Each pass builds nine CA strings and deliberately leaks them (see panelIsOpen). At 5 s that is
// ~9 short strings per pass and the cap bounds the total, so a refusal left running overnight cannot
// become the per-frame allocation that once strangled the lobby (lobby.cpp's reverted name pass).
static constexpr uint64_t ATTRIB_INTERVAL_MS = 5000;
static constexpr int      ATTRIB_MAX_PASSES  = 240;    // ~20 minutes of watching, then stop and say so

// ★★★ Read the manager's open-panel array directly. This is the authoritative answer to "how many
// panels are open", and unlike the nine-name probe it CANNOT miss one: it does not depend on a
// name being in kGatePanels. Returns the entry count, or -1 if the manager could not be read.
//
// Read-only and allocation-free: the singleton is read out of its global rather than fetched with
// FUN_142D504E0, so a probe can never be the thing that constructs the manager.
static int enumerateOpenPanels(uintptr_t* out, int cap)
{
    __try {
        uintptr_t mgr = 0;
        if (!readAt(g_base + RVA_PANEL_MGR_SINGLETON, mgr) || mgr <= 0x10000) return -1;

        uint32_t  count = 0;
        uintptr_t arr   = 0;
        if (!readAt(mgr + OFF_PANELMGR_COUNT, count)) return -1;
        if (count == 0) return 0;                       // nothing open; arr may be anything
        if (count > (uint32_t)MAX_OPEN_PANELS) return -1;   // implausible: treat as unreadable
        if (!readAt(mgr + OFF_PANELMGR_ARRAY, arr) || arr <= 0x10000) return -1;

        int n = 0;
        for (uint32_t i = 0; i < count && n < cap; ++i) {
            uintptr_t p = 0;
            if (!readAt(arr + i * sizeof(uintptr_t), p) || p <= 0x10000) continue;
            out[n++] = p;
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// `mask` keeps its old meaning — a bit per NAMED panel that is open. What is new is `unnamed`:
// entries the engine reports as open that match none of the nine. That count is the whole point,
// because a refusal with all four numerics permitting and mask==0 used to be a contradiction with
// nowhere to go; now it reads as "N panels open, none of them ours" and names the gap.
static bool collectBlockingPanels(unsigned& mask, int& unreadable, int& openTotal, int& unnamed)
{
    if (!gateStringsVerified()) return false;
    mask = 0;
    unreadable = 0;
    unnamed = 0;

    uintptr_t open[MAX_OPEN_PANELS];
    openTotal = enumerateOpenPanels(open, MAX_OPEN_PANELS);

    // Resolve each known name to its panel object once, then match against what is actually open.
    uintptr_t known[sizeof(kGatePanels) / sizeof(kGatePanels[0])] = { 0 };
    int ki = 0;
    for (const GatePanel& p : kGatePanels) {
        bool ok = false;
        uintptr_t ptr = 0;
        const bool topmost = panelIsOpen(p, ok, &ptr);
        known[ki++] = ptr;
        if (!ok) ++unreadable;

        if (openTotal > 0 && ptr) {
            for (int i = 0; i < openTotal; ++i)
                if (open[i] == ptr) { mask |= p.bit; break; }
        } else if (openTotal < 0 && topmost) {
            // Enumeration unavailable — fall back to the old, weaker signal rather than go blind.
            mask |= p.bit;
        }
    }

    if (openTotal > 0) {
        for (int i = 0; i < openTotal; ++i) {
            bool named = false;
            for (int k = 0; k < ki; ++k)
                if (known[k] && known[k] == open[i]) { named = true; break; }
            if (!named) ++unnamed;
        }
    }
    return true;
}

static void panelNames(char* buf, size_t cap, unsigned mask)
{
    size_t w = 0;
    buf[0] = '\0';
    for (const GatePanel& p : kGatePanels) {
        if (!(mask & p.bit)) continue;
        const int k = _snprintf_s(buf + w, cap - w, _TRUNCATE, "%s\"%s\"", w ? ", " : "", p.name);
        if (k < 0) return;
        w += (size_t)k;
    }
    if (!buf[0]) _snprintf_s(buf, cap, _TRUNCATE, "(none)");
}

// `prev` is 0xFFFFFFFF for the first attribution of a refusal, so a change can be told from a start.
static void reportBlockingPanels(unsigned mask, int unreadable, unsigned prev, const GateNumerics& n,
                                 int openTotal, int unnamed)
{
    const int total = (int)(sizeof(kGatePanels) / sizeof(kGatePanels[0]));
    logGateNumerics(n);

    // The engine's own count comes first, because it is the only line here that cannot be wrong by
    // omission — every other line is filtered through the nine names we happen to know.
    if (openTotal >= 0)
        logf("    panel stack: the engine reports %d panel(s) open%s", openTotal,
             unnamed ? "" : " (all of them named below)");
    else
        logf("    panel stack: UNREADABLE (manager singleton or its count/array did not read) — the "
             "lines below fall back to the weaker per-name 'is topmost' test.");

    if (prev != 0xFFFFFFFFu) {
        char before[256], after[256];
        panelNames(before, sizeof(before), prev);
        panelNames(after,  sizeof(after),  mask);
        logf("    ⇒ ★★ THE OPEN PANEL SET CHANGED while the gate stayed shut: %s -> %s",
             before, after);
    }

    if (mask == 0) {
        // ⚠ This branch used to accuse panelIsOpen() of being broken. It is not: on 2026-09-20 a
        // single-player test opened the diplomacy panel and the probe named it correctly. What the
        // old text could not express is the case the enumeration now makes obvious — a panel that
        // IS open but is not one of the nine.
        if (unnamed > 0)
            logf("    ⇒ ★★★ %d panel(s) ARE open and NONE is one of the %d we name. That is the "
                 "refusal, and the gap is our list, not the engine. Add the missing name to "
                 "kGatePanels; until then this is as far as attribution goes.", unnamed, total);
        else if (openTotal == 0 && n.read && n.allPermit())
            logf("    ⇒ …and the engine agrees NOTHING is open, while all four numerics permit. "
                 "⚠⚠ THAT is a real contradiction — the gate would have a condition none of the "
                 "five things we model covers. Worth a capture.");
        else
            logf("    ⇒ …but NO panel reports itself open (%d of %d unreadable).%s", unreadable,
                 total, (n.read && n.allPermit())
                     ? "  The panel stack line above says whether the engine agrees."
                     : "  The numerics above account for the refusal.");
        return;
    }

    if (unnamed > 0)
        logf("    ⇒ ⚠ %d FURTHER panel(s) are open that match none of the %d names we carry — the "
             "list below is therefore incomplete.", unnamed, total);

    for (const GatePanel& p : kGatePanels)
        if (mask & p.bit)
            logf("    ⇒ ★★★ BLOCKING PANEL: \"%s\" is OPEN (bit 0x%03X).%s", p.name, p.bit,
                 p.bit == 0x020
                     ? "  <-- THE POST-BATTLE SCREEN, which is the panel the phantom-screen "
                       "hypothesis is about. Record WHICH machine logged this and whether that "
                       "machine pressed continue."
                 : p.bit == 0x010
                     ? "  <-- the pre-battle screen. Expected on a battle PARTICIPANT while the "
                       "battle is set up, and not by itself a fault."
                 : "");
}

static bool eventFeedGateProbeDisabled()
{
    // S18 discriminator: omit this probe's engine gate/panel calls while keeping feed hooks and
    // player fixes identical. Set in the game's launch environment before startup; read once.
    // This is an experiment control, not evidence that the probe caused the Reforms-close CTD.
    static const bool disabled = [] {
        char value[2]{};
        const bool off = GetEnvironmentVariableA("TW3K_DISABLE_GATE_PROBE", value, sizeof(value)) == 1 &&
                         value[0] == '1';
        if (off) logf("EVENTFEED GATE: sampling disabled by TW3K_DISABLE_GATE_PROBE=1 (S18 experiment)");
        return off;
    }();
    return disabled;
}

// Called every campaign tick, on the game thread. Silent unless something CHANGES — either the
// gate's answer, or, while it is refusing, the set of panels holding it shut.
static void tickEventFeedGateProbe()
{
    // Keep the C++ static initializer outside the SEH function (MSVC C2712).
    if (eventFeedGateProbeDisabled()) return;
    static uint64_t lastPoll   = 0;
    static uint64_t lastAttrib = 0;
    static int      lastState  = -1;            // -1 = never sampled, 0 = refusing, 1 = permitting
    static unsigned lastMask   = 0xFFFFFFFFu;   // sentinel: nothing attributed this episode yet
    static int      passes     = 0;

    const uint64_t now = GetTickCount64();
    if (now - lastPoll < 2000) return;   // the UI's own ticker runs at 200 ms; 2 s is plenty
    lastPoll = now;

    // Only inside a campaign: the gate's tail dereferences the campaign root unguarded.
    uintptr_t campaignRoot = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, campaignRoot) || campaignRoot <= 0x10000) return;

    // ★ The B1 discriminator USED to be sampled from here every 2 s, and finding the object to
    // sample cost a sweep of every committed private region. Both are gone: `feedTickHook` is a
    // detour on the ticker itself, so the object arrives as an argument and every transition is
    // seen rather than polled for. See the header above feedTickHook for why a sample could never
    // have worked — the healthy resting state and the stuck state are the same bytes.

    int state = -1;
    __try {
        state = ((uint8_t(*)())(g_base + RVA_EVENTFEED_GATE))() ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool said = false;
        if (!said) { said = true; logf("EVENTFEED GATE: the gate call faulted — not sampling again."); }
        lastPoll = now + (1ull << 40);   // never again this process
        return;
    }

    const bool changed = (state != lastState);
    const int  was     = lastState;
    lastState = state;

    if (state == 1) {
        if (changed) {
            logf("EVENTFEED GATE: now PERMITTING%s — the event feed may auto-open, so a pending "
                 "dilemma will be presented.", was < 0 ? " (first sample)" : " again");
            lastMask = 0xFFFFFFFFu;      // the next refusal is a fresh episode
            passes   = 0;
        }
        return;
    }

    if (changed) {
        logf("★★★ EVENTFEED GATE: REFUSING%s. While this holds, `EventFeedAutoOpen` never fires and "
             "a pending dilemma CANNOT be presented on this client — B1's mechanism, if B1 is this.",
             was < 0 ? " (first sample)" : "");
        lastMask   = 0xFFFFFFFFu;
        passes     = 0;
        lastAttrib = 0;                  // attribute immediately, then on the timer
    }

    // ★ Keep looking while it stays shut. Edge-only attribution is why `post_battle_screen` had
    // never once been reported — see the header block.
    if (passes >= ATTRIB_MAX_PASSES) return;
    if (lastAttrib && (now - lastAttrib) < ATTRIB_INTERVAL_MS) return;
    lastAttrib = now;
    ++passes;

    // ⚠⚠ ENGINE CALLS ARE NOW GATED ON THE NUMERICS, and this is a SAFETY change, not a tidy-up.
    // `collectBlockingPanels` calls the panel manager and builds nine CA strings, on the game
    // thread, and the first build of this probe did that every 5 s for the whole of every battle.
    // But the numerics are PLAIN READS and they explain most refusals by themselves: a battle drives
    // `state` to 0x01/0x08/0x09/0x0B, none of which permit. If a numeric is already refusing, the
    // gate's answer is fully accounted for and asking the engine about panels adds risk for nothing.
    GateNumerics n;
    readGateNumerics(n);

    if (n.read && !n.allPermit()) {
        static unsigned lastNumericPattern = 0xFFFFFFFFu;
        const unsigned pattern = (n.statePermits() ? 1u : 0u) | (n.q27B0 == 0 ? 2u : 0u) |
                                 (n.global == 0 ? 4u : 0u)    | (n.b2D35 == 0 ? 8u : 0u) |
                                 ((unsigned)n.state << 8);
        if (pattern != lastNumericPattern) {
            lastNumericPattern = pattern;
            logGateNumerics(n);
            logf("    (no panel lookup made: a numeric condition already accounts for the refusal, "
                 "and asking the engine costs a call on the game thread.)");
        }
        lastMask = 0xFFFFFFFFu;   // next all-permit refusal re-reports its panels from scratch
        return;
    }

    unsigned mask = 0;
    int      unreadable = 0, openTotal = 0, unnamed = 0;
    if (!collectBlockingPanels(mask, unreadable, openTotal, unnamed)) return;

    // ⚠ Re-report on a change in the UNNAMED count too, not just the mask. The case this probe
    // exists to catch — panels open that we cannot name — leaves the mask at 0 throughout, so
    // keying the report on the mask alone would print it once and then go quiet as the stack
    // changed underneath.
    const unsigned key = mask | ((unsigned)(unnamed & 0xFF) << 16)
                              | ((unsigned)((openTotal < 0 ? 0xFF : openTotal) & 0xFF) << 24);
    if (key != lastMask) {
        const unsigned prev = lastMask;
        lastMask = key;
        reportBlockingPanels(mask, unreadable, prev == 0xFFFFFFFFu ? prev : (prev & 0xFFFFu),
                             n, openTotal, unnamed);
    }

    if (passes == ATTRIB_MAX_PASSES)
        logf("    (EVENTFEED GATE: %d attribution passes on this refusal — no longer re-checking, "
             "to bound the CA-string leak. The gate is STILL refusing.)", ATTRIB_MAX_PASSES);
}

// Called from the command executor hook — game thread, campaign tick, every frame.
void drainAnswerRequest()
{
    tickEventFeedGateProbe();   // ⚠ see the block above: this is our only game-thread campaign tick

    if (g_answerRequest < 0) return;
    if (InterlockedCompareExchange(&g_inAnswerDrain, 1, 0) != 0) return;   // brake 1: re-entrancy
    const long idx = InterlockedExchange(&g_answerRequest, -1);            // brake 2: one-shot
    if (idx >= 0) drainAnswerRequestInner(idx);
    InterlockedExchange(&g_inAnswerDrain, 0);
}

// `option` is 1-based, as a person reads it off the screen. Queues only.
void answerPendingDilemma(int option)
{
    // ⚠ A crude plausibility floor ONLY. The real bound is this dilemma's own option count and is
    // checked below — 2026-08-07 the run sheet said to test with `answer 99`, which tripped this
    // line and so never exercised the real check at all. A test that passes against the wrong guard
    // is worse than no test.
    if (option < 1 || option > 64) {
        logf("ANSWER: %d is not a plausible option number for any dilemma. Use 1..N as they are "
             "numbered on screen.", option);
        return;
    }
    if (g_answerRequest >= 0) {
        logf("ANSWER: a request is already queued and has not drained yet. If it never does, the "
             "campaign tick is not running — which is itself the answer to what is wrong.");
        return;
    }
    // ⚠ Refuse BEFORE queueing if the drain cannot run. A request that sits in the queue forever
    // looks exactly like a request that was made and ignored, and that ambiguity cost session 5k.
    // ⚠ Gate on the CAMPAIGN TICK, not the command executor. The executor was the original drain and
    // it does not tick — it is called only when there are commands to execute, so an idle campaign
    // never reaches it and a queued request sat there silently (measured 2026-08-07). Gating on a
    // hook that can legitimately never run is the same defect one layer up.
    if (!campaignTickHookActive()) {
        logf("ANSWER: REFUSED — the campaign tick hook is not installed, and it is the game-thread "
             "drain. Queueing here would look like it worked and do nothing. The attach log says "
             "whether that hook installed.");
        return;
    }
    // ★ Resolve NOW and refuse NOW if there is nothing to answer.
    //
    // ❌ This used to report "nothing to answer" and then queue anyway, saying "it will submit on the
    // next campaign tick" — a sentence that was simply false (2026-08-07, caught by the refusal
    // tests). The drain did refuse a moment later, so nothing bad happened, but two things were
    // wrong: the operator was told the opposite of the truth, and a request with no target sat armed
    // waiting for ANY dilemma to arrive and be answered with an option chosen for nothing.
    DilemmaView v; char why[192] = { 0 };
    if (!resolveDilemma(v, why, sizeof(why))) {
        logf("ANSWER: REFUSED — %s. Nothing queued.", why);
        return;
    }

    // Show what is pending at the moment of asking, even if the drain later refuses.
    reportDilemmaView(v);

    // ★ THE REAL BOUND, against this dilemma's own option count, at request time — so the operator
    // is told immediately rather than discovering it in the log a tick later. The drain checks it
    // again against the count it reads itself; both are cheap and they guard different moments.
    if (v.options <= 0) {
        logf("ANSWER: REFUSED — this dilemma reports %d options, so there is no index to submit.",
             v.options);
        return;
    }
    if (option > v.options) {
        logf("ANSWER: REFUSED — option %d is out of range; this dilemma offers %d (1..%d).",
             option, v.options, v.options);
        return;
    }
    if (v.answer != -1) {
        logf("ANSWER: REFUSED — this record is already answered (+0x48=%d). Nothing queued.",
             v.answer);
        return;
    }
    if (!v.owned) {
        logf("ANSWER: REFUSED — this decision is not this machine's. Nothing queued. Send it to the "
             "machine that plays faction %u.", (unsigned)v.recFactionId);
        return;
    }

    g_answerRecord = v.rec;                                      // bind it to THIS decision
    InterlockedExchange(&g_answerRequest, (long)(option - 1));   // engine index is 0-based
    logf("ANSWER: option %d of %d queued for record %016llX; it will submit on the next campaign "
         "tick.", option, v.options, (unsigned long long)v.rec);
}

static constexpr size_t OFF_AUTOSAVE_PENDING = 0x19E;   // on the campaign root object

// 1 = an autosave has been requested and not yet taken, 0 = none, -1 = not in a campaign.
int autosavePending()
{
    uintptr_t root = 0, obj = 0; uint8_t flag = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) return -1;
    if (!readAt(root + OFF_ROOT_OBJ, obj) || !obj)          return -1;
    if (!readAt(obj + OFF_AUTOSAVE_PENDING, flag))          return -1;
    return flag ? 1 : 0;
}

void dumpTurnState()
{
    logf("---- TURN STATE (B1: does this client believe it is my turn?) ----");

    uintptr_t root = 0, obj = 0, model = 0, cont = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) {
        logf("  not in a campaign (DAT_1443CFA50 = 0) — nothing to read.");
        return;
    }
    if (!readAt(root + OFF_ROOT_OBJ, obj) || !obj)      { logf("  chain broke at +0x2188"); return; }
    if (!readAt(obj + OFF_MODEL, model) || !model)      { logf("  chain broke at +0x78");   return; }
    if (!readAt(model + OFF_CONTAINER, cont) || !cont)  { logf("  chain broke at +0x3B68"); return; }

    uint32_t facCount = 0; uintptr_t facPtr = 0;
    readAt(cont + OFF_FACTIONS_COUNT, facCount);
    readAt(cont + OFF_FACTIONS_PTR, facPtr);

    // Same refusal discipline as the participant walk: this object has not been read in this state
    // before, so an implausible count means our reading is wrong rather than the game being odd.
    if (facCount < 5 || facCount > 512 || !facPtr) {
        logf("  faction array reads count=%u ptr=%016llX — REFUSING to read further. Those numbers "
             "are not credible, so treat the offsets as wrong rather than the game as broken.",
             facCount, (unsigned long long)facPtr);
        return;
    }

    uint32_t  phase = 0xFFFFFFFF;
    uintptr_t cur = 0, wrap = 0, localFac = 0, modeObj = 0;
    readAt(cont + OFF_TURN_PHASE, phase);
    readAt(cont + OFF_CUR_FACTION, cur);
    readAt(cont + OFF_WRAP_FACTION, wrap);
    readAt(obj + OFF_LOCAL_FACTION, localFac);
    readAt(model + OFF_MODE_OBJ, modeObj);

    logf("  container=%016llX  factions=%u", (unsigned long long)cont, facCount);
    {   // The game's own turn number, so this capture can be lined up against "it broke on turn 4".
        const uint32_t t = readTurnNumber(model);
        if (t) logf("  ★ CAMPAIGN TURN = %u   (model+0x3B78 -> +0x5C, plus one, as Lua's "
                    "turn_number returns it)", t);
        else   logf("  campaign turn unreadable at model+0x3B78 -> +0x5C");
    }
    logf("  turn phase (+0x8C) = %u  (%s)", phase, turnPhaseName(phase));

    // ---- the clauses of the engine's own predicate that are plain reads ------------------------
    //
    // ⚠ The read results are KEPT (B13). A failed read leaves the value at 0, which every test below
    // then reads as "this clause passes" — and the VERDICT would go on to say so in prose. An unread
    // clause is not a passing clause, and conflating the two is the same defect in the other
    // direction. In practice these three cannot fail (`model` has already been walked twice to get
    // here), which is precisely why nobody would have noticed if they did.
    uint32_t gateA = 0; uint8_t gateE = 0, gateF = 0;
    const bool gateARead = readAt(model + OFF_GATE_A, gateA);
    const bool gateERead = readAt(model + OFF_GATE_E, gateE);
    const bool gateFRead = readAt(model + OFF_GATE_F, gateF);

    // ★ The clause that used to be skipped as "a call": an empty-list test, two loads and a compare.
    bool listRead = false, listEmpty = false;
    uintptr_t listTail = 0;
    if (modeObj && readAt(modeObj + OFF_LIST_TAIL, listTail)) {
        listRead  = true;
        listEmpty = (listTail == modeObj + OFF_LIST_HEAD);
    }

    logf("  --- the engine's own gate, FUN_1419B5CA0, clause by clause ---");
    logf("      (A) model+0x3D84            = %-10u %s", gateA,
         gateA ? "<-- NON-ZERO: EVERYTHING IS REFUSED HERE, before any other clause" : "ok (zero)");
    logf("      phase == 1 (acting)         = %-10s %s", (phase == 1) ? "yes" : "no",
         (phase == 1) ? "ok" : "<-- REFUSED");
    if (listRead && listEmpty) {
        logf("      (B) MOVEMENTS IN FLIGHT       = none       ok");
    } else if (listRead) {
        const int inFlight = walkMovementQueue(modeObj, true);
        logf("      (B) MOVEMENTS IN FLIGHT       = %-10d <-- REFUSED", inFlight);
        logf("          ⇒ The movement controller (*(model+0x3B70), the subsystem whose own string");
        logf("            table reads walk / flee / navy_embark / teleport) still holds %d movement(s).",
             inFlight);
        logf("            While that list is non-empty NO army may move and NO turn may end — and");
        logf("            recruiting, deploying, splitting, building and diplomacy are untouched,");
        logf("            because none of them consult this predicate. That is B1's report exactly.");
        logf("          ⇒ ★ If this is still non-empty a minute from now, the movement never");
        logf("            COMPLETED, and B1 is a movement whose completion never arrived.");
    } else {
        logf("      (B) list at (model+0x3B70) unreadable (ptr=%016llX)", (unsigned long long)modeObj);
    }
    // ---- (C) and (D): no longer unevaluated -----------------------------------------------------
    uintptr_t pbMgr = 0, timerObj = 0;
    int32_t   pbState = 0;
    uint64_t  timerVal = 0;
    bool pbRead = false, timerRead = false;

    if (readAt(model + OFF_PB_MGR, pbMgr) && pbMgr && readAt(pbMgr + OFF_PB_STATE, pbState))
        pbRead = true;
    if (readAt(model + OFF_GATE_D_LIST, timerObj) && timerObj && readAt(timerObj, timerVal))
        timerRead = true;

    const bool blockedC = pbRead    && (pbState != 0 && pbState != 0xE);
    const bool blockedD = timerRead && (timerVal != 0);

    if (pbRead)
        logf("      (C) pending-battle state    = %-10d %s", pbState,
             blockedC ? "<-- REFUSED: a battle is outstanding (state is neither 0 nor 14)"
                      : (pbState == 0 ? "ok (0 = idle)" : "ok (14 = explicitly permitted)"));
    else
        logf("      (C) pending-battle state    = unreadable (mgr=%016llX)",
             (unsigned long long)pbMgr);

    int blkDepth = 0;   // kept for the VERDICT below, so it need not walk the list a second time
    g_itemVectorPrinted = false;
    if (timerRead) {
        logf("      (D) blocked list at +0x3B88 = head %016llX %s", (unsigned long long)timerVal,
             blockedD ? "<-- REFUSED: the list is NOT EMPTY" : "ok (empty)");
        if (blockedD) blkDepth = walkBlockList(model, true);
    } else {
        logf("      (D) blocked list at +0x3B88 = unreadable (head cell=%016llX)",
             (unsigned long long)timerObj);
    }

    // ★ Print the vector even when nothing is blocking. A capture taken during ordinary play is how
    // the HEALTHY shape gets on record — and the healthy shape is what the stuck one has to be
    // compared against. Every reading that mattered on 2026-08-07 came from having both.
    if (!g_itemVectorPrinted) reportItemVector(model, -1);

    logf("      (E) model+0x3C41            = %-10u %s", gateE, gateE ? "<-- REFUSED" : "ok (zero)");
    logf("      (F) model+0x3C42            = %-10u %s", gateF, gateF ? "<-- REFUSED" : "ok (zero)");
    logf("      ⇒ EVERY clause is now evaluated. Nothing in this predicate is guesswork any more.");
    logf("      ★ (A)(C)(D)(E)(F) and (B) are all MODEL-level — they refuse EVERY player at once.");
    logf("        Only `current == mine` is per-faction. tester's friend reports that in the stuck");
    logf("        state NOBODY can quick-save, and quick-save is normally the turn-holder's alone,");
    logf("        so a model-level clause is exactly what that points at.");

    describeFaction("CURRENT (+0x48)", cur,      facPtr, facCount);
    describeFaction("wrap (+0x50)",    wrap,     facPtr, facCount);
    describeFaction("MINE (+0x1A8)",   localFac, facPtr, facCount);

    // Every human, so a four-player session can be lined up side by side in one glance.
    logf("  --- every faction carrying the human flag ---");
    uint32_t humans = 0;
    for (uint32_t i = 0; i < facCount; ++i) {
        uintptr_t fac = 0;
        if (!readAt(facPtr + (uintptr_t)i * 8, fac) || !fac) continue;
        uint8_t human = 0;
        if (!readAt(fac + OFF_FACTION_IS_HUMAN, human) || !human) continue;
        ++humans;

        uint64_t id = 0; uint8_t skip1 = 0, skip2 = 0;
        readAt(fac + OFF_FACTION_ID, id);
        readAt(fac + OFF_FACTION_SKIP1, skip1);
        readAt(fac + OFF_FACTION_SKIP2, skip2);
        logf("    human[%u] arrayIndex=%u id=%08X skip=%u/%u%s%s",
             humans, i, (unsigned)(id & 0xFFFFFFFF), skip1, skip2,
             (fac == localFac) ? "   <-- ME"      : "",
             (fac == cur)      ? "   <-- IT IS THIS FACTION'S TURN" : "");
    }
    logf("    %u human faction(s) in the simulation.", humans);

    // ---- the reading -----------------------------------------------------------------------
    //
    // Written out here rather than left to be worked out later, because the value of this dump is
    // the comparison between the locked machine and a healthy one, and the person holding both logs
    // should not have to reconstruct the argument.
    uint64_t curId = 0, myId = 0;
    if (cur)      readAt(cur + OFF_FACTION_ID, curId);
    if (localFac) readAt(localFac + OFF_FACTION_ID, myId);
    const bool mine   = cur && localFac && (uint32_t)curId == (uint32_t)myId;
    const bool acting = (phase == 1);

    uint8_t mySkip1 = 0, mySkip2 = 0;
    if (localFac) { readAt(localFac + OFF_FACTION_SKIP1, mySkip1);
                    readAt(localFac + OFF_FACTION_SKIP2, mySkip2); }

    // ✗✗ B13, FIXED 2026-08-06 — THIS BLOCK USED TO OMIT (C) AND (D), AND IT COST THREE SESSIONS.
    //
    // The clause-by-clause section above has printed `(D) ... <-- REFUSED: the list is NOT EMPTY`
    // correctly since (D) was identified. This verdict listed only (A), phase, (B), `mine` and
    // (E)(F) — and then concluded, in prose, *"every clause of the gate that can be read from here
    // PASSES ⇒ whatever refuses movement and END TURN is not in this predicate's readable half."*
    //
    // That sentence is an instruction to STOP LOOKING AT THE PREDICATE, and it was printed eleven
    // lines under the line that named the answer. (D) was in the readable half, was being read
    // correctly, and was the whole bug. Same species as the `FREE: F7 — nothing reads them` banner
    // that was taken as authoritative during an F7 test: our own output actively misinforming the
    // person reading it, in the one case it most needed to be right.
    //
    // ⇒ Two rules fall out of that and both are enforced below: every clause the section above
    // evaluates appears here, and the closing sentence is CONDITIONAL — the "not in the readable
    // half" line is only ever reachable when nothing refuses.
    const bool blockedA    = (gateA != 0);
    const bool blockedList = (listRead && !listEmpty);
    const bool blockedEF   = (gateE != 0 || gateF != 0);
    const bool anyExtra    = blockedA || blockedList || blockedC || blockedD || blockedEF;

    logf("  >>> VERDICT (every clause of the predicate that is a plain read):");
    logf("        (A) model+0x3D84 is zero     : %s", blockedA    ? "NO  <-- REFUSED" : "YES");
    logf("        phase == 1 (acting)          : %s", acting      ? "YES" : "NO  <-- REFUSED");
    logf("        (B) the list is empty        : %s", !listRead   ? "unreadable"
                                                    : blockedList ? "NO  <-- REFUSED" : "YES");
    logf("        current faction == my faction: %s", mine        ? "YES" : "NO  <-- REFUSED");
    logf("        (C) no battle outstanding    : %s", !pbRead     ? "unreadable"
                                                    : blockedC    ? "NO  <-- REFUSED" : "YES");
    logf("        (D) the blocked list is empty: %s", !timerRead  ? "unreadable"
                                                    : blockedD    ? "NO  <-- REFUSED" : "YES");
    logf("        (E)(F) both bytes are zero   : %s", blockedEF   ? "NO  <-- REFUSED" : "YES");

    if (anyExtra) {
        // ★★★ This is the branch B1 has never been able to reach, because the clauses were not read.
        logf("  >>> ★★★ THE ENGINE'S OWN GATE REFUSES, AND THIS SAYS WHICH CLAUSE.");

        // ★ The clause letters, in one line, BEFORE the detail. Whoever quotes a capture quotes the
        // prose conclusion rather than the table — so the conclusion has to carry the answer itself.
        // That is precisely what B13 got wrong.
        //
        // ⚠ The empty case is a TRIPWIRE, not dead code: this table and `anyExtra` are two lists of
        // the same clauses, and B13 was exactly those two lists drifting apart. If a clause is ever
        // added to one and not the other, this branch fires and says so in the log rather than
        // quietly printing a confident conclusion with a hole in it.
        char which[80]; int w = 0;
        struct { bool set; const char* tag; } clauses[] = {
            { blockedA,  "(A)" }, { blockedList, "(B)"     }, { blockedC, "(C)" },
            { blockedD,  "(D)" }, { blockedEF,   "(E)/(F)" },
        };
        for (const auto& c : clauses) {
            if (!c.set) continue;
            const int r = _snprintf_s(which + w, sizeof(which) - w, _TRUNCATE, "%s%s",
                                      w ? " + " : "", c.tag);
            if (r < 0) break;   // truncated. Cannot happen at 34 chars max; never walk off the end.
            w += r;
        }
        logf("      ⇒ REFUSING CLAUSE(S): %s", w ? which
             : "(NONE NAMED — something refuses but this table does not list it. That is a defect "
               "in this verdict block, not a reading of the game. Do not conclude anything from it.)");

        if (blockedA)
            logf("      (A) model+0x3D84 = %u. This is the FIRST thing the predicate tests and it "
                 "refuses everything — movement, END TURN, the lot.", gateA);
        if (blockedList)
            logf("      (B) A MOVEMENT IS STILL IN FLIGHT. The movement controller's queue is not "
                 "empty, and until it drains no army may move and no turn may end. ★ This is the "
                 "clause that matches B1's report without needing anything else to be wrong.");
        if (blockedC)
            logf("      (C) A BATTLE IS OUTSTANDING. The pending-battle manager's state machine "
                 "reads %d, which is neither 0 (idle) nor 14 (explicitly permitted), so nothing may "
                 "act until it resolves.", pbState);
        if (blockedD)
            logf("      (D) A GUARD NODE IS ON THE BLOCKED LIST (depth %d, head %016llX). ★ This is "
                 "the clause that was measured holding a live three-machine session on 2026-08-06. "
                 "The walk above says which node and WHAT IT IS WAITING ON — read the `waits on "
                 "item` line, not this one.",
                 blkDepth, (unsigned long long)timerVal);
        if (blockedEF)
            logf("      (E)/(F) model+0x3C41=%u model+0x3C42=%u — one of the two blocking bytes is "
                 "set.", gateE, gateF);
        logf("      ⇒ This is a MECHANISM, not a symptom: it is the same predicate the engine runs, "
             "and recruiting/deploying/splitting do not consult it — which is exactly the split tester "
             "reports from the chair.");
        logf("      ⇒ Compare the same line on a HEALTHY machine. A clause set here and clear there "
             "names the divergence outright.");
    } else if (mine && acting) {
        // ⚠ REACHABLE ONLY WHEN NOTHING REFUSES. (C) and (D) are in `anyExtra` now, so this branch
        // can no longer print over the top of a clause that was refusing three lines above it.
        //
        // ⚠⚠ And it must not print over an UNREAD one either. "Nothing refused" and "everything was
        // read and passed" are different statements, and only the second licenses the sentence
        // below. B13 was the first standing in for the second.
        const bool allRead = gateARead && gateERead && gateFRead && listRead && pbRead && timerRead;
        if (!allRead) {
            logf("  >>> Nothing REFUSED — but not every clause could be READ, so this capture does "
                 "not license a conclusion. (A)=%s (B)=%s (C)=%s (D)=%s (E)=%s (F)=%s.",
                 gateARead ? "read" : "UNREADABLE", listRead  ? "read" : "UNREADABLE",
                 pbRead    ? "read" : "UNREADABLE", timerRead ? "read" : "UNREADABLE",
                 gateERead ? "read" : "UNREADABLE", gateFRead ? "read" : "UNREADABLE");
            logf("      ⇒ An unread clause is NOT a passing clause. Take another capture rather than "
                 "reading this one as an all-clear.");
        } else {
            logf("  >>> This client believes it IS my turn, the phase permits acting, and every "
                 "clause of the gate that can be read from here PASSES — (A) (B) (C) (D) (E) (F), "
                 "all six, all actually read.");
            logf("      ⇒ Whatever refuses movement and END TURN is not in the part of this predicate "
                 "we can read. ⚠ That is now a NARROW statement, not the old sweeping one: the only "
                 "thing left unevaluated in FUN_1419B5CA0 is the faction-side tail on faction+0xE58. "
                 "It is no longer true that 'the two pending-battle calls and the one on "
                 "model+0x3B88' are outstanding — those are (C) and (D), and they are read, listed "
                 "and passing above.");
        }
    } else if (!mine) {
        logf("  >>> This client believes the turn belongs to a DIFFERENT faction.");
        logf("      ⇒ Movement and END TURN are turn-gated and would be refused exactly as reported,");
        logf("        while diplomacy and construction — which this game gates elsewhere — keep");
        logf("        working. That is the reported shape, so this is very likely the bug.");
        logf("      ⇒ Now the healthy machine's capture decides WHICH bug:");
        logf("          same current faction on both  -> the turn really is elsewhere; the advance");
        logf("                                           skipped me. Look at my skip flags above.");
        logf("          different current faction     -> the two clients DISAGREE about whose turn");
        logf("                                           it is. That is a campaign DESYNC.");
    } else {
        logf("  >>> It is my faction's turn, but the phase is %u rather than 1.", phase);
        logf("      ⇒ The turn-phase state machine is not in its acting phase on this client. Read");
        logf("        the healthy machine's phase next: a different value there is the state machine");
        logf("        having diverged; the same value means the whole session is held, not just me.");
    }

    if (mySkip1 || mySkip2)
        logf("  ⚠ MY faction carries a skip flag (+0xEE8=%u +0xF68=%u). The round-robin steps over "
             "flagged factions, so my turn would never come round at all.", mySkip1, mySkip2);

    logf("  ⚠ Compare machines on id and arrayIndex, never on pointers — the pointers are per-process "
         "allocations and will differ on healthy machines too.");
}

// ------------------------------------------- B1: the END-TURN NOTIFICATION QUEUE
//
// ★★★ WHY THIS EXISTS, and whose idea it is. tester, from the chair: *"one of the reasons the game
// does not allow you to end turns is the following: moving units, reform available, and an event is
// received"*. Those are **end-turn notifications**, and the engine has a whole subsystem for them —
// `CcoCampaignPendingActionNotificationQueue`, documented by CA's own description as *"the end turn
// notification system (showing notifications/warnings to take care of before end turn)"*, with a
// query whose description is *"returns true if there are currently no notifications so can just end
// turn"*. That is a named mechanism for "END TURN does nothing, silently", which is B1's report.
//
// ⚠ It does NOT explain the other half of the report — armies refusing to move. It is written down
// as the leading hypothesis, not as the answer, and this dump is what decides it: a stuck machine
// showing an empty queue kills the theory in one capture, which is worth as much as confirming it.
//
// ── THE LAYOUT, READ OUT OF THE BINARY (2026-08-04, static) ──────────────────────────────────────
//
// The queue object is EMBEDDED IN THE CAMPAIGN ROOT — no pointer to chase, no call to make:
//
//     Q = *(DAT_1443CFA50) + 0x21C8
//
// ★ Proven by two call sites reaching the same refresh with the same first argument, rather than by
// assuming a layout:
//
//     FUN_142E32460:  MOV RBX,[0x1443CFA50] / ADD RBX,0x21C8 / MOV RCX,RBX / CALL FUN_143040A20
//     SetActive:      MOV RCX,[ccoContext+0x50]                            / CALL FUN_143040A20
//
// ⇒ `ccoContext + 0x50` — which is what EVERY getter on this CCO type reads — is `root + 0x21C8`.
//
//     Q + 0x170  u32   HOW MANY NOTIFICATIONS ARE PENDING.
//                      `IsNotificationListEmpty` (FUN_14301D3D0) is literally
//                      `MOV RAX,[RCX+0x50] / CMP dword [RAX+0x170],0 / SETZ DL`.
//     Q + 0x174  u8    the notification system is ACTIVE / on screen.
//                      `AreNotificationsVisible` (FUN_142FF5670) is `MOVZX EDX,byte [RAX+0x174]`,
//                      and `SetActive` (FUN_143046FE0) writes 1 here before refreshing.
//     Q + 0x175  u8    set by the refresh's third argument. Second arming flag.
//
//     the per-category vectors, each a CA {cap:u32, count:u32, ptr:u64} of 0x18-byte elements:
//         +0x0F0  "character"        +0x150  "army"          +0x140  "faction"
//         +0x100  "region"           +0x110  "region"        +0x130  "region"   +0x160  "region"
//         +0x120  in the full list, in no category
//         +0x0E0  swapped by the refresh, in neither list — unidentified, printed anyway
//
// The names are not invented: `PendingActionsOfCategoryList` (FUN_14302B0E0) compares its argument
// against the literals "character" (@0x143360F50), "army" (@0x1432781F4), "region" (@0x14333E584)
// and "faction" (@0x143278D08) and hands back exactly those vectors, and `PendingActionList`
// (FUN_14302AE20) concatenates +0xF0, +0x150, +0x100, +0x110 and walks +0x120 inline.
//
// ⚠⚠ ONE CAVEAT THAT MUST TRAVEL WITH THE NUMBER, or a zero will be over-read. The refresh only
// recomputes +0x170 when +0x174 or +0x175 is set:
//
//     CMP byte [Q+0x174],0 -> if clear, CMP byte [Q+0x175],0 -> if that is clear too, SKIP
//     ... otherwise gather from campaignModel+0x3DD0 and store the count into [Q+0x170]
//
// ⇒ `pending = 0` with **both flags clear** means "not recomputed since the system was last on",
// NOT "nothing is pending". Only a zero WITH a flag set is evidence of an empty queue.
//
// Read-only, no calls into the engine, nothing rate-limited: it is a state, so there is nothing to
// hook and nothing that could desync a session.

static constexpr size_t OFF_ETN_QUEUE   = 0x21C8;  // on the campaign root: the queue itself
static constexpr size_t OFF_ETN_PENDING = 0x170;   // u32: IsNotificationListEmpty reads == 0
static constexpr size_t OFF_ETN_ACTIVE  = 0x174;   // u8:  AreNotificationsVisible
static constexpr size_t OFF_ETN_ARMED   = 0x175;   // u8:  the refresh's third argument

struct EtnVector { size_t off; const char* category; };

static const EtnVector ETN_VECTORS[] = {
    { 0x0E0, "(unidentified — swapped by the refresh, in no list)" },
    { 0x0F0, "character" },
    { 0x100, "region" },
    { 0x110, "region" },
    { 0x120, "(no category — walked inline by PendingActionList)" },
    { 0x130, "region" },
    { 0x140, "faction" },
    { 0x150, "army" },
    { 0x160, "region" },
};

void dumpEndTurnNotifications()
{
    logf("---- END-TURN NOTIFICATIONS (B1: is something BLOCKING the end turn?) ----");

    uintptr_t root = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) {
        logf("  not in a campaign (DAT_1443CFA50 = 0) — nothing to read.");
        return;
    }

    const uintptr_t q = root + OFF_ETN_QUEUE;

    uint32_t pending = 0; uint8_t active = 0, armed = 0;
    if (!readAt(q + OFF_ETN_PENDING, pending)) {
        logf("  queue at root+0x21C8 = %016llX is unreadable — treat the offset as wrong rather than "
             "the game as broken.", (unsigned long long)q);
        return;
    }
    readAt(q + OFF_ETN_ACTIVE, active);
    readAt(q + OFF_ETN_ARMED,  armed);

    // Same refusal discipline as the faction walk: this object has never been read in this state, so
    // an implausible number means our reading is wrong, and saying so is worth more than printing it.
    if (pending > 4096) {
        logf("  queue=%016llX  pending(+0x170)=%u — REFUSING to read further. That is not a credible "
             "notification count, so the offsets are wrong, not the game.",
             (unsigned long long)q, pending);
        return;
    }

    logf("  queue = %016llX  (campaign root %016llX + 0x21C8)",
         (unsigned long long)q, (unsigned long long)root);
    logf("  pending notifications (+0x170) = %u        <-- IsNotificationListEmpty is (this == 0)",
         pending);
    logf("  system ACTIVE / on screen (+0x174) = %u    <-- AreNotificationsVisible", active);
    logf("  second arming flag (+0x175) = %u", armed);

    logf("  --- the per-category vectors {cap, count, ptr} ---");
    uint32_t total = 0;
    for (size_t v = 0; v < sizeof(ETN_VECTORS) / sizeof(ETN_VECTORS[0]); ++v) {
        const uintptr_t vec = q + ETN_VECTORS[v].off;
        uint32_t cap = 0, count = 0; uintptr_t ptr = 0;
        if (!readAt(vec, cap) || !readAt(vec + 4, count) || !readAt(vec + 8, ptr)) {
            logf("      +0x%03zX %-52s <unreadable>", ETN_VECTORS[v].off, ETN_VECTORS[v].category);
            continue;
        }
        if (count > 4096 || cap > 4096) {
            logf("      +0x%03zX %-52s cap=%u count=%u — not credible, skipped",
                 ETN_VECTORS[v].off, ETN_VECTORS[v].category, cap, count);
            continue;
        }
        total += count;
        logf("      +0x%03zX %-52s cap=%-4u count=%-4u ptr=%016llX%s",
             ETN_VECTORS[v].off, ETN_VECTORS[v].category, cap, count,
             (unsigned long long)ptr, count ? "   <-- NOT EMPTY" : "");

        // The 0x18-byte element has not been decoded, so it is printed as bytes rather than as a
        // layout we would be inventing. The notification's record pointer is somewhere in here, and
        // a hex dump is how that gets identified without guessing first.
        for (uint32_t i = 0; i < count && i < 4 && ptr; ++i) {
            uint8_t raw[0x18] = { 0 };
            if (!safeRead((void*)(ptr + (uintptr_t)i * 0x18), raw, sizeof(raw))) break;
            char hex[80] = { 0 }; int n = 0;
            for (size_t b = 0; b < sizeof(raw); ++b)
                n += _snprintf_s(hex + n, sizeof(hex) - n, _TRUNCATE, "%02X ", raw[b]);
            logf("            [%u] %s", i, hex);
        }
        if (count > 4) logf("            (+%u more not shown)", count - 4);
    }

    // ---- the reading ----------------------------------------------------------------------------
    logf("  >>> READING IT:");
    if (pending > 0) {
        logf("      %u notification(s) are pending. ⇒ The blocking-notification theory is LIVE on this",
             pending);
        logf("        machine: this is the engine's own \"take care of these before ending turn\" set,");
        logf("        and one of them carrying `blocking` would refuse END TURN silently — which is");
        logf("        exactly how B1 presents.");
        logf("      ⇒ Compare with a HEALTHY machine's capture. Everyone sees their own notifications,");
        logf("        so a non-zero count here is only interesting if it is much larger than theirs,");
        logf("        or if it never falls while the player clicks END TURN across two captures.");
    } else if (active || armed) {
        logf("      ZERO pending, and the system is armed (+0x174=%u +0x175=%u) so the count IS "
             "current.", active, armed);
        logf("      ⇒ Nothing is being held back at the notification layer. ✗ That KILLS the blocking-");
        logf("        notification theory for this capture, and the refusal is somewhere else.");
    } else {
        logf("      ZERO pending, but BOTH arming flags are clear — so the refresh has not recomputed");
        logf("      the count and this zero says nothing either way. ⚠ Not evidence. The value only");
        logf("      becomes readable once the player opens or triggers the notification system.");
    }
    logf("      (vectors hold %u entr%s in total; that is the raw set the categories are drawn from.)",
         total, total == 1 ? "y" : "ies");
}

// ------------------------------------------- B1: the watcher, because the bug will not wait for F3
//
// ★★★ WHY THIS EXISTS. B1 has now survived two attempts to catch it on demand. It reproduced twice
// last night — a four-player fresh game and a three-player loaded save — and then a twelve-turn
// three-player fresh session today produced nothing. So the capture cannot depend on somebody
// pressing a key at the moment it happens: by the time the symptom is noticed, the interesting
// transition is already in the past.
//
// ⇒ This samples from the probe thread and logs only CHANGES. Two of them, and both are read-only:
//
//   1. WHICH HUMAN HAS THE TURN. Every faction's turn is not worth a line — a round visits all 272 —
//      so only human turns are logged, plus a per-round summary naming the humans that got one.
//      ★ A human missing from that summary is B1 stated outright: not "a turn locked" but "a player
//        was never given a turn", which is a different bug with a different fix.
//
//   2. THE HUMAN SET ITSELF. `CCQ_FACTION_SWITCH_HUMAN_TO_AI` executed twice in today's healthy
//      session, and a human faction turned AI would produce exactly B1's report and would persist
//      into a save — which is what tester describes when a game saved in the bugged state cannot be
//      restarted with everyone present. ⚠ A LEAD, NOT A FINDING: the set was still 3 humans
//      afterwards, so on the evidence those two were benign. This is how the next one gets caught
//      instead of inferred.

static constexpr uint32_t MAX_WATCHED_HUMANS = 8;

static uintptr_t g_watchContainer = 0;          // reset everything if the campaign is reloaded
static uint64_t  g_watchCurId     = 0xFFFFFFFFFFFFFFFFull;
static uint64_t  g_watchMyId      = 0xFFFFFFFFFFFFFFFFull;
static uint32_t  g_watchHumanIds[MAX_WATCHED_HUMANS] = { 0 };
static uint32_t  g_watchHumanCount = 0xFFFFFFFF;
static uint32_t  g_roundActed[MAX_WATCHED_HUMANS]    = { 0 };  // humans that have acted this round
static uint32_t  g_roundActedCount = 0;
static uint32_t  g_watchTurn       = 0;    // ⚠ BISECT: OUR OWN round count again, not the game's turn
static uint32_t  g_watchChanges    = 0;    // turn handovers seen since this campaign loaded

static void flushRoundSummary()
{
    if (g_roundActedCount == 0 || g_watchHumanCount == 0 || g_watchHumanCount > MAX_WATCHED_HUMANS)
        return;

    char got[256] = { 0 }; int n = 0;
    char missing[256] = { 0 }; int m = 0;
    for (uint32_t i = 0; i < g_watchHumanCount; ++i) {
        bool acted = false;
        for (uint32_t j = 0; j < g_roundActedCount; ++j)
            if (g_roundActed[j] == g_watchHumanIds[i]) { acted = true; break; }
        if (acted) n += _snprintf_s(got + n, sizeof(got) - n, _TRUNCATE, "id=%u ", g_watchHumanIds[i]);
        else       m += _snprintf_s(missing + m, sizeof(missing) - m, _TRUNCATE, "id=%u ",
                                    g_watchHumanIds[i]);
    }

    logf("TURN WATCH: round %u (OUR count, not the game turn) ended — %u of %u humans got a turn "
         "(I am id=%u).  had one: %s",
         g_watchTurn, g_roundActedCount, g_watchHumanCount, (uint32_t)g_watchMyId,
         got[0] ? got : "(none)");
    if (missing[0]) {
        logf("  ★★★ NO TURN THIS ROUND: %s", missing);
        logf("      ⇒ That is B1, and it is not \"a turn locked\" — it is a player never being GIVEN");
        logf("        one. Check that faction's skip flags (+0xEE8/+0xF68) and whether it is still");
        logf("        flagged human, and compare this line against the other machines' logs.");
    }
}

void watchCampaignTurns()
{
    uintptr_t root = 0, obj = 0, model = 0, cont = 0;
    if (!readAt(g_base + RVA_CAMPAIGN_ROOT, root) || !root) return;
    if (!readAt(root + OFF_ROOT_OBJ, obj) || !obj) return;
    if (!readAt(obj + OFF_MODEL, model) || !model) return;
    if (!readAt(model + OFF_CONTAINER, cont) || !cont) return;

    uint32_t facCount = 0; uintptr_t facPtr = 0;
    readAt(cont + OFF_FACTIONS_COUNT, facCount);
    readAt(cont + OFF_FACTIONS_PTR, facPtr);
    if (facCount < 5 || facCount > 512 || !facPtr) return;      // same refusal as the F3 dump

    // A reload replaces every object, so the previous session's ids mean nothing. Say so rather
    // than silently reporting a torrent of "changes" that are really a new campaign.
    if (cont != g_watchContainer) {
        if (g_watchContainer) logf("TURN WATCH: campaign container changed (%016llX -> %016llX) — "
                                   "reload or new campaign. Watch state reset.",
                                   (unsigned long long)g_watchContainer, (unsigned long long)cont);
        g_watchContainer  = cont;
        g_watchCurId      = 0xFFFFFFFFFFFFFFFFull;
        g_watchMyId       = 0xFFFFFFFFFFFFFFFFull;
        g_watchHumanCount = 0xFFFFFFFF;
        g_roundActedCount = 0;
        g_watchTurn       = 0;
        g_watchChanges    = 0;
    }

    // ---- 1. the human set -----------------------------------------------------------------------
    uint32_t ids[MAX_WATCHED_HUMANS] = { 0 };
    uint32_t count = 0;
    for (uint32_t i = 0; i < facCount && count < MAX_WATCHED_HUMANS; ++i) {
        uintptr_t fac = 0;
        if (!readAt(facPtr + (uintptr_t)i * 8, fac) || !fac) continue;
        uint8_t human = 0;
        if (!readAt(fac + OFF_FACTION_IS_HUMAN, human) || !human) continue;
        uint64_t id = 0;
        readAt(fac + OFF_FACTION_ID, id);
        ids[count++] = (uint32_t)id;
    }

    bool setChanged = (count != g_watchHumanCount);
    if (!setChanged)
        for (uint32_t i = 0; i < count; ++i)
            if (ids[i] != g_watchHumanIds[i]) { setChanged = true; break; }

    if (setChanged) {
        char now[160] = { 0 }; int n = 0;
        for (uint32_t i = 0; i < count; ++i)
            n += _snprintf_s(now + n, sizeof(now) - n, _TRUNCATE, "%u ", ids[i]);

        if (g_watchHumanCount == 0xFFFFFFFF) {
            logf("TURN WATCH: watching %u human faction(s): %s", count, now);
        } else {
            char was[160] = { 0 }; int w = 0;
            for (uint32_t i = 0; i < g_watchHumanCount && i < MAX_WATCHED_HUMANS; ++i)
                w += _snprintf_s(was + w, sizeof(was) - w, _TRUNCATE, "%u ", g_watchHumanIds[i]);
            logf("★★★ HUMAN SET CHANGED: was %u {%s}, now %u {%s}",
                 g_watchHumanCount, was, count, now);
            if (count < g_watchHumanCount)
                logf("     ⇒ A faction STOPPED being human. That player would stop being given turns "
                     "as one, which is B1's report — and it would persist into a save, which is why "
                     "a game saved in that state cannot be restarted with everyone.");
        }
        for (uint32_t i = 0; i < count; ++i) g_watchHumanIds[i] = ids[i];
        g_watchHumanCount = count;
    }

    // ---- 1b. WHO THIS CLIENT THINKS IT IS -------------------------------------------------------
    //
    // ★★★ THE READ THE 2026-08-03 REPRO ASKS FOR. tester's sequence was: three players, play three
    // rounds, SAVE, LOAD, player 1 ends turn, and player 2 is stuck. The bug arrived on the FIRST
    // HANDOVER AFTER A LOAD — and B4 already reports that a reload lobby loses players. So the
    // suspicion is that a load can leave a client bound to the wrong faction, or to none.
    //
    // ⇒ Every machine states WHO IT THINKS IT IS, and does it again after every load (the container
    // change above resets this, so the identity is re-stated fresh). Three logs then answer it by
    // inspection: the three ids must be the human set exactly once each. A duplicate means two
    // clients believe they are the same faction; an id outside the human set means a client is bound
    // to a faction the campaign does not think is human.
    //
    // ★ This is also what makes the locked machine's capture readable on its own: if `MINE` is not
    // in the human set, `current != mine` is not "someone else's turn", it is a broken binding.
    {
        uintptr_t localFacNow = 0; uint64_t myIdNow = 0;
        if (readAt(obj + OFF_LOCAL_FACTION, localFacNow) && localFacNow &&
            readAt(localFacNow + OFF_FACTION_ID, myIdNow) && myIdNow != g_watchMyId) {

            uint8_t myHuman = 0;
            readAt(localFacNow + OFF_FACTION_IS_HUMAN, myHuman);

            bool inSet = false;
            for (uint32_t i = 0; i < count; ++i)
                if (ids[i] == (uint32_t)myIdNow) { inSet = true; break; }

            if (g_watchMyId == 0xFFFFFFFFFFFFFFFFull)
                logf("TURN WATCH: this client's OWN faction is id=%u (isHuman=%u)%s",
                     (uint32_t)myIdNow, myHuman, inSet ? "" : "   <-- NOT IN THE HUMAN SET");
            else
                logf("★★★ LOCAL FACTION CHANGED: was id=%u, now id=%u (isHuman=%u)%s",
                     (uint32_t)g_watchMyId, (uint32_t)myIdNow, myHuman,
                     inSet ? "" : "   <-- NOT IN THE HUMAN SET");

            if (!inSet)
                logf("     ⇒ This client is bound to a faction the campaign does not hold as human. "
                     "It would never be given a turn, and every turn-gated action would refuse — "
                     "which is B1's report. ★ Compare this id against the other machines' logs.");

            g_watchMyId = myIdNow;
        }
    }

    // ---- 1c. ★★★★★ THE MOVEMENT QUEUE, WHICH IS THE ONE THING B1 PREDICTS WILL BE STUCK ---------
    //
    // Clause (B) of the engine's own gate refuses every movement and every end turn while the
    // movement controller's list is non-empty (§6mmm). Movements finish in seconds, so a list that
    // is still occupied ten seconds later is already wrong — and if B1 is "a movement whose
    // completion never arrived", this fires **the moment the bug starts**, minutes before a player
    // notices they cannot act and long before the 120 s turn-stall capture.
    //
    // ⚠ Deliberately quiet in the normal case: movement is constant, so only a queue that STAYS
    // occupied is logged, once per episode, with a full capture when it passes 30 s.
    {
        static uint64_t occupiedSince = 0;
        static bool     announced     = false;
        static bool     captured      = false;

        constexpr uint64_t kWarnMs    = 10000;
        constexpr uint64_t kCaptureMs = 30000;

        uintptr_t modeObj = 0, tail = 0;
        const uint64_t now = GetTickCount64();
        bool empty = true;
        if (readAt(model + OFF_MODE_OBJ, modeObj) && modeObj &&
            readAt(modeObj + OFF_LIST_TAIL, tail))
            empty = (tail == modeObj + OFF_LIST_HEAD);

        if (empty) {
            if (announced)
                logf("MOVE WATCH: the movement queue drained after %llu s — not stuck after all.",
                     (unsigned long long)((now - occupiedSince) / 1000));
            occupiedSince = 0; announced = false; captured = false;
        } else {
            if (occupiedSince == 0) occupiedSince = now;
            const uint64_t held = now - occupiedSince;

            // ★★★★★ THE RACE ITSELF, IF IT IS ONE. An autosave has been requested while a movement
            // is still in flight — the exact overlap tester's chain needs. Logged the instant the two
            // are true together, because the window is short and nothing else would record it.
            // ⚠ Seeing this is NOT the bug: the save may well be taken after the movement finishes.
            // It is the *opportunity* for the bug, and if B1 never happens without this line
            // appearing first, that is the correlation the whole chain rests on.
            static bool overlapLogged = false;
            if (autosavePending() == 1) {
                if (!overlapLogged) {
                    overlapLogged = true;
                    logf("★★★ SAVE/MOVE OVERLAP: an autosave is PENDING while %d movement(s) are "
                         "still in flight (%llu s so far). ⇒ If the save is taken now it serialises "
                         "a movement whose completion has not arrived — B1's chain, at the moment it "
                         "would happen.",
                         walkMovementQueue(modeObj, false), (unsigned long long)(held / 1000));
                }
            } else {
                overlapLogged = false;
            }

            if (!announced && held >= kWarnMs) {
                announced = true;
                logf("★★★ MOVE WATCH: a movement has been IN FLIGHT for %llu s (queue holds %d). "
                     "While this list is non-empty this client cannot move an army or end its turn "
                     "— see B1 clause (B).",
                     (unsigned long long)(held / 1000), walkMovementQueue(modeObj, false));
            }
            if (!captured && held >= kCaptureMs) {
                captured = true;
                logf("################################################################");
                logf("★★★ MOVEMENT STUCK FOR %llu SECONDS — AUTO-CAPTURE (B1 clause B)",
                     (unsigned long long)(held / 1000));
                logf("    Movements finish in seconds. This one has not, and until it does every");
                logf("    army order and every END TURN on this client is refused by the engine's");
                logf("    own gate — while recruiting, building and diplomacy keep working.");
                logf("################################################################");
                dumpTurnState();
                reportCommandTraffic();
                dumpEndTurnNotifications();
                logf("★★★ END MOVEMENT CAPTURE ###############################");
            }
        }
    }

    // ---- 1d. ★★★★★ THE BLOCKED LIST, WATCHED — B1 IS CLAUSE (D) AND THIS CATCHES EVERY PUSH ------
    //
    // Read live out of a stuck session on 2026-08-05, on all three machines at once (§6qqq): every
    // clause passed except (D), which was non-zero on all of them — including the machine whose turn
    // it was. Model-level, so it refuses every player together, which is what "nobody can quick-save,
    // not even the turn-holder" already said.
    //
    // ✗✗ **AND IT IS NOT A TIMER.** See the constants above: `*(model+0x3B88)` is the head cell of an
    // intrusive LIFO of guard nodes, and each node's vptr is its TYPE — the operation that is
    // holding the campaign. So this watcher no longer prints "microseconds" (it never was); it walks
    // the chain and prints the vptr, the countdown and the depth.
    //
    // ★★ AND IT LOGS EVERY EPISODE, NOT ONLY THE STUCK ONE. The naming session recorded ten healthy
    // pushes on one machine and seven on the other, holding 0-27 s each. Every one of those is a
    // free sample of a node type doing its job — so one ordinary session maps most of the ~19 types,
    // and when the leak finally happens its vptr is already a name rather than a number.
    {
        static bool      wasBlocked  = false;
        static uintptr_t lastHead    = 0;
        static uint32_t  lastCount   = 0;
        uint64_t&        blockedSince = g_gateBlockedSince;

        uintptr_t cell = 0; uint64_t head = 0;
        int32_t   pbState = 0; uintptr_t pbMgr = 0;
        bool haveList = false;

        if (readAt(model + OFF_GATE_D_LIST, cell) && cell && readAt(cell, head))
            haveList = true;
        if (readAt(model + OFF_PB_MGR, pbMgr) && pbMgr)
            readAt(pbMgr + OFF_PB_STATE, pbState);

        const bool blockedNow = haveList && (head != 0);
        const uint64_t now = GetTickCount64();

        if (blockedNow && !wasBlocked) {
            blockedSince = now;
            logf("★★★★★ GATE CLAUSE (D) JUST BECAME SET — a BLOCKED node was pushed, caught at the "
                 "moment it happened.");
            logf("      head cell %016llX now holds node %016llX. Every action gated by "
                 "FUN_1419B5CA0 is refused until this list empties — for EVERY player, not just "
                 "this one.", (unsigned long long)cell, (unsigned long long)head);
            walkBlockList(model, true);

            // ★ D3 — NAME THE DILEMMA, EVERY TIME, NOT JUST ON A LONG HOLD.
            //
            // ⚠ This is the third session in a row where the counting job on #3 could not start.
            // The 2026-08-09 evening playtest logged 81 `(D)` episodes across three machines and
            // **zero** option keys, because the only thing that printed them was the auto-capture,
            // and that is threshold-driven: a healthy 30 s dilemma never trips it. So "no B1 in this
            // session" has never had a denominator, and neither has "it is always the same dilemma".
            //
            // ⚠ `(D)` carries ~19 node types and only ONE is a dilemma guard, so most pushes have no
            // key to print. `resolveDilemma` refuses anything whose vptr is not the FC70 guard, which
            // is exactly the filter wanted here — a silent return means "that push was not a
            // dilemma", which is the common and correct case.
            {
                DilemmaView dv; char why[192] = { 0 };
                if (resolveDilemma(dv, why, sizeof(why))) {
                    logf("      ★ DILEMMA PUSHED — key logged HERE, on the push, so #3's count can "
                         "finally be made: faction %u, %d option(s), +0x48=%d",
                         (unsigned)dv.recFactionId, dv.options, dv.answer);
                    reportOptionKeys(dv);

                    // #57: timestamp the dilemma push for the read-only feed watch.
                    noteFeedMarker("the dilemma push");

                } else {
                    logf("      (not a dilemma guard: %s — most `(D)` pushes are not, and this line "
                         "is what stops a silence being read as one.)", why);
                }
            }

            logf("      ⇒ THE NODE'S vptr IS THE ANSWER. Each of the ~19 push sites installs its own "
                 "derived vtable, so that pointer names the operation that just blocked the "
                 "campaign. Most pushes here clear in seconds and are perfectly healthy.");
            logf("      ⇒ AND WHAT HAPPENED IMMEDIATELY ABOVE THIS LINE IS THE TRIGGER. In the "
                 "naming session the one that never cleared was pushed at the TURN HANDOVER, 0.9 s "
                 "after CCQ_END_TURN and in the same millisecond as 'now has the turn'.");
            logf("      context: campaign turn=%u  pending-battle state=%d  movements=%d  "
                 "autosavePending=%d",
                 readTurnNumber(model), pbState, movementsInFlight(), autosavePending());
            // ★ tester's friend, 2026-08-05: *"he did not see a saving icon appear before it was his
            // turn"*. If this line reports `autosavePending=1`, a save was asked for and the gate
            // shut in the same breath — which is a save that STARTED AND NEVER FINISHED, and that
            // would explain the missing icon, the frozen session, and the unusable save file with
            // one mechanism. If it reports 0, his observation means the save never began, and the
            // node belongs to something else entirely. Either answer is worth the line.
        } else if (!blockedNow && wasBlocked) {
            logf("★★★ GATE CLAUSE (D) CLEARED after %llu s — the node popped by itself, which is what "
                 "normally happens. ⚠ Healthy holds of 17 s and 27 s have been measured, so a long "
                 "one is not by itself a fault.",
                 (unsigned long long)((now - blockedSince) / 1000));
        } else if (blockedNow && (uintptr_t)head != lastHead) {
            logf("      (D) still set, and the HEAD CHANGED: %016llX -> %016llX. Something popped and "
                 "something else pushed, or a second guard went on top of the first — and only the "
                 "head is ever ticked, so the one underneath cannot self-pop.",
                 (unsigned long long)lastHead, (unsigned long long)head);
            walkBlockList(model, true);
        } else if (blockedNow) {
            // The countdown, sampled. Stuck at zero means the node's poll never completes; stuck
            // NON-zero means the node is not being ticked at all — different bugs, and they are
            // indistinguishable from one reading. Logged only every ~15 s so it cannot flood.
            static uint64_t lastCountLog = 0;
            if (now - lastCountLog > 15000) {
                lastCountLog = now;
                uint32_t cd = 0;
                readAt((uintptr_t)head + OFF_BLK_COUNTDOWN, cd);
                logf("      (D) still set after %llu s: head %016llX countdown %u -> %u. %s",
                     (unsigned long long)((now - blockedSince) / 1000),
                     (unsigned long long)head, lastCount, cd,
                     (cd == lastCount)
                         ? (cd == 0 ? "STUCK AT ZERO — the node is being polled and its poll never "
                                      "says 'done'."
                                    : "STUCK NON-ZERO — the node is not being TICKED at all, which "
                                      "is the buried-under-a-newer-push shape.")
                         : "still counting down, so this is a live operation, not a leak.");
                lastCount = cd;
            }
        }

        wasBlocked = blockedNow;
        lastHead   = (uintptr_t)head;

        // ★★★ THE AUTOSAVE FLAG, WATCHED IN ITS OWN RIGHT — the direct test of the missing icon.
        //
        // `rootObj + 0x19E` is set when the turn arrives at this client's faction (§6ppp) and is
        // cleared when the save is taken. So a 0->1 that NEVER returns to 0 is a save that was asked
        // for and never happened — which is exactly what "no saving icon appeared" looks like from
        // the chair, and exactly what would leave an unusable save file behind.
        //
        // ⚠ The consumer of that byte was never found in the readable image, so "it is cleared when
        // the save completes" is inference from its name and its setter, not something measured.
        // Watching it is how that gets settled.
        {
            static int      lastPending = -1;
            static uint64_t pendingSince = 0;
            static bool     complainedAt10 = false;

            const int pend = autosavePending();
            const uint64_t nowMs = GetTickCount64();

            if (pend != lastPending) {
                if (pend == 1) {
                    pendingSince = nowMs;
                    complainedAt10 = false;
                    logf("SAVE WATCH: an autosave has been REQUESTED (rootObj+0x19E 0 -> 1). "
                         "movements=%d  gate(D) set=%s",
                         movementsInFlight(), blockedNow ? "YES" : "no");
                } else if (pend == 0 && lastPending == 1) {
                    logf("SAVE WATCH: the autosave flag cleared after %llu ms — the save was taken.",
                         (unsigned long long)(nowMs - pendingSince));
                }
                lastPending = pend;
            } else if (pend == 1 && !complainedAt10 && (nowMs - pendingSince) >= 10000) {
                complainedAt10 = true;
                logf("★★★ SAVE WATCH: the autosave has been PENDING FOR %llu SECONDS and has not been "
                     "taken. gate(D) set=%s. ⇒ A save that was asked for and never happened is what "
                     "\"no saving icon appeared\" looks like from the chair — and it would leave the "
                     "save file unusable, which is B1's oldest symptom.",
                     (unsigned long long)((nowMs - pendingSince) / 1000),
                     blockedNow ? "YES — and the gate is shut, so the two go together" : "no");
            }
        }
    }

    // ---- 2. whose turn it is --------------------------------------------------------------------
    uintptr_t cur = 0;
    if (!readAt(cont + OFF_CUR_FACTION, cur) || !cur) return;

    uint64_t curId = 0;
    if (!readAt(cur + OFF_FACTION_ID, curId)) return;

    // ---- ★★★★★ B1: CATCH IT WITHOUT ANYONE PRESSING ANYTHING -----------------------------------
    //
    // The bug has cost hours of rig time and reproduces perhaps once a session, so the capture must
    // not depend on somebody recognising the moment and reaching for `F3`. The 16:48 capture only
    // exists because tester happened to press it, and even then the `since` column was empty because
    // the second press never came.
    //
    // ⇒ Every machine now watches for THE TURN NOT MOVING, and dumps by itself when it stalls. The
    // condition is deliberately whose-turn-agnostic: on the stuck player it fires because they cannot
    // release the turn, and on everyone else it fires because they are waiting on that player — so
    // one stall produces a capture from EVERY machine, at the same moment, without coordination.
    //
    // ★★ It fires REPEATEDLY at intervals, which is the part that makes it worth more than one `F3`:
    // the second and later captures carry a `since` column covering the stall itself. If the stuck
    // player is clicking END TURN and no `CCQ_END_TURN` appears in `since`, that is proof the UI is
    // refusing to submit — which is the open question, and the one tester's blocking-notification
    // theory predicts the answer to.
    //
    // ⚠ 120 s is chosen to sit above a slow human turn. A player thinking costs a spurious capture in
    // the log, which is cheap; the alternative — missing the real one — has already cost a session.
    {
        static uint64_t stallSince   = 0;      // when the current faction last changed
        static uint64_t lastDumpAt   = 0;
        static uint64_t stallCurId   = 0xFFFFFFFFFFFFFFFFull;
        static int      dumpsThisEpisode = 0;

        constexpr uint64_t kStallMs    = 120000;   // 2 minutes without the turn moving
        constexpr uint64_t kFirstMs    = 300000;   // ...but 5 before the first handover — see below
        constexpr uint64_t kRepeatMs   = 60000;    // then again every minute
        constexpr int      kMaxPerStall = 4;       // enough for a `since` series, not a flood

        // ⚠ MEASURED FALSE POSITIVE (2026-08-04, 17:36): a clean three-player session produced three
        // auto-captures on every machine, and all six were the FIRST TURN. The campaign had just
        // loaded, the host was setting up, and the first handover genuinely took four minutes — the
        // stall clock starts at campaign load, so an unhurried turn 1 trips it every time.
        //
        // ⇒ Before the first handover is seen, the threshold is five minutes rather than two. It is
        // deliberately NOT suppressed: "the turn never moves after a load" is a real B1 shape (a
        // player who is never GIVEN a turn), so the capture must still happen — just not while
        // somebody is arranging their opening turn.
        const uint64_t threshold = (g_watchChanges == 0) ? kFirstMs : kStallMs;

        const uint64_t now = GetTickCount64();
        if (curId != stallCurId) {                 // the turn moved — reset the episode
            stallCurId       = curId;
            stallSince       = now;
            lastDumpAt       = 0;
            dumpsThisEpisode = 0;
        } else if (stallSince && (now - stallSince) >= threshold &&
                   dumpsThisEpisode < kMaxPerStall &&
                   (lastDumpAt == 0 || (now - lastDumpAt) >= kRepeatMs)) {
            lastDumpAt = now;
            ++dumpsThisEpisode;

            uint64_t myId = 0; uintptr_t localFac = 0;
            if (readAt(obj + OFF_LOCAL_FACTION, localFac) && localFac)
                readAt(localFac + OFF_FACTION_ID, myId);

            logf("################################################################");
            logf("★★★ TURN HAS NOT MOVED FOR %llu SECONDS — AUTO-CAPTURE #%d (B1)",
                 (unsigned long long)((now - stallSince) / 1000), dumpsThisEpisode);
            logf("    faction id=%u has held it that whole time. This client is id=%u, so it is %s.",
                 (uint32_t)curId, (uint32_t)myId,
                 ((uint32_t)curId == (uint32_t)myId) ? "**THE STUCK PLAYER**"
                                                     : "waiting on someone else");
            if (g_watchChanges == 0)
                logf("    ⚠ NO HANDOVER HAS BEEN SEEN YET on this client since the campaign loaded, so "
                     "this may simply be a long first turn. It is captured anyway because \"the turn "
                     "never arrives after a load\" is itself a B1 shape.");
            else
                logf("    %u handover(s) have been seen since the campaign loaded, so the rotation was "
                     "running and has stopped.", g_watchChanges);
            logf("    Nobody pressed anything — read the `since` column below against the previous");
            logf("    auto-capture: it covers the stall, so a CCQ_END_TURN of 0 there while the");
            logf("    player was clicking END TURN means the UI never submitted it.");
            logf("################################################################");

            dumpTurnState();
            reportCommandTraffic();
            dumpEndTurnNotifications();
            dumpPendingBattleState();

            logf("★★★ END AUTO-CAPTURE #%d ###############################", dumpsThisEpisode);
            if (dumpsThisEpisode == kMaxPerStall)
                logf("    (that is %d for this stall — no more until the turn moves again)",
                     kMaxPerStall);
        }
    }

    if (curId == g_watchCurId) return;
    g_watchCurId = curId;
    ++g_watchChanges;

    uint8_t human = 0;
    readAt(cur + OFF_FACTION_IS_HUMAN, human);
    if (!human) return;              // AI turns are the overwhelming majority and not worth a line

    // ✂ REVERTED 2026-08-04 — BISECT. This read the GAME's turn number (model+0x3B78 -> +0x5C) to
    // set the round boundary, replacing the heuristic below. It is the **second** of only two changes
    // since the last known-good build that run automatically, so it comes out alongside the other
    // while a hang is being chased — even though it is two guarded reads and the weaker suspect.
    //
    // ⚠ The offsets are not in doubt (§6kkk read them off the Lua `turn_number` binding, and F3
    // still prints the real turn from the same helper). What is untested is calling it from the
    // probe thread every time a human turn changes, INCLUDING while a campaign is still loading and
    // that chain is half-built.
    //
    // Restoring the old heuristic verbatim: a human who has already acted this round acting again
    // means the round wrapped. ⚠ It is arbitrary in phase and resets on load — which is exactly what
    // tester spotted — so this is a temporary step backwards, not a decision. Tracked on the backlog.
    for (uint32_t j = 0; j < g_roundActedCount; ++j) {
        if (g_roundActed[j] == (uint32_t)curId) {
            flushRoundSummary();
            ++g_watchTurn;
            g_roundActedCount = 0;
            break;
        }
    }
    if (g_roundActedCount < MAX_WATCHED_HUMANS) g_roundActed[g_roundActedCount++] = (uint32_t)curId;

    uintptr_t localFac = 0; uint64_t myId = 0;
    if (readAt(obj + OFF_LOCAL_FACTION, localFac) && localFac)
        readAt(localFac + OFF_FACTION_ID, myId);

    uint32_t phase = 0; uint8_t skip1 = 0, skip2 = 0;
    readAt(cont + OFF_TURN_PHASE, phase);
    readAt(cur + OFF_FACTION_SKIP1, skip1);
    readAt(cur + OFF_FACTION_SKIP2, skip2);

    // ★ `moving=` is on this line because the handover is the moment tester's observation points at:
    // the previous player ends their turn, the game AUTOSAVES, and only then does the turn arrive
    // here. A handover that normally reads `moving=0` and reads non-zero on the round that breaks is
    // the whole finding, and it costs one integer per human turn.
    logf("TURN WATCH: round %u (OUR count) — human id=%u now has the turn (phase=%u, skip=%u/%u, "
         "moving=%d, autosave=%d)%s",
         g_watchTurn, (uint32_t)curId, phase, skip1, skip2, movementsInFlight(), autosavePending(),
         ((uint32_t)curId == (uint32_t)myId) ? "   <-- ME" : "");
}

// ------------------------------------------- one-key full capture

void dumpEverything()
{
    logf("################ FULL CAPTURE ################");
    logf("hooks: lobby guard=%s  MP session=%s  join=%s  seat order=%s  share=%s",
         g_lobbyDetour.active ? "on" : "off",
         g_mpDetour.active    ? "on" : "off",
         g_joinDetour.active  ? "on" : "off",
         g_seatDetour.active  ? "on" : "off",
         g_shareDetour.active ? "on" : "off");

    // ★ #13/B10. No switch by design, so the ONLY way to know a client applied it is to say so —
    // and it is the one patch whose absence makes this client disagree with its peers about who
    // owns which army. A capture that does not show ARMED is a capture from a broken client.
    logf("#13 bystander side gate: %s%s", bystanderSideGateArmed() ? "ARMED" : "NOT APPLIED",
         bystanderSideGateArmed() ? ""
             : "   ⚠⚠ this client puts spectators on the ATTACKER's side above two humans, and "
               "disagrees with any client that did apply it");

    reportLobbyGuard();
    reportSeatHook();

    // The probes fire from the panel-population hook (game thread). Report their latest values here
    // too, so a capture is self-contained even if nothing has changed since the last sample.
    // Deliberately NOT re-invoking the ready-check virtual from this thread — F3 runs on the probe
    // thread, and calling into game objects from here could race the game thread.
    logf("panel expansion: extra panels=%ld pushFails=%ld slots handled=%ld",
         g_panelsAdded, g_panelPushFails, g_extraSeats);
    logf("panel populate hook: %s | calls=%ld", g_panelDetour.active ? "ACTIVE" : "not installed",
         g_panelCalls);
    if (g_lastReadyMask == 0xFFFFFFFF)
        logf("ready check: no sample yet (needs the panel hook to fire — join/leave a lobby)");
    else
        logf("ready check: last observed mask=%08X%s", g_lastReadyMask,
             g_lastReadyMask <= 1 ? "  (nothing blocking)" : "  (START CAMPAIGN would be refused)");

    const uintptr_t live = g_liveLobby;
    if (!live) {
        logf("--- LIVE lobby not captured yet (open an MP campaign lobby) ---");
    } else if (!lobbyLooksLive(live)) {
        // Expected once a campaign has started: the lobby is destroyed and its memory reused.
        logf("--- LIVE lobby pointer %016llX is STALE (listener vtable at +0x80 no longer matches) ---",
             (unsigned long long)live);
        logf("    not dumping it: the lobby was freed when the campaign started and this allocation "
             "now belongs to something else. This is normal in-campaign / in-battle.");
    } else {
        logf("--- LIVE lobby instance (captured by the guard hook) ---");
        dumpInstanceDeep(live);
    }

    dumpMpSession();
    dumpBattleSides();      // B10 — silent outside a battle, so it costs nothing in a campaign
    dumpPendingBattleChoice();  // #13 — the key list a bystander's vote falls back to. Says so and
                                // stops when no battle is pending, which is most of the time
    reportCcoResolver();    // #73 — call volume through the CCO resolver: is a slow panel ours?
    reportUpdateNullGuard();// #75 — did a registry miss get swallowed, and where
    reportTreeWalkGuard();  // #12 - has the widget-tree corruption happened this run?
    reportTelestration();   // #47/#50 — which ping/arrow slots this client holds. Silent outside a battle
    dumpEventMgrRegistry(); // #63 — the list that actually gates the event feed. Read this one.
    reportEventCursors();   // #63 — and whether this machine can survive a 3rd human at all
    reportHumanFactionCountHold();
    reportHumanFactionLoad(); // #68 — what the save actually contained, and what survived the load
    dumpHumanFactionVector();
    reportBattleRoles();
    // B1, the locked turn: the two things that would explain armies and END TURN being refused while
    // diplomacy and construction still work. Both are read-only and cost nothing when nothing is
    // wrong, so they belong in the capture rather than behind a key nobody remembers.
    //
    // Turn state FIRST, because it is the candidate that matches the reported shape without needing
    // a battle to have happened at all — and because the logs have already put the pre-battle path
    // on the back foot: every PRE-BATTLE ROLE sequence in all three run-31 logs ends with `every
    // participant has chosen`, and every one of them was `role=2 (AutoResolve)`.
    dumpTurnState();
    reportCommandTraffic();
    dumpEndTurnNotifications();
    dumpPendingBattleState();
    logf("################ END FULL CAPTURE ################");
}


#endif // developer facilities
