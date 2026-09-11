"""The τ extension, loaded by τ's own loader and pointed at a live pet.

    ./gobboclippy --script scripts/pet_demo.py &
    ~/path/to/tau-venv/bin/python tests/test_gobbopet.py

Two things need to be true for this to run, and it says which is missing rather
than skipping quietly: τ has to be importable by the interpreter running this,
and a gobboclippy serving the control channel has to be up. Neither is true on a
CI runner -- there is no display and no τ -- so this is a local test, and
tests/test_emote.py is the half that runs anywhere.

What it proves is the claim the whole design rests on: the tools and the
turn-end emote are independent, and they do not fight.
"""

import asyncio
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(HERE, "..", "scripts")
EXT = os.path.join(SCRIPTS, "gobbo", "gobbopet.py")

sys.path.insert(0, SCRIPTS)

try:
    from tau_agent_core.extension_types import ExtensionAPI
    from tau_agent_core.extensions.runner import ExtensionHandlers
    from tau_agent_core.sdk import _load_extensions
except ImportError as exc:
    raise SystemExit(
        f"tau is not importable by this interpreter ({exc}).\n"
        "Run this with the python of a venv that has ffwf-tau-coding-agent "
        "installed -- the extension runs in tau's interpreter, so the test "
        "does too.")

from gobbo import control        # noqa: E402

FAILURES = []


def check(label, got, want):
    if got != want:
        FAILURES.append(f"{label}: got {got!r}, want {want!r}")
    print(f"{'PASS' if got == want else 'FAIL'}  {label}")


def assistant(text="", calls=()):
    content = [{"type": "text", "text": text}] if text else []
    content += [{"type": "toolCall", "id": f"c{i}", "name": n, "arguments": {}}
                for i, n in enumerate(calls)]
    return {"role": "assistant", "content": content, "stop_reason": "stop"}


def tool_result(tool_name, is_error=False):
    return {"role": "toolResult", "tool_call_id": "c0", "tool_name": tool_name,
            "content": [], "is_error": is_error}


def endpoint():
    """Where the running host said it is listening, or an explanation."""
    path = os.environ.get("GOBBOCLIPPY_CONTROL")
    if not path:
        # Only this one lookup needs the host's own library, and it is why the
        # test is told the path when it cannot import clippy.
        try:
            import clippy
            path = os.path.join(clippy.pref_path(), control.ENDPOINT_FILE)
        except ImportError:
            raise SystemExit(
                "set GOBBOCLIPPY_CONTROL to the running pet's control.json "
                "(printed by scripts/pet_demo.py at startup); this "
                "interpreter has no clippy to ask.")
    if not os.path.exists(path):
        raise SystemExit(
            f"no control endpoint at {path} -- start one with\n"
            "  ./gobboclippy --script scripts/pet_demo.py")
    return path


def playing(ep):
    with control.Client(ep) as client:
        return client.call("state")["playing"]


async def main():
    ep = endpoint()
    buckets = {}

    def factory(path):
        bucket = ExtensionHandlers(path=path)
        buckets["it"] = bucket
        return ExtensionAPI(config={"endpoint": ep}, hook_handlers=bucket)

    result = await _load_extensions([EXT], discover=False, api_factory=factory)
    check("the extension loaded", [e.error for e in result.errors], [])

    api = result.extensions[0].api
    tools = api._registry.get_active_tools()
    check("it registered its tools", sorted(tools),
          ["pet_hide", "pet_info", "pet_play", "pet_say", "pet_show"])
    check("pet_play offers the nine states",
          len(tools["pet_play"].parameters["properties"]["state"]["enum"]), 9)

    # --- the model's half ---
    out = tools["pet_play"].execute("t1", {"state": "waving"}, None, None, None)
    check("pet_play reaches the pet", out.get("is_error"), None)
    check("and the pet is playing it", playing(ep), "waving")

    info = tools["pet_info"].execute("t2", {}, None, None, None)
    check("pet_info fences the submitter's text",
          "<pet-metadata>" in info["content"][0]["text"], True)

    # --- the no-model half ---
    handlers = buckets["it"].handlers.get("user_turn_end") or []
    check("it registered a user_turn_end handler", len(handlers), 1)
    emote_turn_ended = handlers[0]

    # A turn that wrote a file and never touched the pet: the classifier acts.
    emote_turn_ended(loop_turns=2,
                     messages=[assistant(calls=["write"]), tool_result("write"),
                               assistant("Done.")])
    check("a write turn makes the pet wave", playing(ep), "waving")

    # A turn where the model drove the pet itself: the classifier stands down,
    # even though a six-turn transcript would otherwise classify as 'running'.
    tools["pet_play"].execute("t3", {"state": "jumping"}, None, None, None)
    emote_turn_ended(loop_turns=6,
                     messages=[assistant(calls=["pet_play"]), assistant("There.")])
    check("the classifier yields to the model", playing(ep), "jumping")

    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURE(S)")
        for f in FAILURES:
            print(f"  {f}")
        raise SystemExit(1)
    print("\nall gobbopet checks passed")


asyncio.run(main())
