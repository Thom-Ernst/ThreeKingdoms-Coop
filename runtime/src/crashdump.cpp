#ifndef TW3K_RELEASE
// crashdump.cpp — the dump the witness could never take: a minidump written at the instant of death.
//
// ============================================================================================
// WHY THIS EXISTS
//
// `crashWitness` (main.cpp) already converts a fault into a LINE: the code, the faulting
// instruction, the module that owns it, the address touched, and the registers. That line has
// carried the project a long way — the 2026-08-14 `exe+0x2F5EBFC` diagnosis was read out of it.
//
// But a line is what somebody thought to print. A dump answers the questions nobody thought of,
// which is precisely the rule `tools\dumps\Capture-TwDump.ps1` was written to serve:
//
//     "when a session is in a state worth reading, DUMP FIRST and ask questions afterwards"
//
// ⚠ And that script CANNOT serve it for a crash. procdump attaches to a process that is still
// running; a crash-to-desktop is gone in the time it takes to alt-tab. Every CTD this project has
// had was reconstructed from a log line and whatever Windows happened to keep. This closes that:
// the dump is written by us, from inside the dying process, before the process finishes dying.
//
// ============================================================================================
// THE HANDLERS, AND THE NARROW FIRST-CHANCE EXCEPTION
//
// A VEH sees FIRST-CHANCE exceptions — every fault, including the ones the game throws and handles
// as normal business. `crashWitness` is a VEH, which is right for a log line and WRONG for a dump:
// writing one costs hundreds of milliseconds. Ordinary handled faults remain log-only.
// S18: both execute AVs at RIP=0x61 reached the witness, but neither produced the UEF worker's
// FATAL banner. The logs cannot distinguish interception, displacement, termination, worker
// starvation or failed unwind on the unusual RSP. UEF-only capture cannot cover all of these. A VEH
// captures ONLY execute AVs below 64 KiB, once, before any unwind or handler can discard evidence.
// This is first-chance evidence, NOT a claim that the fault was unhandled. It returns SEARCH;
// a recovering game handler still gets the original exception. A debug seat may stall for this
// one diagnostic write, even if the game recovers. No general first-chance AV dumping.
//
//     ⚠⚠ IN LOCKSTEP MP THAT STALL IS A DESYNC RISK. Capture-TwDump.ps1's own header says
//     suspending a client for a long write "is very likely to desync the session or drop the peer".
//     Ordinary first-chance AVs remain log-only. S18 accepts one diagnostic stall for a low
//     execute AV to preserve evidence that the UEF-only recorder lost.
//
// The default dump path uses `SetUnhandledExceptionFilter`, which runs only when nothing in the
// process handled the exception — i.e. only when the game is already dead. At that point the peer
// is losing this client regardless, and a one-second write costs nothing that was not already lost.
//
//   crashWitness   (VEH, first-chance)  -> a LINE, always, costs microseconds     [unchanged]
//   crashFatal     (UEF, last-chance)   -> a DUMP, when the process is dying
//   crashLowExecute (VEH)              -> a DUMP, first low-address execute AV only
//
// ⇒ Read that as: the witness says what happened, this says what the process looked like when it
//   did. All return to exception dispatch; the low-execute path can delay recovery.
//
// ============================================================================================
// ⚠ THE RE-ARM, WHICH IS NOT OPTIONAL
//
// `SetUnhandledExceptionFilter` keeps exactly ONE filter per process. The proxy loads us off the
// exe's import table, so our DllMain runs BEFORE the game's own startup code — and the game (or
// Denuvo, or the CRT) installs its filter afterwards, displacing ours. Installing once at attach
// and walking away means the filter is silently gone by the time the main menu appears, which is
// the same class of failure as a stale DLL: everything looks armed and nothing is.
//
// So the probe loop re-arms every ~2 s. Each re-arm captures whoever displaced us and keeps them as
// the chain target, so the game's own handler still runs and still decides the process's fate — we
// look first, then hand over. `crash` reports the re-arm count, which is how you tell the
// difference between "installed" and "installed and still winning".
//
// ============================================================================================
// ⚠ STACK OVERFLOW, AND WHY THERE IS A THREAD
//
// On EXCEPTION_STACK_OVERFLOW the filter runs on the stack that just overflowed — there is not
// enough room left to call MiniDumpWriteDump, which needs a great deal of it. The standard answer,
// and the one used here: a dumper thread created AT ATTACH, with its own fresh stack, parked on an
// event. The filter signals it and waits. That also keeps the fatal path free of any allocation,
// any LoadLibrary and any loader-lock contact, all of which are unsafe in a process this far gone.
//
// dbghelp.dll is loaded and MiniDumpWriteDump resolved at ATTACH for the same reason. A crash
// handler that has to LoadLibrary before it can work is a crash handler that fails exactly when the
// loader is the thing that broke.
//
// ✗ AND THE FIX THAT WAS DELIBERATELY NOT TAKEN: `SetThreadStackGuarantee` (2026-08-17).
//
// It is the textbook answer to "my exception handler has no stack", and it was the first thing
// proposed when the live overflow test came back empty. It is the WRONG answer here, twice over:
//
//   1. ⚠⚠ It is PER-THREAD, and it can only be called from the thread it applies to. A real stack
//      overflow in this game would land on a GAME thread — a widget walk on a cyclic child list is
//      the obvious candidate, and `treewalk` exists because such lists occur (#12). We never run on
//      those threads at a moment when we could set anything, so a guarantee would not be in force
//      where it is actually needed.
//   2. ★ Setting it on OUR probe thread would make `crash test overflow --yes` pass while changing
//      nothing about the real case. The instrument would then certify a fix that does not hold — the
//      exact species of false confidence this project keeps having to retract.
//
// ⇒ So the stack cost was REMOVED from the fatal path instead of the budget being raised. That works
//   on any thread, ours or the game's, with no cooperation from the thread that dies — and it leaves
//   `crash test overflow` a hostile test, which is the only kind worth having.
// ============================================================================================

