#include "PyDraw.h"

#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "Animation.h"
#include "AppPaths.h"
#include "Caption.h"
#include "Drawable.h"
#include "Easing.h"
#include "Font.h"
#include "Sprite.h"
#include "Texture.h"

namespace {

// ---------------------------------------------------------------------------
// Object layouts
//
// Every wrapper owns a shared_ptr, which is not trivially constructible, so
// tp_new placement-news it and tp_dealloc destroys it explicitly. Forgetting
// either half is the classic way to leak or double-free a C++ member inside a
// Python object, so both live in the two templates below and nowhere else.
// ---------------------------------------------------------------------------

struct PyTextureObject   { PyObject_HEAD std::shared_ptr<Texture>   data; };
struct PyFontObject      { PyObject_HEAD std::shared_ptr<Font>      data; };
struct PyDrawableObject  { PyObject_HEAD std::shared_ptr<Drawable>  data; };
struct PyAnimationObject { PyObject_HEAD std::shared_ptr<Animation> data; };

// A view over a list of drawables: either the stage's roots (owner == nullptr)
// or one drawable's children. The list itself is never copied, so appending
// through this is appending to the real thing.
struct PyDrawListObject  { PyObject_HEAD std::shared_ptr<Drawable>  data; };

PyTypeObject TextureType   = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject FontType      = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject DrawableType  = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject SpriteType    = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject CaptionType   = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject AnimationType = {PyVarObject_HEAD_INIT(nullptr, 0)};
PyTypeObject DrawListType  = {PyVarObject_HEAD_INIT(nullptr, 0)};

template <typename T>
PyObject* genericNew(PyTypeObject* type, PyObject*, PyObject*)
{
    T* self = (T*)type->tp_alloc(type, 0);
    if (self) new (&self->data) decltype(T::data)();
    return (PyObject*)self;
}

template <typename T>
void genericDealloc(PyObject* self)
{
    using Held = decltype(T::data);
    ((T*)self)->data.~Held();
    Py_TYPE(self)->tp_free(self);
}

// ---------------------------------------------------------------------------
// Small conversions
// ---------------------------------------------------------------------------

// A relative name means assets/, subfolders included; an absolute path is
// taken as given. Same rule as clippy.set_sprite(), so a script does not have
// to remember which call resolves and which does not.
std::string resolveAsset(const char* path)
{
    return AppPaths::resolveAsset(path);
}

bool asFloat(PyObject* o, float& out)
{
    const double d = PyFloat_AsDouble(o);
    if (d == -1.0 && PyErr_Occurred()) return false;
    out = (float)d;
    return true;
}

bool asPoint(PyObject* o, SDL_FPoint& out, const char* what)
{
    if (!PySequence_Check(o) || PySequence_Size(o) != 2) {
        PyErr_Format(PyExc_TypeError, "%s must be a 2-sequence (x, y)", what);
        return false;
    }
    PyObject* px = PySequence_GetItem(o, 0);
    PyObject* py = PySequence_GetItem(o, 1);
    const bool ok = px && py && asFloat(px, out.x) && asFloat(py, out.y);
    Py_XDECREF(px);
    Py_XDECREF(py);
    if (!ok && !PyErr_Occurred()) {
        PyErr_Format(PyExc_TypeError, "%s must be a 2-sequence of numbers", what);
    }
    return ok;
}

bool asColor(PyObject* o, SDL_Color& out, const char* what)
{
    if (!PySequence_Check(o)) {
        PyErr_Format(PyExc_TypeError, "%s must be (r, g, b) or (r, g, b, a)", what);
        return false;
    }
    const Py_ssize_t n = PySequence_Size(o);
    if (n != 3 && n != 4) {
        PyErr_Format(PyExc_ValueError,
                     "%s must have 3 or 4 components, got %zd", what, n);
        return false;
    }
    int channel[4] = {0, 0, 0, 255};
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(o, i);
        if (!item) return false;
        const long v = PyLong_AsLong(item);
        Py_DECREF(item);
        if (v == -1 && PyErr_Occurred()) return false;
        if (v < 0 || v > 255) {
            PyErr_Format(PyExc_ValueError,
                         "%s component %zd is %ld; must be 0..255", what, i, v);
            return false;
        }
        channel[i] = (int)v;
    }
    out = SDL_Color{(Uint8)channel[0], (Uint8)channel[1],
                    (Uint8)channel[2], (Uint8)channel[3]};
    return true;
}

PyObject* pointToPy(const SDL_FPoint& p)
{
    return Py_BuildValue("(dd)", (double)p.x, (double)p.y);
}

PyObject* colorToPy(const SDL_Color& c)
{
    return Py_BuildValue("(iiii)", (int)c.r, (int)c.g, (int)c.b, (int)c.a);
}

PyObject* rectToPy(const SDL_FRect& r)
{
    return Py_BuildValue("(dddd)", (double)r.x, (double)r.y,
                                   (double)r.w, (double)r.h);
}

// Accepts an Easing enum member (an IntEnum, so also an int), a name in either
// spelling, or None for linear. An unknown value is an error, never a silent
// fall back to linear.
bool easingFromArg(PyObject* arg, EasingFunction& out)
{
    if (!arg || arg == Py_None) { out = Easing::linear; return true; }

    if (PyLong_Check(arg)) {
        const long v = PyLong_AsLong(arg);
        if (v == -1 && PyErr_Occurred()) return false;
        EasingFunction fn = Easing::byIndex((int)v);
        if (!fn) {
            PyErr_Format(PyExc_ValueError,
                         "invalid easing value %ld; expected 0..%d or a "
                         "clippy.Easing member", v, Easing::count - 1);
            return false;
        }
        out = fn;
        return true;
    }

    if (PyUnicode_Check(arg)) {
        const char* name = PyUnicode_AsUTF8(arg);
        if (!name) return false;
        EasingFunction fn = Easing::byName(name);
        if (!fn) {
            PyErr_Format(PyExc_ValueError,
                         "unknown easing function '%s'; use a clippy.Easing "
                         "member, e.g. clippy.Easing.EASE_OUT_BACK", name);
            return false;
        }
        out = fn;
        return true;
    }

    PyErr_SetString(PyExc_TypeError,
                    "easing must be a clippy.Easing member, a name, or None");
    return false;
}

bool alignFromArg(PyObject* arg, Align& out)
{
    if (!arg || arg == Py_None) { out = Align::NONE; return true; }

    if (PyLong_Check(arg)) {
        const long v = PyLong_AsLong(arg);
        if (v == -1 && PyErr_Occurred()) return false;
        if (v < -1 || v > 8) {
            PyErr_Format(PyExc_ValueError,
                         "invalid align value %ld; use a clippy.Align member", v);
            return false;
        }
        out = (Align)v;
        return true;
    }

    if (PyUnicode_Check(arg)) {
        const char* name = PyUnicode_AsUTF8(arg);
        if (!name) return false;
        if (!alignFromName(name, out)) {
            PyErr_Format(PyExc_ValueError,
                         "unknown alignment '%s'; use a clippy.Align member, "
                         "e.g. clippy.Align.BOTTOM_CENTER", name);
            return false;
        }
        return true;
    }

    PyErr_SetString(PyExc_TypeError,
                    "align must be a clippy.Align member, a name, or None");
    return false;
}

// ---------------------------------------------------------------------------
// Drawable access
// ---------------------------------------------------------------------------

Drawable* drawableOf(PyObject* self)
{
    PyDrawableObject* obj = (PyDrawableObject*)self;
    if (!obj->data) {
        PyErr_SetString(PyExc_RuntimeError, "drawable is not initialised");
        return nullptr;
    }
    return obj->data.get();
}

std::shared_ptr<Drawable> sharedDrawable(PyObject* o)
{
    if (!o || !PyObject_TypeCheck(o, &DrawableType)) return nullptr;
    return ((PyDrawableObject*)o)->data;
}

