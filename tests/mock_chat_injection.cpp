// Mock implementation of the ChatInjection interface for unit tests.
// Linked INSTEAD of src/ChatInjection.cpp (which needs Win32).

#include "ChatInjection.h"

#include "mock_chat_injection.h"

namespace MockChatInjection
{
    bool nextOk = true;
    std::string nextDetail = "sent";
    std::string lastCommandLine;
    std::string lastFrequencyText;

    void Reset()
    {
        nextOk = true;
        nextDetail = "sent";
        lastCommandLine.clear();
        lastFrequencyText.clear();
    }
}

namespace ChatInjection
{
    SendResult SendCommandLine(const std::string& text)
    {
        MockChatInjection::lastCommandLine = text;
        return { MockChatInjection::nextOk, MockChatInjection::nextDetail };
    }

    SendResult SendToPrimaryFrequency(const std::string& text)
    {
        MockChatInjection::lastFrequencyText = text;
        return { MockChatInjection::nextOk, MockChatInjection::nextDetail };
    }
}
