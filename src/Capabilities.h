#pragma once
#include <string>
#include <vector>

#include <SDL3/SDL.h>

// What the platform actually gave us, as opposed to what we asked for.
//
// Every field is determined one of three ways:
//   granted  -- SDL_GetWindowFlags() readback after window creation
//   backend  -- whether the active SDL video driver implements the flag at all
//   built    -- what this binary was compiled to do, which is `image_formats`
//               alone. It is not a platform fact and does not pretend to be
//               probed; it is here because a script that is about to download
//               art needs to know what this build can decode, and a fork that
//               drops a decoder should say so in the same place everything
//               else is said.
//
// The second matters because SDL keeps a requested flag in the window's flag
// set even when the backend has no code for it. Wayland and SDL_WINDOW_
// ALWAYS_ON_TOP is the case that bites: the flag reads back as set, and the
// window still does not stay on top. Reporting only the readback would be a
// lie, so we cross-check against a table derived from SDL's own sources.
struct Capabilities {
    std::string platform;       // SDL_GetPlatform()
    std::string video_driver;   // SDL_GetCurrentVideoDriver()

    bool borderless     = false;
    bool always_on_top  = false;
    bool transparent    = false;
    bool skip_taskbar   = false;
    bool tray           = false;
    bool microphone     = false;

    // Image formats Texture::load can decode, in no particular order.
    std::vector<std::string> image_formats;

    // Human-readable degradations, e.g. why always_on_top is false.
    std::vector<std::string> notes;

    // Fill in everything except `tray` and `microphone`, which the host sets
    // once Tray::create() and Mic::devices() have answered.
    static Capabilities probe(SDL_Window* window, SDL_WindowFlags requested);

    std::string report() const;   // multi-line, for --capabilities and the log
};
