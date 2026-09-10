// Pixel-level check of the effect compositor.
//
// scripts/smoke_test.py covers the scripting surface -- that the properties
// exist, round-trip, clamp and animate -- and it drives real frames, so it
// catches a composite that fails outright. It cannot see what those frames
// contain. This can.
//
// The claim worth testing is the one the whole design turns on. The obvious way
// to write an additive pass leaves alpha untouched:
//
//     SDL_BLENDMODE_ADD_PREMULTIPLIED   dstRGB = srcRGB + dstRGB, dstA = dstA
//
// On the pet's transparent window, cleared to alpha 0, that produces a halo the
// compositor will not show -- colour in a buffer with nothing to composite it
// against. Effects.cpp establishes coverage first for exactly this reason, and
// "no fringe pixel has colour but zero alpha" below is what proves it still
// does. A regression there is invisible in a screenshot of an opaque window and
// obvious on a real desktop, which is the worst way round.
//
// Built only when GC_BUILD_TESTS=ON. Needs a video driver; CI runs it under
// xvfb alongside the smoke test.
#include "Drawable.h"
#include "Sprite.h"
#include "Texture.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

int failures = 0;

void check(const char* label, bool ok, const char* detail = nullptr)
{
    std::printf("%s  %-46s %s\n", ok ? "PASS" : "FAIL", label,
                detail ? detail : "");
    if (!ok) ++failures;
}

void pixelAt(SDL_Surface* s, int x, int y, Uint8 out[4])
{
    SDL_ReadSurfacePixel(s, x, y, &out[0], &out[1], &out[2], &out[3]);
}

} // namespace

