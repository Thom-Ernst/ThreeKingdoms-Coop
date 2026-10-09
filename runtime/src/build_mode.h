#pragma once
// Build capability boundary, shared by code and the Windows version resource.
#ifdef TW3K_RELEASE
#define TW3K_BUILD_MODE "RELEASE"
#define TW3K_MODE_TEXT(debugText, releaseText) releaseText
// Diagnostic arguments are not evaluated or emitted in the player DLL.
#define diagLogf(...) ((void)0)
#define TW3K_DIAGNOSTIC(...) ((void)0)
#else
#define TW3K_BUILD_MODE "DEBUG"
#define TW3K_MODE_TEXT(debugText, releaseText) debugText
#define diagLogf(...) logf(__VA_ARGS__)
#define TW3K_DIAGNOSTIC(...) __VA_ARGS__
#endif
