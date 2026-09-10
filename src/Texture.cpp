#include "Texture.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

std::shared_ptr<Texture> Texture::load(SDL_Renderer* renderer,
                                       const std::string& path,
                                       int cell_w, int cell_h,
                                       std::string& error_out)
{
    if (!renderer) {
        error_out = "Texture::load: no renderer";
        return nullptr;
    }

    int w = 0, h = 0, channels = 0;
    // Force 4 channels: we always want RGBA regardless of how the PNG was saved.
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        error_out = "stbi_load(" + path + "): " +
                    (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return nullptr;
    }

    // 0 means "the whole image is one frame" -- McRogueFace's default, and the
    // right one for a PNG that came straight out of a generator.
    if (cell_w <= 0) cell_w = w;
    if (cell_h <= 0) cell_h = h;

    if (w % cell_w != 0 || h % cell_h != 0) {
        stbi_image_free(pixels);
        error_out = path + ": " + std::to_string(w) + "x" + std::to_string(h) +
                    " does not divide evenly into " + std::to_string(cell_w) +
                    "x" + std::to_string(cell_h) + " cells";
        return nullptr;
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) {
        error_out = std::string("SDL_CreateSurfaceFrom: ") + SDL_GetError();
        stbi_image_free(pixels);
        return nullptr;
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    stbi_image_free(pixels);

    if (!tex) {
        error_out = std::string("SDL_CreateTextureFromSurface: ") + SDL_GetError();
        return nullptr;
    }

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);

    // make_shared cannot reach the private constructor, so wrap explicitly.
    std::shared_ptr<Texture> t(new Texture());
    t->m_tex      = tex;
    t->m_source   = path;
    t->m_sprite_w = cell_w;
    t->m_sprite_h = cell_h;
    t->m_sheet_w  = w / cell_w;
    t->m_sheet_h  = h / cell_h;
    return t;
}

Texture::~Texture()
{
    if (m_tex)  SDL_DestroyTexture(m_tex);
    if (m_flat) SDL_DestroyTexture(m_flat);
}

SDL_Texture* Texture::silhouette(SDL_Renderer* renderer)
{
    if (m_flat)        return m_flat;
    if (m_flat_failed) return nullptr;

    // Decoded again rather than kept from load(): every texture would otherwise
    // carry a copy of its pixels for a pass most of them never take.
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(m_source.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        m_flat_failed = true;
        SDL_Log("texture '%s': cannot build the glow_flat silhouette: %s",
                m_source.c_str(),
                stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return nullptr;
    }

    // Keep the alpha, throw the colour away. RGBA32 is 0xAABBGGRR on a
    // little-endian host, so this is the top byte masked back in.
    Uint32* px = (Uint32*)pixels;
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        px[i] = 0x00FFFFFFu | (px[i] & 0xFF000000u);
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (surface) {
        m_flat = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_DestroySurface(surface);
    }
    stbi_image_free(pixels);

    if (!m_flat) {
        m_flat_failed = true;
        SDL_Log("texture '%s': cannot upload the glow_flat silhouette: %s",
                m_source.c_str(), SDL_GetError());
        return nullptr;
    }

    SDL_SetTextureBlendMode(m_flat, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(m_flat, m_smooth ? SDL_SCALEMODE_LINEAR
                                             : SDL_SCALEMODE_NEAREST);
    return m_flat;
}

SDL_FRect Texture::frame(int index) const
{
    const int total = spriteCount();
    if (total <= 0) return SDL_FRect{0, 0, 0, 0};

    // Positive modulo, so a negative index counts back from the end rather
    // than reading outside the sheet.
    int i = index % total;
    if (i < 0) i += total;

    const int col = i % m_sheet_w;
    const int row = i / m_sheet_w;
    return SDL_FRect{
        (float)(col * m_sprite_w), (float)(row * m_sprite_h),
        (float)m_sprite_w,         (float)m_sprite_h};
}

void Texture::setSmooth(bool smooth)
{
    m_smooth = smooth;
    const SDL_ScaleMode mode = smooth ? SDL_SCALEMODE_LINEAR
                                      : SDL_SCALEMODE_NEAREST;
    if (m_tex)  SDL_SetTextureScaleMode(m_tex, mode);
    if (m_flat) SDL_SetTextureScaleMode(m_flat, mode);
}
