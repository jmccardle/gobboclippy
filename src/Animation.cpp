#include "Animation.h"

#include <algorithm>
#include <cmath>

#include "Drawable.h"

namespace {

Uint8 clampChannel(float v)
{
    return (Uint8)SDL_lroundf(SDL_clamp(v, 0.0f, 255.0f));
}

} // namespace

Animation::Animation(std::string targetProperty,
                     AnimationValue targetValue,
                     float duration,
                     EasingFunction easing,
                     bool delta,
                     bool loop,
                     AnimationCallback callback)
    : m_property(std::move(targetProperty))
    , m_target_value(std::move(targetValue))
    , m_duration(duration)
    , m_easing(easing ? easing : Easing::linear)
    , m_delta(delta)
    , m_loop(loop)
    , m_callback(std::move(callback))
{
}

void* Animation::targetPtr() const
{
    if (auto sp = m_target.lock()) return sp.get();
    return nullptr;
}

void Animation::start(const std::shared_ptr<Drawable>& target)
{
    if (!target) return;

    m_target         = target;
    m_elapsed        = 0.0f;
    m_stopped        = false;
    m_callback_fired = false;

    // Capture the start value in the same type as the target value, so
    // interpolate() never has to reconcile two different variant alternatives.
    std::visit([this, &target](const auto& tv) {
        using T = std::decay_t<decltype(tv)>;

        if constexpr (std::is_same_v<T, float>) {
            float v;
            if (target->getProperty(m_property, v)) m_start = v;
        } else if constexpr (std::is_same_v<T, int>) {
            // Most properties are float-backed; try that first, as McRogueFace
            // does, so animating "z_index" to an int literal still works.
            float fv;
            int   iv;
            if (target->getProperty(m_property, fv))      m_start = (int)fv;
            else if (target->getProperty(m_property, iv)) m_start = iv;
        } else if constexpr (std::is_same_v<T, std::vector<int>>) {
            int v;
            if (target->getProperty(m_property, v)) m_start = v;
        } else if constexpr (std::is_same_v<T, SDL_Color>) {
            SDL_Color v;
            if (target->getProperty(m_property, v)) m_start = v;
        } else if constexpr (std::is_same_v<T, SDL_FPoint>) {
            SDL_FPoint v;
            if (target->getProperty(m_property, v)) m_start = v;
        } else if constexpr (std::is_same_v<T, std::string>) {
            std::string v;
            if (target->getProperty(m_property, v)) m_start = v;
        }
    }, m_target_value);
}

void Animation::complete()
{
    if (m_stopped) return;

    m_elapsed = m_duration;
    if (auto target = m_target.lock()) {
        applyValue(target.get(), interpolate(m_easing(1.0f)));
    }
    if (!m_callback_fired) fireCallback();
}

void Animation::stop()
{
    m_stopped        = true;
    m_callback_fired = true;   // stopping is not completing: no callback
}

bool Animation::update(float dt)
{
    if (m_stopped) return false;

    auto target = m_target.lock();
    if (!target) return false;   // target died; drop the animation silently

    m_elapsed += dt;
    if (m_loop && m_duration > 0.0f) {
        while (m_elapsed >= m_duration) m_elapsed -= m_duration;
    } else {
        m_elapsed = SDL_min(m_elapsed, m_duration);
    }

    const float t = m_duration > 0.0f ? m_elapsed / m_duration : 1.0f;
    applyValue(target.get(), interpolate(m_easing(t)));

    if (isComplete() && !m_callback_fired) fireCallback();
    return !isComplete();
}

AnimationValue Animation::interpolate(float t) const
{
    return std::visit([this, t](const auto& target) -> AnimationValue {
        using T = std::decay_t<decltype(target)>;

        if constexpr (std::is_same_v<T, float>) {
            const float* s = std::get_if<float>(&m_start);
            if (!s) return target;
            return m_delta ? *s + target * t : *s + (target - *s) * t;
        }
        else if constexpr (std::is_same_v<T, int>) {
            const int* s = std::get_if<int>(&m_start);
            if (!s) return target;
            const float r = m_delta ? *s + target * t
                                    : *s + (target - *s) * t;
            return (int)std::lround(r);
        }
        else if constexpr (std::is_same_v<T, std::vector<int>>) {
            // A frame sequence steps, it does not blend. Each frame gets an
            // equal share of the duration; the last frame holds through t==1.
            if (target.empty()) return 0;
            size_t index = (size_t)(t * (float)target.size());
            index = SDL_min(index, target.size() - 1);
            return target[index];
        }
        else if constexpr (std::is_same_v<T, SDL_Color>) {
            const SDL_Color* s = std::get_if<SDL_Color>(&m_start);
            if (!s) return target;
            SDL_Color out;
            if (m_delta) {
                out.r = clampChannel(s->r + target.r * t);
                out.g = clampChannel(s->g + target.g * t);
                out.b = clampChannel(s->b + target.b * t);
                out.a = clampChannel(s->a + target.a * t);
            } else {
                out.r = clampChannel(s->r + (target.r - s->r) * t);
                out.g = clampChannel(s->g + (target.g - s->g) * t);
                out.b = clampChannel(s->b + (target.b - s->b) * t);
                out.a = clampChannel(s->a + (target.a - s->a) * t);
            }
            return out;
        }
        else if constexpr (std::is_same_v<T, SDL_FPoint>) {
            const SDL_FPoint* s = std::get_if<SDL_FPoint>(&m_start);
            if (!s) return target;
            if (m_delta) {
                return SDL_FPoint{s->x + target.x * t, s->y + target.y * t};
            }
            return SDL_FPoint{s->x + (target.x - s->x) * t,
                              s->y + (target.y - s->y) * t};
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            // Typewriter: reveal the target a character at a time. In delta
            // mode the target is appended to whatever was already there.
            const std::string* s = std::get_if<std::string>(&m_start);
            if (!s) return target;
            if (m_delta) {
                return *s + target.substr(0, (size_t)(target.length() * t));
            }
            if (t < 0.5f) {
                return s->substr(0, (size_t)(s->length() * (1.0f - t * 2.0f)));
            }
            return target.substr(0, (size_t)(target.length() * ((t - 0.5f) * 2.0f)));
        }
        else {
            return target;
        }
    }, m_target_value);
}

