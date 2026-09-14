// Global hotkeys on Windows.
//
// RegisterHotKey is the whole API, and it is a short one: the system posts
// WM_HOTKEY when the chord is pressed, and that is all it ever posts. There is
// no release message, no WM_HOTKEYUP, no flag to ask for one. So this backend
// reports deliversRelease() == false, and it is the reason that question exists
// separately from available() at all -- a toggle works here, and push-to-talk
// cannot, and the honest thing is to say which rather than synthesise a key
// release from a timer and call a held key released while it is still held.
//
// (The mechanism that would give a release is a WH_KEYBOARD_LL hook, which is a
// different thing entirely: it sees every keystroke on the desktop rather than
// one registered chord, it needs its own message pump, and it is the shape of
// thing security software objects to. If push-to-talk on Windows is wanted, that
// is the work, and it belongs behind this same interface rather than mixed into
// it.)
//
// The message is read back through SDL_SetWindowsMessageHook, which SDL calls
// from its pump after PeekMessage and before TranslateMessage
// (SDL_windowsevents.c), so WM_HOTKEY arrives on the main thread with no pump
// of ours. The chord is registered against the pet's own window, which means
// Windows also releases it if that window goes away without us asking.

#include "../Hotkey.h"

#include <SDL3/SDL_system.h>

#include <windows.h>

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

namespace {

// Any value will do; it only has to be unique among the hotkeys this window
// registers, and this window registers one.
constexpr int kHotkeyId = 0xBC1;

HWND g_hwnd     = nullptr;
bool g_hooked   = false;
bool g_grabbed  = false;

// Names first, characters second. Every SDL keycode that is a printable
// character can be turned into a virtual key by asking the *current* layout
// through VkKeyScanW, which is both shorter and more correct than a table of
// VK_OEM_1-style constants -- those are defined by position on a US keyboard and
// mean something else on any other.
bool virtualKeyFor(SDL_Keycode key, UINT& out)
{
    switch (key) {
    case SDLK_RETURN:    out = VK_RETURN; return true;
    case SDLK_ESCAPE:    out = VK_ESCAPE; return true;
    case SDLK_BACKSPACE: out = VK_BACK;   return true;
    case SDLK_TAB:       out = VK_TAB;    return true;
    case SDLK_SPACE:     out = VK_SPACE;  return true;
    case SDLK_DELETE:    out = VK_DELETE; return true;
    case SDLK_INSERT:    out = VK_INSERT; return true;
    case SDLK_HOME:      out = VK_HOME;   return true;
    case SDLK_END:       out = VK_END;    return true;
    case SDLK_PAGEUP:    out = VK_PRIOR;  return true;
    case SDLK_PAGEDOWN:  out = VK_NEXT;   return true;
    case SDLK_LEFT:      out = VK_LEFT;   return true;
    case SDLK_RIGHT:     out = VK_RIGHT;  return true;
    case SDLK_UP:        out = VK_UP;     return true;
    case SDLK_DOWN:      out = VK_DOWN;   return true;
    default: break;
    }
    if (key >= SDLK_F1  && key <= SDLK_F12) { out = VK_F1  + (key - SDLK_F1);  return true; }
    if (key >= SDLK_F13 && key <= SDLK_F24) { out = VK_F13 + (key - SDLK_F13); return true; }

    if (key >= 0x20 && key <= 0xFFFF) {
        const SHORT scan = VkKeyScanW((WCHAR)key);
        if (scan == -1) return false;

        // The high byte is the shift state needed to produce that character.
        // An SDL keycode is the unshifted key, so this should be zero; if it is
        // not, the layout only reaches this character through a modifier, and
        // the chord the user wrote is not the chord that would fire.
        if (((scan >> 8) & 0xFF) != 0) return false;

        out = (UINT)(scan & 0xFF);
        return true;
    }
    return false;
}

bool SDLCALL messageHook(void*, MSG* msg)
{
    if (msg->message == WM_HOTKEY && (int)msg->wParam == kHotkeyId) {
        Hotkey::post(true);
        return false;      // ours; nothing else needs to see it
    }
    return true;
}

} // namespace

namespace Hotkey::backend {

bool available()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver && SDL_strcmp(driver, "windows") == 0;
}

// See the file comment: WM_HOTKEY is press-only, and that is the API rather
// than an omission here.
bool deliversRelease() { return false; }

const char* unavailableReason()
{
    return "global hotkeys need the windows video driver";
}

bool grab(SDL_Window* window, const Chord& c, std::string& error_out)
{
    if (!g_hwnd) {
        g_hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                              SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                              nullptr);
        if (!g_hwnd) {
            error_out = "the window has no Win32 HWND";
            return false;
        }
    }

    UINT vk = 0;
    if (!virtualKeyFor(c.key, vk)) {
        error_out = std::string("this keyboard layout has no virtual key for '") +
                    SDL_GetKeyName(c.key) + "'";
        return false;
    }

    // MOD_NOREPEAT because a held chord would otherwise post WM_HOTKEY over and
    // over, and for a toggle that is the pet flickering rather than switching.
    UINT mods = MOD_NOREPEAT;
    if (c.mods & kCtrl)  mods |= MOD_CONTROL;
    if (c.mods & kAlt)   mods |= MOD_ALT;
    if (c.mods & kShift) mods |= MOD_SHIFT;
    if (c.mods & kSuper) mods |= MOD_WIN;

    if (!RegisterHotKey(g_hwnd, kHotkeyId, mods, vk)) {
        const DWORD err = GetLastError();
        if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            error_out = "another application already owns " + c.canonical;
        } else {
            error_out = "Windows refused the hotkey " + c.canonical +
                        " (error " + std::to_string((unsigned long)err) + ")";
        }
        return false;
    }
    g_grabbed = true;

    if (!g_hooked) {
        SDL_SetWindowsMessageHook(messageHook, nullptr);
        g_hooked = true;
    }
    return true;
}

void ungrab()
{
    if (!g_grabbed) return;
    UnregisterHotKey(g_hwnd, kHotkeyId);
    g_grabbed = false;
}

void quit()
{
    ungrab();
    if (g_hooked) {
        SDL_SetWindowsMessageHook(nullptr, nullptr);
        g_hooked = false;
    }
    g_hwnd = nullptr;
}

} // namespace Hotkey::backend
