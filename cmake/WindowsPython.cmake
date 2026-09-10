# Windows CPython for cross-compiled builds.
#
# find_package(Python3) cannot help here: when cross-compiling it would find
# the build machine's Linux interpreter and hand back a libpython3.11.so that
# cannot be linked into a PE binary. So the kit is supplied explicitly.
#
# Expected layout of GC_WINPY_ROOT (see tools/make_windows_kit.py):
#
#   <kit>/
#     include/            Python.h, pyconfig.h and the rest of Include/
#     libpython3XX.a      MinGW import library
#     python3XX.dll       the runtime
#     python3XX.zip       the standard library
#     LICENSE.txt         the embeddable package's own notice, shipped verbatim
#     Lib/                the standard library as loose .py files
#     DLLs/*.pyd          stdlib C extension modules (omit if the runtime has
#                         them built in, as McRogueFace's does)
#     *.dll               their dependencies (libssl, libffi, ...)
#
# Everything below fails loudly on a missing piece. A kit that is quietly
# half-assembled produces a binary that links and then dies at startup with an
# unhelpful error, so it is worth being strict here.

if(NOT GC_WINPY_ROOT)
    message(FATAL_ERROR
        "Cross-compiling to Windows requires a Windows CPython kit.\n"
        "  cmake ... -DGC_WINPY_ROOT=/path/to/kit\n"
        "See docs/cross-compile.md for how to build one.")
endif()

get_filename_component(GC_WINPY_ROOT "${GC_WINPY_ROOT}" ABSOLUTE
                       BASE_DIR "${CMAKE_SOURCE_DIR}")

if(NOT IS_DIRECTORY "${GC_WINPY_ROOT}")
    message(FATAL_ERROR "GC_WINPY_ROOT is not a directory: ${GC_WINPY_ROOT}")
endif()

if(NOT EXISTS "${GC_WINPY_ROOT}/include/Python.h")
    message(FATAL_ERROR
        "No include/Python.h in the kit at ${GC_WINPY_ROOT}")
endif()

# No loose Lib/ is required. An earlier version of this file demanded one,
# believing it was what fixed "can't initialize sys standard streams". It was
# not: that error came from running the exe with piped stdio under wine 8.0,
# and it reproduces with the stock python.org python.exe in its own directory.
# The stdlib zip alone brings the interpreter up. See docs/cross-compile.md.

# Derive the version from the import library rather than asking for it, so the
# kit cannot disagree with the flags the build uses.
file(GLOB GC_WINPY_IMPLIB "${GC_WINPY_ROOT}/libpython3*.a")
list(LENGTH GC_WINPY_IMPLIB _n)
if(NOT _n EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one libpython3XX.a in ${GC_WINPY_ROOT}, found ${_n}. "
        "Two kits mixed together, or none at all.")
endif()

string(REGEX MATCH "libpython3([0-9]+)\\.a$" _m "${GC_WINPY_IMPLIB}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Cannot parse a version from ${GC_WINPY_IMPLIB}")
endif()

set(GC_WINPY_MINOR "${CMAKE_MATCH_1}")
set(GC_PY_TAG_X    "3${GC_WINPY_MINOR}")
set(GC_PY_VERSION_X "3.${GC_WINPY_MINOR}")
set(GC_WINPY_DLL   "${GC_WINPY_ROOT}/python${GC_PY_TAG_X}.dll")
set(GC_WINPY_ZIP   "${GC_WINPY_ROOT}/python${GC_PY_TAG_X}.zip")

# The embeddable package's own LICENSE.txt, which is required rather than
# optional. It is not just the PSF licence: it carries the "Additional
# Conditions for this Windows binary build" clause covering the Microsoft
# Distributable Code linked into python3XX.dll, the .pyd files and the
# vcruntime DLLs this kit ships. Redistributing those without it is the one
# licence problem in this project that is actually binding.
set(GC_WINPY_LICENSE "${GC_WINPY_ROOT}/LICENSE.txt")

foreach(f "${GC_WINPY_DLL}" "${GC_WINPY_ZIP}" "${GC_WINPY_LICENSE}")
    if(NOT EXISTS "${f}")
        message(FATAL_ERROR "Kit is missing ${f}")
    endif()
endforeach()

# The pip wheel the package ships for its interpreter mode. python.org's
# layout keeps it under Lib/ensurepip/_bundled, and the kit carries that
# directory even though the loose Lib/ is otherwise not shipped.
file(GLOB GC_PIP_WHEEL_PATH "${GC_WINPY_ROOT}/Lib/ensurepip/_bundled/pip-*.whl")
list(LENGTH GC_PIP_WHEEL_PATH _n)
if(NOT _n EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one pip-*.whl in ${GC_WINPY_ROOT}/Lib/ensurepip/_bundled, "
        "found ${_n}. The package ships pip so its interpreter mode can "
        "install packages.")
endif()

message(STATUS "Windows CPython kit: ${GC_WINPY_ROOT} (Python ${GC_PY_VERSION_X})")

# Stand in for the Python3::Python target the native path provides, so
# CMakeLists.txt links the same way either way.
add_library(gc_winpython INTERFACE)
target_include_directories(gc_winpython INTERFACE "${GC_WINPY_ROOT}/include")
target_link_libraries(gc_winpython INTERFACE "${GC_WINPY_IMPLIB}")

# Consumed by Package.cmake.
set(GC_WINPY_DLLDIR "${GC_WINPY_ROOT}/DLLs")
file(GLOB GC_WINPY_DEPDLLS "${GC_WINPY_ROOT}/*.dll")
