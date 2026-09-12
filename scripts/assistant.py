"""gobboclippy, listening.

Double-click the pet to toggle the microphone. What it hears goes to a
transcriber; committed utterances go to the agent; the answer is drawn on the
window.

Run it with:

    ./gobboclippy --script scripts/assistant.py

The transcriber has to be named in the config file first -- the assistant will
tell you which file and what to write in it the first time you ask it to
listen. See scripts/gobbo/config.py.

The one rule that is not this script's to bend: recording only happens while
the pet is visible, because the visible pet is the recording indicator. The
host enforces it -- hiding the window closes the device, and asking for the
microphone while hidden raises. Everything else here is policy.
"""

import queue
import textwrap

import clippy

from gobbo import accumulate, asr, config, settings, tau

WINDOW = (360, 420)

INK = (28, 33, 42)
PAPER = (238, 242, 248)
SLATE = (150, 160, 175)
LIVE = (232, 84, 84)
WARN = (226, 170, 72)

BODY_ORIGIN = (128.0, 226.0)
HEARD_WIDTH = 40          # characters per line, at font_size 14
REPLY_LINES = 7

events = queue.Queue()


# --- text -------------------------------------------------------------------

def wrapped(text, width=HEARD_WIDTH, lines=None):
    """Fold text to the window's width, keeping the tail when it is too long."""
    out = []
    for para in text.splitlines() or [""]:
        out.extend(textwrap.wrap(para, width) or [""])
    if lines is not None:
        out = out[-lines:]
    return "\n".join(out)


class Shadowed:
    """A caption drawn twice, so it reads on any wallpaper.

    The window is transparent: there is no background to pick a readable ink
    against. A dark copy with a light copy parented to it and offset by a pixel
    and a half is a drop shadow, and children inherit both translation and
    opacity, so one animation moves and fades the pair.
    """

    def __init__(self, font, size, color, align, margin, name):
        self.shadow = clippy.Caption(
            text="", font=font, font_size=size, fill_color=INK,
            align=align, margin=margin, opacity=0.0, name=f"{name}.shadow",
        )
        self.face = clippy.Caption(
            text="", font=font, font_size=size, fill_color=color,
            pos=(-1.5, -1.5), parent=self.shadow, name=name,
        )
        clippy.stage.append(self.shadow)
        self._text = ""
        self._color = color

    def set(self, text, color=None):
        # Setting a caption's text rebuilds its glyph texture, so a redundant
        # set is a re-rasterisation for nothing. Streaming text changes every
        # frame and has to pay that; a status line that says the same thing
        # twice does not.
        if text == self._text and (color is None or color == self._color):
            return
        self._text = text
        self.shadow.text = text
        self.face.text = text
        if color is not None:
            self._color = color
            self.face.fill_color = color
        self.shadow.opacity = 1.0 if text else 0.0


# --- the assistant ----------------------------------------------------------