// Read one animatable property back as a Python value, trying the property
// system's overloads in the order that resolves the overlaps: z_index and
// sprite_index answer to both int and float, and int is the truthful one.
PyObject* propertyToPy(const Drawable* d, const std::string& name)
{
    int i;
    if (d->getProperty(name, i)) return PyLong_FromLong(i);

    float f;
    if (d->getProperty(name, f)) return PyFloat_FromDouble((double)f);

    SDL_FPoint p;
    if (d->getProperty(name, p)) return pointToPy(p);

    SDL_Color c;
    if (d->getProperty(name, c)) return colorToPy(c);

    std::string s;
    if (d->getProperty(name, s)) return PyUnicode_FromString(s.c_str());

    Py_RETURN_NONE;
}

// ---------------------------------------------------------------------------
// clippy.Texture
// ---------------------------------------------------------------------------

int texture_init(PyObject* self, PyObject* args, PyObject* kwds)
{
    static const char* kw[] = {"path", "sprite_width", "sprite_height",
                               "smooth", nullptr};
    const char* path   = nullptr;
    int         cell_w = 0, cell_h = 0;
    int         smooth = 1;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|iip", (char**)kw,
                                     &path, &cell_w, &cell_h, &smooth)) {
        return -1;
    }

    SDL_Renderer* r = Stage::instance().renderer;
    if (!r) {
        PyErr_SetString(PyExc_RuntimeError,
                        "clippy.Texture: no renderer yet (called too early?)");
        return -1;
    }

    std::string err;
    auto tex = Texture::load(r, resolveAsset(path), cell_w, cell_h, err);
    if (!tex) {
        PyErr_SetString(PyExc_OSError, err.c_str());
        return -1;
    }
    tex->setSmooth(smooth != 0);

    ((PyTextureObject*)self)->data = std::move(tex);
    return 0;
}

PyObject* texture_repr(PyObject* self)
{
    Texture* t = ((PyTextureObject*)self)->data.get();
    if (!t) return PyUnicode_FromString("<clippy.Texture uninitialised>");
    return PyUnicode_FromFormat("<clippy.Texture '%s' %dx%d cells, %d frames>",
                                t->source().c_str(), t->spriteWidth(),
                                t->spriteHeight(), t->spriteCount());
}

PyObject* texture_get_int(PyObject* self, void* closure)
{
    Texture* t = ((PyTextureObject*)self)->data.get();
    if (!t) { PyErr_SetString(PyExc_RuntimeError, "texture is not initialised"); return nullptr; }

    const char* which = (const char*)closure;
    if (!std::strcmp(which, "sprite_width"))  return PyLong_FromLong(t->spriteWidth());
    if (!std::strcmp(which, "sprite_height")) return PyLong_FromLong(t->spriteHeight());
    if (!std::strcmp(which, "sheet_width"))   return PyLong_FromLong(t->sheetWidth());
    if (!std::strcmp(which, "sheet_height"))  return PyLong_FromLong(t->sheetHeight());
    return PyLong_FromLong(t->spriteCount());
}

PyObject* texture_get_source(PyObject* self, void*)
{
    Texture* t = ((PyTextureObject*)self)->data.get();
    if (!t) { PyErr_SetString(PyExc_RuntimeError, "texture is not initialised"); return nullptr; }
    return PyUnicode_FromString(t->source().c_str());
}

PyGetSetDef texture_getset[] = {
    {"sprite_width",  texture_get_int, nullptr, "Cell width in pixels.",  (void*)"sprite_width"},
    {"sprite_height", texture_get_int, nullptr, "Cell height in pixels.", (void*)"sprite_height"},
    {"sheet_width",   texture_get_int, nullptr, "Cells across.",          (void*)"sheet_width"},
    {"sheet_height",  texture_get_int, nullptr, "Cells down.",            (void*)"sheet_height"},
    {"sprite_count",  texture_get_int, nullptr, "Total frames.",          (void*)"sprite_count"},
    {"source",        texture_get_source, nullptr, "Path this was loaded from.", nullptr},
    {nullptr}
};

// ---------------------------------------------------------------------------
// clippy.Font
// ---------------------------------------------------------------------------

int font_init(PyObject* self, PyObject* args, PyObject* kwds)
{
    static const char* kw[] = {"path", nullptr};
    const char* path = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", (char**)kw, &path)) return -1;

    std::string err;
    auto font = Font::load(resolveAsset(path), err);
    if (!font) {
        PyErr_SetString(PyExc_OSError, err.c_str());
        return -1;
    }
    ((PyFontObject*)self)->data = std::move(font);
    return 0;
}

PyObject* font_repr(PyObject* self)
{
    Font* f = ((PyFontObject*)self)->data.get();
    if (!f) return PyUnicode_FromString("<clippy.Font uninitialised>");
    return PyUnicode_FromFormat("<clippy.Font '%s'>", f->source().c_str());
}

PyObject* font_get_source(PyObject* self, void*)
{
    Font* f = ((PyFontObject*)self)->data.get();
    if (!f) { PyErr_SetString(PyExc_RuntimeError, "font is not initialised"); return nullptr; }
    return PyUnicode_FromString(f->source().c_str());
}

PyObject* font_measure(PyObject* self, PyObject* args, PyObject* kwds)
{
    static const char* kw[] = {"text", "size", nullptr};
    const char* text = nullptr;
    int         size = 16;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|i", (char**)kw, &text, &size)) {
        return nullptr;
    }

    Font* f = ((PyFontObject*)self)->data.get();
    if (!f) { PyErr_SetString(PyExc_RuntimeError, "font is not initialised"); return nullptr; }

    SDL_Renderer* r = Stage::instance().renderer;
    if (!r) {
        PyErr_SetString(PyExc_RuntimeError, "clippy.Font.measure: no renderer yet");
        return nullptr;
    }

    SDL_FPoint extent;
    std::string err;
    if (!f->measure(r, text, size, extent, err)) {
        PyErr_SetString(PyExc_RuntimeError, err.c_str());
        return nullptr;
    }
    return pointToPy(extent);
}

PyGetSetDef font_getset[] = {
    {"source", font_get_source, nullptr, "Path this was loaded from.", nullptr},
    {nullptr}
};

PyMethodDef font_methods[] = {
    {"measure", (PyCFunction)font_measure, METH_VARARGS | METH_KEYWORDS,
     "measure(text, size=16) -> (w, h) in pixels."},
    {nullptr}
};

// ---------------------------------------------------------------------------
// clippy.Drawable -- shared getters, setters and methods
//
// The float/int/point/colour/string accessors route through the same
// setProperty/getProperty pair that Animation uses, so the set of attribute
// names and the set of animatable property names cannot drift apart.
// ---------------------------------------------------------------------------

PyObject* dr_get_float(PyObject* self, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    float v;
    if (!d->getProperty((const char*)closure, v)) {
        PyErr_Format(PyExc_AttributeError, "no float property '%s'", (const char*)closure);
        return nullptr;
    }
    return PyFloat_FromDouble((double)v);
}

int dr_set_float(PyObject* self, PyObject* value, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return -1;
    if (!value) { PyErr_SetString(PyExc_AttributeError, "cannot delete"); return -1; }
    float v;
    if (!asFloat(value, v)) return -1;
    if (!d->setProperty((const char*)closure, v)) {
        PyErr_Format(PyExc_AttributeError, "no float property '%s'", (const char*)closure);
        return -1;
    }
    return 0;
}

PyObject* dr_get_int(PyObject* self, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    int v;
    if (!d->getProperty((const char*)closure, v)) {
        PyErr_Format(PyExc_AttributeError, "no int property '%s'", (const char*)closure);
        return nullptr;
    }
    return PyLong_FromLong(v);
}

