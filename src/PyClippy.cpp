#include "PyClippy.h"

#include <Python.h>

#include <map>
#include <string>

#include "App.h"
#include "AppPaths.h"
#include "Drawable.h"
#include "PyDraw.h"

namespace {

App* g_app = nullptr;
std::map<std::string, PyObject*> g_hooks;

// Every entry point goes through this so a script that somehow runs before
// bind() gets a clear error instead of dereferencing null.
App* app_or_error()
{
    if (!g_app) {
        PyErr_SetString(PyExc_RuntimeError,
                        "clippy: host not bound (called too early?)");
        return nullptr;
    }
    return g_app;
}

PyObject* c_show(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    a->setVisible(true);
    Py_RETURN_NONE;
}

PyObject* c_hide(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    a->setVisible(false);
    Py_RETURN_NONE;
}

PyObject* c_toggle(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    a->setVisible(!a->window.visible());
    Py_RETURN_NONE;
}

PyObject* c_visible(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    return PyBool_FromLong(a->window.visible() ? 1 : 0);
}

PyObject* c_quit(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    a->running = false;
    Py_RETURN_NONE;
}

PyObject* c_set_sprite(PyObject*, PyObject* args)
{
    App* a = app_or_error(); if (!a) return nullptr;

    const char* path = nullptr;
    if (!PyArg_ParseTuple(args, "s:set_sprite", &path)) return nullptr;

    // A bare filename is resolved against assets/; anything with a separator
    // is taken as given.
    std::string resolved = path;
    if (resolved.find('/') == std::string::npos &&
        resolved.find('\\') == std::string::npos) {
        resolved = AppPaths::asset(resolved);
    }

    std::string err;
    if (!a->window.setSprite(resolved, err)) {
        PyErr_SetString(PyExc_OSError, err.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* c_position(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    int x, y; a->window.getPosition(x, y);
    return Py_BuildValue("(ii)", x, y);
}

PyObject* c_set_position(PyObject*, PyObject* args)
{
    App* a = app_or_error(); if (!a) return nullptr;
    int x, y;
    if (!PyArg_ParseTuple(args, "ii:set_position", &x, &y)) return nullptr;
    a->window.setPosition(x, y);
    Py_RETURN_NONE;
}

PyObject* c_size(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    int w, h; a->window.getSize(w, h);
    return Py_BuildValue("(ii)", w, h);
}

PyObject* c_set_size(PyObject*, PyObject* args)
{
    App* a = app_or_error(); if (!a) return nullptr;
    int w, h;
    if (!PyArg_ParseTuple(args, "ii:set_size", &w, &h)) return nullptr;
    if (w < 32 || h < 32 || w > 4096 || h > 4096) {
        PyErr_Format(PyExc_ValueError,
                     "set_size(%d, %d): each edge must be between 32 and 4096",
                     w, h);
        return nullptr;
    }
    std::string err;
    if (!a->window.setSize(w, h, err)) {
        PyErr_SetString(PyExc_RuntimeError, err.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

// The work area of the display the pet is currently on: the screen minus the
// panels and docks the window manager reserves.
//
// scripts/clippy.py used to assume 1920x1080 because the host had no way to
// answer this. Guessing a screen size is the kind of plausible-looking default
// that puts the pet off the edge of somebody else's monitor.
PyObject* c_display_bounds(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;

    const SDL_DisplayID id = a->window.handle()
        ? SDL_GetDisplayForWindow(a->window.handle())
        : SDL_GetPrimaryDisplay();
    if (!id) {
        PyErr_SetString(PyExc_RuntimeError, SDL_GetError());
        return nullptr;
    }

    SDL_Rect r;
    if (!SDL_GetDisplayUsableBounds(id, &r)) {
        PyErr_SetString(PyExc_RuntimeError, SDL_GetError());
        return nullptr;
    }
    return Py_BuildValue("(iiii)", r.x, r.y, r.w, r.h);
}

PyObject* c_capabilities(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    const Capabilities& c = a->window.caps();

    PyObject* notes = PyList_New(0);
    if (!notes) return nullptr;
    for (const auto& n : c.notes) {
        PyObject* s = PyUnicode_FromString(n.c_str());
        if (!s || PyList_Append(notes, s) < 0) {
            Py_XDECREF(s); Py_DECREF(notes); return nullptr;
        }
        Py_DECREF(s);
    }

    PyObject* d = Py_BuildValue(
        "{s:s, s:s, s:O, s:O, s:O, s:O, s:O, s:N}",
        "platform",      c.platform.c_str(),
        "video_driver",  c.video_driver.c_str(),
        "borderless",    c.borderless    ? Py_True : Py_False,
        "always_on_top", c.always_on_top ? Py_True : Py_False,
        "transparent",   c.transparent   ? Py_True : Py_False,
        "skip_taskbar",  c.skip_taskbar  ? Py_True : Py_False,
        "tray",          c.tray          ? Py_True : Py_False,
        "notes",         notes);
    if (!d) Py_DECREF(notes);
    return d;
}

PyObject* c_on(PyObject*, PyObject* args)
{
    const char* event = nullptr;
    PyObject* cb = nullptr;
    if (!PyArg_ParseTuple(args, "sO:on", &event, &cb)) return nullptr;

    if (!PyCallable_Check(cb)) {
        PyErr_SetString(PyExc_TypeError, "clippy.on(): second argument must be callable");
        return nullptr;
    }

    static const char* kKnown[] = {"show", "hide", "quit", "frame", nullptr};
    bool known = false;
    for (int i = 0; kKnown[i]; ++i) {
        if (std::string(event) == kKnown[i]) { known = true; break; }
    }
    if (!known) {
        // Fail loudly: a typo'd event name would otherwise be a hook that
        // silently never fires.
        PyErr_Format(PyExc_ValueError,
                     "clippy.on(): unknown event '%s' (expected one of "
                     "'show', 'hide', 'quit', 'frame')", event);
        return nullptr;
    }

    auto it = g_hooks.find(event);
    if (it != g_hooks.end()) Py_DECREF(it->second);

    Py_INCREF(cb);
    g_hooks[event] = cb;
    Py_RETURN_NONE;
}

PyObject* c_log(PyObject*, PyObject* args)
{
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "s:log", &msg)) return nullptr;
    SDL_Log("[clippy] %s", msg);
    Py_RETURN_NONE;
}

PyMethodDef kMethods[] = {
    {"show",         c_show,         METH_NOARGS,  "Show the pet window."},
    {"hide",         c_hide,         METH_NOARGS,  "Hide the pet window (stays running in the tray)."},
    {"toggle",       c_toggle,       METH_NOARGS,  "Toggle window visibility."},
    {"visible",      c_visible,      METH_NOARGS,  "True if the window is currently mapped."},
    {"quit",         c_quit,         METH_NOARGS,  "Shut the application down."},
    {"set_sprite",   c_set_sprite,   METH_VARARGS, "set_sprite(path) -> load a PNG. Bare names resolve against assets/."},
    {"position",     c_position,     METH_NOARGS,  "Window position as (x, y)."},
    {"set_position", c_set_position, METH_VARARGS, "set_position(x, y)"},
    {"size",         c_size,         METH_NOARGS,  "Window size as (w, h)."},
    {"set_size",     c_set_size,     METH_VARARGS, "set_size(w, h) -> resize the window; the stage follows."},
    {"display_bounds", c_display_bounds, METH_NOARGS,
     "Usable bounds of the display the pet is on, as (x, y, w, h)."},
    {"capabilities", c_capabilities, METH_NOARGS,  "What the platform actually granted, as a dict."},
    {"on",           c_on,           METH_VARARGS,
     "on(event, fn) -> register a hook: 'show', 'hide', 'quit', 'frame'.\n"
     "'frame' is called with the seconds since the last frame; the others "
     "take no arguments."},
    {"log",          c_log,          METH_VARARGS, "log(msg) -> write to the SDL log."},
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef kModule = {
    PyModuleDef_HEAD_INIT,
    "clippy",
    "Host interface for the gobboclippy desktop pet.",
    -1,
    kMethods,
    nullptr, nullptr, nullptr, nullptr
};

PyObject* moduleInit()
{
    PyObject* m = PyModule_Create(&kModule);
    if (!m) return nullptr;
    PyModule_AddStringConstant(m, "__version__", GC_VERSION);

    // The harvested drawing layer registers alongside the free functions
    // rather than replacing them: clippy.show() keeps its shape.
    std::string err;
    if (!PyDraw::addToModule(m, err)) {
        // An exception may already be set from deeper in; if not, say what
        // went wrong here rather than returning a half-built module.
        if (!PyErr_Occurred()) PyErr_SetString(PyExc_RuntimeError, err.c_str());
        Py_DECREF(m);
        return nullptr;
    }
    return m;
}

} // namespace

namespace PyClippy {

bool registerModule(std::string& error_out)
{
    if (PyImport_AppendInittab("clippy", &moduleInit) == -1) {
        error_out = "PyImport_AppendInittab('clippy') failed";
        return false;
    }
    return true;
}

void bind(App* app) { g_app = app; }

bool fire(const char* event)
{
    auto it = g_hooks.find(event);
    if (it == g_hooks.end()) return true;   // no hook registered is not an error

    PyObject* result = PyObject_CallNoArgs(it->second);
    if (!result) {
        // Print rather than swallow: a broken hook must be visible.
        PyErr_Print();
        return false;
    }
    Py_DECREF(result);
    return true;
}

bool fireFrame(float dt)
{
    auto it = g_hooks.find("frame");
    if (it == g_hooks.end()) return true;

    PyObject* result = PyObject_CallFunction(it->second, "f", (double)dt);
    if (!result) {
        PyErr_Print();
        return false;
    }
    Py_DECREF(result);
    return true;
}

} // namespace PyClippy
