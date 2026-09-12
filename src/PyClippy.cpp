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
#include "Settings.h"

#include <memory>

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
    a->setVisible(!a->visible());
    Py_RETURN_NONE;
}

PyObject* c_visible(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    // a->visible(), not window.visible(): during a settings preview the
    // window is on screen and the pet is still hidden. See App.h.
    return PyBool_FromLong(a->visible() ? 1 : 0);
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
                                   "click", "double_click", "mic",
                                   "configure", nullptr};
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
                     "'double_click', 'mic', 'configure')", event);
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

// --- clippy.settings --------------------------------------------------------
//
// The host renders a schema it does not understand. It knows what an int field
// is and what OK/Cancel/Apply mean; it does not know what `window.x` is, which
// file the values came from, or what any of them do. That is all in
// scripts/gobbo/settings.py, and adding a setting is a dict there rather than a
// change here.
//
// Everything below is the translation between a Python dict and Settings::Spec,
// plus the GIL. The settings window is drawn from the event loop, which runs
// without the GIL, so each callback takes it for itself -- the same arrangement
// the fire* hooks use, and for the same reason.

// The four callables the spec may carry, kept alive for exactly as long as the
// window is. Held by shared_ptr so the std::functions in the Spec can share
// them and the last one to go releases the references.
struct SettingsCallbacks {
    PyObject* on_change = nullptr;
    PyObject* on_apply  = nullptr;
    PyObject* on_cancel = nullptr;
    PyObject* on_close  = nullptr;

    ~SettingsCallbacks()
    {
        // Reached from the event loop, which does not hold the GIL, and a
        // DECREF without it is a data race on the refcount.
        PyGILState_STATE gil = PyGILState_Ensure();
        Py_XDECREF(on_change);
        Py_XDECREF(on_apply);
        Py_XDECREF(on_cancel);
        Py_XDECREF(on_close);
        PyGILState_Release(gil);
    }
};

// A callable under `name`, borrowed from the spec dict and checked. Returns
// false only on a genuine error; an absent key is a successful nothing.
bool settingsCallable(PyObject* spec, const char* name, PyObject** out)
{
    PyObject* fn = PyDict_GetItemString(spec, name);   // borrowed
    if (!fn || fn == Py_None) return true;
    if (!PyCallable_Check(fn)) {
        PyErr_Format(PyExc_TypeError,
                     "clippy.settings_open(): %s must be callable", name);
        return false;
    }
    Py_INCREF(fn);
    *out = fn;
    return true;
}

// A string under `key`, or `fallback` when absent. Missing is not an error --
// every one of these is decoration on a field that already has a key.
std::string settingsStr(PyObject* d, const char* key, const char* fallback = "")
{
    PyObject* v = PyDict_GetItemString(d, key);        // borrowed
    if (!v || v == Py_None) return fallback;
    const char* utf8 = PyUnicode_AsUTF8(v);
    if (!utf8) { PyErr_Clear(); return fallback; }
    return utf8;
}

bool settingsBool(PyObject* d, const char* key, bool fallback = false)
{
    PyObject* v = PyDict_GetItemString(d, key);        // borrowed
    if (!v || v == Py_None) return fallback;
    return PyObject_IsTrue(v) == 1;
}

