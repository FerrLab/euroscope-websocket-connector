#include "posix_http_client.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>

namespace
{
    using Clock = std::chrono::steady_clock;

    class PosixHttpClient : public Http::Client
    {
    public:
        ~PosixHttpClient() override { Abort(); }

        Http::Response Get(const Http::Url& url, const std::string& pathSuffix,
                           const std::string& bearerToken, int timeoutMs) override
        {
            return Do("GET", url, pathSuffix, "", bearerToken, timeoutMs);
        }

        Http::Response Post(const Http::Url& url, const std::string& pathSuffix,
                            const std::string& jsonBody,
                            const std::string& bearerToken, int timeoutMs) override
        {
            return Do("POST", url, pathSuffix, jsonBody, bearerToken, timeoutMs);
        }

        void Abort() override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_aborted = true;
            if (m_fd >= 0)
            {
                shutdown(m_fd, SHUT_RDWR);
                close(m_fd);
                m_fd = -1;
            }
        }

    private:
        std::mutex m_mutex;
        bool m_aborted = false;
        int m_fd = -1;

        Http::Response Do(const char* method, const Http::Url& url,
                          const std::string& pathSuffix, const std::string& body,
                          const std::string& bearerToken, int timeoutMs)
        {
            Http::Response r;
            if (url.https)
            {
                r.error = "posix test client speaks plain http only";
                return r;
            }

            addrinfo hints = {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* results = nullptr;
            if (getaddrinfo(url.host.c_str(),
                            std::to_string(url.port).c_str(), &hints,
                            &results) != 0 ||
                !results)
            {
                r.error = "could not resolve " + url.host;
                return r;
            }
            int fd = -1;
            for (addrinfo* ai = results; ai; ai = ai->ai_next)
            {
                fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (fd < 0)
                    continue;
                if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
                    break;
                close(fd);
                fd = -1;
            }
            freeaddrinfo(results);
            if (fd < 0)
            {
                r.error = "could not connect to " + url.host + ":" +
                          std::to_string(url.port);
                return r;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_aborted)
                {
                    close(fd);
                    r.error = "aborted";
                    return r;
                }
                m_fd = fd;
            }

            std::string request = std::string(method) + " " +
                                  (url.basePath + pathSuffix) + " HTTP/1.1\r\n";
            request += "Host: " + url.host + "\r\n";
            if (!bearerToken.empty())
                request += "Authorization: Bearer " + bearerToken + "\r\n";
            request += "Content-Type: application/json\r\n";
            request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            request += "Connection: close\r\n\r\n";
            request += body;

            size_t sent = 0;
            while (sent < request.size())
            {
                const ssize_t n = send(fd, request.data() + sent,
                                       request.size() - sent, MSG_NOSIGNAL);
                if (n <= 0)
                {
                    r.error = "send failed";
                    return r;
                }
                sent += static_cast<size_t>(n);
            }

            // Read until EOF (Connection: close) or deadline.
            std::string raw;
            const auto deadline =
                Clock::now() + std::chrono::milliseconds(timeoutMs);
            for (;;)
            {
                const auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - Clock::now())
                        .count();
                if (remaining <= 0)
                {
                    r.error = "timeout";
                    return r;
                }
                fd_set readSet;
                FD_ZERO(&readSet);
                FD_SET(fd, &readSet);
                timeval tv;
                tv.tv_sec = static_cast<long>(remaining / 1000);
                tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
                const int ready = select(fd + 1, &readSet, nullptr, nullptr, &tv);
                if (ready < 0)
                {
                    r.error = "select failed";
                    return r;
                }
                if (ready == 0)
                    continue;
                char chunk[8192];
                const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
                if (n < 0)
                {
                    r.error = "recv failed (aborted?)";
                    return r;
                }
                if (n == 0)
                    break;  // EOF: response complete
                raw.append(chunk, static_cast<size_t>(n));
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_fd >= 0)
                {
                    close(m_fd);
                    m_fd = -1;
                }
            }

            // Parse status line + body.
            const size_t headerEnd = raw.find("\r\n\r\n");
            if (headerEnd == std::string::npos ||
                raw.compare(0, 5, "HTTP/") != 0)
            {
                r.error = "malformed response";
                return r;
            }
            const size_t space = raw.find(' ');
            r.status = std::atoi(raw.c_str() + space + 1);
            r.body = raw.substr(headerEnd + 4);
            r.ok = true;
            return r;
        }
    };
}

std::unique_ptr<Http::Client> MakePosixHttpClient()
{
    return std::unique_ptr<Http::Client>(new PosixHttpClient());
}
