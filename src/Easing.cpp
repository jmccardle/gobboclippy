#include "Easing.h"

#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Easing {

float linear(float t) { return t; }

float easeIn(float t)  { return t * t; }
float easeOut(float t) { return t * (2.0f - t); }
float easeInOut(float t)
{
    return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
}

// Quadratic
float easeInQuad(float t)  { return t * t; }
float easeOutQuad(float t) { return t * (2.0f - t); }
float easeInOutQuad(float t)
{
    return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
}

// Cubic
float easeInCubic(float t) { return t * t * t; }
float easeOutCubic(float t)
{
    const float t1 = t - 1.0f;
    return t1 * t1 * t1 + 1.0f;
}
float easeInOutCubic(float t)
{
    return t < 0.5f ? 4.0f * t * t * t
                    : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
}

// Quartic
float easeInQuart(float t) { return t * t * t * t; }
float easeOutQuart(float t)
{
    const float t1 = t - 1.0f;
    return 1.0f - t1 * t1 * t1 * t1;
}
float easeInOutQuart(float t)
{
    if (t < 0.5f) return 8.0f * t * t * t * t;
    const float t1 = t - 1.0f;
    return 1.0f - 8.0f * t1 * t1 * t1 * t1;
}

// Sine
float easeInSine(float t)    { return 1.0f - std::cos(t * (float)M_PI / 2.0f); }
float easeOutSine(float t)   { return std::sin(t * (float)M_PI / 2.0f); }
float easeInOutSine(float t) { return 0.5f * (1.0f - std::cos((float)M_PI * t)); }

// Exponential
float easeInExpo(float t)
{
    return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f));
}
float easeOutExpo(float t)
{
    return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}
float easeInOutExpo(float t)
{
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    if (t < 0.5f)  return 0.5f * std::pow(2.0f, 20.0f * t - 10.0f);
    return 1.0f - 0.5f * std::pow(2.0f, -20.0f * t + 10.0f);
}

// Circular
float easeInCirc(float t) { return 1.0f - std::sqrt(1.0f - t * t); }
float easeOutCirc(float t)
{
    const float t1 = t - 1.0f;
    return std::sqrt(1.0f - t1 * t1);
}
float easeInOutCirc(float t)
{
    if (t < 0.5f) return 0.5f * (1.0f - std::sqrt(1.0f - 4.0f * t * t));
    const float t1 = 2.0f * t - 2.0f;
    return 0.5f * (std::sqrt(1.0f - t1 * t1) + 1.0f);
}

// Elastic
float easeInElastic(float t)
{
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    const float p  = 0.3f;
    const float s  = p / 4.0f;
    const float t1 = t - 1.0f;
    return -(std::pow(2.0f, 10.0f * t1) *
             std::sin((t1 - s) * (2.0f * (float)M_PI) / p));
}
float easeOutElastic(float t)
{
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    const float p = 0.3f;
    const float s = p / 4.0f;
    return std::pow(2.0f, -10.0f * t) *
               std::sin((t - s) * (2.0f * (float)M_PI) / p) + 1.0f;
}
float easeInOutElastic(float t)
{
    if (t == 0.0f) return 0.0f;
    if (t == 1.0f) return 1.0f;
    const float p  = 0.45f;
    const float s  = p / 4.0f;
    const float t1 = 2.0f * t - 1.0f;
    if (t < 0.5f) {
        return -0.5f * (std::pow(2.0f, 10.0f * t1) *
                        std::sin((t1 - s) * (2.0f * (float)M_PI) / p));
    }
    return std::pow(2.0f, -10.0f * t1) *
               std::sin((t1 - s) * (2.0f * (float)M_PI) / p) * 0.5f + 1.0f;
}

// Back (overshoots the target, then settles)
float easeInBack(float t)
{
    const float s = 1.70158f;
    return t * t * ((s + 1.0f) * t - s);
}
float easeOutBack(float t)
{
    const float s  = 1.70158f;
    const float t1 = t - 1.0f;
    return t1 * t1 * ((s + 1.0f) * t1 + s) + 1.0f;
}
float easeInOutBack(float t)
{
    const float s = 1.70158f * 1.525f;
    if (t < 0.5f) return 0.5f * (4.0f * t * t * ((s + 1.0f) * 2.0f * t - s));
    const float t1 = 2.0f * t - 2.0f;
    return 0.5f * (t1 * t1 * ((s + 1.0f) * t1 + s) + 2.0f);
}

