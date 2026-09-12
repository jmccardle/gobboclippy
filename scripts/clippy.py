"""gobboclippy default behaviour.

The C++ host owns the window, the tray and the event loop. Everything here is
policy, and it is plain CPython -- the full standard library is available, and
so is anything you drop next to this file.

This script is a tech demonstrator for the drawing layer harvested from
McRogueFace, not a personality. It puts a composed, animated character on the
screen using every piece of that layer:

    parts and parenting   the eyes are children of the paperclip, the eyebrows
                          are children of the eyes -- so lifting a brow is one
                          animation on one small sprite
    frame sequences       blinking is animate("sprite_index", [0,1,2,...])
    independent axes      the clip squashes and stretches on scale_x/scale_y
                          separately, and the face does not distort, because a
                          child inherits its parent's position but not its scale
    easing                elastic on the boing, back on the slide-in, ping-pong
                          on anything that has to loop seamlessly
    alignment             the clip is aligned to the top-centre of the window,
                          the caption to the bottom-centre, the badge to a corner
    opacity               captions fade in and out, and opacity multiplies down
                          the tree
    text                  a shipped TrueType face, rasterised at runtime
    effects               a pulsing halo, and a chromatic-aberration kick on the
                          boing -- both animated like any other property, and
                          both applied to the whole character rather than to
                          each part, so the face fringes with the clip instead
                          of against it
"""

import random

import clippy

from gobbo import settings

# --- geometry ---------------------------------------------------------------
#
# All of these are pixel offsets inside assets/clip_body.png's 256x256 frame,
# so they can be read straight off assets/clippy.svg, which draws the whole
# character in that same frame.

WINDOW = (300, 380)

# The clip's pivot: bottom-centre of the wire. Rotating about it makes the
# paperclip sway like something standing up rather than spinning about its
# middle, and scaling about it squashes it onto the floor.
BODY_ORIGIN = (128.0, 226.0)

# Headroom above the clip. Nothing draws outside the window -- the renderer
# clips it -- so a stretch that reaches past the top edge is simply cut off.
# This is the margin that keeps the biggest boing inside.
BODY_TOP_MARGIN = 30.0

# Eye centres, relative to that pivot.
EYE_OFFSET = {"left": (-10.0, -108.0), "right": (34.0, -108.0)}

# Eyebrow centre, relative to its own eye.
BROW_OFFSET = (-2.5, -29.0)

EYE_CELL = 48        # assets/eyes.png is a 5-frame strip of 48x48 cells
BLINK = [0, 1, 2, 3, 4, 4, 3, 2, 1, 0]

# The halo spills this far past the character, and the window clips it -- the
# renderer will not draw outside the window and nothing here resizes it to
# compensate. So this has to stay under BODY_TOP_MARGIN (30) and under the
# slack at the sides ((300 - 256) / 2 = 22), or the glow ends in a hard edge.
GLOW_RADIUS = 18.0

# Aberration is measured in pixels of channel separation. Past about 6 it stops
# reading as a glitch and starts reading as three sprites.
BOING_ABERRATION = 5.0

INK = (40, 49, 61)
PAPER = (238, 242, 248)
# Mid-tone, so the badge stays legible whatever the desktop behind it is.
SLATE = (150, 160, 175)


# --- a very small scheduler --------------------------------------------------
#
# The host hands the frame hook the seconds since the last frame -- the same
# number the animation system is ticked with -- so timing here cannot drift
# away from the animations it starts.

_pending = []


def after(seconds, fn):
    """Call fn() once, `seconds` from now."""
    _pending.append([seconds, fn])


def _tick(dt):
    for entry in list(_pending):
        entry[0] -= dt
        if entry[0] <= 0.0:
            _pending.remove(entry)
            entry[1]()


