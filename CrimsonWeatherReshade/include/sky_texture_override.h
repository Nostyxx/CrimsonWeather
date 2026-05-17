#pragma once

#include <Windows.h>

struct ID3D12Device;

bool InitializeSkyTextureOverride(HMODULE module);
void SkyTextureOnInitDevice(ID3D12Device* device);
void ShutdownSkyTextureOverride();
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

void MilkywayTextureReload();
const char* MilkywayTextureStatus();
bool MilkywayTextureReady();
void MilkywayTextureRefreshList();
int MilkywayTextureOptionCount();
const char* MilkywayTextureOptionName(int index);
const char* MilkywayTextureOptionLabel(int index);
const char* MilkywayTextureOptionPack(int index);
int MilkywayTextureFindOptionByName(const char* name);
int MilkywayTextureSelectedOption();
void MilkywayTextureSelectOption(int index);
bool MilkywayTextureSelectByName(const char* name);
