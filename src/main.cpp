#include <Python.h>          // must precede SDL headers on some platforms

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>

#include "Animation.h"
#include "App.h"
#include "AppPaths.h"
#include "Drawable.h"
#include "PyClippy.h"

namespace {

struct Options {
    int         width           = 256;
    int         height          = 256;
    std::string script;                 // empty -> scripts/clippy.py
    bool        print_caps_only = false;
};

// Give the process a valid stdin, stdout and stderr.
//
// The Windows build is a GUI-subsystem binary so that double-clicking the pet
// does not open a console window alongside it. Two consequences, both handled
// here:
//
//   1. Run from a terminal, stdout is discarded, so --capabilities and --help
//      would print nothing. AttachConsole reclaims the parent's console.
//
//   2. Run with no console at all, the standard handles are invalid. CPython
//      probes each of fds 0/1/2 during startup; a fd that is cleanly invalid
//      yields sys.stdout = None and is not an error, but one that passes the
//      probe and then fails on real I/O aborts startup outright. Giving the
//      process three real descriptors avoids the question.
//
// Binding the leftovers to NUL is not papering over an error. A GUI process
// legitimately has nowhere for stdio to go, and NUL is what that means.
void ensureStdioStreams()
{
#ifdef _WIN32
    FILE* unused = nullptr;

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (_fileno(stdout) < 0) freopen_s(&unused, "CONOUT$", "w", stdout);
        if (_fileno(stderr) < 0) freopen_s(&unused, "CONOUT$", "w", stderr);
        if (_fileno(stdin)  < 0) freopen_s(&unused, "CONIN$",  "r", stdin);
    }

    if (_fileno(stdout) < 0) freopen_s(&unused, "NUL", "w", stdout);
    if (_fileno(stderr) < 0) freopen_s(&unused, "NUL", "w", stderr);
    if (_fileno(stdin)  < 0) freopen_s(&unused, "NUL", "r", stdin);
#endif
}

void usage(const char* argv0)
{
    std::printf(
        "gobboclippy " GC_VERSION "\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "  --size N | WxH    Window size in pixels (default 256, i.e. 256x256)\n"
        "  --script PATH     Python entry point (default scripts/clippy.py)\n"
        "  --capabilities    Print the platform capability report and exit\n"
        "  --version         Print version and exit\n"
        "  --help            This message\n",
        argv0);
}

