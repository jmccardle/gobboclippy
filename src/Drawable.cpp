#include "Drawable.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct AlignName { const char* name; Align value; };

const AlignName kAlignNames[] = {
    {"NONE",          Align::NONE},
    {"TOP_LEFT",      Align::TOP_LEFT},
    {"TOP_CENTER",    Align::TOP_CENTER},
    {"TOP_RIGHT",     Align::TOP_RIGHT},
    {"CENTER_LEFT",   Align::CENTER_LEFT},
    {"CENTER",        Align::CENTER},
    {"CENTER_RIGHT",  Align::CENTER_RIGHT},
    {"BOTTOM_LEFT",   Align::BOTTOM_LEFT},
    {"BOTTOM_CENTER", Align::BOTTOM_CENTER},
    {"BOTTOM_RIGHT",  Align::BOTTOM_RIGHT},
};

// Render children in z_index order without disturbing the child list itself:
// insertion order is the tiebreak, so equal z_index draws in the order added.
std::vector<Drawable*> zOrdered(const std::vector<std::shared_ptr<Drawable>>& v)
{
    std::vector<Drawable*> out;
    out.reserve(v.size());
    for (const auto& d : v) if (d) out.push_back(d.get());
    std::stable_sort(out.begin(), out.end(),
                     [](const Drawable* a, const Drawable* b) {
                         return a->z_index < b->z_index;
                     });
    return out;
}

} // namespace

const char* alignName(Align a)
{
    for (const AlignName& e : kAlignNames) if (e.value == a) return e.name;
    return "NONE";
}

bool alignFromName(const std::string& name, Align& out)
{
    for (const AlignName& e : kAlignNames) {
        if (name == e.name) { out = e.value; return true; }
    }
    return false;
}

Drawable::~Drawable() = default;

void Drawable::attach(const std::shared_ptr<Drawable>& child,
                      const std::shared_ptr<Drawable>& new_parent)
{
    if (!child) return;
    detach(child);
    if (!new_parent) return;
    new_parent->children.push_back(child);
    child->parent = new_parent;
}

void Drawable::detach(const std::shared_ptr<Drawable>& child)
{
    if (!child) return;
    if (auto p = child->parent.lock()) {
        auto& v = p->children;
        v.erase(std::remove(v.begin(), v.end(), child), v.end());
    }
    child->parent.reset();
}

SDL_FPoint Drawable::globalPosition() const
{
    // Translation only: scale and rotation stop at the drawable that owns
    // them. See the class comment for why.
    SDL_FPoint p = position;
    auto up = parent.lock();
    while (up) {
        p.x += up->position.x;
        p.y += up->position.y;
        up = up->parent.lock();
    }
    return p;
}

SDL_FRect Drawable::bounds() const
{
    const SDL_FPoint c  = contentSize();
    const float      sx = SDL_fabsf(scale.x);
    const float      sy = SDL_fabsf(scale.y);
    return SDL_FRect{
        position.x - origin.x * sx,
        position.y - origin.y * sy,
        c.x * sx,
        c.y * sy};
}

SDL_FRect Drawable::globalBounds() const
{
    SDL_FRect b = bounds();
    const SDL_FPoint g = globalPosition();
    b.x += g.x - position.x;
    b.y += g.y - position.y;
    return b;
}

