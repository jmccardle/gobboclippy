"""Pick an emote from what an agent turn did.

This is deliberately a pure function over the two arguments τ's ``user_turn_end``
hook is handed -- ``loop_turns`` and ``messages`` -- so it can be tested against
literal dicts with no τ, no gobboclippy, no model and no network. Nothing here
imports anything but the standard library.

**It classifies what happened, not what was said.** petdex's nine state names
are conspicuously event-shaped -- ``failed``, ``waiting``, ``review``,
``running`` -- and a turn already knows whether its tools errored, how many
round trips it took and whether it wrote anything. Reading the assistant's prose
to guess a mood would be less accurate, English-only, and would change its answer
when the same work is described differently.

One signal is text-derived and is marked where it appears: a reply ending in a
question mark means the agent is waiting on the user. That is a fact about the
shape of the turn rather than about its sentiment, which is why it is the one
that earns its place.

The message shapes this reads are τ's, from tau_llm/types.py:

    {"role": "assistant",   "content": [{"type": "text"|"toolCall", ...}],
     "stop_reason": ...}
    {"role": "toolResult",  "tool_name": str, "is_error": bool, ...}
"""

# Tools whose use means the turn changed something on disk. Tuned for τ's
# built-ins; a caller with different tools passes its own set.
WRITE_TOOLS = frozenset({"write", "edit", "multi_edit", "apply_patch", "notebook_edit"})

# Above this many agent-loop turns, the turn was work rather than an answer.
BUSY_TURNS = 4

DEFAULT = "review"


def classify(loop_turns, messages, write_tools=WRITE_TOOLS, busy_turns=BUSY_TURNS):
    """One of petdex's nine state names, describing the turn that just ended.

    ``loop_turns`` is how many agent-loop turns the user's one utterance
    consumed; ``messages`` is that turn's transcript.
    """
    calls = tool_calls(messages)
    errored = any(m.get("is_error") for m in messages
                  if m.get("role") == "toolResult")

    # An error is the most important thing that can have happened, so it wins
    # over everything below regardless of how the turn otherwise went.
    if errored or _stopped_badly(messages):
        return "failed"

    if any(name in write_tools for name in calls):
        return "waving"

    if loop_turns >= busy_turns or len(calls) >= busy_turns:
        return "running"

    # The one text-derived signal -- see the module docstring. A turn that ends
    # in a question is a turn waiting on a person.
    if reply_text(messages).rstrip().endswith("?"):
        return "waiting"

    return DEFAULT


def tool_calls(messages):
    """Every tool name called in this turn, in order, including repeats."""
    names = []
    for m in messages:
        if m.get("role") != "assistant":
            continue
        for block in m.get("content") or ():
            if isinstance(block, dict) and block.get("type") == "toolCall":
                names.append(block.get("name"))
    return names


def reply_text(messages):
    """The assistant's final text block, or ''."""
    for m in reversed(messages):
        if m.get("role") != "assistant":
            continue
        parts = [b.get("text", "") for b in (m.get("content") or ())
                 if isinstance(b, dict) and b.get("type") == "text"]
        if parts:
            return "\n".join(parts)
    return ""


def _stopped_badly(messages):
    """Whether the last assistant message ended in an error stop.

    ``length`` and ``aborted`` are not errors -- the turn was cut short, which
    is a different thing from having gone wrong, and a pet that looks stricken
    every time an answer hits the token ceiling is crying wolf.
    """
    for m in reversed(messages):
        if m.get("role") == "assistant":
            return m.get("stop_reason") == "error"
    return False


def touched_appearance(messages, names):
    """Whether this turn already called one of ``names``.

    This is how the turn-end classifier keeps out of the model's way when the
    model drove the pet itself. Reading it out of the transcript rather than
    tracking a flag is the point: there is no per-turn state to set, and
    therefore none to forget to clear.
    """
    wanted = frozenset(names)
    return any(name in wanted for name in tool_calls(messages))
