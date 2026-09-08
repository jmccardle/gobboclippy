#pragma once
#include <string>

#include <SDL3/SDL.h>

#include "Capabilities.h"

// A borderless, always-on-top, transparent window that draws one sprite.
//
// Deliberately NOT click-through: the window is a rectangle that swallows
// clicks over its whole area, transparent corners included. Shaped input
// regions are where the hover/drag/click state machine gets genuinely hard,
// and skipping them removes the worst cross-platform compatibility cliff.
class PetWindow {
public:
    bool create(int size, std::string& error_out);
    void destroy();

    void show();
    void hide();
    void toggle();
    bool visible() const { return m_visible; }

    void render();

    // Replaces the displayed image. Returns false and sets error_out if the
    // file is missing or not decodable -- it does not silently keep the old
    // sprite or substitute a placeholder.
    bool setSprite(const std::string& png_path, std::string& error_out);

    void setPosition(int x, int y);
    void getPosition(int& x, int& y) const;
    void getSize(int& w, int& h) const;

    // Background used when the compositor cannot give us an alpha channel.
    void setFallbackColor(Uint8 r, Uint8 g, Uint8 b);

    SDL_Window* handle() const { return m_window; }
    const Capabilities& caps() const { return m_caps; }

    // Tray availability is only known after Tray::create(), which happens
    // after the window exists, so the host fills that field in later.
    Capabilities& caps_mutable() { return m_caps; }

private:
    SDL_Window*   m_window   = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture*  m_sprite   = nullptr;
    Capabilities  m_caps;
    bool          m_visible  = false;
    int           m_size     = 0;
    SDL_Color     m_fallback { 0x16, 0x18, 0x1c, 0xff };
};