int dr_set_int(PyObject* self, PyObject* value, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return -1;
    if (!value) { PyErr_SetString(PyExc_AttributeError, "cannot delete"); return -1; }
    const long v = PyLong_AsLong(value);
    if (v == -1 && PyErr_Occurred()) return -1;
    if (!d->setProperty((const char*)closure, (int)v)) {
        PyErr_Format(PyExc_AttributeError, "no int property '%s'", (const char*)closure);
        return -1;
    }
    return 0;
}

PyObject* dr_get_point(PyObject* self, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    SDL_FPoint v;
    if (!d->getProperty((const char*)closure, v)) {
        PyErr_Format(PyExc_AttributeError, "no point property '%s'", (const char*)closure);
        return nullptr;
    }
    return pointToPy(v);
}

int dr_set_point(PyObject* self, PyObject* value, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return -1;
    if (!value) { PyErr_SetString(PyExc_AttributeError, "cannot delete"); return -1; }
    const char* name = (const char*)closure;

    // `scale` takes a single number as shorthand for both axes -- the common
    // case, and what McRogueFace's float `scale` property means.
    if (!std::strcmp(name, "scale") && PyNumber_Check(value) && !PySequence_Check(value)) {
        float v;
        if (!asFloat(value, v)) return -1;
        d->scale = SDL_FPoint{v, v};
        return 0;
    }

    SDL_FPoint v;
    if (!asPoint(value, v, name)) return -1;
    if (!d->setProperty(name, v)) {
        PyErr_Format(PyExc_AttributeError, "no point property '%s'", name);
        return -1;
    }
    return 0;
}

PyObject* dr_get_color(PyObject* self, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    SDL_Color v;
    if (!d->getProperty((const char*)closure, v)) {
        PyErr_Format(PyExc_AttributeError, "no colour property '%s'", (const char*)closure);
        return nullptr;
    }
    return colorToPy(v);
}

int dr_set_color(PyObject* self, PyObject* value, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return -1;
    if (!value) { PyErr_SetString(PyExc_AttributeError, "cannot delete"); return -1; }
    SDL_Color v;
    if (!asColor(value, v, (const char*)closure)) return -1;
    if (!d->setProperty((const char*)closure, v)) {
        PyErr_Format(PyExc_AttributeError, "no colour property '%s'", (const char*)closure);
        return -1;
    }
    return 0;
}

PyObject* dr_get_str(PyObject* self, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    std::string v;
    if (!d->getProperty((const char*)closure, v)) {
        PyErr_Format(PyExc_AttributeError, "no string property '%s'", (const char*)closure);
        return nullptr;
    }
    return PyUnicode_FromString(v.c_str());
}

int dr_set_str(PyObject* self, PyObject* value, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return -1;
    if (!value) { PyErr_SetString(PyExc_AttributeError, "cannot delete"); return -1; }
    if (!PyUnicode_Check(value)) {
        PyErr_Format(PyExc_TypeError, "%s must be a str", (const char*)closure);
        return -1;
    }
    const char* s = PyUnicode_AsUTF8(value);
    if (!s) return -1;
    if (!d->setProperty((const char*)closure, std::string(s))) {
        PyErr_Format(PyExc_AttributeError, "no string property '%s'", (const char*)closure);
        return -1;
    }
    return 0;
}

PyObject* dr_get_visible(PyObject* self, void*)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    return PyBool_FromLong(d->visible ? 1 : 0);
}

int dr_set_visible(PyObject* self, PyObject* value, void*)
{
    Drawable* d = drawableOf(self); if (!d) return -1;
    const int v = PyObject_IsTrue(value);
    if (v < 0) return -1;
    d->visible = (v != 0);
    return 0;
}

PyObject* dr_get_name(PyObject* self, void*)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    return PyUnicode_FromString(d->name.c_str());
}

int dr_set_name(PyObject* self, PyObject* value, void*)
{
    Drawable* d = drawableOf(self); if (!d) return -1;
    if (!PyUnicode_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "name must be a str");
        return -1;
    }
    const char* s = PyUnicode_AsUTF8(value);
    if (!s) return -1;
    d->name = s;
    return 0;
}

PyObject* dr_get_align(PyObject* self, void*)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    return PyLong_FromLong((long)d->align);
}

int dr_set_align(PyObject* self, PyObject* value, void*)
{
    Drawable* d = drawableOf(self); if (!d) return -1;
    Align a;
    if (!alignFromArg(value, a)) return -1;
    d->align = a;
    d->realign();
    return 0;
}

PyObject* dr_get_margin(PyObject* self, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    const char* which = (const char*)closure;
    if (!std::strcmp(which, "margin"))       return PyFloat_FromDouble(d->margin);
    if (!std::strcmp(which, "horiz_margin")) return PyFloat_FromDouble(d->horiz_margin);
    return PyFloat_FromDouble(d->vert_margin);
}

int dr_set_margin(PyObject* self, PyObject* value, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return -1;
    float v;
    if (!asFloat(value, v)) return -1;
    const char* which = (const char*)closure;
    if (!std::strcmp(which, "margin"))            d->margin = v;
    else if (!std::strcmp(which, "horiz_margin")) d->horiz_margin = v;
    else                                          d->vert_margin = v;
    d->realign();
    return 0;
}

PyObject* dr_get_bounds(PyObject* self, void* closure)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    const bool global = closure != nullptr;
    return rectToPy(global ? d->globalBounds() : d->bounds());
}

PyObject* dr_get_global_pos(PyObject* self, void*)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    return pointToPy(d->globalPosition());
}

PyObject* dr_get_parent(PyObject* self, void*)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    auto p = d->parent.lock();
    if (!p) Py_RETURN_NONE;
    return PyDraw::wrap(p);
}

int dr_set_parent(PyObject* self, PyObject* value, void*)
{
    PyDrawableObject* obj = (PyDrawableObject*)self;
    if (!obj->data) { PyErr_SetString(PyExc_RuntimeError, "drawable is not initialised"); return -1; }

    if (!value || value == Py_None) {
        Drawable::detach(obj->data);
        return 0;
    }
    auto p = sharedDrawable(value);
    if (!p) {
        PyErr_SetString(PyExc_TypeError, "parent must be a clippy.Drawable or None");
        return -1;
    }
    if (p == obj->data) {
        PyErr_SetString(PyExc_ValueError, "a drawable cannot be its own parent");
        return -1;
    }
    // Walk up from the proposed parent: if we are already above it, this would
    // build a cycle, and the render walk would not return.
    for (auto up = p; up; up = up->parent.lock()) {
        if (up == obj->data) {
            PyErr_SetString(PyExc_ValueError,
                            "that parent is a descendant of this drawable");
            return -1;
        }
    }
    Stage::instance().remove(obj->data);
    Drawable::attach(obj->data, p);
    return 0;
}

PyObject* makeDrawList(const std::shared_ptr<Drawable>& owner);

PyObject* dr_get_children(PyObject* self, void*)
{
    PyDrawableObject* obj = (PyDrawableObject*)self;
    if (!obj->data) { PyErr_SetString(PyExc_RuntimeError, "drawable is not initialised"); return nullptr; }
    return makeDrawList(obj->data);
}

