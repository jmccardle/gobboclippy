#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include <SDL3/SDL.h>

#include "Easing.h"

class Drawable;

// McRogueFace's Animation, cut to one kind of target.
//
// The model is unchanged: name a property with a string, give a target value,
// a duration and an easing curve, and the manager ticks it. `delta` makes the
// target relative to wherever the property started; `loop` restarts instead of
// finishing. A std::vector<int> target is the frame-sequence case -- it steps
// through the list rather than interpolating between numbers.
//
// Cut: entities, 3D entities, the Python object cache, and the PyAnimation
// wrapper's back-reference. The Python callback stayed, because "do this when
// the animation lands" is most of what makes a sequence of animations read
// like behaviour instead of a script.

// How to handle a second animation on a property that is already animating.
enum class ConflictMode {
    REPLACE,      // finish the old one immediately, start the new one
    QUEUE,        // start the new one when the old one finishes
    RAISE_ERROR,  // refuse, and say so
};

using AnimationValue = std::variant<
    float,             // x, y, scale_x, opacity, rotation, ...
    int,               // sprite_index, z_index
    std::vector<int>,  // a frame sequence
    SDL_Color,         // color, fill_color
    SDL_FPoint,        // pos, origin, scale
    std::string        // text
>;

// Called when an animation completes. The host installs a trampoline that
// invokes a Python callable; nothing in this file knows about Python.
using AnimationCallback = std::function<void()>;

class Animation {
public:
    Animation(std::string targetProperty,
              AnimationValue targetValue,
              float duration,
              EasingFunction easing = Easing::linear,
              bool delta = false,
              bool loop  = false,
              AnimationCallback callback = nullptr);

    // Captures the property's current value as the start of the interpolation.
    void start(const std::shared_ptr<Drawable>& target);

    // Jump to the final value and fire the callback.
    void complete();

    // Abandon without applying a final value and without firing the callback.
    void stop();

    // Returns false once the animation is finished and can be dropped.
    bool update(float dt);

    const std::string& targetProperty() const { return m_property; }
    float duration() const { return m_duration; }
    float elapsed()  const { return m_elapsed; }
    bool  isComplete() const { return (!m_loop && m_elapsed >= m_duration) || m_stopped; }
    bool  isStopped()  const { return m_stopped; }
    bool  isDelta()    const { return m_delta; }
    bool  isLooping()  const { return m_loop; }

    bool  hasValidTarget() const { return !m_target.expired(); }
    void* targetPtr() const;

    // Drop the callback without firing it. The Python wrapper calls this when
    // it is torn down, so a dead callable is never invoked.
    void clearCallback() { m_callback = nullptr; }

private:
    std::string       m_property;
    AnimationValue    m_start;
    AnimationValue    m_target_value;
    float             m_duration;
    float             m_elapsed = 0.0f;
    EasingFunction    m_easing;
    bool              m_delta;
    bool              m_loop;
    bool              m_stopped = false;

    std::weak_ptr<Drawable> m_target;

    AnimationCallback m_callback;
    bool              m_callback_fired = false;

    AnimationValue interpolate(float t) const;
    void applyValue(Drawable* target, const AnimationValue& value);
    void fireCallback();
};

// One list of running animations, ticked once per frame.
class AnimationManager {
public:
    static AnimationManager& instance();

    // Returns false if `mode` is RAISE_ERROR and the property was already
    // animating; the caller turns that into an exception.
    bool add(const std::shared_ptr<Animation>& animation,
             ConflictMode mode = ConflictMode::REPLACE);

    void update(float dt);
    void clear(bool complete_first = false);

    bool   isPropertyAnimating(void* target, const std::string& property) const;
    size_t activeCount() const { return m_active.size(); }

private:
    AnimationManager() = default;

    struct Key {
        void*       target;
        std::string property;
        bool operator==(const Key& o) const
        {
            return target == o.target && property == o.property;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const
        {
            return std::hash<void*>()(k.target) ^
                   (std::hash<std::string>()(k.property) << 1);
        }
    };

    std::vector<std::shared_ptr<Animation>> m_active;
    std::vector<std::shared_ptr<Animation>> m_pending;   // added during update
    std::unordered_map<Key, std::weak_ptr<Animation>, KeyHash> m_locks;
    std::vector<std::pair<Key, std::shared_ptr<Animation>>>    m_queue;

    bool m_updating = false;

    void cleanupLocks();
    void processQueue();
};