bool parseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires an argument\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (!std::strcmp(a, "--help") || !std::strcmp(a, "-h")) {
            usage(argv[0]);
            std::exit(0);
        } else if (!std::strcmp(a, "--version")) {
            std::printf("gobboclippy " GC_VERSION "\n");
            std::exit(0);
        } else if (!std::strcmp(a, "--capabilities")) {
            o.print_caps_only = true;
        } else if (!std::strcmp(a, "--size")) {
            const char* v = next("--size"); if (!v) return false;

            // "N" is square; "WxH" is not. Parsed strictly rather than with
            // atoi's habit of reading "256xyz" as 256 and saying nothing.
            int w = 0, h = 0;
            char tail = 0;
            const int fields = std::sscanf(v, "%d x %d %c", &w, &h, &tail);
            if (fields == 1)      h = w;
            else if (fields != 2) {
                std::fprintf(stderr, "--size takes N or WxH, e.g. 256 or 300x360\n");
                return false;
            }
            if (w < 32 || w > 2048 || h < 32 || h > 2048) {
                std::fprintf(stderr, "--size edges must be between 32 and 2048\n");
                return false;
            }
            o.width  = w;
            o.height = h;
        } else if (!std::strcmp(a, "--script")) {
            const char* v = next("--script"); if (!v) return false;
            o.script = v;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", a);
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

// Locate a bundled CPython runtime, if the package ships one.
//
// Development builds have none and use whatever Python the binary was linked
// against. Which mode is in effect is logged rather than inferred silently,
// because a wrong runtime surfaces much later as a confusing import error.
struct Runtime {
    bool        bundled = false;
    std::string home;
    std::string stdlib_zip;
    std::string dynload;               // POSIX: lib/python3.X/lib-dynload

    // Whether to let CPython derive sys.path from home, or to state it.
    // Windows derivation works and is what McRogueFace relies on with this
    // exact runtime; the POSIX derivation does not find a bundled tree, so
    // there the paths are given explicitly.
    bool        derive_paths = false;
};

Runtime findRuntime()
{
    Runtime rt;

    // Two legitimate layouts, because CPython's own default sys.path differs:
    // Windows keeps python3XX.zip at the prefix root with .pyd files in DLLs/,
    // POSIX puts the zip under lib/ with lib-dynload beside it.
    const std::string zip_root = AppPaths::base() + "python" GC_PY_TAG ".zip";
    const std::string zip_lib  = AppPaths::libDir() + "/python" GC_PY_TAG ".zip";

    if (AppPaths::exists(zip_root))      rt.stdlib_zip = zip_root;
    else if (AppPaths::exists(zip_lib))  rt.stdlib_zip = zip_lib;
    else {
        SDL_Log("python: system runtime " GC_PY_VERSION
                " (no bundled python" GC_PY_TAG ".zip beside the binary)");
        return rt;
    }

    rt.bundled = true;

#ifdef _WIN32
    // The package root is the prefix, matching python.org's embeddable layout:
    // python3XX.zip and python3XX.dll beside the exe, .pyd files in DLLs/.
    // CPython derives sys.path from there itself, so leave it to do that.
    //
    // This used to point at <base>/lib/Python and ship a second, loose copy of
    // the standard library, on the theory that Windows CPython could not start
    // without one. It can; see docs/cross-compile.md.
    rt.home         = AppPaths::base();
    rt.derive_paths = true;
#else
    rt.home         = AppPaths::base();
    rt.derive_paths = false;

    const std::string dynload = AppPaths::libDir() + "/python" GC_PY_VERSION "/lib-dynload";
    if (AppPaths::exists(dynload)) rt.dynload = dynload;
#endif

    SDL_Log("python: bundled runtime " GC_PY_VERSION " (%s)", rt.stdlib_zip.c_str());
    return rt;
}

bool startPython(const Options& opts, std::string& error_out)
{
    (void)opts;

    // Pre-initialise in UTF-8 mode, before Py_InitializeFromConfig.
    //
    // This fixes the encoding of paths and stdio rather than deriving it from
    // the console code page or the locale, so a script behaves the same on a
    // machine whose console is cp437 as on one set to UTF-8. It is hygiene,
    // not a fix for anything: contrary to what this comment used to claim, it
    // was never what stood between this build and a working interpreter.
    PyPreConfig preconfig;
    PyPreConfig_InitIsolatedConfig(&preconfig);
    preconfig.utf8_mode = 1;

    PyStatus status = Py_PreInitialize(&preconfig);
    if (PyStatus_Exception(status)) {
        error_out = std::string("Py_PreInitialize: ") +
                    (status.err_msg ? status.err_msg : "unknown");
        return false;
    }

    const Runtime rt = findRuntime();

    PyConfig config;
    if (rt.bundled) {
        PyConfig_InitIsolatedConfig(&config);
    } else {
        // Development: let CPython find the system interpreter the normal way.
        PyConfig_InitPythonConfig(&config);
    }

    config.configure_c_stdio = 1;
    PyConfig_SetBytesString(&config, &config.stdio_encoding, "utf-8");
    PyConfig_SetBytesString(&config, &config.stdio_errors,   "surrogateescape");
    PyConfig_SetBytesString(&config, &config.program_name,   "gobboclippy");

    if (rt.bundled) {
        PyConfig_SetBytesString(&config, &config.home, rt.home.c_str());
    }

    if (rt.bundled && !rt.derive_paths) {

        // State sys.path outright rather than letting CPython derive it from
        // home. The derivation rules differ between Windows and POSIX, and
        // when they get it wrong they do so silently -- the interpreter comes
        // up with an empty path and every import fails.
        config.module_search_paths_set = 1;
        const std::string paths[] = { rt.stdlib_zip, rt.dynload, rt.home };
        for (const std::string& path : paths) {
            if (path.empty()) continue;

            // Py_DecodeLocale allocates, and returns NULL on a decoding error
            // or out of memory. Appending NULL would be undefined, and the
            // buffer is ours to release either way.
            wchar_t* wide = Py_DecodeLocale(path.c_str(), nullptr);
            if (!wide) {
                error_out = "could not decode " + path + " for the module search path";
                PyConfig_Clear(&config);
                return false;
            }

            status = PyWideStringList_Append(&config.module_search_paths, wide);
            PyMem_RawFree(wide);

            if (PyStatus_Exception(status)) {
                error_out = "could not add " + path + " to the module search path";
                PyConfig_Clear(&config);
                return false;
            }
        }
    }

    status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);

    if (PyStatus_Exception(status)) {
        error_out = std::string("Py_InitializeFromConfig: ") +
                    (status.err_msg ? status.err_msg : "unknown");
        return false;
    }

    // scripts/ on sys.path so the entry point can import siblings.
    const std::string dir = AppPaths::scriptDir();
    PyObject* sys_path = PySys_GetObject("path");     // borrowed
    if (!sys_path) {
        error_out = "sys.path unavailable";
        return false;
    }
    PyObject* p = PyUnicode_FromString(dir.c_str());
    if (!p || PyList_Insert(sys_path, 0, p) < 0) {
        Py_XDECREF(p);
        error_out = "could not add scripts/ to sys.path";
        return false;
    }
    Py_DECREF(p);
    return true;
}