PyGetSetDef drawable_getset[] = {
    {"x",            dr_get_float, dr_set_float, "Pivot x, in the parent's space.", (void*)"x"},
    {"y",            dr_get_float, dr_set_float, "Pivot y, in the parent's space.", (void*)"y"},
    {"pos",          dr_get_point, dr_set_point, "Pivot as (x, y).",                (void*)"pos"},
    {"origin",       dr_get_point, dr_set_point, "Pivot within the content, in unscaled pixels.", (void*)"origin"},
    {"origin_x",     dr_get_float, dr_set_float, "Pivot x within the content.",     (void*)"origin_x"},
    {"origin_y",     dr_get_float, dr_set_float, "Pivot y within the content.",     (void*)"origin_y"},
    {"scale",        dr_get_point, dr_set_point, "(sx, sy), or assign one number for both. Negative mirrors.", (void*)"scale"},
    {"scale_x",      dr_get_float, dr_set_float, "Horizontal scale.",               (void*)"scale_x"},
    {"scale_y",      dr_get_float, dr_set_float, "Vertical scale.",                 (void*)"scale_y"},
    {"rotation",     dr_get_float, dr_set_float, "Degrees clockwise about origin.", (void*)"rotation"},
    {"opacity",      dr_get_float, dr_set_float, "0..1, multiplied onto children.", (void*)"opacity"},
    {"z_index",      dr_get_int,   dr_set_int,   "Draw order among siblings.",      (void*)"z_index"},
    {"visible",      dr_get_visible, dr_set_visible, "Whether this and its children draw.", nullptr},
    {"name",         dr_get_name,  dr_set_name,  "Free-form label.",                nullptr},
    {"align",        dr_get_align, dr_set_align, "A clippy.Align member; assigning realigns.", nullptr},
    {"margin",       dr_get_margin, dr_set_margin, "Alignment margin on all edges.", (void*)"margin"},
    {"horiz_margin", dr_get_margin, dr_set_margin, "Overrides margin horizontally (-1 to inherit).", (void*)"horiz_margin"},
    {"vert_margin",  dr_get_margin, dr_set_margin, "Overrides margin vertically (-1 to inherit).",   (void*)"vert_margin"},
    {"bounds",       dr_get_bounds, nullptr, "(x, y, w, h) in the parent's space.", nullptr},
    {"global_bounds", dr_get_bounds, nullptr, "(x, y, w, h) in stage space.", (void*)1},
    {"global_pos",   dr_get_global_pos, nullptr, "Pivot in stage space.", nullptr},
    {"parent",       dr_get_parent, dr_set_parent, "Owning drawable, or None.", nullptr},
    {"children",     dr_get_children, nullptr, "Mutable list of child drawables.", nullptr},
    {nullptr}
};

// --- methods ---------------------------------------------------------------

PyObject* dr_realign(PyObject* self, PyObject*)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    d->realign();
    Py_RETURN_NONE;
}

PyObject* dr_move(PyObject* self, PyObject* args)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    float dx, dy;
    if (!PyArg_ParseTuple(args, "ff:move", &dx, &dy)) return nullptr;
    d->position.x += dx;
    d->position.y += dy;
    Py_RETURN_NONE;
}

PyObject* dr_remove(PyObject* self, PyObject*)
{
    PyDrawableObject* obj = (PyDrawableObject*)self;
    if (!obj->data) { PyErr_SetString(PyExc_RuntimeError, "drawable is not initialised"); return nullptr; }
    Drawable::detach(obj->data);
    Stage::instance().remove(obj->data);
    Py_RETURN_NONE;
}

PyObject* makeAnimationWrapper(const std::shared_ptr<Animation>& anim);

PyObject* dr_animate(PyObject* self, PyObject* args, PyObject* kwds)
{
    static const char* kw[] = {"property", "target", "duration", "easing",
                               "delta", "loop", "callback", "conflict_mode",
                               nullptr};

    const char* property_name  = nullptr;
    PyObject*   target_value   = nullptr;
    float       duration       = 0.0f;
    PyObject*   easing_arg     = nullptr;
    int         delta          = 0;
    int         loop           = 0;
    PyObject*   callback       = nullptr;
    const char* conflict_str   = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sOf|OppOs", (char**)kw,
                                     &property_name, &target_value, &duration,
                                     &easing_arg, &delta, &loop, &callback,
                                     &conflict_str)) {
        return nullptr;
    }

    PyDrawableObject* obj = (PyDrawableObject*)self;
    if (!obj->data) {
        PyErr_SetString(PyExc_RuntimeError, "drawable is not initialised");
        return nullptr;
    }
    Drawable* d = obj->data.get();

    // A misspelled property would otherwise be an animation that runs to
    // completion having changed nothing.
    if (!d->hasProperty(property_name)) {
        PyErr_Format(PyExc_ValueError,
                     "'%s' is not an animatable property of %s",
                     property_name, Py_TYPE(self)->tp_name);
        return nullptr;
    }

    if (duration < 0.0f) {
        PyErr_SetString(PyExc_ValueError, "duration must be >= 0");
        return nullptr;
    }

    // --- target value ------------------------------------------------------
    AnimationValue value;
    if (PyBool_Check(target_value)) {
        PyErr_SetString(PyExc_TypeError, "animation target must not be a bool");
        return nullptr;
    } else if (PyFloat_Check(target_value)) {
        value = (float)PyFloat_AsDouble(target_value);
    } else if (PyLong_Check(target_value)) {
        const long v = PyLong_AsLong(target_value);
        if (v == -1 && PyErr_Occurred()) return nullptr;
        value = (int)v;
    } else if (PyList_Check(target_value)) {
        // A frame sequence: step through these indices over the duration.
        std::vector<int> frames;
        const Py_ssize_t n = PyList_Size(target_value);
        if (n == 0) {
            PyErr_SetString(PyExc_ValueError, "frame sequence must not be empty");
            return nullptr;
        }
        frames.reserve((size_t)n);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject* item = PyList_GetItem(target_value, i);   // borrowed
            if (!PyLong_Check(item) || PyBool_Check(item)) {
                PyErr_SetString(PyExc_TypeError,
                                "a frame sequence must contain only ints");
                return nullptr;
            }
            frames.push_back((int)PyLong_AsLong(item));
        }
        value = frames;
    } else if (PyTuple_Check(target_value)) {
        const Py_ssize_t n = PyTuple_Size(target_value);
        if (n == 2) {
            SDL_FPoint p;
            if (!asPoint(target_value, p, "animation target")) return nullptr;
            value = p;
        } else if (n == 3 || n == 4) {
            SDL_Color c;
            if (!asColor(target_value, c, "animation target")) return nullptr;
            value = c;
        } else {
            PyErr_SetString(PyExc_ValueError,
                            "a tuple target must be 2 (point) or 3-4 (colour) long");
            return nullptr;
        }
    } else if (PyUnicode_Check(target_value)) {
        value = std::string(PyUnicode_AsUTF8(target_value));
    } else {
        PyErr_SetString(PyExc_TypeError,
                        "animation target must be a float, int, list of ints, "
                        "2-tuple, 3/4-tuple or str");
        return nullptr;
    }

    EasingFunction easing;
    if (!easingFromArg(easing_arg, easing)) return nullptr;

    ConflictMode mode = ConflictMode::REPLACE;
    if (conflict_str) {
        if      (!std::strcmp(conflict_str, "replace")) mode = ConflictMode::REPLACE;
        else if (!std::strcmp(conflict_str, "queue"))   mode = ConflictMode::QUEUE;
        else if (!std::strcmp(conflict_str, "error"))   mode = ConflictMode::RAISE_ERROR;
        else {
            PyErr_Format(PyExc_ValueError,
                         "invalid conflict_mode '%s'; expected 'replace', "
                         "'queue' or 'error'", conflict_str);
            return nullptr;
        }
    }

    // --- completion callback ----------------------------------------------
    AnimationCallback trampoline;
    if (callback && callback != Py_None) {
        if (!PyCallable_Check(callback)) {
            PyErr_SetString(PyExc_TypeError, "callback must be callable");
            return nullptr;
        }
        // The reference is owned by a shared_ptr with a releasing deleter, so
        // it is dropped exactly once however the animation ends -- completed,
        // replaced, or abandoned because the target went away.
        Py_INCREF(callback);
        std::shared_ptr<PyObject> held(callback, [](PyObject* o) {
            if (o && Py_IsInitialized()) {
                PyGILState_STATE g = PyGILState_Ensure();
                Py_DECREF(o);
                PyGILState_Release(g);
            }
        });

        std::weak_ptr<Drawable> weak_target = obj->data;
        const std::string       prop        = property_name;

        trampoline = [held, weak_target, prop]() {
            auto target = weak_target.lock();
            if (!target) return;

            PyGILState_STATE g = PyGILState_Ensure();
            PyObject* py_target = PyDraw::wrap(target);
            PyObject* py_value  = propertyToPy(target.get(), prop);
            if (py_target && py_value) {
                PyObject* result = PyObject_CallFunction(
                    held.get(), "OsO", py_target, prop.c_str(), py_value);
                // Print rather than swallow: a broken callback must be visible.
                if (!result) PyErr_Print();
                else         Py_DECREF(result);
            } else {
                PyErr_Print();
            }
            Py_XDECREF(py_target);
            Py_XDECREF(py_value);
            PyGILState_Release(g);
        };
    }

    auto animation = std::make_shared<Animation>(
        property_name, std::move(value), duration, easing,
        delta != 0, loop != 0, std::move(trampoline));

    animation->start(obj->data);

    if (!AnimationManager::instance().add(animation, mode)) {
        PyErr_Format(PyExc_RuntimeError,
                     "'%s' is already being animated on this drawable; pass "
                     "conflict_mode='replace' or 'queue'", property_name);
        return nullptr;
    }

    return makeAnimationWrapper(animation);
}

