// Global hotkeys on X11.
//
// XGrabKey on the root window is the whole mechanism, and it is asked for on
// SDL's own display connection rather than a second one of ours. That is the
// decision the rest of this file follows from: the grab and the events arrive
// on the same connection SDL is already pumping, so SDL_SetX11EventHook sees
// them, there is no second thread, no select() loop and no XNextEvent of our
// own to starve or be starved by SDL's.
//
// Two details here are the difference between a hotkey that works and one that
// works until somebody's NumLock is on:
//
//   * A grab is per exact modifier mask, and the lock modifiers are part of the
//     mask. Grabbing Ctrl+Alt+G alone means Ctrl+Alt+G stops working the moment
//     CapsLock or NumLock is engaged, so every combination of the lock bits is
//     grabbed too.
//
//   * X11 reports a grab it cannot give you asynchronously, as a BadAccess on
//     the connection, which means the default handling is that XGrabKey appears
//     to succeed and the key is simply never delivered. A hotkey that silently
//     does nothing is the worst available outcome, so the grabs are bracketed
//     by an error handler and an XSync to turn that into a failure at bind
//     time, naming the chord that is already taken.
//
// Auto-repeat needs no handling: SDL enables detectable auto-repeat on this
// connection during its keyboard init (SDL_x11keyboard.c calls
// XkbSetDetectableAutoRepeat), so a held key is one KeyPress and one KeyRelease
// rather than a stream of pairs. The down-flag below does not depend on that
// being true -- it is what guarantees press and release alternate for whoever
// is consuming them.

#include "../Hotkey.h"

#include <SDL3/SDL_system.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

namespace {

// Every modifier bit X11 has, so a chord can be compared against the state of
// an event without the lock bits or the mouse-button bits joining in.
constexpr unsigned int kAllMods =
    ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask | Mod5Mask;

Display* g_display = nullptr;    // SDL's connection. Not ours to close.
Window   g_root    = 0;
bool     g_hooked  = false;
bool     g_down    = false;

unsigned int g_keycode = 0;      // X11 keycode of the grabbed key, 0 when none
unsigned int g_mask    = 0;      // the chord's modifiers, lock bits excluded
unsigned int g_ignore  = 0;      // lock bits grabbed in every combination

unsigned int g_grabbed[4] = {0, 0, 0, 0};
int          g_grabbed_n  = 0;

// --- catching BadAccess -----------------------------------------------------

int g_error = 0;
int (*g_prev_handler)(Display*, XErrorEvent*) = nullptr;

int catchError(Display*, XErrorEvent* e)
{
    g_error = e->error_code;
    return 0;
}

void beginCatching()
{
    g_error        = 0;
    g_prev_handler = XSetErrorHandler(catchError);
}

// XSync before restoring, or the error arrives after our handler is gone and
// goes to SDL's instead -- which is the silent case this exists to avoid.
int endCatching()
{
    XSync(g_display, False);
    XSetErrorHandler(g_prev_handler);
    g_prev_handler = nullptr;
    return g_error;
}

// --- modifiers --------------------------------------------------------------

// Which modN carries a given modifier key on this keyboard.
//
// Mod1 is Alt and Mod4 is Super on every mainstream Linux desktop, but that is
// a convention rather than a rule, and it is cheap to ask instead of assume:
// the X server will say where the key actually is.
unsigned int maskForKeysyms(const KeySym* syms, int n)
{
    XModifierKeymap* map = XGetModifierMapping(g_display);
    if (!map) return 0;

    unsigned int found = 0;
    for (int row = 0; row < 8 && !found; ++row) {
        for (int i = 0; i < map->max_keypermod && !found; ++i) {
            const KeyCode kc = map->modifiermap[row * map->max_keypermod + i];
            if (!kc) continue;
            for (int s = 0; s < n; ++s) {
                if (XKeysymToKeycode(g_display, syms[s]) == kc) {
                    found = 1u << row;
                    break;
                }
            }
        }
    }
    XFreeModifiermap(map);
    return found;
}

// --- keys -------------------------------------------------------------------

// The first 256 keysyms *are* Latin-1 by definition, so a printable SDL keycode
// is already its own keysym and needs no table. The named keys do, and the ones
// below are the ones worth putting in a global chord.
KeySym keysymFor(SDL_Keycode key)
{
    switch (key) {
    case SDLK_RETURN:    return XK_Return;
    case SDLK_ESCAPE:    return XK_Escape;
    case SDLK_BACKSPACE: return XK_BackSpace;
    case SDLK_TAB:       return XK_Tab;
    case SDLK_DELETE:    return XK_Delete;
    case SDLK_INSERT:    return XK_Insert;
    case SDLK_HOME:      return XK_Home;
    case SDLK_END:       return XK_End;
    case SDLK_PAGEUP:    return XK_Page_Up;
    case SDLK_PAGEDOWN:  return XK_Page_Down;
    case SDLK_LEFT:      return XK_Left;
    case SDLK_RIGHT:     return XK_Right;
    case SDLK_UP:        return XK_Up;
    case SDLK_DOWN:      return XK_Down;
    default: break;
    }
    if (key >= SDLK_F1  && key <= SDLK_F12) return XK_F1  + (key - SDLK_F1);
    if (key >= SDLK_F13 && key <= SDLK_F24) return XK_F13 + (key - SDLK_F13);
    if (key >= 0x20 && key <= 0xFF)         return (KeySym)key;
    return NoSymbol;
}

// --- the hook ---------------------------------------------------------------

bool SDLCALL eventHook(void*, XEvent* ev)
{
    if (!g_keycode) return true;
    if (ev->type != KeyPress && ev->type != KeyRelease) return true;

    const XKeyEvent& k = ev->xkey;
    if (k.keycode != g_keycode) return true;

    if (ev->type == KeyPress) {
        // A press has to carry the modifiers, or this is the same key being
        // pressed for some other reason and not our chord at all.
        if ((k.state & kAllMods & ~g_ignore) != g_mask) return true;
        if (g_down) return false;
        g_down = true;
        Hotkey::post(true);
        return false;
    }

    // A release is matched on the key alone, deliberately.
    //
    // The modifiers are usually already gone by the time the key comes up:
    // people let go of Ctrl and Alt first and the letter last, and so does
    // every tool that synthesises a chord. Checking the mask here would
    // therefore reject most real releases -- which for a toggle means the next
    // press is swallowed as a repeat, and for push-to-talk means a key that is
    // never let go of. The keycode and the fact that we saw the press are what
    // identifies it; the modifier state at that moment is not information.
    if (!g_down) return true;
    g_down = false;
    Hotkey::post(false);
    return false;
}

bool ensureDisplay(SDL_Window* window, std::string& error_out)
{
    if (g_display) return true;

    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    g_display = (Display*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    if (!g_display) {
        error_out = "the window has no X11 display connection";
        return false;
    }
    g_root = DefaultRootWindow(g_display);
    return true;
}

} // namespace

namespace Hotkey::backend {

bool available()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && SDL_strcmp(driver, "x11") == 0;
}

bool deliversRelease() { return true; }

const char* unavailableReason()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && SDL_strcmp(driver, "wayland") == 0) {
        return "Wayland has no protocol a client can use to grab a global key "
               "-- shortcuts-inhibit needs keyboard focus, which a desktop pet "
               "never has. Run under X11 or XWayland "
               "(SDL_VIDEODRIVER=x11) for hotkeys.";
    }
    return "global hotkeys need the x11 video driver";
}

