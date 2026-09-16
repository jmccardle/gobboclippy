# Roadmap

What is open, and why it is open. `README.md` describes what the program *is*;
this file tracks what it is *not yet*, so the two do not have to be read against
each other to work out which is which.

The rule for what belongs here: an item is on this list if someone could pick it
up. A constraint imposed by something upstream — SDL's Wayland backend, GNOME's
missing tray host — is recorded under **Standing constraints** instead, because
there is nothing to pick up. Anything deliberately cut is under **Cuts**, which
is a list of decisions rather than a list of work, and an entry moving off it is
a change of mind that should be argued for.

Items point at the prose that explains them rather than restating it. The
explanations live in `README.md` and `docs/harvest.md` and should stay there.

**Where this stands.** v0.6.0 is tagged and released. The base is done: window, transparency, tray, the harvested drawing
layer, the effect compositor, the bundled interpreter and a relocatable package
on three platforms; a microphone; a global chord that reaches the pet without
focus; petdex pets in nine states; a control channel and a τ extension that
drives one; a settings window whose fields Python defines. Windows and macOS
build and smoke-test in CI. There are no TODO markers in the tree.

The v0.6.0 release attaches a Windows `.zip`, a macOS `.dmg` and a Linux
`.tar.gz`. That is not only distribution: every item under **Verification** is
waiting on a person with one of those two platforms, and until 0.6.0 there was
nothing for such a person to download.

Two things are not what they look like. **The Forgejo CI does not run, and never
will** — that instance has no runner able to build this and one is not coming,
so every execution fails during setup and always has. The workflow is correct
and costs nothing to keep, but a red run there is expected state rather than
something to investigate: GitHub is the only CI that verifies anything, and the
Debian-interpreter check and the wine test of the cross-compiled Windows build
are done by hand. And **a local `package` stages `assets/` verbatim**, so
untracked pet art lands inside a locally built archive; released artifacts are
built from a clean checkout and are unaffected.

---

## Hearing

`scripts/assistant.py` is a dictation loop today: the global chord summons the
pet and starts recording (a second press stops and dismisses it), the
double-click toggles the microphone on a pet already up, every committed
utterance goes to the agent, and the reply is drawn on the window.

**The pet does not decide whether it was spoken to.** Listening is something the
user turns on, in one of three ways — two of which now exist, the double-click
and the global chord — and that is the whole of the policy.
Speaker identification and turn-taking analysis are out of scope for the base
system — see **Cuts**.

**Global input capture is done, and the hotkey toggle with it.** `src/Hotkey.h`
is the facility and `src/platform/Hotkey*.cpp` the three backends; `Ctrl+Alt+G`
summons the pet and toggles the microphone in `scripts/assistant.py`,
configurable as `hotkey.toggle`. Two findings from building it are worth
keeping, because both contradict what this file used to say:

* **macOS needs no Accessibility permission and no second TCC prompt.** Carbon's
  `RegisterEventHotKey` registers one chord with the window server. The event
  tap that would need Accessibility is only required to observe keys you have
  *not* registered, which is not what a hotkey is.
* **Windows reports the press and not the release.** `RegisterHotKey` posts
  `WM_HOTKEY` and there is no counterpart; X11 and macOS report both. So
  `--capabilities` answers two questions, `hotkey` and `hotkey release`, and the
  second is what push-to-talk has to consult.

The settings window sets the chord by listening for it — the `hotkey` field
type, with Set and Clear. Capture works because the dialog *has keyboard focus*,
so plain SDL key events are enough; it has to *release* the grab while
listening, or the chord already in the box would be the one chord impossible to
re-choose.

**The chord owns the whole gesture, not just the microphone**: summon, record,
stop, dismiss, with the pet kept if it was already up or if the start failed.
That is policy and it lives in `scripts/assistant.py` — `clippy.py` has no
microphone to summon anything for. It is *not* the settings window's preview,
and reaching for one would be a regression: a preview is mapped-but-not-`shown`
and the host refuses to record in that state, correctly, because the visible pet
*is* the recording indicator. `Assistant.summoned` plays
`App::m_preview_engaged`'s part. Both of these replaced wrong assumptions this
file used to carry; the long form is in `.archive/HISTORY.md`.