PyMethodDef drawable_methods[] = {
    {"animate", (PyCFunction)dr_animate, METH_VARARGS | METH_KEYWORDS,
     "animate(property, target, duration, easing=None, delta=False, loop=False,\n"
     "        callback=None, conflict_mode='replace') -> Animation\n\n"
     "target may be a number, a list of ints (a frame sequence), a 2-tuple\n"
     "(point), a 3/4-tuple (colour) or a str. The callback is invoked with\n"
     "(drawable, property, final_value)."},
    {"realign", dr_realign, METH_NOARGS,
     "Recompute pos from `align` against the parent's bounds (or the window)."},
    {"move",    dr_move,    METH_VARARGS, "move(dx, dy) -> shift pos."},
    {"remove",  dr_remove,  METH_NOARGS,
     "Detach from the parent or the stage. The object stays usable."},
    {nullptr}
};

PyObject* drawable_repr(PyObject* self)
{
    Drawable* d = drawableOf(self);
    if (!d) { PyErr_Clear(); return PyUnicode_FromString("<clippy.Drawable uninitialised>"); }
    return PyUnicode_FromFormat("<%s '%s' at (%d, %d)%s>",
                                Py_TYPE(self)->tp_name, d->name.c_str(),
                                (int)d->position.x, (int)d->position.y,
                                d->visible ? "" : " hidden");
}

// Everything a Sprite and a Caption share, applied after the type-specific
// arguments so that `align` sees the final size.
bool applyCommon(Drawable* d, PyObject* o_pos, PyObject* o_origin,
                 PyObject* o_scale, float rotation, float opacity,
                 PyObject* o_visible, int z_index, const char* name,
                 PyObject* o_align, float margin)
{
    if (o_pos    && !asPoint(o_pos,    d->position, "pos"))    return false;
    if (o_origin && !asPoint(o_origin, d->origin,   "origin")) return false;

    if (o_scale) {
        if (PyNumber_Check(o_scale) && !PySequence_Check(o_scale)) {
            float v;
            if (!asFloat(o_scale, v)) return false;
            d->scale = SDL_FPoint{v, v};
        } else if (!asPoint(o_scale, d->scale, "scale")) {
            return false;
        }
    }

    d->rotation = rotation;
    d->opacity  = SDL_clamp(opacity, 0.0f, 1.0f);
    d->z_index  = z_index;
    if (name) d->name = name;

    if (o_visible) {
        const int v = PyObject_IsTrue(o_visible);
        if (v < 0) return false;
        d->visible = (v != 0);
    }

    d->margin = margin;
    Align a;
    if (!alignFromArg(o_align, a)) return false;
    d->align = a;
    return true;
}

// ---------------------------------------------------------------------------
// clippy.Sprite
// ---------------------------------------------------------------------------

int sprite_init(PyObject* self, PyObject* args, PyObject* kwds)
{
    static const char* kw[] = {"texture", "pos", "sprite_index", "origin",
                               "scale", "rotation", "opacity", "visible",
                               "z_index", "name", "align", "margin", "color",
                               "parent", nullptr};

    PyObject* o_tex     = nullptr;
    PyObject* o_pos     = nullptr;
    int       index     = 0;
    PyObject* o_origin  = nullptr;
    PyObject* o_scale   = nullptr;
    float     rotation  = 0.0f;
    float     opacity   = 1.0f;
    PyObject* o_visible = nullptr;
    int       z_index   = 0;
    const char* name    = nullptr;
    PyObject* o_align   = nullptr;
    float     margin    = 0.0f;
    PyObject* o_color   = nullptr;
    PyObject* o_parent  = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OOiOOffOisOfOO", (char**)kw,
                                     &o_tex, &o_pos, &index, &o_origin,
                                     &o_scale, &rotation, &opacity, &o_visible,
                                     &z_index, &name, &o_align, &margin,
                                     &o_color, &o_parent)) {
        return -1;
    }

    auto sprite = std::make_shared<Sprite>();

    if (o_tex && o_tex != Py_None) {
        if (!PyObject_TypeCheck(o_tex, &TextureType)) {
            PyErr_SetString(PyExc_TypeError, "texture must be a clippy.Texture");
            return -1;
        }
        sprite->setTexture(((PyTextureObject*)o_tex)->data);
    }
    sprite->setSpriteIndex(index);

    if (o_color && !asColor(o_color, sprite->color, "color")) return -1;

    if (!applyCommon(sprite.get(), o_pos, o_origin, o_scale, rotation, opacity,
                     o_visible, z_index, name, o_align, margin)) {
        return -1;
    }

    ((PyDrawableObject*)self)->data = sprite;

    if (o_parent && o_parent != Py_None) {
        auto parent = sharedDrawable(o_parent);
        if (!parent) {
            PyErr_SetString(PyExc_TypeError, "parent must be a clippy.Drawable");
            return -1;
        }
        Drawable::attach(sprite, parent);
    }

    sprite->realign();
    return 0;
}

// The Sprite and Caption accessors below downcast. Their getsets are only
// installed on the matching type, but a descriptor can be pulled off the class
// and applied to anything -- clippy.Sprite.texture.__get__(some_caption) -- so
// the kind is checked rather than assumed.
Sprite* spriteOf(PyObject* self)
{
    Drawable* d = drawableOf(self);
    if (!d) return nullptr;
    if (d->kind() != Drawable::Kind::Sprite) {
        PyErr_SetString(PyExc_TypeError, "expected a clippy.Sprite");
        return nullptr;
    }
    return (Sprite*)d;
}

Caption* captionOf(PyObject* self)
{
    Drawable* d = drawableOf(self);
    if (!d) return nullptr;
    if (d->kind() != Drawable::Kind::Caption) {
        PyErr_SetString(PyExc_TypeError, "expected a clippy.Caption");
        return nullptr;
    }
    return (Caption*)d;
}

PyObject* sprite_get_texture(PyObject* self, void*)
{
    Sprite* s = spriteOf(self); if (!s) return nullptr;
    auto tex = s->texture();
    if (!tex) Py_RETURN_NONE;

    PyTextureObject* obj = (PyTextureObject*)TextureType.tp_alloc(&TextureType, 0);
    if (!obj) return nullptr;
    new (&obj->data) std::shared_ptr<Texture>(std::move(tex));
    return (PyObject*)obj;
}

int sprite_set_texture(PyObject* self, PyObject* value, void*)
{
    Sprite* s = spriteOf(self); if (!s) return -1;
    if (!value || value == Py_None) {
        s->setTexture(nullptr);
        return 0;
    }
    if (!PyObject_TypeCheck(value, &TextureType)) {
        PyErr_SetString(PyExc_TypeError, "texture must be a clippy.Texture or None");
        return -1;
    }
    s->setTexture(((PyTextureObject*)value)->data);
    return 0;
}

