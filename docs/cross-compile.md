# Cross-compiling from Debian

## Summary

| target | buildable on a Debian podman runner? | testable there? |
|---|---|---|
| Linux | yes, native | yes |
| Windows | **yes**, mingw-w64 cross-compile | **yes**, under wine (needs wine 10 / Debian trixie) |
| macOS | **no** | no |

Only macOS needs a hosted runner. That is an Apple licensing constraint, not
a tooling gap: cross-compiling to macOS needs the macOS SDK, and Apple's Xcode
and Apple SDKs Agreement blocks it three separate ways -- the header restricts
execution to Apple-branded hardware, §2.5 separately forbids "separately using
the Apple SDKs ... on non-Apple-branded hardware", and §2.7 forbids enabling
others to do so. osxcross is alive and maintained, but there is no freely
redistributable macOS SDK to feed it. See `docs/macos.md` for the options.

## Windows

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

Three things that bit during setup and are now handled.

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

**Never hand CPython a `FILE*`.** See the next section -- this one is subtle,
and it does not fail, it hangs.

### The C runtime boundary

This is the constraint that shapes the Windows target, and it is worth
understanding before changing anything in `src/main.cpp`.

The binary is built by mingw and links **`msvcrt.dll`**, the legacy C runtime.
The bundled `python314.dll` is python.org's embeddable build, produced by MSVC
and linked against the **Universal CRT** (`api-ms-win-crt-*`, `VCRUNTIME140`).
Two C runtimes are therefore live in one process.

That is supported and routine -- PyO3 ships mingw-built extension modules
against this same MSVC-built runtime -- because the Python C API is pure C with
no ABI surface to disagree about. What is *not* allowed is letting a CRT-owned
object cross the boundary. CPython says so directly:

> the FILE structure for different C libraries can be different and
> incompatible ... care should be taken that FILE\* parameters are only passed
> to these functions if it is certain that they were created by the same
> library that the Python runtime is using.
>
> -- <https://docs.python.org/3/c-api/veryhigh.html>

`runScript()` used to `fopen()` the entry point and pass the `FILE*` to
`PyRun_SimpleFile`. Python started fine and then **hung** -- no error, no
traceback. It now reads the file with `SDL_LoadFile` and runs it through
`Py_CompileString` + `PyEval_EvalCode`, so nothing crosses.

The same rule bans three other things in this codebase, none of which it
currently does. Keep it that way:

- **Do not** set `PYTHONHOME`/`PYTHONPATH` with `putenv`/`_putenv`. Each CRT
  keeps its own environment block, so `python314.dll`'s `getenv` will not see
  them. Set the corresponding `PyConfig` fields instead, which is what
  `startPython()` does.
- **Do not** assign a hand-allocated string to a `PyConfig` field. `PyConfig_Clear`
  calls `PyMem_RawFree` on every one of them, which would free an msvcrt
  pointer on the UCRT heap. Always go through `PyConfig_Set*String`.
- **Do not** pass file descriptors across. Use Win32 `HANDLE`s if you ever must.

Going UCRT on both sides would remove the whole hazard class. Debian 12 has no
UCRT cross-toolchain, but **Debian 13 trixie does**: `g++-mingw-w64-ucrt64`,
`mingw-w64-ucrt64-dev`, target triple `x86_64-w64-mingw32ucrt`. `llvm-mingw` is
the other option and is UCRT by default. Neither is required today, and neither
is a substitute for the rules above.

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
  DLLs/*.pyd        stdlib C extensions (omit if built into the DLL)
  *.dll             their dependencies (libssl, libffi, sqlite3, vcruntime)
```

A loose `Lib/` is **not** needed; the zip is the whole standard library. An
earlier version of this file said otherwise and the package shipped both,
which doubled the archive to 29 MB. See "The wine 8.0 red herring" below.

`cmake/WindowsPython.cmake` validates this and fails loudly on a missing
piece, because a half-assembled kit links fine and then dies at startup.

McRogueFace already has one, which is where the kit in `deps/winpy` came from:

| piece | source |
|---|---|
| `include/` | `McRogueFace/modules/cpython/Include` + `PC/pyconfig.h` |
| `libpython314.a`, `python314.dll`, `*.pyd`, `*.dll` | `McRogueFace/__lib_windows/` |

That directory is python.org's **embeddable package** verbatim -- `python.cat`,
`python314._pth` and all -- which is the easiest way to get one yourself. Take
the embeddable zip (DLL + stdlib zip, no import library) and generate the
import library:

```sh
gendef python314.dll                      # -> python314.def
x86_64-w64-mingw32-dlltool \
    --dllname python314.dll \
    --def python314.def \
    --output-lib libpython314.a
```

Headers come from the matching source tarball's `Include/`, plus
`PC/pyconfig.h`.

## Testing under wine

```sh
cd build-windows/stage/gobboclippy-*-Windows
xvfb-run -a wine ./gobboclippy.exe --capabilities
xvfb-run -a wine ./gobboclippy.exe --script scripts/smoke_test.py
```

Expect `video driver : windows`, every capability `yes`, and
`[clippy] all checks passed`. Windows is the one platform where the whole
matrix is supported without caveats.

**Use wine 10 or newer** — Debian trixie's `wine` package, not bookworm's.
This is why `.forgejo/workflows/build.yml` runs the Windows job in
`debian:trixie` while the Linux job stays on `debian:12`.

### The wine 8.0 red herring

Kept because it cost a lot of time and is very easy to walk back into.

Debian 12 ships wine 8.0, which gives a process **invalid standard handles
when its stdio is a pipe**. CPython probes fds 0/1/2 during startup, and the
result is:

```
Py_InitializeFromConfig: can't initialize sys standard streams
```

That is not a build problem and no `PyConfig` change affects it. The proof
takes one command — the stock python.org `python.exe`, unmodified, in its own
directory, with nothing of this project involved:

```sh
$ wine ./python.exe -c "print('hi')" 2>&1 | cat
Fatal Python error: init_sys_streams: can't initialize sys standard streams
OSError: [WinError 6] Invalid handle

$ script -qec "wine ./python.exe -c \"print('hi')\"" /dev/null
hi
```

Same binary, same directory; the only difference is a terminal instead of a
pipe. Under wine 10 both forms work.

Why the message is so unhelpful: an fd that is *cleanly* invalid is not an
error — CPython sets `sys.stdout = None` and carries on. This fatal error means
an fd passed the cheap `_get_osfhandle`/`GetFileType` probe and *then* failed on
real I/O, which is exactly what wine 8.0 produces. Thirteen distinct conditions
in `create_stdio`/`init_sys_streams` (`Python/pylifecycle.c`) all reach this one
string, so it names a stage, not a cause. When it appears for real, the pending
exception is printed to the preliminary `sys.stderr` by
`_Py_FatalError_PrintExc` — read that, not the message.

Ruled out along the way, all still true and none of them the cause: a
mismatched runtime (the kit's `python314.dll` and `python314.zip` are
md5-identical to a known-working release), missing stdlib, GUI vs console
subsystem, UTF-8 mode, a missing loose `Lib/`, and `config.executable`.