- [ ] **Push to talk.** Hold to record. The key release *is* the endpoint: it
      replaces the accumulator's committed `final` rather than racing it, so
      under PTT there is nothing to project and no half-sentence to send.
      `scripts/gobbo/accumulate.py` is the seam this lands in.

      Two things stand between here and there, neither of them the endpoint
      logic. **Windows needs a second backend** — a `WH_KEYBOARD_LL` hook, which
      sees every keystroke on the desktop rather than one registered chord,
      needs its own message pump, and is the shape of thing security software
      objects to. Until it exists, `clippy.hotkey.delivers_release()` is False
      there and PTT must refuse with that as the reason rather than invent a
      release from a timer. **And two chords have to be bindable at once**,
      since a toggle and a hold are both useful: `bind()` takes one today, and
      the hook is already handed the chord so that `bind(chord, name)` is an
      added argument rather than a changed signature.
- [ ] **Conversational mode.** The double-click's behaviour, made deliberate:
      the agent listens and answers on pauses until it is switched off. This is
      the mode with the hard problem in it — see the echo note below.
- [ ] **End-of-utterance projection.** Only conversational mode needs it. The
      accumulator commits when the engine's endpointer says `final`, and a
      person pausing mid-sentence is a `final`, so a sentence can reach the
      agent in halves.

**The echo problem, recorded before it is met.** Once the pet has a voice, an
open room microphone hears it and returns its own words as a prompt. Hotkey and
PTT dodge this by construction; conversational mode does not. tectum solved it
in `agent.persona_live` with a half-duplex gate closed after every turn, and its
handset node notes it does *not* need one because a phone cannot hear itself
(`tectum/nodes/agents/edge_asr.py`). A desktop pet is the room case, not the
handset case.

### Transcribers

The contract is fixed and good: raw `s16le` 16 kHz mono on stdin, one JSON
object per line on stdout. Both items below are new programs speaking it, not
branches in `scripts/gobbo/asr.py`.

- [ ] **A self-starting local worker.** The transcriber that works today is
      tectum's `streaming_stt_worker.py`, which is sherpa-onnx and expects a
      hand-assembled venv and a models directory. That is the current cost of
      the first double-click: a config file to write, and an afternoon building
      an ASR environment before it can be written. A whisper or faster-whisper
      worker that installs its own dependencies and fetches its own model on
      first run removes the afternoon. It also loses per-word confidence,
      `unclear` spans and speaker labels — which the accumulator handles, since
      every one of those fields is already optional in `render_tagged`.
- [ ] **FastRTC streaming.** Audio over WebRTC rather than a pipe, so the
      transcriber does not have to be a local subprocess at all. The stdin
      contract stays; what changes is who is on the other end of it.

---

## Speaking

There is no audio output path anywhere. `src/Mic.cpp` is one capture-only
`SDL_AudioStream`, and the pet answers by drawing text. This is structurally the
mirror of the microphone — a fixed format, the host owning the device, and a
subprocess for the synthesiser so it can be swapped the way the transcriber can.

The reference implementation is tectum's `effector.speech`
(`tectum/nodes/effectors/speech.py`), which is worth reading before writing
this. What it already establishes:

- [ ] **Piper as the synthesiser.** `piper --model <voice>.onnx --output_raw`,
      text on stdin, raw PCM on stdout at the sample rate named in the voice's
      own JSON (22050 for `amy-medium`). Play it, or in our case hand it to SDL.
- [ ] **A named voice, with no default.** `edge_asr.py` records what the
      alternative costs: omitting the voice means "whichever one is selected",
      which is only an answer when one *is* — otherwise the far end refuses the
      utterance and the stack cannot see that it did. Same rule as the
      transcriber: print the config to write. Note the multi-speaker case, where
      an omitted `speaker_id` means 0 rather than "unspecified".
- [ ] **An unspeakable reply is refused, not truncated.** tectum's limit is 320
      characters, about twenty seconds, and text over it comes back to the agent
      with the reason and an instruction to rewrite it as one speakable
      sentence. A prefix that sounds finished is a lie, which is the same rule
      `assistant.py` already applies to a `length`-stopped answer on screen.
- [ ] **DSP colouring is optional and last.** tectum's `radio` preset is a
      character choice, not a requirement.

