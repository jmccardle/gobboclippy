#pragma once
#include <string>

// All runtime paths are resolved relative to the executable, never baked in at
// build time. The build tree and the packaged tree therefore have the same
// layout, and the whole directory stays relocatable:
//
//   gobboclippy(.exe)
//   assets/
//   scripts/
//   lib/            (packaged builds only: libpython + stdlib zip)
//
namespace AppPaths {

// Must be called once before anything else here. SDL_Init is not required:
// SDL_GetBasePath is a filesystem call, and the interpreter mode uses it
// without ever bringing up video.
bool init(std::string& error_out);

const std::string& base();                    // directory containing the exe
std::string asset(const std::string& name);   // <base>/assets/<name>
std::string script(const std::string& name);  // <base>/scripts/<name>
std::string scriptDir();                      // <base>/scripts
std::string libDir();                         // <base>/lib

// What a script means when it names an asset. A relative path resolves under
// assets/, subdirectories included, so a set of art can be foldered and still
// be found wherever the tree is installed. An absolute path is taken as given,
// which is how a script reaches art it ships alongside itself.
//
// Relative used to mean "relative to the working directory" as soon as the
// path contained a separator, which made a foldered asset unreachable except
// by a path that only worked when launched from one particular directory.
std::string resolveAsset(const std::string& name);

// Whether a path names a location on its own, without a working directory.
// Covers the Windows forms as well: "C:\x", "C:/x" and "\\server\share".
bool isAbsolute(const std::string& path);

bool exists(const std::string& path);

// Read a whole file into memory. Used for Python sources: handing CPython a
// FILE* is unsafe when the interpreter links a different C runtime than this
// binary, which is the case for the mingw-built Windows target.
bool readFile(const std::string& path, std::string& out, std::string& error_out);

} // namespace AppPaths
