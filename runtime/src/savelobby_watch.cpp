#ifndef TW3K_RELEASE
// savelobby_watch.cpp - B4-S5: find the writer, without changing widget lifetimes.
// ★ Default OFF. Four x64 hardware WRITE breakpoints, not page protection changes.
// A trap is AFTER the instruction; save RIP, the preceding code bytes and an
// unwound stack. The helper logs after returning from VEH, never under an engine
// allocator lock. Arm in the host-only save lobby BEFORE the other seats join.
#include "tw3k.h"
#include <tlhelp32.h>

namespace {
constexpr long MaxEvents = 128;
constexpr DWORD64 WatchDr7 = 0x99990055ULL; // local enable + write/8-byte on DR0..3
constexpr DWORD64 DebugMask = 0xFFFF00FFULL;
struct WriteEvent {
    volatile long ready = 0;
    DWORD tid = 0;
    uintptr_t rip = 0, address[4] = {}, value[4] = {}, stack[16] = {};
    uint8_t code[32] = {};
    DWORD64 hits = 0;
    unsigned frames = 0;
};
WriteEvent events[MaxEvents];
volatile long wanted = 0, captured = 0, issued = 0, saturated = 0;
volatile long covered = 0, refused = 0, cleanupFailed = 0;
uintptr_t targets[4] = {}, watchedLobby = 0;
HANDLE wake = nullptr, helper = nullptr;
PVOID handler = nullptr;

bool ownsContext(const CONTEXT& c) {
    return captured == 2 && c.Dr0 == targets[0] && c.Dr1 == targets[1] &&
        c.Dr2 == targets[2] && c.Dr3 == targets[3] && (c.Dr7 & DebugMask) == WatchDr7;
}
void configure(CONTEXT& c, bool on) {
    c.Dr0 = on ? targets[0] : 0; c.Dr1 = on ? targets[1] : 0;
    c.Dr2 = on ? targets[2] : 0; c.Dr3 = on ? targets[3] : 0;
    c.Dr7 = (c.Dr7 & ~DebugMask) | (on ? WatchDr7 : 0); c.Dr6 &= ~0xFULL;
}
LONG CALLBACK writerTrap(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord ||
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP ||
        !ownsContext(*ep->ContextRecord)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& c = *ep->ContextRecord;
    const auto hits = c.Dr6 & 0xFULL;
    if (!hits || (c.Dr6 & 0x6000)) return EXCEPTION_CONTINUE_SEARCH; // debugger single-step/BD
    const long index = InterlockedIncrement(&issued) - 1;
    if (index < MaxEvents) {
        WriteEvent& e = events[index];
        e.tid = GetCurrentThreadId(); e.rip = c.Rip; e.hits = hits;
        for (unsigned i = 0; i < 4; ++i) {
            e.address[i] = targets[i];
            safeRead((void*)targets[i], &e.value[i], sizeof(uintptr_t));
        }
        safeRead((void*)(c.Rip - 16), e.code, sizeof(e.code));
        CONTEXT frame = c;
        __try {
            for (unsigned i = 0; i < 16 && frame.Rip; ++i) {
                e.stack[e.frames++] = frame.Rip;
                DWORD64 image = 0;
                auto fn = RtlLookupFunctionEntry(frame.Rip, &image, nullptr);
                const auto oldSp = frame.Rsp;
                if (fn) {
                    PVOID data = nullptr; DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, frame.Rip, fn, &frame,
                                     &data, &establisher, nullptr);
                } else {
                    if (!safeRead((void*)frame.Rsp, &frame.Rip, sizeof(frame.Rip))) break;
                    frame.Rsp += 8;
                }
                if (frame.Rsp <= oldSp || frame.Rsp - c.Rsp > 0x10000) break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        InterlockedExchange(&e.ready, 1);
    } else {
        InterlockedExchange(&saturated, 1);
    }
    c.Dr6 &= ~0xFULL;
    // Keep the addresses/encoding intact until the helper restores every thread.
    // A full buffer is bounded: later traps are consumed but never allocate/log.
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Only the helper calls this; it never tries to obtain its OWN running context.
// No logging, engine calls or locks between SuspendThread and ResumeThread.
void updateThreads(bool on) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { InterlockedIncrement(&refused); return; }
    THREADENTRY32 e = {}; e.dwSize = sizeof(e);
    long yes = 0, no = 0;
    if (Thread32First(snapshot, &e)) do {
        if (e.th32OwnerProcessID != GetCurrentProcessId() || e.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                                   FALSE, e.th32ThreadID);
        if (!thread) { ++no; continue; }
        if (SuspendThread(thread) == (DWORD)-1) { ++no; CloseHandle(thread); continue; }
        CONTEXT c = {}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        bool ok = GetThreadContext(thread, &c) != FALSE;
        const bool ours = ok && ownsContext(c);
        if (ok && on && !ours && (c.Dr7 & 0xFF)) ok = false; // leave debugger-owned DRs alone
        if (ok && ((on && !ours) || (!on && ours))) {
            configure(c, on); ok = SetThreadContext(thread, &c) != FALSE;
        }
        if (ResumeThread(thread) == (DWORD)-1) ok = false;
        CloseHandle(thread);
        if (ok) ++yes; else ++no;
    } while (Thread32Next(snapshot, &e));
    CloseHandle(snapshot);
    if (on) { InterlockedExchange(&covered, yes); InterlockedExchange(&refused, no); }
    else if (no) InterlockedExchange(&cleanupFailed, 1);
}

DWORD WINAPI watchThread(void*) {
    unsigned printed = 0;
    ULONGLONG next = 0;
    bool armed = false;
    while (wanted && !saturated) {
        if (captured == 2 && GetTickCount64() >= next) {
            updateThreads(true); next = GetTickCount64() + 1000;
            if (!armed) {
                armed = true;
                logf("B4 WATCH ARMED lobby=%016llX threads=%ld refused=%ld targets={%016llX %016llX %016llX %016llX}",
                    (unsigned long long)watchedLobby, covered, refused,
                    (unsigned long long)targets[0], (unsigned long long)targets[1],
                    (unsigned long long)targets[2], (unsigned long long)targets[3]);
            }
        }
        while (printed < MaxEvents && events[printed].ready) {
            const WriteEvent& e = events[printed++];
            logf("B4 WATCH WRITE #%u tid=%lu DR-mask=%llX RIP-after=%016llX exe-RVA=%llX",
                 printed, e.tid, e.hits, (unsigned long long)e.rip,
                 (unsigned long long)(e.rip >= g_base ? e.rip - g_base : e.rip));
            for (unsigned i = 0; i < 4; ++i)
                logf("B4 WATCH cell[%u]=%016llX value=%016llX", i,
                     (unsigned long long)e.address[i], (unsigned long long)e.value[i]);
            char code[65] = {};
            for (unsigned i = 0; i < 32; ++i) sprintf_s(code + i * 2, 65 - i * 2, "%02X", e.code[i]);
            logf("B4 WATCH bytes[RIP-16..RIP+15]=%s", code);
            for (unsigned i = 0; i < e.frames; ++i)
                logf("B4 WATCH frame[%u]=%016llX", i, (unsigned long long)e.stack[i]);
        }
        WaitForSingleObject(wake, 50);
    }
    if (captured == 2) updateThreads(false);
    logf("B4 WATCH stopped: writes=%ld saturated=%ld cleanup=%s; no widget/free was suppressed",
         issued, saturated, cleanupFailed ? "INCONCLUSIVE (restart required)" : "restored");
    return 0;
}
} // namespace

bool setSaveLobbyWatch(bool on) {
    if (!on) {
        InterlockedExchange(&wanted, 0);
        if (wake) SetEvent(wake);
        if (helper) {
            if (WaitForSingleObject(helper, 5000) != WAIT_OBJECT_0) {
                logf("B4 WATCH cleanup pending; restart required"); return false;
            }
            CloseHandle(helper); helper = nullptr;
        }
        // Keep the handler mapped when any thread's removal cannot be verified.
        if (handler && !cleanupFailed) {
            if (!RemoveVectoredExceptionHandler(handler)) return false;
            handler = nullptr;
        }
        if (wake) { CloseHandle(wake); wake = nullptr; }
        return cleanupFailed == 0;
    }
    if (wanted) return true;
    if (cleanupFailed || helper || handler || IsDebuggerPresent()) return false;
    memset(events, 0, sizeof(events)); memset(targets, 0, sizeof(targets));
    captured = issued = saturated = covered = refused = 0; watchedLobby = 0;
    wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!wake) return false;
    handler = AddVectoredExceptionHandler(1, writerTrap);
    if (!handler) { CloseHandle(wake); wake = nullptr; return false; }
    InterlockedExchange(&wanted, 1);
    helper = CreateThread(nullptr, 0, watchThread, nullptr, 0, nullptr);
    if (!helper) { setSaveLobbyWatch(false); return false; }
    logf("B4 WATCH requested; waiting for a valid loaded_map on the lobby game thread");
    return true;
}

