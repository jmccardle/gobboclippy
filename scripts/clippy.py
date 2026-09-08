"""gobboclippy default behaviour.

The C++ host owns the window, the tray and the event loop. Everything here is
policy, and it is plain CPython -- the full standard library is available, and
so is anything you drop next to this file.
"""

import clippy


def place_bottom_right(margin=40):
    """Park the pet above the bottom-right corner of the primary display.

    Uses the window size the host reports rather than assuming 256, so --size
    keeps working.
    """
    w, h = clippy.size()
    # No display-geometry call in the host yet, so derive from the window only
    # when we can't do better. Position is a no-op on Wayland regardless.
    clippy.set_position(1920 - w - margin, 1080 - h - margin)


def main():
    caps = clippy.capabilities()

    clippy.log(f"video driver: {caps['video_driver']} on {caps['platform']}")
    for note in caps["notes"]:
        clippy.log(f"note: {note}")

    clippy.set_sprite("clippy.png")

    if caps["always_on_top"]:
        place_bottom_right()

    clippy.on("show", lambda: clippy.log("shown"))
    clippy.on("hide", lambda: clippy.log("hidden"))
    clippy.on("quit", lambda: clippy.log("goodbye"))

    clippy.show()


main()
