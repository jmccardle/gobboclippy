#pragma once
#include <string>

struct App;

// The `clippy` extension module: the seam between the C++ host and the
// scripted behaviour. Kept deliberately small -- window, tray and lifecycle
// only. Sprite/animation/caption objects harvested from McRogueFace will land
// alongside these, not replace them.
namespace PyClippy {

// Register the module. Must be called before Py_InitializeFromConfig.
bool registerModule(std::string& error_out);

// Point the module at the live host. Call after the App exists.
void bind(App* app);

// Invoke a callback registered from Python with clippy.on(event, fn).
// Returns false if the callback raised; the traceback is printed.
bool fire(const char* event);

// The per-frame hook, which unlike the others is handed the seconds elapsed
// since the previous frame -- the same number the animation system is ticked
// with, so script-side timing and animation timing cannot drift apart.
bool fireFrame(float dt);

} // namespace PyClippy