// Turn one field dict into a Field. The type decides which of `value`'s
// spellings is read, and an unknown type is refused rather than guessed at: a
// field silently rendered as text would be a setting that writes back the wrong
// shape without ever saying so.
bool settingsField(PyObject* d, Settings::Field& f)
{
    if (!PyDict_Check(d)) {
        PyErr_SetString(PyExc_TypeError,
                        "clippy.settings_open(): every field must be a dict");
        return false;
    }

    PyObject* key = PyDict_GetItemString(d, "key");    // borrowed
    if (!key || !PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_ValueError,
                        "clippy.settings_open(): a field needs a string 'key'");
        return false;
    }
    f.key   = PyUnicode_AsUTF8(key);
    f.label = settingsStr(d, "label", f.key.c_str());
    f.tab   = settingsStr(d, "tab");
    f.help  = settingsStr(d, "help");
    f.live  = settingsBool(d, "live");

    const std::string type = settingsStr(d, "type", "text");
    PyObject* value = PyDict_GetItemString(d, "value");    // borrowed, may be null

    if (type == "int" || type == "range") {
        f.kind = type == "range" ? Settings::Field::Kind::Range
                                 : Settings::Field::Kind::Int;
        if (value && value != Py_None) {
            f.int_value = PyLong_AsLongLong(value);
            if (PyErr_Occurred()) return false;
        }
        PyObject* lo = PyDict_GetItemString(d, "min");
        PyObject* hi = PyDict_GetItemString(d, "max");
        const bool bounded = lo && hi && lo != Py_None && hi != Py_None;
        if (bounded) {
            f.int_min = PyLong_AsLongLong(lo);
            f.int_max = PyLong_AsLongLong(hi);
            if (PyErr_Occurred()) return false;
        }
        // A slider needs two ends. Without them there is nothing to draw, and
        // an unbounded one would silently become a plain box -- a widget the
        // schema did not ask for.
        if (f.kind == Settings::Field::Kind::Range &&
            (!bounded || f.int_min >= f.int_max)) {
            PyErr_Format(PyExc_ValueError,
                         "clippy.settings_open(): field '%s' is a range and "
                         "needs 'min' and 'max', with min < max",
                         f.key.c_str());
            return false;
        }
    } else if (type == "bool") {
        f.kind = Settings::Field::Kind::Bool;
        f.bool_value = value && PyObject_IsTrue(value) == 1;
    } else if (type == "choice") {
        f.kind = Settings::Field::Kind::Choice;
        PyObject* choices = PyDict_GetItemString(d, "choices");   // borrowed
        if (!choices || !PySequence_Check(choices)) {
            PyErr_Format(PyExc_ValueError,
                         "clippy.settings_open(): field '%s' is a choice and "
                         "needs a 'choices' sequence", f.key.c_str());
            return false;
        }
        const Py_ssize_t n = PySequence_Size(choices);
        for (Py_ssize_t i = 0; i < n; ++i) {
            PyObject* item = PySequence_GetItem(choices, i);       // new
            if (!item) return false;
            const char* utf8 = PyUnicode_AsUTF8(item);
            if (!utf8) { Py_DECREF(item); return false; }
            f.choices.emplace_back(utf8);
            Py_DECREF(item);
        }
        // The current value is matched by string, not by index, so a config
        // file keeps meaning the same thing when the list is reordered. A value
        // that is not in the list selects nothing rather than item zero.
        f.choice_index = -1;
        if (value && value != Py_None) {
            const char* utf8 = PyUnicode_AsUTF8(value);
            if (!utf8) { PyErr_Clear(); }
            else for (size_t i = 0; i < f.choices.size(); ++i) {
                if (f.choices[i] == utf8) { f.choice_index = (int)i; break; }
            }
        }
    } else if (type == "text") {
        f.kind = Settings::Field::Kind::Text;
        if (value && value != Py_None) {
            const char* utf8 = PyUnicode_AsUTF8(value);
            if (!utf8) return false;
            f.text_value = utf8;
        }
    } else {
        PyErr_Format(PyExc_ValueError,
                     "clippy.settings_open(): field '%s' has unknown type '%s' "
                     "(expected 'int', 'range', 'text', 'bool' or 'choice')",
                     f.key.c_str(), type.c_str());
        return false;
    }
    return true;
}

