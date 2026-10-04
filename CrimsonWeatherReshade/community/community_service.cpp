#include "pch.h"

#include "community_service.h"
#include "community_protocol.h"

#include "community_endpoint_config.h"
#include "community_http.h"
#include "preset_service.h"
#include "runtime_shared.h"
#include "../core/background_work.h"

#include <bcrypt.h>

#include <atomic>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <ctime>

namespace {

using namespace community_protocol;

constexpr unsigned long long kAutoRefreshSeconds = 24ull * 60ull * 60ull;
constexpr unsigned long long kManualRefreshCooldownSeconds = 60ull;
constexpr size_t kMaxDownloadBytes = 65536;
constexpr size_t kMaxStatusBodyChars = 180;

std::mutex g_communityMutex;
std::mutex g_communityLifecycleMutex;
constexpr size_t kCommunityQueueCapacity = 8;
std::atomic<BackgroundWorkQueue*> g_detachWorkQueue{ nullptr };
std::atomic<bool> g_communityStopping{ false };
std::atomic<unsigned long long> g_communityGeneration{ 1 };
OwnerCompletionQueue g_communityCompletions(64);
thread_local bool g_communityWorkerContext = false;
thread_local unsigned long long g_communityWorkerGeneration = 0;
thread_local const std::atomic<bool>* g_communityWorkerStop = nullptr;
thread_local std::string g_communityWorkerEndpoint;
thread_local std::string g_communityWorkerClientId;
std::vector<CommunityCatalogItem> g_catalog;
std::vector<CommunityMyUpload> g_myUploads;
std::vector<std::string> g_likedPresetIds;
std::string g_communityStatusText = "Community presets idle";
std::string g_endpoint;
std::string g_clientId;
std::atomic<unsigned long long> g_lastCatalogRefresh{ 0 };
std::atomic<bool> g_initialized{ false };
std::once_flag g_stateLoadOnce;
std::atomic<unsigned long long> g_lastManualRefreshTick{ 0 };

BackgroundWorkQueue& CommunityWorkQueue() {
    static BackgroundWorkQueue queue(kCommunityQueueCapacity);
    g_detachWorkQueue.store(&queue);
    return queue;
}

void SetStatusNow(const std::string& status) {
    std::lock_guard<std::mutex> lock(g_communityMutex);
    g_communityStatusText = status;
}

bool PostCompletion(unsigned long long generation, std::function<void()> completion, bool critical = false) {
    if (g_communityStopping.load()) return false;
    std::function<void()> guarded = [generation, completion = std::move(completion)]() mutable {
        if (g_communityStopping.load() || g_communityGeneration.load() != generation) return;
        completion();
    };
    if (critical) {
        return g_communityCompletions.PostUntilAccepted(generation, std::move(guarded), g_communityStopping);
    }
    return g_communityCompletions.Post(generation, std::move(guarded));
}

std::string TrimCopy(const std::string& value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(start, end - start);
}

std::string JoinPathLocal(const std::string& dir, const std::string& fileName) {
    if (dir.empty()) return fileName;
    if (dir.back() == '\\' || dir.back() == '/') return dir + fileName;
    return dir + "\\" + fileName;
}

void EnsureDirectory(const std::string& path) {
    if (path.empty()) return;
    std::string partial;
    for (char c : path) {
        partial.push_back(c);
        if ((c == '\\' || c == '/') && partial.size() > 1) {
            CreateDirectoryA(partial.c_str(), nullptr);
        }
    }
    CreateDirectoryA(path.c_str(), nullptr);
}

std::string CommunityRoot() {
    const std::string base = g_pluginDir[0] ? std::string(g_pluginDir) : ".";
    return JoinPathLocal(JoinPathLocal(base, "CrimsonWeather"), "community");
}

std::string CatalogCachePath() {
    return JoinPathLocal(CommunityRoot(), "catalog.v1.json");
}

std::string StatePath() {
    return JoinPathLocal(CommunityRoot(), "state.v1");
}

void SetStatus(const std::string& status) {
    if (g_communityWorkerContext) {
        const unsigned long long generation = g_communityWorkerGeneration;
        PostCompletion(generation, [status]() { SetStatusNow(status); });
        return;
    }
    SetStatusNow(status);
}

std::string StatusHttpError(const char* prefix, const CommunityHttpResponse& response) {
    std::string status = std::string(prefix) + ": HTTP " + std::to_string(response.statusCode);
    std::string body = TrimCopy(response.body);
    if (!body.empty()) {
        if (body.size() > kMaxStatusBodyChars) {
            body.resize(kMaxStatusBodyChars);
            body += "...";
        }
        status += " - " + body;
    }
    return status;
}

std::string ReadFileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool WriteFileText(const std::string& path, const std::string& text) {
    EnsureDirectory(CommunityRoot());
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::string ReadStateValue(const std::string& text, const char* key) {
    const std::string prefix = std::string(key) + "=";
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(prefix, 0) == 0) {
            return TrimCopy(line.substr(prefix.size()));
        }
    }
    return {};
}