class Pet:
    def __init__(self):
        body_tex = clippy.Texture("clip_body.png")
        eye_tex = clippy.Texture("eyes.png", EYE_CELL, EYE_CELL)
        brow_tex = clippy.Texture("brow.png")

        self.body = clippy.Sprite(
            texture=body_tex,
            origin=BODY_ORIGIN,
            align=clippy.Align.TOP_CENTER,
            margin=BODY_TOP_MARGIN,
            name="clip",
        )
        clippy.stage.append(self.body)

        self.eyes = {}
        self.brows = {}
        for side, offset in EYE_OFFSET.items():
            # parent= is the whole point: the eye's pos is relative to the
            # clip's pivot, so the clip can move and the face comes along.
            eye = clippy.Sprite(
                texture=eye_tex,
                pos=offset,
                origin=(EYE_CELL / 2, EYE_CELL / 2),
                parent=self.body,
                name=f"eye.{side}",
            )
            brow = clippy.Sprite(
                texture=brow_tex,
                pos=(BROW_OFFSET[0] if side == "left" else -BROW_OFFSET[0],
                     BROW_OFFSET[1]),
                origin=(16.0, 8.0),
                # brow.png is drawn rising to the right. Mirroring the x axis
                # gives the other brow from the same asset.
                scale=(1.0 if side == "left" else -1.0, 1.0),
                parent=eye,
                name=f"brow.{side}",
            )
            self.eyes[side] = eye
            self.brows[side] = brow

        # Where the clip belongs once it has finished sliding in. Read after
        # align placed it, so it stays right if the window size changes.
        self.home_x = self.body.x

    # --- entrance ----------------------------------------------------------

    def slide_in(self, then=None):
        """Come in from off the left edge and overshoot slightly."""
        w, _ = clippy.size()
        self.body.x = -w * 0.6
        self.body.animate(
            "x", self.home_x, 0.8,
            easing=clippy.Easing.EASE_OUT_BACK,
            callback=lambda *_: then and then(),
        )

    # --- idle --------------------------------------------------------------

    def start_idling(self):
        """Two seamless loops, running for as long as the pet is up.

        Both use delta targets with a ping-pong curve, so each cycle ends
        exactly where it started and the seam is invisible. A one-directional
        easing would snap back at the loop point.
        """
        self.body.rotation = -5.0
        self.body.animate("rotation", 10.0, 3.4,
                          easing=clippy.Easing.PING_PONG_SMOOTH,
                          delta=True, loop=True)

        # Breathing: taller and narrower, then back. The eyes are children, so
        # they hold their place on the clip's pivot while the wire stretches.
        self.body.animate("scale_y", 0.05, 2.6,
                          easing=clippy.Easing.PING_PONG_EASE_IN_OUT,
                          delta=True, loop=True)
        self.body.animate("scale_x", -0.035, 2.6,
                          easing=clippy.Easing.PING_PONG_EASE_IN_OUT,
                          delta=True, loop=True)

        self.start_glowing()
        self.schedule_blink()
        self.schedule_boing()

    def start_glowing(self):
        """A halo that breathes, on the character as a whole.

        The effect is set on `body`, and body is the root of the tree, so the
        clip, the eyes and the brows are composited together and haloed once --
        one outline around the character rather than three around its parts.

        Only `glow_strength` is looped, not `glow`. Both are animatable, but the
        radius is what the effect's render target is sized from, so animating it
        resizes that target; brightness costs nothing. The radius is set once
        and left alone.

        `glow_hardness` sits between the two extremes on purpose: at 0 the halo
        is a broad soft cloud, at 1 a dense outline that hugs the wire. Just
        under half keeps the paperclip's shape legible while still reading as
        light rather than as a second sprite.
        """
        self.body.glow = GLOW_RADIUS
        self.body.glow_color = (120, 190, 255)
        # Without glow_flat the halo is a blurred copy of the art, so its colour
        # is the paperclip's tinted blue rather than blue. The clip is pale
        # enough that both read as a glow, but flat is what makes glow_color
        # mean what it says -- and it is the difference between a halo and a
        # smear on anything darker.
        self.body.glow_flat = True
        self.body.glow_hardness = 0.45
        self.body.glow_strength = 0.55
        self.body.animate("glow_strength", 0.5, 3.1,
                          easing=clippy.Easing.PING_PONG_EASE_IN_OUT,
                          delta=True, loop=True)

    def blink(self):
        for eye in self.eyes.values():
            # A frame sequence: the list is stepped through over the duration,
            # not interpolated between.
            eye.animate("sprite_index", BLINK, 0.34)
        self.schedule_blink()

    def schedule_blink(self):
        after(random.uniform(2.5, 6.0), self.blink)

    def boing(self):
        """A big elastic squash-and-stretch, with the face reacting to it.

        Three animations on three different objects at three different depths
        of the tree, each about its own pivot.
        """
        self.body.animate("scale_y", 1.22, 0.9,
                          easing=clippy.Easing.EASE_OUT_ELASTIC,
                          conflict_mode="replace")
        self.body.animate("scale_x", 0.86, 0.9,
                          easing=clippy.Easing.EASE_OUT_ELASTIC,
                          conflict_mode="replace")

        for side, eye in self.eyes.items():
            eye.animate("y", EYE_OFFSET[side][1] - 6.0, 0.25,
                        easing=clippy.Easing.EASE_OUT_BACK)
            eye.animate("scale", 1.15, 0.25, easing=clippy.Easing.EASE_OUT_BACK)

        for brow in self.brows.values():
            brow.animate("y", BROW_OFFSET[1] - 7.0, 0.22,
                         easing=clippy.Easing.EASE_OUT_BACK)
            brow.animate("rotation", -8.0 if brow.scale[0] > 0 else 8.0, 0.22,
                         easing=clippy.Easing.EASE_OUT_BACK)

        # A colour-separation kick that decays over the bounce. Snapping out and
        # easing back is what makes it read as an impact rather than a wobble --
        # the same shape as the elastic scale it rides on.
        self.body.animate("aberration", BOING_ABERRATION, 0.09,
                          easing=clippy.Easing.EASE_OUT_QUAD,
                          conflict_mode="replace")
        after(0.12, self.settle_aberration)

        after(0.7, self.settle)

    def settle_aberration(self):
        self.body.animate("aberration", 0.0, 0.55,
                          easing=clippy.Easing.EASE_OUT_QUAD,
                          conflict_mode="replace")

    def settle(self):
        """Put the face back, then hand the clip's scale back to the breathing
        loop. Restarting the loop rather than letting it resume matters: the
        boing replaced it, and 'replace' means the old one is gone."""
        for side, eye in self.eyes.items():
            eye.animate("y", EYE_OFFSET[side][1], 0.5,
                        easing=clippy.Easing.EASE_IN_OUT_SINE)
            eye.animate("scale", 1.0, 0.5, easing=clippy.Easing.EASE_IN_OUT_SINE)
        for brow in self.brows.values():
            brow.animate("y", BROW_OFFSET[1], 0.5,
                         easing=clippy.Easing.EASE_IN_OUT_SINE)
            brow.animate("rotation", 0.0, 0.5,
                         easing=clippy.Easing.EASE_IN_OUT_SINE)

        self.body.animate("scale_y", 1.0, 0.4,
                          easing=clippy.Easing.EASE_OUT_QUAD,
                          callback=lambda *_: self.resume_breathing())
        self.body.animate("scale_x", 1.0, 0.4, easing=clippy.Easing.EASE_OUT_QUAD)
        self.schedule_boing()

    def resume_breathing(self):
        self.body.animate("scale_y", 0.05, 2.6,
                          easing=clippy.Easing.PING_PONG_EASE_IN_OUT,
                          delta=True, loop=True)
        self.body.animate("scale_x", -0.035, 2.6,
                          easing=clippy.Easing.PING_PONG_EASE_IN_OUT,
                          delta=True, loop=True)

    def schedule_boing(self):
        after(random.uniform(6.0, 11.0), self.boing)


