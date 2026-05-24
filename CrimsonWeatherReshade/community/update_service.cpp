#include "pch.h"

#include "update_service.h"

#include "community_endpoint_config.h"
#include "community_http.h"
#include "runtime_shared.h"

#include <bcrypt.h>
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
constexpr size_t kMaxAddonDownloadBytes = 128ull * 1024ull * 1024ull;
constexpr const char* kUpdateChannel = "stable";
constexpr const char* kFallbackDownloadPageUrl = "https://www.nexusmods.com/crimsondesert/mods/632?tab=files";
constexpr const char* kAddonFileName = "CrimsonWeather.addon64";

std::mutex g_updateMutex;
UpdateCheckInfo g_updateInfo;
std::atomic<bool> g_updateChecking{ false };
std::atomic<bool> g_updateDownloading{ false };
std::atomic<bool> g_updateCleanupDone{ false };
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

long long ExtractJsonInt64(const std::string& object, const char* key) {
    const std::string marker = std::string("\"") + key + "\"";
    size_t pos = object.find(marker);
    if (pos == std::string::npos) return 0;
    pos = object.find(':', pos + marker.size());
    if (pos == std::string::npos) return 0;
    while (++pos < object.size() && std::isspace(static_cast<unsigned char>(object[pos]))) {}
    bool negative = false;
    if (pos < object.size() && object[pos] == '-') {
        negative = true;
        ++pos;
    }
    long long value = 0;
    while (pos < object.size() && std::isdigit(static_cast<unsigned char>(object[pos]))) {
        value = value * 10 + (object[pos++] - '0');
    }
    return negative ? -value : value;
}

bool IsSha256Hex(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    for (char c : value) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool Sha256Hex(const std::string& body, std::string& outHex) {
    outHex.clear();
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD cbData = 0;
    unsigned char digest[32] = {};
    char hex[65] = {};

    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &cbData, 0) != 0) {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    std::vector<unsigned char> object(objectSize);
    if (BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) != 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(body.data())), static_cast<ULONG>(body.size()), 0) != 0 ||
        BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        sprintf_s(hex + i * 2, sizeof(hex) - i * 2, "%02x", digest[i]);
    }
    outHex = hex;
    return true;
}

void BuildAddonPath(char* outPath, size_t outSize, const char* suffix = "") {
    if (!outPath || outSize == 0) {
        return;
    }
    const char* dir = g_pluginDir[0] ? g_pluginDir : ".";
    sprintf_s(outPath, outSize, "%s\\%s%s", dir, kAddonFileName, suffix ? suffix : "");
}

bool WriteBinaryFile(const char* path, const std::string& body, std::string& error) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "create file failed: " + std::to_string(GetLastError());
        return false;
    }

    size_t offset = 0;
    while (offset < body.size()) {
        const DWORD chunk = static_cast<DWORD>(min<size_t>(body.size() - offset, 1ull << 20));
        DWORD written = 0;
        if (!WriteFile(file, body.data() + offset, chunk, &written, nullptr) || written != chunk) {
            error = "write file failed: " + std::to_string(GetLastError());
            CloseHandle(file);
            return false;
        }
        offset += written;
    }
    CloseHandle(file);
    return true;
}

void SetUpdateInfo(const UpdateCheckInfo& info) {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    g_updateInfo = info;
    g_updateInfo.checking = g_updateChecking.load();
    g_updateInfo.downloading = g_updateDownloading.load();
}

void SetStatus(UpdateCheckState state, const std::string& status) {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    g_updateInfo.state = state;
    g_updateInfo.status = status;
    g_updateInfo.currentVersion = MOD_BASE_VERSION;
    g_updateInfo.checking = g_updateChecking.load();
    g_updateInfo.downloading = g_updateDownloading.load();
    g_updateInfo.installed = state == UpdateCheckState::Installed;
    if (state == UpdateCheckState::Installed) {
        g_updateInfo.updateAvailable = false;
    }
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
    info.addonDownloadUrl = ExtractJsonString(response.body, "addonDownloadUrl");
    info.addonSha256 = ExtractJsonString(response.body, "addonSha256");
    info.addonSizeBytes = ExtractJsonInt64(response.body, "addonSizeBytes");
    if (info.latestVersion.empty()) {
        info.latestVersion = MOD_BASE_VERSION;
    }

    info.state = info.updateAvailable ? UpdateCheckState::UpdateAvailable : UpdateCheckState::Latest;
    info.status = info.updateAvailable ? "Update available" : "Latest";
    g_lastUpdateCheck.store(static_cast<unsigned long long>(std::time(nullptr)));
    SetUpdateInfo(info);
    g_updateChecking.store(false);
}

