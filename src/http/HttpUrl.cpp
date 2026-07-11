#include "HttpUrl.h"

namespace Http
{
    Url ParseUrl(const std::string& url)
    {
        Url out;

        std::string rest;
        if (url.compare(0, 8, "https://") == 0)
        {
            out.https = true;
            rest = url.substr(8);
        }
        else if (url.compare(0, 7, "http://") == 0)
        {
            out.https = false;
            rest = url.substr(7);
        }
        else
        {
            out.error = "URL must start with https:// (or http:// for local "
                        "development), e.g. https://api.example.com/euroscope";
            return out;
        }

        const size_t slash = rest.find('/');
        if (slash != std::string::npos)
        {
            out.basePath = rest.substr(slash);
            rest = rest.substr(0, slash);
            // strip trailing slashes so {base}/poll composes cleanly
            while (!out.basePath.empty() && out.basePath.back() == '/')
                out.basePath.pop_back();
        }

        const size_t colon = rest.find(':');
        if (colon != std::string::npos)
        {
            out.host = rest.substr(0, colon);
            const std::string portText = rest.substr(colon + 1);
            if (portText.empty() ||
                portText.find_first_not_of("0123456789") != std::string::npos)
            {
                out.error = "invalid port in URL";
                return out;
            }
            out.port = std::stoi(portText);
            if (out.port < 1 || out.port > 65535)
            {
                out.error = "invalid port in URL";
                return out;
            }
        }
        else
        {
            out.host = rest;
            out.port = out.https ? 443 : 80;
        }

        if (out.host.empty())
        {
            out.error = "missing host in URL";
            return out;
        }
        out.ok = true;
        return out;
    }
}
