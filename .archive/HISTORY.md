# Archive

Reasoning that has left `ROADMAP.md` because the work it explains is shipped.
Nothing here is open, and nothing here is a decision — decisions stay in the
roadmap's **Cuts**. This is the long form of arguments the roadmap now states in
a line or two, kept so that "why is it like that" has an answer that does not
need a `git log` archaeology session.

## 2026-09-16

Archived on cutting v0.6.0, from **Hearing**:

### Capturing a chord is not the same mechanism as grabbing one

The roadmap used to say the settings window's chord field needed nothing new
"because the host is already grabbing". That was wrong, and wrong in a way worth
recording: a grab observes *one registered chord*, and capture has to observe
*any of them*. What makes capture work instead is that the settings window **has
keyboard focus** — the one thing the pet never has — so ordinary SDL key events
are enough and no new platform facility was needed after all.

The grab is still involved, but the other way round. Capture has to *release*
the grab while it is listening, because a grabbed chord is not delivered to the
focused window at all. Without that release, the chord already in the box would
be the single chord impossible to re-choose, which is the sort of bug that only
shows up when someone tries to set the hotkey to what it already is.

### A chord that only toggled the microphone was broken in the one case it was built for

The first cut of the global hotkey wired the `hotkey` hook straight to the
double-click's handler. The double-click's handler assumes the pet is on screen,
which is *true of a double-click* — you cannot double-click a window that is not
there — and *false of a chord pressed from another window*, which is the only
situation the chord exists for.

A hidden pet is refused the microphone by design (`src/PyClippy.cpp`: the window
is hidden, and a hidden pet cannot show that it is recording). So the chord
raised, and the explanation was written onto a window nobody could see. Both
halves of that are the same mistake.

The fix made the chord own the whole gesture — summon, record, stop, dismiss —
and put it in `scripts/assistant.py`, because `clippy.py` has no microphone to
summon anything for. Three judgements inside it:

* **A pet that was already up stays up.** `Assistant.summoned` remembers who put
  the window on screen, so the chord only takes away what the chord brought.
  It is read on the stop path alone and recomputed on every summon, so a stale
  `True` can never be acted on.
* **A failed start keeps the pet.** The failure message is written on the pet's
  face; hiding the window carrying it is the original bug in miniature.
* **It is not the settings window's preview.** A preview is
  mapped-but-not-`shown`, and the host refuses to record in that state —
  correctly, because the visible pet *is* the recording indicator. What carries
  over is only the shape: on screen for a reason, gone when the reason ends,
  kept if the user asked separately. `Assistant.summoned` plays
  `App::m_preview_engaged`'s part without being it.

No files were archived: the test drivers for this work lived in the session
scratchpad rather than the tree, and there are no stray outputs or logs in the
repository.
