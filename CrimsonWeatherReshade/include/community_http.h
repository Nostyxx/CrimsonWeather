#pragma once

#include <string>
#include <vector>

struct CommunityHttpHeader {
    std::string name;
    std::string value;
};

struct CommunityHttpResponse {
    int statusCode = 0;
    std::string body;
    std::string error;
};

bool CommunityHttp_Request(
    const char* method,
    const std::string& url,
    const std::vector<CommunityHttpHeader>& headers,
    const std::string& body,
    CommunityHttpResponse& outResponse);

