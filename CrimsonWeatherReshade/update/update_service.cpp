#include "pch.h"

#include "update_service.h"

#include "community_endpoint_config.h"
#include "community_http.h"
#include "runtime_shared.h"
#include "../core/background_work.h"
#include "update_install.h"
#include "update_protocol.h"

#include <bcrypt.h>
#include <Shellapi.h>

#include <atomic>
#include <cctype>
#include <ctime>
#include <mutex>
#include <vector>

namespace {

constexpr unsigned long long kUpdateCheckIntervalSeconds = 24ull * 60ull * 60ull;
constexpr unsigned long long kFailedUpdateRetrySeconds = 5ull * 60ull;
constexpr size_t kMaxUpdateResponseBytes = 48ull * 1024ull;
constexpr size_t kMaxAddonDownloadBytes = 128ull * 1024ull * 1024ull;
constexpr const char* kUpdateChannel = "stable";
constexpr const char* kFallbackDownloadPageUrl = "https://www.nexusmods.com/crimsondesert/mods/632?tab=files";
constexpr const char* kAddonFileName = "CrimsonWeather.addon64";

std::mutex g_updateMutex;
std::mutex g_updateLifecycleMutex;
UpdateCheckInfo g_updateInfo;
std::atomic<bool> g_updateChecking{ false };
std::atomic<bool> g_updateDownloading{ false };
std::atomic<bool> g_updateCleanupDone{ false };
std::mutex g_updateRecoveryMutex;
std::atomic<unsigned long long> g_updateLastRecoveryAttempt{ 0 };
std::atomic<bool> g_updateDisabledStatusPublished{ false };
std::atomic<unsigned long long> g_lastUpdateCheck{ 0 };
std::atomic<bool> g_lastUpdateCheckSucceeded{ false };
std::atomic<BackgroundWorkQueue*> g_updateQueuePointer{ nullptr };
std::atomic<bool> g_updateStopping{ false };
std::atomic<unsigned long long> g_updateGeneration{ 1 };
OwnerCompletionQueue g_updateCompletions(16);
thread_local bool g_updateWorkerContext = false;
thread_local unsigned long long g_updateWorkerGeneration = 0;

void SetStatus(UpdateCheckState state, const std::string& status);

BackgroundWorkQueue& UpdateWorkQueue() {
    static BackgroundWorkQueue queue(2);
    g_updateQueuePointer.store(&queue);
    return queue;
}

bool PostUpdateCompletion(unsigned long long generation, std::function<void()> completion) {
    if (g_updateStopping.load()) return false;
    std::function<void()> guarded = [generation, completion = std::move(completion)]() mutable {
        if (g_updateStopping.load() || g_updateGeneration.load() != generation) return;
        completion();
    };
    return g_updateCompletions.PostUntilAccepted(generation, std::move(guarded), g_updateStopping);
}

BackgroundSubmitResult QueueUpdateWork(
    const char* key,
    BackgroundWorkQueue::Work work) {
    std::lock_guard<std::mutex> lifecycleLock(g_updateLifecycleMutex);
    if (g_updateStopping.load()) return BackgroundSubmitResult::Stopping;
    BackgroundWorkQueue& queue = UpdateWorkQueue();
    if (g_updateStopping.load()) {
        queue.SignalStopWithoutLock();
        return BackgroundSubmitResult::Stopping;
    }
    const unsigned long long generation = g_updateGeneration.load();
    return queue.Submit(key ? key : "", [work = std::move(work), generation](const std::atomic<bool>& stop) {
        if (stop.load() || g_updateStopping.load()) return;
        const bool previousContext = g_updateWorkerContext;
        const unsigned long long previousGeneration = g_updateWorkerGeneration;
        g_updateWorkerContext = true;
        g_updateWorkerGeneration = generation;
        try {
            work(stop);
        } catch (...) {
            g_updateDownloading.store(false);
            g_updateChecking.store(false);
            PostUpdateCompletion(generation, [] {
                SetStatus(UpdateCheckState::Error, "Update operation failed unexpectedly");
            });
        }
        g_updateWorkerContext = previousContext;
        g_updateWorkerGeneration = previousGeneration;
    });
}

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

bool WriteBinaryFile(
    const char* path,
    const std::string& body,
    std::string& error,
    const std::atomic<bool>* stopRequested = nullptr) {
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "create file failed: " + std::to_string(GetLastError());
        return false;
    }

