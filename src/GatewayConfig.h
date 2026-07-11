#pragma once

// Parser for the ".lpc gateway config <base64>" argument.
//
// EuroScope's command line does not pass ':' characters through to
// plugins, so URLs (and "url + token" as two commands) cannot be typed
// directly. Instead the whole configuration travels as one base64 token
// that decodes to
//
//     <url>:<token>          e.g.  https://api.example.com/euroscope:3|x7Jd...
//
// The split is at the LAST colon (URLs contain colons; bearer tokens must
// not). Standard and URL-safe base64 alphabets are accepted, padding is
// optional, and whitespace around the decoded payload is trimmed (the
// classic `echo url:token | base64` trailing newline).
//
// No Windows / EuroScope dependencies — unit-tested in
// tests/gateway_config_test.cpp.

#include <string>

struct GatewayConfig
{
    std::string url;    // valid only when error is empty
    std::string token;  // valid only when error is empty
    std::string error;  // empty on success, human-readable reason otherwise
};

GatewayConfig ParseGatewayConfig(const std::string& base64);
