"""The transcriber, as a subprocess speaking one line of JSON at a time.

The contract, which is tectum's ``streaming_stt_worker.py`` unchanged:

    stdin    raw ``s16le``, 16 kHz, mono -- exactly what ``clippy.mic.read()``
             hands back, which is why the host pins that format
    stdout   one JSON object per line::

                 {"type": "ready"}                     once, after model load
                 {"type": "partial", "text": ...}      the revisable hypothesis
                 {"type": "final", "text": ..., ...}   an endpoint-committed turn

    stderr   diagnostics, kept so that a worker which dies can say why

Nothing about this file knows what engine is on the other end. A network
backend -- FastRTC to a Whisper server, say -- is a different program reading
the same bytes and writing the same lines, not a branch here.

Three threads, and all of them only ever put dicts on the caller's queue: the
drawing layer belongs to the frame hook, and nothing else may touch it.
"""

import collections
import json
import queue
import subprocess
import threading
import time

import clippy

# How often the pump looks for audio. The worker commits on a silence endpoint
# measured in hundreds of milliseconds, so this only has to be small enough not
# to add latency of its own.
POLL_S = 0.02

# Digital silence for this long, while the device is open, is worth saying out
# loud: it is what a muted microphone looks like from in here, and it is
# indistinguishable from a working one until somebody says so.
SILENCE_WARN_S = 3.0

STDERR_LINES = 40


class Transcriber:
    """One worker process, running for as long as the assistant does.

    Started once and kept warm, because a local engine spends ten to twenty
    seconds loading models. Muting is the microphone stopping, not this: when
    ``clippy.mic`` is closed the pump simply has nothing to write, and the
    worker blocks on its own stdin until there is.
    """

    def __init__(self, argv, events):
        self.argv = list(argv)
        self.events = events          # queue.Queue, drained by the frame hook
        self.proc = None
        self._stop = threading.Event()
        self._stderr = collections.deque(maxlen=STDERR_LINES)
        self._silent_since = None
        self._warned_silent = False

    # --- lifecycle ---------------------------------------------------------

    def start(self):
        try:
            self.proc = subprocess.Popen(
                self.argv,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        except OSError as e:
            raise RuntimeError(f"cannot run {self.argv[0]!r}: {e}") from None

        for target in (self._pump, self._read, self._drain_stderr):
            threading.Thread(target=target, daemon=True).start()

    def stop(self):
        self._stop.set()
        if not self.proc:
            return
        # Closing stdin is how the worker is asked to finish; terminate is for
        # one that will not.
        try:
            self.proc.stdin.close()
        except (OSError, ValueError):
            pass
        try:
            self.proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            self.proc.terminate()

    def stderr_tail(self):
        return "\n".join(self._stderr)

    # --- threads -----------------------------------------------------------

    def _pump(self):
        """Microphone -> worker stdin."""
        while not self._stop.is_set():
            chunk = clippy.mic.read()
            if not chunk:
                # An empty read is the ordinary gap between device buffers, not
                # a quiet room: clearing the silence clock here would reset it
                # several times a second and the warning below could never
                # fire. Only a closed device ends a silence.
                if not clippy.mic.active():
                    self._silent_since = None
                    self._warned_silent = False
                time.sleep(POLL_S)
                continue

            self._watch_for_silence(chunk)
            try:
                self.proc.stdin.write(chunk)
                self.proc.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                return   # _read reports the death; one report is enough

    def _watch_for_silence(self, chunk):
        """Say when an open device is delivering nothing but zeroes.

        A muted input is not an error anywhere in the stack -- the device
        opens, the samples arrive on time, and every one of them is zero. The
        transcriber will simply never commit a turn, which looks exactly like a
        broken transcriber. This is the only place that can tell the difference.
        """
        now = time.monotonic()
        if any(chunk):
            self._silent_since = None
            self._warned_silent = False
            return

        if self._silent_since is None:
            self._silent_since = now
        elif not self._warned_silent and now - self._silent_since >= SILENCE_WARN_S:
            self._warned_silent = True
            self.events.put({
                "type": "silent",
                "seconds": round(now - self._silent_since, 1),
            })

    def _read(self):
        """Worker stdout -> the caller's queue, one JSON object per line."""
        for line in iter(self.proc.stdout.readline, b""):
            line = line.strip()
            if not line:
                continue
            try:
                self.events.put(json.loads(line))
            except json.JSONDecodeError:
                # A worker writing something else on stdout has broken the
                # contract, and hiding it would mean losing turns silently.
                self.events.put({
                    "type": "junk",
                    "line": line.decode("utf-8", "replace")[:400],
                })

        # EOF: the worker is gone. Never restarted from here -- something is
        # wrong with the engine, its models or its venv, and a restart loop
        # would turn that into a mystery.
        self.events.put({
            "type": "gone",
            "returncode": self.proc.poll(),
            "stderr": self.stderr_tail(),
        })

    def _drain_stderr(self):
        for line in iter(self.proc.stderr.readline, b""):
            self._stderr.append(line.decode("utf-8", "replace").rstrip())


def from_config(events):
    """The configured transcriber, or an error naming what to write."""
    from gobbo import config

    argv = config.command(
        "asr",
        ["/path/to/asr-venv/bin/python",
         "/path/to/tectum/tectum/audio/streaming_stt_worker.py"],
        "The transcriber is a program that reads raw s16le 16 kHz mono audio\n"
        "on stdin and writes one JSON object per line on stdout\n"
        "({\"type\": \"ready\"|\"partial\"|\"final\", ...}).\n"
        "tectum's streaming_stt_worker.py is one, run by its own venv's python.",
    )
    return Transcriber(argv, events)
