#include "pch.h"

#include "community_service.h"

#include "community_endpoint_config.h"
#include "community_http.h"
#include "preset_service.h"
#include "runtime_shared.h"

#include <bcrypt.h>

#include <atomic>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <ctime>

namespace {

constexpr unsigned long long kAutoRefreshSeconds = 24ull * 60ull * 60ull;
constexpr unsigned long long kManualRefreshCooldownSeconds = 60ull;
constexpr size_t kMaxDownloadBytes = 65536;
constexpr size_t kMaxStatusBodyChars = 180;

std::mutex g_communityMutex;
std::vector<CommunityCatalogItem> g_catalog;
std::vector<CommunityMyUpload> g_myUploads;
std::vector<std::string> g_likedPresetIds;
std::string g_communityStatusText = "Community presets idle";
std::string g_endpoint;
std::string g_clientId;
std::atomic<unsigned long long> g_lastCatalogRefresh{ 0 };
bool g_initialized = false;
std::once_flag g_stateLoadOnce;
int g_busyCount = 0;
std::atomic<bool> g_shutdownRequested{ false };
std::atomic<unsigned long long> g_lastManualRefreshTick{ 0 };

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
    std::lock_guard<std::mutex> lock(g_communityMutex);
    g_communityStatusText = status;
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

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 32) out += ' ';
            else out += c;
            break;
        }
    }
    return out;
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

