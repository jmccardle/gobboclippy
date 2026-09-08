# Cross-compile to Windows x86_64 with MinGW-w64.
#
#   cmake -B build-windows \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
#     -DGC_WINPY_ROOT=deps/winpy
#
# Adapted from McRogueFace's cmake/toolchains/mingw-w64-x86_64.cmake, which
# has the mileage. Debian: apt install mingw-w64

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# The -posix variants, not the -win32 ones: the win32 threading model has no
# std::mutex / std::thread, and the failure is a wall of template errors deep
# inside <mutex> rather than anything that names the cause.
set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Plain -static, not just -static-libgcc/-static-libstdc++.
#
# The -posix threading model links libwinpthread-1.dll dynamically, and the
# two -static-lib* flags do not cover it. The result loads fine on the build
# machine's wine prefix if that DLL happens to be around and dies with
# STATUS_DLL_NOT_FOUND (0xc0000135) on a clean Windows box -- a failure that
# only shows up after shipping. -static pulls the whole GCC runtime in.
#
# It does not affect SDL3 or CPython: those are linked through import
# libraries and still load as DLLs at runtime.
#
# --enable-auto-import is required for CPython's data symbol exports.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-static -Wl,--enable-auto-import")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "-static-libgcc -static-libstdc++ -Wl,--enable-auto-import")
