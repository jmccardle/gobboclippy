"""The petdex atlas geometry and the nine animation states. Data, and nothing else.

This module imports nothing -- not clippy, not the rest of gobbo -- and that is
its job. Three different interpreters need these numbers:

  * gobboclippy's, to slice a sheet and animate it (:mod:`gobbo.pet`);
  * gobboclippy's again, to check a download before keeping it
    (:mod:`gobbo.petdex`);
  * **τ's**, to describe the tools an agent may call (:mod:`gobbo.gobbopet`),
    where there is no host, no window and no ``clippy`` to import at all.

Putting the table in the module that draws with it would have made the third
impossible, which is how it ended up here.

**Where the numbers come from.** petdex ships no animation metadata with a pet:
``pet.json`` carries a name, a description and a file name, and says nothing
about rows, frame counts or timing. This table is transcribed from petdex's own
site source, ``src/lib/pet-states.ts``, which is where its renderer's numbers
actually live. So it is a fact about petdex that has been copied, and when
petdex changes it this is a diff here rather than a mystery about why the walk
cycle stutters.

The frame counts are not decoration. Rows are padded out to 8 cells with fully
transparent frames, so a 4-frame state played across all 8 columns spends half
its loop invisible. Counting non-empty cells instead is also wrong: some pets
draw art in cells the table says are unused.
"""

# A cell, and how many sit side by side. Both atlas versions share these; they
# differ only in how many rows follow.
CELL_W, CELL_H = 192, 208
COLUMNS = 8

# spriteVersionNumber -> rows. v2's last two rows are unnamed: petdex's own
# docs say they are "available to the consuming client", so what they mean is
# per-pet and nothing here invents a meaning for them.
ATLAS_ROWS = {1: 9, 2: 11}

# name -> (row, frames, loop milliseconds, what it is for)
STATES = {
    "idle":          (0, 6, 1100, "Neutral breathing and blinking loop"),
    "running-right": (1, 8, 1060, "Directional locomotion to the right"),
    "running-left":  (2, 8, 1060, "Directional locomotion to the left"),
    "waving":        (3, 4,  700, "Greeting or attention gesture"),
    "jumping":       (4, 5,  840, "Anticipation, lift, peak, descent, settle"),
    "failed":        (5, 8, 1220, "Readable error or sad reaction"),
    "waiting":       (6, 6, 1010, "Patient idle variant"),
    "running":       (7, 6,  820, "Generic in-place run loop"),
    "review":        (8, 6, 1030, "Focused inspecting or thinking loop"),
}

NAMES = tuple(STATES)

# Where a pet goes when it has nothing else to do.
RESTING = "idle"


def frames(state):
    """The sprite_index run for ``state``, as a list."""
    row, count, _, _ = lookup(state)
    base = row * COLUMNS
    return [base + i for i in range(count)]


def duration(state):
    """How long one loop of ``state`` takes, in seconds."""
    return lookup(state)[2] / 1000.0


def purpose(state):
    """What the state is for, in petdex's own words."""
    return lookup(state)[3]


def lookup(state):
    try:
        return STATES[state]
    except KeyError:
        raise ValueError(
            f"{state!r} is not a petdex state. Known: "
            f"{', '.join(NAMES)}") from None
