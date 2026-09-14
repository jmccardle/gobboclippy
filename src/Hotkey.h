#pragma once
#include <string>
#include <vector>

#include <SDL3/SDL.h>

// A global hotkey: a chord that reaches the pet while the user is working in
// something else.
//
// This exists because the pet never holds keyboard focus. It is borderless,
// it is SDL_WINDOW_UTILITY, and the window manager is right not to give it
// focus -- so SDL's keyboard events describe only the seconds when the user has
// clicked on the pet, which are exactly the seconds they did not need a
// shortcut. Nothing here can be built on the window; each platform has to be
// asked separately, with its own API, for the privilege of seeing a key that
// was pressed somewhere else.
//
// The three answers do not match, and one of them is short:
//
//   X11      XGrabKey on the root window, read back through
//            SDL_SetX11EventHook.                        press and release
//   macOS    Carbon RegisterEventHotKey and one
//            application event handler.                  press and release
//   Windows  RegisterHotKey, read back through
//            SDL_SetWindowsMessageHook.                  press only
//
// RegisterHotKey has no release message at all -- WM_HOTKEY is the whole API.
// That is why deliversRelease() is a separate question from available(): a
// toggle works everywhere, and push-to-talk needs the answer to be yes.
// Reporting it is the alternative to inventing a key release from a timer,
// which would be a held key that let go on its own.
//
// Wayland is absent from that table on purpose. A Wayland client cannot grab a
// global key: the shortcuts-inhibit protocol needs keyboard focus, which is the
// thing we do not have, and that is compositor policy rather than an SDL gap.
// Same ceiling as always-on-top, same answer -- X11 and XWayland are the
// supported Linux path, and available() says no with a reason rather than
// binding something that will never fire.
namespace Hotkey {

// Our own modifier bits, deliberately not any platform's. Each backend
// translates them, and none of the three agrees with another about the values.
//
// Named with the project's k-prefix rather than the MOD_ALT / MOD_CONTROL shape
// the names want to take, because those exact spellings are preprocessor macros
// in <winuser.h> -- and a macro would textually eat `Hotkey::MOD_ALT` in the
// Win32 backend, where windows.h and this header necessarily meet.
enum Mod : unsigned {
    kCtrl  = 1u << 0,
    kAlt   = 1u << 1,
    kShift = 1u << 2,
    kSuper = 1u << 3,
};

struct Chord {
    unsigned    mods = 0;
    SDL_Keycode key  = SDLK_UNKNOWN;

    // Rebuilt from mods and key rather than copied from the input, so what
    // bound() answers and what the hook is handed is one spelling of the chord
    // and not the user's. "ctrl + alt+g" and "ALT+CTRL+G" are the same chord and
    // both come back as "Ctrl+Alt+G".
    std::string canonical;
};

// "Ctrl+Alt+G" -> Chord. Modifier names are case-insensitive and may be given
// in any order; the key is whatever SDL_GetKeyFromName() knows, so the
// vocabulary is SDL's documented one rather than a second list of our own to
// get out of step with it.
//
// Refuses a chord with no Ctrl, Alt or Super in it. A bare key grabbed globally
// is taken away from every other application on the desktop -- bind "G" and you
// stop being able to type g -- and Shift alone is the same trick with a capital
// letter. There is no way to offer that which is not a trap, so it is not
// offered.
bool parse(const std::string& text, Chord& out, std::string& error_out);

// The same chord, built from what SDL reported for a key press rather than from
// a string. This is what the settings window's "press the chord you want"
// capture uses, and it deliberately shares parse()'s rules and spelling: a
// chord typed into a config file and a chord pressed into the dialog have to be
// the same chord or the dialog is lying about what it saved.
bool fromKey(SDL_Keycode key, SDL_Keymod mods, Chord& out, std::string& error_out);

// Whether a keycode is a modifier rather than something that can be a chord's
// key. Capture needs this: pressing Ctrl+Alt+G is five events, and only one of
// them completes a chord.
bool isModifierKey(SDL_Keycode key);

// The window whose native handle the backends need. Called once, after the
// window exists; nothing here works before it. Also records which thread this
// is -- see onMainThread().
void attach(SDL_Window* window);

// Whether the caller is the thread attach() was called on, which is the thread
// SDL pumps events on.
//
// Everything here belongs to that thread, and unlike the microphone that is a
// rule rather than an implementation detail to be papered over with a mutex.
// Locking the queue would not be enough on its own -- the backends touch the
// window system too -- and a lock around those calls would invert against the
// one Xlib takes for itself: the main thread holds the display lock inside
// XNextEvent and then calls our hook, which wants the queue, while a second
// thread holding the queue would be waiting for the display. That is a
// deadlock, and it would be a rare one, which is the worst kind.
//
// So bind() refuses off-thread instead. Nothing is lost: binding a chord is
// setup, not work, and a script that wants to do it from a worker thread wants
// to hand the request to its frame hook.
bool onMainThread();

// Whether this build has a backend AND the running platform can actually grab.
// Answerable before anything is bound, so --capabilities can report it without
// taking a chord away from the desktop to find out.
bool available();
bool deliversRelease();

// Why available() is false, for the capability note. Empty when it is true.
const char* unavailableReason();

// Replaces any previous binding. Fails if the platform cannot grab, if the key
// cannot be mapped to this platform's key space, or if another application
// already owns the chord -- that last one being the failure worth the trouble,
// because the alternative is a hotkey that is silently never delivered.
bool bind(const Chord& c, std::string& error_out);
void unbind();

// The bound chord, or nullptr. One binding, matching clippy.on()'s one hook per
// event; rebinding is a second call to bind().
const Chord* bound();

struct Event { bool pressed; };

// Events the backend has seen since the last call, in order.
//
// The backends do not dispatch: they queue here and the event loop drains it.
// That is not indirection for its own sake -- the X11 hook is called from
// inside SDL_PollEvent, so a Python hook invoked from there could call back
// into SDL while SDL is part-way through pumping its own event queue. Every
// other path into Python in this program is entered from the loop body, after
// the pump has returned, and this one keeps that true.
std::vector<Event> drain();

// Release the grab and forget the window. Safe when nothing was ever bound.
void quit();

// --- what a platform backend implements -------------------------------------
//
// One of src/platform/Hotkey*.cpp is compiled per platform, chosen in
// CMakeLists.txt. They are not interchangeable at runtime and there is no
// dispatch table: the linker picks one.
namespace backend {

bool        available();
bool        deliversRelease();
const char* unavailableReason();

bool grab(SDL_Window* window, const Chord& c, std::string& error_out);
void ungrab();
void quit();

} // namespace backend

// Called by a backend from its platform hook, on the thread SDL pumps events
// on. Never called from anywhere else, which is why the queue needs no lock.
void post(bool pressed);

} // namespace Hotkey
