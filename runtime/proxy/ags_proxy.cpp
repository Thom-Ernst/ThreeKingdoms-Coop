// ags_proxy.cpp — a forwarder DLL the GAME loads by itself, so nothing has to inject.
//
// ============================================================================================
// WHY THIS EXISTS
// ============================================================================================
//
// The distribution is being flagged as malware — by scanners, and now by tester's ISP, which is
// blocking how the mod reaches other players. The trigger is almost certainly `injector.exe`:
// `OpenProcess` -> `VirtualAllocEx` -> `WriteProcessMemory` -> `CreateRemoteThread` is the textbook
// remote-injection pattern, with a PowerShell watcher beside it.
//
// ⇒ **A proxy DLL removes the behaviour rather than hiding it.** No injector, no remote thread, no
// watcher, no elevation prompt. The game loads a DLL out of its own folder because its own import
// table says to. It is how ENB and ReShade have worked for fifteen years.
//
// ✗ Explicitly NOT the approach: packing, obfuscating or encrypting the DLL, renaming extensions,
// or telling people to switch antivirus off. Those raise detection rates rather than lowering them,
// and would make the flag deserved instead of wrong.
//
// ============================================================================================
// WHY `amd_ags_x64.dll`, AND NOT SOMETHING ELSE
// ============================================================================================
//
// Read out of the binary rather than chosen by habit:
//
// * `Three_Kingdoms.exe` imports exactly THREE symbols from it — `agsInit`, `agsDeInit`,
//   `agsGetEyefinityConfigInfo` — through IAT slots `0x14323D200/208/210`.
// * It is a **STATIC** import, not delay-loaded, verified two ways: `dumpbin /dependents` lists it
//   under the regular import directory with no delay-load section at all, and the thunk for
//   `agsInit` at `0x142702486` is a plain `JMP qword ptr [0x14323D210]` rather than a
//   `__delayLoadHelper` stub.
//
//   ★★ **That is the decisive property.** The Windows loader resolves static imports *before* the
//   exe's entry point runs, so this DLL is mapped and its `DllMain` runs on **every** machine —
//   including one with no AMD GPU, where AGS itself does nothing. A delay-loaded target could
//   simply never be triggered on somebody's machine, and the mod would silently not load.
// * The game's own `amd_ags_x64.dll` is AGS 3.x, x64, with **22** exports. The exe imports 3 of
//   them; this forwards all 22, because the file must remain a drop-in replacement for anything
//   else that resolves against it.
//
// ⚠ **Do NOT move this to `CLOCKWORK.RELEASE.X64.DLL`** — the other game-folder import. That is the
// netcode layer this entire project studies; putting our own DLL in front of it would confuse every
// future reading of the thing we are trying to understand.
// ⚠ The system DLLs the exe imports (`OLE32`, `SHELL32`, `ADVAPI32`, `GDI32` …) are mostly
// **KnownDLLs** and cannot be proxied by dropping a file in the game folder at all.
//
// ============================================================================================
// HOW IT WORKS, AND THE ONE FILE OPERATION IT NEEDS
// ============================================================================================
//
//     amd_ags_x64.dll        <- THIS file (the proxy)
//     amd_ags_x64_orig.dll   <- the game's original, renamed once
//
// Every export below is a linker **forward**: the loader resolves `agsInit` by loading
// `amd_ags_x64_orig.dll` and taking its `agsInit`. No thunks, no hand-written stubs, nothing to
// keep in step with a calling convention.
//
// `tools/Install-TwProxy.ps1` does the rename, refuses to do it twice, and reverses it.
//
// ============================================================================================
// ⚠ THE ONE REAL UNKNOWN, AND HOW THIS FILE IS BUILT AROUND IT
// ============================================================================================
//
// **Nobody has tested whether Denuvo tolerates an extra module in its address space at process
// start.** Community practice (ReShade and ENB use exactly this method on Denuvo titles) says it is
// fine, and Denuvo protects the exe rather than its DLL dependencies — but that is other people's
// experience, not a measurement on `Three_Kingdoms.exe`.
//
// ⇒ So the mod is loaded **only if `tw3k_coop.dll` is sitting next to this file**, which makes the
// experiment a rename rather than a rebuild:
//
//     1. Install the proxy. Move `tw3k_coop.dll` aside. Start the game.
//        Reaches the main menu  -> Denuvo tolerates the proxy. That is the whole unknown, settled.
//        Does not              -> the proxy itself is the problem, and no mod code ran at all,
//                                 so the answer is unambiguous.
//     2. Move `tw3k_coop.dll` back. Start the game. The log appears without anyone injecting.
//
// ★ It is also the right behaviour permanently: if someone deletes the mod DLL, this degrades to a
// pure passthrough instead of failing. A proxy that breaks the game when the mod is absent would be
// a worse thing to hand a stranger than the injector was.

