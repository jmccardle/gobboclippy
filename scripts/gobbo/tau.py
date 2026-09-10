"""The agent, as ``tau --mode rpc``: JSON-RPC 2.0 over a subprocess's stdio.

Same shape as the transcriber -- a program on the other end of a pipe, one JSON
object per line -- so the assistant drives both the same way and imports
neither.

Two things about this wire are load-bearing and easy to get wrong:

  * **Responses have no size bound.** ``get_capabilities`` alone answers with
    more than 64 KiB, and 64 KiB is the default line cap in several popular
    stream readers. Python's own ``readline`` has no cap, which is why this
    reads with it and not with anything cleverer.
  * **A submission finishes twice.** ``prompt`` answers immediately to say the
    Submission was accepted, and the turn itself ends later with an
    ``agent_end`` event. A host that waits for the response and calls it a
    reply gets an empty one.

Everything read here goes onto the caller's queue. The frame hook drains it;
nothing in this file touches the drawing layer.
"""

import json
import subprocess
import sys
import threading

# The protocol MAJOR this was written against. A MINOR bump is additive and
# safe to ignore -- an unrecognised field is ignored by contract. A MAJOR bump
# is not, and is refused up front rather than discovered on a failing request.
PROTOCOL_MAJOR = 1

STDERR_LINES = 40


# What to do with an utterance spoken while the agent is still answering the
# last one. The protocol's own default is 'reject', which is right for a host
# that can refuse to send -- and wrong here, because by the time this knows,
# the words have already been said. 'enqueue' answers both, in order; nothing
# spoken is dropped. 'steer' folds the new words into the running turn, which
# is what interrupting sounds like, and is a config away.
DEFAULT_MULTITASK = "enqueue"


class Agent:
    def __init__(self, argv, events, multitask=DEFAULT_MULTITASK):
        self.argv = list(argv)
        self.events = events
        self.multitask = multitask
        self.proc = None
        self.ready = False

        self._id = 0
        self._write_lock = threading.Lock()
        self._stderr = []
        self._text = ""          # the reply accumulated this turn
        self._thinking = False

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

        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._drain_stderr, daemon=True).start()

        # Version negotiation before anything that mutates, as the protocol
        # requires. The answer arrives on the reader thread; nothing blocks.
        self._send("get_capabilities", {})

    def stop(self):
        if not self.proc:
            return
        try:
            self.proc.stdin.close()
        except (OSError, ValueError):
            pass
        try:
            self.proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            self.proc.terminate()

    def stderr_tail(self):
        return "\n".join(self._stderr[-STDERR_LINES:])

    # --- sending -----------------------------------------------------------

    def ask(self, text):
        """Submit one utterance as a turn. Returns the request id."""
        if not self.ready:
            raise RuntimeError("the agent has not finished its handshake yet")
        self._text = ""
        # 'voice' is one of the source values the protocol already knows, and
        # it is exactly what this is.
        return self._send("prompt", {"text": text, "source": "voice",
                                     "submitter": "gobboclippy",
                                     "multitask_strategy": self.multitask})

    def _send(self, method, params):
        self._id += 1
        line = json.dumps({"jsonrpc": "2.0", "id": self._id,
                           "method": method, "params": params}) + "\n"
        with self._write_lock:
            try:
                self.proc.stdin.write(line.encode("utf-8"))
                self.proc.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                pass   # the reader reports the death; one report is enough
        return self._id

    # --- reading -----------------------------------------------------------

    def _read(self):
        for line in iter(self.proc.stdout.readline, b""):
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                self.events.put({"type": "agent_junk",
                                 "line": line.decode("utf-8", "replace")[:400]})
                continue
            self._dispatch(msg)

        self.events.put({"type": "agent_gone",
                         "returncode": self.proc.poll(),
                         "stderr": self.stderr_tail()})

    def _dispatch(self, msg):
        if "error" in msg:
            err = msg["error"]
            self.events.put({"type": "agent_error",
                             "message": f"{err.get('code')}: {err.get('message')}"})
            return

        if "result" in msg:
            self._on_result(msg["result"])
            return

        if msg.get("method") == "event":
            self._on_event(msg.get("params") or {})

        # Any other notification is additive protocol the contract says to
        # ignore, and ignoring it is what keeps a MINOR bump non-breaking.

    def _on_result(self, result):
        if result.get("method") != "get_capabilities":
            return

        version = str(result.get("protocol_version", ""))
        major = version.split(".")[0]
        if major != str(PROTOCOL_MAJOR):
            self.events.put({
                "type": "agent_error",
                "message": (f"tau speaks protocol {version}; this was built "
                            f"against {PROTOCOL_MAJOR}.x"),
            })
            return

        self.ready = True
        self.events.put({"type": "agent_ready", "protocol": version})

    def _on_event(self, ev):
        kind = ev.get("type")

        if kind in ("agent_start", "turn_start"):
            self._text = ""
            self._thinking = False
            self.events.put({"type": "agent_start"})

        elif kind == "message_update":
            delta = ev.get("delta")
            if delta is None:
                return
            block = ev.get("block_type")
            if block == "thinking":
                # The local model thinks out loud. That is not the answer, and
                # rendering it as one would put the reasoning on screen and
                # then replace it -- but it is worth saying that something is
                # happening. Once per turn: the deltas arrive by the dozen and
                # "still thinking" is not news.
                if not self._thinking:
                    self._thinking = True
                    self.events.put({"type": "agent_thinking"})
                return
            if block != "text":
                return
            # replace means the provider rewrote the block rather than
            # extending it, so the accumulator resets rather than appends.
            self._text = delta if ev.get("replace") else self._text + delta
            self.events.put({"type": "agent_text", "text": self._text})

        elif kind == "agent_end":
            self.events.put({"type": "agent_end",
                             "text": self._text,
                             "error": ev.get("error"),
                             "end_reason": ev.get("end_reason")})

    def _drain_stderr(self):
        for line in iter(self.proc.stderr.readline, b""):
            self._stderr.append(line.decode("utf-8", "replace").rstrip())


def from_config(events):
    """The configured agent.

    Unlike the transcriber, this one has a default worth having: the package
    ships its own interpreter, and ``tau`` installs into that interpreter's
    ``site/``. ``sys.executable`` is the package's own ``python3``, so the
    default names no path outside the tree.

    A build tree is the exception, and it is named rather than papered over: a
    host that borrows the system runtime has no ``sys.executable`` at all, so
    there is nothing for the default to be, and the config has to say.
    """
    from gobbo import config

    argv = (config.load().get("tau") or {}).get("command")
    if not argv:
        if not sys.executable:
            raise config.Missing(
                f"No tau.command in {config.path()}, and this interpreter has "
                "no sys.executable to default to -- that is a build tree "
                "running against the system runtime rather than a package.\n\n"
                "Either run a packaged build, or name the agent explicitly:\n\n"
                '{\n  "tau": {\n    "command": ["/path/to/venv/bin/tau", '
                '"--mode", "rpc", "--model", "local-llm"]\n  }\n}'
            )
        model = config.setting("tau", "model", "local-llm")
        argv = [sys.executable, "-m", "tau_coding_agent.cli",
                "--mode", "rpc", "--model", model]

    return Agent(argv, events,
                 config.setting("tau", "multitask_strategy", DEFAULT_MULTITASK))