std::vector<std::string> SplitCsv(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        std::string value = TrimCopy(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!value.empty()) out.push_back(value);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string JoinCsv(const std::vector<std::string>& values) {
    std::string out;
    for (const std::string& value : values) {
        if (value.empty()) continue;
        if (!out.empty()) out += ",";
        out += value;
    }
    return out;
}

void SetLikedPresetId(const std::string& id, bool liked) {
    if (id.empty()) return;
    for (auto it = g_likedPresetIds.begin(); it != g_likedPresetIds.end(); ++it) {
        if (_stricmp(it->c_str(), id.c_str()) == 0) {
            if (!liked) g_likedPresetIds.erase(it);
            return;
        }
    }
    if (liked) g_likedPresetIds.push_back(id);
}

std::string GenerateCommunityClientId() {
    unsigned char bytes[16] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        const unsigned long long fallback =
            (static_cast<unsigned long long>(GetTickCount64()) << 17) ^
            static_cast<unsigned long long>(counter.QuadPart) ^
            (static_cast<unsigned long long>(GetCurrentProcessId()) << 33);
        memcpy(bytes, &fallback, min(sizeof(bytes), sizeof(fallback)));
    }
    char id[48] = {};
    sprintf_s(id,
        "cw-%02x%02x%02x%02x-%02x%02x%02x%02x-%02x%02x%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return id;
}

void SaveCommunityState() {
    std::string clientId;
    std::string endpoint;
    std::vector<std::string> likedPresetIds;
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        clientId = g_clientId;
        endpoint = g_endpoint;
        likedPresetIds = g_likedPresetIds;
    }
    std::string body;
    body += "client_id=" + clientId + "\r\n";
    body += "last_catalog_refresh=" + std::to_string(g_lastCatalogRefresh.load()) + "\r\n";
    body += "liked_preset_ids=" + JoinCsv(likedPresetIds) + "\r\n";
#if defined(CW_DEV_BUILD)
    if (!endpoint.empty() && _stricmp(endpoint.c_str(), CW_COMMUNITY_DEFAULT_ENDPOINT) != 0) {
        body += "endpoint_override=" + endpoint + "\r\n";
    }
#endif
    WriteFileText(StatePath(), body);
}

void LoadCommunityState() {
    std::call_once(g_stateLoadOnce, []() {
        EnsureDirectory(CommunityRoot());
        std::string endpoint = TrimCopy(CW_COMMUNITY_DEFAULT_ENDPOINT);

        const std::string state = ReadFileText(StatePath());
        std::string clientId = ReadStateValue(state, "client_id");
        if (clientId.empty()) {
            clientId = GenerateCommunityClientId();
        }
        const std::string lastRefresh = ReadStateValue(state, "last_catalog_refresh");
        const unsigned long long refreshValue = lastRefresh.empty() ? 0 : _strtoui64(lastRefresh.c_str(), nullptr, 10);
        std::vector<std::string> likedPresetIds = SplitCsv(ReadStateValue(state, "liked_preset_ids"));
#if defined(CW_DEV_BUILD)
        const std::string endpointOverride = ReadStateValue(state, "endpoint_override");
        if (!endpointOverride.empty()) {
            endpoint = endpointOverride;
        }
#endif
        {
            std::lock_guard<std::mutex> lock(g_communityMutex);
            g_endpoint = endpoint;
            g_clientId = clientId;
            g_likedPresetIds = likedPresetIds;
        }
        g_lastCatalogRefresh.store(refreshValue);
        SaveCommunityState();
    });
}