class Assistant:
    def __init__(self, font):
        self.body = clippy.Sprite(
            texture=clippy.Texture("clip_body.png"),
            origin=BODY_ORIGIN,
            align=clippy.Align.TOP_CENTER,
            margin=20.0,
            name="clip",
        )
        clippy.stage.append(self.body)

        self.status = Shadowed(font, 11, SLATE, clippy.Align.TOP_LEFT, 8.0,
                               "status")
        self.heard = Shadowed(font, 14, PAPER, clippy.Align.BOTTOM_CENTER, 12.0,
                              "heard")
        self.reply = Shadowed(font, 13, PAPER, clippy.Align.CENTER_LEFT, 10.0,
                              "reply")

        self.accumulator = accumulate.Accumulator(
            unclear_render=config.setting("asr", "unclear_render", "guess"))
        self.transcriber = None
        self.agent = None
        self.listening = False      # the worker has loaded and is ready

        self.body.animate("scale_y", 0.04, 2.6,
                          easing=clippy.Easing.PING_PONG_EASE_IN_OUT,
                          delta=True, loop=True)
        self.body.animate("scale_x", -0.03, 2.6,
                          easing=clippy.Easing.PING_PONG_EASE_IN_OUT,
                          delta=True, loop=True)

    # --- the microphone ----------------------------------------------------

    def toggle_mic(self, *_):
        if clippy.mic.active():
            clippy.mic.stop()
            return
        try:
            self.ensure_workers()
            clippy.mic.start()
        except (RuntimeError, config.Missing) as e:
            self.fail(str(e))

    def ensure_workers(self):
        """Start the two subprocesses, once, on the first request to listen.

        Not at startup: a transcriber costs ten to twenty seconds of model
        loading and an agent holds a model server open, and neither is owed by
        a pet nobody has spoken to yet.
        """
        if self.transcriber is None:
            self.transcriber = asr.from_config(events)
            self.transcriber.start()
            self.set_status("loading the transcriber", SLATE)

        if self.agent is None:
            self.agent = tau.from_config(events)
            try:
                self.agent.start()
            except RuntimeError as e:
                # Transcription is a capability of its own and still works.
                # Saying so on screen is the point -- an agent that is not
                # there must not look like an agent with nothing to say.
                self.agent = None
                self.fail(f"agent unavailable: {e}")

    def on_mic(self, active):
        if active:
            self.set_status("listening" if self.listening
                            else "loading the transcriber",
                            LIVE if self.listening else SLATE)
        else:
            self.set_status("idle" if self.listening else "", SLATE)
            self.heard.set("")

    # --- display -----------------------------------------------------------

    def set_status(self, text, color=SLATE):
        self.status.set(text, color)

    def fail(self, message):
        clippy.log(f"! {message}")
        self.set_status("problem", WARN)
        self.reply.set(wrapped(message, lines=REPLY_LINES), WARN)

    # --- events ------------------------------------------------------------

    def handle(self, msg):
        kind = msg.get("type")

        # --- the transcriber ---
        if kind == "ready":
            self.listening = True
            if clippy.mic.active():
                self.set_status("listening", LIVE)

        elif kind == "partial":
            self.heard.set(wrapped(self.accumulator.partial(msg)), SLATE)

        elif kind == "final":
            utterance = self.accumulator.final(msg)
            text = utterance["text"].strip()
            self.heard.set(wrapped(text), PAPER)
            clippy.log(f"turn {utterance['turn_index']}: {utterance['tagged_text']}")
            if text:
                self.ask(text)

        elif kind == "silent":
            # An open device delivering pure zeroes. Worth saying: it looks
            # exactly like a transcriber that has stopped working.
            self.fail(f"the microphone has been silent for {msg['seconds']}s -- "
                      "is the input muted?")

        elif kind == "junk":
            self.fail(f"the transcriber wrote something that is not JSON: "
                      f"{msg['line']}")

        elif kind == "gone":
            self.transcriber = None
            self.listening = False
            if clippy.mic.active():
                clippy.mic.stop()
            self.fail(f"the transcriber exited ({msg['returncode']})\n"
                      f"{msg['stderr']}")

        # --- the agent ---
        elif kind == "agent_ready":
            clippy.log(f"agent ready, protocol {msg['protocol']}")

        elif kind == "agent_start":
            self.reply.set("...", SLATE)

        elif kind == "agent_thinking":
            self.reply.set("thinking...", SLATE)

        elif kind == "agent_text":
            self.reply.set(wrapped(msg["text"], lines=REPLY_LINES), PAPER)

        elif kind == "agent_end":
            clippy.log(f"reply: {msg['text']}")
            if msg.get("error"):
                self.fail(f"the agent stopped: {msg['error']}")
            elif msg.get("end_reason") not in (None, "done", "terminate"):
                # 'length' or 'max_turns' means the answer on screen is a
                # prefix, and a prefix that looks finished is a lie.
                self.reply.set(
                    wrapped(msg["text"], lines=REPLY_LINES) +
                    f"\n[cut short: {msg['end_reason']}]", WARN)

        elif kind in ("agent_error", "agent_junk"):
            self.fail(f"agent: {msg.get('message') or msg.get('line')}")

        elif kind == "agent_gone":
            self.agent = None
            self.fail(f"the agent exited ({msg['returncode']})\n{msg['stderr']}")

    def ask(self, text):
        if not self.agent or not self.agent.ready:
            return
        try:
            self.agent.ask(text)
        except RuntimeError as e:
            self.fail(f"agent: {e}")

    # --- the frame hook ----------------------------------------------------

    def frame(self, dt):
        """The only thing in this script that touches the drawing layer.

        The reader threads put dicts on a queue and nothing else; the drawing
        layer and the SDL renderer belong to this thread.
        """
        while True:
            try:
                msg = events.get_nowait()
            except queue.Empty:
                break
            self.handle(msg)

    def shutdown(self):
        if self.transcriber:
            self.transcriber.stop()
        if self.agent:
            self.agent.stop()


def place_bottom_right(margin=40):
    """Park the pet above the bottom-right corner of the display it is on."""
    x, y, dw, dh = clippy.display_bounds()
    w, h = clippy.size()
    clippy.set_position(x + dw - w - margin, y + dh - h - margin)


def main():
    caps = clippy.capabilities()
    clippy.log(f"video driver: {caps['video_driver']} on {caps['platform']}")
    for note in caps["notes"]:
        clippy.log(f"note: {note}")

    clippy.set_size(*WINDOW)
    saved_geometry = settings.apply_saved_geometry()
    font = clippy.Font("JetBrainsMono.ttf")

    me = Assistant(font)

    clippy.on("frame", me.frame)
    clippy.on("double_click", me.toggle_mic)
    clippy.on("mic", me.on_mic)
    clippy.on("quit", me.shutdown)
    clippy.on("configure", settings.open)

    if caps["always_on_top"] and not saved_geometry:
        place_bottom_right()
    clippy.show()

    if not caps["microphone"]:
        me.fail("no microphone; --capabilities says why")
    else:
        me.set_status("double-click to listen", SLATE)


# Guarded, even though --script always runs this as __main__. This directory
# ends up on sys.path, and scripts/clippy.py shares a name with the host's
# built-in module -- so an `import clippy` from an interpreter with no host
# (the --python mode, or tau loading an extension from gobbo/) resolves to that
# file. Without a guard, that import silently runs a demo.
if __name__ == "__main__":
    main()
