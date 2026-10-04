#pragma once

#include <Windows.h>

bool InitializeOverlayBridge(HMODULE module, HMODULE reshadeModule = nullptr);
void ShutdownOverlayBridge(HMODULE module = nullptr, HMODULE reshadeModule = nullptr);
