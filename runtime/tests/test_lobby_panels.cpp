// Run the ACTUAL extra-panel path against engine ABI mocks. The mock ApplyFaction
// deliberately refuses to show a widget with +0xA2=1, reproducing a late reset.
#include "../src/panels.cpp"
#include "../src/detour.cpp"
#include <cassert>
uintptr_t g_base = 0;
bool repairRecords = true;
bool saveLobbyFixArmed() { return repairRecords; }
void tickFreshLobbyFactions(uintptr_t, bool) {}
void logf(const char*, ...) {}
void reportPlayerVectorIfChanged(uintptr_t) {}
void reportReadyMaskIfChanged() {}
void captureLobbyPlayerNames(uintptr_t, bool) {}
bool safeRead(const void* p, void* out, size_t n) {
    __try { memcpy(out,p,n); return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static uint8_t callbacks[4][0xA8] = {}, widgets[4][0x300] = {}, groupData[0x40] = {};
static int inits[4] = {}, displays[4] = {}, statuses[4] = {}, lastReady[4] = {};
static int enables[4] = {};
static const char* displayedFaction[4] = {};
static const char* displayedParty[4] = {};
static uint32_t displayedGroup[4] = {};
static unsigned feedbackCommands = 0;
static bool faultDisplay = false;
template<class T> void put(uint8_t* p, size_t o, T v) { memcpy(p+o,&v,sizeof(v)); }
static unsigned seat(void* cb) { return (unsigned)(((uint8_t*)cb-callbacks[0])/sizeof(callbacks[0])); }
static void* getCb(void* w) { return callbacks[((uint8_t*)w-widgets[0])/sizeof(widgets[0])]; }
static uint32_t strLen(void* p) { return (uint32_t)*(uint64_t*)p; }
static char* strData(void* p) { return *(char**)((uint8_t*)p+8); }
static void assign(void* a, void* b) { memcpy(a,b,16); }
static void init(void* cb, void* grp, uint32_t id, void*) {
    assert(id==seat(cb)); ++inits[id];
    put((uint8_t*)cb,CB_PLAYER_IDX,id); put((uint8_t*)cb,CB_GROUP_DATA,(uintptr_t)grp);
    if (*((uint8_t*)cb+CB_SHOWN_FLAG)) widgets[id][0x2C4]=widgets[id][0x2C5]=1;
}
static void apply(void* cb, void* faction, void* party, uint32_t group, char) {
    const auto id=seat(cb); ++displays[id];
    // Engine dropdown text setters invoke the picker, which sends a NETWORK
    // command unless the native global guard is 1. A handler re-entry lock alone
    // cannot suppress that send. Count it as a regression, even on timer repair.
    if (*(uint8_t*)(g_base+RVA_FACTION_UI_GUARD)!=1) ++feedbackCommands;
    if (faultDisplay) RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    displayedFaction[id]=strData(faction);
    displayedParty[id]=strData(party);displayedGroup[id]=group;
    if (!*((uint8_t*)cb+CB_SHOWN_FLAG)) widgets[id][0x2C4]=widgets[id][0x2C5]=1;
    *((uint8_t*)cb+CB_SHOWN_FLAG)=1;
}
static void dropdown(void* cb, char force) { if (!force) ++enables[seat(cb)]; }
static void mustNotRunStock(void*) { assert(false); }
static void ready(void*, uint32_t id, uint8_t value) { assert(id>=2 && id<4); ++statuses[id]; lastReady[id]=value; }
static void jump(uintptr_t rva, uintptr_t fn) {
    auto p=(uint8_t*)(g_base+rva); const uint8_t op[]={0xFF,0x25,0,0,0,0};
    memcpy(p,op,6);memcpy(p+6,&fn,8);
}
static void string(uint8_t* p,size_t o,const char* s) {
    put(p,o,(uint64_t)strlen(s));put(p,o+8,(uintptr_t)s);
}
static unsigned originalCalls=0;
static void original(void*,uint32_t,void*,void*,void*,void*,int,int) {++originalCalls;}
int main() {
    auto image=(uint8_t*)VirtualAlloc(nullptr,0x4400000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
    assert(image);g_base=(uintptr_t)image;
    jump(RVA_GET_PANEL_CB,(uintptr_t)getCb);jump(RVA_STR_ASSIGN,(uintptr_t)assign);
    jump(RVA_STR_LEN,(uintptr_t)strLen);jump(RVA_STR_DATA,(uintptr_t)strData);
    jump(RVA_PANEL_INIT,(uintptr_t)init);jump(RVA_APPLY_FACTION,(uintptr_t)apply);
    jump(RVA_SET_DROPDOWNS,(uintptr_t)dropdown);jump(RVA_PANEL_READY,(uintptr_t)ready);
    assert(FlushInstructionCache(GetCurrentProcess(),image,0x2E00000));
    g_slotCacheReady=1;g_panelsAdded=2;g_panelReadyVerified=true;
    uint8_t lobby[0x190]={}, records[4*0x48]={}, setup[0xC0]={}, slots[0x1400]={};
    static uint8_t session[0xD3900]={};
    uintptr_t panelPtrs[4]={};for(unsigned i=0;i<4;++i) panelPtrs[i]=(uintptr_t)widgets[i];
    put(lobby,0x80,g_base+0x37DC0A8);put(lobby,0xB8,(uintptr_t)groupData);
    put(lobby,0xC0,(uintptr_t)setup);put(lobby,0xD0,(uintptr_t)records);
    put(lobby,0xC8,4u);put(lobby,0xCC,2u);put(lobby,0xDC,4u);put(lobby,0xE0,(uintptr_t)panelPtrs);
    put(lobby,0xB0,(uintptr_t)session);put(session,OFF_MP_SLOTOBJ,(uintptr_t)slots);
    put(slots,OFF_SLOT_COUNT,4u);
    for(unsigned i=0;i<2;++i) {
        const unsigned id=3-i; auto rec=records+i*0x48;
        put(rec,0x10,id);string(rec,0x18,id==2?"gongsun_zan":"kong_rong");string(rec,0x28,"party");
        rec[0x38]=id==3?1:0;
        put(slots,id*SLOT_ENTRY_STRIDE+SLOT_ENTRY_FLAGS,0x340u|id);
        put(slots,id*SLOT_ENTRY_STRIDE+0x60,1u);
        put(callbacks[id],CB_PLAYER_IDX,id);put(callbacks[id],CB_GROUP_DATA,(uintptr_t)groupData);
        callbacks[id][CB_SHOWN_FLAG]=1; // stale marker, real widget hidden
        recordPanelCompletion(g_panelCompletion[id],(uintptr_t)lobby,panelPtrs[id],(uintptr_t)callbacks[id],
            (uintptr_t)groupData,id,id,1,id==2?"gongsun_zan":"kong_rong","party",0,true);
    }
    apply(callbacks[2],records+0x48+0x18,records+0x48+0x28,0,0);
    assert(feedbackCommands==1); // negative control: unguarded native display emits a picker send
    feedbackCommands=0;displays[2]=0;widgets[2][0x2C4]=widgets[2][0x2C5]=0;
    // Negative control: original completion gate misses the hidden widget and AI status.
    InterlockedExchange(&g_extraPanelRepair,0);refreshSavedExtraPanels((uintptr_t)lobby,(uintptr_t)slots);
    assert(!inits[2]&&!statuses[2]&&!widgets[2][0x2C4]);
    InterlockedExchange(&g_extraPanelRepair,1);repairRecords=false;
    tickExtraLobbyPanels((uintptr_t)lobby); // FRESH retry, no saved enumeration required
    for(unsigned id=2;id<4;++id) {
        assert(inits[id]==1 && displays[id]==1 && statuses[id]==1);
        assert(widgets[id][0x2C4]&&widgets[id][0x2C5]);
        assert(lastReady[id]==(id==3?1:0));
    }
    refreshSavedExtraPanels((uintptr_t)lobby,(uintptr_t)slots);
    assert(displays[2]==1&&statuses[2]==1); // converges, no repeated notifications
    puts("ok actual fresh extra-panel path repairs stale shown/hidden callbacks and initial AI status; sparse record order and real ready bytes");
    setup[0x60]=1;repairRecords=true;
    widgets[3][0x2C4]=widgets[3][0x2C5]=0;
    refreshSavedExtraPanels((uintptr_t)lobby,(uintptr_t)slots);
    assert(inits[3]==2&&statuses[3]==2&&widgets[3][0x2C4]);
    // No occupied human -> no extra panel display; never calls slots 0/1.
    widgets[2][0x2C4]=0;put(slots,2*SLOT_ENTRY_STRIDE+SLOT_ENTRY_FLAGS,0x3C2u);
    refreshSavedExtraPanels((uintptr_t)lobby,(uintptr_t)slots);assert(inits[2]==1);
    put(slots,2*SLOT_ENTRY_STRIDE+SLOT_ENTRY_FLAGS,0x742u);
    refreshSavedExtraPanels((uintptr_t)lobby,(uintptr_t)slots);assert(inits[2]==1);
    assert(!inits[0]&&!inits[1]&&!statuses[0]&&!statuses[1]);
    assert(records[0x38]==1&&records[0x48+0x38]==0);
    puts("ok save-lobby late hide retries, vacant/spectator exclusion, stock panels and network ready state unchanged");
    // The engine refuses a duplicate and acknowledges the previous accepted
    // faction. Reconcile text and re-enable even though our completion key matches.
    put(slots,2*SLOT_ENTRY_STRIDE+SLOT_ENTRY_FLAGS,0x342u);
    const int before=displays[2], enabled=enables[2];
    displayedFaction[2]="cao_cao"; // dropdown optimistic text from the attempted pick
    assert(handleExtraSlotChange((uintptr_t)lobby+0x80,2,records+0x48+0x18,
        records+0x48+0x28,0,&mustNotRunStock,nullptr));
    assert(displays[2]==before+1 && enables[2]==enabled+1);
    assert(!strcmp(displayedFaction[2],"gongsun_zan"));
    refreshSavedExtraPanels((uintptr_t)lobby,(uintptr_t)slots);
    assert(displays[2]==before+1); // timer still converges
    puts("ok refused duplicate acknowledgement restores accepted faction and controls; timer converges and unsafe stock handler never runs");
    // Consecutive accepted picks on BOTH extra ids, including a group/party
    // change with the same faction key. Records are deliberately in {3,2} order.
    for (unsigned id=2;id<4;++id) {
        auto rec=records+(3-id)*0x48;
        for (unsigned pick=0;pick<2;++pick) {
            string(rec,0x18,pick?"zhang_yan":"yuan_shao");
            string(rec,0x28,pick?"bandit_party":"coalition_party");put(rec,0x3C,pick+1);
            slotChangedHook((void*)((uintptr_t)lobby+0x80),id,rec+0x18,rec+0x18,rec+0x28,rec+0x28,pick+1,0);
            assert(!strcmp(displayedFaction[id],pick?"zhang_yan":"yuan_shao"));
            assert(displayedGroup[id]==pick+1 && !feedbackCommands);
            assert(*(uint8_t*)(g_base+RVA_FACTION_UI_GUARD)==0);
        }
        const int displayBefore=displays[id];
        string(rec,0x28,"different_party");put(rec,0x3C,3u);
        handleExtraSlotChange((uintptr_t)lobby+0x80,id,rec+0x18,rec+0x28,3,nullptr,nullptr);
        assert(displays[id]==displayBefore+1 && displayedGroup[id]==3);
        assert(!strcmp(displayedParty[id],"different_party"));
        refreshSavedExtraPanels((uintptr_t)lobby,(uintptr_t)slots);
        assert(displays[id]==displayBefore+1 && !feedbackCommands);
    }
    // Preserve an enclosing native guard, including early convergence and faults.
    *(uint8_t*)(g_base+RVA_FACTION_UI_GUARD)=1;
    refreshSavedExtraPanels((uintptr_t)lobby,(uintptr_t)slots);
    assert(*(uint8_t*)(g_base+RVA_FACTION_UI_GUARD)==1);
    faultDisplay=true;
    handleExtraSlotChange((uintptr_t)lobby+0x80,2,records+0x48+0x18,records+0x48+0x28,3,&mustNotRunStock,nullptr);
    assert(*(uint8_t*)(g_base+RVA_FACTION_UI_GUARD)==1 && g_inExtraHandler==0);
    faultDisplay=false;*(uint8_t*)(g_base+RVA_FACTION_UI_GUARD)=0;
    assert(!feedbackCommands);
    puts("ok S21 both extra ids change faction twice and group/party alone; no UI feedback sends; nested guard and SEH restoration");
    // ★ Reproduce count=3 < panelCount=4 with ids {0,2,1}. A stale callback
    // for id 3 must not converge, assign its cache, display or force its index.
    put(lobby,0xCC,3u);
    put(records,0x10,0u);put(records+0x48,0x10,2u);put(records+0x90,0x10,1u);
    uint8_t cacheBefore[16]={};memcpy(cacheBefore,g_slotCache[3],16);
    const int oldInit=inits[3],oldDisplay=displays[3],oldStatus=statuses[3];
    g_panelCompletion[3].complete=true;
    handleExtraSlotChange((uintptr_t)lobby+0x80,3,records+0x48+0x18,records+0x48+0x28,0,nullptr,nullptr);
    assert(!g_panelCompletion[3].complete && inits[3]==oldInit && displays[3]==oldDisplay && statuses[3]==oldStatus);
    assert(memcmp(cacheBefore,g_slotCache[3],16)==0);
    assert(g_inExtraHandler==0);
    // Remaining id 2 lives at vector position 1 and still retries correctly.
    const int initBefore=inits[2];
    widgets[2][0x2C4]=widgets[2][0x2C5]=0;
    handleExtraSlotChange((uintptr_t)lobby+0x80,2,records+0x48+0x18,records+0x48+0x28,0,nullptr,nullptr);
    assert(inits[2]==initBefore+1);
    puts("ok shrinking sparse roster refuses stale id 3 before cache/display and retains id 2 by record identity");
    g_origSlotChanged=original;g_slotCacheReady=0;
    for(unsigned id=2;id<5;++id) slotChangedHook((void*)((uintptr_t)lobby+0x80),id,nullptr,nullptr,nullptr,nullptr,0,0);
    assert(!originalCalls);
    slotChangedHook((void*)((uintptr_t)lobby+0x80),0,nullptr,nullptr,nullptr,nullptr,0,0);
    slotChangedHook((void*)((uintptr_t)lobby+0x80),1,nullptr,nullptr,nullptr,nullptr,0,0);
    assert(originalCalls==2);
    puts("ok extra ids never fall back to two-entry stock cache when expansion is unavailable; stock ids preserved");
    VirtualFree(image,0,MEM_RELEASE);
}
