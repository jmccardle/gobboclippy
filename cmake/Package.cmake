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
    # Windows CPython wants <home>/Lib as a real directory; the zip alone is
    # not enough to bring the interpreter up. Both are shipped, matching the
    # layout McRogueFace uses with this same runtime.
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${GC_WINPY_ZIP}" "${GC_STDLIB_ZIP}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${GC_WINPY_ROOT}/Lib" "${GC_STAGE}/lib/Python/Lib"
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
    # holds only .py files; these cannot be imported from inside a zip and must
    # sit in <home>/lib/python<ver>/lib-dynload.
    set(GC_DYNLOAD_SRC "${Python3_STDLIB}/lib-dynload")
    if(EXISTS "${GC_DYNLOAD_SRC}")
        add_custom_command(TARGET package-dir POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${GC_DYNLOAD_SRC}"
                    "${GC_STAGE}/lib/python${GC_PY_VERSION_VALUE}/lib-dynload"
            VERBATIM)
    endif()
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
    if(Python3_RUNTIME_LIBRARY_RELEASE)
        add_custom_command(TARGET package-dir POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${Python3_RUNTIME_LIBRARY_RELEASE}" "${GC_STAGE}/"
            VERBATIM)
    else()
        message(WARNING
            "Python3_RUNTIME_LIBRARY_RELEASE is unset: the packaged Windows "
            "build will have no python3XX.dll and will not start.")
    endif()
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
