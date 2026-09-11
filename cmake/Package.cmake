# ---------------------------------------------------------------------------
# Packaging: assemble a relocatable, zip-and-ship directory.
#
#   cmake --build build --target package-dir     # staging tree
#   cmake --build build --target package         # tar.gz / zip
#
# The layout differs between Windows and POSIX because CPython's own default
# sys.path does. Setting PYTHONHOME to the package root satisfies both; only
# where the files sit changes.
#
#   POSIX                             Windows
#   ------------------------------    ------------------------------
#   gobboclippy                       gobboclippy.exe
#   python3 -> gobboclippy            python.exe         <- a copy of the exe
#   libSDL3.so.0                      SDL3.dll
#   assets/  scripts/                 assets/  scripts/
#   lib/python3XX.zip                 python3XX.zip      <- prefix root
#   lib/python3.XX/lib-dynload/       DLLs/*.pyd
#   lib/libpython3.XX.so.1.0          python3XX.dll      <- beside the exe
#   lib/pip-*.whl                     lib/pip-*.whl
#                                     libssl-3.dll, ...
#   site/                             site/              <- sys.prefix; empty
#                                                           until pip fills it
#
# macOS is the POSIX layout, moved wholesale into an .app bundle:
#
#   gobboclippy-<version>-macOS/          <- becomes the DMG's volume
#     Applications -> /Applications       <- the drag target
#     gobboclippy.app/Contents/
#       Info.plist                        <- identity, and the microphone string
#       MacOS/gobboclippy                 <- the binary, alone
#       Resources/                        <- everything above, verbatim
#         python3 -> ../MacOS/gobboclippy
#
# A bundle rather than a directory because macOS grants the microphone per
# bundle: the prompt string comes from NSMicrophoneUsageDescription in the
# Info.plist and the grant is recorded against CFBundleIdentifier. A bare
# executable has neither, and inherits whatever launched it -- which works from
# a terminal the user has already allowed, and cannot ask for itself in Finder.
#
# Resources/ rather than MacOS/ because SDL_GetBasePath answers with the
# resource directory for a bundled app, and AppPaths.cpp resolves everything
# this program opens from that one call. Nothing in the C++ changes; the
# executable gains two rpath entries (CMakeLists.txt) so it can still find
# libraries that are now a directory away.
# ---------------------------------------------------------------------------

if(WIN32)
    set(GC_PLATFORM "Windows")
elseif(APPLE)
    set(GC_PLATFORM "macOS")
else()
    set(GC_PLATFORM "Linux")
endif()

# The name of the thing a user ends up with: the directory inside the archive
# everywhere else, the DMG's volume on macOS.
set(GC_ARCHIVE_DIR "gobboclippy-${PROJECT_VERSION}-${GC_PLATFORM}")
set(GC_VOLUME "${CMAKE_BINARY_DIR}/stage/${GC_ARCHIVE_DIR}")

# GC_STAGE is the payload root -- the directory the running binary sees as its
# base path, and the one every rule below stages into. GC_EXEDIR is where the
# binary itself goes. They are the same directory everywhere except in an .app.
if(APPLE)
    set(GC_APP    "${GC_VOLUME}/gobboclippy.app")
    set(GC_STAGE  "${GC_APP}/Contents/Resources")
    set(GC_EXEDIR "${GC_APP}/Contents/MacOS")
else()
    set(GC_STAGE  "${GC_VOLUME}")
    set(GC_EXEDIR "${GC_VOLUME}")
endif()

# CPython looks for "python<major><minor>.zip" on sys.path -- no dot. Naming it
# anything else means the interpreter silently never finds its stdlib.
if(WIN32)
    set(GC_STDLIB_ZIP "${GC_STAGE}/python${GC_PY_TAG_VALUE}.zip")
else()
    set(GC_STDLIB_ZIP "${GC_STAGE}/lib/python${GC_PY_TAG_VALUE}.zip")
endif()

add_custom_target(package-dir
    DEPENDS gobboclippy
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${GC_VOLUME}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GC_STAGE}/lib" "${GC_EXEDIR}"

    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gobboclippy> "${GC_EXEDIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"  "${GC_STAGE}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/scripts" "${GC_STAGE}/scripts"

    COMMENT "Staging ${GC_STAGE}"
    VERBATIM
)

