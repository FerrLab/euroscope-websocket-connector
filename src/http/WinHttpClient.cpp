// Production Http::Client backend on Windows WinHTTP. TLS (1.2/1.3),
// server-certificate validation against the OS trust store, SNI and proxy
// settings are all handled by Windows - nothing crypto-related lives in
// this codebase.
//
// One instance per request (the gateway's usage pattern). Abort() closes
// the handles, which makes a blocked WinHttp call in the worker thread
// return with an error - the documented cancellation mechanism.

#ifdef _WIN32

#include "HttpClient.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <vector>

namespace Http
{
    namespace
    {
        std::wstring Widen(const std::string& s)
        {
            if (s.empty())
                return std::wstring();
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                              static_cast<int>(s.size()),
                                              nullptr, 0);
            std::wstring out(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                static_cast<int>(s.size()), &out[0], n);
            return out;
        }

        std::string LastErrorText(const char* what)
        {
            return std::string(what) + " failed (WinHTTP error " +
                   std::to_string(GetLastError()) + ")";
        }

        class WinHttpClient : public Client
        {
        public:
            ~WinHttpClient() override { Abort(); }

            Response Get(const Url& url, const std::string& pathSuffix,
                         const std::string& bearerToken, int timeoutMs) override
            {
                return Do(L"GET", url, pathSuffix, nullptr, 0, bearerToken,
                          timeoutMs);
            }

            Response Post(const Url& url, const std::string& pathSuffix,
                          const std::string& jsonBody,
                          const std::string& bearerToken, int timeoutMs) override
            {
                return Do(L"POST", url, pathSuffix, jsonBody.data(),
                          jsonBody.size(), bearerToken, timeoutMs);
            }

            void Abort() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_aborted = true;
                CloseHandles();
            }

        private:
            std::mutex m_mutex;
            bool m_aborted = false;
            HINTERNET m_session = nullptr;
            HINTERNET m_connect = nullptr;
            HINTERNET m_request = nullptr;

            void CloseHandles()
            {
                // Closing a handle cancels the operation blocked on it.
                if (m_request)
                {
                    WinHttpCloseHandle(m_request);
                    m_request = nullptr;
                }
                if (m_connect)
                {
                    WinHttpCloseHandle(m_connect);
                    m_connect = nullptr;
                }
                if (m_session)
                {
                    WinHttpCloseHandle(m_session);
                    m_session = nullptr;
                }
            }

            Response Do(const wchar_t* method, const Url& url,
                        const std::string& pathSuffix, const char* body,
                        size_t bodyLen, const std::string& bearerToken,
                        int timeoutMs)
            {
                Response r;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_aborted)
                    {
                        r.error = "aborted";
                        return r;
                    }
                    m_session = WinHttpOpen(
                        L"euroscope-websocket-connector",
                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
                    if (!m_session)
                    {
                        r.error = LastErrorText("WinHttpOpen");
                        return r;
                    }
                    // resolve, connect, send, receive - receive must cover
                    // the server's long-poll hold.
                    WinHttpSetTimeouts(m_session, 15000, 15000, 30000,
                                       timeoutMs);

                    m_connect = WinHttpConnect(m_session, Widen(url.host).c_str(),
                                               static_cast<INTERNET_PORT>(url.port),
                                               0);
                    if (!m_connect)
                    {
                        r.error = LastErrorText("WinHttpConnect");
                        CloseHandles();
                        return r;
                    }

                    const std::wstring path = Widen(url.basePath + pathSuffix);
                    m_request = WinHttpOpenRequest(
                        m_connect, method, path.c_str(), nullptr,
                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                        url.https ? WINHTTP_FLAG_SECURE : 0);
                    if (!m_request)
                    {
                        r.error = LastErrorText("WinHttpOpenRequest");
                        CloseHandles();
                        return r;
                    }
                }

                std::wstring headers =
                    L"Content-Type: application/json\r\n"
                    L"Accept: application/json";
                if (!bearerToken.empty())
                    headers += L"\r\nAuthorization: Bearer " + Widen(bearerToken);

                // Blocking section - intentionally outside the mutex so
                // Abort() can close the handles and unblock us.
                if (!WinHttpSendRequest(m_request, headers.c_str(),
                                        static_cast<DWORD>(-1),
                                        const_cast<char*>(body),
                                        static_cast<DWORD>(bodyLen),
                                        static_cast<DWORD>(bodyLen), 0))
                {
                    r.error = LastErrorText("WinHttpSendRequest");
                    Abort();
                    return r;
                }
                if (!WinHttpReceiveResponse(m_request, nullptr))
                {
                    r.error = LastErrorText("WinHttpReceiveResponse");
                    Abort();
                    return r;
                }

                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                if (!WinHttpQueryHeaders(
                        m_request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX))
                {
                    r.error = LastErrorText("WinHttpQueryHeaders");
                    Abort();
                    return r;
                }

                for (;;)
                {
                    DWORD available = 0;
                    if (!WinHttpQueryDataAvailable(m_request, &available))
                    {
                        r.error = LastErrorText("WinHttpQueryDataAvailable");
                        Abort();
                        return r;
                    }
                    if (available == 0)
                        break;
                    std::vector<char> chunk(available);
                    DWORD read = 0;
                    if (!WinHttpReadData(m_request, chunk.data(), available,
                                         &read))
                    {
                        r.error = LastErrorText("WinHttpReadData");
                        Abort();
                        return r;
                    }
                    r.body.append(chunk.data(), read);
                }

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    CloseHandles();
                }
                r.ok = true;
                r.status = static_cast<int>(status);
                return r;
            }
        };
    }

    std::unique_ptr<Client> CreateWinHttpClient()
    {
        return std::unique_ptr<Client>(new WinHttpClient());
    }
}

#endif  // _WIN32
