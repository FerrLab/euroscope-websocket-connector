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
// WM_SETTEXT, and presses the appropriate key:
//
//   * private message : inject ".msg <callsign> <text>" + ENTER
//     (".msg" is a documented EuroScope command-line command)
//   * frequency text  : inject "<text>" + the FREQ key. Per the EuroScope
//     manual ("Editing and function keys"), the FREQ key - numeric pad '*'
//     by default - sends a non-empty command line to the primary frequency.
//
// LIMITATIONS - read before relying on this:
//   * This drives undocumented UI internals. A future EuroScope version can
//     break it silently. Treat it as best-effort and test after upgrades.
//   * The FREQ key is assumed to be on its DEFAULT binding (numpad '*').
//     If the user remapped it, frequency sends will not work.
//   * Frequency text goes to the PRIMARY frequency only, and EuroScope may
//     prefix the currently selected (ASEL) aircraft's callsign.
//   * Must be called on the EuroScope main thread (any CPlugIn callback).
//
// If injection fails, the previous command-line content is restored and a
// diagnostic message is returned; nothing else is affected.

#include <string>

namespace ChatInjection
{
    struct SendResult
    {
        bool ok = false;
        std::string detail; // diagnostic, suitable for showing to the user
    };

    // Puts `text` into EuroScope's command line and presses ENTER.
    // Used for dot-commands such as ".msg CALLSIGN hello".
    SendResult SendCommandLine(const std::string& text);

    // Puts `text` into EuroScope's command line and presses the FREQ key
    // (numpad '*'), which sends it as a text message to the primary
    // frequency.
    SendResult SendToPrimaryFrequency(const std::string& text);
}
