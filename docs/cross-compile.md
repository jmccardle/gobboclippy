# Linux → Windows cross-compile

**Status: not yet wired up.** CI builds Windows natively
(`.github/workflows/build.yml`), which is the supported path. This document
records what a local cross-build needs, because the pieces are already on this
machine and in McRogueFace.

## Why bother

Native CI is correct for releases. Cross-compiling is for the local loop —
checking a Windows build compiles without pushing a commit and waiting for a
runner.

## What is already available

```
x86_64-w64-mingw32-gcc       present
x86_64-w64-mingw32-g++       present
x86_64-w64-mingw32-dlltool   present
wine                         present   (for smoke-testing the result)
gendef                       MISSING   (mingw-w64-tools)
```

SDL3 cross-compiles cleanly with mingw; it is a normal CMake project with a
toolchain file.

## The hard part: CPython for Windows

You cannot link a Windows binary against the Linux `libpython3.11.so`. You
need a Windows CPython kit:

- `python3XX.dll` — the runtime
- `libpython3XX.a` — the mingw import library
- `python3XX.zip` — the stdlib
- `*.pyd` — stdlib C extension modules
- the `Include/` headers

McRogueFace already assembled exactly this at
`~/Development/McRogueFace/__lib_windows/`:

```
python314.dll          6.8 MB
libpython314.a         1.4 MB     <- the mingw import library
python314.def
python314.zip
python314._pth
_asyncio.pyd  _bz2.pyd  _ctypes.pyd  _decimal.pyd  _hashlib.pyd
_lzma.pyd  _multiprocessing.pyd  _overlapped.pyd  pyexpat.pyd  ...
libcrypto-3.dll  libffi-8.dll  libssl-3.dll  libz.dll
```

That is the reference. It is Python 3.14, so a cross-build here targets 3.14
rather than the 3.11 the Linux build uses — the version only has to be
self-consistent within one package.

## Building the import library yourself

From python.org's *embeddable package* zip, which contains the DLL and stdlib
but no import library:

```sh
sudo apt install mingw-w64-tools          # provides gendef
gendef python314.dll                      # -> python314.def
x86_64-w64-mingw32-dlltool \
    --dllname python314.dll \
    --def python314.def \
    --output-lib libpython314.a
```

Headers come from the matching source tarball's `Include/` directory, plus a
Windows `pyconfig.h`.

## Sketch of the CMake side

A toolchain file, plus pointing `find_package(Python3)` at the kit:

```cmake
# cmake/toolchain-mingw64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

```sh
cmake -B build-windows \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
  -DPython3_INCLUDE_DIR=/path/to/winpython/Include \
  -DPython3_LIBRARY=/path/to/winpython/libpython314.a
```

`Package.cmake` already handles the Windows side of staging: it copies
`Python3_RUNTIME_LIBRARY_RELEASE` (the DLL) to the package root rather than
`lib/`, because Windows has no `$ORIGIN/lib` loader equivalent.

Two things `Package.cmake` does **not** do yet and would need for a real
cross-build: copying the `.pyd` extension modules, and copying the OpenSSL /
libffi DLLs they depend on.

## Smoke test

```sh
wine build-windows/gobboclippy.exe --capabilities
```

Expect `video driver : windows` and every capability `yes` — Windows is the
one platform where the whole matrix is supported without caveats.