    size_t offset = 0;
    while (offset < body.size()) {
        if (stopRequested && stopRequested->load()) {
            error = "write cancelled";
            CloseHandle(file);
            DeleteFileA(path);
            return false;
        }
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

void SetUpdateInfoNow(const UpdateCheckInfo& info) {
    std::lock_guard<std::mutex> lock(g_updateMutex);
    g_updateInfo = info;
    g_updateInfo.checking = g_updateChecking.load();
    g_updateInfo.downloading = g_updateDownloading.load();
}

void SetUpdateInfo(const UpdateCheckInfo& info) {
    if (g_updateWorkerContext) {
        const unsigned long long generation = g_updateWorkerGeneration;
        PostUpdateCompletion(generation, [info] { SetUpdateInfoNow(info); });
        return;
    }
    SetUpdateInfoNow(info);
}

void SetStatusNow(UpdateCheckState state, const std::string& status) {
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

void SetStatus(UpdateCheckState state, const std::string& status) {
    if (g_updateWorkerContext) {
        const unsigned long long generation = g_updateWorkerGeneration;
        PostUpdateCompletion(generation, [state, status] { SetStatusNow(state, status); });
        return;
    }
    SetStatusNow(state, status);
}

std::vector<CommunityHttpHeader> UpdateHeaders() {
    return {
        { "accept", "application/json" },
        { "x-cw-client-version", MOD_BASE_VERSION },
        { "x-cw-channel", kUpdateChannel },
    };
}

void CheckWorker(const std::atomic<bool>& stopRequested) {
    if (stopRequested.load() || g_updateStopping.load()) {
        g_updateChecking.store(false);
        return;
    }
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
    if (!CommunityHttp_Request("GET", url, UpdateHeaders(), "", response, &stopRequested)) {
        if (stopRequested.load() || g_updateStopping.load()) {
            g_updateChecking.store(false);
            return;
        }
        info.state = UpdateCheckState::Error;
        info.status = "Update check failed: " + response.error;
        SetUpdateInfo(info);
        g_updateChecking.store(false);
        return;
    }
    if (stopRequested.load() || g_updateStopping.load()) {
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

    if (!update_protocol::ParseUpdateMetadata(response.body, info)) {
        info.state = UpdateCheckState::Error;
        info.status = "Update check failed: invalid JSON";
        SetUpdateInfo(info);
        g_updateChecking.store(false);
        return;
    }
    if (info.latestVersion.empty()) {
        info.latestVersion = MOD_BASE_VERSION;
    }

    info.state = info.updateAvailable ? UpdateCheckState::UpdateAvailable : UpdateCheckState::Latest;
    info.status = info.updateAvailable ? "Update available" : "Latest";
    g_lastUpdateCheck.store(static_cast<unsigned long long>(std::time(nullptr)));
    g_lastUpdateCheckSucceeded.store(true);
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

void InstallWorker(UpdateCheckInfo info, bool autoDownload, const std::atomic<bool>& stopRequested) {
    if (stopRequested.load() || g_updateStopping.load()) {
        g_updateDownloading.store(false);
        return;
    }
    SetStatus(UpdateCheckState::Downloading, "Downloading update...");
    PostUpdateCompletion(g_updateWorkerGeneration, [] {
        GUI_SetStatus("Downloading Crimson Weather update...");
    });

    const auto finish = [](UpdateCheckState state, const std::string& status) {
        g_updateDownloading.store(false);
        SetStatus(state, status);
        if (state == UpdateCheckState::Installed) {
            PostUpdateCompletion(g_updateWorkerGeneration, [] {
                GUI_SetStatus("Update success. Restart Crimson Desert to apply.");
            });
        }
    };

    if (!autoDownload) {
        finish(UpdateCheckState::Error, "Direct update download is disabled");
        return;
    }
    if (info.addonDownloadUrl.empty() || !IsSha256Hex(info.addonSha256)) {
        finish(UpdateCheckState::Error, "Direct update package is not available");
        return;
    }

    CommunityHttpResponse response;
    if (!CommunityHttp_Request("GET", info.addonDownloadUrl, UpdateArtifactHeaders(), "", response, &stopRequested)) {
        if (stopRequested.load() || g_updateStopping.load()) {
            g_updateDownloading.store(false);
            return;
        }
        finish(UpdateCheckState::Error, "Update download failed: " + response.error);
        return;
    }
    if (stopRequested.load() || g_updateStopping.load()) {
        g_updateDownloading.store(false);
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

    std::string recoveryDetail;
    bool recovered = false;
    {
        // Cleanup runs from the render tick. Keep only this short recovery step
        // serialized with it; downloading the artifact and writing .new stay unlocked.
        std::lock_guard<std::mutex> recoveryLock(g_updateRecoveryMutex);
        recovered = update_install::RecoverInterruptedInstall(currentPath, tempPath, oldPath, recoveryDetail);
    }
    if (!recovered) {
        finish(UpdateCheckState::Error, "Update recovery failed: " + recoveryDetail);
        return;
    }
    Log("[update] recovery: %s\n", recoveryDetail.c_str());

    std::string error;
    if (!WriteBinaryFile(tempPath, response.body, error, &stopRequested)) {
        if (stopRequested.load() || g_updateStopping.load()) {
            g_updateDownloading.store(false);
            return;
        }
        finish(UpdateCheckState::Error, "Update write failed: " + error);
        return;
    }
    if (stopRequested.load() || g_updateStopping.load()) {
        DeleteFileA(tempPath);
        g_updateDownloading.store(false);
        return;
    }
    if (!update_install::ActivateStagedFile(currentPath, tempPath, oldPath, error)) {
        finish(UpdateCheckState::Error, "Update install failed: " + error);
        return;
    }

    Log("[update] installed version=%s sha256=%s file=%s\n",
        info.latestVersion.c_str(),
        hash.c_str(),
        currentPath);
    finish(UpdateCheckState::Installed, "Update success. Restart to apply.");
}

} // namespace

void UpdateService_CleanupStaleFiles() {
#if defined(CW_WIND_ONLY)
    return;
#else
    if (g_updateCleanupDone.load() || g_updateStopping.load()) {
        return;
    }
    std::lock_guard<std::mutex> recoveryLock(g_updateRecoveryMutex);
    // The install request sets this before queueing. The shared mutex closes
    // the check/start race with an already-running recovery attempt.
    if (g_updateCleanupDone.load() || g_updateStopping.load() || g_updateDownloading.load()) return;
    const unsigned long long nowTick = GetTickCount64();
    const unsigned long long lastAttempt = g_updateLastRecoveryAttempt.load();
    if (lastAttempt != 0 && nowTick - lastAttempt < 5000) return;
    g_updateLastRecoveryAttempt.store(nowTick);
    char currentPath[MAX_PATH] = {};
    char stagedPath[MAX_PATH] = {};
    char backupPath[MAX_PATH] = {};
    BuildAddonPath(currentPath, sizeof(currentPath));
    BuildAddonPath(stagedPath, sizeof(stagedPath), ".new");
    BuildAddonPath(backupPath, sizeof(backupPath), ".old");
    std::string detail;
    if (update_install::RecoverInterruptedInstall(currentPath, stagedPath, backupPath, detail)) {
        Log("[update] recovery: %s\n", detail.c_str());
        g_updateCleanupDone.store(true);
    } else {
        Log("[W] update recovery: %s\n", detail.c_str());
    }
#endif
}

void UpdateService_BeginShutdown() {
    std::lock_guard<std::mutex> lifecycleLock(g_updateLifecycleMutex);
    if (g_updateStopping.exchange(true)) return;
    g_updateGeneration.fetch_add(1);
    BackgroundWorkQueue* queue = g_updateQueuePointer.load();
    if (queue) queue->StopAcceptingWork();
}

bool UpdateService_WaitForShutdown(unsigned long waitMilliseconds) {
    BackgroundWorkQueue* queue = nullptr;
    {
        std::lock_guard<std::mutex> lifecycleLock(g_updateLifecycleMutex);
        queue = g_updateQueuePointer.load();
    }
    return !queue || queue->WaitForStop(waitMilliseconds);
}

void UpdateService_SignalStopWithoutWait() noexcept {
    if (g_updateStopping.exchange(true)) return;
    g_updateGeneration.fetch_add(1);
    BackgroundWorkQueue* queue = g_updateQueuePointer.load();
    if (queue) queue->SignalStopWithoutLock();
}

void UpdateService_CloseAfterShutdown() {
    {
        std::lock_guard<std::mutex> lifecycleLock(g_updateLifecycleMutex);
        BackgroundWorkQueue* queue = g_updateQueuePointer.load();
        if (queue) queue->CloseAfterStop();
    }
    g_updateCompletions.Clear();
}

void UpdateService_Tick() {
#if defined(CW_WIND_ONLY)
    return;
#else
    if (g_updateStopping.load()) return;
    g_updateCompletions.Drain(g_updateGeneration.load());
    UpdateService_CleanupStaleFiles();
    if (!g_cfg.updaterEnabled) {
        if (!g_updateDisabledStatusPublished.exchange(true)) {
            SetStatus(UpdateCheckState::Disabled, "Update check disabled");
        }
        return;
    }
    g_updateDisabledStatusPublished.store(false);
    const unsigned long long now = static_cast<unsigned long long>(std::time(nullptr));
    const unsigned long long last = g_lastUpdateCheck.load();
    const unsigned long long interval = g_lastUpdateCheckSucceeded.load()
        ? kUpdateCheckIntervalSeconds
        : kFailedUpdateRetrySeconds;
    if (last == 0 || now > last + interval) {
        UpdateService_RequestCheck(false);
    }
#endif
}

void UpdateService_RequestCheck(bool force) {
#if defined(CW_WIND_ONLY)
    (void)force;
#else
    if (g_updateStopping.load()) return;
    if (!g_cfg.updaterEnabled) {
        if (!g_updateDisabledStatusPublished.exchange(true)) {
            SetStatus(UpdateCheckState::Disabled, "Update check disabled");
        }
        return;
    }
    g_updateDisabledStatusPublished.store(false);
    const unsigned long long now = static_cast<unsigned long long>(std::time(nullptr));
    const unsigned long long last = g_lastUpdateCheck.load();
    const unsigned long long interval = g_lastUpdateCheckSucceeded.load()
        ? kUpdateCheckIntervalSeconds
        : kFailedUpdateRetrySeconds;
    if (!force && last != 0 && now <= last + interval) {
        return;
    }
    bool expected = false;
    if (!g_updateChecking.compare_exchange_strong(expected, true)) {
        return;
    }
    g_lastUpdateCheck.store(now);
    g_lastUpdateCheckSucceeded.store(false);
    SetStatus(UpdateCheckState::Checking, "Checking for updates...");
    const BackgroundSubmitResult result = QueueUpdateWork("update-check", [](const std::atomic<bool>& stop) {
        CheckWorker(stop);
    });
    if (result != BackgroundSubmitResult::Accepted) {
        g_updateChecking.store(false);
        if (result != BackgroundSubmitResult::Stopping) {
            SetStatus(UpdateCheckState::Error, "Could not queue update check");
        }
    }
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
    if (g_updateStopping.load()) return;
    const bool autoDownload = g_cfg.updaterAutoDownload;
    if (!autoDownload) {
        UpdateService_OpenDownloadPage();
        return;
    }
    bool expected = false;
    if (!g_updateDownloading.compare_exchange_strong(expected, true)) {
        return;
    }
    UpdateCheckInfo info = UpdateService_GetInfo();
    const BackgroundSubmitResult result = QueueUpdateWork("update-install", [info = std::move(info), autoDownload](const std::atomic<bool>& stop) mutable {
        InstallWorker(std::move(info), autoDownload, stop);
    });
    if (result != BackgroundSubmitResult::Accepted) {
        g_updateDownloading.store(false);
        if (result != BackgroundSubmitResult::Stopping) {
            SetStatus(UpdateCheckState::Error, "Could not queue update download");
        }
    }
#endif
}
