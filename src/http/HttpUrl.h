#pragma once

// http(s):// URL parsing — pure, unit-tested in tests/http_gateway_test.cpp.

#include <string>

namespace Http
{
    struct Url
    {
        bool ok = false;
        std::string error;
        bool https = false;
        std::string host;
        int port = 0;          // resolved: explicit, else 443/80
        std::string basePath;  // no trailing slash; "" for root
    };

    // Accepts "https://host[:port][/base/path]" and http:// for local
    // development. The base path is where the two endpoints live:
    // {base}/poll and {base}/messages.
    Url ParseUrl(const std::string& url);
}
