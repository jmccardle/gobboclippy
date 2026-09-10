# macOS

Built by GitHub CI, on hosted arm64 runners, as a `.dmg` containing a signed
`.app`. Nothing here has ever been run on real Apple hardware by a human: CI
proves the package assembles, seals, mounts and starts, and everything that
needs a person in front of a screen — the microphone prompt, the Dock, the
menu bar — is unverified. Where that is the case below, it says so.

The research sections are kept so the decisions do not have to be re-derived.

## Why it cannot be cross-compiled

Apple's Xcode and Apple SDKs Agreement blocks it three separate ways: the
header restricts execution to Apple-branded hardware, §2.5 independently
forbids "separately using the Apple SDKs ... on non-Apple-branded hardware",
and §2.7 forbids enabling others to do so. osxcross is maintained and works,
but there is no freely redistributable macOS SDK to feed it.

Note §2.4 does permit distributing macOS apps outside the App Store without a
separate agreement. It is the *building on non-Apple hardware* that is barred,
not the shipping.

Running macOS in a VM on this hardware is barred by the same document: the
virtualisation grant is conditioned on an Apple-branded host. It is also a dead
end technically — the x86-on-Linux VM projects cannot host Apple Silicon, and
macOS 26 is the last Intel release.

## Options

**Recommended: mirror to a public GitHub repo and build on `macos-latest`.**
GitHub-hosted runners are free and unmetered for public repositories, macOS
included; `macos-latest` is arm64. Real Xcode, real `codesign`, real `hdiutil`,
$0. The mirror PAT needs the `workflow` scope or pushes touching
`.github/workflows/` are rejected.

**Self-hosted alternative: Scaleway Mac mini M1**, ~€0.11/hr with Apple's
mandatory 24-hour minimum lease, so roughly €2.64 per release burst. Runs
`forgejo-runner` with the `host` label against forge.goblincorps.com. Note
Forgejo's runner publishes **Linux binaries only** — it is Go, so
`GOOS=darwin go build` works, but you build it yourself. (Gitea's upstream
`act_runner` does publish darwin builds; the forks are not interchangeable.)

MacStadium's free OSS program is real and would be ideal, but applications are
currently closed. Cirrus CI stopped running jobs in June 2026.

## The bundle

`cmake/Package.cmake` stages the POSIX layout unchanged and puts it inside an
app bundle:

```
gobboclippy-0.1.0-macOS/          the DMG's volume
  Applications -> /Applications   the drag target
  gobboclippy.app/Contents/
    Info.plist                    cmake/Info.plist.in, configured
    MacOS/gobboclippy             the binary, alone
    Resources/                    the entire payload, verbatim
      assets/ scripts/ licenses/ site/
      lib/  libSDL3.0.dylib  gobboclippy.icns
      python3 -> ../MacOS/gobboclippy
    _CodeSignature/CodeResources  the seal, written last
```

No C++ changed for this. `SDL_GetBasePath` answers with the *resource*
directory for a bundled app and with the executable's directory otherwise, and
`AppPaths.cpp` resolves every runtime path from that one call, so both layouts
land on the directory that holds `assets/`. The executable gained two rpath
entries (`@executable_path/../Resources` and `.../Resources/lib`) because the
libraries are now one directory away from it; an rpath entry naming a
directory that does not exist is skipped, so the build tree is unaffected.

The interpreter alias stays in `Resources/` — `sys.executable` has to sit under
the same root as `sys.base_prefix` — and points back at `../MacOS/gobboclippy`.
Launching *through* that symlink lands on the same base path either way: if
`_NSGetExecutablePath` reports the symlink, `NSBundle` treats `Resources/` as a
plain directory and `resourcePath` is that directory; if it reports the resolved
path, `NSBundle` finds the `.app` and `resourcePath` is the same directory
again.

Two keys are deliberately **absent** from `Info.plist`.
`SDL_FILESYSTEM_BASE_DIR_TYPE` is left at SDL's default of `resource`; setting
it to `bundle` or `parent` would put the payload outside `Contents/`, which is
a malformed bundle. `LSUIElement` is left out because removing the Dock icon
needs `SDL_HINT_MAC_BACKGROUND_APP` set before `SDL_Init` as well (see the Dock
icon note below), the two have to land together, and neither can be tested from
here.

### The bundle writes into itself

`site/` is `sys.prefix`, and it is inside `Contents/Resources/`. So
`gobboclippy --python -m pip install <x>`, which is how the interpreter mode is
meant to be used, adds files to a directory the bundle signature seals.

Three consequences, in order of how much they matter:

- **`codesign --verify` fails afterwards.** Not at launch: the kernel checks
  the main executable, and `pip` does not touch it. Gatekeeper's assessment
  runs once, when a quarantined app is first opened, and `pip` has not run yet
  at that point. So the app keeps working and stops being verifiable, which is
  a bad combination to discover during a support conversation rather than here.