#include <windows.h>

// ---- the forwarded exports ---------------------------------------------------------------------
//
// All 22, from `dumpbin /exports` on the game's own copy. x64 exports C names undecorated, so the
// names below are exactly what the loader looks for.
//
// ⚠ Regenerate this list if a game patch ever replaces the DLL. A forwarder pinned to 22 names is
// wrong the moment the original grows a 23rd, and the failure mode is the game not starting.

#define FWD(name) __pragma(comment(linker, "/export:" #name "=amd_ags_x64_orig." #name))

FWD(agsDeInit)
FWD(agsDriverExtensions_BeginUAVOverlap)
FWD(agsDriverExtensions_CreateBuffer)
FWD(agsDriverExtensions_CreateTexture1D)
FWD(agsDriverExtensions_CreateTexture2D)
FWD(agsDriverExtensions_CreateTexture3D)
FWD(agsDriverExtensions_DeInit)
FWD(agsDriverExtensions_EndUAVOverlap)
FWD(agsDriverExtensions_IASetPrimitiveTopology)
FWD(agsDriverExtensions_Init)
FWD(agsDriverExtensions_MultiDrawIndexedInstancedIndirect)
FWD(agsDriverExtensions_MultiDrawInstancedIndirect)
FWD(agsDriverExtensions_NotifyResourceBeginAllAccess)
FWD(agsDriverExtensions_NotifyResourceEndAllAccess)
FWD(agsDriverExtensions_NotifyResourceEndWrites)
FWD(agsDriverExtensions_RegisterApp)
FWD(agsDriverExtensions_SetDepthBounds)
FWD(agsGetCrossfireGPUCount)
FWD(agsGetEyefinityConfigInfo)
FWD(agsGetGPUMemorySize)
FWD(agsGetTotalGPUCount)
FWD(agsInit)

// A marker export, so a script can ask "is the amd_ags_x64.dll in this folder ours?" without
// hashing anything or guessing from a file size. `Install-TwProxy.ps1` checks for it before
// renaming, which is what stops a second run from renaming the PROXY over the saved original and
// destroying the real DLL.
extern "C" __declspec(dllexport) void Tw3kCoopProxyMarker(void) { }

// ---- loading the mod ----------------------------------------------------------------------------

static const wchar_t* const kModDll = L"tw3k_coop.dll";

static HMODULE g_self = nullptr;

// Runs on its own thread, NOT in DllMain.
//
// ⚠ `LoadLibrary` from inside `DllMain` is a documented deadlock: we would already hold the loader
// lock that `LoadLibrary` needs. Creating a thread instead is safe precisely because the new thread
// cannot start until the loader releases that lock — so by the time this runs, process
// initialisation has moved on. The mod's own `DllMain` does the same thing for the same reason.
static DWORD WINAPI loadModThread(LPVOID)
{
    wchar_t path[MAX_PATH] = { 0 };
    const DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;

    // Replace our own filename with the mod's. Absolute, deliberately: relying on the DLL search
    // order would mean loading whatever `tw3k_coop.dll` the PATH happens to reach first, and this
    // project has already lost a session to a stale copy in a different folder.
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    *(slash + 1) = L'\0';
    if (wcslen(path) + wcslen(kModDll) >= MAX_PATH) return 0;
    wcscat_s(path, MAX_PATH, kModDll);

    // Absent is a normal, supported state — see the Denuvo experiment above. No message box, no
    // log file of our own: this DLL stays silent so that anything in the game's folder that looks
    // like our doing is the mod's log and nothing else.
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 0;

    // ★ Tell the mod HOW it got here, so its log can say so.
    //
    // A log currently cannot distinguish "the proxy loaded me" from "somebody ran injector.exe",
    // and we are in the middle of swapping one for the other — which is exactly when that
    // distinction decides whether a test proved anything. One environment variable, set on this
    // process only, read once in the banner.
    //
    // Set BEFORE LoadLibrary: the mod's DllMain starts its probe thread immediately, and a value
    // written afterwards would be a race with the line that reads it.
    SetEnvironmentVariableW(L"TW3K_LOADED_BY", L"proxy");

    LoadLibraryW(path);     // the mod's DllMain takes it from here and does all the waiting
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(nullptr, 0, loadModThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
