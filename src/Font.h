#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

// A TrueType face, rasterised on demand into per-size glyph atlases.
//
// McRogueFace's PyFont is a thin wrapper over sf::Font, which does the
// rasterising and caching itself. SDL3 core has no text at all, so that part
// is written here on stb_truetype -- which comes from the same stb checkout
// the PNG loader already uses, so it costs no new dependency.
//
// Coverage is ASCII 32..126. That is what the shipped JetBrains Mono is for
// here (labels and short captions), and a partial-coverage font that silently
// drops characters would be worse than one that says so: Font::measure and
// Caption both substitute nothing -- an unsupported codepoint is skipped and
// counted, and Caption reports the count so a caller can see it happened.
class Font {
public:
    static std::shared_ptr<Font> load(const std::string& path,
                                      std::string& error_out);
    ~Font();

    Font(const Font&)            = delete;
    Font& operator=(const Font&) = delete;

    const std::string& source() const { return m_source; }

    // One rasterised size. Owned by the Font; valid until the Font dies.
    struct Atlas {
        SDL_Texture* tex         = nullptr;
        float        ascent      = 0;   // baseline offset from the top, px
        float        line_height = 0;   // ascent - descent + line gap, px

        // Packed metrics for codepoints 32..126, in that order.
        //
        // `src` and `quad` are deliberately separate rectangles. The atlas is
        // rasterised with oversampling, so a glyph's rect in the atlas is
        // larger than the rect it is drawn into -- treating one as the other
        // renders every glyph at double width, overlapping its neighbours.
        std::vector<SDL_FRect> src;      // glyph rect within the atlas, px
        std::vector<SDL_FRect> quad;     // destination rect, relative to the
                                         // pen position on the baseline
        std::vector<float>     advance;  // pen advance
    };

    // Builds the atlas the first time a size is asked for, then caches it.
    // `pixel_size` is the em size, the same thing a CSS `font-size` or an
    // sf::Text character size means -- so a line of text is taller than this,
    // not exactly this.
    //
    // Returns nullptr with error_out set if rasterising fails.
    const Atlas* atlas(SDL_Renderer* renderer, int pixel_size,
                       std::string& error_out);

    // Text extent in pixels at `pixel_size`, honouring '\n'. Returns false and
    // sets error_out only if the atlas could not be built.
    bool measure(SDL_Renderer* renderer, const std::string& text,
                 int pixel_size, SDL_FPoint& size_out, std::string& error_out);

    static const int kFirstCodepoint = 32;
    static const int kNumCodepoints  = 95;   // 32..126 inclusive

    // Decode one UTF-8 codepoint from `text` at byte index `i`, advancing i.
    //
    // Text is counted and skipped in codepoints, not bytes: "—" is one glyph
    // this font does not have, not three unexplained ones, and Caption's
    // skipped-glyph count is only meaningful if it says that. Malformed input
    // yields U+FFFD and advances, so a bad byte cannot loop forever.
    static Uint32 nextCodepoint(const std::string& text, size_t& i);

    static const Uint32 kReplacement = 0xFFFDu;

private:
    Font() = default;

    std::string                m_source;
    std::vector<unsigned char> m_bytes;      // the whole file; stb indexes it
    std::map<int, Atlas>       m_atlases;    // pixel size -> atlas
};
