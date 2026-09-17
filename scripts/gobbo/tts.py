"""The synthesiser, as a subprocess that turns lines of text into samples.

The mirror of ``gobbo/asr.py``, and deliberately the same shape:

    stdin    one line of plain text per utterance
    stdout   raw ``s16le`` mono at ``tts.rate`` -- handed straight to
             ``clippy.speaker.write()``, which is why the host lets the rate be
             the voice's rather than pinning one
    stderr   diagnostics, kept so that a worker which dies can say why

``piper --model <voice>.onnx --output-raw`` is exactly this program, which is
the reason the contract is shaped this way rather than the other way round.
Nothing here knows it is piper: a different synthesiser reading the same lines
and writing the same bytes is a config change, not a branch in this file.

**One process, kept warm.** Measured against piper 1.4.2 and a 63 MB
``en_US-amy-medium``: the first utterance takes 1.56 s to its first samples
because the model loads, and every utterance after it takes 0.24 s in the same
process. So the worker stays up, exactly as the transcriber does, and for the
same reason.

**Audio arrives in bursts, not a trickle.** piper synthesises a whole sentence
and then writes it -- 192 KB inside 40 ms, in the measurement above. That is
why there is no pacing or scheduling in here: SDL's stream is the buffer, and
writing a sentence at once is what it is for.
"""

import collections
import queue
import re
import subprocess
import threading

import clippy

STDERR_LINES = 40

# Bytes per read from the worker. Big enough that a sentence-sized burst is a
# handful of calls rather than fifty -- each one takes the GIL back from the
# thread that draws.
CHUNK = 16384

# Roughly twenty seconds of speech, which is about as long as anyone waits for
# a pet to finish a sentence they can already read.
DEFAULT_MAX_CHARS = 320

#: Markup a listener cannot hear. Each entry is (pattern, what to say about
#: it). The judgement behind the whole tuple: text written to be read is not
#: automatically text that can be said.
UNSPEAKABLE = (
    (re.compile(r"```"), "it contains a code block"),
    (re.compile(r"^#{1,6}\s", re.M), "it contains a heading"),
    (re.compile(r"^\s*([-*+]|\d+[.)])\s", re.M), "it contains a list"),
    (re.compile(r"https?://"), "it contains a URL"),
    (re.compile(r"\n\s*\n"), "it is more than one paragraph"),
)


