// saveload.cpp - B4: keep the engine's record builder, include every occupied lobby team.
#include "tw3k.h"

Detour g_saveLobbyDetour;
thread_local bool g_inLobbyCacheRefresh = false;
static volatile long g_saveLobbyArmed = 1;
static volatile long g_saveLobbyMerges = 0;
typedef uint32_t (*EnumTeamFn)(uintptr_t, uint32_t, uint32_t, uint32_t*);
static EnumTeamFn g_origEnumTeam = nullptr;
static thread_local bool g_buildSaveLobbyRecords = false;

// S16: the vanilla validator (FUN_1404702E0) considers occupied slots, not the
// saved PLAYERS roster. FUN_1404747E0 stores that roster at session+D38F0;
// FUN_1403F8590 copies every header player into a 0x30-byte entry, faction at +8.
// ★ Feed bit 24 back to the ENGINE: ReadyButtonState becomes inactive and
// ReadyStatusText renders mp_ready_error_team_has_no_players (FUN_142D49A80).
// No cached count, no lobby-record count, and no guessed player/slot mapping.
static constexpr uintptr_t RVA_SAVE_READY_MASK = 0x004702E0;
static constexpr uint8_t EXPECT_SAVE_READY_MASK[20] = {
    0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
    0x48,0x81,0xEC,0xE8,0x00,0x00,0x00
}; // whole instructions, no RIP-relative operand
static constexpr uint32_t SAVE_PLAYER_MISSING = 1u << 24;
static Detour g_saveReadyDetour;
static uint32_t (*g_origSaveReadyMask)(uintptr_t) = nullptr;

static bool savedPlayersMissing(uintptr_t lobby, uintptr_t validator)
{
    if (!lobbyLooksLive(lobby)) return false;
    uintptr_t setup = 0, session = 0, validatorSession = 0, slots = 0;
    uint8_t loaded = 0, leaving = 0;
    if (!readAt(lobby + 0x90, leaving) || leaving == 1 ||
        !readAt(lobby + 0xC0, setup) || !setup ||
        !readAt(setup + 0x60, loaded) || !loaded ||
        !readAt(lobby + 0xB0, session) || !session ||
        !readAt(validator + 0x20, validatorSession) || validatorSession != session) return false;

    // From here we positively identified this save lobby. Missing evidence must
    // not enable START; the next query retries, including after a leave/rejoin.
    uint32_t saved = 0, cap = 0, slotCount = 0;
    uintptr_t roster = 0, vt = 0, factionFn = 0, sessionSlots = 0;
    if (!readAt(session + 0xD38F0, cap) || !readAt(session + 0xD38F4, saved) ||
        saved == 0 || saved > 4 || cap < saved ||
        !readAt(session + 0xD38F8, roster) || !roster ||
        !readAt(validator + 0x40, slots) || !slots ||
        !readAt(session + OFF_MP_SLOTOBJ, sessionSlots) || sessionSlots != slots ||
        !readAt(slots + OFF_SLOT_COUNT, slotCount) || slotCount > SLOT_MAX_ENTRIES ||
        !readAt(validator, vt) || !vt || !readAt(vt + 0xC0, factionFn) ||
        factionFn != g_base + 0x0046E280) return true;

    char seated[4][128] = {};
    uint32_t seatedCount = 0;
    for (uint32_t team = 0; team <= SLOT_TEAM_MASK; ++team) {
        uint32_t ids[SLOT_MAX_ENTRIES] = {};
        const uint32_t n = g_origEnumTeam(slots, team, SLOT_MAX_ENTRIES, ids);
        if (n > 4 || n > 4 - seatedCount) return true;
        for (uint32_t i = 0; i < n; ++i) {
            if (ids[i] >= slotCount) return true;
            // vt+C0 is the engine's slot-id -> DB faction KEY accessor, not
            // the numeric campaign faction id at slot+60.
            const uintptr_t key = ((uintptr_t(*)(uintptr_t, uint32_t))factionFn)(validator, ids[i]);
            if (!readCaString((void*)key, seated[seatedCount], sizeof(seated[0])) ||
                !seated[seatedCount][0]) return true;
            ++seatedCount;
        }
    }
    bool used[4] = {};
    for (uint32_t i = 0; i < saved; ++i) {
        char key[128] = {};
        if (!readCaString((void*)(roster + i * 0x30 + 8), key, sizeof(key)) || !key[0]) return true;
        bool found = false;
        for (uint32_t j = 0; j < seatedCount; ++j) {
            if (!used[j] && strcmp(key, seated[j]) == 0) {
                used[j] = true; found = true; break;
            }
        }
        if (!found) return true;
    }
    return false;
}

