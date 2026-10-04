#include "pch.h"
#include "community_protocol.h"
#include "mod_metadata.h"
#include "../third_party/nlohmann/json.hpp"

#include <limits>

namespace community_protocol {
namespace {

using Json = nlohmann::json;

Json ParseObject(const std::string& text) {
    Json value = Json::parse(text, nullptr, false);
    return value.is_object() ? std::move(value) : Json::object();
}

std::string StringMember(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int IntMember(const Json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) return 0;
    if (it->is_number_unsigned()) {
        const auto value = it->get<uint64_t>();
        return value <= static_cast<uint64_t>(std::numeric_limits<int>::max()) ? static_cast<int>(value) : 0;
    }
    const auto value = it->get<int64_t>();
    return value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max()
        ? static_cast<int>(value) : 0;
}

bool BoolMember(const Json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

std::vector<std::string> StringArrayMember(const Json& object, const char* key) {
    std::vector<std::string> values;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array()) return values;
    for (const Json& value : *it) {
        if (value.is_string()) values.push_back(value.get<std::string>());
    }
    return values;
}

bool ParsePresetArray(const std::string& text, Json& rows) {
    Json root = Json::parse(text, nullptr, false);
    if (!root.is_object()) return false;
    const auto it = root.find("presets");
    if (it == root.end() || !it->is_array()) return false;
    rows = std::move(*it);
    return true;
}

bool IsLikedPresetIdIn(const std::vector<std::string>& likedIds, const std::string& id) {
    for (const std::string& liked : likedIds) {
        if (_stricmp(liked.c_str(), id.c_str()) == 0) return true;
    }
    return false;
}

} // namespace

std::string JsonEscape(const std::string& value) {
    std::string quoted = Json(value).dump(-1, ' ', false, Json::error_handler_t::replace);
    return quoted.substr(1, quoted.size() - 2);
}

std::string ExtractJsonString(const std::string& object, const char* key) {
    return StringMember(ParseObject(object), key);
}

int ExtractJsonInt(const std::string& object, const char* key) {
    return IntMember(ParseObject(object), key);
}

bool ExtractJsonBool(const std::string& object, const char* key) {
    return BoolMember(ParseObject(object), key);
}

bool ParseCatalog(const std::string& json, const std::vector<std::string>& likedPresetIds, std::vector<CommunityCatalogItem>& out) {
    Json rows;
    if (!ParsePresetArray(json, rows)) return false;
    std::vector<CommunityCatalogItem> parsed;
    for (const Json& object : rows) {
        if (!object.is_object()) continue;
        CommunityCatalogItem item;
        item.id = StringMember(object, "id");
        item.title = StringMember(object, "title");
        if (item.id.empty() || item.title.empty()) continue;
        item.author = StringMember(object, "author");
        item.description = StringMember(object, "description");
        item.tags = StringArrayMember(object, "tags");
        item.updatedAt = StringMember(object, "updatedAt");
        item.downloads = IntMember(object, "downloads");
        item.likes = IntMember(object, "likes");
        item.liked = IsLikedPresetIdIn(likedPresetIds, item.id);
        const auto file = object.find("file");
        if (file != object.end() && file->is_object()) {
            item.sha256 = StringMember(*file, "sha256");
            item.sizeBytes = IntMember(*file, "size");
        }
        parsed.push_back(std::move(item));
    }
    out.swap(parsed);
    return true;
}

bool ParseMyUploads(const std::string& json, std::vector<CommunityMyUpload>& out) {
    Json rows;
    if (!ParsePresetArray(json, rows)) return false;
    std::vector<CommunityMyUpload> parsed;
    for (const Json& object : rows) {
        if (!object.is_object()) continue;
        CommunityMyUpload item;
        item.id = StringMember(object, "id");
        if (item.id.empty()) continue;
        item.title = StringMember(object, "title");
        item.author = StringMember(object, "author_name");
        item.description = StringMember(object, "description");
        item.status = StringMember(object, "status");
        item.updateOf = StringMember(object, "update_of");
        item.pendingUpdateId = StringMember(object, "pending_update_id");
        item.pendingUpdateTitle = StringMember(object, "pending_update_title");
        item.pendingUpdateAt = StringMember(object, "pending_update_at");
        item.updatedAt = StringMember(object, "updated_at");
        item.downloads = IntMember(object, "downloads");
        item.likes = IntMember(object, "likes");
        parsed.push_back(std::move(item));
    }
    out.swap(parsed);
    return true;
}

bool ParseLikeResponse(const std::string& json, bool& liked, int& likes) {
    const Json root = Json::parse(json, nullptr, false);
    if (!root.is_object()) return false;
    const auto likedValue = root.find("liked");
    const auto likesValue = root.find("likes");
    if (likedValue == root.end() || !likedValue->is_boolean() ||
        likesValue == root.end() || !likesValue->is_number_integer()) return false;
    const int parsedLikes = IntMember(root, "likes");
    if (parsedLikes < 0 || (parsedLikes == 0 && *likesValue != 0)) return false;
    liked = likedValue->get<bool>();
    likes = parsedLikes;
    return true;
}

std::string BuildPresetUploadBody(
    const std::string& title,
    const std::string& author,
    const std::string& description,
    const std::string& ini) {
    return Json{
        { "title", title },
        { "authorName", author.empty() ? "Anonymous" : author },
        { "description", description },
        { "tags", Json::array() },
        { "clientVersion", MOD_VERSION },
        { "iniText", ini },
    }.dump(-1, ' ', false, Json::error_handler_t::replace);
}

} // namespace community_protocol