void ApplyCatalog(
    std::vector<CommunityCatalogItem> parsed,
    std::string json,
    bool fromNetwork) {
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        g_catalog.swap(parsed);
        if (fromNetwork) {
            g_lastCatalogRefresh.store(static_cast<unsigned long long>(time(nullptr)));
        }
    }
    if (fromNetwork) {
        WriteFileText(CatalogCachePath(), json);
        SaveCommunityState();
    }
    SetStatus(fromNetwork ? "Community catalog refreshed" : "Loaded cached community catalog");
}

void StoreCatalog(const std::string& json, bool fromNetwork) {
    std::vector<CommunityCatalogItem> parsed;
    std::vector<std::string> likedPresetIds;
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        likedPresetIds = g_likedPresetIds;
    }
    if (!ParseCatalog(json, likedPresetIds, parsed)) {
        SetStatus("Community catalog parse failed");
        return;
    }
    if (g_communityWorkerContext && fromNetwork) {
        const unsigned long long generation = g_communityWorkerGeneration;
        PostCompletion(generation, [parsed = std::move(parsed), json]() mutable {
            ApplyCatalog(std::move(parsed), json, true);
        }, true);
        return;
    }
    ApplyCatalog(std::move(parsed), json, fromNetwork);
}

std::string Endpoint() {
    if (g_communityWorkerContext) {
        return g_communityWorkerEndpoint;
    }
    LoadCommunityState();
    std::lock_guard<std::mutex> lock(g_communityMutex);
    return TrimCopy(g_endpoint);
}

std::string UrlFor(const std::string& path) {
    std::string endpoint = Endpoint();
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    return endpoint + path;
}

std::vector<CommunityHttpHeader> JsonHeaders() {
    if (g_communityWorkerContext) {
        return {
            { "content-type", "application/json; charset=utf-8" },
            { "x-cw-client-id", g_communityWorkerClientId },
            { "x-cw-client-version", MOD_VERSION },
        };
    }
    LoadCommunityState();
    std::string clientId;
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        clientId = g_clientId;
    }
    return {
        { "content-type", "application/json; charset=utf-8" },
        { "x-cw-client-id", clientId },
        { "x-cw-client-version", MOD_VERSION },
    };
}

bool Sha256Hex(const std::string& body, std::string& outHex) {
    outHex.clear();
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD cbData = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &cbData, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<unsigned char> object(objectSize);
    unsigned char digest[32] = {};
    if (BCryptCreateHash(alg, &hash, object.data(), objectSize, nullptr, 0, 0) != 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(body.data())), static_cast<ULONG>(body.size()), 0) != 0 ||
        BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65] = {};
    for (int i = 0; i < 32; ++i) {
        sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
    }
    outHex = hex;
    return true;
}

bool RunAsync(const std::function<void()>& work, bool allowParallel = false, const char* coalesceKey = nullptr) {
    std::lock_guard<std::mutex> lifecycleLock(g_communityLifecycleMutex);
    if (g_communityStopping.load()) return false;
    BackgroundWorkQueue& queue = CommunityWorkQueue();
    if (g_communityStopping.load()) {
        queue.SignalStopWithoutLock();
        return false;
    }
    if (!allowParallel && queue.OutstandingCount() > 0) {
        SetStatus("Another community request is still in progress");
        return false;
    }

    LoadCommunityState();
    std::string endpoint;
    std::string clientId;
    unsigned long long generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        endpoint = g_endpoint;
        clientId = g_clientId;
        generation = g_communityGeneration.load();
    }
    const BackgroundSubmitResult result = queue.Submit(coalesceKey ? coalesceKey : "", [work, endpoint = std::move(endpoint), clientId = std::move(clientId), generation](const std::atomic<bool>& stop) {
        if (stop.load() || g_communityStopping.load()) return;
        const bool previousWorkerContext = g_communityWorkerContext;
        const unsigned long long previousGeneration = g_communityWorkerGeneration;
        const std::atomic<bool>* previousStop = g_communityWorkerStop;
        std::string previousEndpoint = std::move(g_communityWorkerEndpoint);
        std::string previousClientId = std::move(g_communityWorkerClientId);
        g_communityWorkerContext = true;
        g_communityWorkerGeneration = generation;
        g_communityWorkerStop = &stop;
        g_communityWorkerEndpoint = endpoint;
        g_communityWorkerClientId = clientId;
        try {
            work();
        } catch (...) {
            SetStatus("Community request failed unexpectedly");
        }
        g_communityWorkerContext = previousWorkerContext;
        g_communityWorkerGeneration = previousGeneration;
        g_communityWorkerStop = previousStop;
        g_communityWorkerEndpoint = std::move(previousEndpoint);
        g_communityWorkerClientId = std::move(previousClientId);
    });
    if (result == BackgroundSubmitResult::Accepted) return true;
    if (result == BackgroundSubmitResult::Duplicate) SetStatus("That community request is already queued");
    else if (result == BackgroundSubmitResult::Full) SetStatus("Community request queue is full");
    else if (result == BackgroundSubmitResult::StartFailed) SetStatus("Could not start community request worker");
    else if (result == BackgroundSubmitResult::Stopping) SetStatus("Community service is shutting down");
    return false;
}

