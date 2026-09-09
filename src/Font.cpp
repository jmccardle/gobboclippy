#include "Font.h"

#include <cmath>
#include <cstring>

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {

// One 512x512 sheet holds ASCII at any size this project renders at (96px
// glyphs still fit). If a caller asks for something larger the pack fails and
// says so rather than dropping the glyphs that did not fit.
const int kAtlasW = 512;
const int kAtlasH = 512;

} // namespace

std::shared_ptr<Font> Font::load(const std::string& path, std::string& error_out)
{
    size_t len  = 0;
    void*  data = SDL_LoadFile(path.c_str(), &len);
    if (!data) {
        error_out = "could not read " + path + ": " + SDL_GetError();
        return nullptr;
    }

    std::shared_ptr<Font> f(new Font());
    f->m_source = path;
    f->m_bytes.assign((unsigned char*)data, (unsigned char*)data + len);
    SDL_free(data);

    // Reject a non-font here rather than at the first Caption that uses it.
    //
    // The offset lookup has to be checked separately: it returns -1 for a file
    // whose header is not a font, and stbtt_InitFont given a negative offset
    // reads out of bounds rather than returning an error. Handing it a PNG is
    // a segfault, not a diagnostic.
    const int offset = stbtt_GetFontOffsetForIndex(f->m_bytes.data(), 0);
    stbtt_fontinfo info;
    if (offset < 0 || !stbtt_InitFont(&info, f->m_bytes.data(), offset)) {
        error_out = path + ": not a TrueType/OpenType font stb_truetype can read";
        return nullptr;
    }

    return f;
}

Uint32 Font::nextCodepoint(const std::string& text, size_t& i)
{
    const size_t n = text.size();
    if (i >= n) return 0;

    const unsigned char b0 = (unsigned char)text[i];
    if (b0 < 0x80) { ++i; return b0; }

    int      extra = 0;
    Uint32   cp    = 0;
    if      ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1Fu; }
    else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0Fu; }
    else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07u; }
    else { ++i; return kReplacement; }        // stray continuation byte

    // The continuation bytes run from i+1 to i+extra, so they all have to be
    // inside the string.
    if (i + (size_t)extra >= n) {
        i = n;                                // truncated sequence at the end
        return kReplacement;
    }

    for (int k = 1; k <= extra; ++k) {
        const unsigned char b = (unsigned char)text[i + (size_t)k];
        if ((b & 0xC0) != 0x80) { ++i; return kReplacement; }
        cp = (cp << 6) | (Uint32)(b & 0x3Fu);
    }
    i += (size_t)extra + 1;
    return cp;
}

Font::~Font()
{
    for (auto& kv : m_atlases) {
        if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
    }
}

