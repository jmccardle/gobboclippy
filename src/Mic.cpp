#include "Mic.h"

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

namespace Mic {

const char* const kFormat = "s16le";

bool ensure(std::string& error_out)
{
    if (g_audio_up) return true;
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
    SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
    if (!ids) {
        error_out = std::string("SDL_GetAudioRecordingDevices: ") + SDL_GetError();
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

bool start(SDL_AudioDeviceID device, std::string& error_out)
{
    if (active()) {
        error_out = "the microphone is already recording";
        return false;
    }
    if (!ensure(error_out)) return false;

    // Opening the default recording device on a machine that has none does not
    // fail -- it hands back a stream that never delivers a sample. Ask what
    // exists first, so "there is no microphone" is an error where it was asked
    // for rather than a silence nobody can explain.
    std::string probe_err;
    if (devices(probe_err).empty()) {
        const char* driver = SDL_GetCurrentAudioDriver();
        error_out = "no recording device (audio driver '" +
                    std::string(driver ? driver : "none") + "')";
        if (!probe_err.empty()) error_out += ": " + probe_err;
        return false;
    }

    SDL_AudioSpec spec;
    spec.format   = SDL_AUDIO_S16LE;
    spec.channels = kChannels;
    spec.freq     = kRate;

    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(
        device ? device : SDL_AUDIO_DEVICE_DEFAULT_RECORDING,
        &spec, nullptr, nullptr);
    if (!s) {
        error_out = std::string("SDL_OpenAudioDeviceStream: ") + SDL_GetError();
        return false;
    }

    // Devices opened this way start paused, by SDL's design. Nothing reaches
    // the stream until it is resumed, so this is the line that begins the
    // recording.
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
    // device it opened alongside it.
    if (s) SDL_DestroyAudioStream(s);
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
    const int n = g_stream ? SDL_GetAudioStreamAvailable(g_stream) : 0;
    SDL_UnlockMutex(lock());
    return n < 0 ? 0 : n;
}

int read(void* buf, int len)
{
    if (len <= 0) return 0;
    SDL_LockMutex(lock());
    const int n = g_stream ? SDL_GetAudioStreamData(g_stream, buf, len) : 0;
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

} // namespace Mic
