// S17: keep fresh-lobby records and duplicate validation on the accepted service state.
#include "tw3k.h"

static Detour g_freshFactionDetour;
// Record recovery and the duplicate ready-mask filter have separate dependencies.
// S16 already owns the validator entry in a combined build; its collision must
// not disable accepted-state record convergence for every extra-seat pick.
static bool g_freshRecordsArmed = false;
static uint32_t (*g_origFactionReadyMask)(uintptr_t) = nullptr;
static constexpr uintptr_t RVA_FACTION_READY_MASK = 0x004702E0;
// 1.7.2: eight pushes followed by SUB RSP,E8; whole, non-relative instructions.
static constexpr uint8_t EXPECT_FACTION_READY_MASK[20] = {
    0x40,0x53,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,
    0x56,0x41,0x57,0x48,0x81,0xEC,0xE8,0x00,0x00,0x00
};

struct FreshFactionSeat {
    uint32_t id;
    char faction[128];
    char party[128];
    uint32_t group;
    uint8_t ready;
};
struct FreshFactionState {
    uintptr_t service;
    uint32_t count;
    FreshFactionSeat seats[4];
};
static constexpr uintptr_t RVA_DEFAULT_GROUP_GETTER = 0x0046F450;

// ★ Enumerate ACTUAL ids through the service's team enumerator. Record positions
// and the session player->slot table are not player ids; sparse order is normal.
// This reads the accepted state, never a UI label or an optimistic requested key.
static bool readFreshFactionState(uintptr_t lobby, FreshFactionState& state)
{
    if (!g_freshRecordsArmed || g_inLobbyCacheRefresh || !lobbyLooksLive(lobby)) return false;
    uintptr_t setup = 0, session = 0, slots = 0, table = 0, serviceFn = 0;
    uint8_t loaded = 0, leaving = 0;
    uint32_t capacity = 0;
    if (!readAt(lobby + 0x90, leaving) || leaving == 1 ||
        !readAt(lobby + 0xC0, setup) || !setup ||
        !readAt(setup + 0x60, loaded) || loaded ||
        !readAt(lobby + 0xB0, session) || !session ||
        !readAt(session + OFF_MP_SLOTOBJ, slots) || !slots ||
        !readAt(slots + OFF_SLOT_COUNT, capacity) || capacity < 3 || capacity > 4 ||
        !readAt(session + 0x18, table) || !table ||
        !readAt(table + 0x208, serviceFn) || serviceFn < 0x10000) return false;
    __try {
        state.service = ((uintptr_t(*)(uintptr_t))serviceFn)(session + 0x18);
        uintptr_t serviceSlots = 0, vt = 0, factionFn = 0, partyFn = 0, groupFn = 0, readyFn = 0;
        if (!state.service || !readAt(state.service + 0x40, serviceSlots) || serviceSlots != slots ||
            !readAt(state.service, vt) || !vt ||
            !readAt(vt + 0xC0, factionFn) || factionFn < 0x10000 ||
            !readAt(vt + 0xD0, partyFn) || partyFn < 0x10000 ||
            !readAt(vt + 0x178, groupFn) || groupFn < 0x10000 ||
            !readAt(vt + 0x50, readyFn) || readyFn < 0x10000) return false;
        state.count = 0;
        for (uint32_t team = 0; team <= SLOT_TEAM_MASK; ++team) {
            uint32_t ids[4] = {};
            const uint32_t n = ((uint32_t(*)(uintptr_t,uint32_t,uint32_t,uint32_t*))
                (g_base + RVA_SAVELOBBY_ENUM))(slots, team, 4, ids);
            if (n > 4 || n > 4 - state.count) return false;
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t flags = 0;
                if (ids[i] >= capacity ||
                    !readAt(slots + ids[i] * SLOT_ENTRY_STRIDE + SLOT_ENTRY_FLAGS, flags) ||
                    !(flags & 0x40) || (flags & (SLOT_FLAG_VACANT | SLOT_FLAG_SPEC))) return false;
                for (uint32_t j = 0; j < state.count; ++j)
                    if (state.seats[j].id == ids[i]) return false;
                FreshFactionSeat& seat = state.seats[state.count++];
                seat.id = ids[i];
                void* faction = ((void*(*)(uintptr_t,uint32_t))factionFn)(state.service, seat.id);
                void* party = ((void*(*)(uintptr_t,uint32_t))partyFn)(state.service, seat.id);
                if (!readCaString(faction, seat.faction, sizeof(seat.faction)) ||
                    !readCaString(party, seat.party, sizeof(seat.party))) return false;
                ((void(*)(uintptr_t,uint32_t,uint32_t*))groupFn)(state.service, seat.id, &seat.group);
                // ★ This base campaign service's +178 is a verified stub: it
                // always writes 0, even after CD stores the accepted group in
                // slot+68 (1404996FA). Extra-panel timers must retain that
                // accepted group instead of resetting its picker to group 0.
                // Keep actual overridden getters and stock panels unchanged.
                if (seat.id >= 2 && groupFn == g_base + RVA_DEFAULT_GROUP_GETTER &&
                    !readAt(slots + seat.id * SLOT_ENTRY_STRIDE + 0x68, seat.group)) return false;
                seat.ready = ((uint8_t(*)(uintptr_t,uint32_t))readyFn)(state.service, seat.id);
            }
        }
        return state.count > 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool freshFactionRecordsMatch(uintptr_t lobby, const FreshFactionState& state)
{
    uintptr_t records = 0;
    uint32_t count = 0;
    if (!readAt(lobby + OFF_PLAYER_COUNT, count) || count != state.count ||
        !readAt(lobby + OFF_PLAYER_RECORDS, records) || !records) return false;
    for (uint32_t i = 0; i < state.count; ++i) {
        const FreshFactionSeat& seat = state.seats[i];
        uintptr_t found = 0;
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t id = ~0u;
            if (!readAt(records + j * 0x48 + 0x10, id)) return false;
            if (id == seat.id) { if (found) return false; found = records + j * 0x48; }
        }
        char faction[128] = {}, party[128] = {};
        uint32_t group = 0;
        uint8_t ready = 0;
        if (!found || !readCaString((void*)(found + 0x18), faction, sizeof(faction)) ||
            !readCaString((void*)(found + 0x28), party, sizeof(party)) ||
            !readAt(found + 0x3C, group) || !readAt(found + 0x38, ready) ||
            strcmp(faction, seat.faction) || strcmp(party, seat.party) ||
            group != seat.group || ready != seat.ready) return false;
    }
    return true;
}

