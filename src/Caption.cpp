#include "Caption.h"

#include <cmath>

Caption::Caption(std::shared_ptr<Font> font, std::string text, int size)
    : m_font(std::move(font)), m_text(std::move(text)), m_size(size)
{
}

Caption::~Caption()
{
    if (m_tex) SDL_DestroyTexture(m_tex);
}

void Caption::fail(const std::string& why) const
{
    m_error = why;
    // A caption that cannot rasterise draws nothing, and blank space says
    // nothing about why. Logged once per distinct failure rather than every
    // frame -- rebuild() only runs when something actually changed.
    SDL_Log("caption '%s': %s", name.empty() ? m_text.c_str() : name.c_str(),
            why.c_str());
}

void Caption::setText(const std::string& text)
{
    if (text == m_text) return;
    m_text  = text;
    m_dirty = true;
}

void Caption::setFont(std::shared_ptr<Font> font)
{
    m_font  = std::move(font);
    m_dirty = true;
}

void Caption::setFontSize(int px)
{
    if (px == m_size) return;
    m_size  = px;
    m_dirty = true;
}

SDL_FPoint Caption::contentSize() const
{
    rebuild(Stage::instance().renderer);
    return m_extent;
}

int Caption::skippedGlyphs() const
{
    rebuild(Stage::instance().renderer);
    return m_skipped;
}

const std::string& Caption::error() const
{
    rebuild(Stage::instance().renderer);
    return m_error;
}

void Caption::rebuild(SDL_Renderer* r) const
{
    if (!m_dirty) return;
    if (!r || !m_font) return;   // stay dirty; retry when the host is ready

    m_dirty   = false;
    m_skipped = 0;
    m_error.clear();
    m_extent  = SDL_FPoint{0.0f, 0.0f};

    if (m_tex) { SDL_DestroyTexture(m_tex); m_tex = nullptr; }

    std::string err;
    const Font::Atlas* at = m_font->atlas(r, m_size, err);
    if (!at) { fail(err); return; }

    if (!m_font->measure(r, m_text, m_size, m_extent, err)) {
        m_extent = SDL_FPoint{0.0f, 0.0f};
        fail(err);
        return;
    }
    if (m_extent.x < 1.0f || m_extent.y < 1.0f) return;   // nothing to draw

    const int tw = (int)std::ceil(m_extent.x);
    const int th = (int)std::ceil(m_extent.y);

    SDL_Texture* target = SDL_CreateTexture(
        r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, tw, th);
    if (!target) {
        fail(std::string("SDL_CreateTexture (caption): ") + SDL_GetError());
        return;
    }
    // What comes out of a render target composited with SDL_BLENDMODE_BLEND is
    // premultiplied, so it has to go back out that way too. See Drawable::blit.
    SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetTextureScaleMode(target, SDL_SCALEMODE_LINEAR);

    SDL_Texture* previous = SDL_GetRenderTarget(r);
    if (!SDL_SetRenderTarget(r, target)) {
        fail(std::string("SDL_SetRenderTarget (caption): ") + SDL_GetError());
        SDL_DestroyTexture(target);
        return;
    }

    // Write a genuinely empty buffer rather than blending into whatever the
    // texture happened to contain.
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    // The atlas is white; the tint happens at blit time in draw().
    SDL_SetTextureColorMod(at->tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(at->tex, 255);

    float pen_x = 0.0f;
    float baseline = at->ascent;

    for (size_t i = 0; i < m_text.size();) {
        const Uint32 cp = Font::nextCodepoint(m_text, i);
        if (cp == (Uint32)'\n') {
            pen_x     = 0.0f;
            baseline += at->line_height;
            continue;
        }
        const long idx = (long)cp - Font::kFirstCodepoint;
        if (idx < 0 || idx >= Font::kNumCodepoints) { ++m_skipped; continue; }

        const SDL_FRect& src  = at->src[idx];
        const SDL_FRect& quad = at->quad[idx];
        if (src.w > 0.0f && src.h > 0.0f) {
            // The quad is relative to the pen sitting on the baseline.
            SDL_FRect dst{pen_x + quad.x, baseline + quad.y, quad.w, quad.h};
            SDL_RenderTexture(r, at->tex, &src, &dst);
        }
        pen_x += at->advance[idx];
    }

    SDL_SetRenderTarget(r, previous);
    m_tex = target;
}

void Caption::draw(SDL_Renderer* r, SDL_FPoint offset, float alpha)
{
    rebuild(r);
    if (!m_tex) return;
    blit(r, m_tex, nullptr, offset, alpha, fill_color, /*premultiplied=*/true);
}

// --- property system -------------------------------------------------------

bool Caption::setProperty(const std::string& name, float value)
{
    if (name == "font_size") { setFontSize((int)SDL_lroundf(value)); return true; }
    return Drawable::setProperty(name, value);
}

bool Caption::setProperty(const std::string& name, int value)
{
    if (name == "font_size") { setFontSize(value); return true; }
    return Drawable::setProperty(name, value);
}

bool Caption::setProperty(const std::string& name, const SDL_Color& value)
{
    if (name == "fill_color") { fill_color = value; return true; }
    return Drawable::setProperty(name, value);
}

bool Caption::setProperty(const std::string& name, const std::string& value)
{
    if (name == "text") { setText(value); return true; }
    return Drawable::setProperty(name, value);
}

bool Caption::getProperty(const std::string& name, float& value) const
{
    if (name == "font_size") { value = (float)m_size; return true; }
    return Drawable::getProperty(name, value);
}

bool Caption::getProperty(const std::string& name, int& value) const
{
    if (name == "font_size") { value = m_size; return true; }
    return Drawable::getProperty(name, value);
}

bool Caption::getProperty(const std::string& name, SDL_Color& value) const
{
    if (name == "fill_color") { value = fill_color; return true; }
    return Drawable::getProperty(name, value);
}

bool Caption::getProperty(const std::string& name, std::string& value) const
{
    if (name == "text") { value = m_text; return true; }
    return Drawable::getProperty(name, value);
}

bool Caption::hasProperty(const std::string& name) const
{
    return name == "text" || name == "font_size" || name == "fill_color" ||
           Drawable::hasProperty(name);
}
