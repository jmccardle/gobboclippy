"""One config file, in the one place the platform keeps such things.

``clippy.pref_path()`` is SDL's answer to "where does this application put a
user's files" -- ``~/.local/share/gobboclippy`` on Linux, ``%APPDATA%`` on
Windows, ``~/Library/Application Support`` on macOS -- so no path is compiled
in and there is no search order to get wrong. A fork of this tree, or a copy of
the package on somebody else's machine, reads its own.

A missing setting raises, and the message is the JSON to write and the file to
write it in. There is no default for something only the person running it can
know, and a guess that happened to be wrong would be a transcriber that never
answers rather than a sentence saying what to fix.
"""

import json
import os

import clippy

FILENAME = "config.json"


class Missing(RuntimeError):
    """A setting this assistant cannot invent for you."""


def path():
    return os.path.join(clippy.pref_path(), FILENAME)


def load():
    """The whole config, or an empty one if the file is not there yet.

    A file that exists and does not parse is an error: an unreadable config is
    not the same thing as an absent one, and silently treating it as empty
    would answer a typo with a missing microphone.
    """
    try:
        with open(path(), "r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError:
        return {}
    except json.JSONDecodeError as e:
        raise Missing(f"{path()} is not valid JSON: {e}") from None


def command(section, example, what):
    """The argv of a subprocess named in the config, as a list of strings."""
    section_cfg = load().get(section) or {}
    argv = section_cfg.get("command")

    if not argv:
        raise Missing(
            f"No {section}.command in {path()}\n\n"
            f"{what}\n\n"
            "Write the file with something like:\n\n"
            + json.dumps({section: {"command": example}}, indent=2)
        )

    if not isinstance(argv, list) or not all(isinstance(a, str) for a in argv):
        raise Missing(
            f"{path()}: {section}.command must be a list of strings, "
            f"got {argv!r}"
        )
    return argv


def setting(section, key, default):
    """A tunable with a sane default -- unlike a command, these can have one."""
    return (load().get(section) or {}).get(key, default)