void captureSaveLobbyWatch(uintptr_t lobby) {
    if (!wanted || captured || !lobbyLooksLive(lobby)) return;
    uintptr_t widget = 0, handle = 0, array = 0, entry = 0, vt = 0;
    uint32_t count = 0;
    char name[32] = {};
    if (!readAt(lobby + 0x110, widget) || !widget || !readAt(widget + 0xF0, handle) ||
        !readCaString((void*)handle, name, sizeof(name)) || strcmp(name, "loaded_map") != 0 ||
        !readAt(widget + 0x144, count) || count != 1 ||
        !readAt(widget + 0x148, array) || !array || !readAt(array, entry) || !entry ||
        !readAt(entry, vt) || vt != g_base + 0x38014B8) return;
    if ((lobby | array | entry) & 7) return;
    if (InterlockedCompareExchange(&captured, 1, 0) != 0) return;
    watchedLobby = lobby;
    targets[0] = lobby + 0x108; targets[1] = lobby + 0x110;
    targets[2] = array; targets[3] = entry;
    InterlockedExchange(&captured, 2);
    if (wake) SetEvent(wake);
}

void reportSaveLobbyWatch(char* reply, size_t size) {
    const size_t used = strnlen_s(reply, size);
    if (used >= size) return;
    _snprintf_s(reply + used, size - used, _TRUNCATE,
        "B4 writer watch: %s captured=%ld threads=%ld refused=%ld writes=%ld saturated=%ld cleanup=%s\n",
        wanted && !saturated ? "ARMED/requested" : "off", captured, covered, refused, issued, saturated,
        cleanupFailed ? "INCONCLUSIVE-restart-required" : "ok");
}

#endif // developer facilities
