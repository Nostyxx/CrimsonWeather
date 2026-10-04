#pragma once
#include "community_models.h"
#include <string>
#include <vector>

namespace community_protocol {
std::string JsonEscape(const std::string& value);
std::string ExtractJsonString(const std::string& object, const char* key);
int ExtractJsonInt(const std::string& object, const char* key);
bool ExtractJsonBool(const std::string& object, const char* key);
bool ParseCatalog(const std::string& json, const std::vector<std::string>& likedPresetIds, std::vector<CommunityCatalogItem>& out);
bool ParseMyUploads(const std::string& json, std::vector<CommunityMyUpload>& out);
bool ParseLikeResponse(const std::string& json, bool& liked, int& likes);
std::string BuildPresetUploadBody(const std::string& title, const std::string& author, const std::string& description, const std::string& ini);
}