void Animation::applyValue(Drawable* target, const AnimationValue& value)
{
    if (!target) return;
    std::visit([this, target](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::vector<int>>) {
            // interpolate() has already reduced a sequence to one frame; a
            // vector reaching here would be a bug, not something to paper over.
            (void)v;
        } else {
            target->setProperty(m_property, v);
        }
    }, value);
}

void Animation::fireCallback()
{
    m_callback_fired = true;
    if (m_callback) m_callback();
}

// --- AnimationManager ------------------------------------------------------

AnimationManager& AnimationManager::instance()
{
    static AnimationManager m;
    return m;
}

bool AnimationManager::isPropertyAnimating(void* target,
                                           const std::string& property) const
{
    if (!target) return false;
    auto it = m_locks.find(Key{target, property});
    return it != m_locks.end() && !it->second.expired();
}

void AnimationManager::cleanupLocks()
{
    for (auto it = m_locks.begin(); it != m_locks.end();) {
        if (it->second.expired()) it = m_locks.erase(it);
        else                      ++it;
    }
}

void AnimationManager::processQueue()
{
    for (auto it = m_queue.begin(); it != m_queue.end();) {
        const Key& key  = it->first;
        auto&      anim = it->second;

        auto lock = m_locks.find(key);
        const bool free = (lock == m_locks.end()) || lock->second.expired();

        if (!anim || !anim->hasValidTarget()) {
            it = m_queue.erase(it);
        } else if (free) {
            m_locks[key] = anim;
            m_active.push_back(anim);
            it = m_queue.erase(it);
        } else {
            ++it;
        }
    }
}

bool AnimationManager::add(const std::shared_ptr<Animation>& animation,
                           ConflictMode mode)
{
    if (!animation || !animation->hasValidTarget()) return true;

    const Key key{animation->targetPtr(), animation->targetProperty()};

    auto existing = m_locks.find(key);
    if (existing != m_locks.end() && !existing->second.expired()) {
        auto old = existing->second.lock();
        switch (mode) {
        case ConflictMode::REPLACE:
            if (old) {
                if (m_updating) {
                    // Mid-update the active list is being iterated; stopping is
                    // safe, completing (which can run a callback that adds more
                    // animations) is not. The sweep below drops it.
                    old->stop();
                } else {
                    old->complete();
                    m_active.erase(
                        std::remove(m_active.begin(), m_active.end(), old),
                        m_active.end());
                }
            }
            break;   // fall through to registering the new animation

        case ConflictMode::QUEUE:
            if (m_updating) m_pending.push_back(animation);
            else            m_queue.emplace_back(key, animation);
            return true;

        case ConflictMode::RAISE_ERROR:
            return false;
        }
    }

    m_locks[key] = animation;
    if (m_updating) m_pending.push_back(animation);
    else            m_active.push_back(animation);
    return true;
}

void AnimationManager::update(float dt)
{
    m_updating = true;
    m_active.erase(
        std::remove_if(m_active.begin(), m_active.end(),
                       [dt](std::shared_ptr<Animation>& a) {
                           return !a || !a->update(dt);
                       }),
        m_active.end());
    m_updating = false;

    cleanupLocks();
    processQueue();

    // Animations created from a completion callback during the sweep above.
    if (!m_pending.empty()) {
        std::vector<std::shared_ptr<Animation>> pending;
        pending.swap(m_pending);
        for (auto& anim : pending) {
            if (!anim || !anim->hasValidTarget()) continue;

            const Key key{anim->targetPtr(), anim->targetProperty()};
            auto lock = m_locks.find(key);
            const bool holds_lock = lock != m_locks.end() && lock->second.lock() == anim;
            const bool free = (lock == m_locks.end()) || lock->second.expired();

            if (holds_lock || free) {
                m_locks[key] = anim;
                m_active.push_back(anim);
            } else {
                m_queue.emplace_back(key, anim);
            }
        }
    }
}

void AnimationManager::clear(bool complete_first)
{
    if (complete_first) {
        for (auto& a : m_active) if (a) a->complete();
    }
    m_active.clear();
    m_pending.clear();
    m_queue.clear();
    m_locks.clear();
}
