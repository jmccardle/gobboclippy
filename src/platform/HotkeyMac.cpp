// Global hotkeys on macOS.
//
// RegisterEventHotKey, from Carbon. That choice is the interesting part of this
// file, because the obvious answer is a CGEventTap and the obvious answer is
// worse: a tap observes every keystroke on the machine, so macOS gates it
// behind the Accessibility permission, which means a second TCC prompt on top
// of the microphone's and a pet that does not work until the user has been to
// System Settings. RegisterEventHotKey registers one chord with the window
// server instead, needs no permission at all, and -- unlike Windows'
// RegisterHotKey -- reports the release as well as the press.
//
// Carbon is deprecated and has been for years. It is also still here, still
// works, and is what a large share of shipping Mac applications use for exactly
// this; the replacement Apple points at is the event tap, with the permission
// prompt. When that changes this file is the only thing that has to.
//
// NOT VERIFIED ON HARDWARE. macOS is not buildable on the development machine
// (see ROADMAP.md, "Standing constraints"), so CI compiles this and the smoke
// test exercises bind and unbind, but nobody has yet pressed the key. The two
// things to check first when somebody can: that kEventHotKeyReleased actually
// arrives, and that no permission dialog appears.

#include "../Hotkey.h"

#include <Carbon/Carbon.h>

namespace {

constexpr UInt32 kHotkeySignature = 'gbcl';
constexpr UInt32 kHotkeyId        = 1;

EventHotKeyRef  g_ref     = nullptr;
EventHandlerRef g_handler = nullptr;

// SDL keycodes are unshifted characters; Carbon wants a virtual keycode, which
// is a physical position on an ANSI keyboard. The kVK_ANSI_* constants are
// named for the letter printed on a US layout, so this table reads as the
// identity it looks like -- and on a non-US layout it binds the physical key
// where that letter would be, which is what every Mac application does.
bool virtualKeyFor(SDL_Keycode key, UInt32& out)
{
    switch (key) {
    case SDLK_A: out = kVK_ANSI_A; return true;
    case SDLK_B: out = kVK_ANSI_B; return true;
    case SDLK_C: out = kVK_ANSI_C; return true;
    case SDLK_D: out = kVK_ANSI_D; return true;
    case SDLK_E: out = kVK_ANSI_E; return true;
    case SDLK_F: out = kVK_ANSI_F; return true;
    case SDLK_G: out = kVK_ANSI_G; return true;
    case SDLK_H: out = kVK_ANSI_H; return true;
    case SDLK_I: out = kVK_ANSI_I; return true;
    case SDLK_J: out = kVK_ANSI_J; return true;
    case SDLK_K: out = kVK_ANSI_K; return true;
    case SDLK_L: out = kVK_ANSI_L; return true;
    case SDLK_M: out = kVK_ANSI_M; return true;
    case SDLK_N: out = kVK_ANSI_N; return true;
    case SDLK_O: out = kVK_ANSI_O; return true;
    case SDLK_P: out = kVK_ANSI_P; return true;
    case SDLK_Q: out = kVK_ANSI_Q; return true;
    case SDLK_R: out = kVK_ANSI_R; return true;
    case SDLK_S: out = kVK_ANSI_S; return true;
    case SDLK_T: out = kVK_ANSI_T; return true;
    case SDLK_U: out = kVK_ANSI_U; return true;
    case SDLK_V: out = kVK_ANSI_V; return true;
    case SDLK_W: out = kVK_ANSI_W; return true;
    case SDLK_X: out = kVK_ANSI_X; return true;
    case SDLK_Y: out = kVK_ANSI_Y; return true;
    case SDLK_Z: out = kVK_ANSI_Z; return true;

    case SDLK_0: out = kVK_ANSI_0; return true;
    case SDLK_1: out = kVK_ANSI_1; return true;
    case SDLK_2: out = kVK_ANSI_2; return true;
    case SDLK_3: out = kVK_ANSI_3; return true;
    case SDLK_4: out = kVK_ANSI_4; return true;
    case SDLK_5: out = kVK_ANSI_5; return true;
    case SDLK_6: out = kVK_ANSI_6; return true;
    case SDLK_7: out = kVK_ANSI_7; return true;
    case SDLK_8: out = kVK_ANSI_8; return true;
    case SDLK_9: out = kVK_ANSI_9; return true;

    case SDLK_MINUS:        out = kVK_ANSI_Minus;        return true;
    case SDLK_EQUALS:       out = kVK_ANSI_Equal;        return true;
    case SDLK_LEFTBRACKET:  out = kVK_ANSI_LeftBracket;  return true;
    case SDLK_RIGHTBRACKET: out = kVK_ANSI_RightBracket; return true;
    case SDLK_SEMICOLON:    out = kVK_ANSI_Semicolon;    return true;
    case SDLK_APOSTROPHE:   out = kVK_ANSI_Quote;        return true;
    case SDLK_COMMA:        out = kVK_ANSI_Comma;        return true;
    case SDLK_PERIOD:       out = kVK_ANSI_Period;       return true;
    case SDLK_SLASH:        out = kVK_ANSI_Slash;        return true;
    case SDLK_BACKSLASH:    out = kVK_ANSI_Backslash;    return true;
    case SDLK_GRAVE:        out = kVK_ANSI_Grave;        return true;

    // kVK_Delete is the backspace key and kVK_ForwardDelete is the one labelled
    // Delete on a full keyboard. Getting these the wrong way round is the
    // classic Carbon mistake, so they are written out rather than paired up.
    case SDLK_RETURN:    out = kVK_Return;        return true;
    case SDLK_TAB:       out = kVK_Tab;           return true;
    case SDLK_SPACE:     out = kVK_Space;         return true;
    case SDLK_BACKSPACE: out = kVK_Delete;        return true;
    case SDLK_DELETE:    out = kVK_ForwardDelete; return true;
    case SDLK_ESCAPE:    out = kVK_Escape;        return true;
    case SDLK_INSERT:    out = kVK_Help;          return true;
    case SDLK_HOME:      out = kVK_Home;          return true;
    case SDLK_END:       out = kVK_End;           return true;
    case SDLK_PAGEUP:    out = kVK_PageUp;        return true;
    case SDLK_PAGEDOWN:  out = kVK_PageDown;      return true;
    case SDLK_LEFT:      out = kVK_LeftArrow;     return true;
    case SDLK_RIGHT:     out = kVK_RightArrow;    return true;
    case SDLK_UP:        out = kVK_UpArrow;       return true;
    case SDLK_DOWN:      out = kVK_DownArrow;     return true;

    // Not contiguous, and not a mistake -- Apple's function-key codes are in
    // the order the keys were added to Macs rather than the order they are
    // printed in. There is no F21 upwards.
    case SDLK_F1:  out = kVK_F1;  return true;
    case SDLK_F2:  out = kVK_F2;  return true;
    case SDLK_F3:  out = kVK_F3;  return true;
    case SDLK_F4:  out = kVK_F4;  return true;
    case SDLK_F5:  out = kVK_F5;  return true;
    case SDLK_F6:  out = kVK_F6;  return true;
    case SDLK_F7:  out = kVK_F7;  return true;
    case SDLK_F8:  out = kVK_F8;  return true;
    case SDLK_F9:  out = kVK_F9;  return true;
    case SDLK_F10: out = kVK_F10; return true;
    case SDLK_F11: out = kVK_F11; return true;
    case SDLK_F12: out = kVK_F12; return true;
    case SDLK_F13: out = kVK_F13; return true;
    case SDLK_F14: out = kVK_F14; return true;
    case SDLK_F15: out = kVK_F15; return true;
    case SDLK_F16: out = kVK_F16; return true;
    case SDLK_F17: out = kVK_F17; return true;
    case SDLK_F18: out = kVK_F18; return true;
    case SDLK_F19: out = kVK_F19; return true;
    case SDLK_F20: out = kVK_F20; return true;

    default: return false;
    }
}

OSStatus hotkeyHandler(EventHandlerCallRef, EventRef event, void*)
{
    EventHotKeyID id;
    if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                          nullptr, sizeof(id), nullptr, &id) != noErr) {
        return eventNotHandledErr;
    }
    if (id.signature != kHotkeySignature || id.id != kHotkeyId) {
        return eventNotHandledErr;
    }

    switch (GetEventKind(event)) {
    case kEventHotKeyPressed:  Hotkey::post(true);  return noErr;
    case kEventHotKeyReleased: Hotkey::post(false); return noErr;
    default:                   return eventNotHandledErr;
    }
}

} // namespace

