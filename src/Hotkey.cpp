#include "Hotkey.h"

#include <cctype>

namespace {

SDL_Window*        g_window = nullptr;
SDL_ThreadID       g_thread = 0;
Hotkey::Chord      g_bound;
bool               g_have_bound = false;
std::vector<Hotkey::Event> g_queue;

// The loop drains this every frame, so anything past a handful means the loop
// is not running -- a script blocking the frame hook, or a machine that just
// woke up. Dropping the overflow is right (a keypress from thirty seconds ago
// is not a keypress any more) but doing it quietly is not, because a dropped
// release is how push-to-talk will one day get stuck down.
constexpr size_t kQueueLimit = 64;

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// The modifier names accepted on the way in. Several spellings each, because
// the same key is called different things depending on which keyboard the
// person writing the config has been looking at, and refusing "cmd" on a Mac
// would be pedantry rather than rigour.
unsigned modifierNamed(const std::string& word)
{
    const std::string w = lower(word);
    if (w == "ctrl" || w == "control")            return Hotkey::kCtrl;
    if (w == "alt" || w == "opt" || w == "option") return Hotkey::kAlt;
    if (w == "shift")                              return Hotkey::kShift;
    if (w == "super" || w == "win" || w == "cmd" ||
        w == "command" || w == "meta")             return Hotkey::kSuper;
    return 0;
}

// One fixed order out, whatever order came in, so a chord has one spelling.
std::string canonicalise(unsigned mods, SDL_Keycode key)
{
    std::string out;
    if (mods & Hotkey::kCtrl)  out += "Ctrl+";
    if (mods & Hotkey::kAlt)   out += "Alt+";
    if (mods & Hotkey::kShift) out += "Shift+";
    if (mods & Hotkey::kSuper) out += "Super+";

    const char* name = SDL_GetKeyName(key);
    out += (name && *name) ? name : "?";
    return out;
}

} // namespace

namespace Hotkey {

bool parse(const std::string& text, Chord& out, std::string& error_out)
{
    const std::string whole = trim(text);
    if (whole.empty()) {
        error_out = "empty hotkey; expected something like 'Ctrl+Alt+G'";
        return false;
    }

    // Split on '+'. A key whose own name is '+' does not arise: SDL keycodes
    // are the *unshifted* key, so the plus on a US layout is "=" and writes as
    // "Ctrl+=" with no ambiguity to resolve.
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t plus = whole.find('+', start);
        parts.push_back(trim(whole.substr(start, plus == std::string::npos
                                                 ? std::string::npos
                                                 : plus - start)));
        if (plus == std::string::npos) break;
        start = plus + 1;
    }

    for (const std::string& p : parts) {
        if (p.empty()) {
            error_out = "'" + whole + "' has an empty component; expected "
                        "modifiers and one key joined by '+', as in 'Ctrl+Alt+G'";
            return false;
        }
    }

    Chord c;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const unsigned m = modifierNamed(parts[i]);
        if (!m) {
            error_out = "'" + parts[i] + "' is not a modifier. Expected Ctrl, "
                        "Alt, Shift or Super (Win, Cmd and Option are accepted "
                        "as aliases). Only the last component may be a key.";
            return false;
        }
        c.mods |= m;
    }

    const std::string& key_name = parts.back();
    c.key = SDL_GetKeyFromName(key_name.c_str());
    if (c.key == SDLK_UNKNOWN) {
        // The spellings below are SDL's own, copied from its scancode name
        // table rather than guessed at -- "PageUp" has no space in it, and a
        // message that suggested otherwise would be the error telling you to
        // write something it is about to reject. Matching is
        // case-insensitive, so the capitals here are only for reading.
        error_out = "'" + key_name + "' is not a key name. These are SDL's "
                    "names, and they are matched without regard to case: a "
                    "letter or digit, 'F1' through 'F24', 'Space', 'Tab', "
                    "'Escape', 'Return', 'Backspace', 'Insert', 'Delete', "
                    "'Home', 'End', 'PageUp', 'PageDown', or an arrow "
                    "('Left', 'Right', 'Up', 'Down').";
        return false;
    }

    // See the header: a grab with no real modifier takes the key away from the
    // whole desktop, and Shift on its own is the same thing wearing a hat.
    if (!(c.mods & (kCtrl | kAlt | kSuper))) {
        error_out = "'" + whole + "' needs Ctrl, Alt or Super in it. A global "
                    "grab takes the key from every other application, so a "
                    "bare key -- or Shift and a key -- would stop you typing "
                    "it anywhere.";
        return false;
    }

    c.canonical = canonicalise(c.mods, c.key);
    out = c;
    return true;
}

void attach(SDL_Window* window)
{
    g_window = window;
    g_thread = SDL_GetCurrentThreadID();
}

bool onMainThread()
{
    return g_thread != 0 && SDL_GetCurrentThreadID() == g_thread;
}

bool available()
{
    return g_window != nullptr && backend::available();
}

bool deliversRelease()
{
    return available() && backend::deliversRelease();
}

const char* unavailableReason()
{
    if (available()) return "";
    if (!g_window)   return "no window; global hotkeys need one to hang off";
    return backend::unavailableReason();
}

bool bind(const Chord& c, std::string& error_out)
{
    if (!onMainThread()) {
        error_out = "clippy.hotkey can only be used from the thread the event "
                    "loop runs on. Hand the chord to your 'frame' hook and "
                    "bind it there.";
        return false;
    }
    if (!available()) {
        error_out = std::string("cannot bind ") + c.canonical + ": " +
                    unavailableReason();
        return false;
    }

    // The old grab goes first even though the new one may fail. The two orders
    // are both wrong in some way; this one is wrong in the way that is visible
    // -- a failed rebind leaves nothing bound and says so, rather than leaving
    // the previous chord live while the caller has been told it is gone.
    unbind();

    if (!backend::grab(g_window, c, error_out)) return false;

    g_bound      = c;
    g_have_bound = true;
    return true;
}

void unbind()
{
    if (!g_have_bound) return;
    backend::ungrab();
    g_have_bound = false;
    g_bound      = Chord{};

    // A release that arrives after the grab is gone has nothing to pair with.
    g_queue.clear();
}

const Chord* bound() { return g_have_bound ? &g_bound : nullptr; }

void post(bool pressed)
{
    if (g_queue.size() >= kQueueLimit) {
        SDL_Log("[hotkey] dropping a key %s: %zu events queued and nothing "
                "draining them. The frame hook is blocked.",
                pressed ? "press" : "release", g_queue.size());
        return;
    }
    g_queue.push_back(Event{pressed});
}

std::vector<Event> drain()
{
    std::vector<Event> out;
    out.swap(g_queue);
    return out;
}

void quit()
{
    unbind();
    backend::quit();
    g_window = nullptr;
    g_queue.clear();
}

} // namespace Hotkey
