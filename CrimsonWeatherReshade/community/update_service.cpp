#include "pch.h"

#include "update_service.h"

#include "community_endpoint_config.h"
#include "community_http.h"
#include "runtime_shared.h"

#include <Shellapi.h>

#include <atomic>
#include <cctype>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr unsigned long long kUpdateCheckIntervalSeconds = 24ull * 60ull * 60ull;
constexpr size_t kMaxUpdateResponseBytes = 48ull * 1024ull;
constexpr const char* kUpdateChannel = "stable";
constexpr const char* kFallbackDownloadPageUrl = "https://www.nexusmods.com/crimsondesert/mods/632?tab=files";

std::mutex g_updateMutex;
UpdateCheckInfo g_updateInfo;
std::atomic<bool> g_updateChecking{ false };
std::atomic<unsigned long long> g_lastUpdateCheck{ 0 };

std::string TrimCopy(const std::string& value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

std::string Endpoint() {
    std::string endpoint = TrimCopy(CW_COMMUNITY_DEFAULT_ENDPOINT);
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    return endpoint;
}

std::string UrlForUpdate() {
    const std::string endpoint = Endpoint();
    if (endpoint.empty()) {
        return {};
    }
    return endpoint + "/api/v1/update?version=" MOD_BASE_VERSION "&channel=" + kUpdateChannel;
}

std::string ExtractJsonString(const std::string& object, const char* key) {
    const std::string marker = std::string("\"") + key + "\"";
    size_t pos = object.find(marker);
    if (pos == std::string::npos) return {};
    pos = object.find(':', pos + marker.size());
    if (pos == std::string::npos) return {};
    pos = object.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    std::string out;
    bool escape = false;
    for (++pos; pos < object.size(); ++pos) {
        const char c = object[pos];
        if (escape) {
            switch (c) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += c; break;
            }
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') break;
        out += c;
    }
    return out;
}

bool ExtractJsonBool(const std::string& object, const char* key) {
    const std::string marker = std::string("\"") + key + "\"";
    size_t pos = object.find(marker);
    if (pos == std::string::npos) return false;
    pos = object.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;
    while (++pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) {}
    return object.compare(pos, 4, "true") == 0;
}

void SetUpdateInfo(const UpdateCheckInfo& info) {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    g_updateInfo = info;
    g_updateInfo.checking = g_updateChecking.load();
}

void SetStatus(UpdateCheckState state, const std::string& status) {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    g_updateInfo.state = state;
    g_updateInfo.status = status;
    g_updateInfo.currentVersion = MOD_BASE_VERSION;
    g_updateInfo.checking = g_updateChecking.load();
}

std::vector<CommunityHttpHeader> UpdateHeaders() {
    return {
        { "accept", "application/json" },
        { "x-cw-client-version", MOD_BASE_VERSION },
        { "x-cw-channel", kUpdateChannel },
    };
}

void CheckWorker() {
    UpdateCheckInfo info{};
    info.currentVersion = MOD_BASE_VERSION;
    info.downloadPageUrl = kFallbackDownloadPageUrl;

    const std::string url = UrlForUpdate();
    if (url.empty()) {
        info.state = UpdateCheckState::Disabled;
        info.status = "Update endpoint is not configured";
        SetUpdateInfo(info);
        g_updateChecking.store(false);
        return;
    }

    CommunityHttpResponse response;
    if (!CommunityHttp_Request("GET", url, UpdateHeaders(), "", response)) {
        info.state = UpdateCheckState::Error;
        info.status = "Update check failed: " + response.error;
        SetUpdateInfo(info);
        g_updateChecking.store(false);
        return;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        info.state = UpdateCheckState::Error;
        info.status = "Update check failed: HTTP " + std::to_string(response.statusCode);
        SetUpdateInfo(info);
        g_updateChecking.store(false);
        return;
    }
    if (response.body.size() > kMaxUpdateResponseBytes) {
        info.state = UpdateCheckState::Error;
        info.status = "Update check failed: response too large";
        SetUpdateInfo(info);
        g_updateChecking.store(false);
        return;
    }

    info.updateAvailable = ExtractJsonBool(response.body, "updateAvailable");
    info.latestVersion = ExtractJsonString(response.body, "version");
    info.title = ExtractJsonString(response.body, "title");
    info.changelog = ExtractJsonString(response.body, "changelog");
    const std::string downloadPage = ExtractJsonString(response.body, "downloadPageUrl");
    if (!downloadPage.empty()) {
        info.downloadPageUrl = downloadPage;
    }
    if (info.latestVersion.empty()) {
        info.latestVersion = MOD_BASE_VERSION;
    }

    info.state = info.updateAvailable ? UpdateCheckState::UpdateAvailable : UpdateCheckState::Latest;
    info.status = info.updateAvailable ? "Update available" : "Latest";
    g_lastUpdateCheck.store(static_cast<unsigned long long>(std::time(nullptr)));
    SetUpdateInfo(info);
    g_updateChecking.store(false);
}

} // namespace

void UpdateService_Tick() {
#if defined(CW_WIND_ONLY)
    return;
#else
    if (!g_cfg.updaterEnabled) {
        SetStatus(UpdateCheckState::Disabled, "Update check disabled");
        return;
    }
    const unsigned long long now = static_cast<unsigned long long>(std::time(nullptr));
    const unsigned long long last = g_lastUpdateCheck.load();
    if (last == 0 || now > last + kUpdateCheckIntervalSeconds) {
        UpdateService_RequestCheck(false);
    }
#endif
}

void UpdateService_RequestCheck(bool force) {
#if defined(CW_WIND_ONLY)
    (void)force;
#else
    if (!g_cfg.updaterEnabled) {
        SetStatus(UpdateCheckState::Disabled, "Update check disabled");
        return;
    }
    const unsigned long long now = static_cast<unsigned long long>(std::time(nullptr));
    const unsigned long long last = g_lastUpdateCheck.load();
    if (!force && last != 0 && now <= last + kUpdateCheckIntervalSeconds) {
        return;
    }
    bool expected = false;
    if (!g_updateChecking.compare_exchange_strong(expected, true)) {
        return;
    }
    g_lastUpdateCheck.store(now);
    SetStatus(UpdateCheckState::Checking, "Checking for updates...");
    std::thread([]() {
        CheckWorker();
    }).detach();
#endif
}

UpdateCheckInfo UpdateService_GetInfo() {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    UpdateCheckInfo info = g_updateInfo;
    info.checking = g_updateChecking.load();
    if (info.currentVersion.empty()) {
        info.currentVersion = MOD_BASE_VERSION;
    }
    if (info.downloadPageUrl.empty()) {
        info.downloadPageUrl = kFallbackDownloadPageUrl;
    }
    if (info.state == UpdateCheckState::Idle && g_cfg.updaterEnabled) {
        info.status = "Checking soon";
    }
    return info;
}

void UpdateService_OpenDownloadPage() {
    const UpdateCheckInfo info = UpdateService_GetInfo();
    const std::string url = info.downloadPageUrl.empty() ? kFallbackDownloadPageUrl : info.downloadPageUrl;
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
