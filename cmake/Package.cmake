# ---------------------------------------------------------------------------
# Packaging: assemble a relocatable, zip-and-ship directory.
#
#   cmake --build build --target package-dir     # staging tree
#   cmake --build build --target package         # tar.gz / zip
#
# Layout produced (identical on every platform, so AppPaths has one case):
#
#   gobboclippy-<version>-<platform>/
#     gobboclippy(.exe)
#     assets/
#     scripts/
#     lib/
#       python<ver>.zip       stdlib, minus test suites
#       libpython<ver>.so     (Linux; .dll beside the exe on Windows)
#     SDL3.so / .dll          unless -DGC_STATIC_SDL=ON
# ---------------------------------------------------------------------------

if(WIN32)
    set(GC_PLATFORM "Windows")
elseif(APPLE)
    set(GC_PLATFORM "macOS")
else()
    set(GC_PLATFORM "Linux")
endif()

set(GC_STAGE "${CMAKE_BINARY_DIR}/stage/gobboclippy-${PROJECT_VERSION}-${GC_PLATFORM}")

# Build the stdlib zip with the host interpreter: zipfile is in every stdlib,
# so this needs no extra tooling on any platform.
# CPython looks for "python<major><minor>.zip" on sys.path -- no dot. Naming
# it anything else means the interpreter silently never finds its stdlib.
set(GC_PY_TAG "${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}")
set(GC_STDLIB_ZIP "${GC_STAGE}/lib/python${GC_PY_TAG}.zip")

# Compiled stdlib extension modules (_socket, zlib, _struct...). The zip holds
# only .py files; these .so files cannot be imported from inside a zip and must
# sit in <home>/lib/python<ver>/lib-dynload.
set(GC_DYNLOAD_SRC "${Python3_STDLIB}/lib-dynload")

add_custom_target(package-dir
    DEPENDS gobboclippy
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${GC_STAGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GC_STAGE}/lib"

    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gobboclippy> "${GC_STAGE}/"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"  "${GC_STAGE}/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/scripts" "${GC_STAGE}/scripts"

    COMMAND ${CMAKE_COMMAND}
            -DSTDLIB_SRC=${Python3_STDLIB}
            -DZIP_OUT=${GC_STDLIB_ZIP}
            -DPY_EXE=${Python3_EXECUTABLE}
            -DWORK_DIR=${CMAKE_BINARY_DIR}
            -P "${CMAKE_SOURCE_DIR}/cmake/ZipStdlib.cmake"

    COMMENT "Staging ${GC_STAGE}"
    VERBATIM
)

if(EXISTS "${GC_DYNLOAD_SRC}")
    add_custom_command(TARGET package-dir POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${GC_DYNLOAD_SRC}"
                "${GC_STAGE}/lib/python${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}/lib-dynload"
        VERBATIM)
endif()

# SDL3 shared library beside the binary (skipped for static builds).
#
# Copy the SONAME file (libSDL3.so.0), not the fully-versioned real file
# (libSDL3.so.0.4.16): the SONAME is the name the dynamic loader actually
# searches for. cmake -E copy dereferences the symlink, so this lands as a
# single real file with the right name and no symlink chain to preserve
# through a zip.
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

# libpython. Static Python builds have no shared object to copy, so this is
# conditional on one actually existing.
#
# Windows differs twice: the thing to ship is the runtime DLL (python3XX.dll)
# rather than the link-time .lib, and it must sit beside the executable, since
# Windows does not have an $ORIGIN/lib equivalent for the loader.
if(WIN32)
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

add_custom_target(package
    DEPENDS package-dir
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_BINARY_DIR}/stage"
            ${CMAKE_COMMAND} -E tar
            $<IF:$<BOOL:${WIN32}>,cf,czf>
            "${CMAKE_BINARY_DIR}/gobboclippy-${PROJECT_VERSION}-${GC_PLATFORM}$<IF:$<BOOL:${WIN32}>,.zip,.tar.gz>"
            $<IF:$<BOOL:${WIN32}>,--format=zip,-->
            "gobboclippy-${PROJECT_VERSION}-${GC_PLATFORM}"
    COMMENT "Writing gobboclippy-${PROJECT_VERSION}-${GC_PLATFORM}"
    VERBATIM
)
