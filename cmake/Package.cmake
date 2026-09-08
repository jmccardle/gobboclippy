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
    # Not a plain copy: on macOS the shipped dylib has to be renamed to
    # @rpath and the executable rewritten to match, or the package only runs
    # on the machine that built it. See cmake/MacRelocate.cmake.
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DEXE=${GC_STAGE}/gobboclippy
                -DLIBDIR=${GC_STAGE}/lib
                -P "${CMAKE_SOURCE_DIR}/cmake/MacRelocate.cmake"
        VERBATIM)
elseif(Python3_LIBRARY_RELEASE)
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${Python3_LIBRARY_RELEASE}" "${GC_STAGE}/lib/"
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
