#ifndef TW3K_RELEASE
// ui.cpp - disarmed debug controls, drained at the engine input phase in frontend and campaign.
// ★ WH_GETMESSAGE only installs the signature-checked input hook; it never executes UI requests.
// Click/key messages go through the game WndProc into its own queue, then its normal dispatch.
// No cursor/focus manipulation, direct widget input calls or guessed screen coordinates.
#include "tw3k.h"
#include "ui.h"
#include "ui_protocol.h"
#include "ui_input.h"
#include <cmath>

namespace {
HWND window=nullptr;
DWORD ownerThread=0;
constexpr uintptr_t CStr = 0x668CD0, StringLength = 0x674190, TextToNarrow = 0x674E40, TextToWide = 0x674FA0;
constexpr uintptr_t AssignString = 0x664040, FreeString = 0x670570;
constexpr uintptr_t NarrowEmpty = 0x3C5B3EE, WideEmpty = 0x3C5B3F0;
constexpr uintptr_t GetEdit = 0x51D180, SetText = 0x5ACC90, SetEditText = 0x5A66B0;
constexpr uintptr_t SetFlag = 0x5B6EA0, Width = 0x5BA710, Height = 0x588400;
constexpr uintptr_t PrepareClick = 0x57B5D0, MouseDown = 0x590880, MouseUp = 0x590D20;
constexpr uintptr_t InputDrain = 0x2F7C80, GameWndProc = 0x2F9260;
uintptr_t inputApp=0; // Only used on the verified HWND owner thread, inside inputPhaseHook.
constexpr size_t Parent = 0x118, Id = 0xF0, State = 0x170;
struct EngineString { uintptr_t opaque, ptr; };
bool executable(uintptr_t p) {
    if (p < g_base || p >= g_base + 0x4836000) return false;
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery((void*)p, &m, sizeof(m)) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    return (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
bool callable(uintptr_t rva) { return executable(g_base + rva); }
bool widget(uintptr_t w) {
    uintptr_t vt = 0, fn = 0, handle = 0; uint32_t count = 0; EngineString name{};
    return w > 0x10000 && !(w & 7) && readAt(w, vt) && vt >= g_base && vt < g_base + 0x4836000 &&
        readAt(vt, fn) && executable(fn) && readAt(w + Id, handle) && handle>0x10000 && readAt(handle,name) &&
        readAt(w + OFF_WIDGET_CHILD_COUNT, count) && count <= twui::MaxNodes;
}
// Keep engine calls in tiny SEH functions: no C++ objects requiring unwinding in __try bodies.
bool stringBytes(uintptr_t object, char* out, size_t size) {
    EngineString s{};
    if (!readAt(object, s) || !callable(CStr) || !callable(StringLength)) return false;
    const char* p = nullptr;
    size_t length = 0;
    __try {
        length = ((uint32_t (*)(void*))(g_base + StringLength))((void*)object);
        p = ((const char* (*)(void*))(g_base + CStr))((void*)object);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!p || length >= size || !safeRead(p,out,length)) return false;
    out[length]='\0';
    return memchr(out,'\0',length)==nullptr; // Never accept embedded NULs or truncated Ids/text.
}
bool handleBytes(uintptr_t offset,char* out,size_t size) {
    uintptr_t handle=0;
    return readAt(offset,handle) && handle>0x10000 && stringBytes(handle,out,size);
}
bool releaseString(EngineString& s, uintptr_t empty) {
    if (!s.ptr || s.ptr == g_base + empty || (s.ptr & 0xF000000000000000ULL) == 0x8000000000000000ULL) return true;
    if (!callable(FreeString)) return false;
    __try { ((void (*)(void*))(g_base + FreeString))((void*)s.ptr); s.ptr = g_base + empty; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool wideStringBytes(uintptr_t object, char* out, size_t size) {
    EngineString source{}, result{0, g_base + NarrowEmpty};
    if (!readAt(object, source) || !callable(TextToNarrow)) return false;
    bool converted = false;
    __try { ((void (*)(void*,void*))(g_base + TextToNarrow))(&result, (void*)object); converted = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    const bool ok = converted && stringBytes((uintptr_t)&result, out, size);
    return releaseString(result, NarrowEmpty) && ok;
}
bool stateText(uintptr_t state, char* out, size_t size) { return wideStringBytes(state+0x70,out,size); }
// Native geometry is in client pixels (FUN_1406A8400 uses ScreenToClient).
// Hit testing and capture remain entirely the engine's responsibility.
bool widgetPoint(uintptr_t w, LPARAM& point) {
    float x=0, y=0, width=0, height=0; RECT client{};
    if (!callable(Width) || !callable(Height) || !readAt(w+0x158,x) || !readAt(w+0x15C,y) ||
        !GetClientRect(window,&client)) return false;
    __try {
        width=((float (*)(void*))(g_base+Width))((void*)w);
        height=((float (*)(void*))(g_base+Height))((void*)w);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return twui::clickPoint(x,y,width,height,client,point);
}
class NativeWindowInput final : public twui::WindowInput {
public:
    bool send(UINT message, WPARAM key, LPARAM detail, unsigned type, unsigned event) override {
        DWORD pid=0; uint32_t before=0, after=0, recordType=0, recordEvent=0; uintptr_t array=0;
        if (!inputApp || GetCurrentThreadId()!=ownerThread || !IsWindow(window) ||
            GetWindowThreadProcessId(window,&pid)!=ownerThread || pid!=GetCurrentProcessId() ||
            (uintptr_t)GetWindowLongPtrW(window,GWLP_WNDPROC)!=g_base+GameWndProc ||
            !readAt(inputApp+0x9C,before) || before>10000) return false;
        // 1.7.2 rejects nonzero mouse extra-info. Restore it even when delivery faults.
        const LPARAM previous=SetMessageExtraInfo(0);
        bool delivered=false;
        __try { SendMessageW(window,message,key,detail); delivered=true; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        SetMessageExtraInfo(previous);
        // WndProc's return value is NOT acceptance. Witness its exact 0xC0-byte record.
        return delivered && readAt(inputApp+0x9C,after) && after==before+1 &&
            readAt(inputApp+0xA0,array) && array>0x10000 &&
            readAt(array+uintptr_t(before)*0xC0,recordType) && recordType==type &&
            readAt(array+uintptr_t(before)*0xC0+(type==1?0x60:4),recordEvent) && recordEvent==event;
    }
};
// 2026-10-07 live: S19's window-message click is accepted by the engine but does not reach the
// widget (test-host main menu, 1920x1200). The proven direct widget click runs again by default; it now
// executes inside the engine input phase, which is where real input reaches widgets.
bool g_uiClickViaWindow = false;
// The +0x2CA flag is restored by the caller, and only when the widget is still alive: a click
// that destroys its own panel must not restore through the old pointer. The Reforms-close
// RIP=0x61 crash recurred after this guard; the old restore alone does not explain that crash.
bool simulateClick(uintptr_t w, bool& accepted, uint8_t& flag, bool& changed) {
    float x=0, y=0; flag = 0; changed = false;
    if (!readAt(w+0x2CA,flag)) return false;
    for (auto r : {SetFlag, Width, Height, PrepareClick, MouseDown, MouseUp}) if (!callable(r)) return false;
    bool ok = false;
    __try {
        changed = true;
        ((void (*)(void*,uint8_t))(g_base+SetFlag))((void*)w,0);
        float width = ((float (*)(void*))(g_base+Width))((void*)w);
        float height = ((float (*)(void*))(g_base+Height))((void*)w);
        float point[2]{};
        if (readAt(w+0x158,x) && readAt(w+0x15C,y) && std::isfinite(x) && std::isfinite(y) &&
            std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0) {
            point[0]=x+width*0.5f; point[1]=y+height*0.5f;
            ((void (*)(void*,int))(g_base+PrepareClick))((void*)w,1);
            accepted = ((bool (*)(void*,float*,int,int))(g_base+MouseDown))((void*)w,point,0,0) &&
                ((bool (*)(void*,float*,int))(g_base+MouseUp))((void*)w,point,0);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok;
}
bool restoreClickFlag(uintptr_t w, uint8_t flag) {
    if (!callable(SetFlag)) return false;
    __try { ((void (*)(void*,uint8_t))(g_base+SetFlag))((void*)w,flag); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool setStateText(uintptr_t w, const char* text) {
    for (auto r : {AssignString, TextToWide, FreeString, GetEdit, SetText, SetEditText}) if (!callable(r)) return false;
    EngineString narrow{0,g_base+NarrowEmpty}, wide{0,g_base+WideEmpty};
    bool ok = false;
    __try {
        ((void (*)(void*,const char*))(g_base+AssignString))(&narrow,text);
        ((void (*)(void*,void*))(g_base+TextToWide))(&wide,&narrow);
        void* edit = ((void* (*)(void*))(g_base+GetEdit))((void*)w);
        if (!edit) ((void (*)(void*,void*,int))(g_base+SetText))((void*)w,&wide,0);
        else {
            uintptr_t vt=0,fn=0;
            if (readAt((uintptr_t)edit,vt) && readAt(vt,fn) && executable(fn)) {
                ((void (*)(void*,void*))(g_base+SetEditText))(edit,&wide);
                ok = true;
            }
        }
        if (!edit) ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    const bool freedWide = releaseString(wide,WideEmpty);
    const bool freedNarrow = releaseString(narrow,NarrowEmpty);
    return ok && freedWide && freedNarrow;
}
class NativeBackend final : public twui::Backend {
public:
    std::string sources;
    bool quit(std::string& error) override {
        DWORD pid=0;
        if (GetCurrentThreadId()!=ownerThread || !IsWindow(window) ||
            GetWindowThreadProcessId(window,&pid)!=ownerThread || pid!=GetCurrentProcessId()) {
            error="quit-window-owner-mismatch"; return false;
        }
        if (!PostMessageA(window,WM_CLOSE,0,0)) { error="quit-post-close-failed"; return false; }
        return true;
    }
    bool node(twui::Widget w, twui::Node& n) override {
        if (!widget(w)) return false;
        char id[512]{}, state[512]{}, text[4096]{}; uintptr_t s=0;
        uint8_t own=0,effective=0,disabled=1;
        if (!handleBytes(w+Id,id,sizeof(id)) || !readAt(w+State,s) || s<=0x10000 ||
            !handleBytes(s+0x38,state,sizeof(state)) || !stateText(s,text,sizeof(text)) ||
            !readAt(s+0x26D,disabled) || disabled>1) return false;
        std::unordered_set<uintptr_t> seen;
        bool visible = true;
        for (uintptr_t p=w; p;) {
            if (seen.size() >= 64 || !seen.insert(p).second || !widget(p) ||
                !readAt(p+0x2C4,own) || !readAt(p+0x2C5,effective) || own>1 || effective>1) return false;
            visible = visible && own && effective;
            if (!readAt(p+Parent,p)) return false;
        }
        n = {id,state,text,visible,disabled!=0}; return true;
    }
    bool children(twui::Widget w, std::vector<twui::Widget>& out) override {
        uint32_t count=0; uintptr_t array=0;
        if (!widget(w) || !readAt(w+OFF_WIDGET_CHILD_COUNT,count) || count>twui::MaxNodes ||
            !readAt(w+OFF_WIDGET_CHILD_ARRAY,array) || (count && array<=0x10000)) return false;
        for (uint32_t i=0;i<count;++i) {
            uintptr_t child=0,parent=0;
            if (!readAt(array+i*sizeof(uintptr_t),child) || !widget(child) ||
                !readAt(child+Parent,parent) || parent!=w) return false;
            out.push_back(child);
        }
        return true;
    }
    bool root(twui::Widget& root, std::string& error) override {
        std::vector<twui::Widget> found;
        root=0;
        if (!roots(found,error)) return false;
        if (found.size()!=1) { error="multiple-roots-use-ui-roots"; return false; }
        root=found.front(); return true;
    }
    bool roots(std::vector<twui::Widget>& out, std::string& error) override {
        out.clear(); sources.clear();
        if (!sourceList(0x3CDCDE0,0x3CDCDDC,"topmost",out,error) ||
            !sourceList(0x3CDCE00,0x3CDCDFC,"mouse-listener",out,error)) return false;
        uintptr_t lobby=g_liveLobby, w=0;
        if (lobby && lobbyLooksLive(lobby)) {
            if (!readAt(lobby+0x08,w) || !source(w,"lobby+0x08",out,error)) return false;
        }
        if (out.empty()) { error="no-live-widget-root-source"; return false; }
        // Snapshot root order is first occurrence in topmost, listener, then lobby sources.
        // Validate reciprocal parent/child links rather than accepting a detached stale chain.
        for (auto r : out) {
            std::vector<twui::Widget> children;
            if (!this->children(r,children)) { error="unreadable-root-children"; return false; }
            std::unordered_set<twui::Widget> seen;
            for (auto child : children) {
                uintptr_t parent=0;
                if (!seen.insert(child).second || !readAt(child+Parent,parent) || parent!=r) {
                    error="invalid-root-child-parent"; return false;
                }
            }
        }
        return true;
    }
    bool click(twui::Widget w,std::string& error) override {
        if (g_uiClickViaWindow) {
            LPARAM point=0;
            if (!widgetPoint(w,point)) { error="click-invalid-client-geometry"; return false; }
            NativeWindowInput input;
            return twui::windowClick(input,point,error);
        }
        bool accepted=false, changed=false; uint8_t flag=0;
        std::vector<twui::Widget> chain;   // w, parent, ..., root — read while w is certainly alive
        for (uintptr_t p=w; p && chain.size()<64; ) { chain.push_back(p); if (!readAt(p+Parent,p)) { chain.clear(); break; } }
        const bool clicked=simulateClick(w,accepted,flag,changed);
        // Restore even when down/up faulted (never retry), but never into a widget the click
        // destroyed or detached. Flag 0 needs no restore at all.
        bool restored=true;
        if (changed && flag) restored = stillAttached(chain) ? restoreClickFlag(w,flag) : true;
        if (!clicked) { error="click-fault-or-invalid-geometry-effect-uncertain-never-retry"; return false; }
        if (!restored) { error="click-flag-restore-fault-effect-uncertain-never-retry"; return false; }
        if (!accepted) error="engine-rejected-click-effect-uncertain-never-retry";
        return accepted;
    }
    bool key(const std::string& key,std::string& error) override {
        NativeWindowInput input;
        return twui::windowKey(input,key,error);
    }
    bool text(twui::Widget w,const std::string& text,std::string& error) override {
        if (!setStateText(w,text.c_str())) { error="text-call-fault-effect-uncertain-never-retry"; return false; }
        return true;
    }
    std::string players() override {
        uintptr_t lobby=g_liveLobby, array=0; uint32_t count=0;
        if (!lobby || !lobbyLooksLive(lobby) || !readAt(lobby+OFF_PLAYER_COUNT,count) ||
            !count || count>64 || !readAt(lobby+OFF_PLAYER_RECORDS,array) || array<=0x10000) return twui::fail("present-players-unavailable");
        std::unordered_set<uint32_t> seen; std::string reply;
        // Enumerate actual records. The array index is an iteration cursor, never a player/seat id.
        for (uint32_t i=0;i<count;++i) {
            uint32_t id=0;
            if (!readAt(array+(uintptr_t)i*0x48+0x10,id) || !seen.insert(id).second) return twui::fail("invalid-or-duplicate-player-id");
            // +0x00 is a UTF-16 CA string (panels.cpp), not a cached slot name.
            char text[4096]{};
            const bool readable = wideStringBytes(array+(uintptr_t)i*0x48,text,sizeof(text)) && text[0];
            // Expanded records may have no usable CA name; the lobby already captured it by id.
            const char* captured = id <= INT_MAX ? knownPlayerName((int)id) : "";
            reply += twui::playerRow(id, readable ? text : "", captured ? captured : "");
            if (reply.size() + 100 > twui::MaxReply) return twui::fail("reply-limit");
        }
        return reply+"ok\n";
    }
private:
    // Alive = the pre-click ancestor chain still holds, checked top-down through LIVE parents' child
    // arrays only (a freed widget's own memory can still look valid, so it is never the witness).
    bool stillAttached(const std::vector<twui::Widget>& chain) {
        if (chain.empty()) return false;
        std::vector<twui::Widget> live; std::string ignored;
        if (!roots(live,ignored)) return false;
        bool rooted=false;
        for (auto r : live) if (r==chain.back()) rooted=true;
        if (!rooted) return false;
        for (size_t i=chain.size()-1; i>0; --i) {
            uint32_t count=0; uintptr_t array=0; bool found=false;
            if (!widget(chain[i]) || !readAt(chain[i]+OFF_WIDGET_CHILD_COUNT,count) || count>twui::MaxNodes ||
                !readAt(chain[i]+OFF_WIDGET_CHILD_ARRAY,array) || (count && array<=0x10000)) return false;
            for (uint32_t k=0; k<count && !found; ++k) { uintptr_t c=0; if (readAt(array+k*sizeof(uintptr_t),c) && c==chain[i-1]) found=true; }
            if (!found) return false;
        }
        return true;
    }
    bool source(uintptr_t w,const char* label,std::vector<twui::Widget>& roots,std::string& error) {
        std::unordered_set<uintptr_t> seen;
        while (w) {
            if (seen.size()>=64 || !seen.insert(w).second || !widget(w)) { error="invalid-root-source-"+std::string(label); return false; }
            uintptr_t parent=0;
            if (!readAt(w+Parent,parent)) { error="unreadable-parent"; return false; }
            if (!parent) break;
            std::vector<twui::Widget> siblings;
            if (!children(parent,siblings)) { error="unreadable-source-parent-children"; return false; }
            size_t matches=0;
            for (auto child : siblings) if (child==w) ++matches;
            if (matches!=1) { error="invalid-source-parent-link"; return false; }
            w=parent;
        }
        if (!w) { error="null-root-source"; return false; }
        bool known=false;
        for (auto root : roots) if (root==w) known=true;
        if (!known) {
            if (roots.size()>=twui::MaxNodes) { error="root-limit"; return false; }
            roots.push_back(w);
        }
        char name[512]{};
        if (!handleBytes(w+Id,name,sizeof(name))) { error="root-id-unreadable"; return false; }
        // Avoid quadratic diagnostic growth when many listeners share one root.
        if (sources.size()<4096) sources += std::string(label)+"="+twui::escaped(name)+";";
        return true;
    }
    bool sourceList(uintptr_t arrayRva,uintptr_t countRva,const char* label,std::vector<twui::Widget>& roots,std::string& error) {
        uint32_t count=0; uintptr_t array=0;
        if (!readAt(g_base+countRva,count) || count>twui::MaxNodes || !readAt(g_base+arrayRva,array)) { error="root-list-unreadable-"+std::string(label); return false; }
        if (!count) return true;
        if (array<=0x10000) { error="root-list-null"; return false; }
        for (uint32_t i=0;i<count;++i) {
            uintptr_t w=0;
            if (!readAt(array+i*sizeof(uintptr_t),w) || !source(w,label,roots,error)) return false;
        }
        return true;
    }
};

SRWLOCK queueLock = SRWLOCK_INIT;
enum QueueState { Idle, Pending, Running, Done, Abandoned };
QueueState queueState=Idle;
twui::Request queued;
std::string queuedReply;
bool armed=false, stopping=false;
HHOOK hook=nullptr;
DWORD drainedThread=0;
UINT wakeMessage=0;
Detour inputDetour;
using DrainFn=void (*)(void*,void*,uint8_t);
DrainFn originalDrain=nullptr;
thread_local unsigned inputDepth=0;
void inputPhaseHook(void* app,void* time,uint8_t dispatch) {
    ++inputDepth;
    bool running=false;
    twui::Request request;
    AcquireSRWLockExclusive(&queueLock);
    if (inputDepth==1 && dispatch && queueState==Pending && !stopping && GetCurrentThreadId()==ownerThread) {
        queueState=Running; request=queued; running=true;
    }
    ReleaseSRWLockExclusive(&queueLock);
    std::string result;
    if (running) {
        drainedThread=GetCurrentThreadId(); inputApp=(uintptr_t)app;
        NativeBackend backend;
        try {
            result=twui::execute(request,backend,armed,uiArmingFlagExists());
            if (request.verb=="status") {
                uintptr_t root=0; std::string error; std::vector<twui::Widget> roots;
                bool found=armed && backend.roots(roots,error);
                if (found) root=roots.front();
                char status[256]{};
                _snprintf_s(status,sizeof(status),_TRUNCATE,"status\tpid=%lu\thwnd-thread=%lu\tdrain-thread=%lu\towner-match=%u\troot=%016llX\tinput-path=engine-queue\t",
                    GetCurrentProcessId(),ownerThread,drainedThread,ownerThread==drainedThread,(unsigned long long)root);
                result=std::string(status)+(found?"roots="+std::to_string(roots.size())+";"+backend.sources:(armed?error:"disarmed"))+"\n"+result;
            }
        } catch (...) { armed=false; result=twui::fail("ui-exception-disarmed-effect-uncertain-never-retry"); }
        inputApp=0;
    }
    // ★ Original snapshots app+0x98, clears pending count, and invokes real input listeners.
    // Publish completion AFTER dispatch. Do not catch/suppress engine faults here.
    originalDrain(app,time,dispatch);
    if (running) {
        AcquireSRWLockExclusive(&queueLock);
        const bool abandoned=queueState==Abandoned || stopping;
        if (abandoned) { armed=false; queueState=Idle; }
        else { queuedReply=result; queueState=Done; }
        ReleaseSRWLockExclusive(&queueLock);
        if (request.verb=="arm" || request.verb=="disarm" || request.verb=="quit" || request.verb=="key" || request.verb=="click")
            logf("UI TEST: input-phase %s pid=%lu thread=%lu abandoned=%u result=%s",
                request.verb.c_str(),GetCurrentProcessId(),drainedThread,abandoned,result.c_str());
    }
    --inputDepth;
}
bool installInputPhase() {
    // Whole 1.7.2 instructions with no RIP-relative operands. Install on the owner thread
    // in its message pump, before that same thread can enter this frame's input drain.
    static const uint8_t expected[]={0x48,0x8B,0xC4,0x44,0x88,0x40,0x18,0x53,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xEC,0x98,0,0,0};
    return detourInstall(inputDetour,g_base+InputDrain,sizeof(expected),expected,
        (uintptr_t)&inputPhaseHook,(void**)&originalDrain,"UI TEST engine input phase");
}
void consumeWake(LPARAM param) {
    __try { ((MSG*)param)->message=WM_NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
LRESULT CALLBACK uiMessageHook(int code,WPARAM remove,LPARAM param) {
    if (code>=0 && remove==PM_REMOVE) {
        MSG message{};
        if (safeRead((void*)param,&message,sizeof(message)) && message.hwnd==window && message.message==wakeMessage) {
            AcquireSRWLockExclusive(&queueLock);
            if (queueState==Pending && !stopping && GetCurrentThreadId()==ownerThread &&
                !inputDetour.active && !installInputPhase()) {
                queuedReply=twui::fail("engine-input-phase-signature-refused"); queueState=Done;
            }
            ReleaseSRWLockExclusive(&queueLock);
            consumeWake(param);
        }
    }
    return CallNextHookEx(hook,code,remove,param);
}
struct Windows { HWND hwnd=nullptr; DWORD thread=0; unsigned count=0; };
BOOL CALLBACK enumerate(HWND hwnd,LPARAM param) {
    DWORD pid=0,thread=GetWindowThreadProcessId(hwnd,&pid);
    if (pid==GetCurrentProcessId() && IsWindowVisible(hwnd) && !GetWindow(hwnd,GW_OWNER)) {
        auto& found=*(Windows*)param; ++found.count; found.hwnd=hwnd; found.thread=thread;
    }
    return TRUE;
}
bool ensureHook(std::string& error) {
    if (hook) {
        DWORD pid=0;
        if (!IsWindow(window) || GetWindowThreadProcessId(window,&pid)!=ownerThread || pid!=GetCurrentProcessId()) { error="game-window-changed-restart-required"; return false; }
        return true;
    }
    Windows found;
    if (!EnumWindows(enumerate,(LPARAM)&found) || found.count!=1 || !found.thread) { error="game-window-absent-or-ambiguous"; return false; }
    window=found.hwnd; ownerThread=found.thread;
    wakeMessage=RegisterWindowMessageA("tw3k_coop.ui.test.queue.v1");
    if (!wakeMessage) { error="register-ui-message-failed"; return false; }
    hook=SetWindowsHookExA(WH_GETMESSAGE,uiMessageHook,g_selfModule,ownerThread);
    if (!hook) { error="install-ui-message-hook-failed"; return false; }
    logf("UI TEST: queue installed pid=%lu HWND=%p owner-thread=%lu (requests drain at engine input phase)",GetCurrentProcessId(),window,ownerThread);
    return true;
}
} // namespace

void executeUiControl(const char* command,char* reply,size_t size) {
    twui::Request request; std::string error;
    if (!twui::parse(command,request,error)) { strcpy_s(reply,size,twui::fail(error).c_str()); return; }
    AcquireSRWLockShared(&queueLock); const bool stopped=stopping; ReleaseSRWLockShared(&queueLock);
    if (stopped) { strcpy_s(reply,size,"fail ui-control-stopped\n"); return; }
    if (!ensureHook(error)) { strcpy_s(reply,size,twui::fail(error).c_str()); return; }
    AcquireSRWLockExclusive(&queueLock);
    if (stopping) { ReleaseSRWLockExclusive(&queueLock); strcpy_s(reply,size,"fail ui-control-stopped\n"); return; }
    if (queueState!=Idle) { ReleaseSRWLockExclusive(&queueLock); strcpy_s(reply,size,"fail ui-request-still-in-flight-never-retry\n"); return; }
    queued=request; queuedReply.clear(); queueState=Pending;
    // The phase hook can run without another wake once installed. Keep the lock through
    // posting so post failure still proves no dispatch, rather than cancelling Running.
    if (!PostMessageA(window,wakeMessage,0,0)) {
        queueState=Idle; ReleaseSRWLockExclusive(&queueLock);
        strcpy_s(reply,size,"fail post-ui-message-failed\n"); return;
    }
    ReleaseSRWLockExclusive(&queueLock);
    ULONGLONG deadline=GetTickCount64()+5000;
    while (GetTickCount64()<deadline) {
        AcquireSRWLockExclusive(&queueLock);
        if (stopping) {
            strcpy_s(reply,size,"fail ui-control-stopped-effect-uncertain-never-retry\n");
            ReleaseSRWLockExclusive(&queueLock); return;
        }
        if (queueState==Done) {
            if (queuedReply.size()>=size) strcpy_s(reply,size,"fail reply-limit\n");
            else strcpy_s(reply,size,queuedReply.c_str());
            queueState=Idle; ReleaseSRWLockExclusive(&queueLock); return;
        }
        ReleaseSRWLockExclusive(&queueLock); Sleep(5);
    }
    AcquireSRWLockExclusive(&queueLock);
    // Pending is cancelled under the same lock the engine input hook needs to mark Running.
    // Therefore this exact reply proves the request never reached execute().
    if (queueState==Pending) { queueState=Idle; strcpy_s(reply,size,"fail ui-thread-timeout-before-dispatch\n"); }
    else if (queueState==Done) { strcpy_s(reply,size,queuedReply.size()<size?queuedReply.c_str():"fail reply-limit\n"); queueState=Idle; }
    else { queueState=Abandoned; strcpy_s(reply,size,"fail ui-thread-timeout-effect-uncertain-never-retry\n"); }
    ReleaseSRWLockExclusive(&queueLock);
}
void stopUiControl() {
    AcquireSRWLockExclusive(&queueLock); stopping=true;
    if (queueState==Pending) queueState=Idle;
    ReleaseSRWLockExclusive(&queueLock);
    detourRemove(inputDetour,"UI TEST engine input phase");
    if (hook && UnhookWindowsHookEx(hook)) hook=nullptr;
    // main.cpp retains the DLL until process exit; even a failed unhook cannot point into freed code.
    logf("UI TEST: input queue stopped; message-hook=%s engine-hook=%s; DLL retained until exit",
        hook?"removal-failed":"removed",inputDetour.active?"removal-failed":"removed");
}

#endif // developer facilities
