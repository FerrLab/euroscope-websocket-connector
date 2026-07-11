// Unit tests for the ".wsc gateway config" argument parser
// (src/GatewayConfig.cpp): base64 of "<url>:<token>" in one command-line
// token, because EuroScope's command line does not pass ':' characters
// through to plugins.
//
// Platform-independent — runs on any OS, like json_api_test.

#include <iostream>
#include <string>

#include "GatewayConfig.h"

// ---------------------------------------------------------------------
// tiny assertion helper (same shape as json_api_test.cpp)
// ---------------------------------------------------------------------

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do                                                                     \
    {                                                                      \
        ++g_checks;                                                        \
        if (!(cond))                                                       \
        {                                                                  \
            ++g_failures;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "    \
                      << #cond << "\n";                                    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do                                                                     \
    {                                                                      \
        ++g_checks;                                                        \
        if (!((a) == (b)))                                                 \
        {                                                                  \
            ++g_failures;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "    \
                      << #a << " == " << #b << "  (got: " << (a) << ")\n"; \
        }                                                                  \
    } while (0)

int main()
{
    // --- happy path: https URL + Sanctum-style token ------------------
    {
        // base64("https://api.example.com/euroscope:3|x7JdK9")
        const GatewayConfig c =
            ParseGatewayConfig("aHR0cHM6Ly9hcGkuZXhhbXBsZS5jb20vZXVyb3Njb3BlOjN8eDdKZEs5");
        CHECK_EQ(c.error, "");
        CHECK_EQ(c.url, "https://api.example.com/euroscope");
        CHECK_EQ(c.token, "3|x7JdK9");
    }

    // --- URL with a port: split must be at the LAST colon -------------
    {
        // base64("http://localhost:8000/api/euroscope:secret")
        const GatewayConfig c =
            ParseGatewayConfig("aHR0cDovL2xvY2FsaG9zdDo4MDAwL2FwaS9ldXJvc2NvcGU6c2VjcmV0");
        CHECK_EQ(c.error, "");
        CHECK_EQ(c.url, "http://localhost:8000/api/euroscope");
        CHECK_EQ(c.token, "secret");
    }

    // --- unpadded base64 is accepted -----------------------------------
    {
        const GatewayConfig c =
            ParseGatewayConfig("aHR0cHM6Ly9hcGkuZXhhbXBsZS5jb20vZXVyb3Njb3BlOnNlY3JldAo");
        CHECK_EQ(c.error, "");
        CHECK_EQ(c.token, "secret");
    }

    // --- standard and URL-safe alphabets both accepted -----------------
    {
        // base64("https://h/p:secret???") — contains '/'
        const GatewayConfig std_ = ParseGatewayConfig("aHR0cHM6Ly9oL3A6c2VjcmV0Pz8/");
        CHECK_EQ(std_.error, "");
        CHECK_EQ(std_.url, "https://h/p");
        CHECK_EQ(std_.token, "secret???");

        // same payload, URL-safe alphabet ('_' instead of '/')
        const GatewayConfig safe = ParseGatewayConfig("aHR0cHM6Ly9oL3A6c2VjcmV0Pz8_");
        CHECK_EQ(safe.error, "");
        CHECK_EQ(safe.url, "https://h/p");
        CHECK_EQ(safe.token, "secret???");
    }

    // --- trailing newline in the payload is trimmed --------------------
    // (the classic `echo url:token | base64` without -n)
    {
        // base64("https://api.example.com/euroscope:secret\n")
        const GatewayConfig c =
            ParseGatewayConfig("aHR0cHM6Ly9hcGkuZXhhbXBsZS5jb20vZXVyb3Njb3BlOnNlY3JldAo=");
        CHECK_EQ(c.error, "");
        CHECK_EQ(c.url, "https://api.example.com/euroscope");
        CHECK_EQ(c.token, "secret");
    }

    // --- payload without any colon --------------------------------------
    {
        // base64("no-colon-here")
        const GatewayConfig c = ParseGatewayConfig("bm8tY29sb24taGVyZQ==");
        CHECK(!c.error.empty());
    }

    // --- URL-only payload (forgot the token): the last colon is the
    //     scheme's, leaving "https" as the url part — must be rejected ---
    {
        // base64("https://api.example.com/euroscope")
        const GatewayConfig c =
            ParseGatewayConfig("aHR0cHM6Ly9hcGkuZXhhbXBsZS5jb20vZXVyb3Njb3Bl");
        CHECK(!c.error.empty());
    }

    // --- empty / invalid base64 -----------------------------------------
    {
        CHECK(!ParseGatewayConfig("").error.empty());
        CHECK(!ParseGatewayConfig("!!!!").error.empty());       // bad chars
        CHECK(!ParseGatewayConfig("AAAAA").error.empty());      // len % 4 == 1
        CHECK(!ParseGatewayConfig("AB=CD===").error.empty());   // '=' mid-string
        CHECK(!ParseGatewayConfig("Og==").error.empty());       // ":" alone
    }

    std::cout << (g_failures ? "FAILED" : "OK") << "  (" << g_checks
              << " checks, " << g_failures << " failures)\n";
    return g_failures ? 1 : 0;
}
