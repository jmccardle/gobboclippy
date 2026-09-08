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

// Must be called once, after SDL_Init, before anything else here.
bool init(std::string& error_out);

const std::string& base();                    // directory containing the exe
std::string asset(const std::string& name);   // <base>/assets/<name>
std::string script(const std::string& name);  // <base>/scripts/<name>
std::string scriptDir();                      // <base>/scripts
std::string libDir();                         // <base>/lib

bool exists(const std::string& path);

} // namespace AppPaths