static uint32_t saveReadyMaskHook(uintptr_t validator)
{
    const uint32_t stock = g_origSaveReadyMask(validator);
    bool missing = false;
    __try { missing = savedPlayersMissing(g_liveLobby, validator); }
    __except (EXCEPTION_EXECUTE_HANDLER) { missing = true; }
    static uintptr_t lastLobby = 0;
    static bool lastMissing = false;
    if (lastLobby != g_liveLobby || lastMissing != missing) {
        lastLobby = g_liveLobby; lastMissing = missing;
        if (missing) logf("S16 save START refused: a saved human faction is unseated or roster unreadable; ready reason=team_has_no_players");
        else logf("S16 save readiness: roster adds no blocking reason");
    }
    // Always active in release too; disabling B4's record recovery is not
    // permission to start an incomplete save.
    return stock | (missing ? SAVE_PLAYER_MISSING : 0);
}

bool saveLobbyFixArmed() { return g_saveLobbyArmed != 0 && g_saveLobbyDetour.active; }
void setSaveLobbyFix(bool on)
{
    InterlockedExchange(&g_saveLobbyArmed, on ? 1 : 0);
    diagLogf("B4 save-lobby: %s (hook=%s; existing records are retained)", on ? "ARMED" : "off",
         g_saveLobbyDetour.active ? "active" : "absent");
}

// ★ Only the record writer's exact call site gets the extension. Every other user of
// this service, including campaign/save code, receives the original team result.
// Keep the stock 0/1 loop; fold actual teams 2/3 into its second result. This avoids
// toggling executable bytes while another thread is running that loop.
static uint32_t saveLobbyEnumResult(uintptr_t caller, uintptr_t slots, uint32_t team,
                                   uint32_t capacity, uint32_t* ids)
{
    const uint32_t stock = g_origEnumTeam(slots, team, capacity, ids);
    if (caller != g_base + RVA_LOBBY_ENUM_RETURN || team != 1 || !saveLobbyFixArmed() ||
        !g_buildSaveLobbyRecords || g_inLobbyCacheRefresh || !ids) return stock;
    uint32_t count = 0;
    if (!readAt(slots + OFF_SLOT_COUNT, count) || count == 0 || count > 4 ||
        capacity < count || capacity > 16 || stock > capacity) return stock;

    uint32_t lastTeam = 1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t flags = 0;
        if (!readAt(slots + i * SLOT_ENTRY_STRIDE + SLOT_ENTRY_FLAGS, flags)) return stock;
        if ((flags & 0x40) && !(flags & (SLOT_FLAG_VACANT | SLOT_FLAG_SPEC)) &&
            (flags & SLOT_TEAM_MASK) > lastTeam) lastTeam = flags & SLOT_TEAM_MASK;
    }
    uint32_t total = stock;
    for (uint32_t t = 2; t <= lastTeam; ++t) {
        uint32_t extra[4] = {};
        const uint32_t n = g_origEnumTeam(slots, t, 4, extra);
        if (n > 4 || n > capacity - total) return stock; // fail closed, writer ignores the tail
        memcpy(ids + total, extra, n * sizeof(uint32_t));
        total += n;
    }
#ifndef TW3K_RELEASE
    if (total != stock && InterlockedIncrement(&g_saveLobbyMerges) <= 24)
        logf("B4 save-lobby: record writer teams=0..%u; team1 result %u -> %u (slots=%u)",
             lastTeam, stock, total, count);
#endif
    return total;
}

static uint32_t saveLobbyEnumHook(uintptr_t slots, uint32_t team, uint32_t capacity, uint32_t* ids)
{
    return saveLobbyEnumResult((uintptr_t)_ReturnAddress(), slots, team, capacity, ids);
}

bool installSaveLobbyHook()
{
    uint8_t writer[sizeof(EXPECT_LOBBY_RECORD_WRITER)] = {};
    if (!safeRead((void*)(g_base + RVA_LOBBY_RECORD_WRITER), writer, sizeof(writer)) ||
        memcmp(writer, EXPECT_LOBBY_RECORD_WRITER, sizeof(writer)) != 0) {
        logf("B4 save-lobby: record-writer ENTRY SIGNATURE MISMATCH; refusing hook");
        return false;
    }
    uint8_t call[sizeof(EXPECT_LOBBY_ENUM_CALL)] = {};
    if (!safeRead((void*)(g_base + RVA_LOBBY_ENUM_RETURN - sizeof(call)), call, sizeof(call)) ||
        memcmp(call, EXPECT_LOBBY_ENUM_CALL, sizeof(call)) != 0) {
        logf("B4 save-lobby: record-writer CALL SIGNATURE MISMATCH; refusing hook");
        return false;
    }
    if (!detourInstall(g_saveLobbyDetour, g_base + RVA_SAVELOBBY_ENUM,
        sizeof(EXPECT_SAVELOBBY_ENUM), EXPECT_SAVELOBBY_ENUM, (uintptr_t)&saveLobbyEnumHook,
        (void**)&g_origEnumTeam, "B4 save-lobby teams")) return false;
    if (detourInstall(g_saveReadyDetour, g_base + RVA_SAVE_READY_MASK,
        sizeof(EXPECT_SAVE_READY_MASK), EXPECT_SAVE_READY_MASK, (uintptr_t)&saveReadyMaskHook,
        (void**)&g_origSaveReadyMask, "S16 save START readiness")) return true;
    detourRemove(g_saveLobbyDetour, "B4 save-lobby teams (readiness install failed)");
    return false;
}

