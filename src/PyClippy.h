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

// Say that this process is `--python`: the module exists, and no window,
// renderer or tray will ever be bound to it. Only the mode itself knows, and
// the difference is worth carrying because it is the difference between two
// error messages -- one that names the mistake the user actually made, and one
// that guesses at a race that cannot happen. In windowed mode bind() runs
// before any script does, so "called too early" was never the real answer.
void setInterpreterMode();
bool interpreterMode();

// What to tell a script that asked for something only a window can give.
// Shared with PyDraw so the renderer and the host answer alike.
const char* noHostMessage(const char* what);

// Point the module at the live host. Call after the App exists.
void bind(App* app);

// Invoke a callback registered from Python with clippy.on(event, fn).
// Returns false if the callback raised; the traceback is printed.
//
// Every fire* below takes the GIL for itself. The event loop runs without it
// (see main()), and these are reached from three directions -- the loop, an SDL
// event, and a tray callback -- so making each entry point responsible for the
// GIL is the only arrangement where none of the three has to remember.
bool fire(const char* event);

// The per-frame hook, which unlike the others is handed the seconds elapsed
// since the previous frame -- the same number the animation system is ticked
// with, so script-side timing and animation timing cannot drift apart.
bool fireFrame(float dt);

// A mouse button going down, as (x, y, button, clicks). SDL counts the clicks
// for us, so 'double_click' is the same event with clicks == 2 rather than a
// timer of ours.
bool fireClick(const char* event, float x, float y, int button, int clicks);

// The microphone started or stopped -- including when it stopped because the
// window hid, which is the case the indicator exists for.
bool fireMic(bool active);

} // namespace PyClippy