#include "tw3k.h"
#include <dbghelp.h>

// Resolved dynamically, so the link line stays as it is — no dbghelp.lib, no build.bat edit.
// dbghelp.dll ships with every supported Windows, so this is a formality, but it is checked and
// reported rather than assumed: `crash` says so if it ever is not.
typedef BOOL (WINAPI* MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);

static MiniDumpWriteDumpFn      g_miniDumpWriteDump = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter    = nullptr;
static HANDLE                   g_dumpThread        = nullptr;
static HANDLE                   g_dumpRequest       = nullptr;   // filter -> dumper
static HANDLE                   g_dumpDone          = nullptr;   // dumper -> filter
static volatile long            g_inFatal           = 0;         // reentrancy: one fatal, once
static volatile long            g_rearms            = 0;
static volatile long            g_dumpExit          = 0;         // detach: tell the parked thread to go
static bool                     g_installed         = false;
static bool                     g_wantFullMemory    = false;     // `crash heap on`

// Handed to the dumper thread. Points to resident copies, not frames which a timed-out wait may
// leave behind. The faulting thread blocks during the write, keeping stack memory available.
static EXCEPTION_POINTERS* volatile g_fatalPointers = nullptr;
static volatile DWORD               g_fatalThreadId = 0;
// Copied out of the exception record BY THE FILTER, so the dumper thread can log the crash without
// dereferencing anything on a stack that may be one page from unusable. Two plain stores.
static volatile DWORD               g_fatalCode     = 0;
static volatile uintptr_t           g_fatalAt       = 0;
static char                         g_lastDumpPath[MAX_PATH] = { 0 };
static volatile long                g_dumpsWritten  = 0;
static PVOID                        g_lowExecuteHandler = nullptr;
static bool                         g_firstChance = false;
// Resident copies survive the bounded wait even if another handler resumes execution. Do not
// leave the worker holding a pointer into a frame that the faulting thread can later unwind.
static EXCEPTION_RECORD             g_savedException{};
static CONTEXT                      g_savedContext{};
static EXCEPTION_POINTERS           g_savedPointers{ &g_savedException, &g_savedContext };