- **An app run from the mounted DMG cannot install anything**, because the
  image is read-only. That is the second reason the volume ships an
  `Applications` symlink, after App Translocation.
- **The TCC grant survives it**, because an ad-hoc designated requirement is
  the executable's stored cdhash and `pip` does not rewrite the executable.

The fix, if this is ever worth fixing, is to move `sys.prefix` out of the
bundle on macOS — `~/Library/Application Support/gobboclippy/site` is where
`SDL_GetPrefPath` already points, and `scripts/gobbo/config.py` already writes
there. That is a change to how `src/main.cpp` derives the prefix and it makes
macOS the one platform whose layout differs, so it is a decision rather than a
tidy-up, and it is not made here.

**Ship a DMG, not a ZIP.** Archive Utility propagates the quarantine attribute
onto extracted files, which triggers App Translocation — the app runs from a
randomised read-only path. That breaks executable-relative path resolution,
which is exactly how this project finds `assets/`, `scripts/` and its bundled
interpreter. Dragging out of a mounted image is a move the system recognises,
and translocation does not happen. The image is plain UDZO over HFS+ with no
`.DS_Store`, so the window has no custom background or icon placement; that is
cosmetic and needs AppleScript against Finder to fix.

## Signing

**Ad-hoc sign every arm64 binary regardless of any other decision.** arm64
Mach-O binaries without at least an ad-hoc signature are killed by the kernel,
which is a separate and harder failure than Gatekeeper. Linkers do this
automatically, but `install_name_tool` invalidates it — so rewrite install
names *first*, then sign, inside-out. `cmake/MacFinalize.cmake` does this:
relocate, strip, sign each Mach-O with the executable last, then seal the
bundle.

Sealing the bundle is a different act from signing the binaries in it, not a
repetition. It hashes every file under `Contents/` into
`_CodeSignature/CodeResources` and binds the result to `CFBundleIdentifier` —
which is what the microphone grant is recorded against.

### What each level actually buys

|                                                  | ad-hoc (today) | Developer ID + notarised |
|--------------------------------------------------|----------------|--------------------------|
| arm64 binaries run at all                         | yes            | yes                      |
| microphone prompt appears                         | yes            | yes                      |
| opens from Finder after being downloaded          | **no**: Settings → Privacy & Security → "Open Anyway", with an admin password | yes |
| microphone grant survives the next release        | **no**         | yes                      |
| `spctl -a -t open --context context:primary-signature` passes | no | yes                   |

The third row is the familiar one. **The fourth is the one that matters more
here, and it is not obvious.** For an ad-hoc signature the designated
requirement is the main executable's `cdhash`, and the resource seal is hashed
into that same code directory as a special slot — so changing *any* file in the
bundle, a `.py` script included, changes the cdhash. TCC then sees a different
application, and the user answers the microphone prompt again. With a Developer
ID the requirement is the identifier plus the team, both stable across builds,
and the prompt is answered once ever.

So for a program whose whole point is that it listens, the $99/yr is buying
"asked once" rather than "asked on every update". That is a better argument for
it than Gatekeeper is.

Beyond ad-hoc it is one atomic decision, not two: signing without notarising
buys almost nothing. Unsigned or Developer-ID-without-notarisation both land
the user in System Settings → Privacy & Security → "Open Anyway" **with an
admin authentication prompt**. Sequoia removed the Control-click bypass and it
has not come back.

Both steps can run on Linux, which is worth exploiting: build unsigned on
GitHub, then sign and notarise on the Forgejo runner with `rcodesign`, so the
Developer ID key never enters GitHub secrets. Apple's Notary API is REST and
explicitly documented for avoiding a macOS dependency; `notarytool` itself is
Mac-only. The certificate can be obtained from Linux too — OpenSSL keypair and
CSR, upload, download the `.cer`, export a `.p12`. Apple Developer Program
membership is $99/yr with no OSS waiver.

Notarisation is per-artifact and the ticket is stapled to the artifact, so a
new `.dmg` is a new submission. It is a network round trip of a few minutes,
not a rebuild.

## Repacking a release

The idea is sound and worth writing down carefully, because the thing it saves
is not the thing it looks like it saves.

Most changes to this project are Python: `scripts/` and `scripts/gobbo/` are
where the assistant actually lives, and the C++ underneath it changes rarely.
It is tempting to take the previous release's `.app`, swap
`Contents/Resources/scripts/`, and ship that.

**No signature survives that, and none can.** The resource seal covers every
file under `Contents/`, and its hash sits in the main executable's code
directory. Replacing one `.py` invalidates the bundle, and re-signing produces
a new cdhash even though the binary is byte-identical. Under an ad-hoc
signature that also costs the user's microphone grant, exactly as a rebuild
would. There is no arrangement of resource rules that helps: excluding
`scripts/` from the seal would mean shipping an app whose behaviour is not
covered by its signature, which is the property the seal exists to provide.

