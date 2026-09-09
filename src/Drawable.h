#pragma once
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

// The drawable base, harvested from McRogueFace's UIDrawable.
//
// What came across: position, origin, rotation, per-axis scale, opacity,
// visibility, z-order, the parent/child tree, nine-point alignment, and the
// string-keyed property system that Animation drives.
//
// What was cut, per docs/harvest.md: grids, scenes, shaders, render-texture
// compositing, click/hover dispatch, and the Python object cache. A desktop
// pet has one surface and no input model, so a flat tree over the window is
// the whole scene graph.
//
// Deliberate difference from McRogueFace, and the reason this is useful here:
//
//   * children inherit their parent's *translation only*, never its scale or
//     rotation. That is what makes "stretch the paperclip without distorting
//     the eyes" a one-liner instead of a counter-transform.
//   * children DO inherit opacity, multiplicatively, so fading the character
//     out is one call on the root rather than one per part.

enum class Align {
    NONE = -1,
    TOP_LEFT = 0, TOP_CENTER, TOP_RIGHT,
    CENTER_LEFT,  CENTER,     CENTER_RIGHT,
    BOTTOM_LEFT,  BOTTOM_CENTER, BOTTOM_RIGHT,
};

const char* alignName(Align a);
bool        alignFromName(const std::string& name, Align& out);

class Drawable : public std::enable_shared_from_this<Drawable> {
public:
    virtual ~Drawable();

    enum class Kind { Sprite, Caption };
    virtual Kind kind() const = 0;

    // --- transform ---------------------------------------------------------
    // `position` is the pivot: the point `origin` of the content is placed
    // there, and rotation and scale happen about it.
    SDL_FPoint position {0.0f, 0.0f};
    SDL_FPoint origin   {0.0f, 0.0f};   // in unscaled content pixels
    SDL_FPoint scale    {1.0f, 1.0f};   // signed; a negative axis mirrors
    float      rotation = 0.0f;         // degrees, clockwise
    float      opacity  = 1.0f;         // 0..1, multiplied down the tree
    bool       visible  = true;
    int        z_index  = 0;
    std::string name;

    // --- hierarchy ---------------------------------------------------------
    std::weak_ptr<Drawable>                parent;
    std::vector<std::shared_ptr<Drawable>> children;

    // Both sides of the link in one place, so a drawable can never be in a
    // parent's child list without pointing back at it.
    static void attach(const std::shared_ptr<Drawable>& child,
                       const std::shared_ptr<Drawable>& parent);
    static void detach(const std::shared_ptr<Drawable>& child);

    SDL_FPoint globalPosition() const;

    // --- alignment ---------------------------------------------------------
    Align align        = Align::NONE;
    float margin       = 0.0f;
    float horiz_margin = -1.0f;   // -1 means "inherit from margin"
    float vert_margin  = -1.0f;

    // Recompute `position` from the parent's bounds (or the stage's, at the
    // top level). Alignment is applied when asked, not continuously: a parent
    // that moves does not drag an aligned child with it until realign().
    void realign();

    // --- geometry ----------------------------------------------------------
    virtual SDL_FPoint contentSize() const = 0;   // unscaled, in pixels

    SDL_FRect bounds() const;         // axis-aligned, in the parent's space
    SDL_FRect globalBounds() const;   // axis-aligned, in stage space

    // --- rendering ---------------------------------------------------------
    // Draws this drawable and then its children, in z_index order. `offset` is
    // the accumulated parent translation; `inherited_opacity` the accumulated
    // parent opacity.
    void render(SDL_Renderer* r, SDL_FPoint offset, float inherited_opacity);

    // --- property system, for Animation ------------------------------------
    // Names match McRogueFace's, so an animation written against one animates
    // the same thing here.
    virtual bool setProperty(const std::string& name, float value);
    virtual bool setProperty(const std::string& name, int value);
    virtual bool setProperty(const std::string& name, const SDL_Color& value);
    virtual bool setProperty(const std::string& name, const SDL_FPoint& value);
    virtual bool setProperty(const std::string& name, const std::string& value);

    virtual bool getProperty(const std::string& name, float& value) const;
    virtual bool getProperty(const std::string& name, int& value) const;
    virtual bool getProperty(const std::string& name, SDL_Color& value) const;
    virtual bool getProperty(const std::string& name, SDL_FPoint& value) const;
    virtual bool getProperty(const std::string& name, std::string& value) const;

    // Whether `name` is animatable on this drawable. animate() checks this
    // first so a typo'd property is an error at the call, not a no-op that
    // looks like a broken animation.
    virtual bool hasProperty(const std::string& name) const;

protected:
    // Draw just this drawable. `offset` already includes the parent chain.
    virtual void draw(SDL_Renderer* r, SDL_FPoint offset, float alpha) = 0;

    // Shared by Sprite and Caption: place a texture using this drawable's
    // transform, including the negative-scale mirroring.
    //
    // `premultiplied` says which kind of texture this is. A PNG straight off
    // disk carries straight alpha; anything composited through a render target
    // comes back premultiplied, because that is what SDL_BLENDMODE_BLEND
    // leaves in the target. Blending a premultiplied texture as though it were
    // straight multiplies by alpha a second time, which is not a subtle
    // artefact -- it turns pale text muddy and dark text into a smear.
    void blit(SDL_Renderer* r, SDL_Texture* tex, const SDL_FRect* src,
              SDL_FPoint offset, float alpha, SDL_Color tint,
              bool premultiplied = false) const;
};

// The top-level drawable list, and the bounds that top-level alignment is
// measured against.
//
// This is what replaces McRogueFace's Scene. It is a singleton for the same
// reason AnimationManager is one: there is exactly one, and the alternative is
// threading a pointer through every constructor and property setter.
class Stage {
public:
    static Stage& instance();

    std::vector<std::shared_ptr<Drawable>> roots;

    // Window size in pixels. The host sets this; alignment and the "stay
    // inside the window" helpers read it.
    SDL_FPoint size {0.0f, 0.0f};

    // The renderer everything on this stage draws into. The host sets it once,
    // after the window exists. Textures and font atlases are created against
    // it, so it has to be reachable from the Python constructors too -- this
    // is the one place that knows it, rather than a second global.
    SDL_Renderer* renderer = nullptr;

    void add(const std::shared_ptr<Drawable>& d);
    void remove(const std::shared_ptr<Drawable>& d);
    void clear();

    // Draws every root in z_index order.
    void render(SDL_Renderer* r);

private:
    Stage() = default;
};
