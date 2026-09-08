# Cross-compiling from Debian

## Summary

| target | buildable on a Debian podman runner? | testable there? |
|---|---|---|
| Linux | yes, native | yes |
| Windows | **yes**, mingw-w64 cross-compile | partly, via wine |
| macOS | **no** | no |

Only macOS needs a hosted runner. That is an Apple licensing constraint, not
a tooling gap: cross-compiling to macOS needs the macOS SDK, which may only be
used on Apple hardware. osxcross works but requires an SDK you extracted from
Xcode on a Mac yourself, and redistributing it is prohibited. Codesigning and
notarisation also require macOS. So: build Linux and Windows on your own
runner, and use a hosted macOS runner (or a Mac) for the third.

## Windows

Working today:

```sh
sudo apt install mingw-w64 mingw-w64-tools

cmake -B build-windows \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
  -DGC_WINPY_ROOT=deps/winpy \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows -j$(nproc)
cmake --build build-windows --target package
```

Produces `gobboclippy-<version>-Windows.zip`, about 14 MB.

Two things that bit during setup and are now handled:

**Use the `-posix` compiler variants.** The `-win32` threading model has no
`std::mutex`, and the failure is a wall of template errors inside `<mutex>`
that never names the cause.

**Link with `-static`, not just `-static-libgcc -static-libstdc++`.** The
posix threading model links `libwinpthread-1.dll` dynamically, and those two
narrower flags do not cover it. The result loads on a machine that happens to
have that DLL and dies with `STATUS_DLL_NOT_FOUND` (0xc0000135) on a clean
Windows box. After the fix the only imports are `python3XX.dll`, `SDL3.dll`,
`KERNEL32.dll` and `msvcrt.dll`.

Note that `CMAKE_EXE_LINKER_FLAGS_INIT` only applies on a *fresh* configure.
Changing it in the toolchain and re-running cmake over an existing build
directory silently keeps the old cached flags.

### The Windows CPython kit

`find_package(Python3)` cannot help when cross-compiling — it finds the build
machine's Linux interpreter and hands back a `libpython3.11.so` that cannot be
linked into a PE binary. So the runtime is supplied explicitly via
`-DGC_WINPY_ROOT`. Expected layout:

```
<kit>/
  include/          Python.h, pyconfig.h and the rest of Include/
  libpython3XX.a    MinGW import library
  python3XX.dll     the runtime
  python3XX.zip     stdlib as .pyc
  Lib/              stdlib as loose .py
  DLLs/*.pyd        stdlib C extensions (omit if built into the DLL)
  *.dll             their dependencies (libssl, libffi, sqlite3, vcruntime)
```

`cmake/WindowsPython.cmake` validates this and fails loudly on a missing
piece, because a half-assembled kit links fine and then dies at startup.

McRogueFace already has one, which is where the kit in `deps/winpy` came from:

| piece | source |
|---|---|
| `include/` | `McRogueFace/modules/cpython/Include` + `PC/pyconfig.h` |
| `libpython314.a`, `python314.dll`, `*.pyd`, `*.dll` | `McRogueFace/__lib_windows/` |
| `Lib/` | `McRogueFace/modules/cpython/Lib` |

To build one from scratch instead, take python.org's *embeddable package*
(DLL + stdlib zip, no import library) and generate the import library:

```sh
gendef python314.dll                      # -> python314.def
x86_64-w64-mingw32-dlltool \
    --dllname python314.dll \
    --def python314.def \
    --output-lib libpython314.a
```

Headers come from the matching source tarball's `Include/`, plus
`PC/pyconfig.h`.

## Open: the bundled interpreter does not start on Windows

`gobboclippy.exe --capabilities` works — window, tray, all capabilities
granted, exit 0. Anything that runs a script fails first:

```
Py_InitializeFromConfig: can't initialize sys standard streams
```

### Ruled out

- **wine.** McRogueFace's own `McRogueFace-0.2.8-Windows-full.zip` starts its
  embedded interpreter fine in the *same* wine prefix — `mcrogueface.exe
  --help` prints CPython's help and exits 0.
- **A mismatched runtime.** `python314.dll` and `python314.zip` in the kit are
  byte-identical (md5) to the ones in that working release.
- **Missing stdlib.** The zip holds 562 modules including `encodings/` (123
  entries), `io`, `codecs` and `abc`.
- **GUI vs console subsystem.** Fails identically with
  `-DGC_WIN_CONSOLE=ON`, which builds a console-subsystem binary like
  McRogueFace's.
- **Invalid stdio handles.** `ensureStdioStreams()` in `src/main.cpp`
  attaches the parent console and binds anything left to `NUL`. No change.
- **UTF-8 mode.** `Py_PreInitialize` with `preconfig.utf8_mode = 1`, which
  stops CPython probing the console code page for an encoding. No change.
- **A missing loose `Lib/`.** Shipping `lib/Python/Lib` and pointing `home`
  at `lib/Python`, mirroring McRogueFace's layout. No change.

### Where to look next

The fault is in `startPython()` in `src/main.cpp`, not in the runtime. The
remaining difference from McRogueFace's working `McRFPy_API.cpp:1070-1160` is
the surrounding config, so the next step is to match it exactly rather than
approximately, then re-introduce this project's changes one at a time:

- it calls `PyConfig_InitIsolatedConfig` and sets `config.dev_mode = 0`
- it sets `config.executable` to the exe path — **this build never sets
  `executable` at all**, and CPython uses it when deriving the prefix
- it uses `PyConfig_SetString` with wide literals rather than
  `PyConfig_SetBytesString`
- it sets `stdio_errors` to `surrogateescape`

The `config.executable` gap is the most promising of those.

Also worth checking: `Py_DecodeLocale`'s return value is passed straight to
`PyWideStringList_Append` in `startPython()` without a NULL check, and is
never freed.

## Smoke test under wine

```sh
wine build-windows/gobboclippy.exe --capabilities
```

Expect `video driver : windows` and every capability `yes`. Windows is the one
platform where the whole matrix is supported without caveats.
