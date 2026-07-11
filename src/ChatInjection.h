#pragma once

// ChatInjection — sending text messages from a plugin.
//
// The EuroScope plugin API (compatibility code 16) has NO function to send
// a chat message to a frequency or a private message to a user; the chat
// callbacks (OnCompileFrequencyChat / OnCompilePrivateChat) are
// receive-only, and DisplayUserMessage() only prints locally.
//
// This module implements the only known in-process workaround: it locates
// EuroScope's command-line edit control, types the text into it with
// WM_SETTEXT, and POSTS the appropriate key through the thread message
// queue:
//
//   * private message : inject ".msg <callsign> <text>" + ENTER
//     (".msg" is a documented EuroScope command-line command)
//   * frequency text  : inject "<text>" + the FREQ key. Per the EuroScope
//     manual ("Editing and function keys"), the FREQ key - numeric pad '*'
//     by default - sends a non-empty command line to the primary frequency.
//
// The keys MUST be posted (PostMessage), never sent (SendMessage):
// EuroScope is an MFC application and handles command-line keys in its
// message pump (PreTranslateMessage / accelerators), which only sees
// messages retrieved from the thread queue. SendMessage delivers straight
// to the edit control's window procedure, invisibly to the pump — verified
// live: SendMessage'd ENTER and FREQ keystrokes are ignored while the same
// physical key presses work.
//
// Posting has a consequence: the key is processed only after the current
// plugin callback returns, so "did EuroScope consume it?" cannot be
// checked synchronously. Sends are queued; call Pump() once per second
// (OnTimer) to resolve the in-flight send — judge the command line
// (InjectionVerdict.h), restore the controller's own text, collect the
// Outcome — and dispatch the next queued send. One send is in flight at a
// time; a burst of N messages takes ~N seconds.
//
// LIMITATIONS - read before relying on this:
//   * This drives undocumented UI internals. A future EuroScope version can
//     break it silently. Treat it as best-effort and test after upgrades.
//   * The FREQ key is assumed to be on its DEFAULT binding (numpad '*').
//     If the user remapped it, frequency sends will not work.
//   * Frequency text goes to the PRIMARY frequency only, and EuroScope may
//     prefix the currently selected (ASEL) aircraft's callsign.
//   * Everything must be called on the EuroScope main thread (any CPlugIn
//     callback).
//
// If a send fails, the previous command-line content is restored and the
// Outcome says why; nothing else is affected.

#include <string>
#include <vector>

namespace ChatInjection
{
    struct SendResult
    {
        bool ok = false;    // accepted into the send queue (NOT yet verified)
        std::string detail; // diagnostic, suitable for showing to the user
    };

    // Resolved result of a queued send, produced by Pump() one tick after
    // its keystroke was posted.
    struct Outcome
    {
        bool ok = false;
        std::string detail; // self-describing (includes the send's label)
    };

    // Queues `text` + ENTER for EuroScope's command line. Used for
    // dot-commands such as ".msg CALLSIGN hello". `label` identifies the
    // send in its eventual Outcome (e.g. "Private message to DLH4TX").
    SendResult SendCommandLine(const std::string& text,
                               const std::string& label);

    // Queues `text` + the FREQ key (numpad '*'), which sends it as a text
    // message to the primary frequency.
    SendResult SendToPrimaryFrequency(const std::string& text,
                                      const std::string& label);

    // Call once per second on the EuroScope main thread. Returns the
    // outcomes that resolved this tick (usually empty).
    std::vector<Outcome> Pump();
}
