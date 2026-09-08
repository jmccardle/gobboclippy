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
#   libSDL3.so.0                      SDL3.dll
#   assets/  scripts/                 assets/  scripts/
#   lib/python3XX.zip                 python3XX.zip      <- prefix root
#   lib/python3.XX/lib-dynload/       DLLs/*.pyd
#   lib/libpython3.XX.so              python3XX.dll      <- beside the exe
#                                     libssl-3.dll, ...
# ---------------------------------------------------------------------------

if(WIN32)
    set(GC_PLATFORM "Windows")
elseif(APPLE)
    set(GC_PLATFORM "macOS")
else()
    set(GC_PLATFORM "Linux")
endif()

set(GC_STAGE "${CMAKE_BINARY_DIR}/stage/gobboclippy-${PROJECT_VERSION}-${GC_PLATFORM}")

# CPython looks for "python<major><minor>.zip" on sys.path -- no dot. Naming it
# anything else means the interpreter silently never finds its stdlib.
if(WIN32)
    set(GC_STDLIB_ZIP "${GC_STAGE}/python${GC_PY_TAG_VALUE}.zip")
else()
    set(GC_STDLIB_ZIP "${GC_STAGE}/lib/python${GC_PY_TAG_VALUE}.zip")
endif()

add_custom_target(package-dir
    DEPENDS gobboclippy
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${GC_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GC_STAGE}/lib"

    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gobboclippy> "${GC_STAGE}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"  "${GC_STAGE}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/scripts" "${GC_STAGE}/scripts"

    COMMENT "Staging ${GC_STAGE}"
    VERBATIM
)

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
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${Python3_LIBRARY_RELEASE}" "${GC_STAGE}/lib/"
        VERBATIM)
endif()

# --- third-party notices ---------------------------------------------------
#
# A packaged build redistributes SDL3, stb and CPython as binaries. All three
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

set(GC_NOTICES
    "${CMAKE_SOURCE_DIR}/LICENSE"    "gobboclippy-MIT.txt"
    "${SDL3_SOURCE_DIR}/LICENSE.txt" "SDL3-zlib.txt"
    "${stb_SOURCE_DIR}/LICENSE"      "stb-MIT-or-public-domain.txt"
    "${GC_PY_LICENSE}"               "CPython-PSF.txt"
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

This package is MIT licensed and redistributes three other projects in binary
form. Their notices are here in full.

  gobboclippy-MIT.txt             gobboclippy itself. MIT.
  SDL3-zlib.txt                   SDL3, the window/tray/event layer. zlib.
  stb-MIT-or-public-domain.txt    stb_image, the PNG decoder. MIT or public
                                  domain, at your choice.
  CPython-PSF.txt                 the bundled interpreter and standard
                                  library, Python ${GC_PY_VERSION_VALUE}. PSF-2.0, plus the
                                  notices for the software CPython itself
                                  incorporates.
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
    # Relocate, strip and sign are one script on macOS because their order is
    # forced: install_name_tool and strip each invalidate the signature, so
    # signing has to come last and has to come after both.
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DEXE=${GC_STAGE}/gobboclippy
                -DSTAGE=${GC_STAGE}
                -P "${CMAKE_SOURCE_DIR}/cmake/MacFinalize.cmake"
        VERBATIM)
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

# --- archive ---------------------------------------------------------------
# zip for Windows (what people expect to double-click), tar.gz elsewhere.
set(GC_ARCHIVE_DIR "gobboclippy-${PROJECT_VERSION}-${GC_PLATFORM}")
if(WIN32)
    set(GC_ARCHIVE "${CMAKE_BINARY_DIR}/${GC_ARCHIVE_DIR}.zip")
    set(GC_TAR_ARGS cf "${GC_ARCHIVE}" --format=zip)
else()
    set(GC_ARCHIVE "${CMAKE_BINARY_DIR}/${GC_ARCHIVE_DIR}.tar.gz")
    set(GC_TAR_ARGS czf "${GC_ARCHIVE}")
endif()

add_custom_target(package
    DEPENDS package-dir
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/stage"
            ${CMAKE_COMMAND} -E tar ${GC_TAR_ARGS} "${GC_ARCHIVE_DIR}"
    COMMENT "Writing ${GC_ARCHIVE}"
    VERBATIM
)
