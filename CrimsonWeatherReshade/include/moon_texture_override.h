#pragma once

#include <Windows.h>

bool InitializeMoonTextureOverride(HMODULE module);
void ShutdownMoonTextureOverride();
void MoonTextureReload();
const char* MoonTextureStatus();
bool MoonTextureReady();
void MoonTextureRefreshList();
int MoonTextureOptionCount();
const char* MoonTextureOptionName(int index);
const char* MoonTextureOptionLabel(int index);
const char* MoonTextureOptionPack(int index);
int MoonTextureFindOptionByName(const char* name);
int MoonTextureSelectedOption();
void MoonTextureSelectOption(int index);
bool MoonTextureSelectByName(const char* name);
