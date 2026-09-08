# Build a CPython stdlib zip for the packaged runtime.
#
# Run in script mode by the package-dir target:
#   cmake -DSTDLIB_SRC=... -DZIP_OUT=... -DPY_EXE=... -P ZipStdlib.cmake
#
# Excludes test suites, __pycache__ and site-packages. Those are roughly half
# the stdlib on disk and nothing in a desktop pet imports them.

if(NOT STDLIB_SRC OR NOT ZIP_OUT OR NOT PY_EXE OR NOT WORK_DIR)
    message(FATAL_ERROR
        "ZipStdlib.cmake requires STDLIB_SRC, ZIP_OUT, PY_EXE and WORK_DIR")
endif()

if(NOT IS_DIRECTORY "${STDLIB_SRC}")
    message(FATAL_ERROR "Python stdlib not found at: ${STDLIB_SRC}")
endif()

get_filename_component(ZIP_DIR "${ZIP_OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${ZIP_DIR}")

# Written to a file rather than passed with -c so quoting stays sane across
# the three shells this runs under. It lives in the build tree, never in the
# staging tree, so it cannot leak into the shipped package.
set(SCRIPT "${WORK_DIR}/_zip_stdlib.py")
file(WRITE "${SCRIPT}" "
import os, sys, zipfile

src, out = sys.argv[1], sys.argv[2]
SKIP_DIRS = {'test', 'tests', 'idlelib', 'turtledemo', 'tkinter',
             'site-packages', '__pycache__', 'lib2to3', 'ensurepip',
             'distutils', 'config-*'}

def skip(rel):
    parts = rel.split(os.sep)
    return any(p in SKIP_DIRS for p in parts)

count = 0
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk(src):
        rel_root = os.path.relpath(root, src)
        if rel_root == '.':
            rel_root = ''
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in files:
            if not f.endswith(('.py',)):
                continue
            rel = os.path.join(rel_root, f) if rel_root else f
            if skip(rel):
                continue
            z.write(os.path.join(root, f), rel)
            count += 1

print(f'stdlib zip: {count} modules -> {out} '
      f'({os.path.getsize(out) / 1024 / 1024:.1f} MB)')
")

execute_process(
    COMMAND "${PY_EXE}" "${SCRIPT}" "${STDLIB_SRC}" "${ZIP_OUT}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE  err
)
message(STATUS "${out}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "stdlib zip failed (${rc}): ${err}")
endif()
