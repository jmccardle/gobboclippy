# The petdex format

What [petdex.dev](https://petdex.dev) actually publishes, verified against real
downloads rather than read off the marketing page. Written down because most of
it is not documented anywhere on their side, and the parts that bite are the
parts nobody would guess.

`scripts/gobbo/petdex.py` implements the fetching; `scripts/gobbo/states.py`
holds the table; this file is why they look the way they do.

## The catalogue

One static JSON file behind a redirect, no key, no npx:

```
https://petdex.dev/api/manifest/v2
  -> https://assets.petdex.dev/manifests/petdex-v2.json
```

It is **column-oriented**: a `fields` array naming the columns, then one array
per pet in that order. Same content as the v1 object form at
`/api/manifest`, at half the bytes (855 KB against 1.7 MB for ~4,800 pets).

```json
{
  "v": 2,
  "generatedAt": "...",
  "total": 4826,
  "assetBase": "https://assets.petdex.dev",
  "fields": ["slug", "displayName", "kind", "submittedBy",
             "spritesheet", "petJson", "zip", "spriteVersionNumber"],
  "pets": [["boba", "Boba", "creature", "...",
            "curated/boba/sprite-v2.webp", "...", "...", 1]]
}
```

The three URL columns are relative to `assetBase`. `kind` is one of
`character`, `creature`, `object`.

`/api/manifest/full` and `/api/pets/<slug>` return **401** — the richer
per-pet metadata the repository's README mentions (tags, vibes) lives behind
authentication and is not available to us.

## The sprite sheet

A grid of **192×208 cells, 8 columns**, one row per animation state.

| atlas | rows | pixels | share of the corpus |
|---|---|---|---|
| v1 | 9 | 1536×1872 | 4,611 |
| v2 | 11 | 1536×2288 | 215 |

**97% are WebP** (4,679 against 147 PNG), which is why libwebp is linked into
the binary. `Texture::load` decides by magic bytes rather than by extension,
because some sheets are served under a name that disagrees with their content.

v2's last two rows are unnamed. petdex's own docs say they are "available to the
consuming client", so what they mean is per-pet; `Pet.extra_rows` offers them as
row numbers and invents nothing.

## The animation table is not in the pet

This is the part worth knowing. A pet's `pet.json` is:

```json
{
  "id": "alder-catalog-foot",
  "displayName": "Alder Catalog Foot",
  "description": "A compact alder catalog foot with a rounded support pad.",
  "spritesheetPath": "spritesheet.webp"
}
```

Sampling 68 pets — 60 at random plus the curated set — gives:

```
  68/68  description          free-form, median 77 chars, multilingual
  67/68  displayName
  67/68  spritesheetPath
  66/68  id
  12/68  spriteVersionNumber
   2/68  kind
   1/68  characterProfile, layout_guides, style_notes, house_style, references,
          chroma_key, pet_notes, atlas, rows, schemaNote, pet_id, display_name,
          created_at, canonical_identity_reference, primary_generation_skill
```

Everything in that last row appears exactly once: individual submitters'
authoring notes leaking into the published file. Two pets use snake_case where
the rest use camelCase. **Nothing validates this.** Read `description`,
`displayName` and `spritesheetPath`; treat anything else as absent.

So there is **no animation metadata anywhere** — no state names, no frame
counts, no durations. What the rows mean is in petdex's *website* source,
`src/lib/pet-states.ts`, transcribed into `scripts/gobbo/states.py`:

| row | state | frames | loop ms |
|---|---|---|---|
| 0 | idle | 6 | 1100 |
| 1 | running-right | 8 | 1060 |
| 2 | running-left | 8 | 1060 |
| 3 | waving | 4 | 700 |
| 4 | jumping | 5 | 840 |
| 5 | failed | 8 | 1220 |
| 6 | waiting | 6 | 1010 |
| 7 | running | 6 | 820 |
| 8 | review | 6 | 1030 |

That the vocabulary is fixed across every pet rather than per-pet is what makes
it worth building tools on: `play(state)` is a closed enum of nine that works on
all 4,826, and a tool schema never has to be rebuilt when the pet changes.

**Use the frame counts.** Rows are padded to eight cells with fully transparent
frames, so a four-frame `waving` played across all eight columns spends half its
loop invisible. Counting non-empty cells instead is also wrong — some pets draw
art in cells the table says are unused. An alpha scan of a real v1 sheet gives
`6,8,8,4,5,8,6,6,6`, matching the table exactly.

## Two traps

**The CDN refuses `Python-urllib`.** A request carrying CPython's default
user-agent comes back `403 Forbidden`; any honest one is served. `petdex.py`
sends `gobboclippy (+https://github.com/jmccardle/gobboclippy)`.

**The catalogue's `spriteVersionNumber` can be wrong.** `boba` is listed as
v1 (9 rows) and its sheet has 11. `petdex.install()` checks the geometry of what
it downloaded, trusts the pixels, and logs the disagreement — the sheet is
self-describing and the metadata is not.

## Licensing

> Pets are user-submitted fan art. Petdex does not claim rights to any
> underlying IP.

The platform is MIT; pet assets are "owned by their submitters under whatever
license they choose to declare", and most declare none. The featured collections
are Pokémon and League of Legends, and there is a takedown path.

**So nothing from petdex is committed to this repository or shipped in a
release.** gobboclippy ships the ability to read the format; the user downloads
the art, at runtime, into their own `pref_path()`. The `licenses/` mechanism in
the package covers what the package redistributes, and it could not honestly
cover these.

The descriptions are also stranger-written text. Anything that forwards one to a
language model should fence it as data — `gobbopet.py`'s `pet_info` tool does.

## Quality varies

4,826 pets, of which **16 are curated** (they live under
`assets.petdex.dev/curated/`). A random draw from the rest produced a wooden
stool whose nine states are nearly identical frames; `boba`, from the curated
set, is real pixel art with blinks, a wave and a jump arc. Worth knowing before
building anything that picks a pet at random.

Most of the corpus is pixel art authored well below 192×208 and scaled up into
the cell, which is why `Pet` defaults to `smooth=False` — linear filtering turns
it to mush. The smooth vector-ish minority can pass `smooth=True`.