int main()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* w = SDL_CreateWindow("fx_pixels", 400, 400, SDL_WINDOW_HIDDEN);
    if (!w) { std::printf("SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    SDL_Renderer* r = SDL_CreateRenderer(w, nullptr);
    if (!r) { std::printf("SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }

    // Worth printing: the composite deliberately uses only blend modes every
    // backend implements, so which one ran is part of the result.
    std::printf("render driver: %s\n\n", SDL_GetRendererName(r));

    Stage::instance().renderer = r;
    Stage::instance().size     = SDL_FPoint{400.0f, 400.0f};

    std::string err;
    auto tex = Texture::load(r, "assets/eyes.png", 48, 48, err);
    if (!tex) { std::printf("texture: %s\n", err.c_str()); return 1; }

    auto body = std::make_shared<Sprite>(tex, 0);
    body->position = SDL_FPoint{200.0f, 200.0f};
    body->origin   = SDL_FPoint{24.0f, 24.0f};
    body->name     = "body";

    // The sprite covers [176, 224) on both axes.
    const int inside_x = 200, inside_y = 200;
    const int halo_x   = 168,  halo_y  = 200;   // 8px outside the left edge
    const int far_x    = 136,  far_y   = 200;   // beyond any of these radii

    auto renderFrame = [&]() {
        SDL_SetRenderTarget(r, nullptr);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
        SDL_RenderClear(r);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        Stage::instance().render(r);
    };

    Uint8 p[4];
    char  detail[128];

    // --- baseline ------------------------------------------------------------
    Stage::instance().add(body);
    renderFrame();
    if (SDL_Surface* s = SDL_RenderReadPixels(r, nullptr)) {
        pixelAt(s, inside_x, inside_y, p);
        check("sprite draws with no effect", p[3] > 0);
        pixelAt(s, halo_x, halo_y, p);
        check("nothing outside the sprite without a glow", p[3] == 0);
        SDL_DestroySurface(s);
    } else {
        check("read back the frame", false, SDL_GetError());
    }

    // --- glow ----------------------------------------------------------------
    body->glow          = 20.0f;
    body->glow_color    = SDL_Color{80, 160, 255, 255};
    body->glow_strength = 2.0f;
    renderFrame();
    if (SDL_Surface* s = SDL_RenderReadPixels(r, nullptr)) {
        pixelAt(s, halo_x, halo_y, p);
        std::snprintf(detail, sizeof detail, "rgba(%u,%u,%u,%u) 8px outside",
                      p[0], p[1], p[2], p[3]);
        // The test this file exists for.
        check("glow spills past the sprite WITH alpha", p[3] > 0, detail);
        check("glow takes its tint from glow_color", p[2] > p[0], detail);

        pixelAt(s, far_x, far_y, p);
        std::snprintf(detail, sizeof detail, "alpha=%u at 40px outside", p[3]);
        check("glow falls off within its radius", p[3] == 0, detail);

        pixelAt(s, inside_x, inside_y, p);
        check("the subtree still draws over its halo", p[3] > 0);
        SDL_DestroySurface(s);
    } else {
        check("read back the glow frame", false, SDL_GetError());
    }

    // --- flat silhouette colour ---------------------------------------------
    //
    // The point of glow_flat is that the halo's colour comes from glow_color
    // and not from the art, so the test darkens the art and checks that only
    // the multiplying halo notices.
    body->glow       = 20.0f;
    body->glow_color = SDL_Color{255, 40, 40, 255};
    body->glow_flat  = false;
    body->color      = SDL_Color{40, 40, 40, 255};   // a dark subtree
    renderFrame();
    Uint8 multiplied[4] = {0, 0, 0, 0};
    if (SDL_Surface* s = SDL_RenderReadPixels(r, nullptr)) {
        pixelAt(s, halo_x, halo_y, multiplied);
        SDL_DestroySurface(s);
    }

    body->glow_flat = true;
    renderFrame();
    if (SDL_Surface* s = SDL_RenderReadPixels(r, nullptr)) {
        pixelAt(s, halo_x, halo_y, p);
        std::snprintf(detail, sizeof detail,
                      "multiply rgba(%u,%u,%u,%u) vs flat rgba(%u,%u,%u,%u)",
                      multiplied[0], multiplied[1], multiplied[2], multiplied[3],
                      p[0], p[1], p[2], p[3]);
        check("flat halo survives a dark subtree", p[0] > multiplied[0], detail);
        check("flat halo takes glow_color's hue", p[0] > p[1] && p[0] > p[2],
              detail);
        SDL_DestroySurface(s);
    } else {
        check("read back the flat-glow frame", false, SDL_GetError());
    }

    // --- the silhouette is white, not the art -------------------------------
    //
    // The check above darkens the subtree with `color`, which the flat pass
    // discards -- so it passes even if the pass blurs the art itself, and for a
    // long time it did. The art's own RGB is the thing that has to be thrown
    // away, and no tint can do it: SDL_SetTextureColorMod only multiplies, so
    // it reaches black but never white.
    //
    // With glow_color white, a real silhouette gives a halo at full brightness
    // whatever the art is. Blurring the art instead gives a halo no brighter
    // than the art's own mean, and every asset in this repo is a mid grey --
    // which is exactly why the bug was invisible until it met blonde hair.
    body->color      = SDL_Color{255, 255, 255, 255};
    body->glow       = 20.0f;
    body->glow_color = SDL_Color{255, 255, 255, 255};
    body->glow_flat  = false;
    renderFrame();
    Uint8 from_art[4] = {0, 0, 0, 0};
    if (SDL_Surface* s = SDL_RenderReadPixels(r, nullptr)) {
        pixelAt(s, halo_x, halo_y, from_art);
        SDL_DestroySurface(s);
    }

    body->glow_flat = true;
    renderFrame();
    if (SDL_Surface* s = SDL_RenderReadPixels(r, nullptr)) {
        pixelAt(s, halo_x, halo_y, p);
        std::snprintf(detail, sizeof detail,
                      "art rgb(%u,%u,%u) vs silhouette rgb(%u,%u,%u)",
                      from_art[0], from_art[1], from_art[2], p[0], p[1], p[2]);
        // A white halo off grey art is brighter than the art can account for.
        check("flat halo is brighter than the art it wraps",
              p[0] > from_art[0] + 20, detail);
        // And achromatic, because glow_color was achromatic. The art is not
        // quite neutral, so blurring it would tilt the halo with it.
        const int spread = SDL_max(SDL_max(p[0], p[1]), p[2]) -
                           SDL_min(SDL_min(p[0], p[1]), p[2]);
        std::snprintf(detail, sizeof detail, "channel spread %d", spread);
        check("a white glow_color gives an achromatic halo", spread <= 4,
              detail);
        SDL_DestroySurface(s);
    } else {
        check("read back the silhouette frame", false, SDL_GetError());
    }

    // --- halo above the subtree ----------------------------------------------
    // Drawn over, the halo veils the character rather than ringing it, so the
    // centre pixel has to change. glow_color's alpha is what controls how much.
    body->glow_over = true;
    renderFrame();
    if (SDL_Surface* s = SDL_RenderReadPixels(r, nullptr)) {
        pixelAt(s, inside_x, inside_y, p);
        const int over_r = p[0];
        body->glow_over = false;
        renderFrame();
        if (SDL_Surface* u = SDL_RenderReadPixels(r, nullptr)) {
            pixelAt(u, inside_x, inside_y, p);
            std::snprintf(detail, sizeof detail, "over r=%d vs under r=%u",
                          over_r, p[0]);
            check("glow_over tints the subtree, glow_under does not",
                  over_r > p[0], detail);
            SDL_DestroySurface(u);
        }
        SDL_DestroySurface(s);
    } else {
        check("read back the glow_over frame", false, SDL_GetError());
    }

    // --- aberration ----------------------------------------------------------
    body->color            = SDL_Color{255, 255, 255, 255};
    body->glow_flat        = false;
    body->glow             = 0.0f;
    body->aberration       = 6.0f;
    body->aberration_angle = 0.0f;   // separate along x
    renderFrame();
    if (SDL_Surface* s = SDL_RenderReadPixels(r, nullptr)) {
        bool red = false, blue = false;
        int  coloured_but_invisible = 0;

        for (int x = 160; x < 244; ++x) {
            pixelAt(s, x, 200, p);
            if (p[3] > 0 && p[0] > p[2] + 20) red  = true;
            if (p[3] > 0 && p[2] > p[0] + 20) blue = true;
            // A pass that contributed colour without coverage. Invisible on a
            // transparent window; this is the regression the design prevents.
            if (p[3] == 0 && (p[0] || p[1] || p[2])) ++coloured_but_invisible;
        }
        check("aberration produces a red-leaning fringe", red);
        check("aberration produces a blue-leaning fringe", blue);

        std::snprintf(detail, sizeof detail, "%d such pixels",
                      coloured_but_invisible);
        check("no fringe pixel has colour but zero alpha",
              coloured_but_invisible == 0, detail);
        SDL_DestroySurface(s);
    } else {
        check("read back the aberration frame", false, SDL_GetError());
    }

    // The stage owns drawables holding textures made on this renderer.
    Stage::instance().clear();
    Stage::instance().renderer = nullptr;
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();

    std::printf("\n%s\n", failures ? "FAILURES" : "all pixel checks passed");
    return failures ? 1 : 0;
}
