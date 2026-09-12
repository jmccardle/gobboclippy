#include "PetWindow.h"

#include "Drawable.h"
#include "Effects.h"

bool PetWindow::create(int width, int height, std::string& error_out)
{
    const SDL_WindowFlags requested =
        SDL_WINDOW_BORDERLESS      |   // no titlebar
        SDL_WINDOW_ALWAYS_ON_TOP   |   // float over other windows
        SDL_WINDOW_TRANSPARENT     |   // per-pixel alpha
        SDL_WINDOW_UTILITY         |   // keep out of the taskbar / alt-tab
        SDL_WINDOW_NOT_FOCUSABLE   |   // never steal focus from the user's work
        SDL_WINDOW_HIDDEN;             // shown explicitly, after the first draw

    m_window = SDL_CreateWindow("gobboclippy", width, height, requested);
    if (!m_window) {
        error_out = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }

    m_renderer = SDL_CreateRenderer(m_window, nullptr);
    if (!m_renderer) {
        error_out = std::string("SDL_CreateRenderer: ") + SDL_GetError();
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        return false;
    }

    // Everything drawn on this stage -- textures, font atlases, the captions'
    // private render targets -- is created against this renderer, including
    // from Python, so the stage is where it is published.
    Stage::instance().renderer = m_renderer;
    syncStageSize();

    m_caps = Capabilities::probe(m_window, requested);
    return true;
}

void PetWindow::destroy()
{
    // The stage holds drawables that own textures made on this renderer, so it
    // has to be emptied before the renderer goes.
    Stage::instance().clear();
    Stage::instance().renderer = nullptr;

    // Same reason as the stage: the effect compositor keeps a scratch target
    // made on this renderer.
    Effects::shutdown();

    m_sprite.reset();
    if (m_renderer) { SDL_DestroyRenderer(m_renderer); m_renderer = nullptr; }
    if (m_window)   { SDL_DestroyWindow(m_window);     m_window   = nullptr; }
}

void PetWindow::syncStageSize()
{
    int w = 0, h = 0;
    if (m_window) SDL_GetWindowSize(m_window, &w, &h);
    Stage::instance().size = SDL_FPoint{(float)w, (float)h};
}

bool PetWindow::setSprite(const std::string& png_path, std::string& error_out)
{
    auto tex = Texture::load(m_renderer, png_path, 0, 0, error_out);
    if (!tex) return false;
    m_sprite = std::move(tex);
    return true;
}

bool PetWindow::setSize(int w, int h, std::string& error_out)
{
    if (!m_window) {
        error_out = "setSize: no window";
        return false;
    }
    if (!SDL_SetWindowSize(m_window, w, h)) {
        error_out = std::string("SDL_SetWindowSize: ") + SDL_GetError();
        return false;
    }

    // SDL_SetWindowSize is a request to the window manager, not a change. On
    // X11 a hidden window's size does not take effect until it is mapped, so
    // without this the very next SDL_GetWindowSize -- and every alignment
    // computed from it -- still reports the old size and nothing says so.
    //
    // A failed sync is not fatal: SDL_EVENT_WINDOW_RESIZED still arrives later
    // and onResized() picks the change up then. It is worth reporting, since
    // anything aligned before that point lands against the old bounds.
    if (!SDL_SyncWindow(m_window)) {
        SDL_Log("setSize(%d, %d): window did not sync (%s); alignment will "
                "catch up on the next resize event", w, h, SDL_GetError());
    }

    syncStageSize();
    return true;
}

void PetWindow::onResized()
{
    syncStageSize();
}

void PetWindow::render()
{
    if (!m_renderer) return;

    if (m_caps.transparent) {
        // Write alpha 0 rather than blending into it, so the compositor sees a
        // genuinely empty buffer where nothing is drawn.
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
    } else {
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(m_renderer, m_fallback.r, m_fallback.g,
                               m_fallback.b, 255);
    }
    SDL_RenderClear(m_renderer);
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);

    int ww = 0, wh = 0;
    SDL_GetWindowSize(m_window, &ww, &wh);

    if (m_sprite) {
        const float tw = (float)m_sprite->spriteWidth();
        const float th = (float)m_sprite->spriteHeight();

        // Contain: preserve aspect ratio, centre in the window.
        const float scale = SDL_min((float)ww / tw, (float)wh / th);
        SDL_FRect dst;
        dst.w = tw * scale;
        dst.h = th * scale;
        dst.x = ((float)ww - dst.w) * 0.5f;
        dst.y = ((float)wh - dst.h) * 0.5f;

        SDL_RenderTexture(m_renderer, m_sprite->handle(), nullptr, &dst);
    }

    // The stage draws in its own coordinates -- window pixels, origin top-left
    // -- so SDL clips anything outside the window for us. That is what makes a
    // slide-in animation a plain position tween.
    Stage::instance().render(m_renderer);

    SDL_RenderPresent(m_renderer);
}

void PetWindow::show()
{
    if (!m_window) return;
    render();                 // draw before mapping, so no blank frame flashes
    SDL_ShowWindow(m_window);

    // Mapping is a request too, and on X11 the window manager places the window
    // as it maps it. Anything that moves the window between the request and the
    // map -- which is exactly what the settings window's geometry preview does,
    // since showing the pet is the first half of demonstrating where it goes --
    // is overwritten by that placement, arriving late and silently.
    SDL_SyncWindow(m_window);

    // Re-assert stacking: some window managers drop the above-state when a
    // window is unmapped and remapped.
    SDL_SetWindowAlwaysOnTop(m_window, true);
    m_visible = true;
}

void PetWindow::hide()
{
    if (!m_window) return;
    SDL_HideWindow(m_window);
    m_visible = false;
}

void PetWindow::toggle() { m_visible ? hide() : show(); }

void PetWindow::setPosition(int x, int y)
{
    if (!m_window) return;
    SDL_SetWindowPosition(m_window, x, y);

    // The same request-not-a-change that setSize() documents, and for the same
    // reason: without the sync, SDL_GetWindowPosition answers with what was
    // asked for rather than with what the window manager did, and a position
    // that reads back correct while the window never moved is the one failure
    // this project refuses to ship (see Capabilities.cpp on Wayland's
    // always-on-top). A window manager that declines is then visible as a
    // position that does not match.
    SDL_SyncWindow(m_window);
}

void PetWindow::getPosition(int& x, int& y) const
{
    x = y = 0;
    if (m_window) SDL_GetWindowPosition(m_window, &x, &y);
}

void PetWindow::getSize(int& w, int& h) const
{
    w = h = 0;
    if (m_window) SDL_GetWindowSize(m_window, &w, &h);
}

void PetWindow::setFallbackColor(Uint8 r, Uint8 g, Uint8 b)
{
    m_fallback.r = r; m_fallback.g = g; m_fallback.b = b; m_fallback.a = 255;
}