// DbgHelp normally finds stacks through thread metadata. S18's RSP may be outside that range.
// Include committed readable pages at saved RSP explicitly, bounded to 4 KiB below + 64 KiB above.
// VirtualQuery belongs on the worker; no allocation or stack walk on the faulting thread.
struct StackCapture { ULONG64 base; ULONG size; bool emitted; };
static BOOL CALLBACK dumpMemoryCallback(PVOID cookie, const PMINIDUMP_CALLBACK_INPUT in,
                                       PMINIDUMP_CALLBACK_OUTPUT out)
{
    if (in->CallbackType != MemoryCallback) return TRUE;
    auto* stack = (StackCapture*)cookie;
    if (!stack->size || stack->emitted) return FALSE;
    stack->emitted = true;
    out->MemoryBase = stack->base;
    out->MemorySize = stack->size;
    return TRUE;
}

static StackCapture faultStackMemory()
{
    StackCapture capture{};
    if (!g_fatalPointers || !g_fatalPointers->ContextRecord) return capture;
    const uintptr_t sp = g_fatalPointers->ContextRecord->Rsp;
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery((void*)sp, &m, sizeof(m)) || m.State != MEM_COMMIT ||
        (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || m.Protect == PAGE_EXECUTE) return capture;
    const uintptr_t begin = (uintptr_t)m.BaseAddress;
    const uintptr_t end = begin + m.RegionSize;
    capture.base = sp - begin > 4096 ? sp - 4096 : begin;
    const uintptr_t upper = end - sp > 65536 ? sp + 65536 : end;
    capture.size = (ULONG)(upper - capture.base);
    return capture;
}

