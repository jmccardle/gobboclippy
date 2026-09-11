"""Drive a running gobboclippy from the shell.

    ./gobboclippy --python scripts/pet_ctl.py play waving
    ./gobboclippy --python scripts/pet_ctl.py state
    ./gobboclippy --python scripts/pet_ctl.py say "back in a minute"
    ./gobboclippy --python scripts/pet_ctl.py hide

It needs a gobboclippy running a script that serves the control channel --
scripts/pet_demo.py does. The verbs are whatever that script registered; ask a
host you do not know by sending it something it does not have, which answers
with the list.

This exists to keep the protocol honest. Everything an agent can do to the pet,
a shell can do, because they are the same six lines of client code; a REST head
would be the same again. Run under --python rather than --script: it is a
client, so it wants the interpreter, not a window.

Arguments are ``key=value`` pairs, or one bare value for a single-argument verb.
A value is JSON-decoded when it parses, so ``once=true`` is a boolean.
"""

import json
import os
import sys

# The interpreter mode does not put the script's own directory on sys.path the
# way --script does, and this needs its sibling package.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gobbo import control          # noqa: E402


# Which verb takes the bare positional value, when one is given. A host can
# serve any verbs at all, so this is a convenience for the common two rather
# than a description of the protocol.
POSITIONAL = {"play": "state", "say": "text"}

USAGE = __doc__.strip()


def parse(argv):
    if not argv:
        raise SystemExit(USAGE)

    verb, rest = argv[0], argv[1:]
    params = {}
    for arg in rest:
        if "=" in arg and not arg.startswith("="):
            key, _, raw = arg.partition("=")
            params[key] = _value(raw)
        elif verb in POSITIONAL and POSITIONAL[verb] not in params:
            params[POSITIONAL[verb]] = arg
        else:
            raise SystemExit(
                f"{verb}: don't know what to do with {arg!r}; "
                "pass arguments as key=value")
    return verb, params


def _value(raw):
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return raw              # a bare word is a string, which is the usual case


def main(argv):
    verb, params = parse(argv)

    endpoint = os.environ.get("GOBBOCLIPPY_CONTROL")
    if not endpoint:
        # Asking the host library where the host keeps its files is the one
        # thing a client may do without being told, and it is why this runs
        # under the gobboclippy interpreter rather than any python.
        endpoint = control.endpoint_path()

    try:
        with control.Client(endpoint) as client:
            result = client.call(verb, **params)
    except control.ControlError as e:
        sys.stderr.write(f"{e}\n")
        return 1

    if result is not None:
        print(json.dumps(result, indent=2, ensure_ascii=False)
              if isinstance(result, (dict, list)) else result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
