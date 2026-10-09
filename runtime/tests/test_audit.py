"""Offline regression tests using the actual C++ helpers; no game process is loaded.
Run from an MSVC x64 environment: python runtime/tests/test_audit.py <output-dir>
"""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
event = (root / 'runtime/src/eventcursor.cpp').read_text(encoding='utf-8-sig')
blend = (root / 'runtime/src/turnblend.cpp').read_text(encoding='utf-8-sig')
gift = (root / 'runtime/src/gift.cpp').read_text(encoding='utf-8-sig')

def function(source, name):
    start = re.search(r'(?m)^(?:static )?[^\n;{}]+\b' + name + r'\([^\n]*\)\n\{', source)
    assert start, name
    end = re.search(r'(?m)^}', source[start.start():])
    assert end, name
    return source[start.start():start.start() + end.end()] + '\n'

def region(source, start, end):
    return source[source.index(start):source.index(end, source.index(start))]

code = r'''
#include "build_mode.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
using std::uintptr_t;
using DWORD = unsigned long;
static void logf(const char*, ...) {}
static long InterlockedExchange(volatile long* p, long value) { auto old=*p; *p=value; return old; }
static bool tickActive = true;
static bool campaignTickHookActive() { return tickActive; }
struct FakeDetour { bool active; };
static FakeDetour g_cursorWriteDetour{true}, g_collectDetour{true};
static volatile long g_holdArmed = 0;
'''
code += region(event, 'struct SideCursor {', 'static SideCursor* sideFind(')
code += ''.join(function(event, f) for f in ('sideFind', 'sideFindOrAdd', 'resetSideCursors', 'setHumanFactionCountHold'))
# Exercise the real destructor hook with a fake original that requires invalidation before free.
code += r'''
static void fakeDtor(uintptr_t, uintptr_t mgr) { for (const auto& s:g_side) assert(!s.used || s.manager!=mgr); }
static auto g_origEventMgrDtor = &fakeDtor;
'''
code += function(event, 'eventMgrDtorHook')
code += r'''
static unsigned char managerMemory[0x300]{};
static int currentIndex=2;
static unsigned collectCalls=0;
static uint32_t observedCursor=0;
static int registryIndexOf(uintptr_t, uintptr_t) { return currentIndex; }
template<typename T> static bool readAt(uintptr_t addr,T& out) { memcpy(&out,(void*)addr,sizeof(T)); return true; }
static bool safeRead(const void* from,void* to,size_t n) { memcpy(to,from,n); return true; }
static bool safeWriteBytes(uintptr_t to,const void* from,size_t n) { memcpy((void*)to,from,n); return true; }
static constexpr size_t BORROW_OFF=0x1F8, BORROW_LEN=8, OFF_EVMGR_CURSOR0=0x1F0;
static constexpr unsigned EVMGR_CURSOR_SLOTS=2, CURSOR_SLOTS_MAX=4;
static uintptr_t fakeCollect(uintptr_t mgr,uintptr_t out,uintptr_t,uintptr_t) {
    ++collectCalls;
    observedCursor=*(uint32_t*)(mgr+OFF_EVMGR_CURSOR0+currentIndex*4);
    return out;
}
static auto g_origCollect = &fakeCollect;
'''
code += function(event, 'collectHook')
code += r'''
static constexpr DWORD PAGE_EXECUTE_READWRITE=0x40, PAGE_EXECUTE_READ=0x20;
static unsigned char patchMemory[64]{};
static DWORD protection[2]={PAGE_EXECUTE_READ,PAGE_EXECUTE_READ};
static int protectCall=0, failProtect=0, flushCall=0, failFlush=0;
static bool VirtualProtect(void* addr,size_t,DWORD to,DWORD* old) {
    if (++protectCall==failProtect) return false;
    size_t page=((unsigned char*)addr-patchMemory)/32;
    *old=protection[page]; protection[page]=to; return true;
}
static bool FlushInstructionCache(void*,void*,size_t) { return ++flushCall!=failFlush; }
static void* GetCurrentProcess() { return nullptr; }
static DWORD GetLastError() { return 5; }
'''
code += region(blend, 'struct BlendWrite {', '// Verify every site we intend to touch')
code += r'''
static constexpr int MAX_RECIPIENTS=4;
static int g_recipient[4]={1,2,3,-1}, g_recipientCount=3,g_recipientOverflow=1;
static bool g_giftPanelWanted=true;
static void* g_lastBattleMgr=(void*)1;
static uintptr_t g_lastCmdObj=1;
'''
code += function(gift, 'resetGiftPanelState')
code += 'static int slotForPlayer(int id) { static int map[]={0,1,3,-1}; return id>=0 && id<4 ? map[id] : -1; }\n'
code += function(gift, 'lobbyRecordIdForRecipient')

