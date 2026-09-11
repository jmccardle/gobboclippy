"""The petdex.dev catalogue, and installing one pet out of it.

petdex publishes ~4,800 animated pets as plain HTTPS assets. There is no API
key, no npx, and nothing to run: one JSON file lists every pet, and each pet is
a sprite sheet plus a small metadata file.

**Nothing from petdex is ever bundled with gobboclippy.** The pets are
user-submitted fan art and petdex claims no rights to the underlying IP, so this
repository ships the ability to read the format and the user downloads the art.
Installed pets live under ``clippy.pref_path()``, never inside the source tree.

Three things about this service are load-bearing and were found the hard way:

  * **The CDN refuses ``Python-urllib``.** A request carrying the default
    user-agent comes back 403; any honest one is served. Hence ``USER_AGENT``,
    which names this program and its repository so an operator reading their
    logs can tell what is asking.
  * **The v2 manifest is column-oriented** -- a ``fields`` list naming the
    columns and one array per pet -- which is half the bytes of the v1 object
    form for identical content. It is the one fetched here.
  * **A pet carries no animation metadata whatsoever.** ``pet.json`` has a
    description and a file name. What the rows mean lives in :mod:`gobbo.pet`,
    transcribed from petdex's own site source, because it is not shipped with
    the asset.
"""

import json
import os
import urllib.error
import urllib.request

import clippy

from gobbo.states import ATLAS_ROWS, CELL_H, CELL_W, COLUMNS

MANIFEST_URL = "https://petdex.dev/api/manifest/v2"

# Identifying, and true. See the module docstring: the default is blocked.
USER_AGENT = "gobboclippy (+https://github.com/jmccardle/gobboclippy)"

TIMEOUT = 30


class PetdexError(RuntimeError):
    """Something about the catalogue or a download was wrong."""


def _root():
    return os.path.join(clippy.pref_path(), "pets")


def path(slug):
    """Where ``slug`` is or would be installed."""
    return os.path.join(_root(), slug)


def _get(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        raise PetdexError(f"{url}: HTTP {e.code} {e.reason}") from None
    except (urllib.error.URLError, OSError) as e:
        raise PetdexError(f"{url}: {e}") from None


# --- the catalogue ---------------------------------------------------------

def _cache_path():
    return os.path.join(clippy.pref_path(), "petdex-manifest.json")


def manifest(refresh=False):
    """Every published pet, as a list of dicts.

    Cached on disk after the first call. ``refresh=True`` re-fetches; there is
    no staleness timer, because a catalogue that silently re-downloads 850 KB
    on a schedule nobody asked for is worse than one that updates when told.
    """
    cache = _cache_path()
    if not refresh:
        try:
            with open(cache, "r", encoding="utf-8") as fh:
                return _rows(json.load(fh))
        except FileNotFoundError:
            pass
        except (json.JSONDecodeError, PetdexError):
            # A corrupt cache is not an absent one, but it is recoverable
            # without bothering anybody: the remote copy is authoritative.
            pass

    raw = _get(MANIFEST_URL)
    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as e:
        raise PetdexError(f"{MANIFEST_URL} did not return JSON: {e}") from None

    rows = _rows(doc)          # parse before writing: never cache what we cannot read
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, "wb") as fh:
        fh.write(raw)
    return rows


def _rows(doc):
    """Turn the column-oriented v2 document into dicts.

    ``fields`` names the columns and each pet is an array in that order, so the
    mapping is positional and a field the server adds later simply appears.
    """
    try:
        fields = doc["fields"]
        base = doc["assetBase"].rstrip("/")
        pets = doc["pets"]
    except (KeyError, AttributeError, TypeError) as e:
        raise PetdexError(f"manifest is not in the v2 shape: {e}") from None

    out = []
    for row in pets:
        pet = dict(zip(fields, row))
        # The three URL columns are stored relative to assetBase.
        for key in ("spritesheet", "petJson", "zip"):
            if pet.get(key):
                pet[key] = f"{base}/{pet[key]}"
        out.append(pet)
    return out


def find(slug, refresh=False):
    """One pet by exact slug, or ``None``."""
    for pet in manifest(refresh):
        if pet.get("slug") == slug:
            return pet
    return None


def search(query, limit=20, refresh=False):
    """Pets whose slug or display name contains ``query``, case-insensitively."""
    q = query.casefold()
    hits = []
    for pet in manifest(refresh):
        if q in pet.get("slug", "").casefold() or \
           q in pet.get("displayName", "").casefold():
            hits.append(pet)
            if len(hits) >= limit:
                break
    return hits


# --- installing ------------------------------------------------------------

