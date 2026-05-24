#pragma once

#include <string>
#include <vector>

struct CommunityCatalogItem {
    std::string id;
    std::string title;
    std::string author;
    std::string description;
    std::vector<std::string> tags;
    std::string updatedAt;
    std::string sha256;
    int sizeBytes = 0;
    int downloads = 0;
    int likes = 0;
    bool liked = false;
};

struct CommunityMyUpload {
    std::string id;
    std::string title;
    std::string author;
    std::string description;
    std::string status;
    std::string updateOf;
    std::string pendingUpdateId;
    std::string pendingUpdateTitle;
    std::string pendingUpdateAt;
    std::string updatedAt;
    int downloads = 0;
    int likes = 0;
};

struct CommunityPresetUpdateStatus {
    bool knownCommunityPreset = false;
    bool updateAvailable = false;
    std::string catalogId;
    std::string installedSha256;
    CommunityCatalogItem catalogItem;
};

enum class CommunitySortMode {
    Recent = 0,
    Popular,
    MostDownloaded,
    MostLiked,
};
