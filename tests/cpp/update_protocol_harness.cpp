#include "update/update_protocol.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <string>

int main() {
    UpdateCheckInfo info;
    info.downloadPageUrl = "https://fallback.invalid/";
    const std::string valid = R"json({
        "ok":true,"updateAvailable":true,"version":"0.8.0",
        "title":"A \"quoted\" release","changelog":"caf\u00e9\nchanges",
        "downloadPageUrl":"https://example.invalid/release",
        "addonDownloadUrl":"https://example.invalid/addon",
        "addonSha256":"abc","addonSizeBytes":4294967296
    })json";
    assert(update_protocol::ParseUpdateMetadata(valid, info));
    assert(info.updateAvailable && info.latestVersion == "0.8.0");
    assert(info.title == "A \"quoted\" release");
    assert(info.changelog == "caf\xC3\xA9\nchanges");
    assert(info.downloadPageUrl == "https://example.invalid/release");
    assert(info.addonSizeBytes == 4294967296LL);

    const UpdateCheckInfo previous = info;
    assert(!update_protocol::ParseUpdateMetadata(R"json({"version":"broken",]})json", info));
    assert(info.latestVersion == previous.latestVersion && info.addonSizeBytes == previous.addonSizeBytes);
    assert(!update_protocol::ParseUpdateMetadata(R"json([{"updateAvailable":true}])json", info));

    assert(update_protocol::ParseUpdateMetadata(R"json({
        "outer":{"version":"wrong","addonSizeBytes":42},
        "version":"right","updateAvailable":"true",
        "addonSizeBytes":18446744073709551615,
        "downloadPageUrl":null
    })json", info));
    assert(info.latestVersion == "right");
    assert(!info.updateAvailable && info.addonSizeBytes == 0);
    assert(info.downloadPageUrl == previous.downloadPageUrl);
    std::cout << "Update protocol harness passed\n";
}