bool IsLikedPresetIdIn(const std::vector<std::string>& likedIds, const std::string& id) {
    for (const std::string& liked : likedIds) {
        if (_stricmp(liked.c_str(), id.c_str()) == 0) return true;
    }
    return false;
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

int ExtractJsonInt(const std::string& object, const char* key) {
    const std::string marker = std::string("\"") + key + "\"";
    size_t pos = object.find(marker);
    if (pos == std::string::npos) return 0;
    pos = object.find(':', pos + marker.size());
    if (pos == std::string::npos) return 0;
    return atoi(object.c_str() + pos + 1);
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

std::vector<std::string> ExtractJsonStringArray(const std::string& object, const char* key) {
    std::vector<std::string> out;
    const std::string marker = std::string("\"") + key + "\"";
    size_t pos = object.find(marker);
    if (pos == std::string::npos) return out;
    pos = object.find('[', pos + marker.size());
    const size_t end = object.find(']', pos);
    if (pos == std::string::npos || end == std::string::npos) return out;
    std::string arr = object.substr(pos, end - pos + 1);
    size_t cursor = 0;
    while ((cursor = arr.find('"', cursor)) != std::string::npos) {
        std::string value;
        bool escape = false;
        size_t i = cursor + 1;
        for (; i < arr.size(); ++i) {
            const char c = arr[i];
            if (escape) {
                switch (c) {
                case 'n': value += '\n'; break;
                case 'r': value += '\r'; break;
                case 't': value += '\t'; break;
                default: value += c; break;
                }
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            if (c == '"') break;
            value += c;
        }
        if (i >= arr.size()) break;
        out.push_back(value);
        cursor = i + 1;
    }
    return out;
}

std::string ExtractNestedObject(const std::string& object, const char* key) {
    const std::string marker = std::string("\"") + key + "\"";
    size_t pos = object.find(marker);
    if (pos == std::string::npos) return {};
    pos = object.find('{', pos + marker.size());
    if (pos == std::string::npos) return {};
    int depth = 0;
    for (size_t i = pos; i < object.size(); ++i) {
        if (object[i] == '{') ++depth;
        else if (object[i] == '}') {
            --depth;
            if (depth == 0) return object.substr(pos, i - pos + 1);
        }
    }
    return {};
}

std::vector<std::string> ExtractPresetObjects(const std::string& json) {
    std::vector<std::string> objects;
    const size_t marker = json.find("\"presets\"");
    if (marker == std::string::npos) return objects;
    size_t pos = json.find('[', marker);
    if (pos == std::string::npos) return objects;
    bool inString = false;
    bool escape = false;
    int depth = 0;
    size_t objectStart = std::string::npos;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\' && inString) {
            escape = true;
            continue;
        }
        if (c == '"') {
            inString = !inString;
            continue;
        }
        if (inString) continue;
        if (c == '{') {
            if (depth == 0) objectStart = pos;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(json.substr(objectStart, pos - objectStart + 1));
                objectStart = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    return objects;
}

bool ParseCatalog(const std::string& json, const std::vector<std::string>& likedPresetIds, std::vector<CommunityCatalogItem>& out) {
    std::vector<CommunityCatalogItem> parsed;
    for (const std::string& object : ExtractPresetObjects(json)) {
        CommunityCatalogItem item;
        item.id = ExtractJsonString(object, "id");
        item.title = ExtractJsonString(object, "title");
        item.author = ExtractJsonString(object, "author");
        item.description = ExtractJsonString(object, "description");
        item.tags = ExtractJsonStringArray(object, "tags");
        item.updatedAt = ExtractJsonString(object, "updatedAt");
        item.downloads = ExtractJsonInt(object, "downloads");
        item.likes = ExtractJsonInt(object, "likes");
        item.liked = IsLikedPresetIdIn(likedPresetIds, item.id);
        const std::string file = ExtractNestedObject(object, "file");
        item.sha256 = ExtractJsonString(file, "sha256");
        item.sizeBytes = ExtractJsonInt(file, "size");
        if (!item.id.empty() && !item.title.empty()) {
            parsed.push_back(item);
        }
    }
    if (parsed.empty() && json.find("\"presets\"") != std::string::npos) {
        out.clear();
        return true;
    }
    if (parsed.empty()) return false;
    out.swap(parsed);
    return true;
}

bool ParseMyUploads(const std::string& json, std::vector<CommunityMyUpload>& out) {
    std::vector<CommunityMyUpload> parsed;
    for (const std::string& object : ExtractPresetObjects(json)) {
        CommunityMyUpload item;
        item.id = ExtractJsonString(object, "id");
        item.title = ExtractJsonString(object, "title");
        item.author = ExtractJsonString(object, "author_name");
        item.description = ExtractJsonString(object, "description");
        item.status = ExtractJsonString(object, "status");
        item.updateOf = ExtractJsonString(object, "update_of");
        item.updatedAt = ExtractJsonString(object, "updated_at");
        item.downloads = ExtractJsonInt(object, "downloads");
        item.likes = ExtractJsonInt(object, "likes");
        if (!item.id.empty()) {
            parsed.push_back(item);
        }
    }
    if (parsed.empty() && json.find("\"presets\"") != std::string::npos) {
        out.clear();
        return true;
    }
    if (parsed.empty()) return false;
    out.swap(parsed);
    return true;
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

std::string Endpoint() {
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

void FinishAsyncWork() {
    std::lock_guard<std::mutex> lock(g_communityMutex);
    if (g_busyCount > 0) {
        --g_busyCount;
    }
}

bool RunAsync(const std::function<void()>& work, bool allowParallel = false) {
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        if (g_shutdownRequested.load()) return false;
        if (!allowParallel && g_busyCount > 0) return false;
        ++g_busyCount;
    }
    std::thread([work]() {
        if (!g_shutdownRequested.load()) {
            work();
        }
        FinishAsyncWork();
    }).detach();
    return true;
}

void RefreshWorker(bool manual) {
    if (Endpoint().empty()) {
        SetStatus("Community endpoint is not configured");
        return;
    }
    SetStatus("Refreshing community catalog...");
    CommunityHttpResponse response;
    if (!CommunityHttp_Request("GET", UrlFor("/api/v1/catalog"), JsonHeaders(), "", response)) {
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
    if (!CommunityHttp_Request("GET", UrlFor("/api/v1/me/presets"), JsonHeaders(), "", response)) {
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
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        g_myUploads.swap(parsed);
    }
    if (announce) SetStatus("My uploads refreshed");
}

std::string BuildPresetUploadBody(
    const std::string& title,
    const std::string& author,
    const std::string& description,
    const std::string& ini) {
    std::string body = "{";
    body += "\"title\":\"" + JsonEscape(title) + "\",";
    body += "\"authorName\":\"" + JsonEscape(author.empty() ? "Anonymous" : author) + "\",";
    body += "\"description\":\"" + JsonEscape(description) + "\",";
    body += "\"tags\":[],";
    body += "\"clientVersion\":\"" MOD_VERSION "\",";
    body += "\"iniText\":\"" + JsonEscape(ini) + "\"}";
    return body;
}

void LoadCachedCatalog() {
    const std::string cached = ReadFileText(CatalogCachePath());
    if (!cached.empty()) {
        StoreCatalog(cached, false);
    }
}

} // namespace

void Community_EnsureInitialized() {
    if (g_initialized) return;
    g_initialized = true;
    LoadCommunityState();
    EnsureDirectory(CommunityRoot());
    LoadCachedCatalog();
    if (g_cfg.communityEnabled &&
        !Endpoint().empty() &&
        (static_cast<unsigned long long>(time(nullptr)) > g_lastCatalogRefresh.load() + kAutoRefreshSeconds)) {
        Community_RequestRefresh(false);
    }
}

void Community_Shutdown() {
    g_shutdownRequested.store(true);
    std::lock_guard<std::mutex> lock(g_communityMutex);
    g_busyCount = 0;
}

void Community_Tick() {
    Community_EnsureInitialized();
}

bool Community_IsEnabled() {
    return g_cfg.communityEnabled;
}

void Community_SetEnabled(bool enabled) {
    g_cfg.communityEnabled = enabled;
    SaveCommunityConfig();
}

bool Community_IsBusy() {
    std::lock_guard<std::mutex> lock(g_communityMutex);
    return g_busyCount > 0;
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
    }
    SaveCommunityState();
#else
    (void)endpoint;
    SetStatus("Community endpoint is configured at build time");
#endif
}

const char* Community_GetClientId() {
    static thread_local std::string clientIdText;
    LoadCommunityState();
    {
        std::lock_guard<std::mutex> lock(g_communityMutex);
        clientIdText = g_clientId;
    }
    return clientIdText.c_str();
}

unsigned long long Community_GetLastRefresh() {
    LoadCommunityState();
    return g_lastCatalogRefresh.load();
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
    RunAsync([manual]() { RefreshWorker(manual); });
}

bool Community_RequestInitialViewRefresh() {
    const bool refreshStarted = RunAsync([]() { RefreshWorker(false); }, true);
    const bool uploadsStarted = RunAsync([]() { MyUploadsWorker(false); }, true);
    return refreshStarted || uploadsStarted;
}

void Community_RequestMyUploads() {
    RunAsync([]() { MyUploadsWorker(true); });
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
        if (!CommunityHttp_Request("GET", UrlFor("/api/v1/presets/" + item.id + "/download"), JsonHeaders(), "", response)) {
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
        std::string fileName;
        std::string error;
        if (!Preset_ImportCommunityPresetText(
                item.title.c_str(),
                item.author.c_str(),
                item.id.c_str(),
                item.sha256.c_str(),
                item.updatedAt.c_str(),
                response.body.c_str(),
                fileName,
                error)) {
            SetStatus("Import failed: " + error);
            return;
        }
        SetStatus("Downloaded community preset: " + fileName);
    });
}

void Community_RequestUpdateDownloadedPreset(int presetIndex) {
    CommunityPresetUpdateStatus update = Community_GetPresetUpdateStatus(presetIndex);
    if (!update.updateAvailable || update.catalogItem.id.empty()) {
        SetStatus("Community preset is already up to date");
        return;
    }
    const CommunityCatalogItem item = update.catalogItem;
    RunAsync([presetIndex, item]() {
        if (Endpoint().empty()) {
            SetStatus("Community endpoint is not configured");
            return;
        }
        SetStatus("Updating community preset...");
        CommunityHttpResponse response;
        if (!CommunityHttp_Request("GET", UrlFor("/api/v1/presets/" + item.id + "/download"), JsonHeaders(), "", response)) {
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
        std::string error;
        if (!Preset_UpdateCommunityPresetText(
                presetIndex,
                item.title.c_str(),
                item.author.c_str(),
                item.id.c_str(),
                item.sha256.c_str(),
                item.updatedAt.c_str(),
                response.body.c_str(),
                error)) {
            SetStatus("Update failed: " + error);
            return;
        }
        SetStatus("Community preset updated: " + item.title);
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
        if (!CommunityHttp_Request("POST", UrlFor("/api/v1/presets/" + id + "/like"), JsonHeaders(), "", response)) {
            SetStatus("Like failed: " + response.error);
            return;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            SetStatus(StatusHttpError("Like failed", response));
            return;
        }
        const bool liked = ExtractJsonBool(response.body, "liked");
        const int likes = ExtractJsonInt(response.body, "likes");
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
        if (!CommunityHttp_Request("DELETE", UrlFor("/api/v1/me/presets/" + id), JsonHeaders(), "", response)) {
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
    if (Endpoint().empty()) {
        SetStatus("Community endpoint is not configured");
        return;
    }
    SetStatus("Preparing community update...");
    RunAsync([id, submitTitle, submitAuthor, submitDescription, presetIndex]() {
        std::string ini;
        std::string error;
        if (!Preset_ExportPresetCanonicalByIndex(presetIndex, ini, error)) {
            SetStatus("Update failed: " + error);
            return;
        }
        SetStatus("Uploading community update...");
        CommunityHttpResponse response;
        const std::string body = BuildPresetUploadBody(submitTitle, submitAuthor, submitDescription, ini);
        if (!CommunityHttp_Request("PUT", UrlFor("/api/v1/me/presets/" + id), JsonHeaders(), body, response)) {
            SetStatus("Update failed: " + response.error);
            return;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            SetStatus(StatusHttpError("Update failed", response));
            return;
        }
        SetStatus("Community update submitted");
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
    SetStatus("Preparing community submission...");
    Log("[community] submit requested title=\"%s\"\n", submitTitle.c_str());
    RunAsync([submitTitle, submitAuthor, submitDescription]() {
        if (Endpoint().empty()) {
            SetStatus("Community endpoint is not configured");
            return;
        }
        std::string ini;
        std::string error;
        if (!Preset_ExportCurrentCanonical(ini, error)) {
            SetStatus("Submit failed: " + error);
            Log("[community] submit export failed: %s\n", error.c_str());
            return;
        }
        SetStatus("Uploading community preset...");
        const std::string body = BuildPresetUploadBody(submitTitle, submitAuthor, submitDescription, ini);

        CommunityHttpResponse response;
        if (!CommunityHttp_Request("POST", UrlFor("/api/v1/presets"), JsonHeaders(), body, response)) {
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
        MyUploadsWorker(false);
        Log("[community] submit accepted: %s\n", response.body.c_str());
    });
}