// ---------------------------------------------------------------- the dump itself
//
// MiniDumpWithIndirectlyReferencedMemory is the flag that earns its place. It pulls in the memory
// that stack and register values POINT AT, which is exactly the question `crashWitness` already
// tries to answer by hand — its "node at RDI: vtable=... childCount=..." block is a hard-coded,
// three-field version of what this gives generally, for every pointer in every frame.
//
// It costs a few MB against a game whose full memory is ~8.6 GB, and writes in well under a second.
// ⚠ `crash heap on` swaps in MiniDumpWithFullMemory for a session, which is the ~8.6 GB dump
// Capture-TwDump.ps1 takes. Useful when hunting a specific object, far too slow to leave on.
static MINIDUMP_TYPE dumpFlags()
{
    if (g_wantFullMemory)
        return (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData |
                               MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

    return (MINIDUMP_TYPE)(MiniDumpNormal |
                           MiniDumpWithIndirectlyReferencedMemory |
                           MiniDumpWithThreadInfo |
                           MiniDumpWithUnloadedModules |
                           MiniDumpWithProcessThreadData);
}

// Named like the log deliberately: tw3k_crash_<HOST>_<STAMP>.dmp sits beside
// tw3k_coop_<HOST>_<STAMP>.log in the game's working directory, so one Sync-TwLogs-shaped copy
// brings back the pair and the names say which run they belong to.
static void buildDumpPath(char* out, size_t outSz)
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    _snprintf_s(out, outSz, _TRUNCATE, "tw3k_crash_%s_%04u%02u%02u-%02u%02u%02u.dmp",
                g_hostName, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static bool writeDumpNow()
{
    if (!g_miniDumpWriteDump) return false;

    char path[MAX_PATH] = { 0 };
    buildDumpPath(path, sizeof(path));

    HANDLE f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = g_fatalThreadId;
    mei.ExceptionPointers = g_fatalPointers;
    mei.ClientPointers    = FALSE;              // same process — the pointers are ours to read

    StackCapture stack = faultStackMemory();
    MINIDUMP_CALLBACK_INFORMATION callbacks{ dumpMemoryCallback, &stack };
    logf("  saved RSP=%016llX; explicit stack memory=%016llX +%lu bytes",
         (unsigned long long)g_savedContext.Rsp, (unsigned long long)stack.base, stack.size);
    const BOOL ok = g_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                        dumpFlags(),
                                        g_fatalPointers ? &mei : nullptr, nullptr, &callbacks);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();

    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    CloseHandle(f);

    if (!ok) {
        DeleteFileA(path);                       // a truncated dump is worse than none: it looks real
        SetLastError(error);
        return false;
    }

    strcpy_s(g_lastDumpPath, sizeof(g_lastDumpPath), path);
    InterlockedIncrement(&g_dumpsWritten);

    logf("  DUMP WRITTEN: %s  (%.1f MB, %s)", path,
         (double)size.QuadPart / (1024.0 * 1024.0),
         g_wantFullMemory ? "full memory" : "stacks + indirectly referenced memory");
    return true;
}

// ★★★ EVERY FATAL LINE IS WRITTEN FROM HERE, NOT FROM THE FILTER (2026-08-17, measured).
//
// It used to be `crashFatal` that logged the banner and the faulting address, and on an ACCESS
// VIOLATION that is perfectly safe. On a STACK OVERFLOW it is not, and the first live overflow test
// proved it: the log ended mid-test with `Going down now.` and **nothing else at all** — no FAULT
// line, no FATAL block, no dump. Not even a partial line.
//
// ⚠ THE REASON IT IS ALL-OR-NOTHING: `logf` formats into `msg[1024]` + `line[1200]` — about 2.2 KB
// of STACK buffers — and only then calls WriteFile. Windows dispatches EXCEPTION_STACK_OVERFLOW on
// the stack that just overflowed, with roughly one page of slack, of which the CONTEXT record alone
// takes 1,232 bytes. So the formatting dies before a single byte is written, and a handler that
// wanted to explain the crash is the reason there is no explanation.
//
// ⇒ So the filter now does the least it possibly can — set two fields, SetEvent, wait — and this
//   thread, which has its own fresh stack, writes all of it. That works on ANY thread, which
//   matters: SetThreadStackGuarantee is per-thread and we can only call it on threads we create.
//   A real overflow would land on a GAME thread, where we get no such chance.
static void logFatalPreamble()
{
    const DWORD     code = g_fatalCode;
    const uintptr_t at   = g_fatalAt;

    logf(g_firstChance ? "################ FIRST-CHANCE LOW EXECUTE — CAPTURING BEFORE UNWIND ################"
                      : "################ FATAL — NOTHING HANDLED THIS ################");
    logf("  code=%08lX at %016llX  — taking a dump",
         code, (unsigned long long)at);
    if (g_firstChance)
        logf("  This may still be handled. The VEH returns SEARCH after capture; fatality is unknown.");
    if (code == EXCEPTION_STACK_OVERFLOW)
        logf("  ⚠ STACK OVERFLOW. Everything below is written from the DUMPER thread: the faulting "
             "thread had no stack left to describe itself with, which is why the witness printed no "
             "FAULT block above. That is expected on this path, not a second bug.");

    // ★ The "whose bug is it" line the witness could not write on an overflowed stack. Safe here.
    HMODULE mod = nullptr;
    if (at && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 (LPCSTR)at, &mod) && mod) {
        char path[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(mod, path, MAX_PATH)) {
            const char* leaf = strrchr(path, '\\');
            logf("  module: %s  (base %016llX, +0x%llX)   OURS=%s",
                 leaf ? leaf + 1 : path, (unsigned long long)(uintptr_t)mod,
                 (unsigned long long)(at - (uintptr_t)mod),
                 (mod == g_selfModule) ? "YES — the fault is in tw3k_coop.dll" : "no");
        }
    } else if (at) {
        logf("  module: (no loaded module owns %016llX — a bad call target, or a smashed return "
             "address)", (unsigned long long)at);
    }
}

