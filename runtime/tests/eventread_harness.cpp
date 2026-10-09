// Offline fixture: production helper/module and real Windows executable patch memory.
#include "tw3k.h"
#include "eventread.h"
#include <cassert>
#include <map>
#include <vector>
#include <thread>
#include <stdexcept>
uintptr_t g_base=0;
void logf(const char*,...) {}
static std::map<uintptr_t,uint8_t> memory;
static uintptr_t denied=0;
static size_t reads=0;
static bool fakeRead(uintptr_t a,void* out,size_t n) {
    ++reads;
    auto* p=(uint8_t*)out;
    for (size_t i=0;i<n;++i) {
        if (a+i==denied) return false;
        auto it=memory.find(a+i); if (it==memory.end()) return false; p[i]=it->second;
    }
    return true;
}
bool safeRead(const void* a,void* out,size_t n) {
    if (memory.count((uintptr_t)a)) return fakeRead((uintptr_t)a,out,n);
    __try { memcpy(out,a,n); return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> static void put(uintptr_t a,T v) {
    auto* b=(uint8_t*)&v; for (size_t i=0;i<sizeof(v);++i) memory[a+i]=b[i];
}
static int protectCalls=0,failProtect=0,flushCalls=0,failFlush=0;
static BOOL protect(void* a,SIZE_T n,DWORD p,DWORD* old) {
    if (++protectCalls==failProtect) return FALSE;
    return VirtualProtect(a,n,p,old);
}
static BOOL flush(HANDLE h,const void* a,SIZE_T n) {
    if (++flushCalls==failFlush) return FALSE;
    return FlushInstructionCache(h,a,n);
}
#define VirtualProtect protect
#define FlushInstructionCache flush
#include "detour.cpp"
#include "eventread.cpp"
#undef VirtualProtect
#undef FlushInstructionCache

static constexpr uintptr_t slot=0x100000, root=0x200000, obj=0x300000, model=0x400000;
static constexpr uintptr_t mgr=0x292A4C841D0, self=0x292D6B22780, collection=0x292D69DA5E0;
static constexpr uintptr_t mine=0x292AF7E07E0, acceptedData=0x292900CFF60;
static constexpr uintptr_t registryData=0x292AE5DCDE0, candidate=0x900000;
static void snapshot() {
    memory.clear(); denied=0; reads=0;
    put(slot,root);put(root+0x2188,obj);put(obj+0x78,model);put(model+0x3D30,mgr);
    put(obj+0x1A8,mine);put(self+0x90,collection);put(collection,mgr);put(collection+0x18,mine);
    put(mgr+0x260,uint32_t(123));
    put(mgr+0x228,eventread::Vector{4,4,registryData});
    put(self+0xA8,eventread::Vector{32,20,acceptedData});
    put(candidate+0x88,uint64_t(123)|(uint64_t(4427)<<32));
    #include "eventread_snapshot.inc"
}
static bool query(bool native=false,uintptr_t f=mine,uintptr_t s=self) {
    auto r=fakeRead;
    return eventread::containsRead(native,s,candidate,f,slot,r);
}
static void helperTests() {
    snapshot(); assert(query());
    // Replay all 19 unread siblings using their actual captured identities and addresses.
    for (unsigned i=1;i<20;++i) {
        uintptr_t msg=0; assert(fakeRead(acceptedData+i*16,&msg,8));
        auto r=fakeRead;
        assert(eventread::containsRead(false,self,msg,mine,slot,r));
    }
    assert(!query(false,mine,0));
    reads=0; assert(query(true)); assert(reads==0);
    for (unsigned index=0;index<4;++index) {
        snapshot(); uintptr_t f=0; assert(fakeRead(registryData+index*8,&f,8));
        put(obj+0x1A8,f);put(collection+0x18,f);
        // Make the retained read marker use this local faction.
        uintptr_t msg=0,data=0;fakeRead(acceptedData,&msg,8);fakeRead(msg+0x98,&data,8);put(data,f);
        assert(query(false,f)==(index>=2));
    }
    snapshot(); assert(!query(false,mine+8));
    put(candidate+0x88,uint64_t(124)|(uint64_t(4427)<<32));assert(!query());
    snapshot();put(candidate+0x88,uint64_t(123)|(uint64_t(4428)<<32));assert(!query());
    // Old accepted identities cannot suppress after compaction/reused indices.
    snapshot();put(mgr+0x260,uint32_t(124));assert(!query());
    put(candidate+0x88,uint64_t(124)|(uint64_t(4427)<<32));assert(!query());
    // Same feed/manager addresses recreated with no retained read evidence.
    snapshot();put(self+0xA8,eventread::Vector{0,0,0});assert(!query());
    snapshot();put(collection,mgr+0x1000);assert(!query());
    snapshot();put(model+0x3D30,mgr+0x1000);assert(!query());
    snapshot();put(collection+0x18,mine+8);assert(!query());
    snapshot();put(mgr+0x228,eventread::Vector{4,5,registryData});assert(!query());
    snapshot();put(registryData,mine);assert(!query()); // ambiguous duplicate
    for (auto v: {eventread::Vector{0,1,acceptedData},eventread::Vector{4097,4097,acceptedData},
                  eventread::Vector{1,1,0},eventread::Vector{1,1,UINTPTR_MAX-8}}) {
        snapshot();put(self+0xA8,v);assert(!query());assert(reads<40);
    }
    snapshot();denied=self+0xAC;assert(!query());
    snapshot();
    uintptr_t first=0;fakeRead(acceptedData,&first,8);
    for (uint32_t i=0;i<eventread::maxAccepted;++i) {
        put(acceptedData+i*16,first);put(acceptedData+i*16+8,first-16);
    }
    put(self+0xA8,eventread::Vector{eventread::maxAccepted,eventread::maxAccepted,acceptedData});
    reads=0;assert(query());assert(reads<eventread::maxAccepted*6+64);
    snapshot();denied=acceptedData+8;assert(!query());
    snapshot();put(acceptedData+8,uintptr_t(0));assert(!query());
    uintptr_t msg=0,data=0;
    for (auto v: {eventread::Vector{0,1,0xA00000},eventread::Vector{65,65,0xA00000},
                  eventread::Vector{1,1,0}}) {
        snapshot();fakeRead(acceptedData,&msg,8);put(msg+0x90,v);assert(!query());
    }
    snapshot();fakeRead(acceptedData,&msg,8);fakeRead(msg+0x98,&data,8);
    denied=data;assert(!query());denied=0;put(data,mine+8);assert(!query());
    put(msg+0x90,eventread::Vector{0,0,0});assert(!query());
    // Owner changes during scan: revalidation must reject evidence.
    snapshot(); auto changing=[](uintptr_t a,void* b,size_t n) {
        bool ok=fakeRead(a,b,n);
        if (a==acceptedData+19*16) put(mgr+0x260,uint32_t(124));
        return ok;
    };
    assert(!eventread::containsRead(false,self,candidate,mine,slot,changing));
    assert(eventread::feed==0);
    { eventread::Scope outer(self);assert(eventread::feed==self);
      try {eventread::Scope inner(self+1);assert(eventread::feed==self+1);throw std::runtime_error("nested");}
      catch (...) {} assert(eventread::feed==self);
      std::thread t([] {assert(eventread::feed==0);eventread::Scope x(99);assert(eventread::feed==99);});t.join();
    } assert(eventread::feed==0);
    puts("PASS: October 8 replay, slots 0/1/2/3, exact identities, reuse/generation, malformed/unreadable vectors, bounded fail-open, reentrant TLS");
}
static int originalCalls=0,observerCalls=0;
static void original(void* p,void* sink) {assert(p==(void*)self && sink==(void*)42);assert(eventread::feed==self);++originalCalls;}
static void observer(void* p,void* sink,NextAutoOpenFn fn) {++observerCalls;fn(p,sink);}
static void restoreImage() {
    failProtect=failFlush=0;protectCalls=flushCalls=0;
    memcpy((void*)(g_base+kGetter),kEntry,sizeof(kEntry));
    memcpy((void*)(g_base+kCall),kNativeCall,5);memcpy((void*)(g_base+kCall+5),kClause,sizeof(kClause));
}
static void lifecycleTests() {
    g_base=(uintptr_t)VirtualAlloc(nullptr,0x04400000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    assert(g_base);restoreImage();
    for (uintptr_t bad: {kGetter,kCall,kCall+5}) {
        *((uint8_t*)(g_base+bad))^=1;assert(!installEventReadHook());
        assert(!g_eventReadCall.active && !g_eventReadEntry.active);restoreImage();
    }
    // Inject every protection/cache failure in installation, including after bytes went live.
    for (int which=1;which<=4;++which) {
        restoreImage();failProtect=which;assert(!installEventReadHook());
        failProtect=0;removeEventReadHook();
        assert(!g_eventReadCall.active && !g_eventReadEntry.active);
        assert(bytesMatch(g_base+kCall,kNativeCall,5));assert(bytesMatch(g_base+kGetter,kEntry,14));
    }
    for (int which=1;which<=4;++which) {
        restoreImage();failFlush=which;assert(!installEventReadHook());
        failFlush=0;removeEventReadHook();
        assert(!g_eventReadCall.active && !g_eventReadEntry.active);
        assert(bytesMatch(g_base+kCall,kNativeCall,5));assert(bytesMatch(g_base+kGetter,kEntry,14));
    }
    restoreImage();assert(installEventReadHook());assert(eventReadInstalled());assert(installEventReadHook());
    auto* page=g_eventReadCall.page;assert(page[0]==0xFF && page[1]==0x25);
    int32_t disp=0;memcpy(&disp,(void*)(g_base+kCall+1),4);
    assert(g_base+kCall+5+(intptr_t)disp==(uintptr_t)page);
    assert(bytesMatch(g_base+kCall+5,kClause,sizeof(kClause)));
    // Execute the unchanged native clause bytes in synthetic memory, all eight truth-table rows.
    uint8_t* gate=(uint8_t*)VirtualAlloc(nullptr,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    assert(gate);
    uint8_t setup[]={0x53,0x57,0x88,0xCB,0x88,0xD0,0x44,0x88,0xC7};
    memcpy(gate,setup,sizeof(setup));memcpy(gate+sizeof(setup),kClause,sizeof(kClause));
    uint8_t reject[]={0x5F,0x5B,0x31,0xC0,0xC3};
    uint8_t accept[]={0x5F,0x5B,0xB8,1,0,0,0,0xC3};
    memcpy(gate+sizeof(setup)+13,reject,sizeof(reject));
    memcpy(gate+sizeof(setup)+59,accept,sizeof(accept));
    using Gate=bool (*)(uint8_t,uint8_t,uint8_t);
    for (int a=0;a<2;++a) for (int read=0;read<2;++read) for (int response=0;response<2;++response)
        assert(((Gate)gate)(a,read,response)==bool(a && (!read || response)));
    VirtualFree(gate,0,MEM_RELEASE);
    auto saved=g_eventReadOriginal;g_eventReadOriginal=original;
    setNextAutoOpenObserver(observer);eventReadGetter((void*)self,(void*)42);
    assert(originalCalls==1 && observerCalls==1 && eventread::feed==0);
    setNextAutoOpenObserver(nullptr);eventReadGetter((void*)self,(void*)42);assert(originalCalls==2);
    g_eventReadOriginal=saved;
    // Call the actual production wrapper with a synthetic native leaf: AL only, never game code.
    snapshot(); put(g_base+RVA_CAMPAIGN_ROOT,root);
    uint8_t nativeFalse[]={0x48,0xB8,0,1,0,0,0,0,0,0,0xC3}; // RAX=0x100, AL=false
    memcpy((void*)(g_base+kLeaf),nativeFalse,sizeof(nativeFalse));
    assert(eventReadLeaf((void*)candidate,(void*)mine)==0); // no TLS scope
    {eventread::Scope scope(self);assert(eventReadLeaf((void*)candidate,(void*)mine)==1);}
    uint8_t nativeTrue[]={0xB8,1,0,0,0,0xC3};memcpy((void*)(g_base+kLeaf),nativeTrue,sizeof(nativeTrue));
    assert(eventReadLeaf((void*)candidate,(void*)mine)==1);
    // Third-party replacement is not overwritten; disable still stops supplemental reads.
    *((uint8_t*)(g_base+kCall))=0x90;removeEventReadHook();assert(g_eventReadCall.active);
    assert(!eventReadInstalled());*((uint8_t*)(g_base+kCall))=0xE8;removeEventReadHook();
    assert(!g_eventReadCall.active && !g_eventReadEntry.active);
    assert(bytesMatch(g_base+kCall,kNativeCall,5));assert(bytesMatch(g_base+kGetter,kEntry,14));
    assert(page[0]==0xFF); // in-flight executable backing retained
    restoreImage();assert(installEventReadHook());
    failProtect=protectCalls+1;removeEventReadHook();assert(g_eventReadCall.active);
    failProtect=0;removeEventReadHook();assert(!g_eventReadCall.active && !g_eventReadEntry.active);
    removeEventReadHook();assert(!eventReadInstalled());
    puts("PASS: production install signatures, rollback failures, CALL round trip, actual native clause truth table, original/observer, AL result, ownership-safe cleanup and retry");
}
int main() {helperTests();lifecycleTests();}
