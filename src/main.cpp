#include <Python.h>          // must precede SDL headers on some platforms

#include <SDL3/SDL.h>

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

// Decide where CPython should look for its standard library.
//
// Packaged builds ship lib/python<ver>.zip beside the executable; development
// builds use whatever Python the binary was linked against. Which mode is in
// effect is logged rather than inferred silently, because a wrong PYTHONHOME
// surfaces much later as a confusing import error.
void configurePythonHome(PyConfig& config)
{
    const std::string zip = AppPaths::libDir() + "/python" GC_PY_TAG ".zip";
    const std::string dir = AppPaths::libDir() + "/python" GC_PY_VERSION;

    if (AppPaths::exists(zip) || AppPaths::exists(dir)) {
        const std::string home = AppPaths::base();
        PyConfig_SetBytesString(&config, &config.home, home.c_str());
        config.isolated              = 1;
        config.use_environment       = 0;
        config.user_site_directory   = 0;
        SDL_Log("python: bundled runtime (home=%s)", home.c_str());
    } else {
        SDL_Log("python: system runtime " GC_PY_VERSION
                " (no lib/python" GC_PY_TAG ".zip beside the binary)");
    }
}

bool startPython(const Options& opts, std::string& error_out)
{
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    PyConfig_SetBytesString(&config, &config.program_name, "gobboclippy");
    configurePythonHome(config);

    PyStatus status = Py_InitializeFromConfig(&config);
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