class Speech:
    """A caption that fades one line out and the next one in, forever.

    The window is transparent, so the text sits on whatever the user's desktop
    happens to be -- there is no background colour to pick a readable ink
    against. So it is drawn twice: a dark copy, and a light copy parented to it
    and offset by a pixel and a half. That is a drop shadow, and it reads on a
    dark wallpaper and a light one alike.

    It is also the tidiest demonstration of two properties of the tree: the
    light copy follows the dark one because children inherit translation, and
    one animation on the dark one fades both because children inherit opacity
    multiplicatively.
    """

    LINES = [
        "it looks like you're\nwriting a window\nmanager",
        "would you like help\nwith that?",
        "everything here is\nsprites and easing",
        "no persona yet.\njust the plumbing",
    ]

    def __init__(self, font):
        self.shadow = clippy.Caption(
            text=self.LINES[0],
            font=font,
            font_size=15,
            fill_color=INK,
            align=clippy.Align.BOTTOM_CENTER,
            margin=12.0,
            opacity=0.0,
            name="speech.shadow",
        )
        self.face = clippy.Caption(
            text=self.LINES[0],
            font=font,
            font_size=15,
            fill_color=PAPER,
            pos=(-1.5, -1.5),
            parent=self.shadow,
            name="speech",
        )
        clippy.stage.append(self.shadow)
        self.index = 0

    def start(self):
        self.fade_in()

    def fade_in(self):
        # Only the parent is animated; the child's own opacity stays at 1.0
        # and is multiplied by whatever the parent is showing.
        self.shadow.animate("opacity", 1.0, 0.5,
                            easing=clippy.Easing.EASE_OUT_QUAD)
        after(4.0, self.fade_out)

    def fade_out(self):
        self.shadow.animate(
            "opacity", 0.0, 0.5,
            easing=clippy.Easing.EASE_IN_QUAD,
            callback=lambda *_: self.next_line(),
        )

    def next_line(self):
        self.index = (self.index + 1) % len(self.LINES)
        self.shadow.text = self.LINES[self.index]
        self.face.text = self.LINES[self.index]
        # The text changed size, so the bottom-centre alignment has to be
        # recomputed -- alignment is applied when asked, not continuously.
        self.shadow.realign()
        self.fade_in()


