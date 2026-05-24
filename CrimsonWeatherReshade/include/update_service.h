#pragma once

#include <string>

enum class UpdateCheckState {
    Disabled = 0,
    Idle,
    Checking,
    Latest,
    UpdateAvailable,
    Downloading,
    Installed,
    Error,
};

struct UpdateCheckInfo {
    UpdateCheckState state = UpdateCheckState::Idle;
    bool updateAvailable = false;
    bool checking = false;
    bool downloading = false;
    bool installed = false;
    std::string currentVersion;
    std::string latestVersion;
    std::string title;
    std::string changelog;
    std::string downloadPageUrl;
    std::string addonDownloadUrl;
    std::string addonSha256;
    long long addonSizeBytes = 0;
    std::string status;
};

void UpdateService_CleanupStaleFiles();
void UpdateService_Tick();
void UpdateService_RequestCheck(bool force);
UpdateCheckInfo UpdateService_GetInfo();
void UpdateService_OpenDownloadPage();
void UpdateService_InstallUpdate();