What a repack *does* save is the macOS machine. Every step needed to turn last
release's `.app` plus this release's scripts into a signed, notarised artifact
has a Linux implementation:

  1. unpack the previous `.app` (it is a directory; `hdiutil` is not needed to
     read one out of a `.dmg` on Linux, but `7z`-class tools are),
  2. replace `Contents/Resources/scripts/`,
  3. `rcodesign sign` the bundle with the Developer ID `.p12`,
  4. `rcodesign notary-submit --staple`.

The one step with no good Linux tool is **building the `.dmg` itself** —
`hdiutil` is Mac-only, and `libdmg-hfsplus` is the usual substitute but is
unmaintained. That is the open question if this is ever wanted; until it is
answered, a repack still needs a macOS runner for one `hdiutil create`, and
GitHub gives that away for free on a public repo, which is why none of this is
implemented today.

It becomes worth implementing when the build stops being free: the Scaleway Mac
mini path costs a 24-hour minimum lease per burst, and a scripts-only release
that never touches a Mac is then a real saving rather than a tidiness.

Whatever the route, the check is the same one CI runs:
`codesign --verify --deep --strict gobboclippy.app`. A repack that skips
re-signing produces a bundle that still launches on the machine that made it —
resource seals are enforced by Gatekeeper, not by the kernel — and is refused
on the first machine that downloads it. That is the failure mode to test for,
and `--verify` is what catches it before a user does.

## Technical notes for this app

**Tray.** SDL's Cocoa backend is real (`NSStatusBar`/`NSStatusItem`), must be
called on the main thread, and `SDL_UpdateTrays()` is a no-op there. Two
gotchas: it never calls `setTemplate:YES`, so the icon will not auto-tint for
light and dark menu bars — pick a colour that survives both; and it hard-resizes
to 22×22pt, so supply a 44×44px surface for Retina.

**Dock icon.** SDL forces `NSApplicationActivationPolicyRegular` in
`Cocoa_RegisterApp()` unless `SDL_HINT_MAC_BACKGROUND_APP` is set *before*
`SDL_Init`. Even then SDL does not set `Accessory` for you, so a menu-bar-only
pet needs both that hint and `LSUIElement=YES` in `Info.plist`.

The `Info.plist` half now exists and is deliberately not set: half of a
two-part change is worse than neither half, and the observable difference is a
Dock icon appearing or not, which no runner can report. It is a one-key edit to
`cmake/Info.plist.in` and one `SDL_SetHint` in `src/main.cpp` whenever somebody
with a Mac can watch it happen.

**Window flags.** `ALWAYS_ON_TOP` maps to `NSFloatingWindowLevel`, which floats
above normal windows but below the menu bar and fullscreen Spaces.
`TRANSPARENT` and `BORDERLESS` are implemented.

**Microphone.** The one capability that a bare directory cannot have, and the
reason the macOS package is a bundle. macOS gates the microphone through TCC,
and TCC decides per *bundle*: the prompt a user answers is driven by
`NSMicrophoneUsageDescription` in the app's `Info.plist`, and the grant is
recorded against `CFBundleIdentifier`. A bare executable has neither — it
inherits the grant of whatever launched it, so it works from a terminal the
user has already allowed and has no path to a prompt of its own when
double-clicked in Finder.

Both keys are now in `cmake/Info.plist.in` and CI asserts they survive into the
built bundle, because a missing usage-description string does not produce an
error: the process is killed the moment it asks for the device.

What is still unverified is everything after that. No hosted runner has an
audio input device or a person to answer a prompt, so CI proves the bundle is
shaped to be *able* to ask, not that asking works. On the machine, the honest
behaviours hold either way: `--capabilities` reports what was actually granted,
and `clippy.mic.start()` raises with SDL's reason rather than recording
silence.

The identifier is `com.goblincorps.gobboclippy`, and it is a CMake cache
variable (`GC_BUNDLE_ID`) rather than a literal in the plist. A fork that keeps
the default would share one entry in the user's privacy settings with this
project, and each one's release would disturb the other's grant.

**Bundled interpreter.** `astral-sh/python-build-standalone` is the right
source; take `install_only_stripped`. It ships `aarch64-apple-darwin` and
`x86_64-apple-darwin` separately — **there is no universal2 asset**, and `lipo`
cannot merge two Python installations, so a universal2 bundle would mean
carrying two complete CPython trees. Ship arm64-only: macOS 27 is Apple Silicon
only, and GitHub's x86_64 runner disappears in August 2027.

Its macOS `libpython` install name was hardcoded to a bogus build path until a
fix merged in December 2025; current releases are `@rpath`-relative, but you
still add your own `LC_RPATH`. Every nested `.dylib` and every Python
C-extension needs its own valid signature for notarisation, signed inside-out
with `--timestamp`. `codesign --deep` is deprecated and unreliable; do not use
it.