// A field's value as Python sees it -- an int stays an int, a choice is the
// chosen string. New reference.
PyObject* settingsValue(const Settings::Field& f)
{
    switch (f.kind) {
    case Settings::Field::Kind::Int:
    case Settings::Field::Kind::Range: return PyLong_FromLongLong(f.int_value);
    case Settings::Field::Kind::Bool: return PyBool_FromLong(f.bool_value ? 1 : 0);
    case Settings::Field::Kind::Choice:
        if (f.choice_index >= 0 && f.choice_index < (int)f.choices.size())
            return PyUnicode_FromString(f.choices[(size_t)f.choice_index].c_str());
        Py_RETURN_NONE;
    case Settings::Field::Kind::Text:
    default: return PyUnicode_FromString(f.text_value.c_str());
    }
}

PyObject* c_settings_open(PyObject*, PyObject* args)
{
    App* a = app_or_error(); if (!a) return nullptr;

    PyObject* spec_dict = nullptr;
    if (!PyArg_ParseTuple(args, "O:settings_open", &spec_dict)) return nullptr;
    if (!PyDict_Check(spec_dict)) {
        PyErr_SetString(PyExc_TypeError,
                        "clippy.settings_open(): expected a dict");
        return nullptr;
    }

    auto cb = std::make_shared<SettingsCallbacks>();
    if (!settingsCallable(spec_dict, "on_change", &cb->on_change) ||
        !settingsCallable(spec_dict, "on_apply",  &cb->on_apply)  ||
        !settingsCallable(spec_dict, "on_cancel", &cb->on_cancel) ||
        !settingsCallable(spec_dict, "on_close",  &cb->on_close))
        return nullptr;

    Settings::Spec spec;
    spec.title = settingsStr(spec_dict, "title", "gobboclippy settings");

    PyObject* fields = PyDict_GetItemString(spec_dict, "fields");   // borrowed
    if (!fields || !PySequence_Check(fields)) {
        PyErr_SetString(PyExc_ValueError,
                        "clippy.settings_open(): expected a 'fields' sequence");
        return nullptr;
    }
    const Py_ssize_t n = PySequence_Size(fields);
    for (Py_ssize_t i = 0; i < n; ++i) {
        PyObject* item = PySequence_GetItem(fields, i);             // new
        if (!item) return nullptr;
        Settings::Field f;
        const bool ok = settingsField(item, f);
        Py_DECREF(item);
        if (!ok) return nullptr;
        spec.fields.push_back(std::move(f));
    }

    spec.on_change = [cb](const Settings::Field& f) {
        if (!cb->on_change) return;
        PyGILState_STATE gil = PyGILState_Ensure();
        PyObject* v = settingsValue(f);
        if (!v) {
            PyErr_Print();
        } else {
            PyObject* r = PyObject_CallFunction(cb->on_change, "sO",
                                                f.key.c_str(), v);
            Py_DECREF(v);
            if (!r) PyErr_Print(); else Py_DECREF(r);
        }
        PyGILState_Release(gil);
    };

    // The one callback with an answer. An empty string means the config was
    // written; anything else is shown in the dialog and the dialog stays open.
    // A raising handler is the same answer with the exception for its text --
    // a failed write must never look like a successful one.
    spec.on_apply = [cb](const std::vector<Settings::Field>& edited) -> std::string {
        if (!cb->on_apply) return std::string();

        PyGILState_STATE gil = PyGILState_Ensure();
        std::string problem;

        PyObject* values = PyDict_New();
        if (values) {
            for (const Settings::Field& f : edited) {
                PyObject* v = settingsValue(f);
                if (!v || PyDict_SetItemString(values, f.key.c_str(), v) < 0) {
                    Py_XDECREF(v);
                    Py_CLEAR(values);
                    break;
                }
                Py_DECREF(v);
            }
        }

        if (!values) {
            problem = "could not build the settings payload";
            PyErr_Clear();
        } else {
            PyObject* r = PyObject_CallFunctionObjArgs(cb->on_apply, values, nullptr);
            Py_DECREF(values);
            if (!r) {
                PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
                PyErr_Fetch(&type, &value, &tb);
                PyErr_NormalizeException(&type, &value, &tb);
                PyObject* text = value ? PyObject_Str(value) : nullptr;
                const char* utf8 = text ? PyUnicode_AsUTF8(text) : nullptr;
                problem = utf8 && *utf8 ? utf8 : "the settings handler raised";
                Py_XDECREF(text);
                // Restored and printed as well as reported: the dialog gets one
                // line, and whoever is reading stderr gets the traceback.
                PyErr_Restore(type, value, tb);
                PyErr_Print();
            } else {
                if (PyUnicode_Check(r)) {
                    const char* utf8 = PyUnicode_AsUTF8(r);
                    if (utf8) problem = utf8; else PyErr_Clear();
                }
                Py_DECREF(r);
            }
        }

        PyGILState_Release(gil);
        return problem;
    };

    spec.on_cancel = [cb]() {
        if (!cb->on_cancel) return;
        PyGILState_STATE gil = PyGILState_Ensure();
        PyObject* r = PyObject_CallNoArgs(cb->on_cancel);
        if (!r) PyErr_Print(); else Py_DECREF(r);
        PyGILState_Release(gil);
    };

    // The preview ends with the window, whatever else on_close does, and it
    // ends here rather than in the script so that a handler that raises cannot
    // leave the pet on screen with no banner to explain it.
    App* app = a;
    spec.on_close = [cb, app]() {
        app->endPreview();
        if (!cb->on_close) return;
        PyGILState_STATE gil = PyGILState_Ensure();
        PyObject* r = PyObject_CallNoArgs(cb->on_close);
        if (!r) PyErr_Print(); else Py_DECREF(r);
        PyGILState_Release(gil);
    };

    std::string err;
    if (!Settings::show(spec, err)) {
        PyErr_SetString(PyExc_RuntimeError, err.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

// Write a value back into the open dialog, without it counting as an edit the
// user made. See Settings.h: this is what a locked aspect ratio needs, and the
// reason it does not re-enter on_change is that a width adjusting a height
// adjusting a width would not terminate.
PyObject* c_settings_set(PyObject*, PyObject* args)
{
    const char* key = nullptr;
    PyObject* value = nullptr;
    if (!PyArg_ParseTuple(args, "sO:settings_set", &key, &value)) return nullptr;

    const Settings::Field* f = Settings::find(key);
    if (!f) {
        PyErr_Format(PyExc_KeyError,
                     "clippy.settings_set(): no field '%s' in the open settings "
                     "window (or no window is open)", key);
        return nullptr;
    }

    bool ok = false;
    switch (f->kind) {
    case Settings::Field::Kind::Int:
    case Settings::Field::Kind::Range: {
        const long long v = PyLong_AsLongLong(value);
        if (v == -1 && PyErr_Occurred()) return nullptr;
        ok = Settings::setInt(key, v);
        break;
    }
    case Settings::Field::Kind::Bool:
        ok = Settings::setBool(key, PyObject_IsTrue(value) == 1);
        break;
    case Settings::Field::Kind::Choice: {
        const char* utf8 = PyUnicode_AsUTF8(value);
        if (!utf8) return nullptr;
        if (!Settings::setChoice(key, utf8)) {
            // Distinguished from a missing field: the key is right and the
            // value is not one this dialog is offering, which is a bug in the
            // handler rather than in the schema.
            PyErr_Format(PyExc_ValueError,
                         "clippy.settings_set(): '%s' is not one of the choices "
                         "offered for '%s'", utf8, key);
            return nullptr;
        }
        ok = true;
        break;
    }
    case Settings::Field::Kind::Text:
    default: {
        const char* utf8 = PyUnicode_AsUTF8(value);
        if (!utf8) return nullptr;
        ok = Settings::setText(key, utf8);
        break;
    }
    }

    if (!ok) {
        PyErr_Format(PyExc_TypeError,
                     "clippy.settings_set(): wrong value type for field '%s'", key);
        return nullptr;
    }
    Py_RETURN_NONE;
}

// The inverse of settings_set: what a field currently holds, typed as Python
// sees it. A live handler that has to reason about a field the user is not
// touching -- the other half of a locked ratio, a bound that depends on another
// setting -- should ask the dialog rather than infer it from the world.
PyObject* c_settings_get(PyObject*, PyObject* args)
{
    const char* key = nullptr;
    if (!PyArg_ParseTuple(args, "s:settings_get", &key)) return nullptr;

    const Settings::Field* f = Settings::find(key);
    if (!f) {
        PyErr_Format(PyExc_KeyError,
                     "clippy.settings_get(): no field '%s' in the open settings "
                     "window (or no window is open)", key);
        return nullptr;
    }
    return settingsValue(*f);
}

PyObject* c_previewing(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;
    return PyBool_FromLong(a->previewing() ? 1 : 0);
}

PyObject* c_settings_close(PyObject*, PyObject*)
{
    Settings::close();
    Py_RETURN_NONE;
}

PyObject* c_settings_open_p(PyObject*, PyObject*)
{
    return PyBool_FromLong(Settings::open() ? 1 : 0);
}

PyObject* c_preview(PyObject*, PyObject*)
{
    App* a = app_or_error(); if (!a) return nullptr;

    // Refused rather than ignored outside a settings session: the banner is the
    // only thing that explains why the pet is on screen while hidden, and the
    // dialog closing is the only thing that ends it. Without one there would be
    // no way back.
    if (!Settings::open()) {
        PyErr_SetString(PyExc_RuntimeError,
                        "clippy.preview(): only meaningful while the settings "
                        "window is open, because closing it is what ends the "
                        "preview");
        return nullptr;
    }
    a->engagePreview();
    Py_RETURN_NONE;
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
    if (!a->visible()) {
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
    {"settings_open", c_settings_open, METH_VARARGS,
     "settings_open(spec) -> open the settings window on a field schema.\n"
     "spec is a dict: title, fields (a list of field dicts), and the\n"
     "callbacks on_change(key, value), on_apply(values) -> None | error\n"
     "string, on_cancel() and on_close(). A field dict is key, label, tab,\n"
     "help, type ('int', 'text', 'bool' or 'choice'), value, live, and\n"
     "min/max or choices for the types that take them."},
    {"previewing",   c_previewing,   METH_NOARGS,
     "True while the pet is on screen only to demonstrate a setting.\n"
     "visible() is False at the same time, and both are true answers: the\n"
     "window is mapped, and the user has not asked to see it. A script that\n"
     "draws anything meaning 'I am up' wants this one."},
    {"settings_set", c_settings_set, METH_VARARGS,
     "settings_set(key, value) -> move a field the user did not touch.\n"
     "Does not fire on_change -- the host was told this value, nobody\n"
     "edited it -- so a locked ratio is a linkage rather than a recursion.\n"
     "It still counts as a change to save."},
    {"settings_get", c_settings_get, METH_VARARGS,
     "settings_get(key) -> what that field currently holds, typed."},
    {"settings_close", c_settings_close, METH_NOARGS,
     "Close the settings window, as Cancel would but without on_cancel."},
    {"settings_is_open", c_settings_open_p, METH_NOARGS,
     "True while the settings window is up."},
    {"preview",      c_preview,      METH_NOARGS,
     "Put the pet on screen to demonstrate a setting, without it counting as\n"
     "shown: clippy.visible() stays False, the microphone stays refused, and\n"
     "the window goes when the settings dialog does. Only valid while that\n"
     "dialog is open."},
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

// Unlike every other hook, this one distinguishes "there is no handler" from
// "the handler ran", because those call for different answers: a script with no
// settings has nothing to open, and the tray should say so rather than appear
// broken. A handler that raises has already been reported by finish(), and
// counts as having run -- the script's bug is not the menu's problem.
bool fireConfigure()
{
    PyGILState_STATE g = PyGILState_Ensure();
    PyObject* fn = hook("configure");
    if (fn) finish(PyObject_CallNoArgs(fn));
    PyGILState_Release(g);
    return fn != nullptr;
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
