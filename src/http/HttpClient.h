#pragma once

// HttpClient — the HTTP(S) request abstraction underneath the gateway.
//
// Implementations:
//   * WinHttpClient.cpp      — production backend: Windows WinHTTP
//                              (ships with the OS; TLS, certificate
//                              validation against the system store, and
//                              proxy handling are all done by Windows).
//   * tests/posix_http_client — TEST-ONLY plain-HTTP client so the
//                              gateway logic can be executed on the
//                              CI/dev machine.
//
// One instance = one request. The gateway creates a client per request
// and keeps a handle to the in-flight one so Abort() can cancel a
// blocked long poll from another thread.

#include <memory>
#include <string>

#include "HttpUrl.h"

namespace Http
{
    struct Response
    {
        bool ok = false;      // transport-level success (any status code)
        int status = 0;       // HTTP status when ok
        std::string body;
        std::string error;    // transport error when !ok
    };

    class Client
    {
    public:
        virtual ~Client() = default;

        // Blocking GET {url.basePath + pathSuffix}. `timeoutMs` bounds the
        // whole request - set it ABOVE the server's long-poll hold time.
        virtual Response Get(const Url& url, const std::string& pathSuffix,
                             const std::string& bearerToken,
                             int timeoutMs) = 0;

        // Blocking POST with a JSON body.
        virtual Response Post(const Url& url, const std::string& pathSuffix,
                              const std::string& jsonBody,
                              const std::string& bearerToken,
                              int timeoutMs) = 0;

        // Cancels the in-flight request from another thread; the blocked
        // call returns with a transport error. Idempotent.
        virtual void Abort() = 0;
    };

    // Factory so the gateway can be exercised with a test client on
    // non-Windows. Production: CreateWinHttpClient (Windows only).
    using ClientFactory = std::unique_ptr<Client> (*)();

#ifdef _WIN32
    std::unique_ptr<Client> CreateWinHttpClient();
#endif
}
