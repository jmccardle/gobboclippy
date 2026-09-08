# macOS

Not built yet. This is the research, so the decision does not have to be
re-derived later. Nothing here has been tested on real Apple hardware.

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

## Signing

**Ad-hoc sign every arm64 binary regardless of any other decision.** arm64
Mach-O binaries without at least an ad-hoc signature are killed by the kernel,
which is a separate and harder failure than Gatekeeper. Linkers do this
automatically, but `install_name_tool` invalidates it — so rewrite install
names *first*, then sign, inside-out.

Beyond that it is one atomic decision, not two: signing without notarising buys
almost nothing. Unsigned or Developer-ID-without-notarisation both land the
user in System Settings → Privacy & Security → "Open Anyway" **with an admin
authentication prompt**. Sequoia removed the Control-click bypass and it has
not come back.

Both steps can run on Linux, which is worth exploiting: build unsigned on
GitHub, then sign and notarise on the Forgejo runner with `rcodesign`, so the
Developer ID key never enters GitHub secrets. Apple's Notary API is REST and
explicitly documented for avoiding a macOS dependency; `notarytool` itself is
Mac-only. The certificate can be obtained from Linux too — OpenSSL keypair and
CSR, upload, download the `.cer`, export a `.p12`. Apple Developer Program
membership is $99/yr with no OSS waiver.

**Ship a DMG, not a ZIP.** Archive Utility propagates the quarantine attribute
onto extracted files, which triggers App Translocation — the app runs from a
randomised read-only path. That breaks executable-relative path resolution,
which is exactly how this project finds `assets/`, `scripts/` and its bundled
interpreter.

## Technical notes for this app

**Tray.** SDL's Cocoa backend is real (`NSStatusBar`/`NSStatusItem`), must be
called on the main thread, and `SDL_UpdateTrays()` is a no-op there. Two
gotchas: it never calls `setTemplate:YES`, so the icon will not auto-tint for
light and dark menu bars — pick a colour that survives both; and it hard-resizes
to 22×22pt, so supply a 44×44px surface for Retina.

**Dock icon.** SDL forces `NSApplicationActivationPolicyRegular` in
`Cocoa_RegisterApp()` unless `SDL_HINT_MAC_BACKGROUND_APP` is set *before*
`SDL_Init`. Even then SDL does not set `Accessory` for you, so a menu-bar-only
pet needs both that hint and `LSUIElement=YES` in `Info.plist`. That means a
real `.app` bundle, which is wanted anyway — SDL has documented menu-bar
misbehaviour for non-bundled binaries.

**Window flags.** `ALWAYS_ON_TOP` maps to `NSFloatingWindowLevel`, which floats
above normal windows but below the menu bar and fullscreen Spaces.
`TRANSPARENT` and `BORDERLESS` are implemented.

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