std::vector<CommunityHttpHeader> UpdateArtifactHeaders() {
    return {
        { "accept", "application/octet-stream" },
        { "x-cw-client-version", MOD_BASE_VERSION },
        { "x-cw-channel", kUpdateChannel },
    };
}

void InstallWorker(UpdateCheckInfo info) {
    SetStatus(UpdateCheckState::Downloading, "Downloading update...");
    GUI_SetStatus("Downloading Crimson Weather update...");

    const auto finish = [](UpdateCheckState state, const std::string& status) {
        g_updateDownloading.store(false);
        SetStatus(state, status);
    };

    if (!g_cfg.updaterAutoDownload) {
        finish(UpdateCheckState::Error, "Direct update download is disabled");
        return;
    }
    if (info.addonDownloadUrl.empty() || !IsSha256Hex(info.addonSha256)) {
        finish(UpdateCheckState::Error, "Direct update package is not available");
        return;
    }

    CommunityHttpResponse response;
    if (!CommunityHttp_Request("GET", info.addonDownloadUrl, UpdateArtifactHeaders(), "", response)) {
        finish(UpdateCheckState::Error, "Update download failed: " + response.error);
        return;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        finish(UpdateCheckState::Error, "Update download failed: HTTP " + std::to_string(response.statusCode));
        return;
    }
    if (response.body.empty() || response.body.size() > kMaxAddonDownloadBytes) {
        finish(UpdateCheckState::Error, "Update download rejected: invalid size");
        return;
    }
    if (info.addonSizeBytes > 0 && static_cast<long long>(response.body.size()) != info.addonSizeBytes) {
        finish(UpdateCheckState::Error, "Update download rejected: size mismatch");
        return;
    }

    std::string hash;
    if (!Sha256Hex(response.body, hash) || _stricmp(hash.c_str(), info.addonSha256.c_str()) != 0) {
        finish(UpdateCheckState::Error, "Update download rejected: SHA-256 mismatch");
        return;
    }

    char currentPath[MAX_PATH] = {};
    char tempPath[MAX_PATH] = {};
    char oldPath[MAX_PATH] = {};
    BuildAddonPath(currentPath, sizeof(currentPath));
    BuildAddonPath(tempPath, sizeof(tempPath), ".new");
    BuildAddonPath(oldPath, sizeof(oldPath), ".old");

    DeleteFileA(tempPath);
    DeleteFileA(oldPath);

    std::string error;
    if (!WriteBinaryFile(tempPath, response.body, error)) {
        finish(UpdateCheckState::Error, "Update write failed: " + error);
        return;
    }

    if (!MoveFileExA(currentPath, oldPath, MOVEFILE_REPLACE_EXISTING)) {
        const DWORD code = GetLastError();
        DeleteFileA(tempPath);
        finish(UpdateCheckState::Error, "Update install failed: could not rename current add-on (" + std::to_string(code) + ")");
        return;
    }
    if (!MoveFileExA(tempPath, currentPath, MOVEFILE_REPLACE_EXISTING)) {
        const DWORD code = GetLastError();
        MoveFileExA(oldPath, currentPath, MOVEFILE_REPLACE_EXISTING);
        DeleteFileA(tempPath);
        finish(UpdateCheckState::Error, "Update install failed: could not activate new add-on (" + std::to_string(code) + ")");
        return;
    }

    Log("[update] installed version=%s sha256=%s file=%s\n",
        info.latestVersion.c_str(),
        hash.c_str(),
        currentPath);
    finish(UpdateCheckState::Installed, "Update success. Restart to apply.");
    GUI_SetStatus("Update success. Restart Crimson Desert to apply.");
}

} // namespace

void UpdateService_CleanupStaleFiles() {
#if defined(CW_WIND_ONLY)
    return;
#else
    bool expected = false;
    if (!g_updateCleanupDone.compare_exchange_strong(expected, true)) {
        return;
    }
    char oldPath[MAX_PATH] = {};
    BuildAddonPath(oldPath, sizeof(oldPath), ".old");
    if (DeleteFileA(oldPath)) {
        Log("[update] removed stale backup %s\n", oldPath);
    }
#endif
}

void UpdateService_Tick() {
#if defined(CW_WIND_ONLY)
    return;
#else
    UpdateService_CleanupStaleFiles();
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
    info.downloading = g_updateDownloading.load();
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

void UpdateService_InstallUpdate() {
#if defined(CW_WIND_ONLY)
    return;
#else
    if (!g_cfg.updaterAutoDownload) {
        UpdateService_OpenDownloadPage();
        return;
    }
    bool expected = false;
    if (!g_updateDownloading.compare_exchange_strong(expected, true)) {
        return;
    }
    UpdateCheckInfo info = UpdateService_GetInfo();
    std::thread([info]() {
        InstallWorker(info);
    }).detach();
#endif
}
