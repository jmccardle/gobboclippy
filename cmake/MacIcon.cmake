# Build an .icns from a single PNG.
#
#   cmake -DPNG=... -DOUT=...icns -DWORK=... -P MacIcon.cmake
#
# A bundle without CFBundleIconFile gets the generic application icon, which is
# what an unfinished app looks like -- and the icon is the only thing the user
# sees in the DMG window before they decide to trust it.
#
# Only sizes at or below the source resolution are generated. iconutil accepts
# a subset of the iconset, and an upscaled 512 or 1024 is worse than none: the
# system falls back to the nearest real size rather than to a blurred one.
# assets/clippy.png is 256x256, rendered from assets/clippy.svg; if that ever
# grows, add the larger entries here rather than upscaling.

if(NOT PNG OR NOT OUT OR NOT WORK)
    message(FATAL_ERROR "MacIcon.cmake requires PNG, OUT and WORK")
endif()
if(NOT EXISTS "${PNG}")
    message(FATAL_ERROR "MacIcon.cmake: no such image: ${PNG}")
endif()

find_program(SIPS sips REQUIRED)
find_program(ICONUTIL iconutil REQUIRED)

set(ICONSET "${WORK}/gobboclippy.iconset")
file(REMOVE_RECURSE "${ICONSET}")
file(MAKE_DIRECTORY "${ICONSET}")

# size;name pairs. The @2x names are what a Retina display asks for, and they
# are the same pixels as the next size up under a different name -- that is the
# convention, not a duplicate.
set(SIZES
    16   "icon_16x16.png"
    32   "icon_16x16@2x.png"
    32   "icon_32x32.png"
    64   "icon_32x32@2x.png"
    128  "icon_128x128.png"
    256  "icon_128x128@2x.png"
    256  "icon_256x256.png"
)

list(LENGTH SIZES n)
math(EXPR last "${n} / 2 - 1")
foreach(i RANGE ${last})
    math(EXPR a "${i} * 2")
    math(EXPR b "${i} * 2 + 1")
    list(GET SIZES ${a} px)
    list(GET SIZES ${b} name)

    execute_process(
        COMMAND "${SIPS}" -z ${px} ${px} "${PNG}" --out "${ICONSET}/${name}"
        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "sips failed producing ${name}: ${err}")
    endif()
endforeach()

execute_process(COMMAND "${ICONUTIL}" -c icns "${ICONSET}" -o "${OUT}"
                RESULT_VARIABLE rc ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "iconutil failed: ${err}")
endif()

message(STATUS "MacIcon: ${OUT}")
