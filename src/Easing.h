#pragma once
#include <string>

// The easing library, lifted from McRogueFace's `EasingFunctions` namespace
// (src/Animation.cpp) and its enum table (src/PyEasing.cpp). The curves are
// verbatim -- an easing function is a pure float->float, so there was nothing
// to port.
//
// One simplification: McRogueFace stores these as std::function, because its
// Animation can in principle hold a closure. Nothing here needs that, so a
// plain function pointer does the job and keeps Animation trivially copyable.
using EasingFunction = float (*)(float);

namespace Easing {

float linear(float t);
float easeIn(float t);
float easeOut(float t);
float easeInOut(float t);

float easeInQuad(float t);
float easeOutQuad(float t);
float easeInOutQuad(float t);

float easeInCubic(float t);
float easeOutCubic(float t);
float easeInOutCubic(float t);

float easeInQuart(float t);
float easeOutQuart(float t);
float easeInOutQuart(float t);

float easeInSine(float t);
float easeOutSine(float t);
float easeInOutSine(float t);

float easeInExpo(float t);
float easeOutExpo(float t);
float easeInOutExpo(float t);

float easeInCirc(float t);
float easeOutCirc(float t);
float easeInOutCirc(float t);

float easeInElastic(float t);
float easeOutElastic(float t);
float easeInOutElastic(float t);

float easeInBack(float t);
float easeOutBack(float t);
float easeInOutBack(float t);

float easeInBounce(float t);
float easeOutBounce(float t);
float easeInOutBounce(float t);

// 0 -> 1 -> 0. The point of these is looping animations that return to where
// they started, so the seam between one cycle and the next is invisible.
float pingPong(float t);
float pingPongSmooth(float t);
float pingPongEaseIn(float t);
float pingPongEaseOut(float t);
float pingPongEaseInOut(float t);

// The table backing clippy.Easing. Order is the enum's integer value, and it
// matches McRogueFace's so a script written against one reads the same here.
struct Entry {
    const char*    name;    // SCREAMING_SNAKE, as the enum member
    const char*    legacy;  // camelCase, as McRogueFace's old string API
    EasingFunction fn;
};

extern const Entry table[];
extern const int   count;

// Look up by either spelling. Returns nullptr if the name is unknown -- the
// caller reports that; there is no silent fallback to linear, because a
// misspelled easing name would otherwise animate and look almost right.
EasingFunction byName(const std::string& name);

// nullptr if out of range, for the same reason.
EasingFunction byIndex(int index);

} // namespace Easing