code += r'''
int main() {
    assert(lobbyRecordIdForRecipient(0)==0 && lobbyRecordIdForRecipient(1)==1);
    assert(lobbyRecordIdForRecipient(2)==3 && lobbyRecordIdForRecipient(3)==-1);
    assert(lobbyRecordIdForRecipient(-1)==-1 && lobbyRecordIdForRecipient(20)==-1);
    puts("PASS: recipient 2 joins lobby name ID 3; absent seats never fall back to roster position");
    g_gatePatched=true;
    // More than nine generations, alternating allocator reuse and new addresses, with turn writes.
    for (uintptr_t generation=1; generation<=12; ++generation) {
        uintptr_t mgr=(generation%2) ? 0x1000 : 0x1000+generation*0x100;
        for (uintptr_t turn=1;turn<=5;++turn) for (uintptr_t faction=2;faction<4;++faction) {
            auto* s=sideFindOrAdd(mgr,faction*0x10000); assert(s); s->cursor=(uint32_t)(turn*100);
            assert(sideFind(mgr,faction*0x10000)->cursor==turn*100);
        }
        eventMgrDtorHook(0,mgr);
        assert(!sideFind(mgr,0x20000));
        auto* fresh=sideFindOrAdd(mgr,0x20000); assert(fresh && fresh->cursor==0);
        resetSideCursors(0); // exact callback operation used by HUMAN_FACTIONS deserialization
    }
    auto* a=sideFindOrAdd(0x1000,0x20000); a->cursor=900;
    auto* b=sideFindOrAdd(0x2000,0x20000); assert(b && b->cursor==0);
    eventMgrDtorHook(0,0x2000); assert(sideFind(0x1000,0x20000)->cursor==900);
    resetSideCursors(0); assert(!sideFind(0x1000,0x20000));

    uintptr_t mgr=(uintptr_t)managerMemory;
    *(uint32_t*)(mgr+0x30)=250;
    *(uint64_t*)(mgr+BORROW_OFF)=0x12345678000000EBull;
    auto* cursor=sideFindOrAdd(mgr,0x20000); cursor->cursor=900;
    assert(collectHook(mgr,123,0x20000,0)==123 && observedCursor==0);
    assert(cursor->cursor==0 && g_sideOutOfRange==1);
    assert(*(uint64_t*)(mgr+BORROW_OFF)==0x12345678000000EBull);
    cursor->cursor=200; collectHook(mgr,123,0x20000,0); assert(observedCursor==200);
    currentIndex=1; *(uint32_t*)(mgr+0x1F4)=17;
    collectHook(mgr,123,0x20000,0); assert(observedCursor==17);
    assert(collectCalls==3);
    puts("PASS: 12 cursor lifetimes, reused addresses, owner isolation, load reset, earlier-save bounds, vanilla slots");

    const char* why=nullptr;
    tickActive=false; assert(!setHumanFactionCountHold(true,&why) && why && !g_holdArmed);
    tickActive=true; g_gatePatched=false; assert(!setHumanFactionCountHold(true,&why));
    g_gatePatched=true; assert(setHumanFactionCountHold(true,&why) && g_holdArmed);
    *(uint32_t*)(mgr+0x22C)=1;
    assert(setHumanFactionCountHold(false,nullptr) && !g_holdArmed);
    assert(*(uint32_t*)(mgr+0x22C)==1);
    puts("PASS: tick dependency and disarm without replicated rollback");

    for (int failure=0;failure<=4;++failure) {
        memset(patchMemory,0,sizeof(patchMemory));
        protection[0]=protection[1]=PAGE_EXECUTE_READ;
        protectCall=flushCall=0; failProtect=failure;
        BlendWrite writes[4]{};
        for(int i=0;i<4;++i) { writes[i].address=(uintptr_t)(patchMemory+i*16); writes[i].length=4; memset(writes[i].bytes,7,4); }
        bool committed=false;
        bool ok=applyBlendWrites(writes,4,committed);
        assert(ok==(failure==0) && committed==(failure==0));
        for(int i=0;i<64;++i) assert(patchMemory[i]==((!failure && i%16<4)?7:0));
        assert(protection[0]==PAGE_EXECUTE_READ && protection[1]==PAGE_EXECUTE_READ);
    }
    for(int failure=0;failure<2;++failure) {
        BlendWrite write{}; write.address=(uintptr_t)patchMemory; write.length=4;
        protectCall=flushCall=0; failProtect=failure?2:0; failFlush=failure?0:1;
        bool committed=false;
        assert(!applyBlendWrites(&write,1,committed) && committed);
    }
    puts("PASS: each protection acquisition failure leaves bytes unchanged; shared-page restoration; cache/restore failures reported");
    resetGiftPanelState("test boundary");
    assert(!g_giftPanelWanted && g_recipientCount==0 && g_recipientOverflow==0);
    for(int id:g_recipient) assert(id==-1);
    assert(!g_lastBattleMgr && !g_lastCmdObj);
    resetGiftPanelState("repeated boundary");
    puts("PASS: open gift snapshot invalidated at a lifecycle boundary");
}
'''
fixture = out / 'audit_fixture.cpp'
fixture.write_text(code, encoding='utf-8')
exe = out / 'audit_fixture.exe'
subprocess.run(['cl','/nologo','/EHsc','/std:c++17','/utf-8','/I'+str(root/'runtime/src'),str(fixture),'/Fe:'+str(exe),'/Fo:'+str(out / 'audit_fixture.obj')], check=True)
subprocess.run([str(exe)], check=True)
# The ordering is part of the contract, not just the isolated arming predicate.
main=(root/'runtime/src/main.cpp').read_text(encoding='utf-8-sig')
assert main.index('if (installCampaignTickHook())') < main.index('setHumanFactionCountHold(true')
assert 'if (!installEventMgrLifetimeHook() || !installHumanFactionLoadHook()) return false;' in event
assert 'resetSideCursors(0);' in function(event,'factionByIdHook')
print('PASS: attach prerequisites and load invalidation wired into production paths')