const Font::Atlas* Font::atlas(SDL_Renderer* renderer, int pixel_size,
                               std::string& error_out)
{
    if (!renderer) {
        error_out = "Font::atlas: no renderer";
        return nullptr;
    }
    if (pixel_size < 4 || pixel_size > 256) {
        error_out = "font size " + std::to_string(pixel_size) +
                    " out of range (4..256)";
        return nullptr;
    }

    auto it = m_atlases.find(pixel_size);
    if (it != m_atlases.end()) return &it->second;

    std::vector<unsigned char>  coverage((size_t)kAtlasW * kAtlasH, 0);
    std::vector<stbtt_packedchar> packed(kNumCodepoints);

    stbtt_pack_context pc;
    if (!stbtt_PackBegin(&pc, coverage.data(), kAtlasW, kAtlasH, 0, 1, nullptr)) {
        error_out = "stbtt_PackBegin failed for " + m_source;
        return nullptr;
    }
    // 2x oversampling: the captions here are small and scaled, and this is the
    // difference between crisp and mushy at no meaningful cost.
    stbtt_PackSetOversampling(&pc, 2, 2);

    // STBTT_POINT_SIZE means "pixel_size is the em", which is what every other
    // toolkit's font size means and what leaves room between lines. stb's
    // unwrapped default instead makes ascender-to-descender exactly this many
    // pixels, which produces text whose lines touch.
    const int packed_ok = stbtt_PackFontRange(
        &pc, m_bytes.data(), 0, STBTT_POINT_SIZE((float)pixel_size),
        kFirstCodepoint, kNumCodepoints, packed.data());
    stbtt_PackEnd(&pc);

    if (!packed_ok) {
        error_out = m_source + ": ASCII at " + std::to_string(pixel_size) +
                    "px does not fit in a " + std::to_string(kAtlasW) + "x" +
                    std::to_string(kAtlasH) + " atlas";
        return nullptr;
    }

    // stb gives an 8-bit coverage mask; SDL wants a texture. White with the
    // coverage in alpha, so SDL_SetTextureColorMod tints it per Caption.
    std::vector<Uint32> rgba((size_t)kAtlasW * kAtlasH);
    for (size_t i = 0; i < rgba.size(); ++i) {
        const Uint32 a = coverage[i];
        rgba[i] = 0x00FFFFFFu | (a << 24);   // RGBA32 is 0xAABBGGRR on LE
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        kAtlasW, kAtlasH, SDL_PIXELFORMAT_RGBA32, rgba.data(), kAtlasW * 4);
    if (!surface) {
        error_out = std::string("SDL_CreateSurfaceFrom (font atlas): ") + SDL_GetError();
        return nullptr;
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (!tex) {
        error_out = std::string("SDL_CreateTextureFromSurface (font atlas): ") + SDL_GetError();
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);

    stbtt_fontinfo info;
    stbtt_InitFont(&info, m_bytes.data(),
                   stbtt_GetFontOffsetForIndex(m_bytes.data(), 0));
    int a = 0, d = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &a, &d, &gap);
    // Must match the scale stbtt_PackFontRange used above, or the metrics
    // describe glyphs of a different size than the ones in the atlas.
    const float scale = stbtt_ScaleForMappingEmToPixels(&info, (float)pixel_size);

    Atlas atlas;
    atlas.tex         = tex;
    atlas.ascent      = a * scale;
    atlas.line_height = (a - d + gap) * scale;
    atlas.src.resize(kNumCodepoints);
    atlas.quad.resize(kNumCodepoints);
    atlas.advance.resize(kNumCodepoints);

    // Ask stb for each glyph's quad rather than deriving one from the packed
    // rect: it is what divides the oversampled atlas rect back down to display
    // size and applies the sub-pixel shift the oversampling introduced.
    for (int i = 0; i < kNumCodepoints; ++i) {
        float pen_x = 0.0f, pen_y = 0.0f;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(packed.data(), kAtlasW, kAtlasH, i,
                            &pen_x, &pen_y, &q, /*align_to_integer=*/0);

        atlas.src[i]  = SDL_FRect{q.s0 * kAtlasW, q.t0 * kAtlasH,
                                  (q.s1 - q.s0) * kAtlasW,
                                  (q.t1 - q.t0) * kAtlasH};
        atlas.quad[i] = SDL_FRect{q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
        atlas.advance[i] = pen_x;   // GetPackedQuad advanced the pen for us
    }

    auto inserted = m_atlases.emplace(pixel_size, std::move(atlas));
    return &inserted.first->second;
}

bool Font::measure(SDL_Renderer* renderer, const std::string& text,
                   int pixel_size, SDL_FPoint& size_out, std::string& error_out)
{
    const Atlas* at = atlas(renderer, pixel_size, error_out);
    if (!at) return false;

    float widest = 0.0f, pen = 0.0f;
    int   lines  = 1;

    for (size_t i = 0; i < text.size();) {
        const Uint32 cp = nextCodepoint(text, i);
        if (cp == (Uint32)'\n') {
            widest = SDL_max(widest, pen);
            pen    = 0.0f;
            ++lines;
            continue;
        }
        const long idx = (long)cp - kFirstCodepoint;
        if (idx < 0 || idx >= kNumCodepoints) continue;
        pen += at->advance[idx];
    }
    widest = SDL_max(widest, pen);

    size_out = SDL_FPoint{widest, at->line_height * (float)lines};
    return true;
}
