# Harvesting the McRogueFace sprite layer

The character work — sprites, animation, text — is not written here. It exists
already in [McRogueFace](https://github.com/jmccardle/McRogueFace) and the
intent was to lift it rather than reimplement it.

**Steps 1–7 of the plan below have landed.** What follows records both what was
taken and what was deliberately left, so the next pass does not re-litigate
either.

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

So: take the drawables, leave the window. That is what happened.

## What came across

| here | from McRogueFace | note |
|---|---|---|
| `src/Easing.h/.cpp` | `EasingFunctions` in `Animation.cpp`, table from `PyEasing.cpp` | 36 curves, verbatim. Same names, same enum values. |
| `src/Texture.h/.cpp` | `PyTexture` | Same atlas model. Loader swapped for the `stb_image` path already in `PetWindow.cpp`. |
| `src/Drawable.h/.cpp` | `UIDrawable` | 2964 lines down to ~330. See the cuts below. |
| `src/Sprite.h/.cpp` | `UISprite` | |
| `src/Caption.h/.cpp` | `UICaption` | Rewritten on `stb_truetype`; see below. |
| `src/Font.h/.cpp` | `PyFont` | Likewise. |
| `src/Animation.h/.cpp` | `Animation` + `AnimationManager` | Including the property-lock conflict modes (`replace` / `queue` / `error`). |
| `src/PyDraw.h/.cpp` | the `PyTypeObject`s inlined in each header above | Separated on purpose; see "One structural change". |

The property names are the interface, and they are unchanged: `x`, `y`,
`scale_x`, `scale_y`, `rotation`, `origin_x`, `opacity`, `z_index`,
`sprite_index`, `text`, `font_size`, `fill_color`. An `animate()` call written
against McRogueFace animates the same thing here.

### The seam, as predicted and as it turned out

The plan was to port `src/platform/SDL2Types.h` — McRogueFace's 1419-line
reimplementation of `namespace sf` over SDL2 — to SDL3, then move the drawables
across against it mostly untouched.

**That layer was not needed, and not writing it was the right call.** The
drawables here touch about a dozen SFML types, and SDL3 already supplies the
value types among them: `sf::Vector2f` is `SDL_FPoint`, `sf::Color` is
`SDL_Color`, `sf::FloatRect` is `SDL_FRect`. What remained was `sf::Sprite` and
`sf::Text`, and both are *behaviour* rather than data — a compatibility shim
for them would have been a reimplementation wearing a different name. The
SDL2→SDL3 call mapping in the original plan held up exactly as written:

| SDL2 | SDL3 |
|---|---|
| `SDL_CreateWindow(t,x,y,w,h,f)` | `SDL_CreateWindow(t,w,h,f)` |
| `SDL_RenderCopy` / `SDL_RenderCopyEx` | `SDL_RenderTexture` / `SDL_RenderTextureRotated` |
| `SDL_Rect` in render calls | `SDL_FRect` |
| `SDL_QueryTexture` | `SDL_GetTextureSize` |
| `SDL_FreeSurface` | `SDL_DestroySurface` |
| `SDL_bool` / `SDL_TRUE` | plain `bool` / `true` |

### Text is the one genuine rewrite

McRogueFace gets fonts from SFML, which rasterises and caches glyphs itself.
SDL3 core has no text at all. SDL3_ttf pulls in FreeType, which is a real cost
for the Windows cross-build, so `Font` is written on `stb_truetype` — from the
same stb checkout `stb_image` already comes from, so it costs no new
dependency.

`Font` packs ASCII 32–126 into one atlas per pixel size. `Caption` composites
its glyphs into a private texture and then blits that like a sprite, which is
`UIDrawable::enableRenderTexture()` applied where it earns its keep: rotation,
per-axis scale and mirroring work on text for free, instead of needing a
transformed quad per glyph.

Two things about that path are easy to get wrong and produce output that looks
plausible rather than broken, so they are worth naming:

- a texture composited through a render target holds **premultiplied** alpha.
  Blitting it as though it were straight multiplies by alpha a second time —
  pale text goes muddy, and it gets *brighter* as it fades out. `Caption` uses
  `SDL_BLENDMODE_BLEND_PREMULTIPLIED` and folds the fade into the colour
  modulation, because SDL modulates colour and alpha independently.
- with oversampling on, a glyph's rect **in the atlas** is larger than the rect
  it is drawn into. `stbtt_GetPackedQuad` is what reconciles the two; using the
  packed rect for both draws every glyph at double width, overlapping its
  neighbours.

## One structural change worth keeping

McRogueFace puts each drawable's `PyTypeObject` in the same header as the class
it wraps, so `UISprite.h` includes `Python.h`. Here the Python layer is one
file, `src/PyDraw.cpp`, and `Drawable`, `Sprite`, `Caption`, `Texture`, `Font`
and `Animation` compile with no `Python.h` in sight.

That matches the shape the rest of this project already has — the host is a
library with a scripting layer on top, not a Python program — and it means the
drawing core could carry a CLI, a test harness or a different binding without
the interpreter coming along.

## One behavioural difference, on purpose

**Children inherit their parent's translation and opacity. They do not inherit
its scale or its rotation.**

That is what makes "stretch the paperclip without distorting the eyes" a
one-line animation instead of a counter-transform on every child, and it is the
whole reason the character is composed from parts rather than drawn as one
image. Opacity multiplies down the tree so that fading the character out is one
call on the root.

McRogueFace's `get_global_position()` already sums positions only; this makes
that the documented contract rather than an implementation detail, and
`scripts/smoke_test.py` asserts it.

## Cut, not ported

As planned:

- **the `parent=` kwarg machinery.** It exists there to attach drawables to
  Grids, GridViews and Scenes. What is kept is the plain parent/child tree,
  which is much smaller without those.
- **shaders** (`PyShader`, `PyUniformCollection`). A paperclip does not need
  GLSL ES 2.

  Still true, and it survived contact with the obvious counter-example. The
  colour-shift / chromatic-aberration / glowing-halo set that later landed as
  `src/Effects.*` all sound like shader work and none of it is: colour shifting
  was already `Sprite.color`, and the other two are multi-pass composites. SDL
  3.4 does offer custom fragment shaders through `SDL_GPURenderState`, but it is
  implemented in one backend (`src/render/gpu/`), so using it would mean pinning
  the `gpu` renderer on all three platforms and shipping SPIR-V, DXIL and MSL —
  against a transparent always-on-top window that is the least reproducible part
  of this project. The things that would genuinely need a shader are real hue
  rotation of arbitrary art, a true gaussian, per-pixel distortion,
  outline-from-alpha and dissolve. None of those has been asked for yet.
- **ImGui.** Already excluded from McRogueFace's own SDL2 builds.
- **`Scene` / `PySceneObject`.** A desktop pet has one surface. `Stage` — a
  flat list of roots plus the window bounds that alignment is measured against
  — replaces the scene graph in about 40 lines.

And two more, decided during the port:

- **`PythonObjectCache`.** McRogueFace keeps a serial-number registry so that
  looking a drawable up twice returns the same Python object, preserving
  subclass identity. That is several hundred lines to solve a problem this API
  does not have: all state lives on the C++ object, so two wrappers around one
  `shared_ptr` behave identically. The only visible difference is that
  `stage[0] is stage[0]` is `False`. If Python subclasses of `Sprite` ever need
  to carry their own attributes, this is the thing to bring across.
- **the dirty-flag propagation** (`markContentDirty` / `markCompositeDirty` and
  the parent invalidation chain). It exists to avoid re-compositing frames of
  cached render textures. There is one cached texture here, inside `Caption`,
  and it has a plain local dirty flag.

## Still not here

- **speech bubbles.** A `Caption` on a transparent window sits on whatever the
  user's wallpaper happens to be, so `scripts/clippy.py` draws its text twice,
  offset, as a drop shadow. A real bubble needs a nine-slice `Frame`, which is
  `UIFrame` — the next thing to harvest if it is wanted.
- **input.** `UIDrawable`'s `click_at` / hover dispatch was cut with the rest.
  The window is not click-through and has no hit testing; see the README.
- **a second window on the harvested layer.** There is a second window now --
  the settings dialog -- and it is drawn by Dear ImGui rather than by any of
  this, precisely because a settings dialog is mostly the five things the
  harvest deliberately left out: hit testing, focus, a text caret, clipboard
  and scrolling. `Stage` is still a singleton, and still because there is one
  stage. What has not been revisited is a *pet* window drawn twice; if a speech
  bubble becomes its own borderless window, that is still the assumption to
  revisit first.

## What this base must keep true

For the rest to stay cheap:

1. **The window is not the renderer.** `PetWindow` owns SDL3 window flags and
   publishes its `SDL_Renderer` on the `Stage`. Drawables render into it; they
   do not replace it.
2. **The Python module is additive.** `clippy` is free functions over the host,
   and the harvested types registered alongside them. `clippy.show()` did not
   change shape when they landed.
3. **Alignment is measured against the stage, and the stage follows the
   window.** `SDL_SetWindowSize` is a request, not a change — a hidden X11
   window keeps its old size until it is mapped. `PetWindow::setSize` syncs,
   and `SDL_EVENT_WINDOW_RESIZED` is handled, so nothing aligns against a size
   the window does not have. `setPosition` and `show` sync for the same reason:
   the window manager places a window as it maps it, and without the sync a
   move made between the request and the map is overwritten late and silently
   while `SDL_GetWindowPosition` reports the value that was asked for.

## Also worth stealing

`McRogueFace/src/PathProvider.cpp`, and from the Electron reference,
per-display window placement with re-resolution when the display set changes.

The 1920×1080 assumption that used to be hardcoded in `place_bottom_right()` is
gone: `clippy.display_bounds()` reports the work area of the display the pet is
actually on. It earned itself immediately — this desktop's usable height is
1052, not 1080, and the old code put the pet 28 px under the panel.
