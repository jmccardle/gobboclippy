"""Non-interactive check of the host API.

    ./gobboclippy --script scripts/smoke_test.py

Drives the same code path the tray menu uses -- tray Show/Hide and
clippy.show()/hide() both route through App::setVisible -- so this covers the
tray behaviour without needing to click it.

Exits non-zero on the first failed assertion.
"""

import os
import sys

import clippy

FAILURES = []


def check(label, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{label}: got {got!r}, want {want!r}")
    clippy.log(f"{'PASS' if ok else 'FAIL'}  {label}")
    return ok


# --- capability report -----------------------------------------------------
caps = clippy.capabilities()
clippy.log(f"driver={caps['video_driver']} platform={caps['platform']}")

for key in ("platform", "video_driver", "borderless", "always_on_top",
            "transparent", "skip_taskbar", "tray", "microphone", "notes"):
    if key not in caps:
        FAILURES.append(f"capabilities() missing key {key!r}")

# The tray is required to start at all, so it must report True by now.
check("tray reported available", caps["tray"], True)

# --- sprite ----------------------------------------------------------------
clippy.set_sprite("clippy.png")

try:
    clippy.set_sprite("does_not_exist.png")
except OSError as exc:
    clippy.log(f"PASS  missing sprite raises OSError ({str(exc)[:40]}...)")
else:
    FAILURES.append("set_sprite() accepted a missing file")

# --- unknown event is rejected --------------------------------------------
try:
    clippy.on("definitely_not_an_event", lambda: None)
except ValueError:
    clippy.log("PASS  on() rejects unknown event")
else:
    FAILURES.append("on() accepted an unknown event name")

# --- visibility, and that hooks fire on transitions only -------------------
seen = []
clippy.on("show", lambda: seen.append("show"))
clippy.on("hide", lambda: seen.append("hide"))

check("starts hidden", clippy.visible(), False)

clippy.show()
check("visible after show()", clippy.visible(), True)

clippy.show()                       # already shown: must not re-fire
check("show() is idempotent", seen.count("show"), 1)

clippy.hide()
check("hidden after hide()", clippy.visible(), False)

clippy.toggle()
check("visible after toggle()", clippy.visible(), True)

check("hook sequence", seen, ["show", "hide", "show"])

# --- the settings window ----------------------------------------------------
# The host's half only: a schema goes in, widgets come out, and the preview is a
# third visibility state. What the fields *mean* is scripts/gobbo/settings.py's
# and is not exercised here, deliberately -- driving the real one would write the
# config file of whoever ran the test.
#
# Nothing below clicks OK, because nothing here can: the buttons are drawn by the
# event loop, and none of this has reached it yet.

check("nothing is previewing yet", clippy.previewing(), False)
check("no settings window yet", clippy.settings_is_open(), False)

# preview() outside a settings session is refused rather than ignored: closing
# the dialog is the only thing that ends a preview, so without one there would be
# no way back to hidden.
try:
    clippy.preview()
except RuntimeError as exc:
    clippy.log(f"PASS  preview() refuses with no dialog ({str(exc)[:44]}...)")
else:
    FAILURES.append("preview() engaged with no settings window open")

# A schema the host cannot render is refused at the door. Each of these would
# otherwise be a field silently drawn as something it is not, which is a setting
# that writes back the wrong shape without ever saying so.
for label, spec in (
    ("a spec that is not a dict", ["not", "a", "dict"]),
    ("a field with no key", {"fields": [{"label": "nameless"}]}),
    ("a field of unknown type", {"fields": [{"key": "k", "type": "colour"}]}),
    ("a choice with no choices", {"fields": [{"key": "k", "type": "choice"}]}),
    # A slider needs two ends. Unbounded, it would silently become a plain box
    # -- a widget the schema did not ask for.
    ("a range with no bounds", {"fields": [{"key": "k", "type": "range"}]}),
    ("a range with min == max",
     {"fields": [{"key": "k", "type": "range", "min": 5, "max": 5}]}),
    ("a range with min > max",
     {"fields": [{"key": "k", "type": "range", "min": 9, "max": 2}]}),
    ("a spec with no fields", {"title": "empty"}),
):
    try:
        clippy.settings_open(spec)
    except (TypeError, ValueError) as exc:
        clippy.log(f"PASS  settings_open() refuses {label}")
    else:
        FAILURES.append(f"settings_open() accepted {label}")
        clippy.settings_close()

closed = []
clippy.settings_open({
    "title": "smoke test",
    "fields": [
        {"key": "window.x", "label": "X", "tab": "Window",
         "type": "range", "value": 100, "min": 0, "max": 4096, "live": True},
        {"key": "window.width", "label": "W", "tab": "Window",
         "type": "range", "value": 300, "min": 64, "max": 512, "live": True},
        {"key": "demo.name", "label": "Name", "type": "text", "value": "clippy"},
        {"key": "demo.loud", "label": "Loud", "type": "bool", "value": True},
        {"key": "demo.voice", "label": "Voice", "type": "choice",
         "choices": ["amy", "ryan"], "value": "ryan"},
    ],
    "on_close": lambda: closed.append(True),
})
check("settings window opened", clippy.settings_is_open(), True)

# Two dialogs editing one file is a conflict with no good answer, so the second
# is refused rather than stacked.
try:
    clippy.settings_open({"fields": []})
except RuntimeError:
    clippy.log("PASS  a second settings window is refused")
else:
    FAILURES.append("settings_open() stacked a second window")

# --- reading and writing an open dialog -------------------------------------
# settings_set is what a linked pair of fields needs -- a locked aspect ratio
# moves the field the user is not touching. It deliberately does not fire
# on_change, or a width adjusting a height adjusting a width would not
# terminate; that half cannot be checked here, because on_change only fires from
# the event loop and none of this has reached it.

check("settings_get reads a field", clippy.settings_get("window.width"), 300)
check("settings_get types an int", isinstance(clippy.settings_get("window.width"), int), True)
check("settings_get types a bool", clippy.settings_get("demo.loud"), True)
check("settings_get types a choice", clippy.settings_get("demo.voice"), "ryan")

clippy.settings_set("window.width", 480)
check("settings_set moves a range", clippy.settings_get("window.width"), 480)

# A range is clamped on the way in, so a linked field driven past the end of its
# own scale stops there rather than showing a number the slider cannot reach.
clippy.settings_set("window.width", 9999)
check("settings_set clamps to max", clippy.settings_get("window.width"), 512)
clippy.settings_set("window.width", -5)
check("settings_set clamps to min", clippy.settings_get("window.width"), 64)

clippy.settings_set("demo.name", "gobbo")
check("settings_set moves text", clippy.settings_get("demo.name"), "gobbo")
clippy.settings_set("demo.voice", "amy")
check("settings_set moves a choice", clippy.settings_get("demo.voice"), "amy")

for label, call in (
    ("an absent key", lambda: clippy.settings_set("nope.not.here", 1)),
    ("an absent key for get", lambda: clippy.settings_get("nope.not.here")),
):
    try:
        call()
    except KeyError:
        clippy.log(f"PASS  settings_set/get refuses {label}")
    else:
        FAILURES.append(f"settings_set/get accepted {label}")

# Not one of the offered choices is a bug in the handler, not in the schema, and
# is told apart from a missing field.
try:
    clippy.settings_set("demo.voice", "not-a-voice")
except ValueError:
    clippy.log("PASS  settings_set refuses a value outside the choices")
else:
    FAILURES.append("settings_set accepted a value outside the choices")

clippy.hide()
check("hidden with the dialog up", clippy.visible(), False)

clippy.preview()
check("previewing after preview()", clippy.previewing(), True)
check("a preview is not visible()", clippy.visible(), False)

# The invariant the tri-state exists to protect. The window is on screen, and the
# user believes it is hidden, so the recording indicator promise is unchanged:
# a preview must not be able to record.
try:
    clippy.mic.start()
except RuntimeError as exc:
    clippy.log(f"PASS  mic.start() refuses during a preview ({str(exc)[:36]}...)")
else:
    FAILURES.append("mic.start() recorded during a settings preview")
    clippy.mic.stop()

clippy.settings_close()
check("on_close fired exactly once", closed, [True])
check("preview ends with the dialog", clippy.previewing(), False)
check("still hidden afterwards", clippy.visible(), False)
check("settings window is gone", clippy.settings_is_open(), False)

clippy.show()

# --- the microphone ---------------------------------------------------------
# No CI runner has a recording device, so everything below either holds with no
# hardware at all or is branched on whether this machine turned out to have one.
# Real capture is not covered here and cannot be: it needs a machine with a
# microphone and something making a noise into it.
#
# The branch is on whether start() worked, not on capabilities()["microphone"],
# and the difference is the whole lesson of this section. That flag means "SDL
# enumerates a recording device", which is all anything can know without opening
# one -- and opening one to find out is exactly what this program must not do,
# because it would light the operating system's microphone indicator behind the
# user's back. On a headless Linux runner ALSA advertises a `default` device
# with no sound card behind it, so the flag is true and the open fails. Both
# facts are honest; treating the first as a promise about the second is not.

check("spec is the fixed format", clippy.mic.spec(), (16000, 1, "s16le"))
check("devices() returns a list", isinstance(clippy.mic.devices(), list), True)
check("idle mic is not active", clippy.mic.active(), False)
check("idle mic has nothing queued", clippy.mic.queued(), 0)
check("idle mic reads empty", clippy.mic.read(), b"")

# Stopping something that is not running changed nothing, so it is a no-op
# rather than an error -- and it must not fire the hook.
mic_seen = []
clippy.on("mic", lambda active: mic_seen.append(active))
clippy.mic.stop()
check("stop() on an idle mic is silent", mic_seen, [])

# The invariant, checked where it is cheapest to check: hidden means no
# recording, whether or not this machine has a microphone at all.
clippy.hide()
try:
    clippy.mic.start()
except RuntimeError as exc:
    clippy.log(f"PASS  mic.start() refuses while hidden ({str(exc)[:48]}...)")
else:
    FAILURES.append("mic.start() recorded with the window hidden")
    clippy.mic.stop()

clippy.show()

# start() has exactly two permitted outcomes: it records, or it raises with
# SDL's reason. There is no third, and in particular no quiet no-op.
refusal = None
try:
    clippy.mic.start()
except RuntimeError as exc:
    refusal = str(exc)

# Whatever happened, an unenumerated device cannot have been opened.
if not caps["microphone"]:
    check("no enumerated device, so start() refused", refusal is not None, True)

if refusal is None:
    check("mic is active after start()", clippy.mic.active(), True)
    check("start() fires the hook", mic_seen, [True])

    try:
        clippy.mic.start()
    except RuntimeError:
        clippy.log("PASS  a second start() is refused")
    else:
        FAILURES.append("mic.start() opened a second recording")

    # Hiding must close the device without being asked, and say that it did.
    clippy.hide()
    check("hiding stops the recording", clippy.mic.active(), False)
    check("hiding fires the hook", mic_seen, [True, False])
    clippy.show()
else:
    # The failure path is worth asserting on rather than merely tolerating: a
    # start that could not open a device must leave nothing behind it, or the
    # next start() and the indicator disagree about what is happening.
    clippy.log(f"note: no usable recording device ({refusal[:60]})")
    check("a refused start leaves the mic idle", clippy.mic.active(), False)
    check("a refused start queued nothing", clippy.mic.queued(), 0)
    check("a refused start fires no hook", mic_seen, [])

# --- geometry --------------------------------------------------------------
w, h = clippy.size()
check("window is square", w, h)

clippy.set_position(120, 90)
pos = clippy.position()
# Window managers may adjust placement, so only require it to be reported back
# as a pair of ints rather than exactly what was asked for.
check("position() returns a pair", len(pos), 2)

# Resizing has to be observable immediately: alignment is computed from this,
# so a stale answer silently puts every aligned drawable in the wrong place.
clippy.set_size(320, 240)
check("set_size is observable at once", clippy.size(), (320, 240))

x, y, dw, dh = clippy.display_bounds()
check("display work area is positive", dw > 0 and dh > 0, True)

# --- textures ---------------------------------------------------------------
tex = clippy.Texture("clip_body.png")
check("whole-image texture is one frame", tex.sprite_count, 1)

strip = clippy.Texture("eyes.png", 48, 48)
check("strip cell size", (strip.sprite_width, strip.sprite_height), (48, 48))
check("strip frame count", strip.sprite_count, 5)

try:
    clippy.Texture("eyes.png", 50, 50)
except OSError as exc:
    clippy.log(f"PASS  indivisible cell size raises OSError ({str(exc)[:40]}...)")
else:
    FAILURES.append("Texture() accepted a cell size that does not divide the image")

# --- image formats ----------------------------------------------------------
#
# Which decoders this build has. It is compiled in rather than probed, so the
# check is that the report exists and says what the binary can actually do --
# a script about to download a few thousand WebP sheets reads this first.
formats = caps.get("image_formats") or []
check("png is decodable", "png" in formats, True)
check("webp is decodable", "webp" in formats, True)

# A file whose bytes are not an image it can read must raise. No asset here is
# WebP -- none ever will be, since the format support exists for art the user
# downloads and nothing downloaded is committed -- so this checks the refusal
# rather than a successful decode, which is the half that can go quietly wrong.
bad = os.path.join(clippy.pref_path(), "smoke-not-an-image.webp")
with open(bad, "wb") as fh:
    fh.write(b"RIFF\x24\x00\x00\x00WEBPVP8 " + b"\x00" * 16)
try:
    clippy.Texture(bad, 192, 208)
except OSError as exc:
    clippy.log(f"PASS  undecodable webp raises OSError ({str(exc)[:44]}...)")
else:
    FAILURES.append("Texture() accepted a WebP header with no image behind it")
finally:
    os.unlink(bad)

try:
    clippy.Texture("does_not_exist.png")
except OSError:
    clippy.log("PASS  missing texture raises OSError")
else:
    FAILURES.append("Texture() accepted a missing file")

# --- the tree ---------------------------------------------------------------
body = clippy.Sprite(texture=tex, origin=(128.0, 226.0), name="clip")
eye = clippy.Sprite(texture=strip, pos=(-10.0, -108.0), origin=(24.0, 24.0),
                    parent=body, name="eye")
brow = clippy.Sprite(texture=clippy.Texture("brow.png"), pos=(-2.5, -29.0),
                     origin=(16.0, 8.0), parent=eye, name="brow")

check("parent link", eye.parent.name, "clip")
check("children collection", len(body.children), 1)
check("grandchild", len(eye.children), 1)

# Children accumulate their parents' positions, and nothing else.
body.pos = (100.0, 200.0)
check("global position sums the chain", brow.global_pos,
      (100.0 - 10.0 - 2.5, 200.0 - 108.0 - 29.0))

# The point of the whole exercise: scaling a parent must not move or resize
# its children. If this ever regresses, the paperclip stretches the eyes.
before_bounds = eye.bounds
before_global = eye.global_pos
body.scale = (0.5, 2.0)
check("child bounds ignore parent scale", eye.bounds, before_bounds)
check("child position ignores parent scale", eye.global_pos, before_global)
body.scale = 1.0

# A cycle would make the render walk non-terminating, so it is refused.
try:
    body.parent = brow
except ValueError:
    clippy.log("PASS  reparenting into a cycle raises ValueError")
else:
    FAILURES.append("a drawable was allowed to become its own descendant's child")

clippy.stage.append(body)
check("stage holds the root", len(clippy.stage), 1)
check("stage membership", body in clippy.stage, True)

# --- alignment --------------------------------------------------------------
w, h = clippy.size()
box = clippy.Sprite(texture=strip, align=clippy.Align.BOTTOM_RIGHT, margin=8.0)
clippy.stage.append(box)
bx, by, bw, bh = box.bounds
check("bottom-right alignment", (round(bx + bw + 8), round(by + bh + 8)), (w, h))

box.align = clippy.Align.TOP_CENTER
bx, by, bw, bh = box.bounds
check("top-centre alignment", (round(bx + bw / 2), round(by - 8)), (w // 2, 0))

try:
    box.align = "NOT_AN_ALIGNMENT"
except ValueError:
    clippy.log("PASS  unknown alignment raises ValueError")
else:
    FAILURES.append("align accepted a name that does not exist")

# A child aligns inside its parent's *box*, not against its parent's pivot --
# which matters exactly when the parent has a non-zero origin, as `body` does.
inner = clippy.Sprite(texture=strip, parent=body, align=clippy.Align.TOP_LEFT)
px, py, _, _ = body.bounds
ix, iy, _, _ = inner.global_bounds
check("child aligns to the parent's box corner", (round(ix), round(iy)),
      (round(px), round(py)))
inner.remove()

# --- animation --------------------------------------------------------------
anim = eye.animate("sprite_index", [0, 1, 2, 3, 4], 0.5)
check("animate returns a handle", anim.property, "sprite_index")
check("animation starts incomplete", anim.is_complete, False)

anim.complete()
check("complete() finishes it", anim.is_complete, True)
check("frame sequence ends on its last frame", eye.sprite_index, 4)

# A misspelled property would otherwise run to completion having done nothing.
try:
    eye.animate("sprite_indx", 3, 0.1)
except ValueError:
    clippy.log("PASS  animate() rejects an unknown property")
else:
    FAILURES.append("animate() accepted a property that does not exist")

try:
    eye.animate("x", 10.0, 0.1, easing="no_such_easing")
except ValueError:
    clippy.log("PASS  animate() rejects an unknown easing")
else:
    FAILURES.append("animate() accepted an easing that does not exist")

# conflict_mode='error' has to actually refuse rather than quietly replace.
eye.animate("y", -50.0, 5.0)
try:
    eye.animate("y", 50.0, 5.0, conflict_mode="error")
except RuntimeError:
    clippy.log("PASS  conflict_mode='error' refuses a second animation")
else:
    FAILURES.append("conflict_mode='error' allowed two animations on one property")

# delta targets are relative to wherever the property started.
brow.rotation = 10.0
d = brow.animate("rotation", 5.0, 1.0, delta=True)
d.complete()
check("delta animation is relative", round(brow.rotation), 15)

check("easing enum is ordered", int(clippy.Easing.LINEAR), 0)
check("easing enum has the ping-pong curves",
      hasattr(clippy.Easing, "PING_PONG_EASE_IN_OUT"), True)

# --- text -------------------------------------------------------------------
font = clippy.Font("JetBrainsMono.ttf")
one_w, one_h = font.measure("MM", 20)
two_w, two_h = font.measure("MM\nMM", 20)
check("measure grows with lines", round(two_h), round(one_h * 2))
check("measure is per-line for width", round(two_w), round(one_w))

cap = clippy.Caption(text="hello", font=font, font_size=20, fill_color=(1, 2, 3))
clippy.stage.append(cap)
check("caption reports its size", cap.text_size[0] > 0, True)
check("caption colour round-trips", cap.fill_color, (1, 2, 3, 255))

grew = cap.text_size[0]
cap.text = "hello there"
check("caption remeasures on new text", cap.text_size[0] > grew, True)

# ASCII-only coverage is a stated limit, so it has to be observable rather
# than a character that quietly vanishes.
cap.text = "ok — dash"
check("out-of-range glyphs are counted", cap.skipped_glyphs, 1)

try:
    clippy.Font("does_not_exist.ttf")
except OSError:
    clippy.log("PASS  missing font raises OSError")
else:
    FAILURES.append("Font() accepted a missing file")

try:
    clippy.Caption(text="x", font=clippy.Font("clippy.png"))
except OSError:
    clippy.log("PASS  a non-font file raises OSError")
else:
    FAILURES.append("Font() accepted a file that is not a font")

# --- effects ----------------------------------------------------------------
fx = clippy.Sprite(texture=strip, pos=(100.0, 100.0), origin=(24.0, 24.0))

check("effects are off by default", (fx.glow, fx.aberration), (0.0, 0.0))

fx.glow = 12.0
fx.glow_color = (120, 200, 255)
fx.glow_strength = 2.5
fx.aberration = 4.0
fx.aberration_angle = 30.0
check("glow round-trips", fx.glow, 12.0)
check("glow_color round-trips", fx.glow_color, (120, 200, 255, 255))
check("glow_strength round-trips", fx.glow_strength, 2.5)

fx.glow_hardness = 0.75
check("glow_hardness round-trips", fx.glow_hardness, 0.75)
# Hardness is a coverage curve, not a pass count -- out of range has no meaning.
fx.glow_hardness = 4.0
check("glow_hardness clamps to 1", fx.glow_hardness, 1.0)
fx.glow_hardness = 0.0

check("glow modes default off", (fx.glow_over, fx.glow_flat), (False, False))
fx.glow_over = True
fx.glow_flat = True
check("glow_over round-trips", fx.glow_over, True)
check("glow_flat round-trips", fx.glow_flat, True)
fx.glow_over = False
fx.glow_flat = False
check("aberration round-trips", fx.aberration, 4.0)
check("aberration_angle round-trips", fx.aberration_angle, 30.0)

# A negative blur radius has no meaning, and letting one through would size a
# render target from it.
fx.glow = -5.0
check("negative glow clamps to zero", fx.glow, 0.0)
fx.glow = 12.0

# Effects are ordinary properties, so the existing animation system drives them
# with no special case -- that is the whole reason they live on Drawable.
a = fx.animate("glow", 30.0, 0.5)
check("glow is animatable", a.property, "glow")
a.complete()
check("glow animation lands", fx.glow, 30.0)
fx.glow = 12.0

ac = fx.animate("glow_color", (255, 0, 0, 255), 0.5)
ac.complete()
check("glow_color is animatable", fx.glow_color, (255, 0, 0, 255))
fx.glow_color = (120, 200, 255)

ah = fx.animate("glow_hardness", 1.0, 0.5)
ah.complete()
check("glow_hardness is animatable", fx.glow_hardness, 1.0)
fx.glow_hardness = 0.0

# The two modes are booleans, and there is no meaningful value halfway between
# a halo above the character and below it. animate() has to say so rather than
# interpolate one.
for mode in ("glow_over", "glow_flat"):
    try:
        fx.animate(mode, 1.0, 0.5)
    except ValueError:
        clippy.log(f"PASS  animate() refuses the {mode} mode")
    else:
        FAILURES.append(f"animate() accepted {mode}, which is a mode not a value")

# --- subtree bounds ---------------------------------------------------------
# What the effect target is sized from. Getting this wrong clips the halo, so
# it is checked as geometry rather than left to the eye.
solo = clippy.Sprite(texture=strip, pos=(200.0, 200.0), origin=(0.0, 0.0))
check("subtree bounds of a leaf is its own box", solo.subtree_bounds,
      solo.global_bounds)

kid = clippy.Sprite(texture=strip, pos=(100.0, 0.0), origin=(0.0, 0.0),
                    parent=solo)
sx, sy, sw, sh = solo.subtree_bounds
check("subtree bounds reaches the child", round(sw), 148)   # 48 + 100
check("subtree bounds keeps the near edge", round(sx), 200)

# bounds() ignores rotation, which is right for alignment and wrong for an
# effect target: a rotated child would be clipped out of it.
kid.remove()
solo.rotation = 45.0
_, _, rw, rh = solo.subtree_bounds
check("subtree bounds grows with rotation", rw > 48.0 and rh > 48.0, True)
solo.rotation = 0.0
check("subtree bounds returns when unrotated", round(solo.subtree_bounds[2]), 48)

# --- teardown ---------------------------------------------------------------
cap.remove()
check("remove() takes it off the stage", cap in clippy.stage, False)
clippy.stage.clear()
check("clear() empties the stage", len(clippy.stage), 0)


def finish(drawable, prop, value):
    """Report and exit, once the effects have actually been through the loop."""
    if FAILURES:
        clippy.log(f"{len(FAILURES)} FAILURE(S)")
        for f in FAILURES:
            clippy.log(f"  {f}")
        sys.stderr.write("\n".join(FAILURES) + "\n")
        clippy.quit()
        raise SystemExit(1)

    clippy.log("all checks passed")
    clippy.quit()


# Everything above this line runs before the main loop starts, so none of it has
# drawn a pixel. The effects are a multi-pass composite through a render target,
# and compiling proves nothing about that -- so hand control back to the loop
# with an effected sprite on the stage and quit from an animation callback,
# which puts real frames with a live glow and aberration through the real
# render path. A composite that fails logs and draws nothing; a composite that
# is merely ugly still passes, and is checked by eye.
clippy.stage.append(fx)
clippy.show()
fx.animate("aberration", 6.0, 0.15, callback=finish)
