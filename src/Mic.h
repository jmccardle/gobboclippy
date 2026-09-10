#pragma once
#include <string>
#include <vector>

#include <SDL3/SDL.h>

// The microphone, as SDL3 already sees it.
//
// SDL does capture, format conversion and buffering, so this is a thin wrapper
// over one SDL_AudioStream rather than an audio layer: open a recording device
// at a fixed format, hand out the bytes that have queued up, close it again.
// There is no ring buffer and no callback thread here, because SDL_AudioStream
// is already both of those.
//
// read() is called from whichever Python thread is pumping audio into a
// transcriber while the event loop runs on the main thread, and stop() is
// called from the main thread the instant the window hides. SDL_AudioStream is
// internally locked, but the pointer to it is not, so this module keeps its own
// mutex -- without it, hiding the pet mid-read frees the stream underneath the
// reader.
namespace Mic {

// Fixed, not negotiated. This is the stdin contract of tectum's streaming
// worker verbatim, and it is what whisper, silero and the small edge models all
// want; SDL resamples from whatever the hardware actually offers, so pinning it
// here costs nothing and removes a negotiation from every consumer.
constexpr int kRate     = 16000;
constexpr int kChannels = 1;
extern const char* const kFormat;   // "s16le", for anything that must be told

struct Device {
    SDL_AudioDeviceID id;
    std::string       name;
};

// Bring the audio subsystem up, once. A machine with no audio at all fails
// here, and that is not fatal to the application -- only to the microphone.
// SDL_INIT_AUDIO is deliberately not part of the startup SDL_Init for that
// reason.
bool ensure(std::string& error_out);

// The recording devices SDL can see; empty when there are none, including when
// ensure() failed, in which case error_out says why.
std::vector<Device> devices(std::string& error_out);

// device == 0 means the system default. Fails if a recording is already
// running, if there is no recording device, or if the device will not open.
bool start(SDL_AudioDeviceID device, std::string& error_out);

// Closes the device rather than pausing it. A paused device keeps the
// operating system's own microphone-in-use indicator lit, and the rule this
// whole feature is built on is that that indicator and the pet agree.
void stop();

bool active();
int  queued();                    // bytes waiting to be read
int  read(void* buf, int len);    // bytes written, -1 on error

// Release the device and the subsystem. Safe to call when neither came up.
void quit();

} // namespace Mic