void removeSaveLobbyHook()
{
    setSaveLobbyFix(false);
    detourRemove(g_saveReadyDetour, "S16 save START readiness");
    detourRemove(g_saveLobbyDetour, "B4 save-lobby teams");
}

// ★ GAME THREAD ONLY, before the original tick can tear down the frontend.
// Saved factions produce no 0xCD event. Poll the actual seated service ids, then use
// the engine's writer and our existing guarded panel initialiser/display path.
// S17 (approved by the coordinator 2026-10-07 after S16 merged): refresh fresh-lobby records through the
// same scoped enum detour the save-lobby fix uses; saved-lobby START checks are untouched.
bool refreshFreshLobbyRecords(uintptr_t lobby)
{
    static bool freshWriterFailed = false;
    if (freshWriterFailed || !saveLobbyFixArmed() || g_inLobbyCacheRefresh || !lobbyLooksLive(lobby)) return false;
    uintptr_t setup = 0, session = 0, slots = 0;
    uint8_t loaded = 0, leaving = 0;
    uint32_t count = 0;
    if (!readAt(lobby + 0x90, leaving) || leaving == 1 ||
        !readAt(lobby + 0xC0, setup) || !setup ||
        !readAt(setup + 0x60, loaded) || loaded ||
        !readAt(lobby + 0xB0, session) || !session ||
        !readAt(session + OFF_MP_SLOTOBJ, slots) || !slots ||
        !readAt(slots + OFF_SLOT_COUNT, count) || count < 3 || count > 4) return false;
    const bool nestedBuild = g_buildSaveLobbyRecords;
    bool refreshed = false;
    g_buildSaveLobbyRecords = true;
    __try {
        ((void(*)(uintptr_t))(g_base + RVA_LOBBY_RECORD_WRITER))(lobby);
        refreshed = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("S17 fresh factions: native record writer faulted; refusing further refreshes");
        freshWriterFailed = true; // Leave the existing saved-lobby fix unchanged.
    }
    g_buildSaveLobbyRecords = nestedBuild;
    return refreshed;
}

void tickSaveLobby(uintptr_t lobby)
{
    if (!saveLobbyFixArmed() || !lobbyLooksLive(lobby)) return;
    uintptr_t setup = 0, session = 0, slots = 0, group = 0;
    uint8_t loaded = 0, leaving = 0;
    uint32_t count = 0;
    if (!readAt(lobby + 0x90, leaving) || leaving == 1 ||
        !readAt(lobby + 0xC0, setup) || !setup || !readAt(setup + 0x60, loaded) || !loaded ||
        !readAt(lobby + 0xB8, group) || !group ||
        !readAt(lobby + 0xB0, session) || !session ||
        !readAt(session + OFF_MP_SLOTOBJ, slots) || !slots ||
        !readAt(slots + OFF_SLOT_COUNT, count) || count == 0 || count > 4) return;

    static uintptr_t lastLobby = 0;
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (lobby == lastLobby && now < next) return;
    lastLobby = lobby; next = now + 500;

    uint32_t records = 0;
    uintptr_t base = 0;
    if (!readAt(lobby + OFF_PLAYER_COUNT, records) || records > 4 ||
        !readAt(lobby + OFF_PLAYER_RECORDS, base) || (records && !base)) return;
    bool missing = false;
    for (uint32_t team = 2; team <= SLOT_TEAM_MASK; ++team) {
        uint32_t ids[4] = {};
        const uint32_t n = g_origEnumTeam(slots, team, 4, ids);
        if (n > 4) return;
        for (uint32_t i = 0; i < n; ++i) {
            bool found = false;
            for (uint32_t j = 0; j < records; ++j) {
                uint32_t id = ~0u;
                if (readAt(base + j * 0x48 + 0x10, id) && id == ids[i]) found = true;
            }
            if (!found) missing = true;
        }
    }
    if (missing) {
        const bool nestedBuild = g_buildSaveLobbyRecords;
        g_buildSaveLobbyRecords = true;
        ((void(*)(uintptr_t))(g_base + RVA_LOBBY_RECORD_WRITER))(lobby);
        g_buildSaveLobbyRecords = nestedBuild;
        uint32_t after = 0;
        readAt(lobby + OFF_PLAYER_COUNT, after);
        logf("B4 save-lobby: records %u -> %u from seated service ids; saved factions retained", records, after);

#ifdef TW3K_RELEASE
        captureLobbyPlayerNames(lobby);
#else
        reportPlayerVectorIfChanged(lobby);
#endif
    }
    refreshSavedExtraPanels(lobby, slots);
}
