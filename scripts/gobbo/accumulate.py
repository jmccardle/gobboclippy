"""The utterance accumulator: the seam between hearing and answering.

Ported from tectum's ``audio.accumulator`` node, minus the bus. Partials revise
an in-flight hypothesis; the endpoint-committed ``final`` freezes it into one
finished utterance. tectum keys its in-flight turns by binding id because it
serves a whole substrate; here there is one microphone and one worker, so there
is one in-flight turn.

It is thin on purpose, and it exists as its own module for what goes here next
rather than for what it does now: a wake-word gate, a name resolver, an
end-of-utterance projection. The agent is deliberately not fed a raw transcript
stream, and this is the place that decides what it is fed instead.
"""

from collections import deque

HISTORY = 50


def render_tagged(words, overlapping, mode="guess"):
    """Word list -> readable text with the two LDT tags spliced back in.

    A run of low-confidence words is one unclear *span*. How it renders:

      * ``"guess"``  ``<unclear:army then>`` -- keeps the engine's best guess
        inside the tag. Strictly more information than a bare marker: a human
        can often read through it and a model can weigh it against context.
      * ``"tag"``    a bare ``<unclear>``.
      * ``"plain"``  the guessed word alone; the uncertainty survives only in
        the structured ``unclear`` list.

    A turn containing overlapped speech is prefixed ``<overlapping speech>``.
    """
    parts = []
    run = []

    def flush_run():
        if not run:
            return
        if mode == "tag":
            parts.append("<unclear>")
        elif mode == "plain":
            parts.extend(w.lower() for w in run)
        else:
            parts.append("<unclear:" + " ".join(w.lower() for w in run) + ">")
        run.clear()

    for w in words:
        if w.get("unclear"):
            run.append(str(w.get("w", "")))
        else:
            flush_run()
            parts.append(str(w.get("w", "")).lower())
    flush_run()

    tagged = " ".join(p for p in parts if p)
    if overlapping:
        tagged = "<overlapping speech> " + tagged
    return tagged


class Accumulator:
    def __init__(self, unclear_render="guess"):
        self.unclear_render = unclear_render
        self.history = deque(maxlen=HISTORY)
        self._text = ""
        self._updates = 0
        self._committed = 0

    @property
    def in_flight(self):
        """The best current hypothesis for the turn being spoken right now."""
        return self._text

    def partial(self, msg):
        self._text = msg.get("text", self._text)
        self._updates += 1
        return self._text

    def final(self, msg):
        """Freeze the turn, attach the speaker, return one finished utterance."""
        updates = self._updates
        self._text = ""
        self._updates = 0
        self._committed += 1

        label = msg.get("speaker") or "?"
        words = msg.get("words") or []
        overlapping = msg.get("overlapping") or []

        utterance = {
            "turn_index": self._committed,
            "engine_turn": msg.get("turn"),
            # '?' is UNASSIGNED and stays that way; a speaker is never
            # inherited from the turn before it.
            "speaker": None if label == "?" else label,
            "speaker_label": label,
            "text": msg.get("text", ""),
            "tagged_text": render_tagged(words, overlapping, self.unclear_render),
            "words": words,
            "unclear": msg.get("unclear") or [],
            "overlapping": overlapping,
            "confidence": msg.get("avg_conf"),
            "duration": msg.get("duration"),
            "partials_accumulated": updates,
        }
        self.history.append(utterance)
        return utterance
