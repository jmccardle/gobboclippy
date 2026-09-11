#include "PyClippy.h"

#include <Python.h>

#include <map>
#include <string>
#include <vector>

#include "App.h"
#include "AppPaths.h"
#include "Drawable.h"
#include "Mic.h"
#include "PyDraw.h"

namespace {

App* g_app = nullptr;
bool g_interpreter = false;
std::map<std::string, PyObject*> g_hooks;

// Every entry point goes through this so a script that somehow runs before
// bind() gets a clear error instead of dereferencing null.
App* app_or_error()
{
    if (!g_app) {
        PyErr_SetString(PyExc_RuntimeError, PyClippy::noHostMessage("clippy"));
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

    // A relative name is resolved against assets/, subfolders included; an
    // absolute path is taken as given.
    const std::string resolved = AppPaths::resolveAsset(path);

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

    PyObject* formats = PyList_New(0);
    if (!formats) { Py_DECREF(notes); return nullptr; }
    for (const auto& f : c.image_formats) {
        PyObject* s = PyUnicode_FromString(f.c_str());
        if (!s || PyList_Append(formats, s) < 0) {
            Py_XDECREF(s); Py_DECREF(formats); Py_DECREF(notes); return nullptr;
        }
        Py_DECREF(s);
    }

    PyObject* d = Py_BuildValue(
        "{s:s, s:s, s:O, s:O, s:O, s:O, s:O, s:O, s:N, s:N}",
        "platform",      c.platform.c_str(),
        "video_driver",  c.video_driver.c_str(),
        "borderless",    c.borderless    ? Py_True : Py_False,
        "always_on_top", c.always_on_top ? Py_True : Py_False,
        "transparent",   c.transparent   ? Py_True : Py_False,
        "skip_taskbar",  c.skip_taskbar  ? Py_True : Py_False,
        "tray",          c.tray          ? Py_True : Py_False,
        "microphone",    c.microphone    ? Py_True : Py_False,
        "image_formats", formats,
        "notes",         notes);
    if (!d) { Py_DECREF(formats); Py_DECREF(notes); }
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

    static const char* kKnown[] = {"show", "hide", "quit", "frame",
                                   "click", "double_click", "mic", nullptr};
    bool known = false;
    for (int i = 0; kKnown[i]; ++i) {
        if (std::string(event) == kKnown[i]) { known = true; break; }
    }
    if (!known) {
        // Fail loudly: a typo'd event name would otherwise be a hook that
        // silently never fires.
        PyErr_Format(PyExc_ValueError,
                     "clippy.on(): unknown event '%s' (expected one of "
                     "'show', 'hide', 'quit', 'frame', 'click', "
                     "'double_click', 'mic')", event);
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

// Where a script keeps its own state: config, caches, whatever it writes. SDL
// answers this per platform -- ~/.local/share/gobboclippy on Linux, %APPDATA%
// on Windows, ~/Library/Application Support on macOS -- and creates the
// directory. A script that needs a settings file asks for this rather than
// building a path out of $HOME, which is what keeps the tree forkable.
PyObject* c_pref_path(PyObject*, PyObject*)
{
    char* p = SDL_GetPrefPath(nullptr, "gobboclippy");
    if (!p) {
        PyErr_SetString(PyExc_OSError, SDL_GetError());
        return nullptr;
    }
    PyObject* s = PyUnicode_FromString(p);
    SDL_free(p);
    return s;
}

// --- clippy.mic -------------------------------------------------------------
//
// The host owns the device; the script owns where the audio goes. Everything
// here is a few lines over src/Mic.h, except start(), which is where the one
// rule this feature exists to keep is enforced.

// An empty list answers both "nothing is plugged in" and "this machine has no
// audio stack at all", because to a caller asking what it can record from those
// are the same answer. The difference is not lost: it is in the note that
// --capabilities prints, and mic.start() raises with the reason. A query
// answers; the action is what fails.
PyObject* c_mic_devices(PyObject*, PyObject*)
{
    std::string err;
    const std::vector<Mic::Device> found = Mic::devices(err);

    PyObject* list = PyList_New(0);
    if (!list) return nullptr;
    for (const auto& d : found) {
        PyObject* item = Py_BuildValue("(Is)", (unsigned int)d.id, d.name.c_str());
        if (!item || PyList_Append(list, item) < 0) {
            Py_XDECREF(item); Py_DECREF(list); return nullptr;
        }
        Py_DECREF(item);
    }
    return list;
}

PyObject* c_mic_start(PyObject*, PyObject* args)
{
    App* a = app_or_error(); if (!a) return nullptr;

    PyObject* dev = Py_None;
    if (!PyArg_ParseTuple(args, "|O:start", &dev)) return nullptr;

    SDL_AudioDeviceID id = 0;
    if (dev != Py_None) {
        const unsigned long v = PyLong_AsUnsignedLong(dev);
        if (v == (unsigned long)-1 && PyErr_Occurred()) return nullptr;
        id = (SDL_AudioDeviceID)v;
    }

    // The rule the whole feature rests on: the visible pet *is* the recording
    // indicator, so there is no recording while it is hidden. It lives here
    // rather than in the script because a user relies on it to know when the
    // microphone is live, and a script is not the right place to keep a promise
    // made to somebody else.
    if (!a->window.visible()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "clippy.mic.start(): the window is hidden, and a hidden "
                        "pet cannot show that it is recording");
        return nullptr;
    }

    std::string err;
    if (!Mic::start(id, err)) {
        PyErr_SetString(PyExc_RuntimeError, err.c_str());
        return nullptr;
    }
    PyClippy::fireMic(true);
    Py_RETURN_NONE;
}

PyObject* c_mic_stop(PyObject*, PyObject*)
{
    if (!Mic::active()) Py_RETURN_NONE;   // idempotent; nothing changed, no hook
    Mic::stop();
    PyClippy::fireMic(false);
    Py_RETURN_NONE;
}

PyObject* c_mic_active(PyObject*, PyObject*)
{
    return PyBool_FromLong(Mic::active() ? 1 : 0);
}

PyObject* c_mic_queued(PyObject*, PyObject*)
{
    return PyLong_FromLong(Mic::queued());
}

PyObject* c_mic_spec(PyObject*, PyObject*)
{
    return Py_BuildValue("(iis)", Mic::kRate, Mic::kChannels, Mic::kFormat);
}

PyObject* c_mic_read(PyObject*, PyObject* args)
{
    int max_bytes = 0;
    if (!PyArg_ParseTuple(args, "|i:read", &max_bytes)) return nullptr;
    if (max_bytes < 0) {
        PyErr_SetString(PyExc_ValueError, "mic.read(): max_bytes cannot be negative");
        return nullptr;
    }

    int want = Mic::queued();
    if (max_bytes > 0 && max_bytes < want) want = max_bytes;
    if (want <= 0) return PyBytes_FromStringAndSize(nullptr, 0);

    PyObject* b = PyBytes_FromStringAndSize(nullptr, want);
    if (!b) return nullptr;

    // Nothing else can see this buffer yet, so filling it without the GIL is
    // safe -- and worth it, because a reader thread should not be holding the
    // GIL while it waits on the mutex the event loop takes to stop the device.
    int got = 0;
    char* dst = PyBytes_AS_STRING(b);
    Py_BEGIN_ALLOW_THREADS
    got = Mic::read(dst, want);
    Py_END_ALLOW_THREADS

    if (got < 0) {
        Py_DECREF(b);
        PyErr_SetString(PyExc_RuntimeError, SDL_GetError());
        return nullptr;
    }
    if (got != want && _PyBytes_Resize(&b, got) < 0) return nullptr;
    return b;
}

PyMethodDef kMicMethods[] = {
    {"devices", c_mic_devices, METH_NOARGS,
     "devices() -> [(id, name), ...]  Recording devices SDL can see.\n"
     "Empty both when nothing is plugged in and when there is no audio stack;\n"
     "--capabilities prints which, and start() raises with the reason."},
    {"start",   c_mic_start,   METH_VARARGS,
     "start(device=None) -> begin recording; device is an id from devices(),\n"
     "or None for the system default.\n"
     "Raises if the window is hidden: a hidden pet cannot show that it is\n"
     "recording, and this is the one rule that is not the script's to bend."},
    {"stop",    c_mic_stop,    METH_NOARGS,
     "stop() -> close the device. Hiding the window does this too."},
    {"active",  c_mic_active,  METH_NOARGS,  "True while the device is open."},
    {"read",    c_mic_read,    METH_VARARGS,
     "read(max_bytes=0) -> bytes  Everything queued, or at most max_bytes.\n"
     "Safe to call from a thread; returns b'' when nothing is waiting."},
    {"queued",  c_mic_queued,  METH_NOARGS,  "Bytes waiting to be read."},
    {"spec",    c_mic_spec,    METH_NOARGS,
     "spec() -> (rate, channels, format)  Always (16000, 1, 's16le')."},
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef kMicModule = {
    PyModuleDef_HEAD_INIT,
    "clippy.mic",
    "The microphone. The host owns the device, the script owns the destination.",
    -1,
    kMicMethods,
    nullptr, nullptr, nullptr, nullptr
};

PyMethodDef kMethods[] = {
    {"show",         c_show,         METH_NOARGS,  "Show the pet window."},
    {"hide",         c_hide,         METH_NOARGS,  "Hide the pet window (stays running in the tray)."},
    {"toggle",       c_toggle,       METH_NOARGS,  "Toggle window visibility."},
    {"visible",      c_visible,      METH_NOARGS,  "True if the window is currently mapped."},
    {"quit",         c_quit,         METH_NOARGS,  "Shut the application down."},
    {"set_sprite",   c_set_sprite,   METH_VARARGS, "set_sprite(path) -> load a PNG. Relative paths resolve under assets/."},
    {"position",     c_position,     METH_NOARGS,  "Window position as (x, y)."},
    {"set_position", c_set_position, METH_VARARGS, "set_position(x, y)"},
    {"size",         c_size,         METH_NOARGS,  "Window size as (w, h)."},
    {"set_size",     c_set_size,     METH_VARARGS, "set_size(w, h) -> resize the window; the stage follows."},
    {"display_bounds", c_display_bounds, METH_NOARGS,
     "Usable bounds of the display the pet is on, as (x, y, w, h)."},
    {"capabilities", c_capabilities, METH_NOARGS,  "What the platform actually granted, as a dict."},
    {"on",           c_on,           METH_VARARGS,
     "on(event, fn) -> register a hook.\n"
     "  'show', 'hide', 'quit'  no arguments\n"
     "  'frame'                 the seconds since the last frame\n"
     "  'click'                 (x, y, button, clicks)\n"
     "  'double_click'          (x, y, button)\n"
     "  'mic'                   True when recording started, False when it "
     "stopped\n"
     "A double-click also fires 'click' twice, with clicks 1 then 2, as every "
     "toolkit does it: use one hook or the other, not both."},
    {"log",          c_log,          METH_VARARGS, "log(msg) -> write to the SDL log."},
    {"pref_path",    c_pref_path,    METH_NOARGS,
     "pref_path() -> the per-user directory for this application's own files,\n"
     "created if absent. Where a script keeps its config."},
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

    // clippy.mic is an attribute rather than a package: `import clippy` gets
    // it, and there is no second module for the import machinery to find and
    // disagree about.
    PyObject* mic = PyModule_Create(&kMicModule);
    if (!mic || PyModule_AddObject(m, "mic", mic) < 0) {
        Py_XDECREF(mic);
        Py_DECREF(m);
        return nullptr;
    }

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

void setInterpreterMode() { g_interpreter = true; }
bool interpreterMode()    { return g_interpreter; }

const char* noHostMessage(const char* what)
{
    // Held in a static so the pointer outlives this call: PyErr_SetString
    // copies, but callers pass this straight through and one of them may not.
    static std::string message;

    if (g_interpreter) {
        message = std::string(what) +
            ": there is no window in --python mode, so nothing here can draw.\n"
            "Run the script with --script instead:\n"
            "    gobboclippy --script <script.py>\n"
            "--python is the bundled interpreter -- pip, -c, -m, a client "
            "script -- and clippy.pref_path() is the part of this module that "
            "works there.";
    } else {
        message = std::string(what) + ": host not bound (called too early?)";
    }
    return message.c_str();
}

namespace {

// The hook registered for an event, or nullptr if there is none. A hook that
// was never registered is not an error, so every fire* below is a no-op then.
PyObject* hook(const char* event)
{
    auto it = g_hooks.find(event);
    return it == g_hooks.end() ? nullptr : it->second;
}

// The tail every fire* shares: a null result is a raised exception, and it is
// printed rather than swallowed, because a broken hook must be visible.
bool finish(PyObject* result)
{
    if (!result) { PyErr_Print(); return false; }
    Py_DECREF(result);
    return true;
}

} // namespace

// Each of these takes the GIL for itself, and takes it BEFORE looking the hook
// up. The event loop gives it up for the whole iteration, and hooks are reached
// from three places on that thread -- the loop, an SDL event and a tray
// callback -- so putting PyGILState_Ensure here means none of the three has to
// remember. The lookup is inside it because clippy.on() may be called from a
// Python thread: the GIL is what keeps g_hooks from being rewritten mid-read,
// and what stops on() dropping the last reference to a callable one of these is
// about to invoke.

bool fire(const char* event)
{
    PyGILState_STATE g = PyGILState_Ensure();
    PyObject* fn = hook(event);
    const bool ok = fn ? finish(PyObject_CallNoArgs(fn)) : true;
    PyGILState_Release(g);
    return ok;
}

bool fireFrame(float dt)
{
    PyGILState_STATE g = PyGILState_Ensure();
    PyObject* fn = hook("frame");
    const bool ok = fn
        ? finish(PyObject_CallFunction(fn, "f", (double)dt))
        : true;
    PyGILState_Release(g);
    return ok;
}

bool fireClick(const char* event, float x, float y, int button, int clicks)
{
    PyGILState_STATE g = PyGILState_Ensure();
    PyObject* fn = hook(event);
    // 'double_click' says the count in its name, so it does not repeat it in
    // its arguments; the caller passes clicks < 0 to ask for that shape.
    const bool ok = !fn ? true
        : clicks < 0
            ? finish(PyObject_CallFunction(fn, "ffi",  (double)x, (double)y, button))
            : finish(PyObject_CallFunction(fn, "ffii", (double)x, (double)y,
                                           button, clicks));
    PyGILState_Release(g);
    return ok;
}

bool fireMic(bool active)
{
    PyGILState_STATE g = PyGILState_Ensure();
    PyObject* fn = hook("mic");
    const bool ok = fn
        ? finish(PyObject_CallFunction(fn, "O", active ? Py_True : Py_False))
        : true;
    PyGILState_Release(g);
    return ok;
}

} // namespace PyClippy
