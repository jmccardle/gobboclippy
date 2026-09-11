"""The turn-end emote classifier, tested with no tau, no clippy and no model.

    python3 tests/test_emote.py

That it runs under a bare interpreter is the point of gobbo/emote.py being a
pure function over two arguments: the classifier is the part with the judgement
in it, and judgement that can only be checked by talking to a language model is
judgement nobody checks.

The message dicts below are tau's shapes, from tau_llm/types.py -- an assistant
message whose content is text and toolCall blocks, and a toolResult carrying
tool_name and is_error.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "scripts"))

from gobbo import emote        # noqa: E402

FAILURES = []


def check(label, got, want):
    if got != want:
        FAILURES.append(f"{label}: got {got!r}, want {want!r}")
    print(f"{'PASS' if got == want else 'FAIL'}  {label}")


def assistant(text="", calls=(), stop_reason="stop"):
    content = [{"type": "text", "text": text}] if text else []
    content += [{"type": "toolCall", "id": f"c{i}", "name": name,
                 "arguments": {}} for i, name in enumerate(calls)]
    return {"role": "assistant", "content": content, "stop_reason": stop_reason}


def result(tool_name, is_error=False):
    return {"role": "toolResult", "tool_call_id": "c0", "tool_name": tool_name,
            "content": [{"type": "text", "text": "..."}], "is_error": is_error}


# --- what the turn did decides the emote -----------------------------------

check("a plain answer reviews",
      emote.classify(1, [assistant("The file is 40 lines long.")]),
      "review")

check("a failed tool is failed",
      emote.classify(2, [assistant(calls=["bash"]), result("bash", is_error=True),
                         assistant("That did not work.")]),
      "failed")

check("an error stop is failed",
      emote.classify(1, [assistant("", stop_reason="error")]),
      "failed")

check("writing a file waves",
      emote.classify(2, [assistant(calls=["write"]), result("write"),
                         assistant("Done.")]),
      "waving")

check("a long turn runs",
      emote.classify(6, [assistant(calls=["read"]), result("read"),
                         assistant("Here is the summary.")]),
      "running")

check("many tools in one turn runs",
      emote.classify(2, [assistant(calls=["read", "grep", "read", "grep"]),
                         result("read"), assistant("Found it.")]),
      "running")

check("a question waits",
      emote.classify(1, [assistant("Which of the two did you mean?")]),
      "waiting")

# Precedence: an error outranks everything, including a write in the same turn.
check("an error outranks a write",
      emote.classify(2, [assistant(calls=["write"]), result("write", is_error=True)]),
      "failed")

# A truncated answer is not a failed one.
check("hitting the length ceiling is not failure",
      emote.classify(1, [assistant("A very long ans", stop_reason="length")]),
      "review")

check("an empty turn still answers",
      emote.classify(0, []),
      "review")


# --- standing down for the model -------------------------------------------

check("a turn that moved the pet is detected",
      emote.touched_appearance([assistant(calls=["pet_play"])], ("pet_play",)),
      True)

check("a turn that did not is not",
      emote.touched_appearance([assistant(calls=["read", "write"])], ("pet_play",)),
      False)

check("only the named tools count",
      emote.touched_appearance([assistant(calls=["pet_info"])],
                               ("pet_play", "pet_show", "pet_hide")),
      False)


# --- the helpers the above rest on -----------------------------------------

check("tool_calls keeps order and repeats",
      emote.tool_calls([assistant(calls=["read", "read", "write"])]),
      ["read", "read", "write"])

check("reply_text takes the last assistant text",
      emote.reply_text([assistant("first"), result("read"), assistant("second")]),
      "second")

check("reply_text is empty when nothing was said",
      emote.reply_text([assistant(calls=["read"])]),
      "")


if FAILURES:
    print(f"\n{len(FAILURES)} FAILURE(S)")
    for f in FAILURES:
        print(f"  {f}")
    raise SystemExit(1)
print("\nall emote checks passed")
