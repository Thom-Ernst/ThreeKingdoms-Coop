#pragma once
#include <cstdint>
#include <cstddef>
#include <limits>

// No persistent evidence: the engine owns all handles, for this getter invocation only.
namespace eventread {
inline thread_local uintptr_t feed = 0;
struct Scope {
    uintptr_t previous;
    explicit Scope(uintptr_t self) : previous(feed) { feed = self; }
    ~Scope() { feed = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
struct Vector { uint32_t cap, count; uintptr_t data; };
static_assert(sizeof(Vector) == 16, "64-bit engine vector");
constexpr uint32_t maxAccepted = 4096;
template<class Reader, class T>
bool get(Reader& r, uintptr_t base, size_t offset, T& value) {
    if (base < 0x10000 || base > (std::numeric_limits<uintptr_t>::max)() - offset - sizeof(T))
        return false;
    return r(base + offset, &value, sizeof(value));
}
inline bool valid(const Vector& v, uint32_t bound, size_t stride) {
    return v.count <= v.cap && v.cap <= bound &&
        (!v.count || (v.data >= 0x10000 &&
         v.data <= (std::numeric_limits<uintptr_t>::max)() - size_t(v.count)*stride));
}
// Reader is injectable, but this exact helper is used by the production call-site hook.
// native must have been evaluated first. Unknown state always preserves that result.
template<class Reader>
bool containsRead(bool native, uintptr_t self, uintptr_t candidate, uintptr_t queried,
                  uintptr_t rootSlot, Reader& r) {
    if (native || !self) return native;
    uintptr_t root=0, obj=0, model=0, mgr=0, mine=0, collection=0, owner=0, faction=0;
    if (!get(r,rootSlot,0,root) || !get(r,root,0x2188,obj) ||
        !get(r,obj,0x78,model) || !get(r,model,0x3D30,mgr) ||
        !get(r,obj,0x1A8,mine) || !mine || queried != mine ||
        !get(r,self,0x90,collection) || !get(r,collection,0,owner) || owner != mgr ||
        !get(r,collection,0x18,faction) || faction != mine) return false;
    Vector registry{};
    if (!get(r,mgr,0x228,registry) || !valid(registry,4,8)) return false;
    int index=-1;
    for (uint32_t i=0; i<registry.count; ++i) {
        uintptr_t f=0;
        if (!get(r,registry.data,i*8,f) || !f) return false;
        if (f==mine) { if (index!=-1) return false; index=int(i); }
    }
    if (index!=2 && index!=3) return false;
    uint64_t identity=0;
    uint32_t generation=0;
    if (!get(r,candidate,0x88,identity) || !get(r,mgr,0x260,generation) ||
        uint32_t(identity)!=generation) return false;
    Vector accepted{};
    if (!get(r,self,0xA8,accepted) || !valid(accepted,maxAccepted,16)) return false;
    bool found=false;
    for (uint32_t i=0; i<accepted.count; ++i) {
        uintptr_t msg=0, control=0;
        uint64_t prior=0;
        if (!get(r,accepted.data,i*16,msg) || !get(r,accepted.data,i*16+8,control) ||
            control < 0x10000 || control > (std::numeric_limits<uintptr_t>::max)()-0x10 ||
            msg != control+0x10 || !get(r,msg,0x88,prior)) return false;
        if (msg==candidate || prior!=identity) continue;
        Vector read{};
        if (!get(r,msg,0x90,read) || !valid(read,64,8)) return false;
        for (uint32_t j=0; j<read.count; ++j) {
            uintptr_t f=0;
            if (!get(r,read.data,j*8,f) || !f) return false;
            if (f==mine) found=true;
        }
    }
    // Fail open if an owner/generation changed while evidence was read.
    uintptr_t now=0; uint32_t genNow=0; Vector end{};
    if (!get(r,rootSlot,0,now) || now!=root || !get(r,root,0x2188,now) || now!=obj ||
        !get(r,obj,0x78,now) || now!=model || !get(r,obj,0x1A8,now) || now!=mine ||
        !get(r,model,0x3D30,now) || now!=mgr || !get(r,mgr,0x260,genNow) || genNow!=generation ||
        !get(r,self,0x90,now) || now!=collection || !get(r,collection,0,now) || now!=mgr ||
        !get(r,collection,0x18,now) || now!=mine || !get(r,self,0xA8,end) ||
        end.cap!=accepted.cap || end.count!=accepted.count || end.data!=accepted.data) return false;
    return found;
}
}