# --- the bundle's own files ------------------------------------------------
if(APPLE)
    # Configured at configure time; the version and the identifier are the only
    # substitutions, and both are known then.
    set(GC_PLIST "${CMAKE_BINARY_DIR}/Info.plist")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/Info.plist.in" "${GC_PLIST}" @ONLY)

    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GC_PLIST}" "${GC_APP}/Contents/Info.plist"
        COMMAND ${CMAKE_COMMAND}
                -DPNG=${CMAKE_SOURCE_DIR}/assets/clippy.png
                -DOUT=${GC_STAGE}/gobboclippy.icns
                -DWORK=${CMAKE_BINARY_DIR}
                -P "${CMAKE_SOURCE_DIR}/cmake/MacIcon.cmake"
        # The drag target. A DMG whose window holds only an app is one the user
        # is expected to run from the mounted image, and an app run from a
        # read-only mount is an app whose site/ cannot be installed into.
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                /Applications "${GC_VOLUME}/Applications"
        VERBATIM)
endif()

# --- the standard library --------------------------------------------------
if(CMAKE_CROSSCOMPILING)
    # The Windows kit already carries a built stdlib zip. There is no Windows
    # interpreter here to build one with, and the Linux stdlib is the wrong
    # content, so this is a copy rather than a rebuild.
    #
    # The zip is the whole standard library; the kit's loose Lib/ is deliberately
    # not shipped. It was, briefly, on the theory that Windows CPython needed it
    # to start -- it does not, and shipping both doubled the archive to 29 MB.
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${GC_WINPY_ZIP}" "${GC_STDLIB_ZIP}"
        VERBATIM)
    if(EXISTS "${GC_WINPY_DLLDIR}")
        add_custom_command(TARGET package-dir POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${GC_WINPY_DLLDIR}" "${GC_STAGE}/DLLs"
            VERBATIM)
    endif()
else()
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DSTDLIB_SRC=${Python3_STDLIB}
                -DZIP_OUT=${GC_STDLIB_ZIP}
                -DPY_EXE=${Python3_EXECUTABLE}
                -DWORK_DIR=${CMAKE_BINARY_DIR}
                -P "${CMAKE_SOURCE_DIR}/cmake/ZipStdlib.cmake"
        VERBATIM)

    # Compiled stdlib extension modules (_socket, zlib, _struct...). The zip
    # holds only .py files; these cannot be imported from inside a zip, so they
    # ship loose. Where they go is the one place the two layouts really differ:
    # POSIX wants <home>/lib/python<ver>/lib-dynload, Windows wants <home>/DLLs.
    if(WIN32)
        # Python3_STDLIB is <prefix>/Lib on Windows, so the .pyd directory is
        # its sibling. Fatal if absent: without it the package has no zlib and
        # no _socket, and every failure surfaces later as a puzzling ImportError.
        get_filename_component(GC_PY_PREFIX "${Python3_STDLIB}" DIRECTORY)
        set(GC_DYNLOAD_SRC "${GC_PY_PREFIX}/DLLs")
        set(GC_DYNLOAD_DST "${GC_STAGE}/DLLs")
        if(NOT IS_DIRECTORY "${GC_DYNLOAD_SRC}")
            message(FATAL_ERROR
                "No DLLs/ directory beside the Python stdlib at ${GC_PY_PREFIX}. "
                "The packaged build would ship no extension modules.")
        endif()

        # vcruntime140*.dll sit in the prefix root next to python.exe. The
        # runner has the MSVC redistributable installed; a user's machine may
        # not, and that failure is a dialog at startup, not an error we can
        # report.
        file(GLOB GC_WIN_VCRUNTIME "${GC_PY_PREFIX}/vcruntime140*.dll")
        foreach(dll ${GC_WIN_VCRUNTIME})
            add_custom_command(TARGET package-dir POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${dll}" "${GC_STAGE}/"
                VERBATIM)
        endforeach()
    else()
        set(GC_DYNLOAD_SRC "${Python3_STDLIB}/lib-dynload")
        set(GC_DYNLOAD_DST "${GC_STAGE}/lib/python${GC_PY_VERSION_VALUE}/lib-dynload")
        if(NOT IS_DIRECTORY "${GC_DYNLOAD_SRC}")
            message(FATAL_ERROR
                "No lib-dynload beside the Python stdlib at ${Python3_STDLIB}. "
                "The packaged build would ship no extension modules.")
        endif()
    endif()

    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${GC_DYNLOAD_SRC}" "${GC_DYNLOAD_DST}"
        VERBATIM)