PyGetSetDef sprite_getset[] = {
    {"texture",      sprite_get_texture, sprite_set_texture, "The clippy.Texture drawn.", nullptr},
    {"sprite_index", dr_get_int, dr_set_int, "Frame index into the texture's cell grid.", (void*)"sprite_index"},
    {"color",        dr_get_color, dr_set_color, "(r, g, b[, a]) multiplied onto the texture.", (void*)"color"},
    {nullptr}
};

// ---------------------------------------------------------------------------
// clippy.Caption
// ---------------------------------------------------------------------------

int caption_init(PyObject* self, PyObject* args, PyObject* kwds)
{
    static const char* kw[] = {"text", "font", "font_size", "fill_color",
                               "pos", "origin", "scale", "rotation", "opacity",
                               "visible", "z_index", "name", "align", "margin",
                               "parent", nullptr};

    const char* text      = nullptr;
    PyObject*   o_font    = nullptr;
    int         font_size = 16;
    PyObject*   o_fill    = nullptr;
    PyObject*   o_pos     = nullptr;
    PyObject*   o_origin  = nullptr;
    PyObject*   o_scale   = nullptr;
    float       rotation  = 0.0f;
    float       opacity   = 1.0f;
    PyObject*   o_visible = nullptr;
    int         z_index   = 0;
    const char* name      = nullptr;
    PyObject*   o_align   = nullptr;
    float       margin    = 0.0f;
    PyObject*   o_parent  = nullptr;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|sOiOOOOffOisOfO", (char**)kw,
                                     &text, &o_font, &font_size, &o_fill,
                                     &o_pos, &o_origin, &o_scale, &rotation,
                                     &opacity, &o_visible, &z_index, &name,
                                     &o_align, &margin, &o_parent)) {
        return -1;
    }

    auto caption = std::make_shared<Caption>();
    if (text) caption->setText(text);
    caption->setFontSize(font_size);

    if (o_font && o_font != Py_None) {
        if (!PyObject_TypeCheck(o_font, &FontType)) {
            PyErr_SetString(PyExc_TypeError, "font must be a clippy.Font");
            return -1;
        }
        caption->setFont(((PyFontObject*)o_font)->data);
    }

    if (o_fill && !asColor(o_fill, caption->fill_color, "fill_color")) return -1;

    if (!applyCommon(caption.get(), o_pos, o_origin, o_scale, rotation, opacity,
                     o_visible, z_index, name, o_align, margin)) {
        return -1;
    }

    ((PyDrawableObject*)self)->data = caption;

    if (o_parent && o_parent != Py_None) {
        auto parent = sharedDrawable(o_parent);
        if (!parent) {
            PyErr_SetString(PyExc_TypeError, "parent must be a clippy.Drawable");
            return -1;
        }
        Drawable::attach(caption, parent);
    }

    // realign() is what forces the first rasterise, via contentSize(). Even
    // with no alignment set, ask for the size here: a caption that cannot
    // rasterise draws nothing, and the reason has to surface at the
    // constructor rather than as unexplained blank space later.
    caption->realign();
    (void)caption->contentSize();

    const std::string& err = caption->error();
    if (!err.empty()) {
        PyErr_SetString(PyExc_RuntimeError, err.c_str());
        return -1;
    }
    return 0;
}

PyObject* caption_get_font(PyObject* self, void*)
{
    Caption* c = captionOf(self); if (!c) return nullptr;
    auto font = c->font();
    if (!font) Py_RETURN_NONE;

    PyFontObject* obj = (PyFontObject*)FontType.tp_alloc(&FontType, 0);
    if (!obj) return nullptr;
    new (&obj->data) std::shared_ptr<Font>(std::move(font));
    return (PyObject*)obj;
}

int caption_set_font(PyObject* self, PyObject* value, void*)
{
    Caption* c = captionOf(self); if (!c) return -1;
    if (!value || value == Py_None) {
        c->setFont(nullptr);
        return 0;
    }
    if (!PyObject_TypeCheck(value, &FontType)) {
        PyErr_SetString(PyExc_TypeError, "font must be a clippy.Font or None");
        return -1;
    }
    c->setFont(((PyFontObject*)value)->data);
    return 0;
}

PyObject* caption_get_skipped(PyObject* self, void*)
{
    Caption* c = captionOf(self); if (!c) return nullptr;
    return PyLong_FromLong(c->skippedGlyphs());
}

PyObject* caption_get_size(PyObject* self, void*)
{
    Drawable* d = drawableOf(self); if (!d) return nullptr;
    return pointToPy(d->contentSize());
}

PyGetSetDef caption_getset[] = {
    {"text",       dr_get_str, dr_set_str, "The text drawn.", (void*)"text"},
    {"font",       caption_get_font, caption_set_font, "The clippy.Font used.", nullptr},
    {"font_size",  dr_get_int, dr_set_int, "Pixel size the glyphs are rasterised at.", (void*)"font_size"},
    {"fill_color", dr_get_color, dr_set_color, "(r, g, b[, a]) the glyphs are tinted with.", (void*)"fill_color"},
    {"text_size",  caption_get_size, nullptr, "Measured (w, h) of the rendered text.", nullptr},
    {"skipped_glyphs", caption_get_skipped, nullptr,
     "Characters outside the font's ASCII range dropped from the last render.", nullptr},
    {nullptr}
};

// ---------------------------------------------------------------------------
// clippy.Animation -- a handle on a running animation
// ---------------------------------------------------------------------------

PyObject* makeAnimationWrapper(const std::shared_ptr<Animation>& anim)
{
    PyAnimationObject* obj =
        (PyAnimationObject*)AnimationType.tp_alloc(&AnimationType, 0);
    if (!obj) return nullptr;
    new (&obj->data) std::shared_ptr<Animation>(anim);
    return (PyObject*)obj;
}

Animation* animationOf(PyObject* self)
{
    Animation* a = ((PyAnimationObject*)self)->data.get();
    if (!a) PyErr_SetString(PyExc_RuntimeError, "animation is not initialised");
    return a;
}

PyObject* anim_stop(PyObject* self, PyObject*)
{
    Animation* a = animationOf(self); if (!a) return nullptr;
    a->stop();
    Py_RETURN_NONE;
}

PyObject* anim_complete(PyObject* self, PyObject*)
{
    Animation* a = animationOf(self); if (!a) return nullptr;
    a->complete();
    Py_RETURN_NONE;
}

PyMethodDef animation_methods[] = {
    {"stop",     anim_stop,     METH_NOARGS,
     "Abandon without applying the final value and without firing the callback."},
    {"complete", anim_complete, METH_NOARGS,
     "Jump to the final value now and fire the callback."},
    {nullptr}
};

PyObject* anim_get(PyObject* self, void* closure)
{
    Animation* a = animationOf(self); if (!a) return nullptr;
    const char* which = (const char*)closure;
    if (!std::strcmp(which, "property")) return PyUnicode_FromString(a->targetProperty().c_str());
    if (!std::strcmp(which, "duration")) return PyFloat_FromDouble(a->duration());
    if (!std::strcmp(which, "elapsed"))  return PyFloat_FromDouble(a->elapsed());
    if (!std::strcmp(which, "loop"))     return PyBool_FromLong(a->isLooping() ? 1 : 0);
    if (!std::strcmp(which, "delta"))    return PyBool_FromLong(a->isDelta() ? 1 : 0);
    if (!std::strcmp(which, "stopped"))  return PyBool_FromLong(a->isStopped() ? 1 : 0);
    return PyBool_FromLong(a->isComplete() ? 1 : 0);
}

