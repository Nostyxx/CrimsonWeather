#include "pch.h"

#include "community_http.h"
#include "runtime_shared.h"

#include <winhttp.h>

#include <algorithm>
#include <cwchar>

namespace {

constexpr size_t kMaxHttpResponseBytes = 128ull * 1024ull * 1024ull;

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

std::string WinHttpErrorText(DWORD errorCode) {
    char systemMessage[256] = {};
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        errorCode,
        0,
        systemMessage,
        static_cast<DWORD>(std::size(systemMessage)),
        nullptr);
    std::string text = length ? std::string(systemMessage, length) : std::string();
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

void SetWinHttpError(CommunityHttpResponse& response, const char* stage) {
    const DWORD errorCode = GetLastError();
    response.transportErrorCode = errorCode;
    response.transportStage = stage ? stage : "WinHTTP";
    response.error = response.transportStage + " failed: " + std::to_string(errorCode);
    const std::string systemMessage = WinHttpErrorText(errorCode);
    if (!systemMessage.empty()) {
        response.error += " (" + systemMessage + ")";
    }
}

bool IsTransientWinHttpError(unsigned long errorCode) {
    switch (errorCode) {
    case ERROR_WINHTTP_TIMEOUT:
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_RESEND_REQUEST:
        return true;
    default:
        return false;
    }
}

bool CommunityHttp_RequestOnce(
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
        SetWinHttpError(outResponse, "WinHttpCrackUrl");
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
        SetWinHttpError(outResponse, "WinHttpOpen");
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 30000, 120000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) {
        SetWinHttpError(outResponse, "WinHttpConnect");
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
        SetWinHttpError(outResponse, "WinHttpOpenRequest");
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
        SetWinHttpError(outResponse, "WinHttpSendRequest");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        SetWinHttpError(outResponse, "WinHttpReceiveResponse");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &status,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX)) {
        SetWinHttpError(outResponse, "WinHttpQueryHeaders");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    outResponse.statusCode = static_cast<int>(status);

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            SetWinHttpError(outResponse, "WinHttpQueryDataAvailable");
            break;
        }
        if (available == 0) {
            break;
        }
        const size_t oldSize = outResponse.body.size();
        if (oldSize > kMaxHttpResponseBytes || available > kMaxHttpResponseBytes - oldSize) {
            outResponse.transportErrorCode = ERROR_INSUFFICIENT_BUFFER;
            outResponse.transportStage = "WinHttpReadData";
            outResponse.error = "HTTP response exceeded 128 MiB transport limit";
            break;
        }
        outResponse.body.resize(oldSize + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, outResponse.body.data() + oldSize, available, &read)) {
            SetWinHttpError(outResponse, "WinHttpReadData");
            break;
        }
        outResponse.body.resize(oldSize + read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return outResponse.error.empty();
}

} // namespace

bool CommunityHttp_Request(
    const char* method,
    const std::string& url,
    const std::vector<CommunityHttpHeader>& headers,
    const std::string& body,
    CommunityHttpResponse& outResponse) {
    const char* requestMethod = method && method[0] ? method : "GET";
    const bool idempotent = _stricmp(requestMethod, "GET") == 0 || _stricmp(requestMethod, "HEAD") == 0;
    const int maxAttempts = idempotent ? 3 : 1;

    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (CommunityHttp_RequestOnce(requestMethod, url, headers, body, outResponse)) {
            if (attempt > 1) {
                Log("[http] request recovered method=%s attempts=%d\n",
                    requestMethod, attempt);
            }
            return true;
        }

        const bool retry = attempt < maxAttempts && IsTransientWinHttpError(outResponse.transportErrorCode);
        Log("[http] request failed method=%s stage=%s error=%lu attempt=%d/%d retry=%u\n",
            requestMethod,
            outResponse.transportStage.empty() ? "unknown" : outResponse.transportStage.c_str(),
            outResponse.transportErrorCode,
            attempt,
            maxAttempts,
            retry ? 1u : 0u);
        if (!retry) {
            return false;
        }
        Sleep(static_cast<DWORD>(attempt * 250));
    }
    return false;
}