void RefreshWorker(bool manual) {
    if (Endpoint().empty()) {
        SetStatus("Community endpoint is not configured");
        return;
    }
    SetStatus("Refreshing community catalog...");
    CommunityHttpResponse response;
    if (!CommunityHttp_Request("GET", UrlFor("/api/v1/catalog"), JsonHeaders(), "", response, g_communityWorkerStop)) {
        SetStatus("Community refresh failed: " + response.error);
        return;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        SetStatus(StatusHttpError("Community refresh failed", response));
        return;
    }
    if (manual) {
        g_lastManualRefreshTick.store(GetTickCount64());
    }
    StoreCatalog(response.body, true);
}

void MyUploadsWorker(bool announce) {
    if (Endpoint().empty()) {
        SetStatus("Community endpoint is not configured");
        return;
    }
    if (announce) SetStatus("Refreshing my uploads...");
    CommunityHttpResponse response;
    if (!CommunityHttp_Request("GET", UrlFor("/api/v1/me/presets"), JsonHeaders(), "", response, g_communityWorkerStop)) {
        SetStatus("My uploads refresh failed: " + response.error);
        return;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        SetStatus(StatusHttpError("My uploads refresh failed", response));
        return;
    }
    std::vector<CommunityMyUpload> parsed;
    if (!ParseMyUploads(response.body, parsed)) {
        SetStatus("My uploads parse failed");
        return;
    }
    if (g_communityWorkerContext) {
        const unsigned long long generation = g_communityWorkerGeneration;
        PostCompletion(generation, [parsed = std::move(parsed), announce]() mutable {
            {
                std::lock_guard<std::mutex> lock(g_communityMutex);
                g_myUploads.swap(parsed);
            }
            if (announce) SetStatus("My uploads refreshed");
        }, true);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        g_myUploads.swap(parsed);
    }
    if (announce) SetStatus("My uploads refreshed");
}

void LoadCachedCatalog() {
    const std::string cached = ReadFileText(CatalogCachePath());
    if (!cached.empty()) {
        StoreCatalog(cached, false);
    }
}

} // namespace

void Community_EnsureInitialized() {
    if (g_communityStopping.load()) return;
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true)) return;
    LoadCommunityState();
    EnsureDirectory(CommunityRoot());
    LoadCachedCatalog();
    if (g_cfg.communityEnabled &&
        !Endpoint().empty() &&
        (static_cast<unsigned long long>(time(nullptr)) > g_lastCatalogRefresh.load() + kAutoRefreshSeconds)) {
        Community_RequestRefresh(false);
    }
}

void Community_Tick() {
    Community_EnsureInitialized();
    g_communityCompletions.Drain(g_communityGeneration.load());
}

void Community_BeginShutdown() {
    std::lock_guard<std::mutex> lifecycleLock(g_communityLifecycleMutex);
    if (g_communityStopping.exchange(true)) return;
    g_communityGeneration.fetch_add(1);
    BackgroundWorkQueue* queue = g_detachWorkQueue.load();
    if (queue) queue->StopAcceptingWork();
}

bool Community_WaitForShutdown(unsigned long waitMilliseconds) {
    BackgroundWorkQueue* queue = nullptr;
    {
        std::lock_guard<std::mutex> lifecycleLock(g_communityLifecycleMutex);
        queue = g_detachWorkQueue.load();
    }
    return !queue || queue->WaitForStop(waitMilliseconds);
}

