// Unit tests for the deferred chat-injection verdict
// (src/InjectionVerdict.cpp). Platform-independent.

#include <iostream>
#include <string>

#include "InjectionVerdict.h"

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

int main()
{
    // Text unchanged -> the keystroke was ignored.
    CHECK(JudgeInjection("hello", "hello") == InjectionVerdict::NotConsumed);

    // Box emptied -> EuroScope consumed the text.
    CHECK(JudgeInjection("hello", "") == InjectionVerdict::Consumed);

    // Box holds something else -> controller typing again; hands off.
    CHECK(JudgeInjection("hello", "DLH4TX descend") == InjectionVerdict::Overwritten);

    // Any difference counts as "something else", even whitespace: the
    // controller may have started editing our leftover text.
    CHECK(JudgeInjection("hello", "hello ") == InjectionVerdict::Overwritten);
    CHECK(JudgeInjection("hello", "hell") == InjectionVerdict::Overwritten);

    // Degenerate: empty injected text in an empty box reads as untouched.
    CHECK(JudgeInjection("", "") == InjectionVerdict::NotConsumed);

    std::cout << (g_failures ? "FAILED" : "OK") << "  (" << g_checks
              << " checks, " << g_failures << " failures)\n";
    return g_failures ? 1 : 0;
}
