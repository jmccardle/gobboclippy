#include "PetWindow.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

bool PetWindow::create(int size, std::string& error_out)
{
    m_size = size;

    const SDL_WindowFlags requested =
        SDL_WINDOW_BORDERLESS      |   // no titlebar
        SDL_WINDOW_ALWAYS_ON_TOP   |   // float over other windows
        SDL_WINDOW_TRANSPARENT     |   // per-pixel alpha
        SDL_WINDOW_UTILITY         |   // keep out of the taskbar / alt-tab
        SDL_WINDOW_NOT_FOCUSABLE   |   // never steal focus from the user's work
        SDL_WINDOW_HIDDEN;             // shown explicitly, after the first draw

    m_window = SDL_CreateWindow("gobboclippy", size, size, requested);
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

    m_caps = Capabilities::probe(m_window, requested);
    return true;
}

void PetWindow::destroy()
{
    if (m_sprite)   { SDL_DestroyTexture(m_sprite);  m_sprite = nullptr; }
    if (m_renderer) { SDL_DestroyRenderer(m_renderer); m_renderer = nullptr; }
    if (m_window)   { SDL_DestroyWindow(m_window);   m_window = nullptr; }
}

bool PetWindow::setSprite(const std::string& png_path, std::string& error_out)
{
    int w = 0, h = 0, channels = 0;
    // Force 4 channels: we always want RGBA regardless of how the PNG was saved.
    unsigned char* pixels = stbi_load(png_path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        error_out = "stbi_load(" + png_path + "): " +
                    (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return false;
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) {
        error_out = std::string("SDL_CreateSurfaceFrom: ") + SDL_GetError();
        stbi_image_free(pixels);
        return false;
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(m_renderer, surface);
    SDL_DestroySurface(surface);
    stbi_image_free(pixels);

    if (!tex) {
        error_out = std::string("SDL_CreateTextureFromSurface: ") + SDL_GetError();
        return false;
    }

    // Nearest-neighbour keeps pixel art crisp; this is the equivalent of
    // image-rendering: pixelated. Harmless for the smooth SVG-derived sprite.
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    if (m_sprite) SDL_DestroyTexture(m_sprite);
    m_sprite = tex;
    return true;
}

void PetWindow::render()
{
    if (!m_renderer) return;

    if (m_caps.transparent) {
        // Write alpha 0 rather than blending into it, so the compositor sees a
        // genuinely empty buffer where the sprite is not drawn.
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
    } else {
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(m_renderer, m_fallback.r, m_fallback.g,
                               m_fallback.b, 255);
    }
    SDL_RenderClear(m_renderer);

    if (m_sprite) {
        SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
        float tw = 0, th = 0;
        SDL_GetTextureSize(m_sprite, &tw, &th);

        int ww = 0, wh = 0;
        SDL_GetWindowSize(m_window, &ww, &wh);

        // Contain: preserve aspect ratio, centre in the window.
        const float scale = SDL_min((float)ww / tw, (float)wh / th);
        SDL_FRect dst;
        dst.w = tw * scale;
        dst.h = th * scale;
        dst.x = ((float)ww - dst.w) * 0.5f;
        dst.y = ((float)wh - dst.h) * 0.5f;

        SDL_RenderTexture(m_renderer, m_sprite, nullptr, &dst);
    }

    SDL_RenderPresent(m_renderer);
}

void PetWindow::show()
{
    if (!m_window) return;
    render();                 // draw before mapping, so no blank frame flashes
    SDL_ShowWindow(m_window);
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
    if (m_window) SDL_SetWindowPosition(m_window, x, y);
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
