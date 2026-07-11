#include "ChatInjection.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>

namespace
{
    // ---- window discovery ---------------------------------------------

    BOOL CALLBACK CollectTopLevel(HWND hwnd, LPARAM lParam)
    {
        auto* out = reinterpret_cast<std::vector<HWND>*>(lParam);
        out->push_back(hwnd);
        return TRUE;
    }

    // Top-level windows owned by the current (EuroScope UI) thread. Plugin
    // callbacks run on that thread, so GetCurrentThreadId() is correct.
    std::vector<HWND> TopLevelWindowsOfThisThread()
    {
        std::vector<HWND> windows;
        EnumThreadWindows(GetCurrentThreadId(), CollectTopLevel,
                          reinterpret_cast<LPARAM>(&windows));
        return windows;
    }

    bool TitleContainsEuroScope(HWND hwnd)
    {
        char title[512] = {};
        GetWindowTextA(hwnd, title, sizeof(title) - 1);
        for (const char* p = title; *p; ++p)
        {
            if ((p[0] == 'E' || p[0] == 'e') &&
                _strnicmp(p, "EuroScope", 9) == 0)
                return true;
        }
        return false;
    }

    struct EditSearch
    {
        HWND found = nullptr;
    };

    BOOL CALLBACK FindEditChild(HWND hwnd, LPARAM lParam)
    {
        auto* search = reinterpret_cast<EditSearch*>(lParam);
        char className[64] = {};
        GetClassNameA(hwnd, className, sizeof(className) - 1);
        if (_stricmp(className, "Edit") == 0 && IsWindowVisible(hwnd) &&
            IsWindowEnabled(hwnd))
        {
            search->found = hwnd;
            return FALSE; // stop enumeration
        }
        return TRUE;
    }

    // The EuroScope command line is the (first) visible Edit control of the
    // main frame. EnumChildWindows enumerates recursively.
    HWND FindCommandLineEdit(std::string& detail)
    {
        std::vector<HWND> candidates = TopLevelWindowsOfThisThread();
        if (candidates.empty())
        {
            detail = "no top-level window found on the EuroScope UI thread";
            return nullptr;
        }

        // Prefer windows whose title mentions EuroScope (the main frame),
        // then fall back to any thread window.
        for (int pass = 0; pass < 2; ++pass)
        {
            for (HWND top : candidates)
            {
                if (pass == 0 && !TitleContainsEuroScope(top))
                    continue;
                EditSearch search;
                EnumChildWindows(top, FindEditChild,
                                 reinterpret_cast<LPARAM>(&search));
                if (search.found)
                    return search.found;
            }
        }
        detail = "no visible Edit control found (EuroScope UI layout changed?)";
        return nullptr;
    }

    // ---- key synthesis --------------------------------------------------

    LPARAM KeyLParam(WORD vk, bool keyUp)
    {
        const UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
        LPARAM lp = 1 | (static_cast<LPARAM>(scan) << 16);
        if (keyUp)
            lp |= (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
        return lp;
    }

    void PressKey(HWND target, WORD vk)
    {
        SendMessageA(target, WM_KEYDOWN, vk, KeyLParam(vk, false));
        if (vk == VK_RETURN)
            SendMessageA(target, WM_CHAR, '\r', KeyLParam(vk, false));
        SendMessageA(target, WM_KEYUP, vk, KeyLParam(vk, true));
    }

    std::string GetEditText(HWND edit)
    {
        char buffer[2048] = {};
        SendMessageA(edit, WM_GETTEXT, sizeof(buffer) - 1,
                     reinterpret_cast<LPARAM>(buffer));
        return buffer;
    }

    ChatInjection::SendResult Inject(const std::string& text, WORD sendKey)
    {
        ChatInjection::SendResult result;

        std::string detail;
        HWND edit = FindCommandLineEdit(detail);
        if (!edit)
        {
            result.detail = "command line not found: " + detail;
            return result;
        }

        const std::string previous = GetEditText(edit);

        SendMessageA(edit, WM_SETTEXT, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
        PressKey(edit, sendKey);

        // EuroScope clears the command line when it consumes the content.
        // If our text is still sitting there, the send did not happen.
        const std::string after = GetEditText(edit);
        if (after == text)
        {
            SendMessageA(edit, WM_SETTEXT, 0,
                         reinterpret_cast<LPARAM>(previous.c_str()));
            result.detail =
                "EuroScope did not consume the command line content "
                "(previous content restored). If this persists, this "
                "EuroScope version may not support injection - see "
                "docs/COMMANDS.md";
            return result;
        }

        // Put back whatever the controller was typing before we hijacked
        // the command line.
        if (!previous.empty())
            SendMessageA(edit, WM_SETTEXT, 0,
                         reinterpret_cast<LPARAM>(previous.c_str()));

        result.ok = true;
        result.detail = "sent";
        return result;
    }
}

namespace ChatInjection
{
    SendResult SendCommandLine(const std::string& text)
    {
        return Inject(text, VK_RETURN);
    }

    SendResult SendToPrimaryFrequency(const std::string& text)
    {
        return Inject(text, VK_MULTIPLY);
    }
}
