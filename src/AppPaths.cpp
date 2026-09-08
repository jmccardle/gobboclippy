#include "AppPaths.h"

#include <SDL3/SDL.h>

#include <sys/stat.h>

namespace {
std::string g_base;

std::string join(const std::string& a, const std::string& b)
{
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}
} // namespace

namespace AppPaths {

bool init(std::string& error_out)
{
    // SDL_GetBasePath returns the directory of the running executable. It
    // returns NULL if SDL cannot determine it, which we treat as fatal rather
    // than guessing from argv[0] or the current working directory -- a wrong
    // base path would silently load the wrong scripts.
    const char* base = SDL_GetBasePath();
    if (!base || !*base) {
        error_out = std::string("SDL_GetBasePath() failed: ") + SDL_GetError();
        return false;
    }
    g_base = base;
    return true;
}

const std::string& base() { return g_base; }

std::string asset(const std::string& name)  { return join(join(g_base, "assets"), name); }
std::string script(const std::string& name) { return join(join(g_base, "scripts"), name); }
std::string scriptDir()                     { return join(g_base, "scripts"); }
std::string libDir()                        { return join(g_base, "lib"); }

bool exists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

} // namespace AppPaths
