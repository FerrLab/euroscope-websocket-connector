#pragma once

// Test double for src/ChatInjection.{h,cpp}. The real implementation
// drives EuroScope's Win32 UI; tests link mock_chat_injection.cpp instead
// (never both), which records calls here.

#include <string>

namespace MockChatInjection
{
    extern bool nextOk;             // result the next call returns
    extern std::string nextDetail;  // detail for failed calls
    extern std::string lastCommandLine;
    extern std::string lastFrequencyText;

    void Reset();
}
