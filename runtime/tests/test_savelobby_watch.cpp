// Real Windows hardware write-breakpoint round trip, in THIS test process only.
#include "../src/savelobby_watch.cpp"
#include <cassert>
uintptr_t g_base=0;
void logf(const char*,...) {}
void appendf(char*,size_t,const char*,...) {}
bool lobbyLooksLive(uintptr_t) { return false; }
bool readCaString(void*,char*,size_t) { return false; }
bool safeRead(const void* p,void* out,size_t n) {
    __try { memcpy(out,p,n);return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
alignas(8) static volatile uintptr_t cells[4]={};
int main() {
    assert(!wanted&&!helper&&!handler);
    assert(setSaveLobbyWatch(true));
    for(unsigned i=0;i<4;++i) targets[i]=(uintptr_t)&cells[i];
    InterlockedExchange(&captured,2);SetEvent(wake);
    for(unsigned i=0;i<100&&!covered;++i) Sleep(20);
    assert(covered>0 && refused==0);
    for(unsigned i=0;i<4;++i) cells[i]=0xB400+i;
    for(unsigned i=0;i<100&&issued<4;++i) Sleep(20);
    assert(issued==4);
    for(unsigned i=0;i<4;++i) {
        assert(events[i].ready&&events[i].tid==GetCurrentThreadId());
        assert(events[i].hits==(1ULL<<i)&&events[i].value[i]==0xB400+i);
        assert(events[i].rip&&events[i].frames>=1);
    }
    assert(setSaveLobbyWatch(false));assert(!wanted&&!helper&&!handler&&!cleanupFailed);
    const long before=issued;cells[0]=0xC000;Sleep(50);assert(issued==before);
    puts("ok default-off, real 8-byte WRITE hardware traps on four cells, writer RIP/stack, disarm/restoration");
    // A debugger-owned register is never overwritten, nor an unrelated step consumed.
    CONTEXT c={}; c.Dr0=1;c.Dr7=1;
    assert(!ownsContext(c));
    EXCEPTION_RECORD er={};er.ExceptionCode=EXCEPTION_SINGLE_STEP;
    EXCEPTION_POINTERS ep={&er,&c};assert(writerTrap(&ep)==EXCEPTION_CONTINUE_SEARCH);
    puts("ok foreign debug context and unrelated single-step pass through");
}
