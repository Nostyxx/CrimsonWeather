#include "pch.h"
#include "update_protocol.h"
#include "../third_party/nlohmann/json.hpp"

#include <limits>

namespace update_protocol {
namespace {

using Json = nlohmann::json;

std::string StringMember(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

long long SizeMember(const Json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) return 0;
    if (it->is_number_unsigned()) {
        const uint64_t value = it->get<uint64_t>();
        return value <= static_cast<uint64_t>(std::numeric_limits<long long>::max())
            ? static_cast<long long>(value) : 0;
    }
    const int64_t value = it->get<int64_t>();
    return value >= 0 ? value : 0;
}

} // namespace

bool ParseUpdateMetadata(const std::string& body, UpdateCheckInfo& info) {
    const Json root = Json::parse(body, nullptr, false);
    if (!root.is_object()) return false;

    UpdateCheckInfo parsed = info;
    const auto available = root.find("updateAvailable");
    parsed.updateAvailable = available != root.end() && available->is_boolean() && available->get<bool>();
    parsed.latestVersion = StringMember(root, "version");
    parsed.title = StringMember(root, "title");
    parsed.changelog = StringMember(root, "changelog");
    const std::string downloadPage = StringMember(root, "downloadPageUrl");
    if (!downloadPage.empty()) parsed.downloadPageUrl = downloadPage;
    parsed.addonDownloadUrl = StringMember(root, "addonDownloadUrl");
    parsed.addonSha256 = StringMember(root, "addonSha256");
    parsed.addonSizeBytes = SizeMember(root, "addonSizeBytes");
    info = std::move(parsed);
    return true;
}

} // namespace update_protocol
