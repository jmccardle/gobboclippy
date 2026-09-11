"""A petdex pet on the desktop, driven by hand and by anything else.

    ./gobboclippy --script scripts/pet_demo.py

Double-click the pet to step through its emotes. Nothing here involves a model,
a microphone or a subprocess: it is the drawing layer, a downloaded sprite sheet
and nine names.

It also serves the control channel, so the same nine emotes can be played from
another process while this runs:

    ./gobboclippy --python scripts/pet_ctl.py play waving

That is the whole point of the split. The verbs below are this script's policy;
who calls them -- a double-click, a shell, an agent -- is not its business.
"""

import queue

import clippy

from gobbo import config, control, pet, petdex

WINDOW = (300, 340)

# The demo needs *a* pet, and asking the user to pick one before anything can
# be seen is a worse first run than downloading one and saying so. Override it
# with {"pet": {"slug": "..."}} in the config file.
DEFAULT_SLUG = "boba"

INK = (28, 33, 42)
PAPER = (238, 242, 248)

events = queue.Queue()


class Demo:
    def __init__(self, slug, font):
        self.pet = pet.Pet(slug, align=clippy.Align.TOP_CENTER, margin=10.0)
        clippy.stage.append(self.pet.sprite)

        self.shadow = clippy.Caption(
            text="", font=font, font_size=13, fill_color=INK,
            align=clippy.Align.BOTTOM_CENTER, margin=10.0, opacity=0.0)
        self.label = clippy.Caption(
            text="", font=font, font_size=13, fill_color=PAPER,
            pos=(-1.5, -1.5), parent=self.shadow)
        clippy.stage.append(self.shadow)

        self.order = list(self.pet.states)
        self.index = 0
        self.server = None
        self.play(self.order[0])

    # --- the verbs, which are this script's policy -------------------------

    def play(self, state, once=False):
        if once:
            self.pet.play_once(state)
        else:
            self.pet.play(state)
        self.say(state)
        return state

    def say(self, text):
        self.shadow.text = self.label.text = text
        self.shadow.opacity = 1.0 if text else 0.0
        return text

    def state(self):
        return {"slug": self.pet.slug, "name": self.pet.display_name,
                "playing": self.pet.state, "states": list(self.pet.states)}

    def verbs(self):
        return {
            "play":   self.play,
            "say":    self.say,
            "state":  self.state,
            "show":   lambda: bool(clippy.show() or True),
            "hide":   lambda: bool(clippy.hide() or True),
        }

    # --- input -------------------------------------------------------------

    def next_emote(self, *_):
        self.index = (self.index + 1) % len(self.order)
        self.play(self.order[self.index])

    # --- the frame hook ----------------------------------------------------

    def frame(self, dt):
        """The only thing here that touches the drawing layer."""
        while True:
            try:
                msg = events.get_nowait()
            except queue.Empty:
                break
            if msg.get("type") == "control":
                self.server.run(msg["call"])

    def shutdown(self):
        if self.server:
            self.server.stop()


def main():
    clippy.set_size(*WINDOW)
    font = clippy.Font("JetBrainsMono.ttf")

    slug = config.setting("pet", "slug", DEFAULT_SLUG)
    if slug not in petdex.installed():
        clippy.log(f"installing petdex pet {slug!r} into {petdex.path(slug)}")
        petdex.install(slug)

    demo = Demo(slug, font)
    demo.server = control.serve(demo.verbs(), events)
    clippy.log(f"control channel: {demo.server.endpoint_file}")

    clippy.on("frame", demo.frame)
    clippy.on("double_click", demo.next_emote)
    clippy.on("quit", demo.shutdown)
    clippy.show()


# Guarded, even though --script always runs this as __main__. This directory
# ends up on sys.path, and scripts/clippy.py shares a name with the host's
# built-in module -- so an `import clippy` from an interpreter with no host
# (the --python mode, or tau loading an extension from gobbo/) resolves to that
# file. Without a guard, that import silently runs a demo.
if __name__ == "__main__":
    main()