// The parked thread. Exists so the write — and now the LOGGING — happens on a stack that did not
// just overflow. ⚠ Both halves are load-bearing; see logFatalPreamble.
static DWORD WINAPI dumpThreadProc(LPVOID)
{
    for (;;) {
        if (WaitForSingleObject(g_dumpRequest, INFINITE) != WAIT_OBJECT_0) return 0;

        // ⚠ Detach signals the same event to get this thread OUT of our code before the DLL is
        // unmapped. A thread parked in a Wait inside a module that FreeLibrary then unmaps has its
        // return address in freed memory — see removeCrashDumper.
        if (g_dumpExit) return 0;

        logFatalPreamble();
        if (!writeDumpNow())
            logf("  DUMP FAILED: MiniDumpWriteDump refused (err=%lu). The lines above are all there "
                 "is for this crash.", GetLastError());
        logf(g_firstChance ? "################ END FIRST-CHANCE CAPTURE ################"
                          : "################ END FATAL ################");

        SetEvent(g_dumpDone);
    }
}

// ---------------------------------------------------------------- the last-chance filter

static void captureException(EXCEPTION_POINTERS* ep, bool firstChance)
{
    // One fatal crash gets one dump. A second arrival is either a fault inside this handler or a
    // second thread dying behind the first; either way, do not start a new write on top of one.
    if (InterlockedCompareExchange(&g_inFatal, 1, 0) != 0)
        return;

    // ⚠⚠ NOTHING BELOW MAY USE MEANINGFUL STACK, AND `logf` IS NOT ALLOWED HERE.
    //
    // This filter runs during the SEARCH phase, before any unwinding, so the stack is still as deep
    // as it was when the fault happened — deeper, with the dispatcher's frames on top. On a stack
    // overflow that leaves about one page, and `logf` wants 2.2 KB of it to format a line before it
    // writes anything. The 2026-08-17 overflow test spent the last of the stack inside the witness's
    // first `logf` and the log simply STOPPED: no FAULT, no FATAL, no dump. See logFatalPreamble.
    //
    // ⇒ Copy into static storage, signal, wait. No local CONTEXT buffer or stack walk here.
    g_firstChance = firstChance;
    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        g_savedException = *ep->ExceptionRecord;
        g_savedException.ExceptionRecord = nullptr; // no dangling nested record on the old stack
        g_savedContext = *ep->ContextRecord;
        g_fatalPointers = &g_savedPointers;
    }
    g_fatalThreadId = GetCurrentThreadId();
    if (ep && ep->ExceptionRecord) {
        g_fatalCode = ep->ExceptionRecord->ExceptionCode;
        g_fatalAt   = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    }

    if (g_dumpThread && g_dumpRequest && g_dumpDone) {
        SetEvent(g_dumpRequest);
        // Bounded. If the dumper cannot finish — a corrupt heap can wedge it — the game still gets
        // to die on schedule rather than hanging with no window and no explanation.
        // ⚠ The timeout line is logged by nobody here on purpose: if the dumper is wedged, this
        // thread may be the overflowed one, and a logf would take the process out silently. The
        // missing END FATAL block is the tell, and it is a truthful one.
        WaitForSingleObject(g_dumpDone, 30000);
    } else {
        // The only inline logging left, and it is unreachable on the overflow path that matters:
        // no dumper thread means installCrashDumper() failed at ATTACH, which it reports there.
        logf("  DUMP SKIPPED: the dumper thread was never created.");
    }

}

