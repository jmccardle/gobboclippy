"""The listening assistant: microphone -> transcriber -> agent -> the window.

The host (C++) owns the device, the window and the clock. Everything here is
policy, and all of it is plain CPython.

The two programs this drives are subprocesses speaking one line of JSON at a
time over stdio, because that is what they already are:

  * the transcriber -- ``s16le`` 16 kHz mono in, ``{"type": ...}`` lines out.
    This is the stdin/stdout contract of tectum's ``streaming_stt_worker.py``
    verbatim, so that worker runs here unchanged, and a different engine is a
    different program rather than a branch in :mod:`gobbo.asr`.
  * the agent -- ``tau --mode rpc``, JSON-RPC 2.0 over stdio.

Neither is imported. Nothing heavy enters this interpreter, and either can be
swapped for anything that speaks the same lines.

The pet's *appearance* is the other half, and it is arranged the same way --
things that talk over a line of JSON, and a seam where policy goes:

  * :mod:`gobbo.petdex` and :mod:`gobbo.pet` -- a sprite sheet downloaded from
    petdex.dev, sliced into the nine animation states in :mod:`gobbo.states`.
  * :mod:`gobbo.control` -- a socket, so a process that is not this one can
    drive the pet. The verbs are the calling script's; this module only carries
    them, and hands each to the frame hook to run.
  * :mod:`gobbo.gobbopet` and :mod:`gobbo.emote` -- a τ extension, and the
    classifier it uses. **These two run in τ's interpreter, not this one**, so
    they and :mod:`gobbo.states` import no ``clippy``; anything that does is
    unreachable from there.
"""
