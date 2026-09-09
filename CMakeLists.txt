cmake_minimum_required(VERSION 3.21)
project(gobboclippy VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GC_STATIC_SDL "Link SDL3 statically into the executable" OFF)

# ---------------------------------------------------------------------------
# Dependencies
#
# Both are fetched and pinned. To build against a local checkout instead:
#   cmake -DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL ...
# ---------------------------------------------------------------------------
include(FetchContent)

set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL       OFF CACHE BOOL "" FORCE)
if(GC_STATIC_SDL)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON  CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.4.16
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)

# stb: PNG decoding (stb_image) and text rasterising (stb_truetype, which pulls
# in stb_rect_pack for the glyph atlas). SDL3 core ships neither an image loader
# nor any text, and SDL3_image + SDL3_ttf + FreeType is a far heavier dependency
# than three headers -- particularly for the Windows cross-build.
#
# McRogueFace gets both from SFML instead, so this is the one part of the
# harvest that is a rewrite rather than a move. Its SDL2 backend already uses
# stb for images, so half of it was the intended path anyway.
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        f0569113c93ad095470c54bf34a17b36646bbbb5
    GIT_SHALLOW    FALSE
    GIT_PROGRESS   TRUE
)

FetchContent_MakeAvailable(SDL3 stb)

# Python. Two paths: a native build finds the host interpreter, a cross-build
# is handed a prebuilt Windows kit. Both end up providing GC_PY_TAG/GC_PY_VERSION
# and a link target, so nothing downstream needs to know which happened.
if(CMAKE_CROSSCOMPILING)
    include(cmake/WindowsPython.cmake)
    set(GC_PYTHON_TARGET gc_winpython)
    set(GC_PY_TAG_VALUE     "${GC_PY_TAG_X}")
    set(GC_PY_VERSION_VALUE "${GC_PY_VERSION_X}")
else()
    # Interpreter is needed as well as Development.Embed: it is what defines
    # Python3_EXECUTABLE and Python3_STDLIB, which the packaging step uses to
    # build the bundled stdlib zip.
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter Development.Embed)
    set(GC_PYTHON_TARGET Python3::Python)
    set(GC_PY_TAG_VALUE     "${Python3_VERSION_MAJOR}${Python3_VERSION_MINOR}")
    set(GC_PY_VERSION_VALUE "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}")
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(gobboclippy
    src/main.cpp
    src/PetWindow.cpp
    src/Tray.cpp
    src/PyClippy.cpp
    src/Capabilities.cpp
    src/AppPaths.cpp

    # The drawing layer harvested from McRogueFace. Everything above this line
    # is host; everything below it is the sprite/text/animation subset, and
    # only PyDraw.cpp in it knows that Python exists.
    src/Easing.cpp
    src/Texture.cpp
    src/Font.cpp
    src/Drawable.cpp
    src/Sprite.cpp
    src/Caption.cpp
    src/Animation.cpp
    src/PyDraw.cpp
)

target_include_directories(gobboclippy PRIVATE src ${stb_SOURCE_DIR})

if(GC_STATIC_SDL)
    target_link_libraries(gobboclippy PRIVATE SDL3::SDL3-static)
else()
    target_link_libraries(gobboclippy PRIVATE SDL3::SDL3)
endif()
target_link_libraries(gobboclippy PRIVATE ${GC_PYTHON_TARGET})

target_compile_definitions(gobboclippy PRIVATE
    GC_VERSION="${PROJECT_VERSION}"
    GC_PY_VERSION="${GC_PY_VERSION_VALUE}"
    GC_PY_TAG="${GC_PY_TAG_VALUE}"
)

# On Windows, build as a GUI app so no console window appears alongside the pet.
#
# GC_WIN_CONSOLE=ON builds a console-subsystem binary instead. That is what
# McRogueFace ships, and it sidesteps the stdio-handle problem in
# ensureStdioStreams() entirely, at the cost of a console window appearing
# behind the pet. Useful for bisecting startup failures.
option(GC_WIN_CONSOLE "Build Windows binary as a console app (debugging)" OFF)

if(WIN32 AND NOT GC_WIN_CONSOLE)
    set_target_properties(gobboclippy PROPERTIES WIN32_EXECUTABLE TRUE)

    # /SUBSYSTEM:WINDOWS makes the linker look for WinMain. mingw ships a shim
    # that forwards it to main; MSVC does not, and fails with an unresolved
    # WinMain in exe_winmain.obj. Naming the standard CRT entry point directly
    # is the MSVC counterpart of what mingw does for us.
    #
    # SDL3::SDL3main is the fuller answer -- it also hands main() a UTF-8 argv,
    # which neither toolchain does here -- but it would change the mingw
    # cross-build, which is the Windows binary that has actually been tested.
    # Worth revisiting if a non-ASCII --script path ever needs to work.
    if(MSVC)
        target_link_options(gobboclippy PRIVATE "/ENTRY:mainCRTStartup")
    endif()
endif()

# Resolve shared libraries relative to the executable, so the packaged
# directory is relocatable. Without this the binary carries an absolute
# RUNPATH into the build tree and only runs on the machine that built it.
if(APPLE)
    set_target_properties(gobboclippy PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@loader_path;@loader_path/lib")
elseif(UNIX)
    set_target_properties(gobboclippy PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN:$ORIGIN/lib")
endif()

# ---------------------------------------------------------------------------
# Dev convenience: assets/ and scripts/ beside the binary so the build tree
# runs without an install step. AppPaths.cpp resolves these relative to the
# executable, never to an absolute build path.
# ---------------------------------------------------------------------------
add_custom_command(TARGET gobboclippy POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:gobboclippy>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/scripts $<TARGET_FILE_DIR:gobboclippy>/scripts
    COMMENT "Staging assets/ and scripts/ beside the binary"
)

# SDL3 beside the binary too. The RPATH is $ORIGIN so that the packaged tree is
# relocatable; without this the build tree would be the one layout that cannot
# find its own libraries.
#
# Chosen with if() rather than $<IF:...>: a generator expression must be valid
# in *both* branches, and TARGET_SONAME_FILE does not exist for a Windows DLL.
if(NOT GC_STATIC_SDL)
    if(WIN32)
        set(GC_SDL_DEV $<TARGET_FILE:SDL3::SDL3>)
    else()
        set(GC_SDL_DEV $<TARGET_SONAME_FILE:SDL3::SDL3>)
    endif()
    add_custom_command(TARGET gobboclippy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GC_SDL_DEV}" $<TARGET_FILE_DIR:gobboclippy>
        VERBATIM)
endif()

include(cmake/Package.cmake)
