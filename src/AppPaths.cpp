#include "AppPaths.h"

#include <SDL3/SDL.h>

#include <sys/stat.h>

#include <cstring>

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

bool readFile(const std::string& path, std::string& out, std::string& error_out)
{
    // SDL_LoadFile rather than stdio: it is one call, it reports through
    // SDL_GetError, and it keeps the buffer on SDL's allocator instead of a
    // FILE* we would have to be careful with.
    size_t len = 0;
    void*  data = SDL_LoadFile(path.c_str(), &len);
    if (!data) {
        error_out = "could not read " + path + ": " + SDL_GetError();
        return false;
    }

    const char* p = static_cast<const char*>(data);

    // A UTF-8 BOM is legal in a Python source file (PEP 263) but Py_CompileString
    // does not accept one, so skip it.
    if (len >= 3 && p[0] == '\xEF' && p[1] == '\xBB' && p[2] == '\xBF') {
        p   += 3;
        len -= 3;
    }

    // Py_CompileString takes a NUL-terminated string, so an embedded NUL would
    // silently truncate the source. Refuse instead.
    if (std::memchr(p, '\0', len) != nullptr) {
        SDL_free(data);
        error_out = path + " contains a NUL byte; not a Python source file";
        return false;
    }

    out.assign(p, len);
    SDL_free(data);
    return true;
}

} // namespace AppPaths