endif()

# --- SDL3 ------------------------------------------------------------------
#
# Ship the SONAME file (libSDL3.so.0), not the fully-versioned real file
# (libSDL3.so.0.4.16): the SONAME is the name the dynamic loader searches for.
# cmake -E copy dereferences the symlink, so this lands as a single real file
# with the right name and no symlink chain to survive an archive round-trip.
if(NOT GC_STATIC_SDL)
    if(WIN32)
        set(GC_SDL_SHIP $<TARGET_FILE:SDL3::SDL3>)
    else()
        set(GC_SDL_SHIP $<TARGET_SONAME_FILE:SDL3::SDL3>)
    endif()
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GC_SDL_SHIP}" "${GC_STAGE}/"
        VERBATIM)
endif()

# --- libpython and its dependencies ----------------------------------------
if(CMAKE_CROSSCOMPILING)
    # python3XX.dll plus whatever the .pyd modules link against (libssl,
    # libffi, sqlite3, the MSVC runtime). All beside the exe: Windows has no
    # $ORIGIN/lib equivalent for the loader.
    foreach(dll ${GC_WINPY_DEPDLLS})
        add_custom_command(TARGET package-dir POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${dll}" "${GC_STAGE}/"
            VERBATIM)
    endforeach()
elseif(WIN32)
    # python3XX.dll lives in the prefix root beside python.exe -- the same
    # place DLLs/ and vcruntime140.dll came from. Taken from there rather than
    # from Python3_RUNTIME_LIBRARY_RELEASE, which FindPython does not promise
    # to set and which used to leave this a warning and the package unbootable.
    set(GC_PY_DLL "${GC_PY_PREFIX}/python${GC_PY_TAG_VALUE}.dll")
    if(NOT EXISTS "${GC_PY_DLL}")
        message(FATAL_ERROR
            "No python${GC_PY_TAG_VALUE}.dll at ${GC_PY_PREFIX}. The packaged "
            "build would have no interpreter and would not start.")
    endif()
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GC_PY_DLL}" "${GC_STAGE}/"
        VERBATIM)
elseif(APPLE)
    # Deliberately nothing here. On macOS the shipped dylib cannot be a plain
    # copy -- it has to be renamed to @rpath, with the executable rewritten to
    # match -- and that has to happen after everything else is staged. See the
    # finalisation step at the bottom of this file.
