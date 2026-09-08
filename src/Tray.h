#pragma once
#include <functional>
#include <string>

#include <SDL3/SDL.h>

// System tray icon with a Show / Hide / Exit menu.
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

    // Keeps the menu label in sync with what the window is actually doing.
    void setVisibleState(bool window_visible);

    Action on_show;
    Action on_hide;
    Action on_exit;

private:
    static void dispatch(void* userdata, SDL_TrayEntry* entry);

    SDL_Tray*      m_tray  = nullptr;
    SDL_Surface*   m_icon  = nullptr;
    unsigned char* m_pixels = nullptr;   // stb owns this until destroy()
    SDL_TrayEntry* m_show  = nullptr;
    SDL_TrayEntry* m_hide  = nullptr;
    SDL_TrayEntry* m_exit  = nullptr;
};