**One thing not to copy.** `effector.speech` selects a backend by what is
installed — robot, then piper, then espeak, then a null that records what it
would have said. That chain is right for a substrate that must keep running
unattended and wrong here: gobboclippy's precedent is the transcriber, which has
no default because a wrong guess is a pet that never answers instead of a
sentence saying what to fix.

---

## Remembering

τ owns conversational state within a session. Neither of these is required for
the assistant to work, and both are `gobbo/` τ extensions rather than anything
in the host.

- [ ] **A wiki the agent maintains.** Tagged text files and nothing else — the
      whole point is that it stays readable and greppable by a person with no
      service running. The extension's job is to make the *writing* consistent
      enough that plain search works: a fixed front-matter shape, a tag
      vocabulary that does not drift, one file per thing. Tools for maintenance
      as well as recall, because a memory that only ever grows stops being one —
      forgetting and consolidating are operations the agent should be able to
      perform on its own notes.
- [ ] **JMFTS as an optional retrieval backend.** `pip install jmfts-client`
      (httpx and pydantic, nothing heavier) against an appliance at a URL, for
      hybrid vector/BM25/late-interaction search over the same notes. Optional
      in the real sense: the wiki must not need it, and the tools the agent sees
      should not change shape when it is absent.

---

## Configuration

The window exists. It is a second SDL window drawn with Dear ImGui, opened from
the tray's **Configure...**, and it renders a schema it does not understand —
the host knows what an int field is and what OK/Cancel/Apply mean, and
`scripts/gobbo/settings.py` knows what any of it means. Adding a setting is a
dict in that file. See `README.md` "Settings".

The question of what it is made of is answered and the reasoning is in
`CMakeLists.txt` beside the dependency: tkinter would mean shipping Tcl/Tk on
three platforms, the harvested drawing layer has none of the five things a
dialog is mostly made of, and a desktop pet should not open a browser tab to
move itself.

Two tabs exist: `WindowSection` (geometry, with a live preview) and
`HotkeySection` (the chord, captured by pressing it). The second one is
registered by `scripts/assistant.py` rather than living in `SECTIONS`, because
`scripts/clippy.py` opens the same window and has no microphone for a chord to
toggle.

What is left is the rest of the settings. Each is a `Section` subclass, and
none of them needs C++ — the hotkey field did, since a chord is a widget rather
than a value you type, but every item below is a text box or a list.

- [ ] **The transcriber and agent commands.** The two that currently have no
      default and raise with the JSON to write — see `gobbo/config.py`. A text
      field is not enough on its own: the useful version validates that the
      command exists before saving, because the failure otherwise arrives at the
      first double-click rather than at the dialog.
- [ ] **Choosing a pet.** The petdex catalogue, installed from the window rather
      than from `scripts/pet_demo.py`. Needs the network, and therefore needs a
      progress state and somewhere for the failure to appear.
- [ ] **The startup script.** Which of `scripts/` runs, which today is
      `--script` on a command line the user of a packaged build does not have.
- [ ] **The voice**, once **Speaking** exists, and **push to talk's binding**,
      once that exists.
- [ ] **`--size` against a saved size.** The flag loses to the config file, and
      also lost to the script's own hardcoded size before any of this — neither
      the host nor the script can currently tell whether it was passed. Small,
      pre-existing, and worth fixing when a second flag wants the same answer.

---

## Verification

Implemented, shipped, and never run. Each of these is a branch CI cannot reach,
so the check is a person on the platform rather than a test.

- [ ] **The Windows control channel.** A different transport, not a port: CPython
      exposes no `AF_UNIX` on Windows, so it is loopback TCP plus a token. That
      branch has never executed. See `README.md` "Status".
- [ ] **The macOS control channel.** Same code path as Linux, unverified.
- [ ] **The macOS microphone prompt.** CI checks the `.app` is *shaped* to ask —
      `NSMicrophoneUsageDescription` in the `Info.plist` of a signed bundle,
      which is why the bundle exists at all. No runner has an input device or a
      person to answer the TCC prompt. See `docs/macos.md`.
- [ ] **Capture off Linux.** What CI proves on Windows and macOS is the refusals
      (hidden-pet, close-on-hide, no device), because no runner has an audio
      stack. The capture path itself is verified only on this desktop.