bool grab(SDL_Window* window, const Chord& c, std::string& error_out)
{
    if (!ensureDisplay(window, error_out)) return false;

    const KeySym sym = keysymFor(c.key);
    if (sym == NoSymbol) {
        error_out = std::string("X11 has no keysym for '") +
                    SDL_GetKeyName(c.key) + "', so it cannot be grabbed";
        return false;
    }

    const KeyCode kc = XKeysymToKeycode(g_display, sym);
    if (!kc) {
        error_out = std::string("this keyboard layout has no key that produces "
                                "'") + SDL_GetKeyName(c.key) + "'";
        return false;
    }

    unsigned int mask = 0;
    if (c.mods & kShift) mask |= ShiftMask;
    if (c.mods & kCtrl)  mask |= ControlMask;
    if (c.mods & kAlt) {
        static const KeySym alt[] = {XK_Alt_L, XK_Alt_R, XK_Meta_L, XK_Meta_R};
        const unsigned int m = maskForKeysyms(alt, 4);
        if (!m) {
            error_out = "this keyboard's modifier map has no Alt key, so a "
                        "chord using Alt cannot be bound";
            return false;
        }
        mask |= m;
    }
    if (c.mods & kSuper) {
        static const KeySym super[] = {XK_Super_L, XK_Super_R};
        const unsigned int m = maskForKeysyms(super, 2);
        if (!m) {
            error_out = "this keyboard's modifier map has no Super key, so a "
                        "chord using Super cannot be bound";
            return false;
        }
        mask |= m;
    }

    // CapsLock is always LockMask; NumLock is Mod2 by convention and asked for
    // rather than assumed, same as Alt and Super above.
    static const KeySym num[] = {XK_Num_Lock};
    const unsigned int numlock = maskForKeysyms(num, 1);

    unsigned int combos[4] = {0, LockMask, numlock, LockMask | numlock};
    int n = numlock ? 4 : 2;

    beginCatching();
    for (int i = 0; i < n; ++i) {
        XGrabKey(g_display, kc, mask | combos[i], g_root, False,
                 GrabModeAsync, GrabModeAsync);
        g_grabbed[i] = mask | combos[i];
    }
    g_grabbed_n = n;
    const int err = endCatching();

    if (err != 0) {
        for (int i = 0; i < n; ++i) {
            XUngrabKey(g_display, kc, g_grabbed[i], g_root);
        }
        XSync(g_display, False);
        g_grabbed_n = 0;

        if (err == BadAccess) {
            error_out = "another application already owns " + c.canonical;
        } else {
            error_out = "the X server refused the grab for " + c.canonical +
                        " (X error " + std::to_string(err) + ")";
        }
        return false;
    }

    g_keycode = kc;
    g_mask    = mask;
    g_ignore  = LockMask | numlock;
    g_down    = false;

    if (!g_hooked) {
        SDL_SetX11EventHook(eventHook, nullptr);
        g_hooked = true;
    }
    return true;
}

void ungrab()
{
    if (!g_display || !g_keycode) return;
    for (int i = 0; i < g_grabbed_n; ++i) {
        XUngrabKey(g_display, (KeyCode)g_keycode, g_grabbed[i], g_root);
    }
    XSync(g_display, False);

    g_grabbed_n = 0;
    g_keycode   = 0;
    g_mask      = 0;
    g_ignore    = 0;
    g_down      = false;
}

void quit()
{
    ungrab();
    if (g_hooked) {
        SDL_SetX11EventHook(nullptr, nullptr);
        g_hooked = false;
    }
    // Not XCloseDisplay: this connection is SDL's, and closing it would take
    // the window down with it.
    g_display = nullptr;
    g_root    = 0;
}

} // namespace Hotkey::backend
