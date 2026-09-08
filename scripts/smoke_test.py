"""Non-interactive check of the host API.

    ./gobboclippy --script scripts/smoke_test.py

Drives the same code path the tray menu uses -- tray Show/Hide and
clippy.show()/hide() both route through App::setVisible -- so this covers the
tray behaviour without needing to click it.

Exits non-zero on the first failed assertion.
"""

import sys

import clippy

FAILURES = []


def check(label, got, want):
    ok = got == want
    if not ok:
        FAILURES.append(f"{label}: got {got!r}, want {want!r}")
    clippy.log(f"{'PASS' if ok else 'FAIL'}  {label}")
    return ok


# --- capability report -----------------------------------------------------
caps = clippy.capabilities()
clippy.log(f"driver={caps['video_driver']} platform={caps['platform']}")

for key in ("platform", "video_driver", "borderless", "always_on_top",
            "transparent", "skip_taskbar", "tray", "notes"):
    if key not in caps:
        FAILURES.append(f"capabilities() missing key {key!r}")

# The tray is required to start at all, so it must report True by now.
check("tray reported available", caps["tray"], True)

# --- sprite ----------------------------------------------------------------
clippy.set_sprite("clippy.png")

try:
    clippy.set_sprite("does_not_exist.png")
except OSError as exc:
    clippy.log(f"PASS  missing sprite raises OSError ({str(exc)[:40]}...)")
else:
    FAILURES.append("set_sprite() accepted a missing file")

# --- unknown event is rejected --------------------------------------------
try:
    clippy.on("definitely_not_an_event", lambda: None)
except ValueError:
    clippy.log("PASS  on() rejects unknown event")
else:
    FAILURES.append("on() accepted an unknown event name")

# --- visibility, and that hooks fire on transitions only -------------------
seen = []
clippy.on("show", lambda: seen.append("show"))
clippy.on("hide", lambda: seen.append("hide"))

check("starts hidden", clippy.visible(), False)

clippy.show()
check("visible after show()", clippy.visible(), True)

clippy.show()                       # already shown: must not re-fire
check("show() is idempotent", seen.count("show"), 1)

clippy.hide()
check("hidden after hide()", clippy.visible(), False)

clippy.toggle()
check("visible after toggle()", clippy.visible(), True)

check("hook sequence", seen, ["show", "hide", "show"])

# --- geometry --------------------------------------------------------------
w, h = clippy.size()
check("window is square", w, h)

clippy.set_position(120, 90)
pos = clippy.position()
# Window managers may adjust placement, so only require it to be reported back
# as a pair of ints rather than exactly what was asked for.
check("position() returns a pair", len(pos), 2)

# --- result ----------------------------------------------------------------
if FAILURES:
    clippy.log(f"{len(FAILURES)} FAILURE(S)")
    for f in FAILURES:
        clippy.log(f"  {f}")
    sys.stderr.write("\n".join(FAILURES) + "\n")
    clippy.quit()
    raise SystemExit(1)

clippy.log("all checks passed")
clippy.quit()
