#pragma once
#include <SDL3/SDL.h>

// Multi-pass composites for the glow and chromatic-aberration effects.
//
// Both take a texture holding an already-rendered subtree and draw it into the
// current render target several times. That texture is premultiplied, because
// that is what a render target composited with SDL_BLENDMODE_BLEND leaves
// behind -- the same trap Drawable::blit and Caption::rebuild already document.
//
// No SDL_ComposeCustomBlendMode anywhere in here, deliberately.
//
// The obvious way to write an additive pass wants colour *and* alpha to
// accumulate, and "add alpha too" is only expressible as a composed blend mode.
// The stock additive modes leave alpha alone:
//
//   SDL_BLENDMODE_ADD_PREMULTIPLIED   dstRGB = srcRGB + dstRGB, dstA = dstA
//
// On an opaque window that does not matter. This window is transparent and
// cleared to alpha 0 (PetWindow::render), so a pass that contributes no alpha
// contributes nothing the compositor will show -- an invisible halo.
//
// Composed modes would fix it, and are available nearly everywhere. Measured
// against SDL 3.4.16 rather than assumed:
//
//   x11, this desktop                   -> opengl,   composed modes ok
//   x11, LIBGL_ALWAYS_SOFTWARE=1        -> opengl,   composed modes ok
//   SDL_RENDER_DRIVER=software          -> software, "That operation is not
//                                                     supported"
//
// A machine with no GPU is not the problem: Mesa's llvmpipe still gives SDL the
// `opengl` render driver. SDL's own `software` renderer is the last resort,
// reachable on a box with no working GL at all, and it is the one backend with
// no composed blend modes. Excluding it buys nothing, because the stock modes
// can do this job if coverage and colour are established separately:
//
//   SDL_BLENDMODE_BLEND_PREMULTIPLIED   writes alpha  ("over")
//   SDL_BLENDMODE_ADD_PREMULTIPLIED     sums colour, leaves alpha alone
//
// Lay the first down to get the union of the passes into alpha, then the second
// on top to sum their colour into RGB. Every renderer SDL has implements both.
//
// Each channel contributes at most its own alpha to a region alpha already
// covers, so the result satisfies RGB <= A and stays a valid premultiplied
// texture.

namespace Effects {

struct GlowParams {
    float     radius   = 0.0f;   // how far the halo reaches, in px
    SDL_Color color     {255, 255, 255, 255};
    float     strength = 1.0f;   // brightness; >1 adds additive passes
    float     hardness = 0.0f;   // 0 soft falloff .. 1 near-solid outline
    float     alpha    = 1.0f;   // inherited opacity
};

// Draw `src` blurred and tinted to fill `dst`, as a halo.
//
// The blur is a downsample/upsample, with the bilinear filter as the kernel.
// The downsample is done by *repeated halving* rather than in one step, which
// matters more than it sounds: SDL_SCALEMODE_LINEAR reads four texels no matter
// how far it is minifying and there are no mipmaps, so a single 19:1 step -- an
// ordinary radius on an ordinary sprite -- samples four texels out of the ~360
// it should be averaging. That is not a blur, it is aliasing, and it shows up as
// a lumpy halo that crawls when the subtree moves. Each halving is a clean 2x2
// average, and the chain costs about a third more pixels than the single step it
// replaces.
//
// `color` multiplies rather than replaces, exactly as Sprite::color does -- so a
// dark subtree glows dark. Drawable::glow_flat is the way to get a halo in a
// colour the art does not already contain; it blurs a white silhouette of the
// subtree instead of the subtree, and this function neither knows nor cares
// which it was handed.
//
// Returns false if a scratch target could not be allocated.
bool glow(SDL_Renderer* r, SDL_Texture* src, const SDL_FRect& dst,
          const GlowParams& p);

// Draw `src` three times, one per colour channel, separated by `offset` pixels
// along `angle` degrees. Where the three line up they sum back to the original
// colour; where they do not, the edges fringe.
void aberration(SDL_Renderer* r, SDL_Texture* src, const SDL_FRect& dst,
                float offset, float angle, float alpha);

// Draw `src` once, unmodified. The no-effect path through the same compositor,
// used when a subtree goes through a render target for one effect but not the
// other.
void plain(SDL_Renderer* r, SDL_Texture* src, const SDL_FRect& dst, float alpha);

// Release the shared scratch target. Called when the renderer goes away, since
// a texture outliving its renderer is a dangling pointer.
void shutdown();

} // namespace Effects