def place_bottom_right(margin=40):
    """Park the pet above the bottom-right corner of the display it is on.

    Uses the work area the host reports, which excludes panels and docks, so
    this lands on the desktop rather than under a taskbar. Position is a no-op
    on Wayland regardless.

    This is where the pet goes when nobody has said otherwise. Somebody who has
    -- through the tray's "Configure..." window -- gets what they said instead;
    see :func:`gobbo.settings.apply_saved_geometry`.
    """
    x, y, dw, dh = clippy.display_bounds()
    w, h = clippy.size()
    clippy.set_position(x + dw - w - margin, y + dh - h - margin)


def main():
    caps = clippy.capabilities()

    clippy.log(f"video driver: {caps['video_driver']} on {caps['platform']}")
    for note in caps["notes"]:
        clippy.log(f"note: {note}")

    # Sized for the character plus a line or three of caption underneath,
    # unless the config file has an opinion -- which it does once anyone has
    # touched the Window tab of the settings dialog.
    clippy.set_size(*WINDOW)
    saved_geometry = settings.apply_saved_geometry()

    font = clippy.Font("JetBrainsMono.ttf")

    pet = Pet()
    speech = Speech(font)

    # Alignment to a corner rather than the centre, on a caption that never
    # moves again.
    badge = clippy.Caption(
        text=f"v{clippy.__version__}",
        font=font,
        font_size=10,
        fill_color=SLATE,
        opacity=0.7,
        align=clippy.Align.TOP_RIGHT,
        margin=6.0,
        name="badge",
    )
    clippy.stage.append(badge)

    if caps["always_on_top"] and not saved_geometry:
        place_bottom_right()

    clippy.on("frame", _tick)
    clippy.on("configure", settings.open)
    clippy.on("show", lambda: clippy.log("shown"))
    clippy.on("hide", lambda: clippy.log("hidden"))
    clippy.on("quit", lambda: clippy.log("goodbye"))

    clippy.show()

    pet.slide_in(then=pet.start_idling)
    after(0.6, speech.start)


# Guarded, even though --script always runs this as __main__. This directory
# ends up on sys.path, and scripts/clippy.py shares a name with the host's
# built-in module -- so an `import clippy` from an interpreter with no host
# (the --python mode, or tau loading an extension from gobbo/) resolves to that
# file. Without a guard, that import silently runs a demo.
if __name__ == "__main__":
    main()
