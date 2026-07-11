#include "ChatInjection.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <deque>
#include <vector>

#include "InjectionVerdict.h"

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

    // POSTED so the keystroke travels through the thread message queue and
    // is seen by EuroScope's message pump (PreTranslateMessage), exactly
    // like a physical key press. No explicit WM_CHAR: the pump's own
    // TranslateMessage generates it from the posted WM_KEYDOWN.
    void PostKey(HWND target, WORD vk)
    {
        PostMessageA(target, WM_KEYDOWN, vk, KeyLParam(vk, false));
        PostMessageA(target, WM_KEYUP, vk, KeyLParam(vk, true));
    }

    std::string GetEditText(HWND edit)
    {
        char buffer[2048] = {};
        SendMessageA(edit, WM_GETTEXT, sizeof(buffer) - 1,
                     reinterpret_cast<LPARAM>(buffer));
        return buffer;
    }

    void SetEditText(HWND edit, const std::string& text)
    {
        SendMessageA(edit, WM_SETTEXT, 0,
                     reinterpret_cast<LPARAM>(text.c_str()));
    }

    // ---- send queue -----------------------------------------------------
    //
    // One send is in flight at a time: its text sits in the command line
    // until the posted key is processed, so a second WM_SETTEXT before the
    // next Pump() would clobber it. Further sends wait in g_queue.

    struct PendingSend
    {
        std::string text;
        std::string label;
        WORD key = 0;
    };

    struct InFlight
    {
        bool active = false;
        HWND edit = nullptr;
        std::string text;
        std::string previous; // what the controller had typed before us
        std::string label;
    };

    std::deque<PendingSend> g_queue;
    InFlight g_inFlight;

    // Writes the text into the command line and posts the key; fills
    // g_inFlight for the next Pump() to resolve.
    bool Dispatch(const PendingSend& send, std::string& errorOut)
    {
        std::string detail;
        HWND edit = FindCommandLineEdit(detail);
        if (!edit)
        {
            errorOut = "command line not found: " + detail;
            return false;
        }

        g_inFlight.active = true;
        g_inFlight.edit = edit;
        g_inFlight.text = send.text;
        g_inFlight.previous = GetEditText(edit);
        g_inFlight.label = send.label;

        SetEditText(edit, send.text);
        PostKey(edit, send.key);
        return true;
    }

    // Never dispatches inline: when called from OnCompileCommand the
    // consumed ".lpc ..." line is still sitting in the command line and
    // EuroScope clears it AFTER the callback returns - inline WM_SETTEXT
    // would be wiped before the posted key processes, and the pre-clear
    // command would be captured as the "previous" text to restore
    // (observed live: the .lpc command reappeared in the command line).
    // Dispatch happens on the next Pump() tick, when EuroScope is idle.
    ChatInjection::SendResult Enqueue(const std::string& text,
                                      const std::string& label, WORD key)
    {
        g_queue.push_back({text, label, key});
        ChatInjection::SendResult result;
        result.ok = true;
        result.detail = "queued - result follows in the LPC tab";
        return result;
    }
}

namespace ChatInjection
{
    SendResult SendCommandLine(const std::string& text,
                               const std::string& label)
    {
        return Enqueue(text, label, VK_RETURN);
    }

    SendResult SendToPrimaryFrequency(const std::string& text,
                                      const std::string& label)
    {
        return Enqueue(text, label, VK_MULTIPLY);
    }

    std::vector<Outcome> Pump()
    {
        std::vector<Outcome> outcomes;

        if (g_inFlight.active)
        {
            const InFlight f = g_inFlight;
            g_inFlight = InFlight{};

            Outcome o;
            if (!IsWindow(f.edit))
            {
                o.detail = f.label + ": FAILED - the command-line window "
                                     "disappeared before the send could be "
                                     "verified";
            }
            else
            {
                switch (JudgeInjection(f.text, GetEditText(f.edit)))
                {
                case InjectionVerdict::Consumed:
                    if (!f.previous.empty())
                        SetEditText(f.edit, f.previous);
                    o.ok = true;
                    o.detail = f.label + ": sent";
                    break;
                case InjectionVerdict::NotConsumed:
                    SetEditText(f.edit, f.previous);
                    o.detail = f.label +
                               ": FAILED - EuroScope did not consume the "
                               "injected text (previous content restored). "
                               "If this persists, this EuroScope version may "
                               "not support injection - see docs/COMMANDS.md";
                    break;
                case InjectionVerdict::Overwritten:
                    // The controller is typing again; assume the send
                    // happened and keep our hands off the command line.
                    o.ok = true;
                    o.detail = f.label + ": probably sent (the command line "
                                         "was already back in use)";
                    break;
                }
            }
            outcomes.push_back(o);
        }

        if (!g_inFlight.active && !g_queue.empty())
        {
            const PendingSend next = g_queue.front();
            g_queue.pop_front();
            std::string error;
            if (!Dispatch(next, error))
                outcomes.push_back({false, next.label + ": FAILED - " + error});
        }

        return outcomes;
    }
}