def installed():
    """Slugs already on disk, sorted."""
    try:
        return sorted(d for d in os.listdir(_root())
                      if os.path.isdir(os.path.join(_root(), d)))
    except FileNotFoundError:
        return []


def install(slug, refresh=False):
    """Download one pet into ``pref_path()/pets/<slug>/``. Returns that path.

    The sheet is checked against the atlas geometry *before* the install is
    called done. A sheet that is not 8 columns of 192x208 is refused and named:
    loading it anyway would slice it into a grid that quietly drops whatever
    did not fit, and the failure would surface as an animation playing the
    wrong frames rather than as a bad download.
    """
    pet = find(slug, refresh)
    if pet is None:
        raise PetdexError(
            f"no pet with slug {slug!r} in the catalogue. "
            f"gobbo.petdex.search({slug!r}) looks for near misses, and "
            "manifest(refresh=True) re-fetches if it was published recently.")

    sheet_url = pet.get("spritesheet")
    if not sheet_url:
        raise PetdexError(f"{slug}: the catalogue lists no spritesheet for it")

    sheet = _get(sheet_url)
    width, height = _sheet_size(sheet)
    _check_geometry(slug, width, height, pet.get("spriteVersionNumber"))

    # The extension follows the bytes, not the URL: some sheets are served with
    # a name that disagrees with their content. Texture::load sniffs the magic
    # either way, but a file called .png holding WebP is a trap for every other
    # tool that looks at this directory.
    suffix = ".webp" if _is_webp(sheet) else ".png"

    dest = path(slug)
    os.makedirs(dest, exist_ok=True)
    with open(os.path.join(dest, "sprite" + suffix), "wb") as fh:
        fh.write(sheet)

    # pet.json is metadata only; a pet without one is still usable, so its
    # absence is recorded rather than fatal.
    meta = {k: pet.get(k) for k in
            ("slug", "displayName", "kind", "submittedBy", "spriteVersionNumber")}
    meta["sprite"] = "sprite" + suffix
    meta["width"], meta["height"] = width, height
    if pet.get("petJson"):
        try:
            meta["pet_json"] = json.loads(_get(pet["petJson"]))
        except (PetdexError, json.JSONDecodeError) as e:
            meta["pet_json_error"] = str(e)

    with open(os.path.join(dest, "pet.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, indent=2, ensure_ascii=False)

    return dest


def _is_webp(data):
    return len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WEBP"


def _sheet_size(data):
    """(width, height) of a PNG or a simple-format WebP, from its header.

    Both headers put the dimensions in a fixed place, so this needs no decoder
    -- the C++ side has one, and this runs before anything is written to disk.
    """
    if data[:8] == b"\x89PNG\r\n\x1a\n" and len(data) >= 24:
        return (int.from_bytes(data[16:20], "big"),
                int.from_bytes(data[20:24], "big"))

    if _is_webp(data) and len(data) >= 30:
        fmt = data[12:16]
        if fmt == b"VP8X":                       # extended: 24-bit, minus-one
            return (int.from_bytes(data[24:27], "little") + 1,
                    int.from_bytes(data[27:30], "little") + 1)
        if fmt == b"VP8L":                        # lossless: 14 bits each
            bits = int.from_bytes(data[21:25], "little")
            return ((bits & 0x3FFF) + 1, ((bits >> 14) & 0x3FFF) + 1)
        if fmt == b"VP8 ":                        # lossy: 14 bits each
            return (int.from_bytes(data[26:28], "little") & 0x3FFF,
                    int.from_bytes(data[28:30], "little") & 0x3FFF)

    raise PetdexError(
        "the downloaded sheet is neither a PNG nor a WebP "
        f"(first bytes: {data[:12]!r})")


def _check_geometry(slug, width, height, version):
    if width != COLUMNS * CELL_W or height % CELL_H:
        raise PetdexError(
            f"{slug}: sheet is {width}x{height}, which is not "
            f"{COLUMNS} columns of {CELL_W}x{CELL_H}. This is not a petdex "
            "atlas and slicing it would silently lose frames.")

    rows = height // CELL_H
    if rows not in ATLAS_ROWS.values():
        raise PetdexError(
            f"{slug}: sheet has {rows} rows; petdex publishes "
            f"{sorted(ATLAS_ROWS.values())}. A layout this does not know is "
            "refused rather than guessed at.")

    # A disagreement is worth saying out loud but is not fatal: the pixels are
    # what get sliced, and they are self-describing.
    expected = ATLAS_ROWS.get(version or 1)
    if expected is not None and rows != expected:
        clippy.log(f"! {slug}: catalogue says atlas v{version or 1} "
                   f"({expected} rows) but the sheet has {rows}; "
                   "trusting the sheet")