// Run a script by compiling its source, never by handing CPython a FILE*.
//
// PyRun_SimpleFile takes a FILE*, and a FILE* may not cross a C runtime
// boundary. On Windows this build is mingw (msvcrt.dll) while the bundled
// python3XX.dll is the python.org embeddable build (UCRT, api-ms-win-crt-*).
// The two runtimes have incompatible FILE layouts, so a FILE* opened here and
// read there is undefined behaviour -- it hangs rather than failing cleanly,
// which is worse. CPython documents the constraint:
//
//   "the FILE structure for different C libraries can be different and
//    incompatible ... care should be taken that FILE* parameters are only
//    passed to these functions if it is certain that they were created by the
//    same library that the Python runtime is using."
//   -- https://docs.python.org/3/c-api/veryhigh.html
//
// Reading the bytes ourselves and compiling them sidesteps the boundary
// entirely. It is also what gives tracebacks the real script path.
bool runScript(const std::string& path, std::string& error_out)
{
    std::string source;
    if (!AppPaths::readFile(path, source, error_out)) return false;

    PyObject* code = Py_CompileString(source.c_str(), path.c_str(), Py_file_input);
    if (!code) {
        PyErr_Print();
        error_out = "could not compile: " + path;
        return false;
    }

    PyObject* main_mod = PyImport_AddModule("__main__");   // borrowed
    if (!main_mod) {
        Py_DECREF(code);
        error_out = "no __main__ module";
        return false;
    }
    PyObject* globals = PyModule_GetDict(main_mod);        // borrowed

    PyObject* result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);

    if (!result) {
        PyErr_Print();
        error_out = "script raised an exception: " + path;
        return false;
    }
    Py_DECREF(result);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    ensureStdioStreams();

    Options opts;
    if (!parseArgs(argc, argv, opts)) return 2;

    SDL_SetAppMetadata("gobboclippy", GC_VERSION, "org.mcrogueface.gobboclippy");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    std::string err;
    if (!AppPaths::init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        SDL_Quit();
        return 1;
    }

    App app;
    app.width  = opts.width;
    app.height = opts.height;
    app.script = opts.script.empty() ? AppPaths::script("clippy.py") : opts.script;

    if (!app.window.create(app.width, app.height, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        SDL_Quit();
        return 1;
    }

    // --- tray -------------------------------------------------------------
    // A missing tray is fatal: the window is borderless with no close button,
    // so without the tray there is no way to show, hide or quit the app.
    app.tray.on_show = [&app] { app.setVisible(true); };
    app.tray.on_hide = [&app] { app.setVisible(false); };
    app.tray.on_exit = [&app] { app.running = false; };

    std::string tray_err;
    if (!app.tray.create(AppPaths::asset("clippy.png"), "gobboclippy", tray_err)) {
        std::fprintf(stderr,
            "Tray icon unavailable: %s\n"
            "The pet window is borderless and has no close button, so the tray\n"
            "is the only way to control it. Refusing to start headless.\n"
#ifdef __linux__
            "On Linux the tray needs GTK3 and libayatana-appindicator3 at runtime:\n"
            "  sudo apt install libgtk-3-0 libayatana-appindicator3-1\n"
#endif
            , tray_err.c_str());
        app.window.destroy();
        SDL_Quit();
        return 1;
    }
    app.window.caps_mutable().tray = true;

    if (opts.print_caps_only) {
        std::printf("%s", app.window.caps().report().c_str());
        app.tray.destroy();
        app.window.destroy();
        SDL_Quit();
        return 0;
    }

    SDL_Log("%s", app.window.caps().report().c_str());

    // --- python -----------------------------------------------------------
    if (!PyClippy::registerModule(err) || !startPython(opts, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        app.tray.destroy();
        app.window.destroy();
        SDL_Quit();
        return 1;
    }
    PyClippy::bind(&app);

    if (!runScript(app.script, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        Py_FinalizeEx();
        app.tray.destroy();
        app.window.destroy();
        SDL_Quit();
        return 1;
    }

    app.syncTray();

    // --- loop -------------------------------------------------------------
    // SDL_PollEvent also pumps the tray, so tray callbacks arrive on this
    // thread between iterations. No SDL_UpdateTrays() call is needed.
    //
    // Animations are driven by measured elapsed time rather than by a frame
    // count, so a dropped frame shortens the next step instead of stretching
    // the animation. The first frame's dt is zero by construction.
    Uint64 previous_ns = SDL_GetTicksNS();

    while (app.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                app.running = false;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // Closing the pet hides it; Exit in the tray is what quits.
                app.setVisible(false);
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                // The stage's bounds are what alignment is measured against,
                // so they follow the window rather than the request.
                app.window.onResized();
                break;
            default:
                break;
            }
        }

        const Uint64 now_ns = SDL_GetTicksNS();
        float dt = (float)((double)(now_ns - previous_ns) / 1.0e9);
        previous_ns = now_ns;

        // A long stall -- the machine slept, or a script blocked -- would
        // otherwise complete every running animation in one step. Cap it at a
        // few frames' worth: animations run slow through a hitch rather than
        // teleporting, which is the lesser of the two wrong answers.
        dt = SDL_min(dt, 0.1f);

        AnimationManager::instance().update(dt);
        PyClippy::fireFrame(dt);

        if (app.window.visible()) app.window.render();
        SDL_Delay(16);   // ~60fps ceiling; the pet is idle most of the time
    }

    PyClippy::fire("quit");

    // Drop every animation before the interpreter goes: a completion callback
    // holds a Python reference, and releasing one after Py_FinalizeEx is a
    // use-after-free.
    AnimationManager::instance().clear();
    Stage::instance().clear();

    Py_FinalizeEx();
    app.tray.destroy();
    app.window.destroy();
    SDL_Quit();
    return 0;
}
