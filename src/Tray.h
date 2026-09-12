#pragma once
#include <functional>
#include <string>

#include <SDL3/SDL.h>

// System tray icon with a Show / Hide / Configure / Exit menu.
//
// SDL_Tray is native on each platform: Shell_NotifyIcon on Windows,
// NSStatusItem on macOS, and GTK3 + libayatana-appindicator on Unix. The Unix
// path dlopens those at runtime, so there is no build-time dependency, but a
// machine without them gets no tray. create() reports that rather than leaving
// the user with an invisible, unreachable app.
class Tray {
public:
    using Action = std::function<void()>;

    bool create(const std::string& icon_png,
                const std::string& tooltip,
                std::string& error_out);
    void destroy();

    bool ok() const { return m_tray != nullptr; }

    // Keeps the menu in sync with what the window is actually doing.
    //
    // `preview_mode` is a settings session with the pet's geometry in play. It
    // changes what Hide *does* -- the window stays on screen until the dialog
    // closes -- so it changes what Hide says, rather than letting the user
    // click it and watch nothing happen.
    void setVisibleState(bool logically_visible, bool preview_mode);

    Action on_show;
    Action on_hide;
    Action on_configure;
    Action on_exit;

private:
    static void dispatch(void* userdata, SDL_TrayEntry* entry);

    SDL_Tray*      m_tray  = nullptr;
    SDL_Surface*   m_icon  = nullptr;
    unsigned char* m_pixels = nullptr;   // stb owns this until destroy()
    SDL_TrayEntry* m_show      = nullptr;
    SDL_TrayEntry* m_hide      = nullptr;
    SDL_TrayEntry* m_configure = nullptr;
    SDL_TrayEntry* m_exit      = nullptr;

    // Set only when it differs from what is already on the entry: SDL rebuilds
    // the native menu item for every label change, and this is called from
    // every visibility transition.
    bool m_preview_label = false;
};