// Bounce
float easeOutBounce(float t)
{
    if (t < 1.0f / 2.75f) {
        return 7.5625f * t * t;
    } else if (t < 2.0f / 2.75f) {
        const float t1 = t - 1.5f / 2.75f;
        return 7.5625f * t1 * t1 + 0.75f;
    } else if (t < 2.5f / 2.75f) {
        const float t1 = t - 2.25f / 2.75f;
        return 7.5625f * t1 * t1 + 0.9375f;
    }
    const float t1 = t - 2.625f / 2.75f;
    return 7.5625f * t1 * t1 + 0.984375f;
}
float easeInBounce(float t) { return 1.0f - easeOutBounce(1.0f - t); }
float easeInOutBounce(float t)
{
    if (t < 0.5f) return 0.5f * easeInBounce(2.0f * t);
    return 0.5f * easeOutBounce(2.0f * t - 1.0f) + 0.5f;
}

// Ping-pong
float pingPong(float t)       { return 1.0f - std::fabs(2.0f * t - 1.0f); }
float pingPongSmooth(float t) { return std::sin((float)M_PI * t); }
float pingPongEaseIn(float t)
{
    const float pp = 1.0f - std::fabs(2.0f * t - 1.0f);
    return pp * pp;
}
float pingPongEaseOut(float t)
{
    const float pp = 1.0f - std::fabs(2.0f * t - 1.0f);
    return pp * (2.0f - pp);
}
float pingPongEaseInOut(float t)
{
    const float s = std::sin((float)M_PI * t);
    return s * s;
}

const Entry table[] = {
    {"LINEAR",                "linear",             linear},
    {"EASE_IN",               "easeIn",             easeIn},
    {"EASE_OUT",              "easeOut",            easeOut},
    {"EASE_IN_OUT",           "easeInOut",          easeInOut},
    {"EASE_IN_QUAD",          "easeInQuad",         easeInQuad},
    {"EASE_OUT_QUAD",         "easeOutQuad",        easeOutQuad},
    {"EASE_IN_OUT_QUAD",      "easeInOutQuad",      easeInOutQuad},
    {"EASE_IN_CUBIC",         "easeInCubic",        easeInCubic},
    {"EASE_OUT_CUBIC",        "easeOutCubic",       easeOutCubic},
    {"EASE_IN_OUT_CUBIC",     "easeInOutCubic",     easeInOutCubic},
    {"EASE_IN_QUART",         "easeInQuart",        easeInQuart},
    {"EASE_OUT_QUART",        "easeOutQuart",       easeOutQuart},
    {"EASE_IN_OUT_QUART",     "easeInOutQuart",     easeInOutQuart},
    {"EASE_IN_SINE",          "easeInSine",         easeInSine},
    {"EASE_OUT_SINE",         "easeOutSine",        easeOutSine},
    {"EASE_IN_OUT_SINE",      "easeInOutSine",      easeInOutSine},
    {"EASE_IN_EXPO",          "easeInExpo",         easeInExpo},
    {"EASE_OUT_EXPO",         "easeOutExpo",        easeOutExpo},
    {"EASE_IN_OUT_EXPO",      "easeInOutExpo",      easeInOutExpo},
    {"EASE_IN_CIRC",          "easeInCirc",         easeInCirc},
    {"EASE_OUT_CIRC",         "easeOutCirc",        easeOutCirc},
    {"EASE_IN_OUT_CIRC",      "easeInOutCirc",      easeInOutCirc},
    {"EASE_IN_ELASTIC",       "easeInElastic",      easeInElastic},
    {"EASE_OUT_ELASTIC",      "easeOutElastic",     easeOutElastic},
    {"EASE_IN_OUT_ELASTIC",   "easeInOutElastic",   easeInOutElastic},
    {"EASE_IN_BACK",          "easeInBack",         easeInBack},
    {"EASE_OUT_BACK",         "easeOutBack",        easeOutBack},
    {"EASE_IN_OUT_BACK",      "easeInOutBack",      easeInOutBack},
    {"EASE_IN_BOUNCE",        "easeInBounce",       easeInBounce},
    {"EASE_OUT_BOUNCE",       "easeOutBounce",      easeOutBounce},
    {"EASE_IN_OUT_BOUNCE",    "easeInOutBounce",    easeInOutBounce},
    {"PING_PONG",             "pingPong",           pingPong},
    {"PING_PONG_SMOOTH",      "pingPongSmooth",     pingPongSmooth},
    {"PING_PONG_EASE_IN",     "pingPongEaseIn",     pingPongEaseIn},
    {"PING_PONG_EASE_OUT",    "pingPongEaseOut",    pingPongEaseOut},
    {"PING_PONG_EASE_IN_OUT", "pingPongEaseInOut",  pingPongEaseInOut},
};

const int count = (int)(sizeof(table) / sizeof(table[0]));

EasingFunction byName(const std::string& name)
{
    for (int i = 0; i < count; ++i) {
        if (name == table[i].name || name == table[i].legacy) return table[i].fn;
    }
    return nullptr;
}

EasingFunction byIndex(int index)
{
    if (index < 0 || index >= count) return nullptr;
    return table[index].fn;
}

} // namespace Easing