void Community_SignalStopWithoutWait() noexcept {
    if (g_communityStopping.exchange(true)) return;
    g_communityGeneration.fetch_add(1);
    BackgroundWorkQueue* queue = g_detachWorkQueue.load();
    if (queue) queue->SignalStopWithoutLock();
}

void Community_CloseAfterShutdown() {
    {
        std::lock_guard<std::mutex> lifecycleLock(g_communityLifecycleMutex);
        BackgroundWorkQueue* queue = g_detachWorkQueue.load();
        if (queue) queue->CloseAfterStop();
    }
    g_communityCompletions.Clear();
}

bool Community_IsEnabled() {
    return g_cfg.communityEnabled;
}

bool Community_IsBusy() {
    std::lock_guard<std::mutex> lifecycleLock(g_communityLifecycleMutex);
    if (g_communityStopping.load()) return false;
    BackgroundWorkQueue& queue = CommunityWorkQueue();
    if (g_communityStopping.load()) {
        queue.SignalStopWithoutLock();
        return false;
    }
    return queue.OutstandingCount() > 0;
}

const char* Community_GetStatusText() {
    static thread_local std::string statusText;
    std::lock_guard<std::mutex> lock(g_communityMutex);
    statusText = g_communityStatusText;
    return statusText.c_str();
}

const char* Community_GetEndpoint() {
    static thread_local std::string endpointText;
    LoadCommunityState();
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        endpointText = g_endpoint;
    }
    return endpointText.c_str();
}

void Community_SetEndpoint(const char* endpoint) {
    LoadCommunityState();
#if defined(CW_DEV_BUILD)
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        g_endpoint = TrimCopy(endpoint ? endpoint : "");
        g_communityGeneration.fetch_add(1);
    }
    SaveCommunityState();
#else
    (void)endpoint;
    SetStatus("Community endpoint is configured at build time");
#endif
}

int Community_GetCatalogCount() {
    std::lock_guard<std::mutex> lock(g_communityMutex);
    return static_cast<int>(g_catalog.size());
}

CommunityCatalogItem Community_GetCatalogItem(int index) {
    std::lock_guard<std::mutex> lock(g_communityMutex);
    if (index < 0 || index >= static_cast<int>(g_catalog.size())) return CommunityCatalogItem{};
    return g_catalog[index];
}

int Community_GetMyUploadCount() {
    std::lock_guard<std::mutex> lock(g_communityMutex);
    return static_cast<int>(g_myUploads.size());
}

CommunityMyUpload Community_GetMyUploadItem(int index) {
    std::lock_guard<std::mutex> lock(g_communityMutex);
    if (index < 0 || index >= static_cast<int>(g_myUploads.size())) return CommunityMyUpload{};
    return g_myUploads[index];
}

CommunityPresetUpdateStatus Community_GetPresetUpdateStatus(int presetIndex) {
    CommunityPresetUpdateStatus status{};
    CommunityPresetInstallInfo installInfo{};
    if (!Preset_GetCommunityInstallInfo(presetIndex, installInfo)) {
        return status;
    }
    status.knownCommunityPreset = true;
    status.catalogId = installInfo.catalogId;
    status.installedSha256 = installInfo.sha256;

    std::vector<CommunityCatalogItem> catalogSnapshot;
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        catalogSnapshot = g_catalog;
    }
    for (const CommunityCatalogItem& item : catalogSnapshot) {
        const std::string expectedDisplayName = item.title + " by " + item.author;
        const bool idMatches = !installInfo.catalogId.empty() && _stricmp(item.id.c_str(), installInfo.catalogId.c_str()) == 0;
        const bool legacyNameMatches = installInfo.catalogId.empty() && _stricmp(expectedDisplayName.c_str(), installInfo.displayName.c_str()) == 0;
        if (idMatches || legacyNameMatches) {
            status.catalogItem = item;
            status.catalogId = item.id;
            if (!installInfo.sha256.empty()) {
                status.updateAvailable =
                    !item.sha256.empty() &&
                    _stricmp(installInfo.sha256.c_str(), item.sha256.c_str()) != 0;
            } else if (!installInfo.fullPath.empty() && !item.sha256.empty()) {
                std::string localHash;
                const std::string localText = ReadFileText(installInfo.fullPath);
                status.updateAvailable = !localText.empty() &&
                    Sha256Hex(localText, localHash) &&
                    _stricmp(localHash.c_str(), item.sha256.c_str()) != 0;
                status.installedSha256 = localHash;
            }
            break;
        }
    }
    return status;
}

