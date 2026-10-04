#include "community/community_protocol.h"
#include "third_party/nlohmann/json.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace community_protocol;
    std::vector<CommunityCatalogItem> catalog;
    const std::string valid = R"json({
        "schemaVersion":1,
        "presets":[{
            "id":"one","title":"A \"quoted\" title","author":"caf\u00e9",
            "description":"braces { } and \"file\":\"wrong\"",
            "tags":["rain","\u2603"],"downloads":7,"likes":3,
            "file":{"sha256":"correct","size":42}
        }]
    })json";
    assert(ParseCatalog(valid, { "ONE" }, catalog));
    assert(catalog.size() == 1);
    assert(catalog[0].title == "A \"quoted\" title");
    assert(catalog[0].author == "caf\xC3\xA9");
    assert(catalog[0].description == "braces { } and \"file\":\"wrong\"");
    assert(catalog[0].tags.size() == 2);
    assert(catalog[0].tags[1] == "\xE2\x98\x83");
    assert(catalog[0].liked && catalog[0].sha256 == "correct" && catalog[0].sizeBytes == 42);

    assert(!ParseCatalog(R"json({"presets":[{"id":"broken",]})json", {}, catalog));
    assert(catalog.size() == 1);
    assert(!ParseCatalog(R"json({"presets":{}})json", {}, catalog));
    assert(ParseCatalog(R"json({"presets":[]})json", {}, catalog));
    assert(catalog.empty());
    assert(ExtractJsonString(R"json({"outer":{"id":"wrong"},"id":"right"})json", "id") == "right");
    assert(ExtractJsonInt(R"json({"likes":2147483648})json", "likes") == 0);
    assert(ExtractJsonInt(R"json({"likes":"12"})json", "likes") == 0);
    assert(!ExtractJsonBool(R"json({"liked":"true"})json", "liked"));
    bool liked = false;
    int likes = -1;
    assert(ParseLikeResponse(R"json({"ok":true,"liked":true,"likes":8})json", liked, likes));
    assert(liked && likes == 8);
    assert(!ParseLikeResponse(R"json({"liked":"true","likes":0})json", liked, likes));
    assert(!ParseLikeResponse(R"json({"liked":false,"likes":2147483648})json", liked, likes));
    assert(liked && likes == 8);

    std::vector<CommunityMyUpload> uploads;
    assert(ParseMyUploads(R"json({"presets":[{"id":"mine","author_name":"Owner","pending_update_id":null}]})json", uploads));
    assert(uploads.size() == 1 && uploads[0].author == "Owner" && uploads[0].pendingUpdateId.empty());
    assert(!ParseMyUploads(R"json({"presets":true})json", uploads));
    assert(uploads.size() == 1);

    const std::string body = BuildPresetUploadBody("Title", "", "quote \" and slash \\", "[Preset]\nname=caf\xC3\xA9");
    const auto uploaded = nlohmann::json::parse(body);
    assert(uploaded.at("title") == "Title");
    assert(uploaded.at("authorName") == "Anonymous");
    assert(uploaded.at("description") == "quote \" and slash \\");
    assert(uploaded.at("iniText") == "[Preset]\nname=caf\xC3\xA9");
    assert(uploaded.at("tags").is_array() && uploaded.at("tags").empty());
    std::cout << "Community protocol harness passed\n";
}
