#include "pch.h"

#include "community_ui.h"

#include "community_service.h"
#include "preset_service.h"
#include "runtime_shared.h"

#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace {

#if defined(CW_DEV_BUILD)
char g_endpointEdit[256] = {};
bool g_endpointInitialized = false;
#endif
char g_communityFilter[96] = {};
char g_myUploadsFilter[96] = {};
char g_submitTitle[96] = {};
char g_submitAuthor[64] = "";
char g_submitDescription[512] = {};
char g_updateId[128] = {};
char g_updateTitle[96] = {};
char g_updateAuthor[64] = {};
char g_updateDescription[512] = {};
int g_updatePresetIndex = -1;
char g_deleteId[128] = {};
char g_deleteTitle[96] = {};
int g_sortMode = static_cast<int>(CommunitySortMode::Recent);
bool g_openUpdatePopup = false;
bool g_openDeletePopup = false;
bool g_initialViewRefreshStarted = false;

bool ContainsNoCase(const std::string& haystack, const char* needleRaw) {
    if (!needleRaw || !needleRaw[0]) return true;
    std::string needle = needleRaw;
    auto lower = [](std::string value) {
        for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

bool ItemMatchesFilter(const CommunityCatalogItem& item) {
    if (!g_communityFilter[0]) return true;
    if (ContainsNoCase(item.title, g_communityFilter)) return true;
    if (ContainsNoCase(item.author, g_communityFilter)) return true;
    if (ContainsNoCase(item.description, g_communityFilter)) return true;
    return false;
}

bool MyUploadMatchesFilter(const CommunityMyUpload& item) {
    if (!g_myUploadsFilter[0]) return true;
    if (ContainsNoCase(item.title, g_myUploadsFilter)) return true;
    if (ContainsNoCase(item.author, g_myUploadsFilter)) return true;
    if (ContainsNoCase(item.description, g_myUploadsFilter)) return true;
    if (ContainsNoCase(item.status, g_myUploadsFilter)) return true;
    if (ContainsNoCase(item.pendingUpdateTitle, g_myUploadsFilter)) return true;
    return false;
}

void SortItems(std::vector<CommunityCatalogItem>& items) {
    switch (static_cast<CommunitySortMode>(g_sortMode)) {
    case CommunitySortMode::MostDownloaded:
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.downloads > b.downloads; });
        break;
    case CommunitySortMode::MostLiked:
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.likes > b.likes; });
        break;
    case CommunitySortMode::Popular:
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            return (a.downloads + a.likes * 5) > (b.downloads + b.likes * 5);
        });
        break;
    case CommunitySortMode::Recent:
    default:
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.updatedAt > b.updatedAt; });
        break;
    }
}

std::vector<CommunityCatalogItem> VisibleItems() {
    std::vector<CommunityCatalogItem> items;
    const int count = Community_GetCatalogCount();
    for (int i = 0; i < count; ++i) {
        CommunityCatalogItem item = Community_GetCatalogItem(i);
        if (ItemMatchesFilter(item)) {
            items.push_back(item);
        }
    }
    SortItems(items);
    return items;
}

void DrawEndpointConfig() {
#if defined(CW_DEV_BUILD)
    if (!g_endpointInitialized) {
        strcpy_s(g_endpointEdit, Community_GetEndpoint());
        g_endpointInitialized = true;
    }
    ImGui::SetNextItemWidth(-92.0f);
    if (ImGui::InputTextWithHint("##community_endpoint", "https://your-worker.example.workers.dev", g_endpointEdit, IM_ARRAYSIZE(g_endpointEdit))) {
    }
    ImGui::SameLine();
    if (ImGui::Button("Save##community_endpoint")) {
        Community_SetEndpoint(g_endpointEdit);
    }
#endif
}

void CopyText(char* dst, size_t dstSize, const std::string& src) {
    if (!dst || dstSize == 0) return;
    strncpy_s(dst, dstSize, src.c_str(), _TRUNCATE);
}

void DrawSubmitSection() {
    if (!ImGui::CollapsingHeader("Submit Current Preset##community_submit_section")) {
        return;
    }
    ImGui::InputTextWithHint("Title##community_submit_title", "Your preset name", g_submitTitle, IM_ARRAYSIZE(g_submitTitle));
    ImGui::InputTextWithHint("Author##community_submit_author", "Your name", g_submitAuthor, IM_ARRAYSIZE(g_submitAuthor));
    ImGui::InputTextMultiline("Notes##community_submit_notes", g_submitDescription, IM_ARRAYSIZE(g_submitDescription), ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 3.0f));
    const bool enabled = Community_IsEnabled();
    const bool busy = Community_IsBusy();
    const bool endpointReady = Community_GetEndpoint()[0] != '\0';
    const bool disabled = busy || !enabled || !endpointReady;
    if (disabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Submit Current Preset##community_submit_button", ImVec2(220.0f, 0.0f))) {
        Community_RequestSubmit(g_submitTitle, g_submitAuthor, g_submitDescription);
    }
    if (disabled) {
        ImGui::EndDisabled();
    }
}

