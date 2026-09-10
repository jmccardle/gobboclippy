#include "Drawable.h"

#include "Effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// The axis-aligned box of `b` after rotating it `degrees` about `pivot`.
//
// bounds() ignores rotation, which is right for alignment but would clip a
// rotated child out of an effect target -- the one place the true extent has
// to be known before anything is drawn.
SDL_FRect rotatedAABB(const SDL_FRect& b, SDL_FPoint pivot, float degrees)
{
    if (degrees == 0.0f) return b;

    const float rad = degrees * (SDL_PI_F / 180.0f);
    const float c   = SDL_cosf(rad);
    const float s   = SDL_sinf(rad);

    const float xs[2] = {b.x, b.x + b.w};
    const float ys[2] = {b.y, b.y + b.h};

    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            const float dx = xs[i] - pivot.x;
            const float dy = ys[j] - pivot.y;
            const float rx = pivot.x + dx * c - dy * s;
            const float ry = pivot.y + dx * s + dy * c;
            if (i == 0 && j == 0) { x0 = x1 = rx; y0 = y1 = ry; continue; }
            x0 = SDL_min(x0, rx); x1 = SDL_max(x1, rx);
            y0 = SDL_min(y0, ry); y1 = SDL_max(y1, ry);
        }
    }
    return SDL_FRect{x0, y0, x1 - x0, y1 - y0};
}

SDL_FRect unionRect(const SDL_FRect& a, const SDL_FRect& b)
{
    const float x0 = SDL_min(a.x, b.x);
    const float y0 = SDL_min(a.y, b.y);
    const float x1 = SDL_max(a.x + a.w, b.x + b.w);
    const float y1 = SDL_max(a.y + a.h, b.y + b.h);
    return SDL_FRect{x0, y0, x1 - x0, y1 - y0};
}

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

bool Drawable::s_flat_pass = false;

Drawable::~Drawable()
{
    if (m_fx)      SDL_DestroyTexture(m_fx);
    if (m_fx_flat) SDL_DestroyTexture(m_fx_flat);
}

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

    // The silhouette pass wants shape, not colour: flat white keeps the
    // texture's own alpha and discards everything else.
    if (s_flat_pass) tint = SDL_Color{255, 255, 255, 255};

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

SDL_FRect Drawable::subtreeGlobalBounds() const
{
    SDL_FRect box = rotatedAABB(globalBounds(), globalPosition(), rotation);
    bool      any = (box.w > 0.0f && box.h > 0.0f);

    for (const auto& c : children) {
        if (!c || !c->visible) continue;
        const SDL_FRect cb = c->subtreeGlobalBounds();
        if (cb.w <= 0.0f || cb.h <= 0.0f) continue;
        box = any ? unionRect(box, cb) : cb;
        any = true;
    }

    return any ? box : SDL_FRect{0.0f, 0.0f, 0.0f, 0.0f};
}

float Drawable::effectRadius() const
{
    float pad = 0.0f;
    if (glow > 0.0f)         pad = SDL_max(pad, glow);
    if (aberration != 0.0f)  pad = SDL_max(pad, SDL_fabsf(aberration));
    return pad;
}

void Drawable::render(SDL_Renderer* r, SDL_FPoint offset, float inherited_opacity)
{
    if (!visible) return;

    const float alpha = inherited_opacity * SDL_clamp(opacity, 0.0f, 1.0f);

    // m_compositing is what stops this from recursing: renderWithEffects calls
    // back into the ordinary walk for the same subtree.
    if (!m_compositing && effectRadius() > 0.0f) {
        renderWithEffects(r, offset, alpha);
        return;
    }
    renderPlain(r, offset, alpha);
}

void Drawable::renderPlain(SDL_Renderer* r, SDL_FPoint offset, float alpha)
{
    if (alpha > 0.0f) draw(r, offset, alpha);

    // Children are positioned relative to this drawable's position, and
    // globalPosition() already walks the chain to find it -- so the offset
    // passes through unchanged. Nothing about this drawable's scale or
    // rotation reaches its children; see the header for why.
    for (Drawable* child : zOrdered(children)) {
        child->render(r, offset, alpha);
    }
}

