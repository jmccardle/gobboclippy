# Strip the binaries in the staging tree (non-Apple).
#
# Run in script mode as a POST_BUILD step of package-dir:
#   cmake -DSTAGE=... -DSTRIP=... -DPLATFORM=... -P StripTree.cmake
#
# This exists because the interpreter we bundle is somebody else's build and is
# not necessarily stripped. Debian's is; the one actions/setup-python installs
# is not, and that alone took the Linux package from 8.0 MB to 20 MB compressed
# the first time CI produced it -- 23 MB of libpython and 16 MB of lib-dynload,
# almost all of it symbol tables nothing at runtime reads.
#
# Only .dynsym is needed to load a shared object and resolve its entry points;
# --strip-unneeded keeps that and discards the rest.
#
# Never operates outside STAGE. The bundled interpreter is copied from the
# machine's real Python installation, and stripping that in place would quietly
# vandalise the system.

if(NOT STAGE OR NOT STRIP OR NOT PLATFORM)
    message(FATAL_ERROR "StripTree.cmake requires STAGE, STRIP and PLATFORM")
endif()
if(NOT IS_DIRECTORY "${STAGE}")
    message(FATAL_ERROR "StripTree.cmake: no such staging tree: ${STAGE}")
endif()
if(NOT EXISTS "${STRIP}")
    message(FATAL_ERROR "StripTree.cmake: no strip tool at ${STRIP}")
endif()

if(PLATFORM STREQUAL "Windows")
    # Only the two binaries this build produced. The rest of the tree --
    # python3XX.dll, the .pyd modules, the OpenSSL and vcruntime DLLs -- comes
    # from python.org and was built by MSVC, which keeps its symbols in
    # separate .pdb files rather than in the image. There is nothing there for
    # a mingw strip to remove, and every reason not to point one at it.
    set(candidates "${STAGE}/gobboclippy.exe" "${STAGE}/SDL3.dll")
else()
    file(GLOB_RECURSE candidates
         "${STAGE}/*.so" "${STAGE}/*.so.*")
    list(APPEND candidates "${STAGE}/gobboclippy")
endif()

set(before 0)
set(after 0)
set(n 0)
foreach(f ${candidates})
    if(NOT EXISTS "${f}")
        continue()
    endif()
    file(SIZE "${f}" sz)
    math(EXPR before "${before} + ${sz}")

    execute_process(COMMAND "${STRIP}" --strip-unneeded "${f}"
                    RESULT_VARIABLE rc ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "strip failed on ${f}: ${err}")
    endif()

    file(SIZE "${f}" sz)
    math(EXPR after "${after} + ${sz}")
    math(EXPR n "${n} + 1")
endforeach()

if(n EQUAL 0)
    message(FATAL_ERROR
        "StripTree.cmake stripped nothing in ${STAGE}. The staging tree is not "
        "laid out the way this script assumes.")
endif()

math(EXPR saved_mb "(${before} - ${after}) / 1048576")
message(STATUS "stripped ${n} binaries, ${saved_mb} MB removed")