class Voice:
    """One synthesiser process and the playback device it feeds.

    Owns ``clippy.speaker`` for its lifetime: the device opens in start() so
    that "there is nowhere to play this" is an error where the voice was asked
    for, rather than a silence at the end of the first reply.
    """

    def __init__(self, argv, events, rate, channels=1,
                 max_chars=DEFAULT_MAX_CHARS, gate=True):
        self.argv = list(argv)
        self.events = events          # queue.Queue, drained by the frame hook
        self.rate = int(rate)
        self.channels = int(channels)
        self.max_chars = int(max_chars)
        self.gate = bool(gate)
        self.proc = None
        self._outbox = queue.Queue()
        self._stop = threading.Event()
        self._stderr = collections.deque(maxlen=STDERR_LINES)

        # Which worker the threads below belong to. A replaced worker's threads
        # can still be inside a blocking read when the new one starts, and
        # audio from the process we just killed must not reach the device or be
        # reported as a crash. They check this instead of a flag per thread.
        self._gen = 0
        self._said = False            # has this worker been asked to speak

    # --- lifecycle ---------------------------------------------------------

    def start(self):
        # The device first. A synthesiser is the expensive half and there is no
        # point loading a voice to play it nowhere.
        clippy.speaker.start(rate=self.rate, channels=self.channels)
        try:
            self._spawn()
        except RuntimeError:
            clippy.speaker.stop()
            raise

    def _spawn(self):
        """Start a worker and the three threads that serve it."""
        try:
            proc = subprocess.Popen(
                self.argv,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as e:
            raise RuntimeError(f"cannot run {self.argv[0]!r}: {e}") from None

        self._gen += 1
        self._said = False
        self.proc = proc
        for target in (self._write, self._read, self._drain_stderr):
            threading.Thread(target=target, args=(proc, self._gen),
                             daemon=True).start()

    def stop(self):
        self._stop.set()
        # Closing the device rather than draining it: stop() is what quitting
        # calls, and a pet that took four more seconds to go away because it
        # was mid-sentence would be the wrong trade.
        clippy.speaker.stop()
        if not self.proc:
            return
        try:
            self.proc.stdin.close()
        except (OSError, ValueError):
            pass

        # terminate() first, and this is not impatience. The transcriber is
        # asked to finish because its last line might matter; a synthesiser's
        # remaining work is audio for a device that just closed, so waiting for
        # it politely buys a frozen window and nothing else. Measured: piper
        # mid-sentence takes long enough that a graceful wait here is seconds
        # of locked UI on the way out.
        self.proc.terminate()
        try:
            self.proc.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def stderr_tail(self):
        return "\n".join(self._stderr)

    # --- saying something --------------------------------------------------

    def unspeakable(self, text):
        """Why this should not be read aloud, or None if it should be.

        The reply is on the pet's face either way, so a refusal costs the user
        nothing they had -- whereas reading a code block aloud spends twenty
        seconds saying "backtick" and reading half a sentence as though it were
        whole is the same lie the screen already refuses to tell.
        """
        text = text.strip()
        if not text:
            return "it is empty"
        if not self.gate:
            return None
        if len(text) > self.max_chars:
            return (f"it is {len(text)} characters and the limit for speech "
                    f"is {self.max_chars}")
        for pattern, reason in UNSPEAKABLE:
            if pattern.search(text):
                return reason
        return None

    def say(self, text):
        """Queue an utterance. Returns the refusal reason, or None if accepted.

        A refusal is an outcome rather than an error: the text is already
        readable on screen, and the caller decides whether to mention it.
        """
        refused = self.unspeakable(text)
        if refused:
            return refused
        self._outbox.put(text.strip())
        return None

    def silence(self):
        """Stop talking now, and do not resume.

        Three things have to stop, and only the first two are obvious: the
        samples already handed to SDL, the utterances still in the outbox, and
        **the audio the worker has already synthesised and not yet written**.
        That last one is why this replaces the worker instead of just clearing
        the queue.

        A raw PCM stream has no utterance boundary in it. Waiting for a gap in
        arriving audio to decide where the abandoned reply ends is the obvious
        idea and it does not work: a synthesiser emits one *sentence* per burst,
        so the gaps inside a reply are the same gaps as the ones between
        replies. Measured, when this was a gap heuristic: five seconds after an
        interrupt, 546 KB -- twelve seconds of speech -- was still queued and
        playing. So the interrupt is a kill, which cannot be wrong, and the
        replacement worker reloads its voice during the time the user spends
        saying the thing they interrupted for.

        Cheap when there is nothing to interrupt: a worker that has not been
        asked to speak is left alone, so pressing the chord at a silent pet
        costs nothing.

        Safe from the frame hook. Killing is immediate and the device is only
        cleared, never reopened -- stop() plus start() here cost 60-100 ms of
        the thread that draws, measured, which is six dropped frames every time
        somebody interrupts.
        """
        while True:
            try:
                self._outbox.get_nowait()
            except queue.Empty:
                break
        clippy.speaker.clear()

        if not self._said or self._stop.is_set():
            return

        old = self.proc
        try:
            self._spawn()          # bumps _gen, so old threads go quiet
        except RuntimeError as e:
            # The command ran a moment ago and will not run now. Report it the
            # way a worker that died is reported, rather than leaving a Voice
            # that looks alive and never speaks again.
            self.events.put({"type": "voice_gone", "returncode": None,
                             "stderr": f"could not restart the voice: {e}"})
        if old:
            old.kill()

    def speaking(self):
        return clippy.speaker.active() and not clippy.speaker.drained()

    # --- threads -----------------------------------------------------------

    def _write(self, proc, gen):
        """Utterances -> worker stdin, off the frame hook's thread.

        Writing to the pipe from the caller would put a blocked write in the
        middle of the event loop the moment the worker stopped reading.
        """
        while not self._stop.is_set() and gen == self._gen:
            try:
                text = self._outbox.get(timeout=0.1)
            except queue.Empty:
                continue
            if gen != self._gen:
                # Interrupted while this was waiting. The utterance belongs to
                # the new worker's outbox, so put it back rather than sending
                # it to a process that is being killed.
                self._outbox.put(text)
                return
            try:
                proc.stdin.write(text.encode("utf-8") + b"\n")
                proc.stdin.flush()
                self._said = True
            except (BrokenPipeError, OSError, ValueError):
                return   # _read reports the death; one report is enough

    def _read(self, proc, gen):
        """Worker stdout -> the playback device.

        ``read1`` rather than ``read``: ``read(n)`` blocks until it has exactly
        n bytes, so the last partial chunk of every sentence sat in the buffer
        unplayed until the *next* sentence arrived to fill it. read1 returns
        what one syscall gave, which is the whole sentence when it bursts and
        the tail when that is all there is.
        """
        while not self._stop.is_set():
            chunk = proc.stdout.read1(CHUNK)
            if not chunk:
                break
            # An interrupt replaced this worker while the read was blocked.
            # What just arrived is the reply the user cut off.
            if gen != self._gen:
                return
            try:
                clippy.speaker.write(chunk)
            except RuntimeError:
                # The device closed under us -- a quit, most likely. Not worth
                # reporting: whoever closed it knows.
                break

        if self._stop.is_set() or gen != self._gen:
            return   # a deliberate kill is not a crash

        # EOF with nobody having asked for it: the worker is gone. Never
        # restarted from here, for the transcriber's reason -- something is
        # wrong with the engine, its voice or its venv, and a restart loop
        # would turn that into a mystery. An *interrupt* restarts it, which is
        # a different event with a different cause.
        self.events.put({
            "type": "voice_gone",
            "returncode": proc.poll(),
            "stderr": self.stderr_tail(),
        })

    def _drain_stderr(self, proc, gen):
        for line in iter(proc.stderr.readline, b""):
            if gen != self._gen:
                return
            self._stderr.append(line.decode("utf-8", "replace").rstrip())


def from_config(events):
    """The configured voice, or an error naming what to write.

    No default, for the transcriber's reason: a voice is a file on this
    machine that only the person running it can point at, and a wrong guess is
    a pet that never speaks rather than a sentence saying what to fix. Unlike
    the transcriber, the absence of the setting is not an error -- see
    assistant.py, which treats an unconfigured voice as a pet that does not
    talk and a configured broken one as a failure.
    """
    from gobbo import config

    argv = config.command(
        "tts",
        ["/path/to/piper-venv/bin/piper", "--model",
         "/path/to/voices/en_US-amy-medium.onnx", "--output-raw"],
        "The synthesiser is a program that reads a line of text on stdin and\n"
        "writes raw s16le mono audio on stdout at tts.rate.\n"
        "piper --output-raw is one; its rate is in the voice's .onnx.json\n"
        "as audio.sample_rate (22050 for the medium voices).",
    )
    return Voice(
        argv, events,
        rate=config.setting("tts", "rate", 22050),
        channels=config.setting("tts", "channels", 1),
        max_chars=config.setting("tts", "max_chars", DEFAULT_MAX_CHARS),
        gate=config.setting("tts", "gate", True),
    )
