"""A petdex sprite sheet as something on the stage.

The sheet is a grid of 192x208 cells, 8 to a row, one row per animation state.
gobboclippy's ``Texture`` is already exactly that model -- equal cells indexed
left-to-right then top-to-bottom -- so a state's frames are a contiguous run of
``sprite_index`` and playing one is the frame-sequence animation the drawing
layer was built around. There is no new drawing code here, only arithmetic.

The numbers behind that arithmetic -- which row, how many frames, how long --
are in :mod:`gobbo.states`, which imports nothing. They are needed by τ's
interpreter as well as this one, and anything in this file is unreachable from
there because this file needs a host.
"""

import json
import os

import clippy

from gobbo import petdex
from gobbo.states import (CELL_H, CELL_W, COLUMNS, NAMES, RESTING, STATES,
                          duration, frames, lookup)


class Pet:
    """One installed petdex pet, as a sprite on the stage.

    ``smooth`` defaults off. The corpus is overwhelmingly pixel art drawn well
    below 192x208 and scaled up into the cell, which linear filtering turns to
    mush; the smooth vector-ish pets in it are the minority and can say so.
    """

    def __init__(self, slug, smooth=False, **sprite_kwargs):
        self.slug = slug
        self.dir = petdex.path(slug)
        self.meta = _load_meta(self.dir, slug)

        sheet = os.path.join(self.dir, self.meta.get("sprite") or "")
        if not os.path.isfile(sheet):
            raise FileNotFoundError(
                f"no sprite sheet for {slug!r} at {sheet}. "
                f"gobbo.petdex.install({slug!r}) downloads it.")

        self.texture = clippy.Texture(sheet, CELL_W, CELL_H, smooth=smooth)
        self.rows = self.texture.sheet_height

        self.sprite = clippy.Sprite(texture=self.texture, sprite_index=0,
                                    name=f"pet:{slug}", **sprite_kwargs)
        self.state = None

        # Bumped by every play(). See play_once() for what it is for.
        self._generation = 0

    # --- what this pet can do ----------------------------------------------

    @property
    def states(self):
        """The state names this sheet actually has rows for."""
        return tuple(n for n, (row, *_) in STATES.items() if row < self.rows)

    @property
    def extra_rows(self):
        """Rows past the nine named ones.

        The v2 atlas has 11, and petdex names 9 of them -- its own docs say the
        remaining two are "available to the consuming client". Pets do draw in
        them, and what they mean is per-pet. They are offered as row numbers
        and left unnamed, because inventing names for them here would be this
        file claiming to know something petdex does not say.
        """
        return tuple(range(len(STATES), self.rows))

    def frames(self, state):
        """The sprite_index run for ``state``, as a list."""
        self._check(state)
        return frames(state)

    def duration(self, state):
        """How long one loop of ``state`` takes, in seconds."""
        self._check(state)
        return duration(state)

    def _check(self, state):
        """That the name exists, and that *this* sheet has a row for it.

        The first question is the table's -- lookup() raises with the nine
        names in the message. The second is this pet's, because a sheet can be
        shorter than the table it is described by.
        """
        row = lookup(state)[0]
        if row >= self.rows:
            raise ValueError(
                f"{self.slug} has {self.rows} rows and {state!r} is row {row}")

    # --- playing -----------------------------------------------------------

    def play(self, state, loop=True, callback=None):
        """Animate through ``state``'s frames. Loops until told otherwise."""
        sequence = self.frames(state)
        self._generation += 1
        self.state = state
        self.sprite.animate("sprite_index", sequence, self.duration(state),
                            loop=loop, callback=callback)
        return self

    def play_once(self, state, then=RESTING):
        """Play ``state`` through exactly once, then settle into ``then``.

        The mechanism behind "an override that holds for one loop and lets go".
        It does not decide that anything should behave that way -- callers do.

        The settle is conditional, and that is the whole of the subtlety here.
        A pending "go back to idle" belongs to the animation that scheduled it,
        and if something else has played since, that animation is over and its
        callback has no business speaking for the pet. Unconditional, a wave
        started a second before an agent plays `running` drags the pet back to
        idle mid-stride -- from code that already finished, which is a maddening
        thing to chase. Each play takes a generation number; a settle only fires
        while its own is still the current one.
        """
        mine = self._generation + 1        # what play() below is about to set

        def settle(*_):
            if self._generation == mine:
                self.play(then)

        return self.play(state, loop=False, callback=settle)

    # --- the pet's own text ------------------------------------------------

    @property
    def display_name(self):
        return self.meta.get("displayName") or self.slug

    @property
    def description(self):
        """The submitter's description, or ''.

        Stranger-written text. Anything that forwards this to a model should
        fence it as data; see gobbo/tau_ext.py, which does.
        """
        return ((self.meta.get("pet_json") or {}).get("description") or "")


def _load_meta(directory, slug):
    try:
        with open(os.path.join(directory, "pet.json"), "r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError:
        raise FileNotFoundError(
            f"{slug!r} is not installed ({directory} has no pet.json). "
            f"gobbo.petdex.install({slug!r}) downloads it.") from None
    except json.JSONDecodeError as e:
        raise ValueError(f"{directory}/pet.json is not valid JSON: {e}") from None
