#pragma once

// SHA-256 and HMAC-SHA256 — needed for Pusher private-channel auth tokens
// (auth = "<app_key>:" + hex(HMAC_SHA256(secret, socket_id + ":" + channel))).
//
// Pure, self-contained, no platform headers; verified against the FIPS
// 180-4 SHA-256 examples and the RFC 4231 HMAC test vectors in
// tests/ws_test.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace Crypto
{
    std::vector<uint8_t> Sha256(const uint8_t* data, size_t len);

    std::vector<uint8_t> HmacSha256(const uint8_t* key, size_t keyLen,
                                    const uint8_t* message, size_t messageLen);

    // Convenience: HMAC over strings, lowercase-hex output (the format
    // Pusher auth signatures use).
    std::string HmacSha256Hex(const std::string& key, const std::string& message);
}