elseif(Python3_LIBRARY_RELEASE)
    # Ship libpython under its SONAME, for the same reason SDL is shipped under
    # its own a few lines up: the SONAME is the name the loader searches for.
    #
    # Python3_LIBRARY_RELEASE is the *linker* name -- libpython3.11.so, the
    # symlink the -dev package installs -- and `cmake -E copy` dereferences it,
    # so staging it directly wrote lib/libpython3.11.so while the executable's
    # DT_NEEDED said libpython3.11.so.1.0. Nothing in $ORIGIN/lib matched that,
    # the loader fell through to the default search path, and the package ran
    # against the host's interpreter: fine on any machine with python3.11
    # installed, and unable to start on exactly the machines a bundled runtime
    # exists for. It was 7.7 MB of file that nothing ever opened.
    #
    # INSTSONAME is the interpreter's own record of the name it was built with,
    # so it cannot disagree with what the linker wrote into our executable.
    # Read from the interpreter rather than from the file, because objdump is
    # not guaranteed to be present and this is not a fact worth deriving twice.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -c
                "import sysconfig as s; print(s.get_config_var('Py_ENABLE_SHARED') or 0); print(s.get_config_var('INSTSONAME') or '')"
        OUTPUT_VARIABLE  GC_PY_SONAME_QUERY
        ERROR_VARIABLE   GC_PY_SONAME_ERR
        RESULT_VARIABLE  GC_PY_SONAME_RC
        OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(NOT GC_PY_SONAME_RC EQUAL 0)
        message(FATAL_ERROR
            "Could not ask ${Python3_EXECUTABLE} for its INSTSONAME: "
            "${GC_PY_SONAME_ERR}")
    endif()

    string(REPLACE "\n" ";" GC_PY_SONAME_QUERY "${GC_PY_SONAME_QUERY}")
    list(GET GC_PY_SONAME_QUERY 0 GC_PY_SHARED)
    list(GET GC_PY_SONAME_QUERY 1 GC_PY_INSTSONAME)
    string(STRIP "${GC_PY_SHARED}"     GC_PY_SHARED)
    string(STRIP "${GC_PY_INSTSONAME}" GC_PY_INSTSONAME)

    if(NOT GC_PY_SHARED OR GC_PY_SHARED STREQUAL "0")
        # A statically linked libpython is a legitimate build: the interpreter
        # is inside our executable and there is nothing to ship beside it. Said
        # out loud, because the alternative reading of an empty lib/ is that
        # this step broke.
        message(STATUS
            "python: statically linked (Py_ENABLE_SHARED=0); no libpython to stage")
    else()
        # The SONAME file is a sibling of the linker name by construction, so
        # take the directory from what CMake resolved and the name from the
        # interpreter. Nothing here guesses at a library directory.
        get_filename_component(GC_PY_LIBDIR "${Python3_LIBRARY_RELEASE}" DIRECTORY)
        set(GC_PY_SONAME_FILE "${GC_PY_LIBDIR}/${GC_PY_INSTSONAME}")

        if(NOT EXISTS "${GC_PY_SONAME_FILE}")
            message(FATAL_ERROR
                "No ${GC_PY_INSTSONAME} beside ${Python3_LIBRARY_RELEASE}. That "
                "is the name this build's executable asks the loader for, so "
                "the package would silently run against the host's libpython "
                "instead of the one it ships.")
        endif()

        add_custom_command(TARGET package-dir POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${GC_PY_SONAME_FILE}" "${GC_STAGE}/lib/${GC_PY_INSTSONAME}"
            VERBATIM)
    endif()
endif()

# --- pip -------------------------------------------------------------------
#
# The wheel goes on sys.path as-is; pip is importable from its own wheel, which
# is how ensurepip bootstraps it. GC_PIP_WHEEL_PATH was resolved at configure
# time from the interpreter being bundled (CMakeLists.txt) or from the Windows
# kit (WindowsPython.cmake), so it is always the pip that interpreter would
# have put in a venv. The binary knows the file name as GC_PIP_WHEEL.
add_custom_command(TARGET package-dir POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GC_PIP_WHEEL_PATH}" "${GC_STAGE}/lib/"
    VERBATIM)

# pip's own licence, for the notices directory below. Where it sits inside the
# wheel moved between pip versions (dist-info/LICENSE.txt, then
# dist-info/licenses/LICENSE.txt), so the dist-info is unpacked and searched
# rather than a path assumed. The vendored libraries' notices are excluded
# from the search: they stay inside the wheel, where pip keeps them.
set(GC_PIP_DISTINFO_DIR "${CMAKE_BINARY_DIR}/pip-dist-info")
file(REMOVE_RECURSE "${GC_PIP_DISTINFO_DIR}")
file(ARCHIVE_EXTRACT INPUT "${GC_PIP_WHEEL_PATH}"
     DESTINATION "${GC_PIP_DISTINFO_DIR}"
     PATTERNS "pip-*.dist-info/*")
