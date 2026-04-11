#include "pch.h"
#include "cw_shared.h"
#include "dxgi_hook_backend.h"
#include "preset_system.h"

// =============================================================================
//  DLL ENTRY
// =============================================================================
static bool IsTarget(){
    wchar_t p[MAX_PATH]={};GetModuleFileNameW(nullptr,p,MAX_PATH);
    return wcsstr(p,L"CrimsonDesert.exe")!=nullptr;}

BOOL APIENTRY DllMain(HMODULE hModule,DWORD reason,LPVOID){
    if(reason!=DLL_PROCESS_ATTACH)return TRUE;
    if(!IsTarget())return TRUE;
    DisableThreadLibraryCalls(hModule);
    char dir[MAX_PATH]={};GetModuleFileNameA(nullptr,dir,MAX_PATH);
    if(char*s=strrchr(dir,'\\'))*s='\0';
    LoadConfig(dir);OpenLogFile(dir);
    g_menuOpen = g_cfg.showGuiOnStartup;
    Log("================================================\n");
    Log("  " MOD_NAME " v" MOD_VERSION "\n");
    Log("================================================\n\n");
    Log("[i] Base: %p\n\n",(void*)GetModuleHandle(nullptr));
    if(MH_Initialize()!=MH_OK){Log("[E] MH_Init\n");return TRUE;}
    if(!RunAOBScan()){MH_Uninitialize();return TRUE;}
    SetupConfiguredDxgiHooks();
    SetupInputHooks();
    Preset_ArmAutoApplyRemembered();
    Log("[+] Ready.\n\n");
    return TRUE;
}
