#include "Speaker.h"

namespace {

SDL_AudioStream* g_stream   = nullptr;
bool             g_audio_up = false;

// Guards g_stream only. Function-local static so it exists before any caller
// can reach it, without an init order to get wrong.
SDL_Mutex* lock()
{
    static SDL_Mutex* m = SDL_CreateMutex();
    return m;
}

} // namespace

namespace Speaker {

const char* const kFormat = "s16le";

bool ensure(std::string& error_out)
{
    if (g_audio_up) return true;
    // SDL refcounts subsystem init, so this and Mic::ensure() can both hold
    // SDL_INIT_AUDIO without either knowing the other exists. Each keeps its
    // own bool so each releases exactly what it took.
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error_out = std::string("audio subsystem unavailable: ") + SDL_GetError();
        return false;
    }
    g_audio_up = true;
    return true;
}

std::vector<Device> devices(std::string& error_out)
{
    std::vector<Device> out;
    if (!ensure(error_out)) return out;

    int count = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
    if (!ids) {
        error_out = std::string("SDL_GetAudioPlaybackDevices: ") + SDL_GetError();
        return out;
    }
    out.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        out.push_back({ids[i], name ? name : "(unnamed)"});
    }
    SDL_free(ids);
    return out;
}

bool start(int rate, int channels, SDL_AudioDeviceID device,
           std::string& error_out)
{
    if (active()) {
        error_out = "the speaker is already open";
        return false;
    }
    if (rate <= 0 || channels <= 0) {
        error_out = "rate and channels must both be positive";
        return false;
    }
    if (!ensure(error_out)) return false;

    // Same trap as the microphone: opening the default playback device on a
    // machine that has none succeeds and hands back a stream nothing ever
    // plays. Ask what exists, so "there are no speakers" is an error where it
    // was asked for rather than a silence nobody can explain.
    std::string probe_err;
    if (devices(probe_err).empty()) {
        const char* driver = SDL_GetCurrentAudioDriver();
        error_out = "no playback device (audio driver '" +
                    std::string(driver ? driver : "none") + "')";
        if (!probe_err.empty()) error_out += ": " + probe_err;
        return false;
    }

    // This spec describes what *we* will write, not what the device wants.
    // SDL converts between the two, which is the whole reason a voice's own
    // sample rate can be handed over as-is.
    SDL_AudioSpec spec;
    spec.format   = SDL_AUDIO_S16LE;
    spec.channels = channels;
    spec.freq     = rate;

    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(
        device ? device : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
        &spec, nullptr, nullptr);
    if (!s) {
        error_out = std::string("SDL_OpenAudioDeviceStream: ") + SDL_GetError();
        return false;
    }

    // Devices opened this way start paused, by SDL's design. Resuming an empty
    // playback stream plays nothing -- it only means that what is written from
    // here on is heard, rather than piling up behind a paused device.
    if (!SDL_ResumeAudioStreamDevice(s)) {
        error_out = std::string("SDL_ResumeAudioStreamDevice: ") + SDL_GetError();
        SDL_DestroyAudioStream(s);
        return false;
    }

    SDL_LockMutex(lock());
    g_stream = s;
    SDL_UnlockMutex(lock());
    return true;
}

void stop()
{
    SDL_LockMutex(lock());
    SDL_AudioStream* s = g_stream;
    g_stream = nullptr;
    SDL_UnlockMutex(lock());

    // Destroying a stream opened by SDL_OpenAudioDeviceStream also closes the
    // device it opened alongside it, and discards what was still queued.
    if (s) SDL_DestroyAudioStream(s);
}

void clear()
{
    SDL_LockMutex(lock());
    if (g_stream) SDL_ClearAudioStream(g_stream);
    SDL_UnlockMutex(lock());
}

bool active()
{
    SDL_LockMutex(lock());
    const bool a = g_stream != nullptr;
    SDL_UnlockMutex(lock());
    return a;
}

int queued()
{
    SDL_LockMutex(lock());
    const int n = g_stream ? SDL_GetAudioStreamQueued(g_stream) : 0;
    SDL_UnlockMutex(lock());
    return n < 0 ? 0 : n;
}

bool drained()
{
    SDL_LockMutex(lock());
    const bool d = g_stream && SDL_GetAudioStreamQueued(g_stream) <= 0;
    SDL_UnlockMutex(lock());
    return d;
}

int write(const void* buf, int len)
{
    if (len <= 0) return 0;
    SDL_LockMutex(lock());
    int n = 0;
    if (g_stream) {
        n = SDL_PutAudioStreamData(g_stream, buf, len) ? len : -1;
    }
    SDL_UnlockMutex(lock());
    return n;
}

void quit()
{
    stop();
    if (g_audio_up) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_up = false;
    }
}

} // namespace Speaker
