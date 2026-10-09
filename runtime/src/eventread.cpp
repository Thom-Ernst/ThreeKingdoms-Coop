// #56 client-local auto-open containment. Release-enabled; no model/save writes.
#include "tw3k.h"
#include "eventread.h"
#include <atomic>

static constexpr uintptr_t kGetter=0x02F72090, kCall=0x02F72203, kLeaf=0x014D8610;
// Five complete instructions, RIP-free. MOV R11,RSP is replayed by the trampoline.
static const uint8_t kEntry[] = {0x4C,0x8B,0xDC,0x55,0x56,0x41,0x57,0x49,0x8D,0xAB,0xA8,0xFD,0xFF,0xFF};
static const uint8_t kNativeCall[] = {0xE8,0x08,0x64,0x56,0xFE};
// The unmodified clause: autoOpen && (!IsRead || RequiresResponse).
static const uint8_t kClause[] = {0x84,0xDB,0x74,0x09,0x84,0xC0,0x74,0x33,0x40,0x84,0xFF,0x75,0x2E};
static Detour g_eventReadEntry, g_eventReadCall;
static NextAutoOpenFn g_eventReadOriginal=nullptr;
static std::atomic<NextAutoOpenObserver> g_eventReadObserver{nullptr};
static std::atomic<bool> g_eventReadEnabled{false};

