#include "Effects.h"

#include <cmath>

namespace {

// How many compounding "over" passes glow_hardness == 1.0 is worth. Six takes a
// half-covered halo pixel to 1 - 0.5^7 ~= 0.99, which reads as a solid outline.
const int kHardnessPasses = 6;

// Colour modulation for a premultiplied texture.
//
// SDL modulates colour and alpha independently, but premultiplied colour has to
// be scaled by the same factor as its alpha or fading makes the texture
// brighter as it disappears. Drawable::blit folds the fade into the colour
// modulation for exactly this reason; the composites here do the same.
void setPremultipliedMod(SDL_Texture* tex, SDL_Color tint, float alpha)
{
    const float a = SDL_clamp(alpha, 0.0f, 1.0f);
    const Uint8 ea = (Uint8)SDL_lroundf(a * (float)tint.a);
    SDL_SetTextureColorMod(tex,
                           (Uint8)((tint.r * ea) / 255),
                           (Uint8)((tint.g * ea) / 255),
                           (Uint8)((tint.b * ea) / 255));
    SDL_SetTextureAlphaMod(tex, ea);
}

// A scratch render target shared by every glow, reused across frames.
//
// It grows to fit and never shrinks, and callers use the top-left sw x sh of
// it rather than the whole thing. Sizing it exactly would be simpler, but the
// texture is shared: two glowing drawables of different sizes would then take
// turns reallocating it, once each per drawable per frame, and the cost would
// appear only in the scene that happens to have two of them. Growing only means
// the allocations stop after the first few frames whatever is on the stage.
//
// Allocating a texture every frame at 60Hz is the kind of thing that puts an
// idle desktop pet in `top`.
//
// No destructor: it is released through Effects::shutdown() while the renderer
// is still alive, rather than at static teardown after SDL_Quit.
struct Scratch {
    SDL_Renderer* owner = nullptr;
    SDL_Texture*  tex   = nullptr;
    int           w     = 0;
    int           h     = 0;

    SDL_Texture* get(SDL_Renderer* r, int want_w, int want_h)
    {
        if (tex && owner == r && w >= want_w && h >= want_h) return tex;

        const int new_w = (owner == r) ? SDL_max(w, want_w) : want_w;
        const int new_h = (owner == r) ? SDL_max(h, want_h) : want_h;

        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_TARGET, new_w, new_h);
        owner = r;
        w     = new_w;
        h     = new_h;
        if (tex) {
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            // The blur *is* this filter. Without it the downsample/upsample is
            // a mosaic rather than a blur.
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        }
        return tex;
    }

    void release()
    {
        if (tex) SDL_DestroyTexture(tex);
        tex   = nullptr;
        owner = nullptr;
        w = h = 0;
    }
};

// Two, because the downsample chain ping-pongs: each halving reads the texture
// the previous one wrote, and a texture cannot be its own render target.
Scratch g_blur[2];

// Clear the current target to a genuinely empty buffer rather than blending
// into whatever the texture happened to contain.
//
// The whole scratch is cleared, not just the part about to be used: the scratch
// only ever grows, so the border outside the used region would otherwise hold a
// previous frame's pixels, and the bilinear tap at the edge reads exactly there.
void clearTransparent(SDL_Renderer* r)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
    SDL_RenderClear(r);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
}

// Shrink `src` to dst_w x dst_h by halving repeatedly, leaving the result in one
// of the scratch targets. `used` receives the sub-rectangle of that target the
// result occupies. Returns nullptr on allocation failure, or `src` itself when
// no reduction was needed.
//
// Each step is a straight copy (BLENDMODE_NONE) into a cleared target, so the
// only filtering is the halving itself.
SDL_Texture* downsample(SDL_Renderer* r, SDL_Texture* src, int src_w, int src_h,
                        int dst_w, int dst_h, SDL_FRect& used)
{
    used = SDL_FRect{0.0f, 0.0f, (float)src_w, (float)src_h};
    if (src_w <= dst_w && src_h <= dst_h) return src;

    SDL_Texture* source = src;
    int          cw = src_w, ch = src_h;
    int          slot = 0;
    bool         first = true;

    while (cw > dst_w || ch > dst_h) {
        const int nw = SDL_max(dst_w, cw / 2);
        const int nh = SDL_max(dst_h, ch / 2);

        SDL_Texture* target = g_blur[slot].get(r, nw, nh);
        if (!target) return nullptr;
        if (!SDL_SetRenderTarget(r, target)) return nullptr;
        clearTransparent(r);

        SDL_SetTextureBlendMode(source, SDL_BLENDMODE_NONE);
        SDL_SetTextureColorMod(source, 255, 255, 255);
        SDL_SetTextureAlphaMod(source, 255);

        const SDL_FRect into{0.0f, 0.0f, (float)nw, (float)nh};
        SDL_RenderTexture(r, source, first ? nullptr : &used, &into);

        source = target;
        used   = into;
        cw     = nw;
        ch     = nh;
        slot  ^= 1;      // never render into the texture just read from
        first  = false;
    }

    return source;
}

} // namespace

