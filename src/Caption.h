#pragma once
#include "Drawable.h"
#include "Font.h"

// A line (or several) of text. McRogueFace's UICaption, on stb_truetype
// instead of sf::Text.
//
// The glyphs are composited into a private texture once and re-blitted every
// frame, which is the dirty-flag idea from UIDrawable::markContentDirty()
// applied where it earns its keep: it makes rotation, per-axis scale and
// mirroring work on text exactly as they do on a sprite, for free, instead of
// needing a transformed quad per glyph.
//
// fill_color is applied at blit time, not baked in, so animating a caption's
// colour or opacity never rebuilds the texture.
class Caption : public Drawable {
public:
    Caption() = default;
    Caption(std::shared_ptr<Font> font, std::string text, int size);

    Kind kind() const override { return Kind::Caption; }

    void               setText(const std::string& text);
    const std::string& text() const { return m_text; }

    void setFont(std::shared_ptr<Font> font);
    const std::shared_ptr<Font>& font() const { return m_font; }

    void setFontSize(int px);
    int  fontSize() const { return m_size; }

    SDL_Color fill_color {255, 255, 255, 255};

    // Codepoints outside the font's ASCII range, skipped when rasterising.
    // Reported rather than silently absorbed: text that renders shorter than
    // it reads is exactly the kind of thing that survives to production.
    //
    // Rasterises first if the text has changed since, so this describes the
    // current text rather than the one before it.
    int skippedGlyphs() const;

    // Empty unless rasterising failed. A Caption that cannot rasterise draws
    // nothing and keeps the reason. Also rasterises first, for the same
    // reason as above.
    const std::string& error() const;

    SDL_FPoint contentSize() const override;

    bool setProperty(const std::string& name, float value) override;
    bool setProperty(const std::string& name, int value) override;
    bool setProperty(const std::string& name, const SDL_Color& value) override;
    bool setProperty(const std::string& name, const std::string& value) override;
    bool getProperty(const std::string& name, float& value) const override;
    bool getProperty(const std::string& name, int& value) const override;
    bool getProperty(const std::string& name, SDL_Color& value) const override;
    bool getProperty(const std::string& name, std::string& value) const override;
    bool hasProperty(const std::string& name) const override;

protected:
    void draw(SDL_Renderer* r, SDL_FPoint offset, float alpha) override;

private:
    // Re-rasterise if anything the texture depends on changed. Safe to call
    // every frame; does nothing when clean.
    void rebuild(SDL_Renderer* r) const;

    // Record a rasterisation failure and log it.
    void fail(const std::string& why) const;

    std::shared_ptr<Font> m_font;
    std::string           m_text;
    int                   m_size = 16;

    // Mutable because contentSize() is const and callers (bounds, alignment)
    // legitimately need an up-to-date measurement before the first draw.
    mutable SDL_Texture* m_tex     = nullptr;
    mutable SDL_FPoint   m_extent  {0.0f, 0.0f};
    mutable bool         m_dirty   = true;
    mutable int          m_skipped = 0;
    mutable std::string  m_error;

public:
    ~Caption() override;
};
