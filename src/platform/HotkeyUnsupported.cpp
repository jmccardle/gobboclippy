// Global hotkeys on a platform nobody has written a backend for.
//
// This is not a fallback and it does not stand in for one. It refuses, loudly,
// so that a build on a platform outside the three we ship -- a BSD, most
// likely, where this tree compiles today and should keep compiling -- still
// links, still runs, and still tells the truth: available() is false,
// --capabilities prints "hotkey : no" with the reason, and bind() fails with a
// message naming what is missing rather than accepting a chord that will never
// fire.
//
// A backend added here needs the same four answers as the others, and the one
// worth thinking about before the code is deliversRelease(): if the platform's
// mechanism reports only the press, say so, because push-to-talk reads that
// answer and there is no honest way to guess a key release.

#include "../Hotkey.h"

namespace Hotkey::backend {

bool available()       { return false; }
bool deliversRelease() { return false; }

const char* unavailableReason()
{
    // Held in a static because this returns a borrowed pointer, and the
    // platform name is only known at runtime.
    static std::string message;
    const char* plat = SDL_GetPlatform();
    message = std::string("this build has no global hotkey backend for ") +
              (plat ? plat : "this platform") +
              "; see src/platform/ for the three that exist";
    return message.c_str();
}

bool grab(SDL_Window*, const Chord&, std::string& error_out)
{
    error_out = unavailableReason();
    return false;
}

void ungrab() {}
void quit()   {}

} // namespace Hotkey::backend
