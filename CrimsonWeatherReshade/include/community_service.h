#pragma once

#include "community_models.h"

#include <string>
#include <vector>

void Community_EnsureInitialized();
void Community_Shutdown();
void Community_Tick();
bool Community_IsEnabled();
void Community_SetEnabled(bool enabled);
bool Community_IsBusy();
const char* Community_GetStatusText();
const char* Community_GetEndpoint();
void Community_SetEndpoint(const char* endpoint);
const char* Community_GetClientId();
unsigned long long Community_GetLastRefresh();

int Community_GetCatalogCount();
CommunityCatalogItem Community_GetCatalogItem(int index);
int Community_GetMyUploadCount();
CommunityMyUpload Community_GetMyUploadItem(int index);
CommunityPresetUpdateStatus Community_GetPresetUpdateStatus(int presetIndex);

bool Community_CanManualRefresh();
void Community_RequestRefresh(bool manual);
bool Community_RequestInitialViewRefresh();
void Community_RequestMyUploads();
void Community_RequestDownload(const char* presetId);
void Community_RequestUpdateDownloadedPreset(int presetIndex);
void Community_RequestLike(const char* presetId);
void Community_RequestDeleteMyUpload(const char* presetId);
void Community_RequestUpdateMyUpload(
    const char* presetId,
    const char* title,
    const char* author,
    const char* description,
    int presetIndex);
void Community_RequestSubmit(
    const char* title,
    const char* author,
    const char* description);
