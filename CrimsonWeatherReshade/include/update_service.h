#pragma once

#include <string>

enum class UpdateCheckState {
    Disabled = 0,
    Idle,
    Checking,
    Latest,
    UpdateAvailable,
    Error,
};

struct UpdateCheckInfo {
    UpdateCheckState state = UpdateCheckState::Idle;
    bool updateAvailable = false;
    bool checking = false;
    std::string currentVersion;
    std::string latestVersion;
    std::string title;
    std::string changelog;
    std::string downloadPageUrl;
    std::string status;
};

void UpdateService_Tick();
void UpdateService_RequestCheck(bool force);
UpdateCheckInfo UpdateService_GetInfo();
void UpdateService_OpenDownloadPage();

