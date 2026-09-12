"""The settings window's contents: what is configurable, and what editing it does.

The host draws the window and owns OK/Cancel/Apply. It does not know what any
of these settings *are* -- it renders a list of field dicts and hands the edited
values back. Everything that knows what ``window.x`` means is here, which is
what makes adding a setting a dict in this file rather than a change in C++.

A section is the unit of extension. It says which fields it contributes, what a
live edit of one of them should do right now, and how to fold the edited values
back into the config file. Add a class, put it in :data:`SECTIONS`, and it has a
tab.

Two rules the file-writing follows, both because a settings dialog is the one
place a user reasonably expects not to lose work:

  * The config is re-read at apply time, not reused from when the window opened,
    and only the keys these sections own are replaced. A transcriber command this
    dialog has never heard of survives it.
  * The write is atomic -- a temporary file in the same directory, then
    ``os.replace``. A crash halfway through leaves the old config, not half of
    the new one.
"""

import json
import os
import tempfile

import clippy

from . import config


# --- sections ---------------------------------------------------------------


class Section:
    """One tab's worth of settings.

    Subclasses override :meth:`fields`, and whichever of the three verbs they
    have an opinion about. The base class's versions are the honest do-nothing:
    a section with no live behaviour does not pretend to have any.
    """

    tab = "General"

    # Every key this section owns starts with this plus a dot. It is what routes
    # a live edit to one section rather than to all of them, so a section is
    # never handed a key it has no opinion about.
    prefix = ""

    def owns(self, key):
        return key.split(".", 1)[0] == self.prefix

    def fields(self, cfg):
        """The field dicts to render, given the config as it is on disk."""
        raise NotImplementedError

    def changed(self, key, value):
        """A live field was edited. Do it now, so the user can see it."""

    def store(self, cfg, values):
        """Fold the edited values into ``cfg``, which is about to be written."""

    def restore(self):
        """Cancel: undo whatever :meth:`changed` did."""

    def committed(self):
        """OK or Apply succeeded: whatever :meth:`restore` would return to is
        now here rather than where the window opened."""


class WindowSection(Section):
    """Where the pet sits and how big it is.

    The values are seeded from the live window rather than from the config file,
    because the window is the thing being edited and the file may not mention it
    at all. A pet dragged into place by a script and then configured should show
    the numbers it is actually at.

    Editing any of them puts the pet on screen to demonstrate -- that is what
    ``clippy.preview()`` is for, and why these fields are ``live``. A position
    you cannot see is a number you are guessing at.
    """

    tab = "Window"
    prefix = "window"

    # Deliberately narrower than --size's 32..2048. A desktop pet outside this
    # is either too small to read or big enough to be a wallpaper, and a slider
    # spanning the wider range would make every useful size a pixel apart. The
    # flag still accepts what it always did; this is the dialog's opinion.
    MIN_EDGE, MAX_EDGE = 64, 512

    def __init__(self):
        self._baseline = None
        # Width divided by height, held while the ratio is locked. None when it
        # is not, which is also how :meth:`_link` knows to do nothing.
        self._ratio = None

    # --- the position range --------------------------------------------------
    #
    # Zero to the far edge of the work area, so the top of the slider really
    # does park the pet entirely off the screen -- that is a position somebody
    # may want, and a slider that stopped at "still fully visible" would be
    # deciding otherwise on their behalf.
    #
    # The work area rather than the raw display, because that is the number the
    # host reports and the one that excludes panels and docks. The cost is that
    # a pet on a monitor to the left of the primary needs a negative x, and this
    # slider does not go there. Type it into the box and the slider pins to
    # zero; that case wants a display picker rather than a wider slider.

    def _position_bounds(self):
        x, y, width, height = clippy.display_bounds()
        return x + width, y + height

    def fields(self, cfg):
        x, y = clippy.position()
        w, h = clippy.size()
        self._baseline = (x, y, w, h)

        locked = bool((cfg.get("window") or {}).get("fixed_ratio"))
        self._ratio = (w / h) if (locked and h) else None

        max_x, max_y = self._position_bounds()

        def rng(key, label, value, lo, hi, help_text):
            return {
                "tab": self.tab, "key": key, "label": label, "type": "range",
                "value": value, "min": lo, "max": hi, "live": True,
                "help": help_text,
            }

        return [
            rng("window.x", "Position X", x, 0, max_x,
                "Pixels from the left edge of the desktop. The far end of the "
                "slider is off the screen, which is a place you are allowed to "
                "put it. Ignored on Wayland, which does not let an application "
                "place itself."),
            rng("window.y", "Position Y", y, 0, max_y,
                "Pixels from the top edge of the desktop."),
            rng("window.width", "Width", w, self.MIN_EDGE, self.MAX_EDGE,
                "The window is the pet's whole canvas; art that does not fit "
                "is clipped, not scaled."),
            rng("window.height", "Height", h, self.MIN_EDGE, self.MAX_EDGE,
                "Room for the pet plus whatever the script draws under it."),
            {
                "tab": self.tab, "key": "window.fixed_ratio",
                "label": "Fixed ratio", "type": "bool", "value": locked,
                "live": True,
                "help": "Keep the width and height in the proportion they are "
                        "in now. Dragging either one moves the other.",
            },
        ]

    # --- live edits ----------------------------------------------------------

    def changed(self, key, value):
        # The pet has to be on screen for any of this to mean anything. Asking
        # every time is free -- the host engages the preview once per settings
        # session and ignores the rest.
        clippy.preview()

        x, y = clippy.position()
        w, h = clippy.size()

        if key == "window.fixed_ratio":
            # Locked at whatever the proportions are the moment it is ticked.
            # Nothing moves: turning the lock on is a statement about what
            # happens next, not a resize.
            self._ratio = (w / h) if (value and h) else None
        elif key == "window.x":
            clippy.set_position(value, y)
        elif key == "window.y":
            clippy.set_position(x, value)
        elif key == "window.width":
            self._resize(value, self._link(value, h, vertical=True))
        elif key == "window.height":
            self._resize(self._link(value, w, vertical=False), value)

    def _link(self, driver, other, vertical):
        """The other edge, given the one that just moved.

        Returns ``other`` unchanged when the ratio is not locked. When it is,
        the result is clamped to the same bounds the sliders use, so the pair
        stops at the end of the scale instead of the follower running off it.

        At a stop the proportion is not held -- there is nowhere left for the
        follower to go -- but it is not forgotten either: ``self._ratio`` is
        never recomputed by a drag, so coming back off the stop restores it.
        Clamping the follower rather than the driver is deliberate. Pinning the
        driver would mean writing a value back into the slider the user is
        holding, and a slider that is being dragged rewrites itself from the
        mouse every frame -- the window would end up one size and the dialog
        showing another.
        """
        if not self._ratio:
            return other
        wanted = round(driver / self._ratio) if vertical else round(driver * self._ratio)
        return max(self.MIN_EDGE, min(self.MAX_EDGE, int(wanted)))

    def _resize(self, width, height):
        clippy.set_size(width, height)

        # The slider the user is not holding has to follow, or the dialog is
        # showing a number the window does not have. settings_set is the write
        # that does not come back as an edit; without that this would recurse.
        clippy.settings_set("window.width", width)
        clippy.settings_set("window.height", height)

    # --- saving and cancelling ------------------------------------------------

    def store(self, cfg, values):
        window = cfg.setdefault("window", {})
        window["x"] = values["window.x"]
        window["y"] = values["window.y"]
        window["width"] = values["window.width"]
        window["height"] = values["window.height"]
        window["fixed_ratio"] = values["window.fixed_ratio"]

    def committed(self):
        self._baseline = (clippy.position() + clippy.size())

    def restore(self):
        if self._baseline is None:
            return
        x, y, w, h = self._baseline
        clippy.set_size(w, h)
        clippy.set_position(x, y)


