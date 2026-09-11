#include "Texture.h"

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include <webp/decode.h>

namespace {

// A WebP file is a RIFF container whose form type is "WEBP": 'R','I','F','F',
// four length bytes, then 'W','E','B','P'. Twelve bytes is the whole test, and
// it is the file's own claim about itself rather than a guess from the
// extension -- petdex names some sheets .png and serves WebP bytes.
bool isWebP(const unsigned char* head, size_t len)
{
    return len >= 12 && std::memcmp(head, "RIFF", 4) == 0 &&
           std::memcmp(head + 8, "WEBP", 4) == 0;
}

// One image file to RGBA pixels, whichever of the two decoders owns it.
//
// Both callers need this -- load() and silhouette(), which decodes the source
// a second time rather than keeping a copy of every texture's pixels for a
// pass most of them never take. Having it in one place is not tidiness: while
// this lived only in load(), a WebP sprite with glow_flat lost its halo and
// said so only in the log, because silhouette() still went straight to stb.
//
// `webp_out` tells the caller which allocator to free with. Crossing those
// over is silent until it is not.
unsigned char* decodeRGBA(const std::string& path, int& w, int& h,
                          bool& webp_out, std::string& error_out)
{
    size_t file_len  = 0;
    void*  file_data = SDL_LoadFile(path.c_str(), &file_len);
    if (!file_data) {
        error_out = "could not read " + path + ": " + SDL_GetError();
        return nullptr;
    }

    const unsigned char* bytes = static_cast<const unsigned char*>(file_data);
    webp_out = isWebP(bytes, file_len);

    unsigned char* pixels = nullptr;
    if (webp_out) {
        // WebPDecodeRGBA gives exactly the layout the PNG path forces with its
        // 4-channel request, so every caller sees one format.
        pixels = WebPDecodeRGBA(bytes, file_len, &w, &h);
        if (!pixels) {
            error_out = path + ": not a decodable WebP (truncated, or an "
                               "animation, which this does not read)";
        }
    } else {
        int channels = 0;
        // Force 4 channels: we always want RGBA regardless of how the PNG was
        // saved.
        pixels = stbi_load_from_memory(bytes, (int)file_len, &w, &h, &channels, 4);
        if (!pixels) {
            error_out = "stbi_load(" + path + "): " +
                        (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        }
    }

    SDL_free(file_data);
    return pixels;
}

void freeRGBA(unsigned char* pixels, bool webp)
{
    if (webp) WebPFree(pixels); else stbi_image_free(pixels);
}

} // namespace

std::shared_ptr<Texture> Texture::load(SDL_Renderer* renderer,
                                       const std::string& path,
                                       int cell_w, int cell_h,
                                       std::string& error_out)
{
    if (!renderer) {
        error_out = "Texture::load: no renderer";
        return nullptr;
    }

    int  w = 0, h = 0;
    bool webp = false;
    unsigned char* pixels = decodeRGBA(path, w, h, webp, error_out);
    if (!pixels) return nullptr;

    auto free_pixels = [webp](unsigned char* p) { freeRGBA(p, webp); };

    // 0 means "the whole image is one frame" -- McRogueFace's default, and the
    // right one for a PNG that came straight out of a generator.
    if (cell_w <= 0) cell_w = w;
    if (cell_h <= 0) cell_h = h;

    if (w % cell_w != 0 || h % cell_h != 0) {
        free_pixels(pixels);
        error_out = path + ": " + std::to_string(w) + "x" + std::to_string(h) +
                    " does not divide evenly into " + std::to_string(cell_w) +
                    "x" + std::to_string(cell_h) + " cells";
        return nullptr;
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4);
    if (!surface) {
        error_out = std::string("SDL_CreateSurfaceFrom: ") + SDL_GetError();
        free_pixels(pixels);
        return nullptr;
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    free_pixels(pixels);

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
    // carry a copy of its pixels for a pass most of them never take. Through
    // the same helper load() uses, so this reads every format the program does
    // -- going straight to stb here is how a WebP sprite loses its halo.
    int  w = 0, h = 0;
    bool webp = false;
    std::string err;
    unsigned char* pixels = decodeRGBA(m_source, w, h, webp, err);
    if (!pixels) {
        m_flat_failed = true;
        SDL_Log("texture '%s': cannot build the glow_flat silhouette: %s",
                m_source.c_str(), err.c_str());
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
    freeRGBA(pixels, webp);

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
