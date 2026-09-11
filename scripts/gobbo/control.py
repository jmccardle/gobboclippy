"""A socket another process can drive gobboclippy through.

One JSON object per line in, one per line back -- the same shape as the
transcriber and as ``tau --mode rpc``, so this is the third instance of an idiom
already in the tree rather than a new one.

Server and client live in one file because they are one protocol, and a protocol
described in two places drifts.

**The client half must not import clippy.** It runs inside somebody else's
interpreter -- Tau's, a shell script's -- where there is no host and no drawing
layer. So nothing at module scope imports clippy, and the two functions that
need it (:func:`endpoint_path`, :func:`serve`) import it where they stand. A
client is handed an address; it never asks the host where the host lives.

**The frame hook stays the only thing that touches the drawing layer.** A
request does not run on the socket thread. It goes onto the same queue the
transcriber and the agent already use, the frame hook runs it, and the socket
thread waits for the answer. That rule came from the microphone phase and it is
worth more than the few lines it costs here: every verb a script registers is
automatically running on the thread that owns the renderer.

Transport differs by platform and says so rather than pretending:

  * **POSIX** -- a unix socket at ``pref_path()/control.sock``, mode 0600. The
    filesystem is the access control.
  * **Windows** -- CPython exposes no ``AF_UNIX``, so it is a loopback TCP
    socket on an ephemeral port, guarded by a random token. Any local process
    can reach a loopback port, which is what the token is for; on POSIX the
    socket's own permissions already do that job and there is no token.

Either way the details are written to ``pref_path()/control.json`` and a client
reads them from there, so client code is one path on both.
"""

import json
import os
import queue
import secrets
import socket
import threading

ENDPOINT_FILE = "control.json"

# How long a caller waits for the frame hook to run its verb. The hook runs
# every frame, so this is only ever reached when the host is wedged -- and a
# client that blocks forever on a wedged host is harder to diagnose than one
# that says what it was waiting for.
CALL_TIMEOUT = 5.0

HAS_UNIX = hasattr(socket, "AF_UNIX")


class ControlError(RuntimeError):
    """The channel could not be reached, or refused what was asked."""


# --- where the endpoint is -------------------------------------------------

def endpoint_path():
    """``pref_path()/control.json``. Host side only -- it imports clippy."""
    import clippy
    return os.path.join(clippy.pref_path(), ENDPOINT_FILE)