- [ ] **Pets off Linux.** The WebP decode and its failure are in the smoke test
      on all three platforms. Installing a pet needs the network and driving one
      needs a window; runners have neither.
- [ ] **The hotkey actually firing, off Linux.** Binding, normalising, the
      refusals and the grab are all in the smoke test and run on all three
      platforms. Pressing the key is not and cannot be: a global grab is global,
      so it needs a focused desktop with a keyboard, and no runner has one.
      Locally it is verified with `xdotool`, including the held key, the
      modifiers being released first, and both lock modifiers. On macOS the two
      things to check first are that `kEventHotKeyReleased` arrives at all and
      that no permission dialog appears.

---

## Standing constraints

Not work. Recorded so that rediscovering them costs nothing.

- **Wayland.** SDL's Wayland backend has no `SetWindowAlwaysOnTop` hook and
  `SDL_SetWindowAlwaysOnTop` returns success anyway, so the flag reads back set
  while nothing happened — `src/Capabilities.cpp` reports against the video
  driver rather than trusting SDL. `SDL_SetWindowPosition` fails outright, and a
  desktop pet that cannot place itself is not one. **Nor can a Wayland client
  grab a global key** — `keyboard-shortcuts-inhibit` needs keyboard focus,
  which is the thing a pet never has — so `clippy.hotkey.available()` is False
  there and says why. X11/XWayland is the supported Linux path.
- **The GNOME tray.** SDL talks to libayatana-appindicator over D-Bus; vanilla
  GNOME Shell hosts no StatusNotifierItem, so `SDL_CreateTray()` succeeds and no
  icon appears. Not detectable from SDL. GNOME users need the AppIndicator
  extension.
- **macOS is not buildable here.** Every macOS change is verified only after it
  is pushed.
- **wine is not Windows.** The cross-compiled build under wine exercises the
  bundled interpreter, the stdlib zip and the `.pyd` modules, which is more than
  a link check and less than proof. Requires wine 10+. See
  `docs/cross-compile.md`.

---

## Cuts

Decisions, not a backlog. `README.md` "Deliberately not implemented" and
`docs/harvest.md` "Still not here" carry the arguments for the first three.

- **Speaker identification.** The worker can emit a speaker label and the
  accumulator already carries it through — `'?'` is UNASSIGNED and is never
  inherited from the turn before it. Nothing acts on it, and nothing in the base
  system should. Listening is a thing the user switched on.
- **Turn-taking and conversation analysis.** Too far for a desktop pet. What
  replaces it is the user's hand: a hotkey, a held key, or a double-click.
- **Barge-in, as a thing to build.** Already settled.
  `scripts/gobbo/tau.py` sets the protocol's multitask policy to `enqueue`
  rather than its default `reject` — by the time this code knows a second
  utterance exists it has already been spoken, so refusing it would drop words.
  `steer` is a config key away for anyone who wants an interruption to fold into
  the running turn.
- **Click-through and per-drawable hit testing.** The window is a rectangle that
  swallows clicks across its whole area, transparent corners included. Shaped
  input regions are where the hover/drag/click state machine gets genuinely hard
  across platforms. A click reaches the script as "the pet was clicked" with
  window coordinates; McRogueFace's `click_at` and hover dispatch stay cut.
- **Speech bubbles.** A `Caption` over a transparent window sits on the user's
  wallpaper, so there is no background to pick a readable ink against;
  `scripts/clippy.py` and `scripts/assistant.py` draw their text twice, offset,
  as a drop shadow. A real bubble wants a nine-slice `Frame` — `UIFrame` is the
  next thing to harvest if it is wanted.
- **A second window drawn by the harvested layer.** Uncut in one direction
  only: the settings dialog is a second window, and Dear ImGui draws it,
  because a dialog is mostly the five things the harvest left out. `Stage` is
  still a singleton and still because there is one stage. A *pet* drawn into
  two windows remains cut.

---

## Harvest leftovers

From `docs/harvest.md` "Also worth stealing". Small, and not blocking anything.

- [ ] **`PathProvider.cpp`** from McRogueFace.
- [ ] **Re-resolution on display change.** `clippy.display_bounds()` reports the
      work area of the display the pet is on, which already earned itself — this
      desktop's usable height is 1052, not 1080. The other half, re-placing when
      the display set changes, is not there.
