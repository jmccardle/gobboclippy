# Wayland

**Current position: X11 is the supported Linux path.** Nothing here is
implemented. This documents the route so it does not have to be re-derived.

## It is not the security model

Worth being precise, because "Wayland won't let you" gets applied too broadly.
Wayland does gate things for security -- screen capture, input capture, global
hotkeys -- and those go through portals with user consent.

**Window stacking and positioning are not among them.** `xdg-shell` simply
never exposed them, because the compositor owns window policy by design. There
is no permission to request and nothing to defeat.

The escape is a different shell protocol, `wlr-layer-shell-unstable-v1`, and it
is **unprivileged** in the compositors that implement it -- no portal, no
prompt. It is what panels, docks, notification daemons and wallpaper setters
use. The blocker is not security; it is that GNOME/Mutter declines to implement
it.

## What SDL's Wayland backend actually does

Checked against the pinned `release-3.4.16` in `deps/SDL`:

| | X11 | Wayland |
|---|---|---|
| `SDL_WINDOW_BORDERLESS` | yes | yes |
| `SDL_WINDOW_TRANSPARENT` | yes | yes |
| `SDL_WINDOW_ALWAYS_ON_TOP` | `SDL_x11video.c:182` registers the hook | **no hook exists** |
| `SDL_SetWindowPosition` | works | fails: *"wayland cannot position non-popup windows"* |
| `SDL_WINDOW_NOT_FOCUSABLE` | `wmhints->input = False` | fails: *"focus can only be toggled on popup menu windows"* |
| `SDL_WINDOW_UTILITY` | `_NET_WM_WINDOW_TYPE_UTILITY` | no equivalent |

`grep -rn AlwaysOnTop src/video/wayland/` returns nothing. There is no flag to
set correctly — the capability is absent from the backend.

**Always-on-top fails silently, and that is the dangerous one.**
`SDL_SetWindowAlwaysOnTop` returns `true` regardless, because it only invokes
the backend hook `if (_this->SetWindowAlwaysOnTop)` and that pointer is NULL on
Wayland. `SDL_WINDOW_ALWAYS_ON_TOP` is also in the allowed-flags mask at
creation, so `SDL_GetWindowFlags()` reports it set while nothing has happened.
Neither the return value nor the flag readback can detect this.

This is why `src/Capabilities.cpp` cross-checks `SDL_GetCurrentVideoDriver()`
rather than trusting SDL, and it must keep doing so.

Position and focusability are better behaved — they return `false` with a real
error, so they fail loudly on their own.

## The route, if it is ever wanted

SDL sanctions exactly this. In the pinned tree:

- `SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN`
  (`include/SDL3/SDL_video.h:1441`) — *"true if the application wants to use
  the Wayland surface for a custom role and does not want it attached to an XDG
  toplevel"*
- `test/testwaylandcustom.c` — the reference implementation

Create the window with that property, retrieve
`SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER` / `..._SURFACE_POINTER`, bind the
registry yourself and give the roleless `wl_surface` a
`zwlr_layer_surface_v1` role. SDL keeps doing input, rendering and the event
loop — `testwaylandcustom.c:234` calls `SDL_CreateRenderer(window, NULL)` on
the roleless surface and it works. You own only the shell role.

The protocol fits a desktop pet better than `xdg-shell` does:

| requirement | layer-shell |
|---|---|
| always on top | `layer = TOP`, or `OVERLAY` to sit above fullscreen too |
| `set_position(x, y)` | `set_anchor(TOP\|LEFT)` + `set_margin(y, 0, 0, x)` |
| do not reserve desktop space | `set_exclusive_zone(-1)` |
| never steal keyboard focus | `set_keyboard_interactivity(NONE)` |

There are no absolute coordinates, but anchor + margin is equivalent for our
purposes and survives resolution changes better. `clippy.set_position()` keeps
its signature.

**Cost:** vendor `wlr-layer-shell-unstable-v1.xml`, generate bindings with
`wayland-scanner`, link `libwayland-client`, and write roughly 250 lines —
registry bind, layer surface, configure/ack, anchor+margin mapping.
`Capabilities` gains a `wayland + layer-shell` row.

**Build it as a first-class `PetWindow` backend, not a branch or an #ifdef
thicket.** Shijima-Qt is archived for precisely this reason: its layer-shell
branch diverged until it could not be merged back, and the maintainer's closing
note was that the accumulated hacks had made the project unmanageable.

## Compositor support

| | wlr-layer-shell |
|---|---|
| KDE / KWin | yes |
| Sway, Hyprland, river, Wayfire, labwc | yes |
| COSMIC | yes |
| **GNOME / Mutter** | **no, and declined** |

`ext-layer-shell` — the would-be neutral standard — is an unmerged draft
(wayland-protocols MR !28, still marked Draft) and is absent from
`/usr/share/wayland-protocols/staging/`.

So layer-shell buys KDE and the tiling compositors, not GNOME.

## Why X11 is the answer for now

XWayland is not a hack here. Mutter gives XWayland windows real X11 window
management, so `_NET_WM_STATE_ABOVE` and absolute positioning genuinely work —
on GNOME Wayland *and* KDE Wayland. Forcing the X11 backend gets the complete
capability set on every Linux desktop with no new code.

Prior art splits exactly along this line and nobody has both:

- **Shijima-Qt** — `Qt::WindowStaysOnTopHint | FramelessWindowHint | Tool` on
  X11/XWayland. Archived; its native layer-shell branch never merged.
- **wl_shimeji** — native layer-shell, mandatory, and explicit that it *"will
  never support compositors that do not implement the wlr-layer-shell
  protocol… so no Mutter for now."*

Note the app does **not** currently force a backend. On a Wayland session SDL
picks Wayland and `--capabilities` honestly reports `always on top: no`, which
satisfies Fail Early — the degradation is surfaced, not hidden. Pinning X11
would be one line before `SDL_Init`:

```c
SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
```

That is a default, not a fallback concealing an error, but it is deliberately
not set yet — it would also override a user who has a working reason to want
the Wayland backend.

Revisit when XWayland deprecation actually looms, or when a KDE/Hyprland user
asks.

## The part that really is gated

**Global hotkeys.** Those need
`org.freedesktop.portal.GlobalShortcuts` on Wayland, with real user consent and
no way around it. Relevant because the tkinter predecessor to this project had
a global hotkey; if that feature returns, it is the one piece with an actual
permission model attached.
