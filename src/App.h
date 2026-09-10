#pragma once
#include <string>

#include "Mic.h"
#include "PetWindow.h"
#include "PyClippy.h"
#include "Tray.h"

// Shared host context. The C++ side owns the window, the tray and the event
// loop; Python owns policy and behaviour on top of them.
struct App {
    PetWindow window;
    Tray      tray;
    bool      running = true;

    std::string script;     // scripts/clippy.py unless overridden
    int         width  = 256;
    int         height = 256;

    // Keeps the tray menu in step with the window after any visibility change.
    void syncTray() { tray.setVisibleState(window.visible()); }

    // The single path for visibility changes, whatever triggered them -- the
    // tray menu, a Python call, or the window manager. Hooks fire on the
    // transition only, so a script cannot observe a "show" that did not
    // actually change anything, and re-entrant calls from inside a hook are
    // no-ops rather than infinite recursion.
    //
    // Hiding also stops the microphone, and that is the whole point of routing
    // every visibility change through here: the visible pet is the recording
    // indicator, so a pet that cannot be seen must not be recording. The
    // transcriber the script is feeding is left alone -- only the audio stops,
    // and the agent goes on running.
    void setVisible(bool want)
    {
        if (want == window.visible()) return;
        want ? window.show() : window.hide();
        syncTray();

        // Before the hook, not after: no script should be able to observe the
        // window hidden and the microphone still open.
        const bool stopped_mic = !want && Mic::active();
        if (stopped_mic) Mic::stop();

        PyClippy::fire(want ? "show" : "hide");
        if (stopped_mic) PyClippy::fireMic(false);
    }
};
