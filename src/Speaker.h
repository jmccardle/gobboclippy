#pragma once
#include <string>
#include <vector>

#include <SDL3/SDL.h>

// The speakers, as SDL3 already sees it. Mic.h's mirror, and deliberately the
// same shape: one SDL_AudioStream, opened at a format, written to, closed.
//
// Two differences from Mic, both of which are the microphone's reasoning run
// backwards rather than an inconsistency:
//
//   * **The format is not pinned.** Mic fixes 16 kHz because every transcriber
//     wants exactly that, so pinning removes a negotiation. A synthesiser's
//     rate is a property of the voice it loaded -- piper's medium voices are
//     22050 Hz, its low ones 16000 -- so the rate is an argument to start().
//     SDL converts to whatever the device actually wants, so accepting the
//     voice's rate costs nothing and asking the script to resample would be
//     asking it to do SDL's job badly.
//
//   * **There is no hidden-window rule.** clippy.mic.start() refuses while the
//     pet is hidden because the visible pet is the recording indicator, and a
//     microphone that might be open unannounced is a privacy question. Sound
//     announces itself: nobody has to be shown that the pet is talking, because
//     they can hear it. So a hidden pet may speak, and that is what makes the
//     chord's whole gesture work -- summon, ask, dismiss, and the answer
//     arrives out loud with nothing left on screen to read.
//
// write() is called from the thread pumping a synthesiser's stdout, and stop()
// from the main thread. Same arrangement as Mic, and the same mutex, for the
// same reason: SDL_AudioStream is internally locked but the pointer to it is
// not, so without this a quit mid-write frees the stream underneath the writer.
namespace Speaker {

// What a synthesiser is expected to emit, and what start() defaults to. Only
// the rate is really negotiable -- s16le mono is what piper's --output_raw
// writes and what every other engine offers.
constexpr int kRate     = 22050;
constexpr int kChannels = 1;
extern const char* const kFormat;   // "s16le"

struct Device {
    SDL_AudioDeviceID id;
    std::string       name;
};

// Bring the audio subsystem up, once. Shares SDL's own refcount with Mic, so
// whichever of the two is asked first pays for it and neither has to know
// about the other.
bool ensure(std::string& error_out);

// The playback devices SDL can see; empty when there are none, including when
// ensure() failed, in which case error_out says why.
std::vector<Device> devices(std::string& error_out);

// device == 0 means the system default. Fails if playback is already open, if
// there is no playback device, or if the device will not open.
//
// The device is held from here until stop(). A synthesiser that has loaded a
// voice is going to be asked to speak again, and reopening per utterance
// trades a held handle for a gap at the start of every sentence.
bool start(int rate, int channels, SDL_AudioDeviceID device,
           std::string& error_out);

// Closes the device, dropping whatever had not been played yet. The abrupt
// version is the only one worth having here: the caller that wants the
// sentence finished can wait for drained() first, and the caller that is
// quitting does not want to.
void stop();

// Drop what has not been played yet and keep the device. This is what
// interrupting a sentence wants, and the difference from stop()+start() is not
// a nicety: reopening a playback device costs 60-100 ms of the thread that
// draws, measured, and interrupting is something the user does mid-gesture.
// SDL keeps the device and the format; only the queue goes.
void clear();

bool active();

// Bytes handed to SDL that the device has not consumed yet, which is as close
// to "how much is left to say" as this layer can answer.
int  queued();

bool drained();                        // active and nothing left queued

// Bytes accepted, or -1 with SDL's error set. Everything or nothing: SDL
// queues what it is given rather than filling a fixed buffer, so a short write
// is not a thing that happens.
int  write(const void* buf, int len);

// Release the device and the subsystem. Safe to call when neither came up.
void quit();

} // namespace Speaker
