#pragma once
#include <cstddef>
#include <windows.h>
#ifndef TW3K_RELEASE
inline bool uiArmingFlagExists() {
    const DWORD attributes = GetFileAttributesA("tw3k_ui_test.flag");
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
void executeUiControl(const char* command, char* reply, size_t size);
void stopUiControl();

#endif