// Named `is_complete`, not `complete`: a getset and a method cannot share a
// name. PyType_Ready fills tp_dict from tp_methods before tp_getset and keeps
// whichever landed first, so a `complete` property would silently resolve to
// the complete() method and read as permanently true.
PyGetSetDef animation_getset[] = {
    {"property",    anim_get, nullptr, "Name of the animated property.", (void*)"property"},
    {"duration",    anim_get, nullptr, "Total seconds.",   (void*)"duration"},
    {"elapsed",     anim_get, nullptr, "Seconds so far.",  (void*)"elapsed"},
    {"loop",        anim_get, nullptr, "Whether it repeats.", (void*)"loop"},
    {"delta",       anim_get, nullptr, "Whether the target is relative.", (void*)"delta"},
    {"stopped",     anim_get, nullptr, "Whether stop() was called.", (void*)"stopped"},
    {"is_complete", anim_get, nullptr, "Whether it has finished.", (void*)"is_complete"},
    {nullptr}
};

PyObject* animation_repr(PyObject* self)
{
    Animation* a = ((PyAnimationObject*)self)->data.get();
    if (!a) return PyUnicode_FromString("<clippy.Animation uninitialised>");
    return PyUnicode_FromFormat("<clippy.Animation '%s' %d%% of %dms>",
                                a->targetProperty().c_str(),
                                a->duration() > 0.0f
                                    ? (int)(100.0f * a->elapsed() / a->duration())
                                    : 100,
                                (int)(a->duration() * 1000.0f));
}

// ---------------------------------------------------------------------------
// The drawable lists: clippy.stage and drawable.children
// ---------------------------------------------------------------------------

std::vector<std::shared_ptr<Drawable>>* listOf(PyObject* self)
{
    PyDrawListObject* obj = (PyDrawListObject*)self;
    if (obj->data) return &obj->data->children;
    return &Stage::instance().roots;
}

PyObject* makeDrawList(const std::shared_ptr<Drawable>& owner)
{
    PyDrawListObject* obj =
        (PyDrawListObject*)DrawListType.tp_alloc(&DrawListType, 0);
    if (!obj) return nullptr;
    new (&obj->data) std::shared_ptr<Drawable>(owner);
    return (PyObject*)obj;
}

Py_ssize_t drawlist_len(PyObject* self)
{
    return (Py_ssize_t)listOf(self)->size();
}

PyObject* drawlist_item(PyObject* self, Py_ssize_t index)
{
    auto* v = listOf(self);
    if (index < 0) index += (Py_ssize_t)v->size();
    if (index < 0 || index >= (Py_ssize_t)v->size()) {
        PyErr_SetString(PyExc_IndexError, "index out of range");
        return nullptr;
    }
    return PyDraw::wrap((*v)[(size_t)index]);
}

PyObject* drawlist_append(PyObject* self, PyObject* arg)
{
    auto child = sharedDrawable(arg);
    if (!child) {
        PyErr_SetString(PyExc_TypeError, "expected a clippy.Drawable");
        return nullptr;
    }

    PyDrawListObject* obj = (PyDrawListObject*)self;
    if (obj->data) {
        if (obj->data == child) {
            PyErr_SetString(PyExc_ValueError, "a drawable cannot be its own child");
            return nullptr;
        }
        for (auto up = obj->data; up; up = up->parent.lock()) {
            if (up == child) {
                PyErr_SetString(PyExc_ValueError,
                                "that drawable is an ancestor of this one");
                return nullptr;
            }
        }
        Stage::instance().remove(child);
        Drawable::attach(child, obj->data);
    } else {
        Stage::instance().add(child);
    }
    Py_RETURN_NONE;
}