bool Community_CanManualRefresh() {
    return GetTickCount64() > g_lastManualRefreshTick.load() + kManualRefreshCooldownSeconds * 1000ull;
}

void Community_RequestRefresh(bool manual) {
    if (manual && !Community_CanManualRefresh()) {
        SetStatus("Community refresh is cooling down");
        return;
    }
    RunAsync([manual]() { RefreshWorker(manual); }, false, "catalog-refresh");
}

bool Community_RequestInitialViewRefresh() {
    const bool refreshStarted = RunAsync([]() { RefreshWorker(false); }, true, "catalog-refresh");
    const bool uploadsStarted = RunAsync([]() { MyUploadsWorker(false); }, true, "my-uploads");
    return refreshStarted || uploadsStarted;
}

void Community_RequestMyUploads() {
    RunAsync([]() { MyUploadsWorker(true); }, false, "my-uploads");
}

void Community_RequestDownload(const char* presetId) {
    const std::string id = presetId ? presetId : "";
    if (id.empty()) return;
    CommunityCatalogItem item;
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        for (const CommunityCatalogItem& candidate : g_catalog) {
            if (candidate.id == id) {
                item = candidate;
                break;
            }
        }
    }
    if (item.id.empty()) {
        SetStatus("Community preset not found in catalog");
        return;
    }
    RunAsync([item]() {
        if (Endpoint().empty()) {
            SetStatus("Community endpoint is not configured");
            return;
        }
        SetStatus("Downloading community preset...");
        CommunityHttpResponse response;
        if (!CommunityHttp_Request("GET", UrlFor("/api/v1/presets/" + item.id + "/download"), JsonHeaders(), "", response, g_communityWorkerStop)) {
            SetStatus("Download failed: " + response.error);
            return;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            SetStatus(StatusHttpError("Download failed", response));
            return;
        }
        if (item.sha256.empty()) {
            SetStatus("Download rejected: missing SHA-256");
            return;
        }
        if (item.sizeBytes <= 0 || item.sizeBytes > static_cast<int>(kMaxDownloadBytes)) {
            SetStatus("Download rejected: invalid catalog size");
            return;
        }
        if (response.body.size() > kMaxDownloadBytes || static_cast<int>(response.body.size()) != item.sizeBytes) {
            SetStatus("Download rejected: size mismatch");
            return;
        }
        std::string hash;
        if (!Sha256Hex(response.body, hash) || _stricmp(hash.c_str(), item.sha256.c_str()) != 0) {
            SetStatus("Download rejected: SHA-256 mismatch");
            return;
        }
        const unsigned long long generation = g_communityWorkerGeneration;
        PostCompletion(generation, [item, presetText = std::move(response.body)]() {
            std::string fileName;
            std::string error;
            if (!Preset_ImportCommunityPresetText(
                    item.title.c_str(),
                    item.author.c_str(),
                    item.id.c_str(),
                    item.sha256.c_str(),
                    item.updatedAt.c_str(),
                    presetText.c_str(),
                    fileName,
                    error)) {
                SetStatus("Import failed: " + error);
                return;
            }
            SetStatus("Downloaded community preset: " + fileName);
        }, true);
    });
}

