#include "Capabilities.h"

#include <cstring>
#include <sstream>

namespace {

// Which SDL video backends contain an implementation for each flag.
// Derived by grepping SDL 3.4.16 src/video/*/ for the flag names:
//
//   SDL_WINDOW_TRANSPARENT     cocoa, wayland, x11, windows, openvr
//   SDL_WINDOW_ALWAYS_ON_TOP   cocoa, x11, windows          <-- no wayland
//
// Wayland has no protocol for "always on top"; a client cannot raise itself.
// That is a compositor policy decision, not an SDL gap, and it is the same
// ceiling Electron and every other toolkit hits.
bool backendImplements(const std::string& driver, const char* flag)
{
    if (std::strcmp(flag, "always_on_top") == 0) {
        return driver == "x11" || driver == "windows" || driver == "cocoa";
    }
    if (std::strcmp(flag, "transparent") == 0) {
        return driver == "x11" || driver == "windows" || driver == "cocoa" ||
               driver == "wayland";
    }
    return true;   // borderless / utility are universal
}

} // namespace

Capabilities Capabilities::probe(SDL_Window* window, SDL_WindowFlags requested)
{
    Capabilities c;

    const char* plat = SDL_GetPlatform();
    const char* drv  = SDL_GetCurrentVideoDriver();
    c.platform     = plat ? plat : "unknown";
    c.video_driver = drv  ? drv  : "unknown";

    const SDL_WindowFlags got = SDL_GetWindowFlags(window);

    auto granted = [&](SDL_WindowFlags f) { return (got & f) != 0; };
    auto asked   = [&](SDL_WindowFlags f) { return (requested & f) != 0; };

    c.borderless   = granted(SDL_WINDOW_BORDERLESS);
    c.skip_taskbar = granted(SDL_WINDOW_UTILITY);

    // Transparency: trust the readback. Every backend that implements it also
    // clears the flag when it cannot get an alpha visual.
    c.transparent = granted(SDL_WINDOW_TRANSPARENT) &&
                    backendImplements(c.video_driver, "transparent");

    // Always-on-top: readback AND backend support, per the note above.
    c.always_on_top = granted(SDL_WINDOW_ALWAYS_ON_TOP) &&
                      backendImplements(c.video_driver, "always_on_top");

    if (asked(SDL_WINDOW_TRANSPARENT) && !c.transparent) {
        c.notes.push_back(
            "Transparency unavailable on video driver '" + c.video_driver +
            "'; falling back to an opaque background.");
    }
    if (asked(SDL_WINDOW_ALWAYS_ON_TOP) && !c.always_on_top) {
        if (c.video_driver == "wayland") {
            c.notes.push_back(
                "Wayland has no protocol for always-on-top; the window will "
                "behave as a normal window. Use the tray icon to raise it.");
        } else {
            c.notes.push_back(
                "Always-on-top unavailable on video driver '" +
                c.video_driver + "'.");
        }
    }
    if (asked(SDL_WINDOW_BORDERLESS) && !c.borderless) {
        c.notes.push_back("Window manager refused to remove decorations.");
    }

    return c;
}

std::string Capabilities::report() const
{
    auto yn = [](bool b) { return b ? "yes" : "no "; };

    std::ostringstream o;
    o << "gobboclippy " GC_VERSION "  (SDL " << SDL_MAJOR_VERSION << "."
      << SDL_MINOR_VERSION << "." << SDL_MICRO_VERSION
      << ", Python " GC_PY_VERSION ")\n"
      << "  platform      : " << platform << "\n"
      << "  video driver  : " << video_driver << "\n"
      << "  borderless    : " << yn(borderless) << "\n"
      << "  always on top : " << yn(always_on_top) << "\n"
      << "  transparent   : " << yn(transparent) << "\n"
      << "  skip taskbar  : " << yn(skip_taskbar) << "\n"
      << "  tray icon     : " << yn(tray) << "\n";

    for (const auto& n : notes) {
        o << "  ! " << n << "\n";
    }
    return o.str();
}
