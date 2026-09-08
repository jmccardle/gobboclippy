# Make the staged macOS tree relocatable.
#
# Run in script mode as a POST_BUILD step of package-dir:
#   cmake -DEXE=... -DLIBDIR=... -P MacRelocate.cmake
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
# Signing is the second half and cannot be skipped. arm64 Mach-O binaries
# without at least an ad-hoc signature are killed by the kernel, not merely
# warned about by Gatekeeper -- and install_name_tool invalidates the signature
# the linker applied. Sign inside-out: dependency first, then the executable.

if(NOT EXE OR NOT LIBDIR)
    message(FATAL_ERROR "MacRelocate.cmake requires EXE and LIBDIR")
endif()
if(NOT EXISTS "${EXE}")
    message(FATAL_ERROR "MacRelocate.cmake: no such executable: ${EXE}")
endif()

find_program(OTOOL otool REQUIRED)
find_program(INSTALL_NAME_TOOL install_name_tool REQUIRED)
find_program(CODESIGN codesign REQUIRED)

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
        message(STATUS "MacRelocate: already relative: ${ref}")
        list(APPEND fixed "${ref}")
        continue()
    endif()
    if(NOT EXISTS "${ref}")
        message(FATAL_ERROR
            "MacRelocate: ${EXE} loads ${ref}, which does not exist on this "
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

    execute_process(COMMAND "${CODESIGN}" --force --sign - "${LIBDIR}/${name}"
                    RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "codesign failed on ${LIBDIR}/${name}")
    endif()

    message(STATUS "MacRelocate: ${ref} -> @rpath/${name}")
    list(APPEND fixed "${name}")
endforeach()

if(NOT fixed)
    message(FATAL_ERROR
        "MacRelocate: ${EXE} names no libpython at all. Either Python was "
        "linked statically -- in which case this step and the shipped dylib "
        "are both wrong -- or the link is not what this script assumes.")
endif()

# The executable last: its signature covers the load commands just rewritten.
execute_process(COMMAND "${CODESIGN}" --force --sign - "${EXE}"
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "codesign failed on ${EXE}")
endif()