static LONG CALLBACK crashLowExecute(EXCEPTION_POINTERS* ep)
{
    if (g_installed && ep && ep->ExceptionRecord && ep->ContextRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 &&
            er->ExceptionInformation[0] == 8 && er->ExceptionInformation[1] < 0x10000 &&
            ep->ContextRecord->Rip < 0x10000)
            captureException(ep, true);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI crashFatal(EXCEPTION_POINTERS* ep)
{
    captureException(ep, false);
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- install / re-arm / report

bool installCrashDumper()
{
    if (g_installed) return true;

    // At ATTACH, never in the handler: a crash handler that needs the loader is useless in the one
    // situation it exists for.
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (dbg) g_miniDumpWriteDump = (MiniDumpWriteDumpFn)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!g_miniDumpWriteDump) {
        logf("crash dumper: dbghelp.dll/MiniDumpWriteDump unavailable — no dump will be taken. The "
             "crash witness is unaffected and still prints FAULT.");
        return false;
    }

    g_dumpRequest = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_dumpDone    = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (g_dumpRequest && g_dumpDone)
        g_dumpThread = CreateThread(nullptr, 0, dumpThreadProc, nullptr, 0, nullptr);

    if (!g_dumpThread) {
        logf("crash dumper: could not create the dumper thread (err=%lu) — no dump will be taken.",
             GetLastError());
        return false;
    }

    g_prevFilter = SetUnhandledExceptionFilter(&crashFatal);
    g_installed  = true;
    g_lowExecuteHandler = AddVectoredExceptionHandler(1, &crashLowExecute);
    if (!g_lowExecuteHandler)
        logf("crash dumper: low-execute VEH unavailable (err=%lu); only last-chance capture armed",
             GetLastError());
    else
        logf("crash dumper: first low-address execute AV captured before unwind; saved RSP memory included");
    return true;
}

// Called from the probe loop. See the re-arm note in this file's header: installing once is not
// enough, because the game installs its own filter after we install ours.
void rearmCrashDumper()
{
    if (!g_installed) return;

    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(&crashFatal);
    if (cur == &crashFatal) return;                    // still ours — nothing displaced us

    // Somebody displaced us. Keep them as the chain target so their handler still runs, and count
    // it: a machine that re-arms repeatedly is telling you something installs a filter in a loop.
    g_prevFilter = cur;
    const long n = InterlockedIncrement(&g_rearms);
    if (n <= 3 || (n % 50) == 0)
        logf("crash dumper: re-armed (#%ld) — something installed its own last-chance filter and is "
             "now chained behind ours.", n);
}

// ★★★★ TEARDOWN — AND WITHOUT IT, `detach` IS A DELAYED CRASH (2026-08-17, #62).
//
// `detach --yes` restores every patched byte, logs a clean "detached (unloading)", and then calls
// FreeLibraryAndExitThread. Every DETOUR is undone by then. But an exception handler is not a
// detour, and until today nothing here undid one:
//
//   * the UEF still pointed at `crashFatal`, in a module about to be unmapped — and the probe loop
//     had been RE-ARMING it every 2 s right up to the moment of detach;
//   * the parked dumper thread was still blocked in a Wait INSIDE this module, so its return
//     address was in memory that FreeLibrary was about to release.
//
// (`crashWitness`'s VEH is the third, and the oldest — see the detach tail in main.cpp.)
//
// ⇒ THE OBSERVED BUG: the game does not die at detach. It dies AFTERWARDS, on the next exception,
//   because the process-wide handler chain jumps into an unmapped page. That is exactly #62's
//   signature — "kills the game and leaves no crash dump" — and the "no dump" half is explained by
//   the same sentence: the dumper is the thing that was removed. The recorder cannot record its own
//   removal.
//
// ⚠ THE RECORDER MADE #62 WORSE, and that is worth stating plainly: before this branch only the VEH
// dangled. This branch added a second dangling pointer AND a parked thread, and took away the
// last-chance filter that might otherwise have caught the resulting death.
void removeCrashDumper()
{
    if (!g_installed && !g_dumpThread) return;
    g_installed = false;                 // stop rearmCrashDumper() re-taking it behind us
    if (g_lowExecuteHandler) {
        if (RemoveVectoredExceptionHandler(g_lowExecuteHandler)) g_lowExecuteHandler = nullptr;
        else logf("crash dumper: low-execute VEH removal failed (err=%lu); DLL must stay mapped",
                  GetLastError());
    }

    // ⚠ Restore ONLY if the filter is still ours. Something may have displaced us since the last
    // re-arm, and blindly writing g_prevFilter would replace a NEWER filter with an older one — i.e.
    // we would corrupt the very chain we are trying to leave intact. SetUnhandledExceptionFilter
    // returns the filter it replaced, so one call tells us which case we are in.
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(g_prevFilter);
    if (cur != &crashFatal) {
        SetUnhandledExceptionFilter(cur);       // not ours — put it back, untouched
        logf("crash dumper: the last-chance filter was NOT ours at detach (something displaced us "
             "after the final re-arm). Restored that newer filter; it may still chain ours, so the DLL stays mapped.");
    } else {
        logf("crash dumper: last-chance filter restored to previous (%p); DLL retained for in-flight calls",
             (void*)g_prevFilter);
    }

    // Bounded join. A timeout or failed wait keeps every resource the worker can still use.
    if (g_dumpThread) {
        InterlockedExchange(&g_dumpExit, 1);
        if (g_dumpRequest) SetEvent(g_dumpRequest);
        const DWORD wait = WaitForSingleObject(g_dumpThread, 2000);
        if (wait != WAIT_OBJECT_0) {
            logf("crash dumper: shutdown incomplete (wait=%lu error=%lu); retaining worker resources and DLL; restart required",
                 wait, wait == WAIT_FAILED ? GetLastError() : 0);
            return;
        }
        logf("crash dumper: worker exited");
        CloseHandle(g_dumpThread);
        g_dumpThread = nullptr;
    }
    // A fatal filter already in flight (or chained by a newer filter) can still use these
    // events and the dbghelp function. Keep them with the resident module until process exit.
}

void setCrashDumpFullMemory(bool on) { g_wantFullMemory = on; }

// ---------------------------------------------------------------- the deliberate fault
//
// ★★ WHY THIS COMMAND EXISTS, AND WHY IT IS NOT REDUNDANT WITH THE DESK MEASUREMENT.
//
// §6uuu.43 loaded the shipped DLL into a test host, faulted it, and got a valid MDMP in 48 ms. That
// proves the WRITE. It cannot prove the thing that actually decides whether a real CTD leaves a dump:
// only ONE last-chance filter exists per process, the game installs its own after ours, and a test
// host has no game in it to install a competing filter. So the re-arm — the half that matters — was
// measurable nowhere but inside the running game, and only at the moment of an actual crash.
//
// ⇒ A real crash is a bad instrument: it arrives when it likes, in a state nobody chose, and if no
//   dump appears there is no way to tell "the recorder is broken" from "that particular death never
//   reached an exception filter at all". This gives a fault WE chose, so a missing dump means the
//   recorder, and only the recorder.
//
// ⚠⚠ It kills the game. That is the entire point, and it is why `--yes` is mandatory and why the
// reply says single-player. Nothing is written to model state and nothing is patched: the process
// simply faults, our filter runs, and the filter it displaced still decides the outcome — exactly as
// it would on a crash that arrived by itself.
//
// The state line below is deliberate. `crash` prints the re-arm count only when asked, and this
// project has twice been misled by a counter read twenty minutes before the moment that mattered
// (HANDOVER's `raises performed=0`), so the log records it AT the fault.

// ⚠ `int* volatile`, not `volatile int*` — the VOLATILE HAS TO BE ON THE POINTER. With the qualifier
// on the pointee the compiler still knows this variable is initialised to null and never assigned, so
// it is free to fold the store into an unconditional trap (MSVC emits `ud2` for a provable null
// store), and the fault becomes ILLEGAL_INSTRUCTION at a synthetic address instead of the
// ACCESS_VIOLATION a real CTD looks like. Volatile on the pointer forces the load, so the store is a
// genuine write through a genuine null pointer.
static int* volatile g_faultTarget = nullptr;

// Recursion the compiler may not turn into a loop. It writes to its own frame and passes the address
// down, so each level needs a frame of its own and the stack really is consumed.
static int overflowTheStack(int depth)
{
    volatile int pad[64];
    for (int i = 0; i < 64; ++i) pad[i] = depth + i;
    return pad[depth & 63] + overflowTheStack(depth + 1);
}

void performCrashTest(long kind)
{
    logf("################ DELIBERATE FAULT — `crash test` ################");
    logf("  Somebody asked for this. The game is about to die on purpose, so that the recorder can be");
    logf("  proven in the GAME rather than in a test host. Recorder state AT THIS INSTANT:");
    logf("    dumper installed : %s", crashDumperInstalled() ? "yes" : "NO — expect no dump");
    logf("    re-arms          : %ld %s", crashDumperRearms(),
         crashDumperRearms() == 0 ? "(nothing has displaced us — the chain is untested)"
                                  : "(we were displaced and took the filter back)");
    logf("    chained to       : %s", crashDumperChained()
                                         ? "yes — another filter runs after ours"
                                         : "nothing — we are the only filter in this process");
    logf("    dump size mode   : %s", crashDumpFullMemory() ? "FULL MEMORY (~8.6 GB)" : "compact");
    logf("    faults so far    : %ld distinct, %ld total", faultKindsSeen(), faultsSeenTotal());

    if (kind == CRASH_TEST_OVERFLOW) {
        // ⚠ The case the parked dumper thread exists for. The filter runs on the stack that just
        // overflowed, where there is not enough room left to call MiniDumpWriteDump — so if this kind
        // produces a dump, the thread design is proven and not merely argued.
        logf("  kind: STACK OVERFLOW (EXCEPTION_STACK_OVERFLOW). This is the case the parked dumper");
        logf("  thread exists for — the filter has no stack left of its own, so a dump here proves");
        logf("  that design. Going down now.");
        volatile int sink = overflowTheStack(0);
        (void)sink;
    } else {
        logf("  kind: ACCESS VIOLATION, writing through a null pointer — the shape of a real CTD, so");
        logf("  the FAULT line and the dump should read exactly like one. Going down now.");
        *g_faultTarget = 0x0BADC0DE;
    }

    // Only reachable if something in the chain returned EXCEPTION_CONTINUE_EXECUTION, which is a
    // finding in itself: it means a handler is resuming a process that faulted at a null store.
    logf("  ⚠⚠ THE FAULT WAS RESUMED — execution continued past a deliberate null store. Something in");
    logf("  the filter chain returned CONTINUE_EXECUTION. No dump was expected and the game is still");
    logf("  running; write this down, because it also means a REAL crash here would be resumed.");
    logf("################ END DELIBERATE FAULT ################");
}

// Accessors rather than a report function that formats its own reply: `appendf` is file-static in
// control.cpp, and the pipe reply is worth more than a log line here — `crash` has to answer out of
// the RUNNING process for the same reason `ping` does. control.cpp does the formatting.
bool        crashDumperInstalled()   { return g_installed; }
bool        crashDumperChained()     { return g_prevFilter != nullptr; }
bool        crashDumpFullMemory()    { return g_wantFullMemory; }
long        crashDumperRearms()      { return g_rearms; }
long        crashDumpsWritten()      { return g_dumpsWritten; }
const char* crashDumperLastDump()    { return g_lastDumpPath[0] ? g_lastDumpPath : nullptr; }

#endif // developer facilities
