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
// Posting has two consequences. (1) The key is processed only after the
// current plugin callback returns, so "did EuroScope consume it?" cannot
// be checked synchronously. (2) A send must not even START inside a
// command callback: OnCompileCommand runs while the consumed ".lpc ..."
// line is still sitting in the command line, and EuroScope clears the box
// AFTER the callback returns — verified live: inline-injected text was
// wiped before the posted key processed (nothing sent, box empty, a false
// "consumed" verdict) and the pre-clear ".lpc" command was captured as
// the "previous" content and wrongly restored. Sends therefore always
// wait in a queue: Pump(), called once per second (OnTimer), dispatches
// one send (WM_SETTEXT + posted key) while EuroScope is idle, then
// resolves it on the following tick — judge the command line
// (InjectionVerdict.h), restore the controller's own text, emit the
// Outcome. One send is in flight at a time; expect ~2 s per message.
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