void Drawable::realign()
{
    if (align == Align::NONE) return;

    // The box we are aligning inside: the parent's bounds, or the window.
    //
    // (ox, oy) is that box's top-left corner expressed in this drawable's own
    // coordinate space. For a child that is not the parent's position -- the
    // parent's position is its pivot, which its `origin` can put anywhere --
    // so aligning to a parent's TOP_LEFT means the corner of its box, not
    // wherever its pivot happens to sit.
    float pw, ph, ox = 0.0f, oy = 0.0f;
    if (auto p = parent.lock()) {
        const SDL_FRect pb = p->bounds();
        pw = pb.w;
        ph = pb.h;
        ox = pb.x - p->position.x;
        oy = pb.y - p->position.y;
    } else {
        pw = Stage::instance().size.x;
        ph = Stage::instance().size.y;
    }

    const SDL_FRect self = bounds();
    const float cw = self.w, ch = self.h;

    // A per-axis margin overrides the general one when set; -1 means inherit.
    const float mx = (horiz_margin >= 0.0f) ? horiz_margin : margin;
    const float my = (vert_margin  >= 0.0f) ? vert_margin  : margin;

    float x = 0.0f, y = 0.0f;
    switch (align) {
    case Align::TOP_LEFT:      x = mx;                 y = my;                 break;
    case Align::TOP_CENTER:    x = (pw - cw) * 0.5f;   y = my;                 break;
    case Align::TOP_RIGHT:     x = pw - cw - mx;       y = my;                 break;
    case Align::CENTER_LEFT:   x = mx;                 y = (ph - ch) * 0.5f;   break;
    case Align::CENTER:        x = (pw - cw) * 0.5f;   y = (ph - ch) * 0.5f;   break;
    case Align::CENTER_RIGHT:  x = pw - cw - mx;       y = (ph - ch) * 0.5f;   break;
    case Align::BOTTOM_LEFT:   x = mx;                 y = ph - ch - my;       break;
    case Align::BOTTOM_CENTER: x = (pw - cw) * 0.5f;   y = ph - ch - my;       break;
    case Align::BOTTOM_RIGHT:  x = pw - cw - mx;       y = ph - ch - my;       break;
    case Align::NONE:          return;
    }

    // x,y is where the bounding box goes; `position` is the pivot inside it.
    // McRogueFace special-cases the drawables whose position is not the
    // top-left corner; carrying the offset from bounds() covers every case,
    // including an origin the caller set to something arbitrary.
    position.x = ox + x + (position.x - self.x);
    position.y = oy + y + (position.y - self.y);
}

void Drawable::blit(SDL_Renderer* r, SDL_Texture* tex, const SDL_FRect* src,
                    SDL_FPoint offset, float alpha, SDL_Color tint,
                    bool premultiplied) const
{
    if (!tex) return;

    const SDL_FPoint c  = contentSize();
    const float      sx = SDL_fabsf(scale.x);
    const float      sy = SDL_fabsf(scale.y);
    if (c.x <= 0.0f || c.y <= 0.0f || sx <= 0.0f || sy <= 0.0f) return;

    const SDL_FPoint g = globalPosition();

    // The pivot, expressed where SDL wants it: relative to the destination
    // rectangle's top-left corner.
    const SDL_FPoint pivot{origin.x * sx, origin.y * sy};

    SDL_FRect dst{g.x + offset.x - pivot.x,
                  g.y + offset.y - pivot.y,
                  c.x * sx,
                  c.y * sy};

    // A negative scale axis mirrors rather than producing a negative-width
    // rectangle, which SDL would draw with the pivot in the wrong place.
    SDL_FlipMode flip = SDL_FLIP_NONE;
    if (scale.x < 0.0f) flip = (SDL_FlipMode)(flip | SDL_FLIP_HORIZONTAL);
    if (scale.y < 0.0f) flip = (SDL_FlipMode)(flip | SDL_FLIP_VERTICAL);

    const Uint8 a = (Uint8)SDL_lroundf(SDL_clamp(alpha, 0.0f, 1.0f) * 255.0f);
    const Uint8 effective_alpha = (Uint8)((a * tint.a) / 255);

    if (premultiplied) {
        // SDL modulates colour and alpha independently, but premultiplied
        // colour has to be scaled by the same factor as its alpha or fading
        // makes the texture brighter as it disappears. So the fade is folded
        // into the colour modulation as well.
        SDL_SetTextureColorMod(tex,
                               (Uint8)((tint.r * effective_alpha) / 255),
                               (Uint8)((tint.g * effective_alpha) / 255),
                               (Uint8)((tint.b * effective_alpha) / 255));
    } else {
        SDL_SetTextureColorMod(tex, tint.r, tint.g, tint.b);
    }
    SDL_SetTextureAlphaMod(tex, effective_alpha);

    SDL_RenderTextureRotated(r, tex, src, &dst, (double)rotation, &pivot, flip);
}

