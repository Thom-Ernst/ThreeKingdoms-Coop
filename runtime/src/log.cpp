// log.cpp - Per-machine, per-run log file: naming, reset, and the timestamped logf.
//
// Split out of the original single-file tw3k_coop.cpp. Behaviour is unchanged: this was pure
// code motion. ../tw3k_coop.ASBUILT.cpp remains the known-good single-file snapshot.

#include "tw3k.h"

// ---------------------------------------------------------------- logging

// Each injection writes to its OWN file, named for the machine and the moment it attached:
//
//     tw3k_coop_<HOSTNAME>_<YYYYMMDD-HHMMSS>.log
//
// A 3-machine test produces three files that can never collide, so they can be collected straight
// into one folder with no renaming — which was previously manual per run and got a stale run-3 file
// mistaken for a run-4 result twice. Unique names also mean no rotation is needed: every run's
// evidence is kept automatically instead of one .prev.log deep.
char g_logPath[MAX_PATH] = "tw3k_coop.log";   // fallback if naming fails
char g_hostName[64]      = "UNKNOWN";

void initLogPath()
{
    DWORD n = (DWORD)sizeof(g_hostName);
    if (!GetComputerNameA(g_hostName, &n) || g_hostName[0] == '\0')
        strcpy_s(g_hostName, sizeof(g_hostName), "UNKNOWN");

    // Keep the name filesystem-safe: hostnames are usually fine, but do not trust them.
    for (char* p = g_hostName; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            *p = '_';

    SYSTEMTIME st{};
    GetLocalTime(&st);
    _snprintf_s(g_logPath, sizeof(g_logPath), _TRUNCATE,
                "tw3k_coop_%s_%04u%02u%02u-%02u%02u%02u.log",
                g_hostName, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void resetLog()
{
    initLogPath();
    HANDLE f = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
}

void logf(const char* fmt, ...)
{
    // ★ Logging from a corrupt recursive widget walk must not spend another 2.2 KB
    // of its remaining stack. Per-thread storage also keeps concurrent logs separate.
    // A fault witness can re-enter on this same thread: drop that nested log rather
    // than overwrite the outer message or recursively fault on the same bad %s.
    static thread_local char msg[1024];
    static thread_local char line[1200];
    static thread_local bool active = false;
    if (active) return;
    active = true;
    va_list args;
    va_start(args, fmt);
    __try {
        _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        // Output truncation alone does not make an engine-memory %s safe. Callers
        // should pass bounded local copies; contain a stale source as a last brake.
        strcpy_s(msg, sizeof(msg), "logf: unreadable format/string argument");
    }
    va_end(args);

    // Lead with LOCAL WALL-CLOCK time, not just the tick count. Tick counts are per-process and
    // meaningless across machines; with three logs to compare, wall clock is what allows "player 3
    // was seated at 20:51:07, and the host reacted at 20:51:08" to be read off directly. The tick is
    // kept after it for precise ordering within one process.
    SYSTEMTIME st{};
    GetLocalTime(&st);

    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u|%8u] %s\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetTickCount(), msg);

    OutputDebugStringA(line);

    HANDLE f = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(f, line, (DWORD)strlen(line), &written, nullptr);
        CloseHandle(f);
    }
    active = false;
}

