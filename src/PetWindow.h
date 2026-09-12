#pragma once
#include <memory>
#include <string>

#include <SDL3/SDL.h>

#include "Capabilities.h"
#include "Texture.h"

// A borderless, always-on-top, transparent window that draws the stage.
//
// Deliberately NOT click-through: the window is a rectangle that swallows
// clicks over its whole area, transparent corners included. Shaped input
// regions are where the hover/drag/click state machine gets genuinely hard,
// and skipping them removes the worst cross-platform compatibility cliff.
class PetWindow {
public:
    bool create(int width, int height, std::string& error_out);
    void destroy();

    void show();
    void hide();
    void toggle();
    bool visible() const { return m_visible; }

    // Ask the window manager for the above-state again, and mean it.
    //
    // Nothing else in this program does. A pet that is still flagged
    // always-on-top but is no longer stacked that way has no way back short of
    // restarting the process, which is not a state a desktop pet should be able
    // to get stuck in.
    void reassertAlwaysOnTop();

    void render();

    // The one-image shortcut: a single PNG, aspect-preserved and centred,
    // drawn beneath the stage. It is what `clippy.set_sprite()` sets, and it
    // is enough for a static pet. Composition -- parts, parenting, animation
    // -- goes through the stage instead.
    //
    // Returns false and sets error_out if the file is missing or not
    // decodable; it does not silently keep the old sprite or substitute a
    // placeholder.
    bool setSprite(const std::string& png_path, std::string& error_out);

    void setPosition(int x, int y);
    void getPosition(int& x, int& y) const;
    void getSize(int& w, int& h) const;

    // Resizing keeps the stage's idea of its own bounds in step, so alignment
    // stays meaningful. Existing aligned drawables are not moved -- call
    // realign() on the ones that should follow.
    bool setSize(int w, int h, std::string& error_out);

    // The window manager had the last word on the size. Called from the event
    // loop so the stage's bounds follow whatever actually happened, not what
    // was asked for.
    void onResized();

    // Background used when the compositor cannot give us an alpha channel.
    void setFallbackColor(Uint8 r, Uint8 g, Uint8 b);

    SDL_Window*   handle()   const { return m_window; }
    SDL_Renderer* renderer() const { return m_renderer; }
    const Capabilities& caps() const { return m_caps; }

    // Tray availability is only known after Tray::create(), which happens
    // after the window exists, so the host fills that field in later.
    Capabilities& caps_mutable() { return m_caps; }

private:
    void syncStageSize();

    SDL_Window*   m_window   = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    std::shared_ptr<Texture> m_sprite;
    Capabilities  m_caps;
    bool          m_visible  = false;
    SDL_Color     m_fallback { 0x16, 0x18, 0x1c, 0xff };
};
