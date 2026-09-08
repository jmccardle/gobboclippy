# Finish the staged macOS tree: relocate, strip, sign. In that order, because
# each step invalidates the one before it.
#
# Run in script mode as a POST_BUILD step of package-dir:
#   cmake -DEXE=... -DSTAGE=... -P MacFinalize.cmake
#
# --- relocate ---------------------------------------------------------------
#
# The Mach-O loader has no $ORIGIN. It has @rpath, but only if the *dependency*
# says so: an LC_LOAD_DYLIB entry is a verbatim copy of that library's
# LC_ID_DYLIB. Every prebuilt CPython names itself with an absolute path --
# setup-python's tool cache, Homebrew, python.org's framework -- so a binary
# linked against one carries that absolute path and the package only runs on a
# machine where that exact path exists. That is a build machine, not a user's.
#
# So: ship the dylib the binary actually names, rename it to @rpath, and point
# the binary at it. The executable's RPATH already contains @loader_path/lib.
#
# --- strip ------------------------------------------------------------------
#
# The bundled interpreter is somebody else's build and is not necessarily
# stripped; the one setup-python installs is not. Use -x -S, never a bare
# strip: on a dylib that removes symbols the loader still needs.
#
# --- sign -------------------------------------------------------------------
#
# Not optional and not last by accident. arm64 Mach-O binaries without at
# least an ad-hoc signature are killed by the kernel, not merely warned about
# by Gatekeeper -- and both install_name_tool and strip invalidate whatever
# signature the file arrived with. Sign inside-out: dependencies first, the
# executable last, so its signature covers a tree that has stopped changing.

if(NOT EXE OR NOT STAGE)
    message(FATAL_ERROR "MacFinalize.cmake requires EXE and STAGE")
endif()
if(NOT EXISTS "${EXE}")
    message(FATAL_ERROR "MacFinalize.cmake: no such executable: ${EXE}")
endif()

set(LIBDIR "${STAGE}/lib")

find_program(OTOOL otool REQUIRED)
find_program(INSTALL_NAME_TOOL install_name_tool REQUIRED)
find_program(STRIP_TOOL strip REQUIRED)
find_program(CODESIGN codesign REQUIRED)

# --- relocate ---------------------------------------------------------------

execute_process(COMMAND "${OTOOL}" -L "${EXE}"
                OUTPUT_VARIABLE deps RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "otool -L failed on ${EXE}")
endif()

string(REPLACE "\n" ";" dep_lines "${deps}")

set(fixed "")
foreach(line ${dep_lines})
    # "\t/some/path/libfoo.dylib (compatibility version ...)"
    if(NOT line MATCHES "^\t(.+) \\(compatibility")
        continue()
    endif()
    set(ref "${CMAKE_MATCH_1}")

    # Only CPython. SDL3 is ours and already built with an @rpath id; system
    # libraries under /usr/lib and /System stay where they are.
    if(NOT ref MATCHES "libpython|Python\\.framework")
        continue()
    endif()
    if(ref MATCHES "^@")
        message(STATUS "MacFinalize: already relative: ${ref}")
        list(APPEND fixed "${ref}")
        continue()
    endif()
    if(NOT EXISTS "${ref}")
        message(FATAL_ERROR
            "MacFinalize: ${EXE} loads ${ref}, which does not exist on this "
            "machine. Nothing to ship.")
    endif()

    get_filename_component(name "${ref}" NAME)
    file(MAKE_DIRECTORY "${LIBDIR}")
    file(COPY "${ref}" DESTINATION "${LIBDIR}" FOLLOW_SYMLINK_CHAIN)
    file(CHMOD "${LIBDIR}/${name}" PERMISSIONS
         OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
         WORLD_READ WORLD_EXECUTE)

    execute_process(COMMAND "${INSTALL_NAME_TOOL}"
                            -id "@rpath/${name}" "${LIBDIR}/${name}"
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "install_name_tool -id failed on ${LIBDIR}/${name}")
    endif()

    execute_process(COMMAND "${INSTALL_NAME_TOOL}"
                            -change "${ref}" "@rpath/${name}" "${EXE}"
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "install_name_tool -change failed on ${EXE}")
    endif()

    message(STATUS "MacFinalize: ${ref} -> @rpath/${name}")
    list(APPEND fixed "${name}")
    list(APPEND relocated "${LIBDIR}/${name}")
endforeach()

if(NOT fixed)
    message(FATAL_ERROR
        "MacFinalize: ${EXE} names no libpython at all. Either Python was "
        "linked statically -- in which case this step and the shipped dylib "
        "are both wrong -- or the link is not what this script assumes.")
endif()

# --- strip, then sign -------------------------------------------------------
#
# Found by content, not by extension. A *.dylib/*.so glob looks obviously
# right and is wrong here: a framework CPython -- which is what
# actions/setup-python installs -- names its dylib `Python`, no extension at
# all. Globbing skipped the one file whose signature this script had just
# invalidated, reported success, and left dyld to abort with `code signature
# invalid` at startup.
execute_process(
    COMMAND sh -c
            "find '${STAGE}' -type f -exec file --mime-type {} + | awk -F': ' '$2 ~ /mach-binary/ {print $1}'"
    OUTPUT_VARIABLE machos_raw
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "MacFinalize: could not enumerate Mach-O files in ${STAGE}")
endif()
string(STRIP "${machos_raw}" machos_raw)
string(REPLACE "\n" ";" machos "${machos_raw}")

# The executable goes last: its signature has to cover a tree that has stopped
# changing.
list(REMOVE_ITEM machos "${EXE}")
list(APPEND machos "${EXE}")

# Whatever was relocated above must be in that list, because it is exactly the
# set of files whose signatures are now invalid. If detection ever misses one
# again, it should say so here rather than in dyld.
foreach(f ${relocated})
    if(NOT f IN_LIST machos)
        message(FATAL_ERROR
            "MacFinalize: ${f} was relocated but is not recognised as a Mach-O "
            "binary, so it would ship with the signature this script broke.")
    endif()
endforeach()

set(before 0)
set(after 0)
foreach(f ${machos})
    file(SIZE "${f}" sz)
    math(EXPR before "${before} + ${sz}")

    execute_process(COMMAND "${STRIP_TOOL}" -x -S "${f}"
                    RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "strip failed on ${f}: ${err}")
    endif()

    execute_process(COMMAND "${CODESIGN}" --force --sign - "${f}"
                    RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "codesign failed on ${f}: ${err}")
    endif()

    file(SIZE "${f}" sz)
    math(EXPR after "${after} + ${sz}")
endforeach()

# Verify rather than assume. An unsigned or stale-signed binary is not a
# degraded package, it is one the kernel refuses to run, and the only other
# place that shows up is a user's machine.
foreach(f ${machos})
    execute_process(COMMAND "${CODESIGN}" --verify --strict "${f}"
                    RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "codesign --verify failed on ${f}: ${err}")
    endif()
endforeach()

list(LENGTH machos n)
math(EXPR saved_mb "(${before} - ${after}) / 1048576")
message(STATUS "MacFinalize: stripped, signed and verified ${n} binaries, ${saved_mb} MB removed")