void Community_RequestUpdateDownloadedPreset(int presetIndex) {
    CommunityPresetUpdateStatus update = Community_GetPresetUpdateStatus(presetIndex);
    if (!update.updateAvailable || update.catalogItem.id.empty()) {
        SetStatus("Community preset is already up to date");
        return;
    }
    const CommunityCatalogItem item = update.catalogItem;
    CommunityPresetInstallInfo installInfo{};
    if (!Preset_GetCommunityInstallInfo(presetIndex, installInfo) || installInfo.fullPath.empty()) {
        SetStatus("Community preset is no longer available");
        return;
    }
    const std::string presetPath = installInfo.fullPath;
    RunAsync([presetPath, item]() {
        if (Endpoint().empty()) {
            SetStatus("Community endpoint is not configured");
            return;
        }
        SetStatus("Updating community preset...");
        CommunityHttpResponse response;
        if (!CommunityHttp_Request("GET", UrlFor("/api/v1/presets/" + item.id + "/download"), JsonHeaders(), "", response, g_communityWorkerStop)) {
            SetStatus("Update failed: " + response.error);
            return;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            SetStatus(StatusHttpError("Update failed", response));
            return;
        }
        if (item.sha256.empty()) {
            SetStatus("Update rejected: missing SHA-256");
            return;
        }
        if (item.sizeBytes <= 0 || item.sizeBytes > static_cast<int>(kMaxDownloadBytes)) {
            SetStatus("Update rejected: invalid catalog size");
            return;
        }
        if (response.body.size() > kMaxDownloadBytes || static_cast<int>(response.body.size()) != item.sizeBytes) {
            SetStatus("Update rejected: size mismatch");
            return;
        }
        std::string hash;
        if (!Sha256Hex(response.body, hash) || _stricmp(hash.c_str(), item.sha256.c_str()) != 0) {
            SetStatus("Update rejected: SHA-256 mismatch");
            return;
        }
        const unsigned long long generation = g_communityWorkerGeneration;
        PostCompletion(generation, [presetPath, item, presetText = std::move(response.body)]() {
            std::string error;
            if (!Preset_UpdateCommunityPresetTextByPath(
                    presetPath.c_str(),
                    item.title.c_str(),
                    item.author.c_str(),
                    item.id.c_str(),
                    item.sha256.c_str(),
                    item.updatedAt.c_str(),
                    presetText.c_str(),
                    error)) {
                SetStatus("Update failed: " + error);
                return;
            }
            SetStatus("Community preset updated: " + item.title);
        }, true);
    });
}

void Community_RequestLike(const char* presetId) {
    const std::string id = presetId ? presetId : "";
    if (id.empty()) return;
    if (Endpoint().empty()) {
        SetStatus("Community endpoint is not configured");
        return;
    }
    RunAsync([id]() {
        if (Endpoint().empty()) {
            SetStatus("Community endpoint is not configured");
            return;
        }
        CommunityHttpResponse response;
        if (!CommunityHttp_Request("POST", UrlFor("/api/v1/presets/" + id + "/like"), JsonHeaders(), "", response, g_communityWorkerStop)) {
            SetStatus("Like failed: " + response.error);
            return;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            SetStatus(StatusHttpError("Like failed", response));
            return;
        }
        bool liked = false;
        int likes = 0;
        if (!community_protocol::ParseLikeResponse(response.body, liked, likes)) {
            SetStatus("Like failed: invalid response");
            return;
        }
        const unsigned long long generation = g_communityWorkerGeneration;
        PostCompletion(generation, [id, liked, likes]() {
            {
                std::lock_guard<std::mutex> lock(g_communityMutex);
                SetLikedPresetId(id, liked);
                for (CommunityCatalogItem& item : g_catalog) {
                    if (_stricmp(item.id.c_str(), id.c_str()) == 0) {
                        item.liked = liked;
                        item.likes = likes;
                        break;
                    }
                }
            }
            SaveCommunityState();
            Log("[community] like toggled for %s\n", id.c_str());
        }, true);
    });
}

void Community_RequestDeleteMyUpload(const char* presetId) {
    const std::string id = TrimCopy(presetId ? presetId : "");
    if (id.empty()) return;
    if (Endpoint().empty()) {
        SetStatus("Community endpoint is not configured");
        return;
    }
    RunAsync([id]() {
        SetStatus("Deleting community upload...");
        CommunityHttpResponse response;
        if (!CommunityHttp_Request("DELETE", UrlFor("/api/v1/me/presets/" + id), JsonHeaders(), "", response, g_communityWorkerStop)) {
            SetStatus("Delete failed: " + response.error);
            return;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            SetStatus(StatusHttpError("Delete failed", response));
            return;
        }
        SetStatus("Community upload deleted");
        MyUploadsWorker(false);
        RefreshWorker(false);
    });
}

void Community_RequestCancelMyUploadUpdate(const char* presetId) {
    const std::string id = TrimCopy(presetId ? presetId : "");
    if (id.empty()) return;
    if (Endpoint().empty()) {
        SetStatus("Community endpoint is not configured");
        return;
    }
    RunAsync([id]() {
        SetStatus("Cancelling community update...");
        CommunityHttpResponse response;
        if (!CommunityHttp_Request("DELETE", UrlFor("/api/v1/me/presets/" + id + "/update"), JsonHeaders(), "", response, g_communityWorkerStop)) {
            SetStatus("Cancel update failed: " + response.error);
            return;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            SetStatus(StatusHttpError("Cancel update failed", response));
            return;
        }
        SetStatus("Community update cancelled");
        MyUploadsWorker(false);
        RefreshWorker(false);
    });
}