file(GLOB_RECURSE GC_PIP_LICENSE_CANDIDATES "${GC_PIP_DISTINFO_DIR}/pip-*.dist-info/LICENSE.txt")
list(FILTER GC_PIP_LICENSE_CANDIDATES EXCLUDE REGEX "_vendor")
list(LENGTH GC_PIP_LICENSE_CANDIDATES _n)
if(NOT _n EQUAL 1)
    message(FATAL_ERROR
        "Expected exactly one LICENSE.txt in the dist-info of ${GC_PIP_WHEEL_PATH}, "
        "found ${_n}: ${GC_PIP_LICENSE_CANDIDATES}. The package redistributes "
        "pip, so its notice has to ship with it.")
endif()
list(GET GC_PIP_LICENSE_CANDIDATES 0 GC_PIP_LICENSE)

# --- third-party notices ---------------------------------------------------
#
# A packaged build redistributes SDL3, stb, CPython and pip. All of their
# licences require their notice to travel with them, and CPython's Windows
# build additionally carries Microsoft's terms for the runtime DLLs it links.
#
# Every notice is copied from the source tree it belongs to -- the pinned SDL
# checkout, the pinned stb checkout, the interpreter being bundled -- so it can
# never describe a different version than the one shipped. Nothing here is
# vendored into this repository, and nothing is optional: a missing notice is a
# configure error, because the alternative is discovering it after a release.
set(GC_LICENSE_DIR "${GC_STAGE}/licenses")

if(CMAKE_CROSSCOMPILING)
    set(GC_PY_LICENSE "${GC_WINPY_LICENSE}")
elseif(WIN32)
    # python.org's Windows layout puts it in the prefix root.
    set(GC_PY_LICENSE "${GC_PY_PREFIX}/LICENSE.txt")
else()
    # CPython's `make install` puts it in LIBDEST, beside the stdlib.
    set(GC_PY_LICENSE "${Python3_STDLIB}/LICENSE.txt")
endif()

# The font is the one redistributed component that is vendored here rather than
# fetched, because a .ttf has no source tree to take a notice from. Its terms
# travel with it in assets/, and are copied into licences/ as well so the index
# below can point at all four in one place.
set(GC_NOTICES
    "${CMAKE_SOURCE_DIR}/LICENSE"    "gobboclippy-MIT.txt"
    "${SDL3_SOURCE_DIR}/LICENSE.txt" "SDL3-zlib.txt"
    "${stb_SOURCE_DIR}/LICENSE"      "stb-MIT-or-public-domain.txt"
    "${GC_PY_LICENSE}"               "CPython-PSF.txt"
    "${GC_PIP_LICENSE}"              "pip-MIT.txt"
    "${CMAKE_SOURCE_DIR}/assets/JetBrainsMono-LICENSE.txt"
                                     "JetBrainsMono-Apache-2.0.txt"
    # Two files, one work. libwebp's BSD grant is accompanied by a separate
    # patent grant, and shipping the licence without it would be shipping half
    # of the terms.
    "${libwebp_SOURCE_DIR}/COPYING"  "libwebp-BSD-3-Clause.txt"
    "${libwebp_SOURCE_DIR}/PATENTS"  "libwebp-PATENTS.txt"
)

list(LENGTH GC_NOTICES _n)
math(EXPR _last "${_n} / 2 - 1")
foreach(i RANGE ${_last})
    math(EXPR _src "${i} * 2")
    math(EXPR _dst "${i} * 2 + 1")
    list(GET GC_NOTICES ${_src} _from)
    list(GET GC_NOTICES ${_dst} _name)
    if(NOT EXISTS "${_from}")
        message(FATAL_ERROR
            "Missing licence notice for ${_name}: no file at ${_from}. "
            "The package redistributes this component, so its notice has to "
            "ship with it.")
    endif()
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_from}" "${GC_LICENSE_DIR}/${_name}"
        VERBATIM)
endforeach()

