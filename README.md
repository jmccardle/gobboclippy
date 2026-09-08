# gobboclippy

A desktop pet host: **embedded CPython inside a small C++/SDL3 program**.
The C++ side owns the window, the tray icon and the event loop. Everything
above that is Python, and the whole thing ships as a relocatable directory
you unzip and run.

This is the *canonical base* — the platform, build and distribution layer.
Speech bubbles, animation and a config window are deliberately not here yet;
see [docs/harvest.md](docs/harvest.md) for how they land on top.

```
      ╭───────────────╮
      │  scripts/*.py │   behaviour, policy, stdlib, your libraries
      ├───────────────┤
      │  clippy       │   extension module (src/PyClippy.cpp)
      ├───────────────┤
      │  C++ host     │   window flags, tray, event loop, paths
      ├───────────────┤
      │  SDL3         │   one dependency, four platforms
      ╰───────────────╯
```

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
gobboclippy 0.0.3  (SDL 3.4.16, Python 3.11)
  platform      : Linux
  video driver  : x11
  borderless    : yes
  always on top : yes
  transparent   : yes
  skip taskbar  : yes
  tray icon     : yes
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

**Click-through.** The window is a 256px square that swallows clicks over its
whole area, transparent corners included. Shaped input regions
(`wl_surface.set_input_region` and friends) are where the hover/drag/click
state machine gets genuinely hard, and skipping them removes the worst
cross-platform cliff. Transparency makes it *look* like a paperclip; it is
still a rectangle.

## Build

Needs CMake ≥ 3.21, a C++17 compiler, and Python ≥ 3.10 with development
headers. SDL3 and stb are fetched and pinned automatically.

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
gobboclippy-0.0.3-Linux/
  gobboclippy            152 KB
  libSDL3.so.0           4.2 MB
  assets/                clippy.svg + rendered PNG
  scripts/               clippy.py
  lib/
    libpython3.11.so     7.7 MB
    python311.zip        2.4 MB   stdlib, test suites stripped
    python3.11/lib-dynload/        stdlib C extensions
```

**18 MB on disk, 8.0 MB compressed.** The binary's RUNPATH is
`$ORIGIN:$ORIGIN/lib`, and every runtime path is resolved from
`SDL_GetBasePath()`, so the directory can be moved anywhere. Verified by
running it from a different filesystem with `env -i`.

Three naming details that are easy to get wrong and fail silently:

- the stdlib zip must be `python311.zip`, **no dot** — that is the only name
  CPython looks for on `sys.path`
- ship `libSDL3.so.0` (the SONAME), not `libSDL3.so.0.4.16`
- `lib-dynload/` must be shipped separately; `.so` modules cannot be imported
  from inside the zip

## Scripting

`scripts/clippy.py` is the entry point. The full standard library is
available, plus anything you drop beside it.

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
| `size()` | `(w, h)` |
| `set_sprite(path)` | load a PNG; raises `OSError` if it cannot |
| `capabilities()` | the dict behind `--capabilities` |
| `on(event, fn)` | `show`, `hide`, `quit`, `frame` |
| `quit()` | shut down |
| `log(msg)` | write to the SDL log |

Visibility changes route through one path (`App::setVisible`) whatever
triggers them — tray, script, or window manager — so hooks fire on the
transition only, and a hook that calls `show()` cannot recurse.

`on()` rejects an unknown event name rather than registering a hook that would
never fire.

## Assets

`assets/clippy.svg` is the source of truth; the PNG is generated.

```sh
python tools/render_assets.py 256
```

Uses whichever rasteriser is installed (rsvg-convert, inkscape or
ImageMagick) and reports which one. It errors rather than emitting a
placeholder if none is available.

## Layout

| path | |
|---|---|
| `src/main.cpp` | CLI, init order, event loop |
| `src/PetWindow.*` | SDL3 window flags, sprite, render |
| `src/Tray.*` | `SDL_Tray` menu: Show / Hide / Exit |
| `src/Capabilities.*` | what the platform granted, and why not |
| `src/PyClippy.*` | the `clippy` extension module |
| `src/AppPaths.*` | exe-relative path resolution |
| `cmake/Package.cmake` | the zip-and-ship staging tree |
| `cmake/ZipStdlib.cmake` | stdlib zip construction |

## Status

**Linux/X11 — working.** Build, capability probe, transparency, always-on-top,
tray icon and menu, relocatable package, and the smoke test run from an
extracted tarball with a scrubbed environment on the bundled interpreter.

**Windows — working, under wine.** Cross-compiled from Debian with mingw-w64.
The packaged zip, freshly extracted, passes the same smoke test on its bundled
Python 3.14: window, tray, transparency, always-on-top, sprite loading and the
host API, all reporting `windows` as the video driver.

Tested under wine 10, not on real Windows hardware. That is a genuine gap —
wine is not Windows — but it exercises the bundled interpreter, the stdlib zip
and the `.pyd` extension modules rather than only checking that the binary
links. **Use wine 10 or newer**: wine 8.0 hands a piped process invalid
standard handles, which stops CPython from starting at all, and the stock
python.org `python.exe` fails there identically. `docs/cross-compile.md` has
the details, along with the C runtime rules that the mingw/MSVC split imposes
on `src/main.cpp`.

**macOS — not built.** The only target that genuinely needs hardware we do not
have; see `docs/macos.md` for the routes and what they cost.

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

A packaged build redistributes three other projects, under their own terms:
SDL3 (zlib), stb (MIT / public domain) and CPython (PSF-2.0). Their notices
are not yet copied into the staging tree — an open item before any release
that is not a draft.
