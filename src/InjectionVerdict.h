#pragma once

// Decision logic for the deferred chat-injection check: one timer tick
// after a keystroke was POSTed to EuroScope's command line (see
// ChatInjection.h), what happened to the text we put there?
//
// Pure string logic, no Windows dependencies — unit-tested in
// tests/injection_verdict_test.cpp.

#include <string>

enum class InjectionVerdict
{
    // The box is empty: EuroScope consumed the text. Restore whatever the
    // controller had typed before the injection.
    Consumed,
    // Our text is still sitting there untouched: the keystroke was
    // ignored. Restore the controller's text and report failure.
    NotConsumed,
    // The box holds something else: EuroScope consumed our text and the
    // controller has already started typing again. Report success and do
    // NOT touch the box.
    Overwritten,
};

InjectionVerdict JudgeInjection(const std::string& injected,
                                const std::string& current);