def read_endpoint(path):
    """The dict a running host wrote. Raises if there isn't one."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError:
        raise ControlError(
            f"no control endpoint at {path}. Either gobboclippy is not "
            "running, or the script it is running does not serve the control "
            "channel.") from None
    except json.JSONDecodeError as e:
        raise ControlError(f"{path} is not valid JSON: {e}") from None


# --- the client ------------------------------------------------------------

class Client:
    """Talks to a running gobboclippy. Takes an address; asks nobody.

    Usable as a context manager. A connection carries any number of calls, so
    a long-lived caller opens one and keeps it.
    """

    def __init__(self, endpoint):
        if isinstance(endpoint, str):
            endpoint = read_endpoint(endpoint)
        self.endpoint = endpoint
        self._sock = None
        self._fh = None
        self._lock = threading.Lock()

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *_):
        self.close()

    def connect(self):
        kind = self.endpoint.get("transport")
        try:
            if kind == "unix":
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.endpoint["path"])
            elif kind == "tcp":
                s = socket.create_connection(
                    (self.endpoint["host"], self.endpoint["port"]), timeout=10)
            else:
                raise ControlError(f"unknown transport {kind!r}")
        except OSError as e:
            raise ControlError(f"cannot reach gobboclippy: {e}") from None

        s.settimeout(CALL_TIMEOUT + 5.0)
        self._sock = s
        self._fh = s.makefile("rwb")
        return self

    def close(self):
        for x in (self._fh, self._sock):
            try:
                if x:
                    x.close()
            except OSError:
                pass
        self._fh = self._sock = None

    def call(self, verb, **params):
        """Run one verb. Returns its result, or raises ControlError."""
        if self._fh is None:
            self.connect()

        request = {"verb": verb, "params": params}
        token = self.endpoint.get("token")
        if token:
            request["token"] = token

        with self._lock:
            try:
                self._fh.write((json.dumps(request) + "\n").encode("utf-8"))
                self._fh.flush()
                line = self._fh.readline()
            except OSError as e:
                self.close()
                raise ControlError(f"{verb}: {e}") from None

        if not line:
            self.close()
            raise ControlError(
                f"{verb}: gobboclippy closed the connection without answering")

        try:
            reply = json.loads(line)
        except json.JSONDecodeError as e:
            raise ControlError(f"{verb}: host sent a non-JSON reply: {e}") from None

        if not reply.get("ok"):
            raise ControlError(reply.get("error") or f"{verb} failed")
        return reply.get("result")


# --- the server ------------------------------------------------------------

class _Call:
    """One request, waiting for the frame hook to answer it."""

    __slots__ = ("verb", "params", "reply")

    def __init__(self, verb, params):
        self.verb = verb
        self.params = params
        self.reply = queue.Queue(1)


class Server:
    """Accepts connections and turns their lines into queued calls.

    ``verbs`` is a dict of name -> callable. This module owns the transport;
    what the verbs are is the script's business, which is what keeps the
    channel a control channel rather than a pet API.
    """

    def __init__(self, verbs, events, endpoint_file=None):
        self.verbs = dict(verbs)
        self.events = events
        self.endpoint_file = endpoint_file or endpoint_path()
        self.endpoint = None
        self._sock = None
        self._stop = threading.Event()

    def start(self):
        self._sock, self.endpoint = _listen()
        os.makedirs(os.path.dirname(self.endpoint_file), exist_ok=True)
        # 0600 before anything is in it: on the TCP path this file holds the
        # token, so a window where it is world-readable is a window where the
        # token is.
        fd = os.open(self.endpoint_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            json.dump(self.endpoint, fh)

        threading.Thread(target=self._accept, daemon=True).start()
        return self

    def stop(self):
        self._stop.set()
        try:
            if self._sock:
                self._sock.close()
        except OSError:
            pass
        # Leaving the endpoint file behind would advertise a socket that is
        # gone, and the next client's error would be a refused connection
        # rather than "nothing is running".
        for path in (self.endpoint_file,
                     self.endpoint.get("path") if self.endpoint else None):
            if path:
                try:
                    os.unlink(path)
                except OSError:
                    pass

    # --- threads -----------------------------------------------------------

    def _accept(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._sock.accept()
            except OSError:
                return          # closed by stop(), or the listener died
            threading.Thread(target=self._serve_one, args=(conn,),
                             daemon=True).start()

    def _serve_one(self, conn):
        token = self.endpoint.get("token")
        with conn, conn.makefile("rwb") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                reply = self._handle(line, token)
                try:
                    fh.write((json.dumps(reply) + "\n").encode("utf-8"))
                    fh.flush()
                except OSError:
                    return

    def _handle(self, line, token):
        try:
            request = json.loads(line)
        except json.JSONDecodeError as e:
            return {"ok": False, "error": f"not JSON: {e}"}

        if token and request.get("token") != token:
            return {"ok": False, "error": "bad or missing token"}

        verb = request.get("verb")
        if verb not in self.verbs:
            return {"ok": False,
                    "error": f"unknown verb {verb!r}; this host serves: "
                             f"{', '.join(sorted(self.verbs))}"}

        params = request.get("params") or {}
        if not isinstance(params, dict):
            return {"ok": False, "error": "params must be an object"}

        call = _Call(verb, params)
        self.events.put({"type": "control", "call": call})
        try:
            ok, payload = call.reply.get(timeout=CALL_TIMEOUT)
        except queue.Empty:
            return {"ok": False,
                    "error": f"{verb}: the host did not run it within "
                             f"{CALL_TIMEOUT}s -- its frame loop is not turning"}
        return {"ok": True, "result": payload} if ok else \
               {"ok": False, "error": payload}

    # --- the frame hook's half ---------------------------------------------

    def run(self, call):
        """Execute one queued call. **Call this from the frame hook only.**"""
        try:
            result = self.verbs[call.verb](**call.params)
        except TypeError as e:
            # Wrong arguments for a verb that exists: the caller's mistake, and
            # worth distinguishing from the verb itself failing.
            call.reply.put((False, f"{call.verb}: {e}"))
        except Exception as e:                       # noqa: BLE001
            # A verb is arbitrary script code. Letting it raise here would kill
            # the frame hook and take the window with it, so it is reported
            # down the wire instead -- to the caller, who asked.
            call.reply.put((False, f"{call.verb}: {type(e).__name__}: {e}"))
        else:
            call.reply.put((True, result))


def _listen():
    """A listening socket, and the dict describing how to reach it."""
    import clippy

    if HAS_UNIX:
        path = os.path.join(clippy.pref_path(), "control.sock")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        _clear_stale(path)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        # Bind, then tighten: the socket is created by bind(), so there is no
        # earlier moment to set the mode on.
        s.bind(path)
        os.chmod(path, 0o600)
        s.listen(8)
        return s, {"transport": "unix", "path": path}

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    s.listen(8)
    return s, {"transport": "tcp", "host": "127.0.0.1",
               "port": s.getsockname()[1], "token": secrets.token_urlsafe(24)}


def _clear_stale(path):
    """Remove a socket file left by a dead process -- but never a live one.

    Unlinking unconditionally would silently steal the channel from a second
    running instance, which would then be listening on a path nothing can
    reach. Connecting first is what tells the two apart.
    """
    if not os.path.exists(path):
        return
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        probe.settimeout(0.5)
        probe.connect(path)
    except OSError:
        os.unlink(path)         # nothing answered: it is a leftover
        return
    else:
        raise ControlError(
            f"another gobboclippy is already serving {path}. "
            "Close it, or run this one with the control channel off.")
    finally:
        probe.close()


def serve(verbs, events, endpoint_file=None):
    """Start a control server. Returns it; keep the reference to stop it."""
    return Server(verbs, events, endpoint_file).start()