# The index. Written at configure time from what was actually resolved above,
# and deliberately free of build-machine paths -- the package is meant to be
# relocatable, and that includes not naming the directory it was built in.
set(GC_LICENSE_INDEX "${CMAKE_BINARY_DIR}/licenses-README.txt")
file(WRITE "${GC_LICENSE_INDEX}"
"gobboclippy ${PROJECT_VERSION} -- ${GC_PLATFORM}

This package is MIT licensed and redistributes six other works. Their
notices are here in full.

  gobboclippy-MIT.txt             gobboclippy itself. MIT.
  SDL3-zlib.txt                   SDL3, the window/tray/event layer. zlib.
  stb-MIT-or-public-domain.txt    stb_image and stb_truetype, the PNG decoder
                                  and the text rasteriser. MIT or public
                                  domain, at your choice.
  CPython-PSF.txt                 the bundled interpreter and standard
                                  library, Python ${GC_PY_VERSION_VALUE}. PSF-2.0, plus the
                                  notices for the software CPython itself
                                  incorporates.
  pip-MIT.txt                     pip, shipped unmodified as lib/${GC_PIP_WHEEL_NAME}
                                  for the interpreter mode. MIT. The libraries
                                  pip vendors carry their own notices inside
                                  that wheel, under its dist-info.
  JetBrainsMono-Apache-2.0.txt    JetBrains Mono 1.0.3, the shipped typeface,
                                  at assets/JetBrainsMono.ttf. Apache-2.0.
  libwebp-BSD-3-Clause.txt        libwebp, the WebP decoder linked into the
                                  binary. BSD-3-Clause, and its separate
                                  patent grant is libwebp-PATENTS.txt.
")
if(WIN32)
    file(APPEND "${GC_LICENSE_INDEX}"
"
CPython-PSF.txt also carries \"Additional Conditions for this Windows binary
build\", covering the Microsoft Distributable Code linked into python*.dll,
the .pyd extension modules and the vcruntime DLLs shipped beside them. That
clause applies to this package and to anything you redistribute it inside.
")
endif()

add_custom_command(TARGET package-dir POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GC_LICENSE_INDEX}" "${GC_LICENSE_DIR}/README.txt"
    VERBATIM)

# --- finalise the binaries -------------------------------------------------
#
# Last, because it rewrites files the steps above put there.
#
# The interpreter we bundle is somebody else's build. Debian ships one that is
# stripped; the one actions/setup-python installs is not, and that difference
# alone is 12 MB compressed -- the first Linux package CI produced was 20 MB
# against 8.0 MB for the identical commit built locally. Stripping in the
# staging tree, rather than asking the build machine to provide a small Python,
# is what makes the two agree.
if(APPLE)
    # Deliberately nothing here. Relocating, stripping and signing are one
    # script on macOS because their order is forced -- install_name_tool and
    # strip each invalidate a signature -- and the bundle's own signature seals
    # every file in it, so it cannot be made until the last file is in place.
    # That is after the interpreter alias below, not here.
elseif(MSVC)
    # No strip step, and no strip tool either -- MSVC does not ship one because
    # it does not need one. Its symbols go to a .pdb rather than into the image,
    # and every binary in a native Windows package is MSVC output: our exe,
    # SDL3.dll, and python.org's python3XX.dll and .pyd modules. Demanding a
    # strip here would be inventing a failure, not surfacing one.
    message(STATUS "MSVC: no strip step; symbols live in .pdb, which is not shipped")
else()
    if(NOT CMAKE_STRIP)
        message(FATAL_ERROR
            "No strip tool found. The bundled interpreter may be unstripped, "
            "which silently doubles the package.")
    endif()
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DSTAGE=${GC_STAGE}
                -DSTRIP=${CMAKE_STRIP}
                -DPLATFORM=${GC_PLATFORM}
                -P "${CMAKE_SOURCE_DIR}/cmake/StripTree.cmake"
        VERBATIM)
endif()

# --- the interpreter alias -------------------------------------------------
#
# The same executable under the name python expects to be called by. main.cpp
# dispatches on argv[0], so `python3 -m pip install x` and
# `gobboclippy --python -m pip install x` are one code path; the alias exists
# so that sys.executable names something a subprocess can run as python.
#
# A symlink where the archive format can carry one. Windows gets a copy, and
# gets it here, after the strip step, so the copy is of the stripped binary.
#
# site/ is sys.prefix, and its site-packages is where pip installs. It ships
# empty, so the layout is visible before anything is installed into it, and
# so it is on sys.path from the first run: site.py only adds a site-packages
# directory that exists.
if(WIN32)
    set(GC_SITE_PACKAGES "${GC_STAGE}/site/Lib/site-packages")
