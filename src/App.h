#pragma once
#include <string>

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
    void setVisible(bool want)
    {
        if (want == window.visible()) return;
        want ? window.show() : window.hide();
        syncTray();
        PyClippy::fire(want ? "show" : "hide");
    }
};
