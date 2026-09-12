#include "Tray.h"

#include "stb_image.h"   // implementation lives in PetWindow.cpp

bool Tray::create(const std::string& icon_png,
                  const std::string& tooltip,
                  std::string& error_out)
{
    int w = 0, h = 0, channels = 0;
    m_pixels = stbi_load(icon_png.c_str(), &w, &h, &channels, 4);
    if (!m_pixels) {
        error_out = "tray icon " + icon_png + ": " +
                    (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return false;
    }

    // SDL_CreateSurfaceFrom does not copy; the pixels must outlive the surface.
    m_icon = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, m_pixels, w * 4);
    if (!m_icon) {
        error_out = std::string("SDL_CreateSurfaceFrom (tray): ") + SDL_GetError();
        stbi_image_free(m_pixels);
        m_pixels = nullptr;
        return false;
    }

    m_tray = SDL_CreateTray(m_icon, tooltip.c_str());
    if (!m_tray) {
        error_out = std::string("SDL_CreateTray: ") + SDL_GetError();
        return false;
    }

    SDL_TrayMenu* menu = SDL_CreateTrayMenu(m_tray);
    if (!menu) {
        error_out = std::string("SDL_CreateTrayMenu: ") + SDL_GetError();
        return false;
    }

    m_show = SDL_InsertTrayEntryAt(menu, -1, "Show", SDL_TRAYENTRY_BUTTON);
    m_hide = SDL_InsertTrayEntryAt(menu, -1, "Hide", SDL_TRAYENTRY_BUTTON);
    SDL_InsertTrayEntryAt(menu, -1, nullptr, 0);        // separator
    m_configure = SDL_InsertTrayEntryAt(menu, -1, "Configure...", SDL_TRAYENTRY_BUTTON);
    SDL_InsertTrayEntryAt(menu, -1, nullptr, 0);        // separator
    m_exit = SDL_InsertTrayEntryAt(menu, -1, "Exit", SDL_TRAYENTRY_BUTTON);

    if (!m_show || !m_hide || !m_configure || !m_exit) {
        error_out = std::string("SDL_InsertTrayEntryAt: ") + SDL_GetError();
        return false;
    }

    SDL_SetTrayEntryCallback(m_show,      &Tray::dispatch, this);
    SDL_SetTrayEntryCallback(m_hide,      &Tray::dispatch, this);
    SDL_SetTrayEntryCallback(m_configure, &Tray::dispatch, this);
    SDL_SetTrayEntryCallback(m_exit,      &Tray::dispatch, this);
    return true;
}

void Tray::dispatch(void* userdata, SDL_TrayEntry* entry)
{
    auto* self = static_cast<Tray*>(userdata);
    if (!self) return;

    if (entry == self->m_show && self->on_show) self->on_show();
    else if (entry == self->m_hide && self->on_hide) self->on_hide();
    else if (entry == self->m_configure && self->on_configure) self->on_configure();
    else if (entry == self->m_exit && self->on_exit) self->on_exit();
}

void Tray::setVisibleState(bool logically_visible, bool preview_mode)
{
    // Grey out the action that would be a no-op, so the menu always reflects
    // the real window state.
    if (m_show) SDL_SetTrayEntryEnabled(m_show, !logically_visible);
    if (m_hide) SDL_SetTrayEntryEnabled(m_hide, logically_visible);

    // In a settings session with the geometry in play, Hide stops meaning "the
    // pet leaves the screen" -- it goes back to being a positioning preview
    // until the dialog closes. Saying so is cheaper than the user clicking it
    // twice and concluding the tray is broken.
    if (m_hide && preview_mode != m_preview_label) {
        SDL_SetTrayEntryLabel(m_hide, preview_mode ? "Hide (preview is on)" : "Hide");
        m_preview_label = preview_mode;
    }
}

void Tray::destroy()
{
    if (m_tray) { SDL_DestroyTray(m_tray); m_tray = nullptr; }
    if (m_icon) { SDL_DestroySurface(m_icon); m_icon = nullptr; }
    if (m_pixels) { stbi_image_free(m_pixels); m_pixels = nullptr; }
    m_show = m_hide = m_configure = m_exit = nullptr;
    m_preview_label = false;
}
