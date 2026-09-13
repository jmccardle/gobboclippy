# gobboclippy

A desktop pet host: **embedded CPython inside a small C++/SDL3 program**.
The C++ side owns the window, the tray icon and the event loop. Everything
above that is Python, and the whole thing ships as a relocatable directory
you unzip and run.

This is the *canonical base* — the platform, build and distribution layer —
plus the sprite, animation and text subset harvested from
[McRogueFace](https://github.com/jmccardle/McRogueFace) on top of it — plus a
microphone, a listening assistant built on subprocesses that speak JSON lines,
and a socket other processes can drive the pet through — including an agent.
Speech bubbles, per-drawable hit testing and a config window are deliberately
still absent; see [docs/harvest.md](docs/harvest.md) for what was taken, what
was cut, and why.

```
      ╭───────────────╮
      │  scripts/*.py │   behaviour, policy, stdlib, your libraries
      ├───────────────┤
      │  clippy       │   extension module (src/PyClippy.cpp, src/PyDraw.cpp)
      ├───────────────┤
      │  drawing      │   Texture, Font, Sprite, Caption, Animation, Easing
      ├───────────────┤
      │  C++ host     │   window flags, tray, event loop, paths, microphone
      ├───────────────┤
      │  SDL3         │   one dependency, four platforms
      ╰───────────────╯
```

The drawing layer knows nothing about Python — only `PyDraw.cpp` includes
`Python.h` — so it is a library the host happens to script, not a Python
program.

## Why this shape

The alternative was a frozen Python environment (PyInstaller + PySide6 or
tkinter). The measurement that settled it: **the size difference between those
options is Qt, not the language.** CPython is ~10 MB either way. Dropping Qt
for SDL3 is what gets the package under 20 MB, and SDL3 is also the only
option with the full window capability set plus a native tray on all three
desktop platforms.

## Platform support

Determined from SDL 3.4.16's own sources (`src/video/*/`, `src/tray/*/`), not
assumed. The running binary re-checks and reports the real answer:

```
$ ./gobboclippy --capabilities
gobboclippy 0.5.0  (SDL 3.4.16, Python 3.14)
  platform      : Linux
  video driver  : x11
  borderless    : yes
  always on top : yes
  transparent   : yes
  skip taskbar  : yes
  tray icon     : yes
  microphone    : yes
  image formats : png, webp
```

| | Windows | macOS | X11 | Wayland |
|---|---|---|---|---|
| borderless | yes | yes | yes | yes |
| per-pixel transparency | yes | yes | yes | yes |
| always on top | yes | yes | yes | **no** |
| skip taskbar | yes | yes | yes | yes |
| tray icon | yes | yes | GTK3 + appindicator | GTK3 + appindicator |

Two caveats worth knowing before you file a bug:

**Wayland's `xdg-shell` has no always-on-top, and no positioning either.**
SDL's Wayland backend registers no always-on-top hook at all, yet
`SDL_SetWindowAlwaysOnTop` still returns success and the flag reads back as
set — so `Capabilities` cross-checks the video driver rather than trusting
either. `SDL_SetWindowPosition` fails outright, which matters more for a pet:
it cannot place itself. This is the same ceiling Electron hits.

It is not a security restriction and it is not unsolvable — `wlr-layer-shell`
is the unprivileged protocol that panels and docks use, SDL supports handing
it a roleless surface, and it maps onto a pet cleanly. It just does not exist
on GNOME. **X11/XWayland is the supported Linux path**, and gets the full
capability set on every desktop including GNOME and KDE under Wayland. See
[docs/wayland.md](docs/wayland.md) for the route if that changes.

**The Linux tray dlopens GTK3 and libayatana-appindicator3 at runtime.** There
is no build-time dependency, but a machine without them gets no tray. Because
the window is borderless with no close button, the app treats a missing tray
as fatal rather than starting unreachable:

```
sudo apt install libgtk-3-0 libayatana-appindicator3-1
```

## Deliberately not implemented

**Click-through, and per-drawable hit testing.** The window is a rectangle that
swallows clicks over its whole area, transparent corners included. Shaped input
regions (`wl_surface.set_input_region` and friends) are where the hover/drag/
click state machine gets genuinely hard, and skipping them removes the worst
cross-platform cliff. Transparency makes it *look* like a paperclip; it is
still a rectangle. So a click reaches the script as "the pet was clicked", with
window coordinates — McRogueFace's `click_at` / hover dispatch is still cut, and
drawables have no hit testing.

**Speech bubbles.** A `Caption` on a transparent window sits on whatever the
user's wallpaper happens to be, so there is no background colour to pick a
readable ink against; `scripts/clippy.py` draws its text twice, offset, as a
drop shadow. A real bubble wants a nine-slice `Frame`, which is the next thing
to harvest if it is wanted.

## Build

Needs CMake ≥ 3.21, a C++17 compiler, and Python ≥ 3.10 with development
headers — 3.10 through 3.14 are supported, and a native build bundles whichever
one it finds. Released packages ship 3.14; that version is pinned in CI, not
here. SDL3 and stb are fetched and pinned automatically.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/gobboclippy
```

Linux also needs the SDL3 build dependencies, or you get a binary with no X11
or Wayland backend:

```sh
sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxi-dev libxfixes-dev libxss-dev libxkbcommon-dev libwayland-dev \
  wayland-protocols libdecor-0-dev libgtk-3-dev libayatana-appindicator3-dev
```

Building against a local SDL checkout instead of the fetched one:

```sh
cmake -B build -DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL
```

## Package

```sh
cmake --build build --target package
```

Produces a relocatable directory and an archive:

```
gobboclippy-0.5.0-Linux/
  gobboclippy            1.6 MB
  python3 -> gobboclippy          the same binary, dispatched on argv[0]
  libSDL3.so.0           3.4 MB
  assets/                SVG sources + rendered PNGs + JetBrains Mono
  scripts/               clippy.py, assistant.py, pet_demo.py, pet_ctl.py, gobbo/
  licenses/              notices for everything redistributed here
  lib/
    libpython3.14.so.1.0 5.8 MB   the SONAME, not the linker name
    python314.zip        2.7 MB   stdlib, test suites excluded
    python3.14/lib-dynload/        stdlib C extensions
    pip-26.2.1-py3-none-any.whl    pip, importable straight from the wheel
  site/                            sys.prefix; pip installs under here
```

**23 MB on disk, 11 MB compressed**, measured from the release artifact rather
than a local build — the two disagree about which pip ships, and a local
`libpython` is whatever the build machine's distro made. The binary's RUNPATH is
`$ORIGIN:$ORIGIN/lib`, and every runtime path is resolved from
`SDL_GetBasePath()`, so the directory can be moved anywhere. Verified by
running it from a different filesystem with `env -i`.

Both shipped libraries are staged under their SONAME — `libSDL3.so.0`,
`libpython3.14.so.1.0` — because that is the name the loader searches for, and
a package that gets it wrong does not fail. It falls through to the host's copy
and works on every machine that has one, which is every machine that builds or
tests it. CI relocates the package, clears `LD_LIBRARY_PATH` and asserts that
`ldd` resolves both inside the tree, because running the binary cannot tell the
difference.

Packaging strips every binary in the staging tree, and this is not cosmetic:
the interpreter is somebody else's build, and whether it arrives stripped is
not something the project gets to assume. Debian's is. The one
`actions/setup-python` installs is not — 23 MB of `libpython` and 16 MB of
`lib-dynload`, which took the first Linux package CI ever produced to 20 MB
compressed against 8.0 MB for the identical commit built locally. Stripping in
the staging tree is what makes the two agree. Nothing outside the staging tree
is ever touched; the source of those copies is the machine's real Python
installation.

Three naming details that are easy to get wrong and fail silently:

- the stdlib zip must be `python314.zip`, **no dot** — that is the only name
  CPython looks for on `sys.path`
- ship `libSDL3.so.0` (the SONAME), not `libSDL3.so.0.4.16`
- `lib-dynload/` must be shipped separately; `.so` modules cannot be imported
  from inside the zip

## Scripting

`scripts/clippy.py` is the entry point. The full standard library is
available, plus anything you drop beside it.

The shipped one is a tech demonstrator for the drawing layer, not a
personality: a paperclip composed from parts that blinks, sways, breathes,
squashes and stretches, lifts its eyes and eyebrows, slides in, and cycles
captions that fade. It exercises every piece described below, and it is meant
to be replaced.

`scripts/smoke_test.py` is the non-interactive check — assertions over the host
API and the drawing layer, including that a parent's scale does not move or
resize its children, and that a hidden pet refuses to record:

```sh
./gobboclippy --script scripts/smoke_test.py     # non-zero on any failure
```

### The assistant

`scripts/assistant.py` is the one that listens. Double-click the pet to toggle
the microphone; what it hears goes to a transcriber, committed utterances go to
an agent, and the answer is drawn on the window.

```sh
./gobboclippy --script scripts/assistant.py
```

Both of those are **subprocesses speaking one line of JSON at a time**, because
that is what they already are, and neither is imported:

- the transcriber reads raw `s16le` 16 kHz mono on stdin and writes
  `{"type": "ready"|"partial"|"final", ...}` on stdout. That is the contract of
  [tectum](https://github.com/jmccardle/tectum)'s `streaming_stt_worker.py`
  verbatim, so that worker runs here unchanged — and a different engine, local
  or over the network, is a different program rather than a branch in
  `scripts/gobbo/asr.py`.
- the agent is `tau --mode rpc`, JSON-RPC 2.0 over stdio.

Nothing heavy enters the pet's interpreter, and either end can be replaced by
anything that speaks the same lines.

Name the transcriber in `config.json` under `clippy.pref_path()`. There is no
default for it: a guess that happened to be wrong would be a microphone that
never answers rather than a sentence saying what to fix, so the first
double-click prints the file to write and the JSON to put in it.

```json
{
  "asr": {
    "command": ["/path/to/asr-venv/bin/python",
                "/path/to/tectum/tectum/audio/streaming_stt_worker.py"]
  }
}
```

`tau` does have a default — `sys.executable -m tau_coding_agent.cli --mode rpc`,
which names no path outside the tree because the package ships its own
interpreter and `pip` installs into that interpreter's `site/`. Override it,
and the model, under a `"tau"` key.

`scripts/gobbo/accumulate.py` is the seam between hearing and answering: the
agent is fed committed utterances, not a raw transcript stream. It is thin
today and exists for what goes there next — a wake word, a name resolver, an
end-of-utterance projection.

### Pets

[petdex.dev](https://petdex.dev) publishes several thousand animated pets as
plain HTTPS assets — a sprite sheet and a little metadata each, no API key and
nothing to run. `scripts/pet_demo.py` downloads one and puts it on the desktop:

```sh
./gobboclippy --script scripts/pet_demo.py      # double-click to change emote
```

A sheet is a grid of 192×208 cells, eight to a row, one row per animation
state, which is exactly the atlas model `clippy.Texture` already has. So a
state's frames are a contiguous run of `sprite_index` and playing one is the
frame-sequence animation from [the stage](#the-stage) — there is no pet-shaped
code in the drawing layer, and none was added.

```python
from gobbo import pet, petdex

petdex.search("otter")           # the catalogue, cached after the first call
petdex.install("boba")           # -> clippy.pref_path()/pets/boba/
p = pet.Pet("boba")              # a clippy.Sprite, under p.sprite
p.play("waving")                 # loops
p.play_once("jumping")           # once, then back to idle
```

**No pet is bundled, and none ever will be.** They are user-submitted fan art
and petdex claims no rights to the underlying IP, so this repository ships the
ability to read the format and the user downloads the art. `assets/` stays ours.

The nine state names — `idle`, `running-right`, `running-left`, `waving`,
`jumping`, `failed`, `waiting`, `running`, `review` — are the same for every
pet, which is what makes them worth building on. They live in
`scripts/gobbo/states.py`, which imports nothing, because τ's interpreter needs
them too and has no `clippy` to import.

They are *not* shipped with a pet. petdex's `pet.json` carries a name, a
description and a file name, and says nothing about rows, frame counts or
timing; that table is transcribed from petdex's own site source. The frame
counts matter: rows are padded to eight cells with transparent frames, so a
four-frame wave played across all eight columns spends half its loop invisible.
See [docs/petdex.md](docs/petdex.md) for the format in full.

Reading WebP is why libwebp is linked — 97% of the corpus is WebP and stb_image
decodes none of it. `--capabilities` reports which decoders a build has.

### Settings

The tray's **Configure...** asks the script, rather than doing anything itself:
it fires the `configure` hook, and a script with no handler gets a line in the
log saying so rather than a menu entry that silently does nothing. Both shipped
scripts register one.

The host draws the window and owns OK / Cancel / Apply. It does not know what
any setting *is* — it renders a list of field dicts and hands the edited values
back, so adding a setting is a dict in `scripts/gobbo/settings.py` and no change
to the C++ at all.

```python
clippy.settings_open({
    "title": "gobboclippy settings",
    "fields": [
        {"key": "window.x", "label": "Position X", "tab": "Window",
         "type": "int", "value": 1612, "min": -32768, "max": 32768,
         "live": True, "help": "Pixels from the left edge."},
    ],
    "on_change": lambda key, value: ...,   # a live field was edited
    "on_apply": lambda values: None,       # None, or a message to show
    "on_cancel": lambda: ...,              # put back what on_change did
})
```

`type` is `int`, `range`, `text`, `bool` or `choice`.

A **`range`** is an int that has to have bounds, drawn as a slider with the
number in a box beside it — the slider is the coarse control, the box is the
exact one, and the mouse wheel over either is the fine one, one unit per notch
and ten with Shift. The wheel is claimed for the hovered field, so the list
underneath does not scroll out from under the value you were aiming at. A range
without `min` and `max`, or with `min >= max`, is refused: a slider needs two
ends, and unbounded it would silently become a plain box. Plain `int` stays a
box with steppers, because plenty of bounded integers — a port, a sample rate —
would be absurd as a slider.

A **`choice`** carries `choices` and crosses the boundary as the chosen string,
never as an index, so reordering the list cannot silently change what a config
file means. An unknown type is refused at the door rather than drawn as
something else.

`settings_set()` moves a field the user did not touch, which is what a linked
pair needs. It deliberately does **not** fire `on_change` — the host was *told*
this value, nobody *edited* it — and without that distinction a width adjusting
a height adjusting a width would not terminate. It still counts towards dirty,
because it is still a change someone will want saved.

`on_apply` returns `None` on success, or a string to show in the dialog without
closing it; a handler that raises is the same answer with the exception for its
text, and the traceback goes to stderr. A failed write must never look like a
successful one. It is called only when something actually changed, so **OK** and
**Apply** write nothing when nothing was edited, and **Apply** is disabled until
there is something to apply.

`scripts/gobbo/settings.py` is the policy half. It re-reads the config file
every time the window opens — the dialog is a view of what is on disk, not of
what this process last remembered — and on apply it re-reads again, replaces
only the keys its sections own, and writes through a temporary file in the same
directory. A transcriber command this dialog has never heard of survives it.

A **section** is the unit of extension: a class that says which fields it
contributes, what a live edit does right now, and how to fold the values back
into the config. `WindowSection` is the first one, and writes:

```json
{ "window": { "x": 1612, "y": 580, "width": 300, "height": 380,
              "fixed_ratio": false } }
```

Position runs from 0 to the far edge of the work area, so the top of the slider
really does park the pet entirely off the screen — that is a position somebody
may want, and a slider stopping at "still fully visible" would be deciding
otherwise for them. The cost is that a pet on a monitor to the *left* of the
primary needs a negative x and the slider does not go there; that case wants a
display picker rather than a wider slider. Size runs 64–512, deliberately
narrower than `--size`'s 32–2048, because a slider spanning the wider range
would put every useful size a pixel apart.

**Fixed ratio** locks width against height at whatever proportion they are in
when it is ticked. Dragging either moves the other, through `settings_set()`.
At the end of a scale the pair stops rather than the follower running off it —
the proportion is not held there, but it is not forgotten either, so coming back
off the stop restores it. The follower is what gets clamped, never the slider
being dragged: writing back into a slider the user is holding would leave the
window one size and the dialog showing another, because a slider rewrites itself
from the mouse every frame.

Both shipped scripts apply that at startup and fall back to their own placement
when the file says nothing — a default for somebody who has never expressed a
preference, which is not the same as a fallback around an error. A config file
that exists and does not parse still raises.

#### The geometry preview

A window position you cannot see is a number you are guessing at, so editing one
puts the pet on screen. That is not the same as showing it, and the difference
is a third state:

| | pet window | `visible()` | `previewing()` |
|---|---|---|---|
| hidden | unmapped | `False` | `False` |
| geometry edited while hidden | mapped | `False` | `True` |
| **Show** pressed | mapped | `True` | `False` |
| **Hide** during the session | mapped | `False` | `True` |
| dialog closed | follows `visible()` | | `False` |

While previewing, the dialog carries a banner above the buttons saying the pet
will go back to hidden when the dialog closes, with a **Show** button that makes
it mean what it looks like it means. The tray's Hide becomes **Hide (preview is
on)**, because during a settings session with geometry in play it demotes the
window to a preview rather than taking it off the screen.

This is deliberately independent of OK / Cancel. Cancel puts the *geometry*
back; it does not un-show a pet the user pressed Show on. Repositioning a hidden
pet, showing it, and then cancelling leaves it shown, where it was before.

### The control channel

A script can let other processes drive the pet. One JSON object per line over a
socket, the same shape as the transcriber and the agent:

```sh
./gobboclippy --script scripts/pet_demo.py &
./gobboclippy --python scripts/pet_ctl.py play waving
./gobboclippy --python scripts/pet_ctl.py state
./gobboclippy --python scripts/pet_ctl.py say "back in a minute"
```

**The verbs are the script's, not the channel's.** `scripts/gobbo/control.py`
owns the transport and knows nothing about pets; `pet_demo.py` registers `play`,
`say`, `state`, `show` and `hide`. An unknown verb answers with the list of ones
that host does serve.

A request never runs on the socket thread. It goes onto the same queue the
transcriber and the agent already use, the frame hook runs it, and the socket
thread waits for the answer — so [the rule](#the-microphone) that only the frame
hook touches the drawing layer holds for every verb without a script having to
think about it.

The endpoint is `control.json` in `clippy.pref_path()`. On POSIX it names a unix
socket at mode 0600, and the filesystem is the access control. On Windows
CPython exposes no `AF_UNIX`, so it is a loopback TCP port guarded by a random
token — any local process can reach a loopback port, which is what the token is
for. Clients read the file either way, so client code is one path on both.

### The pet, from an agent

`scripts/gobbo/gobbopet.py` is a [τ](https://github.com/jmccardle/agent-harness-py)
extension. It runs inside τ's interpreter, not this one, and reaches the pet
through the control channel like any other client:

```sh
tau -e scripts/gobbo/gobbopet.py \
    --ext-config gobbopet.endpoint="$HOME/.local/share/gobboclippy/control.json"
```

It registers two halves that are deliberately independent, because which one
you want is a policy question the extension refuses to answer:

- **tools** — `pet_play`, `pet_show`, `pet_hide`, `pet_say`, `pet_info` — so a
  model can move the pet on purpose, with a closed enum of the nine states;
- **a turn-end emote**, so the pet reacts with no model involvement at all.
  `scripts/gobbo/emote.py` classifies what the turn *did* — tools errored,
  files written, how many round trips — rather than reading the prose for a
  mood.

Both are on by default and they do not fight: the emote stands down when the
model already moved the pet that turn, which it detects by looking for its own
tool names in the transcript rather than by keeping a flag there is no right
moment to clear. `--ext-config gobbopet.mode=tools|emote|both` picks.

The hook is `user_turn_end`, which fires once per utterance — `turn_end` fires
once per LLM completion, so an answer that took six tool round trips would emote
six times.

`pet_info` hands the model the pet's own description **fenced and labelled as
untrusted**: those are stranger-written strings from a public gallery, and they
are data rather than instructions.

### The interpreter

The same binary is also a Python interpreter, and the package ships it under
the name one expects:

```sh
./python3 -m pip install ffwf-tau-agent-core    # into site/, no --target needed
./python3 -c 'import tau_agent_core'            # the same runtime the pet uses
./gobboclippy --python -m pip list              # identical; --python must come first
./python3                                       # the REPL
```

`python3` is a symlink to `gobboclippy` (`python.exe` is a copy, on Windows).
The dispatch is on `argv[0]`, so it is one code path with `--python`; the
alias exists so that `sys.executable` names something a subprocess can run as
python — pip's build isolation, `multiprocessing`, anything that spawns
`[sys.executable, "-c", ...]`. Nothing in this mode touches SDL video: a pip
install runs on a machine with no display.

**`clippy` is importable here, and has no window behind it.** That is what
makes `scripts/pet_ctl.py` possible: a client needs `clippy.pref_path()` to
find the running pet's socket, and computing that path any other way means
hardcoding a platform convention. Everything that needs a window —
`set_size`, `show`, `Texture`, `Font.measure` — raises and says to use
`--script` instead. Running a pet script under `--python` is the easy mistake,
so the error names the fix rather than guessing at a startup race.

What `pip install` puts in `site/` is what `--script` can import. That works
because `sys.prefix` is `site/` and `sys.base_prefix` is the package root —
CPython's own model of a venv — and it is done that way for a reason beyond
tidiness. Debian's CPython patches `sysconfig` to answer
`local/lib/python3.X/dist-packages` whenever the two prefixes agree, and
vanilla CPython answers `lib/python3.X/site-packages`; a package built on the
Forgejo runner and one built on GitHub would otherwise install to different
places, only one of them on `sys.path`. The GitHub Linux job installs Tau's
core from PyPI for real and then imports it from the pet, so the layout is
checked rather than assumed.

`.forgejo/workflows/build.yml` is written to be the other half of that check —
it builds in a Debian container against whatever `python3-dev` that image has,
which is the patched interpreter that would have got the path wrong, and it is
the only place the mingw cross-compile is run under wine. **It does not run.**
That Forgejo instance has no runner able to do this work, so every execution
since the workflow was added has failed during setup, inside a minute. The
workflow is kept because it is correct and costs nothing to keep, but nothing
in this file is verified by it: GitHub is the CI that checks things today, and
the Debian interpreter and the wine test are done by hand or not at all.

The runtime ignores `PYTHONPATH`, `PYTHONHOME` and the user site, as
`python -E -s` does. It is self-contained by construction, and the host's
packages were built for a different interpreter. pip ships as its own wheel
on `sys.path` — the same one the bundled interpreter would have put in a venv,
resolved through `ensurepip` at configure time — so it is whatever pip that
CPython release carries, not a pinned version of this project's.

### The host

```python
import clippy

caps = clippy.capabilities()      # dict; what the platform actually granted
clippy.set_sprite("clippy.png")   # bare names resolve against assets/
clippy.set_position(x, y)
clippy.on("show", lambda: clippy.log("shown"))
clippy.show()
```

| call | effect |
|---|---|
| `show()` / `hide()` / `toggle()` | window visibility |
| `visible()` | `bool` |
| `position()` / `set_position(x, y)` | window position |
| `size()` / `set_size(w, h)` | window size; the stage follows |
| `display_bounds()` | `(x, y, w, h)` work area of the display the pet is on |
| `set_sprite(path)` | load one PNG, contained and centred; `OSError` if it cannot |
| `capabilities()` | the dict behind `--capabilities` |
| `on(event, fn)` | `show`, `hide`, `quit`, `frame`, `click`, `double_click`, `mic`, `configure` |
| `previewing()` | `bool`; on screen only to demonstrate a setting |
| `settings_open(spec)` | open the settings window on a field schema |
| `settings_is_open()` / `settings_close()` | is it up; close it |
| `settings_get(key)` / `settings_set(key, v)` | read or move a field of the open dialog |
| `preview()` | put the pet on screen to demonstrate a setting |
| `pref_path()` | per-user directory for this application's own files, created |
| `quit()` | shut down |
| `log(msg)` | write to the SDL log |

| hook | arguments |
|---|---|
| `show` / `hide` / `quit` | none |
| `frame` | seconds since the last frame |
| `click` | `(x, y, button, clicks)` |
| `double_click` | `(x, y, button)` |
| `mic` | `True` when recording started, `False` when it stopped |
| `configure` | none; the tray's "Configure..." was chosen |

The window swallows clicks over its whole area, transparent corners included,
so a click event is "the pet was clicked" and needs no hit test. SDL counts the
clicks, so there is no double-click timer here — but a double-click also fires
`click` twice, with `clicks` 1 then 2, as every toolkit does it. Use one hook
or the other.

`pref_path()` is where a script keeps its own files: `~/.local/share/
gobboclippy` on Linux, `%APPDATA%` on Windows, `~/Library/Application Support`
on macOS. A script that needs a settings file asks for this rather than
assembling a path out of `$HOME`, which is what keeps the tree forkable.

Visibility changes route through one path (`App::setVisible`) whatever
triggers them — tray, script, or window manager — so hooks fire on the
transition only, and a hook that calls `show()` cannot recurse.

`visible()` and `previewing()` are two different questions, and both have true
answers at once. A settings window demonstrating where the pet will sit needs it
on screen; the user has not asked to see it. So the window is mapped,
`visible()` is `False`, the `show` hook has not fired, and `mic.start()` still
refuses — because the visible pet is the recording indicator, and a pet the user
believes is hidden must not be recording. `preview()` is how a settings handler
asks for that state, and it is refused outside a settings session: closing the
dialog is the only thing that ends one.

`on()` rejects an unknown event name rather than registering a hook that would
never fire. The `frame` hook is called with the seconds since the last frame —
the same number the animations are ticked with, so script timing and animation
timing cannot drift apart.

`set_sprite()` is the one-image shortcut. Anything composed goes on the stage.

### The microphone

```python
clippy.mic.spec()        # always (16000, 1, "s16le")
clippy.mic.devices()     # [(id, name), ...]
clippy.mic.start()       # or start(device_id)
clippy.mic.read()        # bytes; b"" when nothing is waiting
clippy.mic.queued()      # bytes waiting
clippy.mic.active()
clippy.mic.stop()
```

The host owns the device; the script owns where the audio goes. There is no
new dependency behind this — SDL3 already does capture, resampling and
buffering, so `src/Mic.cpp` is one `SDL_AudioStream` and a mutex.

The format is fixed rather than negotiated. 16 kHz mono `s16le` is what every
consumer downstream wants — it is the stdin contract of tectum's streaming
worker verbatim, and what whisper, silero and the small edge models expect —
and SDL resamples from whatever the hardware actually offers, so pinning it
costs nothing and removes a negotiation from every caller.

`read()` is safe from a thread, which is the point: the event loop gives up
the GIL for the whole of each iteration and takes it back only to enter Python,
so a script's audio thread actually runs instead of getting a sliver of each
frame.

**Recording only happens while the pet is visible.** Hiding the window closes
the device, and `start()` while hidden raises. That rule is in the host rather
than in the script because a user relies on it to know when the microphone is
live, and a script is not the right place to keep a promise made to somebody
else. Closing rather than pausing is deliberate too: a paused device keeps the
operating system's own microphone-in-use indicator lit, and the whole point is
that that indicator and the pet agree.

`SDL_INIT_AUDIO` is not part of startup. A machine with no audio stack runs
the pet perfectly well; `--capabilities` reports `microphone: no` with the
reason, and only `start()` has grounds to complain.

`devices()` is empty both when nothing is plugged in and when there is no audio
stack at all, because to a caller asking what it can record from those are the
same answer. The difference is in the `--capabilities` note, and in what
`start()` raises.

The reverse does not hold, and it is worth saying plainly: `microphone: yes`
means a device was *enumerated*, not that it can be opened. A headless Linux
machine with ALSA installed advertises a `default` device with no sound card
behind it. Finding out for certain would mean opening the device, which is the
one thing this program must not do behind the user's back — so `start()` is
where that answer arrives, and it arrives as SDL's own error text.

### The stage

`clippy.stage` is a live list of top-level drawables. A drawable's `.children`
is the same kind of list. Both index, iterate, and support `append`, `remove`,
`clear` and `in`.

```python
import clippy

body = clippy.Sprite(texture=clippy.Texture("clip_body.png"),
                     origin=(128, 226),            # pivot: bottom of the clip
                     align=clippy.Align.TOP_CENTER, margin=30)
clippy.stage.append(body)

eye = clippy.Sprite(texture=clippy.Texture("eyes.png", 48, 48),  # 5-frame strip
                    pos=(-10, -108), origin=(24, 24), parent=body)

eye.animate("sprite_index", [0, 1, 2, 3, 4, 3, 2, 1, 0], 0.34)   # blink
body.animate("scale_y", 1.22, 0.9, easing=clippy.Easing.EASE_OUT_ELASTIC)
```

`pos` is the pivot: the point `origin` of the content sits there, and rotation
and scale happen about it. **Children inherit their parent's translation and
opacity, never its scale or rotation** — which is what lets the paperclip
stretch without distorting the eyes, and lets one `opacity` animation on the
root fade the whole character.

| type | |
|---|---|
| `Texture(path, sprite_width=0, sprite_height=0, smooth=True)` | a PNG, optionally sliced into a grid of frames; `0` means one frame |
| `Font(path)` | a TrueType face; ASCII 32–126, rasterised on demand per size |
| `Sprite(texture=, sprite_index=, ...)` | one frame of a texture |
| `Caption(text=, font=, font_size=, fill_color=, ...)` | text; `\n` starts a line |
| `Animation` | the handle `animate()` returns |

Every drawable carries `pos`, `x`, `y`, `origin`, `scale` (per-axis; negative
mirrors), `rotation`, `opacity`, `visible`, `z_index`, `name`, `parent`,
`children`, `bounds`, `global_bounds`, `subtree_bounds`, `align`, `margin`, and
the methods `animate()`, `realign()`, `move()`, `remove()`.

```python
d.animate(property, target, duration,
          easing=None, delta=False, loop=False,
          callback=None, conflict_mode="replace")
```

- `target` may be a number, **a list of ints** (a frame sequence, stepped not
  interpolated), a 2-tuple (point), a 3/4-tuple (colour) or a `str`
  (typewriter reveal).
- `delta` makes the target relative to wherever the property started.
- `easing` is a `clippy.Easing` member — 36 curves, including the `PING_PONG_*`
  family, which return to their start so a `loop=True` animation has no seam.
- `callback(drawable, property, final_value)` fires on completion.
- `conflict_mode` decides what a second animation on the same property does:
  `"replace"` (default), `"queue"`, or `"error"`.

A misspelled property, easing or alignment raises rather than running to
completion having changed nothing.

`align` is a `clippy.Align` member — the nine corners, sides and centre —
measured against the parent's bounds, or the window's for a top-level drawable.
It is applied when set and on `realign()`, not continuously: a caption whose
text changed has to be realigned.

### Effects

| property | |
|---|---|
| `glow` | halo blur radius in px; `0` disables |
| `glow_color` | halo colour; its **alpha** sets how opaque the halo is |
| `glow_strength` | brightness; above 1 adds additive passes |
| `glow_hardness` | `0` broad soft falloff … `1` near-solid outline |
| `glow_over` | draw the halo above the subtree instead of below (mode) |
| `glow_flat` | halo takes its colour from `glow_color` alone (mode) |
| `aberration` | colour-channel separation in px; `0` disables |
| `aberration_angle` | direction of that separation, in degrees |

The quantities are ordinary animatable properties, so nothing about them is
special-cased:

```python
body.glow = 18.0
body.glow_color = (120, 190, 255)
body.glow_flat = True
body.glow_hardness = 0.45
body.animate("glow_strength", 0.5, 3.1,
             easing=clippy.Easing.PING_PONG_EASE_IN_OUT, delta=True, loop=True)
```

`glow_over` and `glow_flat` are **modes, not quantities** — there is no halfway
point between a halo above the character and below it — so they are plain bools
and `animate()` rejects them rather than interpolating something meaningless.

**`glow_flat` is what makes `glow_color` mean what it says.** Without it the
halo is a blurred copy of the art and `glow_color` only tints it, so a dark
character glows dark and no amount of `glow_strength` changes the hue. With it,
a white silhouette of the subtree is blurred instead — costing one extra walk of
the subtree, which is why it is opt-in.

**`glow_over` wants a low `glow_color` alpha.** Drawn above the character an
opaque halo veils it; around 60–90 it reads as a bloom with the face still
legible.

**An effect applies to the whole subtree, not to the drawable alone.** Setting
`glow` on the clip haloes the character's silhouette once, rather than putting
three haloes around the clip, an eye and a brow; setting `aberration` on it
makes the parts fringe *with* each other instead of against each other. A
drawable with an effect renders its subtree into a private target and
composites that.

Two consequences worth knowing:

- **A glow spills past the character and the window clips it.** Nothing resizes
  the window to compensate — a pet that silently grew its own window would be
  harder to reason about than one whose halo stops at the edge. `subtree_bounds`
  is there to work out the headroom you need.
- **`glow` is what the effect's render target is sized from**, so animating the
  radius resizes that target while animating `glow_strength` or `glow_color`
  does not. The target size is rounded to a multiple of 16 so a radius animation
  is not a texture allocation per frame, but brightness is still the cheaper
  knob for a loop.

No shaders. Both effects are multi-pass composites built from the blend modes
every SDL renderer implements, including the software one — see `src/Effects.h`
for why that constraint was worth keeping, and `tests/fx_pixels.cpp` for what
holds it in place. The blur downsamples by repeated halving rather than in one
step: `SDL_SCALEMODE_LINEAR` reads four texels however far it is minifying and
there are no mipmaps, so a single large reduction aliases instead of blurring,
and the halo crawls when the character moves.

## Assets

`assets/*.svg` are the source of truth; the PNGs are generated. Each SVG is
authored at its final pixel size — a sprite strip is as wide as its frames, not
square — so the renderer reads width and height from the file.

```sh
python tools/render_assets.py        # 1:1; pass a factor for a HiDPI variant
```

Uses whichever rasteriser is installed (rsvg-convert, inkscape or
ImageMagick) and reports which one. It errors rather than emitting a
placeholder if none is available.

`clippy.svg` is the whole character, and is the tray icon. It is also broken
into the parts the demo composes — `clip_body.svg`, `eyes.svg` (a 5-frame blink
strip), `brow.svg` (one brow; the other is the same sprite with `scale_x = -1`)
— all drawn in the same 256×256 frame, so the offsets in `scripts/clippy.py`
can be read straight off `clippy.svg`.

`assets/JetBrainsMono.ttf` is shipped. It is JetBrains Mono 1.0.3 under
Apache-2.0, the same font and version McRogueFace redistributes; its notice is
beside it and in the package's `licenses/`.

## Layout

| path | |
|---|---|
| `src/main.cpp` | CLI, init order, event loop, and the interpreter mode |
| `src/PetWindow.*` | SDL3 window flags, render, the one-image shortcut |
| `src/Tray.*` | `SDL_Tray` menu: Show / Hide / Configure... / Exit |
| `src/Settings.*` | the settings window: Dear ImGui over a schema it does not understand |
| `src/Capabilities.*` | what the platform granted, and why not |
| `src/AppPaths.*` | exe-relative path resolution |
| `src/Mic.*` | the recording device: one `SDL_AudioStream`, fixed at 16 kHz mono |
| `src/PyClippy.*` | the `clippy` extension module: host calls and `clippy.mic` |
| `src/Drawable.*` | transform, tree, alignment, the property system, `Stage` |
| `src/Sprite.*` `src/Caption.*` | the two drawables |
| `src/Texture.*` `src/Font.*` | PNG and WebP atlases (stb_image, libwebp), glyph atlases (stb_truetype) |
| `src/Animation.*` `src/Easing.*` | the animation manager and 36 curves |
| `src/Effects.*` | the glow and aberration composites, and why they use no custom blend mode |
| `tests/fx_pixels.cpp` | pixel-level check of those composites (`-DGC_BUILD_TESTS=ON`) |
| `src/PyDraw.*` | Python types for all of the above — the only file here that includes `Python.h` |
| `scripts/assistant.py` | the listening pet: mic, transcriber, agent, captions |
| `scripts/pet_demo.py` | a downloaded petdex pet, and the control channel |
| `scripts/pet_ctl.py` | the control channel's shell head |
| `scripts/gobbo/` | their parts — `config`, `asr`, `accumulate`, `tau`, `petdex`, `pet`, `states`, `control`, `emote`, `gobbopet`; stdlib only |
| `tests/test_emote.py` | the emote classifier, under a bare `python3` |
| `tests/test_gobbopet.py` | the τ extension, against a running pet |
| `cmake/Package.cmake` | the zip-and-ship staging tree |
| `cmake/ZipStdlib.cmake` | stdlib zip construction |
| `cmake/StripTree.cmake` | strip the staged binaries (Linux, Windows) |
| `cmake/MacFinalize.cmake` | macOS: relocate, strip, sign, seal the bundle — in that order |
| `cmake/Info.plist.in` | macOS: the bundle's identity, and the microphone prompt |
| `cmake/MacIcon.cmake` | macOS: `assets/clippy.png` → `.icns` |

## Status

**Linux/X11 — working.** Build, capability probe, transparency, always-on-top,
tray icon and menu, relocatable package, and the smoke test run from an
extracted tarball with a scrubbed environment on the bundled interpreter. The
drawing layer — composed sprites, frame-sequence blinking, per-axis squash and
stretch, eased motion, parented eyes and eyebrows, alignment, and faded text in
the shipped font — is verified on screen and by the smoke test.

The effect compositor is verified further down than that: `tests/fx_pixels.cpp`
reads pixels back and asserts on them, and CI runs it twice, once on whatever
renderer SDL picks and once with `SDL_RENDER_DRIVER=software`. The halo is only
worth anything if it carries alpha, and that is a property of the blend modes
used rather than of anything visible in a screenshot of an opaque window.

**The microphone — working on Linux; unverified elsewhere.** Device enumeration,
the fixed 16 kHz mono capture format, the hidden-pet refusal and the
close-on-hide rule are all in the smoke test, and hold on a machine with no
audio stack at all — which is every CI runner, so what CI proves is the
refusals, not capture. The capture path itself is verified locally: SDL
delivers the right byte count at the right rate, and `scripts/gobbo/asr.py`
raises a `silent` event when an open device delivers nothing but zeroes, which
is what a muted input looks like from in here and is otherwise
indistinguishable from a transcriber that has stopped working.

**Pets and the control channel — working on Linux; the format is checked
everywhere.** The WebP decoder, and that an undecodable one raises rather than
returning a placeholder, are in the smoke test, so CI checks them on all three
platforms. What CI cannot check is anything past that: installing a pet needs
the network and driving one needs a window, and runners have neither.

Locally the whole path runs — catalogue, install, the nine states on screen,
the socket, `pet_ctl.py` from a shell, and the τ extension's two halves against
a live pet via `tests/test_gobbopet.py`. `tests/test_emote.py` is the piece
with the judgement in it and runs under a bare `python3` anywhere, because the
classifier is a pure function of the two arguments τ hands its hook.

The Windows and macOS control channel is unverified. On Windows it is a
different transport — loopback TCP and a token, because CPython exposes no
`AF_UNIX` there — and that branch has never run.

macOS needed packaging work before it could listen at all: it grants microphone
access per *bundle*, through a TCC prompt driven by
`NSMicrophoneUsageDescription` in an `Info.plist`, and a bare executable has
nowhere to put that string. The macOS package is now a `.dmg` holding a signed
`.app` — see `docs/macos.md`. CI checks the bundle is shaped to be able to ask;
no runner has an input device or a person to answer, so the prompt itself is
still unverified.

**Windows — working.** Built natively by CI on a hosted Windows runner, where
the packaged zip is extracted and runs the full smoke test — the drawing layer
included — on its bundled Python: window, tray, transparency, always-on-top,
textures, the tree, animation and text, all reporting `windows` as the video
driver. That is the authoritative check, and v0.4.0 passed it.

Locally the same tree cross-compiles from Debian with mingw-w64 and runs under
wine, which is a convenience rather than proof — wine is not Windows — but it
does exercise the bundled interpreter, the stdlib zip and the `.pyd` extension
modules rather than only checking that the binary links. **Use wine 10 or
newer**: wine 8.0 hands a piped process invalid standard handles, which stops
CPython from starting at all, and the stock python.org `python.exe` fails there
identically. `docs/cross-compile.md` has the details, along with the C runtime
rules that the mingw/MSVC split imposes on `src/main.cpp`.

**macOS — working in CI, not buildable here.** Hosted arm64 runners build,
relocate, sign and smoke-test the package, which is a `.dmg` containing an
ad-hoc signed `.app`; CI mounts the image and re-verifies the bundle's seal on
the other side. Nothing about it can be reproduced on this machine, so every
macOS change is verified only after it is pushed, and nothing that needs a
person in front of a screen is verified at all. See `docs/macos.md` for the
routes, what signing at each level buys, and why a microphone makes the $99
Developer ID a different question than Gatekeeper does.

**Wayland — untested, and expected to be partly broken.** SDL's Wayland
backend has no `SetWindowAlwaysOnTop` hook at all, and `SDL_SetWindowAlwaysOnTop`
returns success regardless, so the flag reads back as set while nothing has
happened. `src/Capabilities.cpp` therefore reports against the video driver
rather than trusting SDL. `SDL_SetWindowPosition` fails outright on Wayland —
a desktop pet cannot place itself — so X11/XWayland is the supported Linux path
for now.

**Linux tray on GNOME.** SDL's tray backend talks to libayatana-appindicator
over D-Bus. Vanilla GNOME Shell has no StatusNotifierItem host, so
`SDL_CreateTray()` succeeds and no icon appears. That silent failure is not
detectable from SDL; GNOME users need the AppIndicator extension.

### CI

Two runners, deliberately not one.

`.forgejo/workflows/build.yml` is the self-hosted podman path: Linux natively
on `debian:12`, Windows cross-compiled with mingw-w64 on `debian:trixie` — the
newer base because the Windows job runs its own output under wine, and that
needs wine 10.

`.github/workflows/build.yml` builds all three natively on hosted runners, and
is the only way macOS gets built at all (see `docs/macos.md` for why it cannot
be cross-compiled). Each job runs the tree it packaged, not the one it built:
every failure this project has actually hit — a missing stdlib zip, a missing
`.pyd` directory, an absolute macOS install name — links cleanly and fails at
startup.

## License

MIT, © 2026 John McCardle. See [LICENSE](LICENSE).

A packaged build redistributes seven other works in binary form, under their own
terms: SDL3 (zlib), stb (MIT / public domain), CPython (PSF-2.0), pip (MIT),
JetBrains Mono (Apache-2.0), libwebp (BSD-3-Clause, with its patent grant) and
Dear ImGui (MIT). Their notices ship in `licenses/` inside the package, with an
index:

```
gobboclippy-0.5.0-Linux/
  licenses/
    README.txt                    what each file covers
    gobboclippy-MIT.txt
    SDL3-zlib.txt
    stb-MIT-or-public-domain.txt
    CPython-PSF.txt
    pip-MIT.txt
    JetBrainsMono-Apache-2.0.txt
    libwebp-BSD-3-Clause.txt
    libwebp-PATENTS.txt
    dear-imgui-MIT.txt
```

libwebp is two files and one work: the BSD grant is accompanied by a separate
patent grant, and shipping the licence without it would be shipping half the
terms.

Each is copied from the tree it belongs to — the pinned SDL checkout, the
pinned stb checkout, the interpreter being bundled — so a notice cannot
describe a different version than the one shipped. A missing notice is a
configure error.

The font is the exception: a `.ttf` has no source tree to take a notice from,
so its terms are vendored beside it at `assets/JetBrainsMono-LICENSE.txt` and
copied into `licenses/` as well.

On Windows that CPython notice matters more than the others: it carries
"Additional Conditions for this Windows binary build", Microsoft's terms for
the Distributable Code linked into `python3XX.dll`, the `.pyd` modules and the
`vcruntime140*.dll` beside them. The Windows kit is required to supply it.
