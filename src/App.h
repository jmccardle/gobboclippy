#pragma once
#include <string>

#include "Mic.h"
#include "PetWindow.h"
#include "PyClippy.h"
#include "Settings.h"
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

    // --- visibility --------------------------------------------------------
    //
    // Two bits, not one, because the settings window needs the pet on screen
    // for a reason that is not "the user asked to see it".
    //
    //   m_shown            what the user asked for. This is what
    //                      clippy.visible() answers, what the show/hide hooks
    //                      report, and what the microphone rule is measured
    //                      against.
    //
    //   m_preview_engaged  a geometry setting has been demonstrated during this
    //                      settings session, so the window stays mapped for the
    //                      rest of it. Cleared when the settings window closes.
    //
    // A window that is mapped while m_shown is false is *previewing*: it is on
    // screen, it is not shown. The pet cannot record in that state, which is
    // the point of deriving the microphone rule from m_shown rather than from
    // whether the window happens to be mapped -- the user believes it is
    // hidden, and a recording indicator that only the program can interpret is
    // not one.
    bool visible() const   { return m_shown; }
    bool previewing() const { return m_preview_engaged && !m_shown; }

    // Keeps the tray menu, and the settings window's banner, in step with both.
    void syncTray()
    {
        tray.setVisibleState(m_shown, m_preview_engaged);
        Settings::setPreviewBanner(previewing());
    }

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
    //
    // Hiding during a settings session leaves the window mapped and demotes it
    // to a preview. Nothing about that changes the above: the hook fires, the
    // microphone stops, and clippy.visible() answers False. What the user sees
    // is the pet still standing where they are dragging it to, with the banner
    // back to say it will go when the dialog does.
    void setVisible(bool want)
    {
        if (want == m_shown) return;
        m_shown = want;
        reconcile();

        // Before the hook, not after: no script should be able to observe the
        // window hidden and the microphone still open.
        const bool stopped_mic = !want && Mic::active();
        if (stopped_mic) Mic::stop();

        PyClippy::fire(want ? "show" : "hide");
        if (stopped_mic) PyClippy::fireMic(false);
    }

    // A settings field is about to demonstrate the window's geometry. Puts the
    // pet on screen if it is not there, without that counting as shown.
    //
    // Only meaningful while the settings window is open, because the banner is
    // the only thing that explains the state and the dialog closing is the only
    // thing that ends it.
    void engagePreview()
    {
        if (m_preview_engaged || !Settings::open()) return;
        m_preview_engaged = true;
        reconcile();
    }

    // The settings window has gone. A pet that was only up to be looked at goes
    // back to hidden; one the user pressed Show on stays.
    void endPreview()
    {
        if (!m_preview_engaged) return;
        m_preview_engaged = false;
        reconcile();
    }

private:
    // The window's mapping follows from the two bits; nothing sets it directly.
    void reconcile()
    {
        const bool want_mapped = m_shown || previewing();
        if (want_mapped != window.visible())
            want_mapped ? window.show() : window.hide();
        syncTray();
    }

    bool m_shown           = false;
    bool m_preview_engaged = false;
};
