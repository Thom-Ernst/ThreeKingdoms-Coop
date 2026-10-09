"""Production-helper regressions for panel completion, lobby lifetime and resident teardown."""
from pathlib import Path
import re, subprocess, sys
root=Path(__file__).resolve().parents[2]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
def source(name): return (root/'runtime/src'/name).read_text(encoding='utf-8-sig')
def function(s,name):
 m=re.search(r'(?m)^(?:static )?[^\n;{}]+\b'+name+r'\([^\n]*\)\n\{',s);assert m,name
 end=re.search(r'(?m)^}',s[m.start():]);assert end
 return s[m.start():m.start()+end.end()]+'\n'
def region(s,a,b): return s[s.index(a):s.index(b,s.index(a))]
panels=source('panels.cpp');lobby=source('lobby.cpp');session=source('session.cpp');gift=source('gift.cpp')
code=r'''
#include "tw3k.h"
#include <cassert>
uintptr_t g_base=0;
void logf(const char*,...) {}
bool safeRead(const void* a,void* b,size_t n) { memcpy(b,a,n);return true; }
static int vpCalls=0,vpFail=0,flushCalls=0,flushFail=0;
static BOOL testProtect(void* p,SIZE_T n,DWORD want,DWORD* old) {
    if (++vpCalls==vpFail) { SetLastError(ERROR_ACCESS_DENIED);return FALSE; }
    return VirtualProtect(p,n,want,old);
}
static BOOL testFlush(HANDLE p,const void* a,SIZE_T n) {
    if (++flushCalls==flushFail) return FALSE;
    return FlushInstructionCache(p,a,n);
}
#define VirtualProtect testProtect
#define FlushInstructionCache testFlush
#include "detour.cpp"
static volatile long g_panelsAdded=2;
'''
code+=region(panels,'struct PanelCompletion {','static volatile long g_inExtraHandler')
code+=function(panels,'resetPanelCompletionState')
code+=r'''
static volatile long g_expandRuns=21,g_expandFaulted=1;
static volatile long g_autoFactionTries=4,g_factionNext=3,g_factionRequest=2,g_localSeat=2;
static volatile long long g_autoFactionNextTry=100000;
static unsigned giftResets=0;
void resetGiftPanelState(const char*) { ++giftResets; }
'''
code+=function(session,'resetSlotExpansionBudget')+function(lobby,'resetAutoFactionBudget')
code+=r'''
static volatile long g_autoExpandTarget=4;
static uintptr_t g_capturedMp=0;
static constexpr uintptr_t RVA_ADD_OPEN_SLOT=0;
using AddOpenSlotFn=uint32_t(*)(void*);
static unsigned addCalls=0;
static bool addFault=false;
static uint32_t fakeAdd(void*) { ++addCalls;if(addFault) RaiseException(0xE0001234,0,0,nullptr);return 0; }
void dumpMpSession() {}
'''
code+=function(session,'expandSlots')
code+=region(lobby,'static Detour g_lobbyCreateDetour;','bool installLobbyGuard()')
# Worker shutdown calls are extracted unchanged. Only OS scheduling/results are controlled.
code+=r'''
static DWORD waitResult=WAIT_TIMEOUT;
static unsigned closeCount=0,eventCount=0;
static BOOL testClose(HANDLE) { ++closeCount;return TRUE; }
static DWORD testWait(HANDLE,DWORD) { return waitResult; }
static BOOL testEvent(HANDLE) { ++eventCount;return TRUE; }
static BOOL testCancel(HANDLE) { return TRUE; }
static HANDLE testCreate(const char*,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE) { return INVALID_HANDLE_VALUE; }
#define CloseHandle testClose
#define WaitForSingleObject testWait
#define SetEvent testEvent
#define CancelSynchronousIo testCancel
#define CreateFileA testCreate
static const char* kPipeName="test";
static volatile long g_ctrlExit=0;
static bool g_ctrlServerUp=true;
static HANDLE g_ctrlThread=(HANDLE)1;
static unsigned uiStops=0;
void stopUiControl() { ++uiStops; }
static bool g_installed=true;
static volatile long g_dumpExit=0;
static HANDLE g_dumpThread=(HANDLE)2,g_dumpRequest=(HANDLE)3,g_dumpDone=(HANDLE)4;
static PVOID g_lowExecuteHandler=nullptr; // current crash-dumper teardown also owns this VEH
static LONG WINAPI crashFatal(EXCEPTION_POINTERS*) { return 0; }
static LONG WINAPI previousFilter(EXCEPTION_POINTERS*) { return 0; }
static LONG WINAPI newerFilter(EXCEPTION_POINTERS*) { return 0; }
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter=&previousFilter,filter=&crashFatal;
static auto testFilter(LPTOP_LEVEL_EXCEPTION_FILTER value) { auto old=filter;filter=value;return old; }
#define SetUnhandledExceptionFilter testFilter
'''
code+=function(source('control.cpp'),'stopControlServer')+function(source('crashdump.cpp'),'removeCrashDumper')
code+=region(gift,'struct CcoEntry {','static_assert(sizeof(CcoEntry)')
code+=function(gift,'restoreCcoEntry')
code+=r'''
int main() {
    // Absent record/group cannot complete. The same incoming faction remains retryable.
    PanelCompletion c;
    recordPanelCompletion(c,1,2,3,0,2,2,0,"faction","party",0,false);
    assert(!panelCompletionMatches(c,1,2,3,4,2,2,1,"faction","party",0));
    // A present record with empty faction is allowed: incoming display is independent.
    recordPanelCompletion(c,1,2,3,4,2,2,1,"faction","party",0,true);
    assert(panelCompletionMatches(c,1,2,3,4,2,2,1,"faction","party",0));
    assert(!panelCompletionMatches(c,1,2,5,4,2,2,1,"faction","party",0)); // callback replaced
    assert(!panelCompletionMatches(c,1,2,3,4,2,2,0,"faction","party",0)); // hidden
    assert(!panelCompletionMatches(c,1,2,3,0,2,2,1,"faction","party",0)); // group lost
    assert(!panelCompletionMatches(c,9,2,3,4,2,2,1,"faction","party",0)); // lobby replaced
    assert(!panelCompletionMatches(c,1,2,3,4,3,2,1,"faction","party",0)); // unregistered
    assert(!panelCompletionMatches(c,1,2,3,4,2,2,1,"different","party",0));
    assert(!panelCompletionMatches(c,1,2,3,4,2,2,1,"faction","different",0));
    assert(!panelCompletionMatches(c,1,2,3,4,2,2,1,"faction","party",1));
    recordPanelCompletion(c,1,2,3,4,2,2,1,"faction","party",0,false);
    assert(!c.complete); // failed attempt cannot reuse earlier success
    recordPanelCompletion(c,1,2,3,4,2,2,1,"","party",0,true);assert(!c.complete);
    puts("PASS: failed display retries, empty record independent of display, callback/group/shown/owner invalidation");

    alignas(8) uint8_t mp[OFF_MP_SLOTOBJ+8]={},slots[OFF_SLOT_COUNT+4]={};
    *(uintptr_t*)(mp+OFF_MP_SLOTOBJ)=(uintptr_t)slots;
    g_capturedMp=(uintptr_t)mp;g_base=(uintptr_t)&fakeAdd;
    resetSlotExpansionBudget();
    expandSlots();assert(g_expandRuns==0 && addCalls==0); // pre-population does not spend budget
    *(uint32_t*)(slots+OFF_SLOT_COUNT)=4;expandSlots();assert(g_expandRuns==0);
    *(uint32_t*)(slots+OFF_SLOT_COUNT)=1;
    for (int i=0;i<30;++i) expandSlots();
    assert(g_expandRuns==21 && addCalls==63); // unsuccessful adds exhaust this lobby only
    resetSlotExpansionBudget();addFault=true;expandSlots();
    assert(g_expandFaulted && g_autoExpandTarget==4);
    auto spent=g_expandRuns;expandSlots();assert(g_expandRuns==spent);
    addFault=false;resetSlotExpansionBudget();expandSlots();assert(g_expandRuns==1);
    puts("PASS: 21 qualifying expansion passes, pre-population, fault suppression and next-lobby retry");

    auto* machine=(uint8_t*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    assert(machine);
    memcpy(machine,EXPECT_LOBBY_CREATE,15);
    // Synthetic factory: real stolen prologue followed by return-context + ABI epilogue.
    const uint8_t tail[]={0x48,0x89,0xD0,0x48,0x8B,0x5C,0x24,0x40,0x48,0x83,0xC4,0x20,0x5F,0xC3};
    memcpy(machine+15,tail,sizeof(tail));
    DWORD oldProtection=0;
    assert(VirtualProtect(machine,4096,PAGE_EXECUTE_READ,&oldProtection));
    g_base=(uintptr_t)machine-0x02CE3120;
    assert(detourInstall(g_lobbyCreateDetour,(uintptr_t)machine,15,EXPECT_LOBBY_CREATE,
                        (uintptr_t)&lobbyCreateHook,(void**)&g_origLobbyCreate,"test factory"));
    auto create=(uintptr_t(*)(uintptr_t,uintptr_t))machine;
    for (unsigned i=1;i<=12;++i) {
        g_expandRuns=21;g_expandFaulted=1;g_autoFactionTries=4;g_autoFactionNextTry=100000;
        g_factionRequest=2;g_panelsAdded=2;g_panelCompletion[2].complete=true;
        assert(create(0,0x123400)==0x123400); // same allocator address every generation
        assert(g_lobbyGeneration==i && giftResets==i && g_liveLobby==0x123400);
        assert(!g_expandRuns && !g_expandFaulted && !g_autoFactionTries && !g_autoFactionNextTry);
        assert(g_factionRequest==-1 && g_localSeat==-1 && !g_panelsAdded && !g_panelCompletion[2].complete);
    }
    g_expandRuns=21;
    assert(create(0,0)==0 && g_expandRuns==21 && g_lobbyGeneration==12); // failed allocation
    // Failed protection acquisition preserves the actual jump and active state.
    vpCalls=0;vpFail=1;detourRemove(g_lobbyCreateDetour,"failure");
    assert(g_lobbyCreateDetour.active && machine[0]==0xFF);
    vpCalls=0;vpFail=2;detourRemove(g_lobbyCreateDetour,"restore failure");
    assert(g_lobbyCreateDetour.active);
    vpFail=0;flushCalls=0;flushFail=1;detourRemove(g_lobbyCreateDetour,"cache failure");
    assert(g_lobbyCreateDetour.active);
    flushFail=0;detourRemove(g_lobbyCreateDetour,"retry");
    assert(!g_lobbyCreateDetour.active && !memcmp(machine,EXPECT_LOBBY_CREATE,15));
    MEMORY_BASIC_INFORMATION info={};
    assert(VirtualQuery(machine,&info,sizeof(info)) && info.Protect==PAGE_EXECUTE_READ);
    puts("PASS: real x64 factory detour, 12 reused-address lifetimes, failed allocation, removal failure and retry");

    for (DWORD failure : {DWORD(WAIT_TIMEOUT),DWORD(WAIT_FAILED)}) {
        waitResult=failure;closeCount=0;
        stopControlServer();
        assert(g_ctrlThread==(HANDLE)1 && g_ctrlServerUp && closeCount==0 && g_ctrlExit==1);
        removeCrashDumper();
        assert(g_dumpThread==(HANDLE)2 && g_dumpRequest==(HANDLE)3 && g_dumpDone==(HANDLE)4 && closeCount==0);
        assert(!g_installed && g_dumpExit==1 && g_prevFilter==&previousFilter);
    }
    waitResult=WAIT_OBJECT_0;stopControlServer();assert(!g_ctrlThread && !g_ctrlServerUp && closeCount==1);
    assert(uiStops==3); // The UI message queue stops before every control-server join attempt.
    filter=&newerFilter;removeCrashDumper();
    assert(!g_dumpThread && closeCount==2 && filter==&newerFilter);
    assert(g_dumpRequest==(HANDLE)3 && g_dumpDone==(HANDLE)4); // filters in flight retain events
    puts("PASS: timeout and WAIT_FAILED retain worker resources; retry joins; newer filter chain retained");

    assert(VirtualProtect(machine,4096,PAGE_READWRITE,&oldProtection));
    auto* entry=(CcoEntry*)machine;
    CcoEntry original={(void*)0x1234,2,99,"original"};
    *entry={(void*)0x5678,0,42,"hijacked"};volatile bool active=true;
    assert(VirtualProtect(machine,4096,PAGE_READONLY,&oldProtection));
    DWORD removalProtection=0;
    g_base=(uintptr_t)machine;vpCalls=0;vpFail=1;
    restoreCcoEntry(0,original,active,removalProtection);assert(active && entry->fn==(void*)0x5678);
    vpCalls=0;vpFail=2;restoreCcoEntry(0,original,active,removalProtection);assert(active);
    vpFail=0;restoreCcoEntry(0,original,active,removalProtection);assert(!active && !memcmp(entry,&original,sizeof(original)));
    assert(VirtualQuery(machine,&info,sizeof(info)) && info.Protect==PAGE_READONLY);
    puts("PASS: full CCO entry restoration, failure ownership and retry");
}
'''
code=code.replace('#include <cassert>','#include <cassert>\n#include <initializer_list>')
fixture=out/'audit_lifecycle_fixture.cpp';fixture.write_text(code,encoding='utf-8')
exe=out/'audit_lifecycle_fixture.exe'
subprocess.run(['cl','/nologo','/EHa','/std:c++17','/utf-8','/I'+str(root/'runtime/src'),str(fixture),'/Fe:'+str(exe),'/Fo:'+str(out/'audit_lifecycle_fixture.obj')],check=True)
subprocess.run([str(exe)],check=True)
main=source('main.cpp')
assert 'FreeLibraryAndExitThread(g_selfModule' not in main
assert 'releaseArmedMutex();' not in main[main.index('detach requested'):]
assert panels.index('panelCompletionMatches(completion') > panels.index('GetPanelCbFn)(g_base + RVA_GET_PANEL_CB))(panel)', panels.index('static bool handleExtraSlotChange'))
assert 'InterlockedCompareExchange(&g_inExtraHandler, 1, 0)' in panels
assert 'restoreCcoEntry(RVA_CCO_DEVKILLENTIREARMY' in gift
assert 'restoreCcoEntry(RVA_CCO_DEVCYCLEARMY' in gift
assert 'g_expandRuns > 20' in session and 'g_autoFactionTries >= kAutoFactionMaxTries' in lobby
print('PASS: resident detach, retained re-entry brake, both CCO removers and bounded retry guards wired')