namespace Effects {

void plain(SDL_Renderer* r, SDL_Texture* src, const SDL_FRect& dst, float alpha)
{
    if (!src) return;
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    setPremultipliedMod(src, SDL_Color{255, 255, 255, 255}, alpha);
    SDL_RenderTexture(r, src, nullptr, &dst);
}

bool glow(SDL_Renderer* r, SDL_Texture* src, const SDL_FRect& dst,
          const GlowParams& p)
{
    const float radius = p.radius;
    const float alpha  = p.alpha;
    if (!src || radius <= 0.0f || dst.w <= 0.0f || dst.h <= 0.0f) return true;

    // Upscaling the small target ramps from covered to uncovered across one of
    // its texels, and that ramp is the halo. One texel covers dst.w/sw pixels,
    // so sw = dst.w/radius scales the falloff with the requested radius.
    //
    // `radius` is the bound, not the measurement: where the ramp actually
    // starts depends on how the subtree's edges fall relative to texel centres,
    // so the halo fades out *within* radius rather than exactly at it. Measured
    // at radius 20 on a 48px sprite, it reaches about half that. What matters is
    // the direction of the inequality -- the padding Drawable::renderWithEffects
    // reserves is radius + 1, so the halo can never be clipped by its own
    // target.
    //
    // Floored at 4 texels: below that the "silhouette" is one or two blocks and
    // the halo stops resembling the subtree at all. A huge radius on a small
    // subtree therefore stops getting softer, rather than degenerating into a
    // rectangle.
    const float factor = SDL_max(1.0f, radius);
    const int   sw = (int)SDL_clamp(SDL_floorf(dst.w / factor), 4.0f, dst.w);
    const int   sh = (int)SDL_clamp(SDL_floorf(dst.h / factor), 4.0f, dst.h);

    SDL_Texture* previous = SDL_GetRenderTarget(r);

    SDL_FRect    used;
    SDL_Texture* small =
        downsample(r, src, (int)dst.w, (int)dst.h, sw, sh, used);
    SDL_SetRenderTarget(r, previous);
    if (!small) return false;

    // Pass 1 establishes the halo's alpha as well as its base colour: "over" is
    // the only kind of pass here that writes alpha at all.
    const float base = SDL_min(p.strength, 1.0f);
    SDL_SetTextureBlendMode(small, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    setPremultipliedMod(small, p.color, alpha * base);
    SDL_RenderTexture(r, small, &used, &dst);

    // Hardness: more "over" passes of the same layer. Each compounds coverage,
    // so alpha follows 1 - (1-a)^n -- zero stays zero while anything partial
    // races to opaque. That is what turns one blur from a broad soft halo into
    // a hard-edged dilated outline, and it is a different axis from brightness,
    // which is why it is a different property.
    float hard = SDL_clamp(p.hardness, 0.0f, 1.0f) * (float)kHardnessPasses;
    for (int pass = 0; pass < kHardnessPasses && hard > 0.0f; ++pass) {
        const float amount = SDL_min(hard, 1.0f);
        SDL_SetTextureBlendMode(small, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        setPremultipliedMod(small, p.color, alpha * base * amount);
        SDL_RenderTexture(r, small, &used, &dst);
        hard -= 1.0f;
    }

    // Brightness beyond 1. These are additive, so they sum colour into a region
    // alpha already covers and leave that alpha untouched -- which is what lets
    // a halo get brighter without also getting more opaque.
    float remaining = p.strength - 1.0f;
    for (int pass = 0; pass < 4 && remaining > 0.0f; ++pass) {
        const float amount = SDL_min(remaining, 1.0f);
        SDL_SetTextureBlendMode(small, SDL_BLENDMODE_ADD_PREMULTIPLIED);
        setPremultipliedMod(small, p.color, alpha * amount);
        SDL_RenderTexture(r, small, &used, &dst);
        remaining -= 1.0f;
    }

    return true;
}

void aberration(SDL_Renderer* r, SDL_Texture* src, const SDL_FRect& dst,
                float offset, float angle, float alpha)
{
    if (!src || dst.w <= 0.0f || dst.h <= 0.0f) return;
    if (offset == 0.0f) { plain(r, src, dst, alpha); return; }

    const float rad = angle * (SDL_PI_F / 180.0f);
    const float dx  = SDL_cosf(rad) * offset;
    const float dy  = SDL_sinf(rad) * offset;

    const SDL_FPoint shift[3] = {{-dx, -dy}, {0.0f, 0.0f}, {dx, dy}};
    const SDL_Color  channel[3] = {
        {255, 0,   0,   255},
        {0,   255, 0,   255},
        {0,   0,   255, 255},
    };

    // Coverage first. Drawing the three copies as black "over" passes puts the
    // union of where they land into alpha while leaving RGB at zero, so the
    // additive colour passes below have somewhere visible to accumulate into.
    // Without this the fringes would carry colour and no alpha, and a
    // transparent window would show none of it.
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    for (const SDL_FPoint& s : shift) {
        setPremultipliedMod(src, SDL_Color{0, 0, 0, 255}, alpha);
        const SDL_FRect at{dst.x + s.x, dst.y + s.y, dst.w, dst.h};
        SDL_RenderTexture(r, src, nullptr, &at);
    }

    // Then one additive pass per channel. Where the three line up they sum back
    // to the original colour; where they do not, the edges fringe.
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_ADD_PREMULTIPLIED);
    for (int i = 0; i < 3; ++i) {
        setPremultipliedMod(src, channel[i], alpha);
        const SDL_FRect at{dst.x + shift[i].x, dst.y + shift[i].y, dst.w, dst.h};
        SDL_RenderTexture(r, src, nullptr, &at);
    }
}

void shutdown()
{
    for (Scratch& s : g_blur) s.release();
}

} // namespace Effects
