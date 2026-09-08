# Harvesting the McRogueFace sprite layer

The character work — animation, speech bubbles, captions — is not written
here. It exists already in [McRogueFace](https://github.com/jmccardle/McRogueFace)
and the intent is to lift it rather than reimplement it. This document records
the seam, so the base does not drift somewhere the harvest cannot follow.

Nothing here is implemented yet. It is a plan, written down while the
reconnaissance is fresh.

## Why not just fork the engine

McRogueFace's packaging is excellent and directly reusable: embedded CPython,
`zip and ship`, a working Linux→Windows cross-compile, 17 MB Windows zip. That
part is why this project exists in its current shape.

Its *window layer* is the wrong starting point:

- `src/GameEngine.cpp:140` hardcodes
  `sf::Style::Titlebar | sf::Style::Close | sf::Style::Resize`
- across all of `src/` there are zero references to always-on-top,
  transparency, tray, or `getSystemHandle`
- `PyWindow` exposes resolution, fullscreen, vsync, title, visible,
  framerate and scaling — no decoration, position, topmost or alpha
- SFML 2 has no API for always-on-top or tray at all, and its X11 window
  creation never selects an ARGB visual, so per-pixel transparency is not
  reachable without patching SFML itself
- there is no macOS target: zero `APPLE`/`darwin` references in
  `CMakeLists.txt` or `Makefile`

So: take the drawables, leave the window.

## The seam

`src/platform/SDL2Types.h` is a 1419-line reimplementation of `namespace sf`
on top of SDL2, written so that game code compiles unchanged against either
backend. That file is the gift. The drawable classes never touch SFML
directly — they touch `sf::Texture`, `sf::Sprite`, `sf::Color` and friends,
which `SDL2Types.h` already proves can be something else underneath.

The harvest is therefore: **port `SDL2Types.h` to SDL3, then move the
drawables across mostly untouched.**

SDL2 → SDL3 is largely mechanical for the calls that file uses:

| SDL2 | SDL3 |
|---|---|
| `SDL_CreateWindow(t,x,y,w,h,f)` | `SDL_CreateWindow(t,w,h,f)` |
| `SDL_RenderCopy` / `SDL_RenderCopyEx` | `SDL_RenderTexture` / `SDL_RenderTextureRotated` |
| `SDL_Rect` in render calls | `SDL_FRect` |
| `SDL_QueryTexture` | `SDL_GetTextureSize` |
| `SDL_FreeSurface` | `SDL_DestroySurface` |
| `SDL_RenderPresent` returns void | returns `bool` |
| `SDL_bool` / `SDL_TRUE` | plain `bool` / `true` |

Deliberately *not* written yet: a stub `SDL3Types.h` with nothing using it
would be dead code. It gets written when the first drawable lands.

## Dependency order

Measured coupling, so the order is not guesswork. `UISprite.cpp` alone
includes `GameEngine.h`, `UIFrame.h`, `UICaption.h`, `PyGridData.h`,
`PySceneObject.h`, `PyAlignment.h`, `PyShader.h` and
`PyUniformCollection.h` — most of that is the `parent=` keyword-argument
machinery, not drawing.

Take them in this order, cutting couplings as you go:

1. **Value types** — `PyColor` (545), `PyVector` (724), `PyEasing` (264).
   Self-contained, no cuts needed.
2. **`SDL3Types.h`** — the port described above.
3. **`PyTexture`** (668) — swap its loader for the `stb_image` path already
   in `PetWindow.cpp`.
4. **`UIDrawable`** (2964) — the base class. This is where the `parent=`
   coupling lives; cut `PyGridData` and `PySceneObject` out of it, since
   there are no grids or scenes here.
5. **`UISprite`** (1054) — after step 4 this is nearly free.
6. **`UICaption`** (1135) — brings `PyFont` (136) and text rendering. This is
   what speech bubbles need.
7. **`Animation`** (1528) + `PyTimer` — the easing/animation system, which is
   what makes it a character rather than a picture.

Steps 1–5 are the minimum for animated sprites. Step 6 unlocks speech
bubbles. Roughly 8k lines total, most of it moved rather than written.

## Things to cut, not port

- **`parent=` kwarg machinery.** It exists to attach drawables to Grids,
  GridViews and Scenes. None of those are here.
- **Shaders** (`PyShader`, `PyUniformCollection`). GLSL ES 2 shader support is
  an engine feature; a paperclip does not need it.
- **ImGui.** Already excluded from McRogueFace's own SDL2 builds.
- **`Scene` / `PySceneObject`.** A desktop pet has one surface. A flat list of
  drawables replaces the scene graph.

## What this base must keep true

For the above to stay cheap, three things about the current code should not
change:

1. **The window is not the renderer.** `PetWindow` owns SDL3 window flags and
   nothing about how sprites are drawn. Drawables will render into its
   `SDL_Renderer`, not replace it.
2. **The Python module is additive.** `clippy` is free functions over the
   host. Harvested types register alongside them as new types; they do not
   need `clippy.show()` to change shape.
3. **A second window is cheap.** Speech bubbles are a second borderless,
   transparent, always-on-top window positioned relative to the first — the
   same `PetWindow` with a different sprite and a `setPosition` call from the
   main loop. Nothing in the current design assumes exactly one window, and
   nothing should start to.

## Also worth stealing

`McRogueFace/src/PathProvider.cpp` and, from the Electron reference,
per-display window placement with re-resolution when the display set changes,
plus work-area clamping. Applies regardless of toolkit, and this base does not
have it — `scripts/clippy.py` currently hardcodes a 1920×1080 assumption in
`place_bottom_right()`, which is a known gap: there is no display-geometry
call in the `clippy` module yet.