SECTIONS = [WindowSection()]


# --- reading the saved geometry back -----------------------------------------


def apply_saved_geometry():
    """Place and size the window from the config file, if it says anything.

    Called by a startup script before it shows the pet. Returns True if the file
    had geometry, so a script can fall back to its own placement when it did not
    -- which is a default for a user who has never expressed a preference, not a
    fallback around an error. A config file that is there and unreadable still
    raises, from :func:`gobbo.config.load`.

    Partial is allowed and means what it says: a file with a size and no
    position gets its size, and the script places it.
    """
    window = config.load().get("window") or {}

    width, height = window.get("width"), window.get("height")
    if isinstance(width, int) and isinstance(height, int):
        clippy.set_size(width, height)

    x, y = window.get("x"), window.get("y")
    if isinstance(x, int) and isinstance(y, int):
        clippy.set_position(x, y)

    return bool(window)


# --- the window ---------------------------------------------------------------


def _write(cfg):
    """Replace the config file with ``cfg``.

    Atomic, and in the same directory so the replace cannot cross a filesystem.
    """
    path = config.path()
    directory = os.path.dirname(path) or "."

    handle, temporary = tempfile.mkstemp(dir=directory, prefix=".config-", suffix=".json")
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh, indent=2, sort_keys=True)
            fh.write("\n")
        os.replace(temporary, path)
    except BaseException:
        # The old config is still the config; the half-written one is not
        # allowed to survive as evidence of a write that did not happen.
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def _apply(values):
    """OK or Apply. Returns None on success, or a message to show in the dialog.

    The host only calls this when something actually changed, so reaching here
    means there is a file to write.
    """
    try:
        cfg = config.load()
        for section in SECTIONS:
            section.store(cfg, values)
        _write(cfg)
    except (OSError, config.Missing) as e:
        # Reported, not raised: the dialog stays open with the reason on it, so
        # the edits are still there to retry or correct. Anything else raises,
        # and the host prints the traceback and shows the exception's text --
        # an unexpected failure is a bug, and a bug should be loud.
        return f"Could not save {config.path()}: {e}"

    for section in SECTIONS:
        section.committed()
    return None


def _changed(key, value):
    for section in SECTIONS:
        if section.owns(key):
            section.changed(key, value)


def _cancel():
    for section in SECTIONS:
        section.restore()


def open():
    """Open the settings window on the config as it is right now.

    The file is read here, every time, rather than cached: the window is a view
    of what is on disk at the moment it opens, and nothing in this process is
    entitled to a staler answer than the user could get with an editor.

    Bound to the tray's "Configure..." entry by the startup script::

        clippy.on("configure", settings.open)

    The name shadows the builtin inside this module, which is why nothing below
    it opens a file by hand -- :func:`_write` goes through ``os.fdopen`` and
    reading is :mod:`gobbo.config`'s job. It is worth the care: this is the
    module's one verb, and ``settings.open`` is what it should be called.
    """
    cfg = config.load()

    fields = []
    for section in SECTIONS:
        fields.extend(section.fields(cfg))

    clippy.settings_open({
        "title": "gobboclippy settings",
        "fields": fields,
        "on_change": _changed,
        "on_apply": _apply,
        "on_cancel": _cancel,
    })
