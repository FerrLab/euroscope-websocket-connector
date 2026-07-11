#include "GatewayConfig.h"

namespace
{

// Decodes standard (+/) or URL-safe (-_) base64, padding optional.
// Returns false on any character outside the alphabet, '=' anywhere but
// the end, or an impossible length (4n+1 data characters).
bool DecodeBase64(const std::string& in, std::string& out)
{
    auto sextet = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };

    // Strip trailing padding, then no '=' may remain.
    size_t end = in.size();
    while (end > 0 && in[end - 1] == '=')
        --end;
    if (in.find('=') < end)
        return false;
    if (end % 4 == 1)
        return false;

    out.clear();
    out.reserve(end * 3 / 4);
    int buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < end; ++i)
    {
        const int value = sextet(in[i]);
        if (value < 0)
            return false;
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

std::string Trim(const std::string& s)
{
    const char* ws = " \t\r\n";
    const size_t first = s.find_first_not_of(ws);
    if (first == std::string::npos)
        return "";
    return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

} // namespace

GatewayConfig ParseGatewayConfig(const std::string& base64)
{
    GatewayConfig config;

    std::string decoded;
    if (base64.empty() || !DecodeBase64(base64, decoded))
    {
        config.error = "not valid base64";
        return config;
    }

    decoded = Trim(decoded);

    const size_t colon = decoded.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon == decoded.size() - 1)
    {
        config.error = "decoded text is not <url>:<token>";
        return config;
    }

    config.url = decoded.substr(0, colon);
    config.token = decoded.substr(colon + 1);

    // A url part without "://" means the last colon was the scheme's own
    // (someone encoded just the URL and forgot the token).
    if (config.url.find("://") == std::string::npos)
    {
        config.error =
            "decoded text is not <url>:<token> (did you forget the token?)";
        config.url.clear();
        config.token.clear();
        return config;
    }

    return config;
}