else()
    set(GC_SITE_PACKAGES "${GC_STAGE}/site/lib/python${GC_PY_VERSION_VALUE}/site-packages")
endif()
add_custom_command(TARGET package-dir POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GC_SITE_PACKAGES}"
    VERBATIM)
if(WIN32)
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                "${GC_STAGE}/gobboclippy.exe" "${GC_STAGE}/python.exe"
        VERBATIM)
elseif(APPLE)
    # The alias stays in Resources, because sys.executable has to name a path
    # under the same root as sys.base_prefix -- but the binary it names is a
    # directory away now, so the link is relative to itself rather than a bare
    # name. Archive formats and codesign both record a symlink by its target
    # string, so this survives both.
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                ../MacOS/gobboclippy "${GC_STAGE}/python3"
        VERBATIM)
else()
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                gobboclippy "${GC_STAGE}/python3"
        VERBATIM)
endif()

# --- finalise the bundle ---------------------------------------------------
#
# Genuinely last: relocate, strip, sign every Mach-O inside-out, then sign the
# bundle itself. A bundle signature seals every file under Contents/, so it has
# to be made after the icon, the plist, site/ and the alias are all in place.
#
# Staging one of them afterwards would ship a bundle whose seal does not match
# its own contents, and that failure is quiet in the worst way: the kernel
# checks the executable, not the seal, so it launches here and is refused on
# the first machine that downloads it. `codesign --verify` is what notices, and
# CI runs it. See "Repacking a release" in docs/macos.md, which is the same
# hazard arriving from the other direction.
if(APPLE)
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DEXE=${GC_EXEDIR}/gobboclippy
                -DSTAGE=${GC_STAGE}
                -DSCAN=${GC_APP}
                -DBUNDLE=${GC_APP}
                -P "${CMAKE_SOURCE_DIR}/cmake/MacFinalize.cmake"
        VERBATIM)
endif()

# --- archive ---------------------------------------------------------------
# zip for Windows (what people expect to double-click), a disk image on macOS,
# tar.gz elsewhere.
#
# The DMG is not decoration either. Archive Utility propagates the quarantine
# attribute onto every file it extracts from a zip, which puts the app into App
# Translocation -- it runs from a randomised read-only path, and every path this
# program resolves is relative to where it thinks it is. Dragging out of a
# mounted image is a move the system recognises, and the translocation does not
# happen. See docs/macos.md.
if(WIN32)
    set(GC_ARCHIVE "${CMAKE_BINARY_DIR}/${GC_ARCHIVE_DIR}.zip")
    set(GC_TAR_ARGS cf "${GC_ARCHIVE}" --format=zip)
elseif(APPLE)
    set(GC_ARCHIVE "${CMAKE_BINARY_DIR}/${GC_ARCHIVE_DIR}.dmg")
    find_program(GC_HDIUTIL hdiutil REQUIRED)
else()
    set(GC_ARCHIVE "${CMAKE_BINARY_DIR}/${GC_ARCHIVE_DIR}.tar.gz")
    set(GC_TAR_ARGS czf "${GC_ARCHIVE}")
endif()

if(APPLE)
    # UDZO over HFS+: the compressed, read-only, universally-mountable form.
    # -ov because hdiutil refuses to overwrite otherwise, and a stale image
    # from the previous build is the worst possible thing to ship.
    add_custom_target(package
        DEPENDS package-dir
        COMMAND ${GC_HDIUTIL} create
                -volname "gobboclippy ${PROJECT_VERSION}"
                -srcfolder "${GC_VOLUME}"
                -fs HFS+ -format UDZO -ov
                "${GC_ARCHIVE}"
        COMMENT "Writing ${GC_ARCHIVE}"
        VERBATIM
    )
else()
    add_custom_target(package
        DEPENDS package-dir
        COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/stage"
                ${CMAKE_COMMAND} -E tar ${GC_TAR_ARGS} "${GC_ARCHIVE_DIR}"
        COMMENT "Writing ${GC_ARCHIVE}"
        VERBATIM
    )
endif()