PyObject* drawlist_remove(PyObject* self, PyObject* arg)
{
    auto child = sharedDrawable(arg);
    if (!child) {
        PyErr_SetString(PyExc_TypeError, "expected a clippy.Drawable");
        return nullptr;
    }

    auto* v = listOf(self);
    const size_t before = v->size();

    PyDrawListObject* obj = (PyDrawListObject*)self;
    if (obj->data) Drawable::detach(child);
    else           Stage::instance().remove(child);

    if (v->size() == before) {
        PyErr_SetString(PyExc_ValueError, "drawable is not in this list");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* drawlist_clear(PyObject* self, PyObject*)
{
    auto* v = listOf(self);
    // Copy first: detaching mutates the very vector being walked.
    std::vector<std::shared_ptr<Drawable>> copy = *v;
    PyDrawListObject* obj = (PyDrawListObject*)self;
    for (auto& d : copy) {
        if (obj->data) Drawable::detach(d);
        else           Stage::instance().remove(d);
    }
    Py_RETURN_NONE;
}

int drawlist_contains(PyObject* self, PyObject* arg)
{
    auto child = sharedDrawable(arg);
    if (!child) return 0;
    auto* v = listOf(self);
    for (const auto& d : *v) if (d == child) return 1;
    return 0;
}

PyMethodDef drawlist_methods[] = {
    {"append", drawlist_append, METH_O,      "Add a drawable, removing it from wherever it was."},
    {"remove", drawlist_remove, METH_O,      "Remove a drawable. ValueError if it is not here."},
    {"clear",  drawlist_clear,  METH_NOARGS, "Remove every drawable."},
    {nullptr}
};

PySequenceMethods drawlist_sequence = {};

PyObject* drawlist_repr(PyObject* self)
{
    PyDrawListObject* obj = (PyDrawListObject*)self;
    return PyUnicode_FromFormat("<clippy.DrawableList (%s) of %zd>",
                                obj->data ? "children" : "stage",
                                (Py_ssize_t)listOf(self)->size());
}

// ---------------------------------------------------------------------------
// Enums, built the way McRogueFace builds them: a real IntEnum from Python's
// own enum module, so members print by name and compare as ints.
// ---------------------------------------------------------------------------

PyObject* makeIntEnum(const char* enum_name,
                      const char* const* names, const long* values, int n)
{
    PyObject* enum_module = PyImport_ImportModule("enum");
    if (!enum_module) return nullptr;

    PyObject* int_enum = PyObject_GetAttrString(enum_module, "IntEnum");
    Py_DECREF(enum_module);
    if (!int_enum) return nullptr;

    PyObject* members = PyDict_New();
    if (!members) { Py_DECREF(int_enum); return nullptr; }

    for (int i = 0; i < n; ++i) {
        PyObject* v = PyLong_FromLong(values[i]);
        if (!v || PyDict_SetItemString(members, names[i], v) < 0) {
            Py_XDECREF(v);
            Py_DECREF(members);
            Py_DECREF(int_enum);
            return nullptr;
        }
        Py_DECREF(v);
    }

    PyObject* result = PyObject_CallFunction(int_enum, "sO", enum_name, members);
    Py_DECREF(members);
    Py_DECREF(int_enum);
    return result;
}

bool addEasingEnum(PyObject* module)
{
    std::vector<const char*> names;
    std::vector<long>        values;
    names.reserve((size_t)Easing::count);
    values.reserve((size_t)Easing::count);
    for (int i = 0; i < Easing::count; ++i) {
        names.push_back(Easing::table[i].name);
        values.push_back(i);
    }

    PyObject* e = makeIntEnum("Easing", names.data(), values.data(), Easing::count);
    if (!e) return false;
    // Steals the reference on success; on failure we still own it.
    if (PyModule_AddObject(module, "Easing", e) < 0) { Py_DECREF(e); return false; }
    return true;
}

bool addAlignEnum(PyObject* module)
{
    static const char* names[] = {
        "NONE", "TOP_LEFT", "TOP_CENTER", "TOP_RIGHT",
        "CENTER_LEFT", "CENTER", "CENTER_RIGHT",
        "BOTTOM_LEFT", "BOTTOM_CENTER", "BOTTOM_RIGHT"};
    static const long values[] = {-1, 0, 1, 2, 3, 4, 5, 6, 7, 8};

    PyObject* e = makeIntEnum("Align", names, values, 10);
    if (!e) return false;
    if (PyModule_AddObject(module, "Align", e) < 0) { Py_DECREF(e); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Type setup
// ---------------------------------------------------------------------------

void configureTypes()
{
    TextureType.tp_name      = "clippy.Texture";
    TextureType.tp_basicsize = sizeof(PyTextureObject);
    TextureType.tp_flags     = Py_TPFLAGS_DEFAULT;
    TextureType.tp_doc       = PyDoc_STR(
        "Texture(path, sprite_width=0, sprite_height=0, smooth=True)\n\n"
        "A PNG, optionally sliced into a grid of equally sized frames.\n"
        "A bare filename resolves against assets/. Cell dimensions of 0 mean\n"
        "the whole image is one frame. Frames are numbered left to right,\n"
        "then top to bottom, and that number is a Sprite's sprite_index.");
    TextureType.tp_new       = genericNew<PyTextureObject>;
    TextureType.tp_dealloc   = genericDealloc<PyTextureObject>;
    TextureType.tp_init      = texture_init;
    TextureType.tp_repr      = texture_repr;
    TextureType.tp_getset    = texture_getset;

    FontType.tp_name      = "clippy.Font";
    FontType.tp_basicsize = sizeof(PyFontObject);
    FontType.tp_flags     = Py_TPFLAGS_DEFAULT;
    FontType.tp_doc       = PyDoc_STR(
        "Font(path)\n\n"
        "A TrueType face. A bare filename resolves against assets/.\n"
        "Glyphs are rasterised on demand, once per pixel size used.\n"
        "Coverage is ASCII 32..126.");
    FontType.tp_new       = genericNew<PyFontObject>;
    FontType.tp_dealloc   = genericDealloc<PyFontObject>;
    FontType.tp_init      = font_init;
    FontType.tp_repr      = font_repr;
    FontType.tp_getset    = font_getset;
    FontType.tp_methods   = font_methods;

    DrawableType.tp_name      = "clippy.Drawable";
    DrawableType.tp_basicsize = sizeof(PyDrawableObject);
    DrawableType.tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    DrawableType.tp_doc       = PyDoc_STR(
        "Base class for Sprite and Caption. Not constructible on its own.\n\n"
        "`pos` is the pivot: the point `origin` of the content sits there, and\n"
        "rotation and scale happen about it. Children inherit their parent's\n"
        "translation and opacity, but never its scale or rotation -- which is\n"
        "what lets the paperclip stretch without distorting the eyes.");
    DrawableType.tp_new       = genericNew<PyDrawableObject>;
    DrawableType.tp_dealloc   = genericDealloc<PyDrawableObject>;
    DrawableType.tp_repr      = drawable_repr;
    DrawableType.tp_getset    = drawable_getset;
    DrawableType.tp_methods   = drawable_methods;

    SpriteType.tp_name      = "clippy.Sprite";
    SpriteType.tp_basicsize = sizeof(PyDrawableObject);
    SpriteType.tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    SpriteType.tp_doc       = PyDoc_STR(
        "Sprite(texture=None, pos=(0, 0), sprite_index=0, origin=None,\n"
        "       scale=1.0, rotation=0.0, opacity=1.0, visible=True,\n"
        "       z_index=0, name='', align=None, margin=0.0, color=None,\n"
        "       parent=None)\n\n"
        "One frame of a Texture. Animate 'sprite_index' against a list of\n"
        "ints to play a frame sequence.");
    SpriteType.tp_base      = &DrawableType;
    SpriteType.tp_init      = sprite_init;
    SpriteType.tp_getset    = sprite_getset;

    CaptionType.tp_name      = "clippy.Caption";
    CaptionType.tp_basicsize = sizeof(PyDrawableObject);
    CaptionType.tp_flags     = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    CaptionType.tp_doc       = PyDoc_STR(
        "Caption(text='', font=None, font_size=16, fill_color=None,\n"
        "        pos=(0, 0), origin=None, scale=1.0, rotation=0.0,\n"
        "        opacity=1.0, visible=True, z_index=0, name='', align=None,\n"
        "        margin=0.0, parent=None)\n\n"
        "Text. Newlines start a new line. Animate 'opacity' to fade, or\n"
        "'text' for a typewriter reveal.");
    CaptionType.tp_base      = &DrawableType;
    CaptionType.tp_init      = caption_init;
    CaptionType.tp_getset    = caption_getset;

    AnimationType.tp_name      = "clippy.Animation";
    AnimationType.tp_basicsize = sizeof(PyAnimationObject);
    AnimationType.tp_flags     = Py_TPFLAGS_DEFAULT;
    AnimationType.tp_doc       = PyDoc_STR(
        "A running animation, as returned by Drawable.animate().\n"
        "Not constructed directly.");
    AnimationType.tp_new       = genericNew<PyAnimationObject>;
    AnimationType.tp_dealloc   = genericDealloc<PyAnimationObject>;
    AnimationType.tp_repr      = animation_repr;
    AnimationType.tp_getset    = animation_getset;
    AnimationType.tp_methods   = animation_methods;

    drawlist_sequence.sq_length   = drawlist_len;
    drawlist_sequence.sq_item     = drawlist_item;
    drawlist_sequence.sq_contains = drawlist_contains;

    DrawListType.tp_name        = "clippy.DrawableList";
    DrawListType.tp_basicsize   = sizeof(PyDrawListObject);
    DrawListType.tp_flags       = Py_TPFLAGS_DEFAULT;
    DrawListType.tp_doc         = PyDoc_STR(
        "A live view of a drawable list -- clippy.stage, or a drawable's\n"
        ".children. Indexing, len(), `in`, append(), remove() and clear()\n"
        "operate on the real list, not a copy.");
    DrawListType.tp_new         = genericNew<PyDrawListObject>;
    DrawListType.tp_dealloc     = genericDealloc<PyDrawListObject>;
    DrawListType.tp_repr        = drawlist_repr;
    DrawListType.tp_as_sequence = &drawlist_sequence;
    DrawListType.tp_methods     = drawlist_methods;
}

} // namespace

namespace PyDraw {

PyObject* wrap(const std::shared_ptr<Drawable>& drawable)
{
    if (!drawable) Py_RETURN_NONE;

    PyTypeObject* type = &DrawableType;
    switch (drawable->kind()) {
    case Drawable::Kind::Sprite:  type = &SpriteType;  break;
    case Drawable::Kind::Caption: type = &CaptionType; break;
    }

    PyDrawableObject* obj = (PyDrawableObject*)type->tp_alloc(type, 0);
    if (!obj) return nullptr;
    new (&obj->data) std::shared_ptr<Drawable>(drawable);
    return (PyObject*)obj;
}

bool addToModule(PyObject* module, std::string& error_out)
{
    configureTypes();

    struct { PyTypeObject* type; const char* name; } types[] = {
        {&TextureType,   "Texture"},
        {&FontType,      "Font"},
        {&DrawableType,  "Drawable"},
        {&SpriteType,    "Sprite"},
        {&CaptionType,   "Caption"},
        {&AnimationType, "Animation"},
        {&DrawListType,  "DrawableList"},
    };

    for (auto& t : types) {
        if (PyType_Ready(t.type) < 0) {
            error_out = std::string("PyType_Ready(clippy.") + t.name + ") failed";
            return false;
        }
        Py_INCREF(t.type);
        if (PyModule_AddObject(module, t.name, (PyObject*)t.type) < 0) {
            Py_DECREF(t.type);
            error_out = std::string("could not add clippy.") + t.name;
            return false;
        }
    }

    if (!addEasingEnum(module)) { error_out = "could not build clippy.Easing"; return false; }
    if (!addAlignEnum(module))  { error_out = "could not build clippy.Align";  return false; }

    // The stage itself: a DrawableList with no owner.
    PyObject* stage = makeDrawList(nullptr);
    if (!stage || PyModule_AddObject(module, "stage", stage) < 0) {
        Py_XDECREF(stage);
        error_out = "could not add clippy.stage";
        return false;
    }

    return true;
}

} // namespace PyDraw
