#include "pch.h"

#include "community_http.h"

#include <winhttp.h>

#include <algorithm>
#include <cwchar>

namespace {

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(0, needed)), L'\0');
    if (needed > 0) {
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), needed);
    }
    return out;
}

std::string LastWinHttpError(const char* prefix) {
    char message[128] = {};
    sprintf_s(message, "%s failed: %lu", prefix ? prefix : "WinHTTP", GetLastError());
    return message;
}

} // namespace

bool CommunityHttp_Request(
    const char* method,
    const std::string& url,
    const std::vector<CommunityHttpHeader>& headers,
    const std::string& body,
    CommunityHttpResponse& outResponse) {
    outResponse = CommunityHttpResponse{};
    const std::wstring wideUrl = Utf8ToWide(url);
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        outResponse.error = LastWinHttpError("WinHttpCrackUrl");
        return false;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    HINTERNET session = WinHttpOpen(
        L"CrimsonWeatherCommunity/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        outResponse.error = LastWinHttpError("WinHttpOpen");
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 30000, 120000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) {
        outResponse.error = LastWinHttpError("WinHttpConnect");
        WinHttpCloseHandle(session);
        return false;
    }

    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const std::wstring wideMethod = Utf8ToWide(method && method[0] ? method : "GET");
    HINTERNET request = WinHttpOpenRequest(
        connect,
        wideMethod.c_str(),
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!request) {
        outResponse.error = LastWinHttpError("WinHttpOpenRequest");
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring headerText;
    for (const CommunityHttpHeader& header : headers) {
        headerText += Utf8ToWide(header.name);
        headerText += L": ";
        headerText += Utf8ToWide(header.value);
        headerText += L"\r\n";
    }

    const void* bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data();
    const DWORD bodySize = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(
        request,
        headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(),
        static_cast<DWORD>(headerText.size()),
        const_cast<void*>(bodyPtr),
        bodySize,
        bodySize,
        0)) {
        outResponse.error = LastWinHttpError("WinHttpSendRequest");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        outResponse.error = LastWinHttpError("WinHttpReceiveResponse");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    outResponse.statusCode = static_cast<int>(status);

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            outResponse.error = LastWinHttpError("WinHttpQueryDataAvailable");
            break;
        }
        if (available == 0) {
            break;
        }
        const size_t oldSize = outResponse.body.size();
        outResponse.body.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, outResponse.body.data() + oldSize, available, &read)) {
            outResponse.error = LastWinHttpError("WinHttpReadData");
            break;
        }
        outResponse.body.resize(oldSize + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return outResponse.error.empty();
}
