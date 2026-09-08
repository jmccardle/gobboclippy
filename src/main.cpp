#include <Python.h>          // must precede SDL headers on some platforms

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>

#include "App.h"
#include "AppPaths.h"
#include "PyClippy.h"

namespace {

struct Options {
    int         size            = 256;
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
//      builds sys.stdin/stdout/stderr from those descriptors during startup
//      and aborts with "can't initialize sys standard streams" if any one of
//      them is bad -- before a single line of script runs. stdin is the one
//      that bites, because redirecting output still leaves fd 0 invalid.
//
// McRogueFace never hits the second case: mcrogueface.exe is a console
// subsystem binary, so Windows always hands it the three streams.
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
        "  --size N          Window edge length in pixels (default 256)\n"
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
            o.size = std::atoi(v);
            if (o.size < 32 || o.size > 2048) {
                std::fprintf(stderr, "--size must be between 32 and 2048\n");
                return false;
            }
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
    // Windows CPython expects <home>/Lib to hold the standard library, and
    // finds python3XX.zip beside the executable on its own. Pointing home at
    // the package root instead -- where there is no Lib/ -- makes
    // Py_InitializeFromConfig fail with "can't initialize sys standard
    // streams", which names the symptom and not the cause.
    rt.home         = AppPaths::base() + "lib/Python";
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

    // Pre-initialise in UTF-8 mode.
    //
    // This has to happen before Py_InitializeFromConfig, and it is what stops
    // CPython probing the console code page to choose an encoding for
    // sys.stdout/stderr. A GUI-subsystem process has no console to probe, and
    // the failure is the singularly unhelpful "can't initialize sys standard
    // streams" raised before any script gets to run. Setting stdio_encoding on
    // PyConfig alone is too late to prevent it.
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
            status = PyWideStringList_Append(&config.module_search_paths,
                                             Py_DecodeLocale(path.c_str(), nullptr));
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

bool runScript(const std::string& path, std::string& error_out)
{
    if (!AppPaths::exists(path)) {
        error_out = "script not found: " + path;
        return false;
    }

    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) {
        error_out = "could not open script: " + path;
        return false;
    }

    const int rc = PyRun_SimpleFile(f, path.c_str());
    std::fclose(f);

    if (rc != 0) {
        error_out = "script raised an exception: " + path;
        return false;
    }
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
    app.size   = opts.size;
    app.script = opts.script.empty() ? AppPaths::script("clippy.py") : opts.script;

    if (!app.window.create(app.size, err)) {
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
            default:
                break;
            }
        }

        PyClippy::fire("frame");

        if (app.window.visible()) app.window.render();
        SDL_Delay(16);   // ~60fps ceiling; the pet is idle most of the time
    }

    PyClippy::fire("quit");

    Py_FinalizeEx();
    app.tray.destroy();
    app.window.destroy();
    SDL_Quit();
    return 0;
}