namespace Hotkey::backend {

bool available()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && SDL_strcmp(driver, "cocoa") == 0;
}

bool deliversRelease() { return true; }

const char* unavailableReason()
{
    return "global hotkeys need the cocoa video driver";
}

bool grab(SDL_Window*, const Chord& c, std::string& error_out)
{
    UInt32 vk = 0;
    if (!virtualKeyFor(c.key, vk)) {
        error_out = std::string("macOS has no virtual key code for '") +
                    SDL_GetKeyName(c.key) + "'";
        return false;
    }

    UInt32 mods = 0;
    if (c.mods & kCtrl)  mods |= controlKey;
    if (c.mods & kAlt)   mods |= optionKey;
    if (c.mods & kShift) mods |= shiftKey;
    if (c.mods & kSuper) mods |= cmdKey;

    if (!g_handler) {
        static const EventTypeSpec kTypes[] = {
            {kEventClassKeyboard, kEventHotKeyPressed},
            {kEventClassKeyboard, kEventHotKeyReleased},
        };
        const OSStatus s = InstallApplicationEventHandler(
            &hotkeyHandler, 2, kTypes, nullptr, &g_handler);
        if (s != noErr) {
            g_handler = nullptr;
            error_out = "could not install the Carbon event handler (OSStatus " +
                        std::to_string((long)s) + ")";
            return false;
        }
    }

    EventHotKeyID id;
    id.signature = kHotkeySignature;
    id.id        = kHotkeyId;

    const OSStatus s = RegisterEventHotKey(vk, mods, id,
                                           GetApplicationEventTarget(), 0, &g_ref);
    if (s != noErr) {
        g_ref = nullptr;
        if (s == eventHotKeyExistsErr) {
            error_out = "another application already owns " + c.canonical;
        } else {
            error_out = "macOS refused the hotkey " + c.canonical +
                        " (OSStatus " + std::to_string((long)s) + ")";
        }
        return false;
    }
    return true;
}

void ungrab()
{
    if (!g_ref) return;
    UnregisterEventHotKey(g_ref);
    g_ref = nullptr;
}

void quit()
{
    ungrab();
    if (g_handler) {
        RemoveEventHandler(g_handler);
        g_handler = nullptr;
    }
}

} // namespace Hotkey::backend