void Drawable::renderWithEffects(SDL_Renderer* r, SDL_FPoint offset, float alpha)
{
    SDL_FRect box = subtreeGlobalBounds();
    if (box.w <= 0.0f || box.h <= 0.0f) return;

    // Room for the effect to spill past the subtree, plus a pixel so the
    // bilinear tap at the target's edge has something to read instead of
    // smearing the border outwards.
    const float pad = effectRadius() + 1.0f;
    box.x -= pad;
    box.y -= pad;
    box.w += pad * 2.0f;
    box.h += pad * 2.0f;

    // Round out, so the composite lands on whole pixels rather than resampling
    // the whole subtree by a fraction of one.
    const float x0 = SDL_floorf(box.x + offset.x);
    const float y0 = SDL_floorf(box.y + offset.y);

    // Rounded up to a multiple of 16 rather than fitted exactly.
    //
    // `glow` is an animatable property, so it is expected to change every
    // frame -- and the target is sized from it. Fitting exactly would
    // reallocate a texture per frame for the whole of any glow animation, which
    // is the sort of cost that only shows up once someone actually animates the
    // thing. Quantising means a reallocation only when the size crosses a
    // boundary. The extra is transparent padding and composites as nothing.
    const int quantum = 16;
    const int tw = ((int)SDL_ceilf(box.w) + quantum) / quantum * quantum;
    const int th = ((int)SDL_ceilf(box.h) + quantum) / quantum * quantum;
    if (tw <= 0 || th <= 0) return;

    // Both targets are the same size, so a size change drops both and
    // ensureTarget remakes whichever is actually needed this frame.
    if (m_fx_w != tw || m_fx_h != th) {
        if (m_fx)      { SDL_DestroyTexture(m_fx);      m_fx      = nullptr; }
        if (m_fx_flat) { SDL_DestroyTexture(m_fx_flat); m_fx_flat = nullptr; }
        m_fx_w = tw;
        m_fx_h = th;
    }
    if (!ensureTarget(r, m_fx, tw, th, "effect target")) return;

    SDL_Texture* previous = SDL_GetRenderTarget(r);

    // The subtree is drawn at its accumulated alpha and composited at 1.0, so
    // fading behaves as it always has. Compositing at alpha instead would give
    // group opacity -- overlapping children would stop showing through each
    // other mid-fade -- which is a different look, not this one.
    //
    // `flat` renders the same subtree as a white silhouette. It is a second
    // walk, which is why glow_flat is opt-in.
    auto drawSubtreeInto = [&](SDL_Texture* target, bool flat) -> bool {
        if (!SDL_SetRenderTarget(r, target)) {
            SDL_Log("drawable '%s': SDL_SetRenderTarget: %s",
                    name.empty() ? "(unnamed)" : name.c_str(), SDL_GetError());
            return false;
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 0);
        SDL_RenderClear(r);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

        // Re-enter the ordinary walk with the subtree translated into the
        // target's space. render() passes `offset` straight down and blit()
        // adds it to globalPosition(), so this one subtraction moves the whole
        // subtree -- no second coordinate system, and children keep working out
        // their own places exactly as they do on the window.
        s_flat_pass   = flat;
        m_compositing = true;
        renderPlain(r, SDL_FPoint{offset.x - x0, offset.y - y0}, alpha);
        m_compositing = false;
        s_flat_pass   = false;
        return true;
    };

    if (!drawSubtreeInto(m_fx, false)) { SDL_SetRenderTarget(r, previous); return; }

    // The halo's source: the subtree itself, or a white silhouette of it when
    // glow_color is meant to be the halo's colour rather than a tint on the art.
    SDL_Texture* halo_src = m_fx;
    if (glow > 0.0f && glow_flat) {
        if (ensureTarget(r, m_fx_flat, tw, th, "silhouette target") &&
            drawSubtreeInto(m_fx_flat, true)) {
            halo_src = m_fx_flat;
        }
    }

    SDL_SetRenderTarget(r, previous);

    const SDL_FRect dst{x0, y0, (float)tw, (float)th};

    Effects::GlowParams gp;
    gp.radius   = glow;
    gp.color    = glow_color;
    gp.strength = glow_strength;
    gp.hardness = glow_hardness;
    gp.alpha    = 1.0f;

    auto drawHalo = [&]() {
        if (glow > 0.0f) Effects::glow(r, halo_src, dst, gp);
    };
    auto drawSubject = [&]() {
        if (aberration != 0.0f) {
            Effects::aberration(r, m_fx, dst, aberration, aberration_angle, 1.0f);
        } else {
            Effects::plain(r, m_fx, dst, 1.0f);
        }
    };

    if (glow_over) { drawSubject(); drawHalo(); }
    else           { drawHalo();    drawSubject(); }
}

bool Drawable::ensureTarget(SDL_Renderer* r, SDL_Texture*& tex, int w, int h,
                            const char* what)
{
    if (tex) return true;

    tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                            SDL_TEXTUREACCESS_TARGET, w, h);
    if (!tex) {
        // Drawing the subtree without its effects would hide this; drawing
        // nothing at all makes it obvious something is wrong, and the log says
        // what. Same choice Caption::fail makes.
        SDL_Log("drawable '%s': %s %dx%d: %s",
                name.empty() ? "(unnamed)" : name.c_str(), what, w, h,
                SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
    return true;
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
    else if (name == "glow")             { glow = SDL_max(0.0f, value); return true; }
    else if (name == "glow_strength")    { glow_strength = SDL_max(0.0f, value); return true; }
    else if (name == "glow_hardness")    { glow_hardness = SDL_clamp(value, 0.0f, 1.0f); return true; }
    else if (name == "aberration")       { aberration = value; return true; }
    else if (name == "aberration_angle") { aberration_angle = value; return true; }
    return false;
}

bool Drawable::setProperty(const std::string& name, int value)
{
    if (name == "z_index") { z_index = value; return true; }
    // Fall through to float, so animating "x" to an int literal works.
    return setProperty(name, (float)value);
}

bool Drawable::setProperty(const std::string& name, const SDL_Color& value)
{
    if (name == "glow_color") { glow_color = value; return true; }
    return false;
}

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
    else if (name == "glow")             { value = glow;             return true; }
    else if (name == "glow_strength")    { value = glow_strength;    return true; }
    else if (name == "glow_hardness")    { value = glow_hardness;    return true; }
    else if (name == "aberration")       { value = aberration;       return true; }
    else if (name == "aberration_angle") { value = aberration_angle; return true; }
    return false;
}

bool Drawable::getProperty(const std::string& name, int& value) const
{
    if (name == "z_index") { value = z_index; return true; }
    return false;
}

bool Drawable::getProperty(const std::string& name, SDL_Color& value) const
{
    if (name == "glow_color") { value = glow_color; return true; }
    return false;
}

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
           name == "pos" || name == "position" ||
           name == "glow" || name == "glow_color" || name == "glow_strength" ||
           name == "glow_hardness" ||
           name == "aberration" || name == "aberration_angle";
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