void Community_RequestUpdateMyUpload(
    const char* presetId,
    const char* title,
    const char* author,
    const char* description,
    int presetIndex) {
    const std::string id = TrimCopy(presetId ? presetId : "");
    const std::string submitTitle = TrimCopy(title ? title : "");
    const std::string submitAuthor = TrimCopy(author ? author : "");
    const std::string submitDescription = TrimCopy(description ? description : "");
    if (id.empty()) return;
    if (submitTitle.empty()) {
        SetStatus("Community update needs a title");
        return;
    }
    if (presetIndex < 0 || presetIndex >= Preset_GetCount()) {
        SetStatus("Community update needs a preset");
        return;
    }
    std::string presetPath;
    std::string ini;
    std::string exportError;
    if (!Preset_GetFilePath(presetIndex, presetPath) ||
        !Preset_ExportPresetCanonicalByIndex(presetIndex, ini, exportError)) {
        SetStatus("Update failed: " + (exportError.empty() ? std::string("Selected preset is not available") : exportError));
        return;
    }
    if (Endpoint().empty()) {
        SetStatus("Community endpoint is not configured");
        return;
    }
    SetStatus("Preparing community update...");
    RunAsync([id, submitTitle, submitAuthor, submitDescription, presetPath, ini = std::move(ini)]() {
        SetStatus("Uploading community update...");
        CommunityHttpResponse response;
        const std::string body = BuildPresetUploadBody(submitTitle, submitAuthor, submitDescription, ini);
        if (!CommunityHttp_Request("PUT", UrlFor("/api/v1/me/presets/" + id), JsonHeaders(), body, response, g_communityWorkerStop)) {
            SetStatus("Update failed: " + response.error);
            return;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            SetStatus(StatusHttpError("Update failed", response));
            return;
        }
        SetStatus("Community update submitted");
        Log("[community] update uploaded from %s to id=%s\n", presetPath.c_str(), id.c_str());
        MyUploadsWorker(false);
        RefreshWorker(false);
    });
}

void Community_RequestSubmit(
    const char* title,
    const char* author,
    const char* description) {
    const std::string submitTitle = TrimCopy(title ? title : "");
    const std::string submitAuthor = TrimCopy(author ? author : "");
    const std::string submitDescription = TrimCopy(description ? description : "");
    if (submitTitle.empty()) {
        SetStatus("Community submission needs a title");
        return;
    }
    if (Endpoint().empty()) {
        SetStatus("Community endpoint is not configured");
        return;
    }
    std::string ini;
    std::string exportError;
    if (!Preset_ExportCurrentCanonical(ini, exportError)) {
        SetStatus("Submit failed: " + exportError);
        Log("[community] submit export failed: %s\n", exportError.c_str());
        return;
    }
    std::string sourcePath;
    (void)Preset_GetFilePath(Preset_GetSelectedIndex(), sourcePath);
    SetStatus("Preparing community submission...");
    Log("[community] submit requested title=\"%s\"\n", submitTitle.c_str());
    RunAsync([submitTitle, submitAuthor, submitDescription, sourcePath, ini = std::move(ini)]() {
        if (Endpoint().empty()) {
            SetStatus("Community endpoint is not configured");
            return;
        }
        SetStatus("Uploading community preset...");
        const std::string body = BuildPresetUploadBody(submitTitle, submitAuthor, submitDescription, ini);

        CommunityHttpResponse response;
        if (!CommunityHttp_Request("POST", UrlFor("/api/v1/presets"), JsonHeaders(), body, response, g_communityWorkerStop)) {
            SetStatus("Submit failed: " + response.error);
            Log("[community] submit request failed: %s\n", response.error.c_str());
            return;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            SetStatus(StatusHttpError("Submit failed", response));
            Log("[community] submit rejected HTTP %lu: %s\n", response.statusCode, response.body.c_str());
            return;
        }
        SetStatus("Community preset submitted for approval");
        if (!sourcePath.empty()) Log("[community] submitted preset snapshot from %s\n", sourcePath.c_str());
        MyUploadsWorker(false);
        Log("[community] submit accepted: %s\n", response.body.c_str());
    });
}