void setNextAutoOpenObserver(NextAutoOpenObserver observer) { g_eventReadObserver.store(observer); }
bool eventReadInstalled() { return g_eventReadEnabled.load() && g_eventReadEntry.active && g_eventReadCall.active; }
static void eventReadGetter(void* self, void* sink) {
    eventread::Scope scope((uintptr_t)self);
    auto observer=g_eventReadObserver.load();
    if (observer) observer(self,sink,g_eventReadOriginal);
    else g_eventReadOriginal(self,sink);
}
static uint8_t eventReadLeaf(void* msg, void* faction) {
    // Deliberately call the native leaf entry: existing generic probe still wraps it.
    // Its return-address filter will miss this routed call; it is not suppression evidence.
    using Native=uint8_t (*)(void*,void*);
    const bool native=((Native)(g_base+kLeaf))(msg,faction)!=0;
    if (!g_eventReadEnabled.load()) return native;
    auto reader=[](uintptr_t a,void* b,size_t n) { return safeRead((void*)a,b,n); };
    return eventread::containsRead(native,eventread::feed,(uintptr_t)msg,(uintptr_t)faction,
                                   g_base+RVA_CAMPAIGN_ROOT,reader);
}
static bool bytesMatch(uintptr_t site,const uint8_t* expected,size_t n) {
    uint8_t bytes[32]{};
    return n<=sizeof(bytes) && safeRead((void*)site,bytes,n) && !memcmp(bytes,expected,n);
}
static void releaseUnused(Detour& d) {
    // Active or partially removed hooks must retain executable backing for retries.
    if (!d.active && d.page) { VirtualFree(d.page,0,MEM_RELEASE); d.page=nullptr; }
}
static void absoluteJump(uint8_t* p,uintptr_t dest) {
    p[0]=0xFF; p[1]=0x25; memset(p+2,0,4); memcpy(p+6,&dest,8);
}
static bool installScopeEntry() {
    auto& d=g_eventReadEntry;
    if (!bytesMatch(g_base+kGetter,kEntry,sizeof(kEntry))) return false;
    d.page=(uint8_t*)VirtualAlloc(nullptr,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    if (!d.page) return false;
    uint8_t* trampoline=d.page+0x40;
    memcpy(trampoline,kEntry,sizeof(kEntry));
    absoluteJump(trampoline+sizeof(kEntry),g_base+kGetter+sizeof(kEntry));
    if (!FlushInstructionCache(GetCurrentProcess(),trampoline,sizeof(kEntry)+14)) {
        releaseUnused(d); return false;
    }
    g_eventReadOriginal=(NextAutoOpenFn)trampoline; // publish before hook becomes reachable
    DWORD old=0;
    if (!VirtualProtect((void*)(g_base+kGetter),sizeof(kEntry),PAGE_EXECUTE_READWRITE,&old)) {
        releaseUnused(d); return false;
    }
    uint8_t patch[14]; absoluteJump(patch,(uintptr_t)&eventReadGetter);
    d.target=g_base+kGetter; d.len=sizeof(kEntry); memcpy(d.orig,kEntry,sizeof(kEntry));
    memcpy((void*)d.target,patch,sizeof(patch)); d.active=true;
    DWORD unused=0;
    const bool flushed=FlushInstructionCache(GetCurrentProcess(),(void*)d.target,d.len)!=0;
    const bool protectedAgain=VirtualProtect((void*)d.target,d.len,old,&unused)!=0;
    if (!protectedAgain) d.removalProtection=old;
    return flushed && protectedAgain;
}
bool installEventReadHook() {
    if (eventReadInstalled()) return true;
    if (g_eventReadEntry.active || g_eventReadCall.active) return false;
    if (!bytesMatch(g_base+kGetter,kEntry,sizeof(kEntry)) ||
        !bytesMatch(g_base+kCall,kNativeCall,sizeof(kNativeCall)) ||
        !bytesMatch(g_base+kCall+5,kClause,sizeof(kClause))) {
        logf("Event read containment: signature mismatch/unreadable; refusing installation"); return false;
    }
    auto& d=g_eventReadCall;
    d.page=(uint8_t*)allocNear(g_base+kCall);
    if (!d.page) return false;
    // CALL rel32 -> JMP [RIP+0] helper. No register clobber, original return address retained
    // by the stub, Win64 caller shadow space and alignment unchanged.
    d.page[0]=0xFF; d.page[1]=0x25; memset(d.page+2,0,4);
    uintptr_t helper=(uintptr_t)&eventReadLeaf; memcpy(d.page+6,&helper,8);
    const int64_t disp=(int64_t)(uintptr_t)d.page-(int64_t)(g_base+kCall+5);
    if (disp<INT32_MIN || disp>INT32_MAX ||
        !FlushInstructionCache(GetCurrentProcess(),d.page,14)) { releaseUnused(d); return false; }
    DWORD old=0;
    if (!VirtualProtect((void*)(g_base+kCall),5,PAGE_EXECUTE_READWRITE,&old)) {
        releaseUnused(d); return false;
    }
    d.target=g_base+kCall; d.len=5; memcpy(d.orig,kNativeCall,5);
    uint8_t patch[5]={0xE8}; int32_t rel=(int32_t)disp; memcpy(patch+1,&rel,4);
    memcpy((void*)d.target,patch,5); d.active=true;
    DWORD unused=0;
    const bool flushed=FlushInstructionCache(GetCurrentProcess(),(void*)d.target,5)!=0;
    const bool protectedAgain=VirtualProtect((void*)d.target,5,old,&unused)!=0;
    if (!protectedAgain) d.removalProtection=old;
    if (!flushed || !protectedAgain || !installScopeEntry()) {
        removeEventReadHook(); return false;
    }
    g_eventReadEnabled.store(true);
    logf("Event read containment installed");
    diagLogf("Event read containment: native IsRead probe misses routed auto-open calls; "
         "zero probe calls means unobserved, not absent candidates");
    return true;
}
void removeEventReadHook() {
    g_eventReadEnabled.store(false);
    g_eventReadObserver.store(nullptr);
    // Restore the CALL first; a getter still in flight then uses the native leaf.
    if (g_eventReadCall.active) {
        uint8_t patch[5]={0xE8};
        int32_t disp=(int32_t)((intptr_t)g_eventReadCall.page-(intptr_t)(g_base+kCall+5));
        memcpy(patch+1,&disp,4);
        if (bytesMatch(g_eventReadCall.target,patch,5) ||
            bytesMatch(g_eventReadCall.target,kNativeCall,5))
            detourRemove(g_eventReadCall,"event read call");
        else logf("Event read call removal refused: bytes no longer owned; restart required");
    }
    if (g_eventReadEntry.active) {
        uint8_t patch[14]; absoluteJump(patch,(uintptr_t)&eventReadGetter);
        if (bytesMatch(g_eventReadEntry.target,patch,14) ||
            bytesMatch(g_eventReadEntry.target,kEntry,sizeof(kEntry)))
            detourRemove(g_eventReadEntry,"event read scope");
        else logf("Event read scope removal refused: bytes no longer owned; restart required");
    }
    // Like existing runtime detours, successfully removed executable pages remain resident:
    // panic/detach has no game-thread quiescence guarantee. Only never-live pages are freed.
}