void Drawable::render(SDL_Renderer* r, SDL_FPoint offset, float inherited_opacity)
{
    if (!visible) return;

    const float alpha = inherited_opacity * SDL_clamp(opacity, 0.0f, 1.0f);
    if (alpha > 0.0f) draw(r, offset, alpha);

    // Children are positioned relative to this drawable's position, and
    // globalPosition() already walks the chain to find it -- so the offset
    // passes through unchanged. Nothing about this drawable's scale or
    // rotation reaches its children; see the header for why.
    for (Drawable* child : zOrdered(children)) {
        child->render(r, offset, alpha);
    }
}

// --- property system -------------------------------------------------------

bool Drawable::setProperty(const std::string& name, float value)
{
    if      (name == "x")        { position.x = value; return true; }
    else if (name == "y")        { position.y = value; return true; }
    else if (name == "origin_x") { origin.x   = value; return true; }
    else if (name == "origin_y") { origin.y   = value; return true; }
    else if (name == "scale")    { scale.x = scale.y = value; return true; }
    else if (name == "scale_x")  { scale.x    = value; return true; }
    else if (name == "scale_y")  { scale.y    = value; return true; }
    else if (name == "rotation") { rotation   = value; return true; }
    else if (name == "opacity")  { opacity = SDL_clamp(value, 0.0f, 1.0f); return true; }
    else if (name == "z_index")  { z_index = (int)SDL_lroundf(value); return true; }
    return false;
}

bool Drawable::setProperty(const std::string& name, int value)
{
    if (name == "z_index") { z_index = value; return true; }
    // Fall through to float, so animating "x" to an int literal works.
    return setProperty(name, (float)value);
}

bool Drawable::setProperty(const std::string&, const SDL_Color&) { return false; }

bool Drawable::setProperty(const std::string& name, const SDL_FPoint& value)
{
    if      (name == "pos" || name == "position") { position = value; return true; }
    else if (name == "origin")                    { origin   = value; return true; }
    else if (name == "scale")                     { scale    = value; return true; }
    return false;
}

bool Drawable::setProperty(const std::string&, const std::string&) { return false; }

bool Drawable::getProperty(const std::string& name, float& value) const
{
    if      (name == "x")        { value = position.x; return true; }
    else if (name == "y")        { value = position.y; return true; }
    else if (name == "origin_x") { value = origin.x;   return true; }
    else if (name == "origin_y") { value = origin.y;   return true; }
    else if (name == "scale")    { value = scale.x;    return true; }
    else if (name == "scale_x")  { value = scale.x;    return true; }
    else if (name == "scale_y")  { value = scale.y;    return true; }
    else if (name == "rotation") { value = rotation;   return true; }
    else if (name == "opacity")  { value = opacity;    return true; }
    else if (name == "z_index")  { value = (float)z_index; return true; }
    return false;
}

bool Drawable::getProperty(const std::string& name, int& value) const
{
    if (name == "z_index") { value = z_index; return true; }
    return false;
}

bool Drawable::getProperty(const std::string&, SDL_Color&) const { return false; }

bool Drawable::getProperty(const std::string& name, SDL_FPoint& value) const
{
    if      (name == "pos" || name == "position") { value = position; return true; }
    else if (name == "origin")                    { value = origin;   return true; }
    else if (name == "scale")                     { value = scale;    return true; }
    return false;
}

bool Drawable::getProperty(const std::string&, std::string&) const { return false; }

bool Drawable::hasProperty(const std::string& name) const
{
    return name == "x" || name == "y" ||
           name == "origin_x" || name == "origin_y" || name == "origin" ||
           name == "scale" || name == "scale_x" || name == "scale_y" ||
           name == "rotation" || name == "opacity" || name == "z_index" ||
           name == "pos" || name == "position";
}

// --- Stage -----------------------------------------------------------------

Stage& Stage::instance()
{
    static Stage s;
    return s;
}

void Stage::add(const std::shared_ptr<Drawable>& d)
{
    if (!d) return;
    Drawable::detach(d);          // a drawable is in exactly one place
    remove(d);
    roots.push_back(d);
}

void Stage::remove(const std::shared_ptr<Drawable>& d)
{
    roots.erase(std::remove(roots.begin(), roots.end(), d), roots.end());
}

void Stage::clear() { roots.clear(); }

void Stage::render(SDL_Renderer* r)
{
    if (!r) return;
    for (Drawable* d : zOrdered(roots)) {
        d->render(r, SDL_FPoint{0.0f, 0.0f}, 1.0f);
    }
}
