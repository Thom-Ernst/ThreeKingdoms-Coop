@echo off
setlocal
REM Run from an x64 Native Tools command prompt. Relative sources and pathmap
REM keep private build paths out of DLL strings. No injector is built.
where cl.exe >nul 2>nul
if errorlevel 1 (
    echo FAIL: open an x64 Native Tools command prompt with C++ and Windows SDK.
    exit /b 1
)
cd /d "%~dp0"
if /i "%~1"=="test" goto :tests
set "TWCOMMIT="
for /f "usebackq delims=" %%c in (`git rev-parse --short HEAD 2^>nul`) do set "TWCOMMIT=%%c"
> src\build_info.h echo #define TW3K_GIT "1.0.0 %TWCOMMIT%"
if /i "%~1"=="debug" goto :debug
call :dll release
if errorlevel 1 exit /b 1
if /i "%~1"=="all" call :dll debug
if errorlevel 1 exit /b 1
cl /nologo /LD /EHsc /O2 /std:c++17 /utf-8 proxy\ags_proxy.cpp /Fe:proxy\amd_ags_x64.dll /Fo:proxy\ /link /DLL
if errorlevel 1 exit /b 1
echo ok requested runtime modes and proxy built; no injector
exit /b 0
:debug
call :dll debug
exit /b %errorlevel%
:dll
set "TWDEFINE="
set "TWOUTPUT=tw3k_coop_debug.dll"
if /i "%~1"=="release" set "TWDEFINE=/DTW3K_RELEASE"
if /i "%~1"=="release" set "TWOUTPUT=tw3k_coop.dll"
rc /nologo %TWDEFINE% /fo build_mode.res build_mode.rc
if errorlevel 1 exit /b 1
cl /nologo /LD /EHa /O2 /Gy /std:c++17 /utf-8 %TWDEFINE% /experimental:deterministic /wd5048 /pathmap:"%CD%=." src\*.cpp build_mode.res /Fe:%TWOUTPUT% /link user32.lib /OPT:REF /OPT:ICF
exit /b %errorlevel%
:tests
python tests\run_offline.py
exit /b %errorlevel%