assert "knownPlayerNameCa(nameId)" in function(gift,"ccoReturnPlayerName")
assert "lobbyRecordIdForRecipient(id)" in function(gift,"ccoReturnPlayerName")

# Exercise the new +9 detour with actual x64 instructions and the production detour installer.
# This is a synthetic destructor body in this test process, never the game executable.
native = r'''
#include "tw3k.h"
#include <cassert>
uintptr_t g_base=0;
void logf(const char*, ...) {}
bool safeRead(const void* from, void* to, size_t n) { memcpy(to,from,n); return true; }
#include "detour.cpp"
'''
native += region(event, 'struct SideCursor {', 'static SideCursor* sideFind(')
native += ''.join(function(event,f) for f in ('sideFind','sideFindOrAdd','resetSideCursors'))
native += region(event, 'static constexpr uintptr_t RVA_EVENT_MGR_DTOR_BODY', '// A guarded write.')
native += r'''
int main() {
    auto* code=(uint8_t*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    assert(code);
    const uint8_t prefix[]={0x48,0x85,0xD2,0x0F,0x84,0x9C,0x01,0x00,0x00};
    memcpy(code,prefix,sizeof(prefix));
    memcpy(code+9,EXPECT_EVENT_MGR_DTOR_BODY,sizeof(EXPECT_EVENT_MGR_DTOR_BODY));
    // Mark the original body as executed, then unwind its real stolen stack/register prologue.
    const uint8_t tail[]={0xC7,0x87,0xF8,0x01,0x00,0x00,0,0,0,0, 0x48,0x83,0xC4,0x40,0x5F,0xC3};
    memcpy(code+24,tail,sizeof(tail)); code[0x1A5]=0xC3;
    FlushInstructionCache(GetCurrentProcess(),code,4096);
    g_base=(uintptr_t)code-(RVA_EVENT_MGR_DTOR_BODY-9);
    code[0]=0x90; assert(!installEventMgrLifetimeHook()); code[0]=prefix[0];
    assert(installEventMgrLifetimeHook());
    alignas(16) uint8_t manager[0x300]{};
    uintptr_t mgr=(uintptr_t)manager;
    auto call=(EventMgrDtorFn)code;
    sideFindOrAdd(mgr,123)->cursor=900;
    *(uint32_t*)(mgr+0x1F8)=235;
    call(0,mgr);
    assert(!sideFind(mgr,123) && *(uint32_t*)(mgr+0x1F8)==0);
    long before=g_sideResets; call(0,0); assert(g_sideResets==before);
    detourRemove(g_eventMgrDtorDetour,"test");
    assert(!memcmp(code+9,EXPECT_EVENT_MGR_DTOR_BODY,sizeof(EXPECT_EVENT_MGR_DTOR_BODY)));
    sideFindOrAdd(mgr,123)->cursor=10;
    call(0,mgr); assert(sideFind(mgr,123)->cursor==10);
    VirtualFree(code,0,MEM_RELEASE);
    puts("PASS: native +9 destructor detour, signature refusal, trampoline execution, null bypass and removal");
}
'''
fixture=out/'audit_native_fixture.cpp'
fixture.write_text(native,encoding='utf-8')
exe=out/'audit_native_fixture.exe'
subprocess.run(['cl','/nologo','/EHa','/std:c++17','/utf-8','/I'+str(root/'runtime/src'),str(fixture),'/Fe:'+str(exe),'/Fo:'+str(out/'audit_native_fixture.obj')],check=True)
subprocess.run([str(exe)],check=True)