// GAME THREAD ONLY. Restore the safe record-only half of 142CE66D0 -> 142D7CD10.
// Calling the panel/cache half for id >=2 would overwrite lobby+110 mid-call.
void tickFreshLobbyFactions(uintptr_t lobby, bool notification)
{
    // Notifications reconcile immediately; idle convergence uses the same 2 Hz
    // clock as the existing extra-panel retry, rather than rebuilding per frame
    // while a native leave cleanup still retains a departing player's record.
    static uintptr_t lastLobby = 0;
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (!notification && lobby == lastLobby && now < next) return;
    lastLobby = lobby; next = now + 500;
    FreshFactionState state = {};
    if (!readFreshFactionState(lobby, state) || freshFactionRecordsMatch(lobby, state)) return;
    if (!refreshFreshLobbyRecords(lobby)) return;
    // The native record builder calls the same default getter and writes 0.
    // Restore only extra ids' accepted group after it has supplied names/keys.
    // Match id fields again: neither enumeration nor record order is positional.
    uintptr_t records = 0;
    uint32_t count = 0;
    if (!readAt(lobby + OFF_PLAYER_COUNT, count) || count != state.count ||
        !readAt(lobby + OFF_PLAYER_RECORDS, records) || !records) return;
    __try {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t id = ~0u;
            const uintptr_t rec = records + i * 0x48;
            if (!readAt(rec + 0x10, id)) return;
            for (uint32_t j = 0; j < state.count; ++j)
                if (id >= 2 && id == state.seats[j].id)
                    *(volatile uint32_t*)(rec + 0x3C) = state.seats[j].group;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("S21 fresh groups: record update faulted; next notification/tick will retry");
        return;
    }
    diagLogf("S17 fresh factions: native records refreshed for %u seated ids; accepted faction/party retained", state.count);
}

static bool freshFactionDuplicate(const FreshFactionState& state)
{
    for (uint32_t i = 0; i < state.count; ++i) {
        const char* key = state.seats[i].faction;
        if (!key[0] || !strcmp(key, "NULL")) continue;
        for (uint32_t j = i + 1; j < state.count; ++j)
            if (!strcmp(key, state.seats[j].faction)) return true;
    }
    return false;
}

static uint32_t freshFactionReadyMask(uintptr_t service)
{
    const uint32_t stock = g_origFactionReadyMask(service);
    FreshFactionState state = {};
    // The native bit-28 producer compares battle unit character records. Fresh
    // campaign seats additionally need the same refusal for duplicate factions.
    // Retain every native reason; only this exact live fresh service is extended.
    if (!readFreshFactionState(g_liveLobby, state) || state.service != service) return stock;
    return freshFactionDuplicate(state) ? stock | (1u << 28) : stock;
}

bool installFreshFactionHook()
{
    if (!saveLobbyFixArmed()) {
        logf("S17 fresh factions: native record enumerator unavailable; refusing fix");
        return false;
    }
    g_freshRecordsArmed = true;
    const bool installed = detourInstall(g_freshFactionDetour, g_base + RVA_FACTION_READY_MASK,
        sizeof(EXPECT_FACTION_READY_MASK), EXPECT_FACTION_READY_MASK,
        (uintptr_t)&freshFactionReadyMask, (void**)&g_origFactionReadyMask, "S17 fresh factions");
    if (!installed)
        logf("S21 fresh records: accepted-state reconciliation armed; duplicate ready-mask filter unavailable (native 0xCD duplicate refusal retained)");
    return installed;
}

void removeFreshFactionHook()
{
    g_freshRecordsArmed = false;
    detourRemove(g_freshFactionDetour, "S17 fresh factions");
}
