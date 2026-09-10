#pragma once
#include <memory>
#include <string>

#include <SDL3/SDL.h>

// A PNG, optionally sliced into a grid of equally-sized frames.
//
// This is McRogueFace's PyTexture with SFML swapped out: same atlas model
// (sprite_width x sprite_height cells, indexed left-to-right then top-to-bottom
// as `sprite_index`), same "0 means the whole image is one frame" default.
//
// The loader is the stb_image path that was already in PetWindow.cpp, moved
// here so there is one PNG decode in the program rather than two.
class Texture {
public:
    // Returns nullptr and fills error_out on a missing or undecodable file, or
    // on a cell size that does not divide the image. It never substitutes a
    // placeholder: a sprite sheet that silently loses its last row is a bug you
    // find at the wrong end of a pipeline.
    static std::shared_ptr<Texture> load(SDL_Renderer* renderer,
                                         const std::string& path,
                                         int cell_w, int cell_h,
                                         std::string& error_out);
    ~Texture();

    Texture(const Texture&)            = delete;
    Texture& operator=(const Texture&) = delete;

    SDL_Texture* handle() const { return m_tex; }

    // The same image with every RGB forced to white and the alpha untouched --
    // what a flat halo has to blur, since the halo's colour is meant to come
    // from glow_color rather than from the art.
    //
    // It cannot be made by tinting. SDL_SetTextureColorMod only multiplies, so
    // it can take a colour down to black but never up to white; blurring the
    // art with a white tint just blurs the art, which is a halo the colour of
    // whatever it is surrounding. Hence a second upload.
    //
    // Built on first use and cached, because most textures never appear under
    // glow_flat and the ones that do are usually one per character. Returns
    // nullptr and logs if the source file has gone away since it was loaded --
    // the flat pass then skips this sprite, so the halo is visibly missing a
    // piece rather than quietly coming back the wrong colour.
    SDL_Texture* silhouette(SDL_Renderer* renderer);

    int spriteWidth()  const { return m_sprite_w; }
    int spriteHeight() const { return m_sprite_h; }
    int sheetWidth()   const { return m_sheet_w; }
    int sheetHeight()  const { return m_sheet_h; }
    int spriteCount()  const { return m_sheet_w * m_sheet_h; }

    const std::string& source() const { return m_source; }

    // Source rectangle for one frame. Out-of-range indices wrap, which is what
    // makes a frame list like [0,1,2,1] safe to hand to an animation without
    // the caller re-checking the sheet size every time.
    SDL_FRect frame(int index) const;

    // Nearest keeps generated pixel art crisp; linear is right for the smooth
    // SVG-derived sprites this project actually ships. Chosen per texture
    // because an image pipeline may well produce both.
    void setSmooth(bool smooth);
    bool smooth() const { return m_smooth; }

private:
    Texture() = default;

    SDL_Texture* m_tex      = nullptr;
    SDL_Texture* m_flat     = nullptr;   // lazy; see silhouette()
    bool         m_flat_failed = false;  // so a missing file logs once, not per frame
    std::string  m_source;
    int          m_sprite_w = 0, m_sprite_h = 0;
    int          m_sheet_w  = 0, m_sheet_h  = 0;
    bool         m_smooth   = true;
};