void OpenUpdatePopup(const CommunityMyUpload& item) {
    CopyText(g_updateId, IM_ARRAYSIZE(g_updateId), item.id);
    CopyText(g_updateTitle, IM_ARRAYSIZE(g_updateTitle), item.title);
    CopyText(g_updateAuthor, IM_ARRAYSIZE(g_updateAuthor), item.author);
    CopyText(g_updateDescription, IM_ARRAYSIZE(g_updateDescription), item.description);
    g_updatePresetIndex = Preset_GetSelectedIndex();
    if (g_updatePresetIndex < 0 && Preset_GetCount() > 0) {
        g_updatePresetIndex = 0;
    }
    g_openUpdatePopup = true;
}

void OpenDeletePopup(const CommunityMyUpload& item) {
    CopyText(g_deleteId, IM_ARRAYSIZE(g_deleteId), item.id);
    CopyText(g_deleteTitle, IM_ARRAYSIZE(g_deleteTitle), item.title);
    g_openDeletePopup = true;
}

void DrawUpdatePopup() {
    if (ImGui::BeginPopupModal("Update Community Upload", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputTextWithHint("Title##community_update_title", "Preset title", g_updateTitle, IM_ARRAYSIZE(g_updateTitle));
        ImGui::InputTextWithHint("Author##community_update_author", "Your name", g_updateAuthor, IM_ARRAYSIZE(g_updateAuthor));
        ImGui::InputTextMultiline("Notes##community_update_notes", g_updateDescription, IM_ARRAYSIZE(g_updateDescription), ImVec2(520.0f, ImGui::GetTextLineHeightWithSpacing() * 3.0f));
        Preset_EnsureInitialized();
        const char* selectedPreset = (g_updatePresetIndex >= 0 && g_updatePresetIndex < Preset_GetCount())
            ? Preset_GetDisplayName(g_updatePresetIndex)
            : "Select preset...";
        ImGui::SetNextItemWidth(520.0f);
        if (ImGui::BeginCombo("Preset INI##community_update_preset", selectedPreset)) {
            for (int i = 0; i < Preset_GetCount(); ++i) {
                const bool selected = i == g_updatePresetIndex;
                if (ImGui::Selectable(Preset_GetDisplayName(i), selected)) {
                    g_updatePresetIndex = i;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        const bool disabled = Community_IsBusy() || g_updatePresetIndex < 0 || !g_updateTitle[0];
        if (disabled) ImGui::BeginDisabled();
        if (ImGui::Button("Submit Update", ImVec2(140.0f, 0.0f))) {
            Community_RequestUpdateMyUpload(g_updateId, g_updateTitle, g_updateAuthor, g_updateDescription, g_updatePresetIndex);
            ImGui::CloseCurrentPopup();
        }
        if (disabled) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawDeletePopup() {
    if (ImGui::BeginPopupModal("Delete Community Upload", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Delete \"%s\" from community presets?", g_deleteTitle);
        if (Community_IsBusy()) ImGui::BeginDisabled();
        if (ImGui::Button("Delete", ImVec2(100.0f, 0.0f))) {
            Community_RequestDeleteMyUpload(g_deleteId);
            ImGui::CloseCurrentPopup();
        }
        if (Community_IsBusy()) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void DrawMyUploadsSection() {
    if (!ImGui::CollapsingHeader("My Uploads##community_my_uploads_section")) {
        return;
    }
    ImGui::SetNextItemWidth(-88.0f);
    ImGui::InputTextWithHint("##community_my_uploads_filter", "Search...", g_myUploadsFilter, IM_ARRAYSIZE(g_myUploadsFilter));
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        Community_RequestMyUploads();
    }
    const float listHeight = min(260.0f, max(150.0f, ImGui::GetTextLineHeightWithSpacing() * 10.0f));
    if (ImGui::BeginChild("CommunityMyUploads", ImVec2(0.0f, listHeight), true)) {
        const int count = Community_GetMyUploadCount();
        if (count == 0) {
            ImGui::TextDisabled("No uploads found for this install");
        }
        bool anyVisible = false;
        for (int i = 0; i < count; ++i) {
            CommunityMyUpload item = Community_GetMyUploadItem(i);
            if (!MyUploadMatchesFilter(item)) {
                continue;
            }
            anyVisible = true;
            ImGui::PushID(item.id.c_str());
            ImGui::Separator();
            const char* author = item.author.empty() ? "Anonymous" : item.author.c_str();
            ImGui::Text("%s by %s", item.title.c_str(), author);
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", item.status.empty() ? "unknown" : item.status.c_str());
            if (!item.pendingUpdateId.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("[Update pending]");
            }
            if (!item.updateOf.empty()) {
                ImGui::TextDisabled("Update for %s", item.updateOf.c_str());
            }
            if (!item.pendingUpdateId.empty()) {
                ImGui::TextDisabled("Pending update: %s", item.pendingUpdateTitle.empty() ? item.pendingUpdateId.c_str() : item.pendingUpdateTitle.c_str());
            }
            if (!item.description.empty()) {
                ImGui::TextWrapped("%s", item.description.c_str());
            }
            if (Community_IsBusy()) ImGui::BeginDisabled();
            const bool canUpdate = _stricmp(item.status.c_str(), "pending") != 0 && item.pendingUpdateId.empty();
            if (!canUpdate) ImGui::BeginDisabled();
            if (ImGui::Button("Update")) {
                OpenUpdatePopup(item);
            }
            if (!canUpdate) {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (!item.pendingUpdateId.empty()) {
                if (ImGui::Button("Cancel Update")) {
                    Community_RequestCancelMyUploadUpdate(item.id.c_str());
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("Delete")) {
                OpenDeletePopup(item);
            }
            if (Community_IsBusy()) ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (count > 0 && !anyVisible) {
            ImGui::TextDisabled("No uploads match");
        }
    }
    ImGui::EndChild();
    if (g_openUpdatePopup) {
        ImGui::OpenPopup("Update Community Upload");
        g_openUpdatePopup = false;
    }
    if (g_openDeletePopup) {
        ImGui::OpenPopup("Delete Community Upload");
        g_openDeletePopup = false;
    }
    DrawUpdatePopup();
    DrawDeletePopup();
}

void DrawCatalogItem(const CommunityCatalogItem& item) {
    ImGui::PushID(item.id.c_str());
    ImGui::Separator();
    const char* author = item.author.empty() ? "Anonymous" : item.author.c_str();
    const std::string stats = std::to_string(item.downloads) + " downloads  |  " + std::to_string(item.likes) + " likes";
    const float statsWidth = ImGui::CalcTextSize(stats.c_str()).x;
    const float statsX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - statsWidth;
    ImGui::TextUnformatted(item.title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("by %s", author);
    if (statsX > ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x) {
        ImGui::SameLine(statsX);
        ImGui::TextDisabled("%s", stats.c_str());
    } else {
        ImGui::TextDisabled("%s", stats.c_str());
    }
    if (!item.description.empty()) {
        ImGui::TextWrapped("%s", item.description.c_str());
    }
    if (Community_IsBusy() || !Community_IsEnabled()) ImGui::BeginDisabled();
    if (ImGui::Button("Download")) {
        Community_RequestDownload(item.id.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(item.liked ? "Unlike" : "Like")) {
        Community_RequestLike(item.id.c_str());
    }
    if (Community_IsBusy() || !Community_IsEnabled()) ImGui::EndDisabled();
    ImGui::PopID();
}

} // namespace

void DrawCommunityTab() {
    ImGui::PushID("CrimsonWeatherCommunityTab");
    Community_Tick();
    if (!g_initialViewRefreshStarted && Community_IsEnabled() && Community_GetEndpoint()[0] != '\0') {
        g_initialViewRefreshStarted = Community_RequestInitialViewRefresh();
    }
    DrawEndpointConfig();
    ImGui::TextDisabled("%s", Community_GetStatusText());
    ImGui::Separator();

    ImGui::SetNextItemWidth(-216.0f);
    ImGui::InputTextWithHint("##community_filter", "Search...", g_communityFilter, IM_ARRAYSIZE(g_communityFilter));
    ImGui::SameLine();
    const char* sortLabels[] = { "Recent", "Popular", "Most Downloaded", "Most Liked" };
    ImGui::SetNextItemWidth(128.0f);
    ImGui::Combo("##community_sort", &g_sortMode, sortLabels, IM_ARRAYSIZE(sortLabels));
    ImGui::SameLine();
    if (!Community_CanManualRefresh() || Community_IsBusy() || !Community_IsEnabled()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Refresh")) {
        Community_RequestRefresh(true);
    }
    if (!Community_CanManualRefresh() || Community_IsBusy() || !Community_IsEnabled()) {
        ImGui::EndDisabled();
    }

    const float listHeight = min(720.0f, max(380.0f, ImGui::GetTextLineHeightWithSpacing() * 28.0f));
    if (ImGui::BeginChild("CommunityCatalog", ImVec2(0.0f, listHeight), true)) {
        const std::vector<CommunityCatalogItem> items = VisibleItems();
        if (items.empty()) {
            ImGui::TextDisabled("No community presets match");
        }
        for (const CommunityCatalogItem& item : items) {
            DrawCatalogItem(item);
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    DrawSubmitSection();
    DrawMyUploadsSection();
    ImGui::PopID();
}
