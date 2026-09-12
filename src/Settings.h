#pragma once
#include <functional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

// The settings window: a second SDL window, drawn with Dear ImGui, that renders
// a schema it does not understand.
//
// The host knows what an int field is and what OK/Cancel/Apply mean. It does
// not know what `window.x` is, which file the values come from, or what any of
// them do -- that is all Python's, in scripts/gobbo/settings.py. Adding a
// setting is a dict in that file, not a change here. The seam is Spec below:
// plain data in, plain data out.
//
// Everything in this class runs on the main thread, called from the event loop,
// and it never touches Python directly. The callbacks are std::function so that
// PyClippy owns every GIL acquisition in one place, as it does for the hooks.
namespace Settings {

// One editable setting. Only `kind` decides which widget is drawn.
struct Field {
    enum class Kind { Int, Text, Bool, Choice };

    std::string tab;          // tab to draw it under; "" is the first tab
    std::string key;          // opaque here; the identity Python knows it by
    std::string label;
    std::string help;         // shown as a tooltip when non-empty
    Kind        kind = Kind::Text;

    // A field marked live reports every edit as it happens, so the thing being
    // configured can demonstrate it. Geometry is the reason this exists: a
    // window position you cannot see is a number you are guessing at.
    bool        live = false;

    // Int
    long long   int_value = 0;
    long long   int_min   = 0;
    long long   int_max   = 0;   // min == max means unbounded

    // Text
    std::string text_value;

    // Bool
    bool        bool_value = false;

    // Choice: an index into `choices`. Values cross the boundary as the chosen
    // string, never as the index, so reordering the list cannot silently change
    // what a config file means.
    std::vector<std::string> choices;
    int                      choice_index = 0;
};

// What Python hands over when it asks for the window.
struct Spec {
    std::string        title = "Settings";
    std::vector<Field> fields;

    // A live field was edited. Handed the field itself, so the value arrives
    // with its type intact -- an int is an int here exactly as it is in the
    // apply payload, and a handler never has to parse a number back out of a
    // string the host only stringified to compare it.
    std::function<void(const Field&)> on_change;

    // OK or Apply, with every field's current value. Returns an empty string on
    // success, or a message to show in the window without closing it. A failed
    // write must not look like a successful one, so OK does not close on a
    // non-empty return.
    std::function<std::string(const std::vector<Field>&)> on_apply;

    // Cancel: put back whatever the live callbacks changed. The host has not
    // kept a copy -- it does not know what the values mean -- so the baseline
    // is Python's to hold.
    std::function<void()> on_cancel;

    // The window is gone, however it went. Always last, always exactly once.
    std::function<void()> on_close;
};

// Whether a window is currently up. Opening a second one is refused rather than
// stacked: two dialogs editing one file is a conflict with no good answer.
bool open();

// Open the window. Returns false and sets error_out if the window, renderer or
// ImGui context could not be created; nothing is left half-built on failure.
bool show(const Spec& spec, std::string& error_out);

// Close it and fire on_close. Safe to call when nothing is open.
void close();

// An SDL event, offered before the pet's own handling. Returns true if it
// belonged to the settings window and has been consumed -- the pet must not see
// a click that landed on a dialog.
bool handleEvent(const SDL_Event& e);

// Draw one frame. Cheap and safe when nothing is open.
void render();

// True while there are edits neither applied nor cancelled. The Apply button is
// disabled without them, and OK writes nothing.
bool dirty();

// --- the preview banner -----------------------------------------------------
//
// The host does not decide when this is shown -- App does, because it owns
// visibility. This is the window being told what to draw.
//
// It says that the pet is on screen only to demonstrate a geometry change, that
// it will go back to hidden when the dialog closes, and it offers the button
// that makes being on screen mean what it looks like it means.
void setPreviewBanner(bool shown);

// What the banner's Show button does. Set once by main().
void setOnPreviewShow(std::function<void()> fn);

} // namespace Settings
