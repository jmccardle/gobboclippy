#include <Python.h>          // must precede SDL headers on some platforms

#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Animation.h"
#include "App.h"
#include "AppPaths.h"
#include "Drawable.h"
#include "Mic.h"
#include "PyClippy.h"
#include "Settings.h"

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
        "  --help            This message\n"
        "\n"
        "Usage: %s --python [python args...]\n"
        "\n"
        "  Run the bundled interpreter as if it were python: -c, -m, a script,\n"
        "  or the REPL. Everything after --python is the interpreter's own\n"
        "  command line. Packages ship python3 (python.exe on Windows) beside\n"
        "  the binary, which is the same executable and behaves the same way.\n"
        "\n"
        "  %s --python -m pip install <package>\n",
        argv0, argv0, argv0);
}

// Whether this process was asked to be Python rather than the pet, and where
// Python's own arguments start if so. Zero means the pet.
//
// Two spellings, one behaviour:
//   gobboclippy --python [args...]     the flag has to come first, because
//                                      nothing after it is ours to parse
//   python3 [args...]                  the packaged symlink (a copy of the exe
//                                      on Windows), so that sys.executable is
//                                      something a subprocess can actually run
//                                      as python: pip's build isolation,
//                                      multiprocessing, anything that spawns
//                                      [sys.executable, "-c", ...]
int pythonArgvStart(int argc, char** argv)
{
    if (argc >= 2 && !std::strcmp(argv[1], "--python")) return 2;

    // Basename of argv[0], with a Windows suffix removed, starting with
    // "python": python3, python3.11, python.exe.
    std::string name = argc >= 1 && argv[0] ? argv[0] : "";
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name.erase(0, slash + 1);
    if (name.size() > 4) {
        std::string tail = name.substr(name.size() - 4);
        for (char& c : tail) c = (char)std::tolower((unsigned char)c);
        if (tail == ".exe") name.erase(name.size() - 4);
    }
    if (name.compare(0, 6, "python") == 0) return 1;

    return 0;
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
        } else if (!std::strcmp(a, "--python")) {
            // Anywhere but first it is ambiguous: the options before it are
            // the pet's, and the pet is not what runs.
            std::fprintf(stderr, "--python must be the first argument; "
                                 "everything after it belongs to Python\n");
            return false;
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
    std::string root;                  // the package directory
    std::string stdlib_zip;
    std::string dynload;               // POSIX: lib/python3.X/lib-dynload
                                       // Windows: DLLs
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

    // Without SDL_GetBasePath's trailing separator: this becomes sys.prefix's
    // parent and sys.base_prefix, and those are compared as strings.
    rt.root = AppPaths::base();
    while (!rt.root.empty() && (rt.root.back() == '/' || rt.root.back() == '\\'))
        rt.root.pop_back();

    // Where the stdlib's compiled extension modules sit. The two layouts
    // match what each platform's CPython would derive on its own: .pyd files
    // in DLLs/ beside the exe, as in python.org's embeddable package, and
    // lib-dynload under lib/python3.X on POSIX.
#ifdef _WIN32
    const std::string dynload = AppPaths::base() + "DLLs";
#else
    const std::string dynload = AppPaths::libDir() + "/python" GC_PY_VERSION "/lib-dynload";
#endif
    if (AppPaths::exists(dynload)) rt.dynload = dynload;

    SDL_Log("python: bundled runtime " GC_PY_VERSION " (%s)", rt.stdlib_zip.c_str());
    return rt;
}

// The name the package ships the interpreter alias under, beside the binary.
// A symlink to gobboclippy on POSIX, a copy of it on Windows; either way the
// same executable, dispatched on argv[0] by pythonArgvStart(). It is what
// sys.executable names in a bundled runtime, because that has to be a path a
// subprocess can run as python and the pet binary is not one.
#ifdef _WIN32
#define GC_PYTHON_ALIAS "python.exe"
#else
#define GC_PYTHON_ALIAS "python3"
#endif

// Pre-initialise in UTF-8 mode, before Py_InitializeFromConfig.
//
// This fixes the encoding of paths and stdio rather than deriving it from
// the console code page or the locale, so a script behaves the same on a
// machine whose console is cp437 as on one set to UTF-8. It is hygiene,
// not a fix for anything: contrary to what this comment used to claim, it
// was never what stood between this build and a working interpreter.
bool preInitialize(std::string& error_out)
{
    PyPreConfig preconfig;
    PyPreConfig_InitIsolatedConfig(&preconfig);
    preconfig.utf8_mode = 1;

    const PyStatus status = Py_PreInitialize(&preconfig);
    if (PyStatus_Exception(status)) {
        error_out = std::string("Py_PreInitialize: ") +
                    (status.err_msg ? status.err_msg : "unknown");
        return false;
    }
    return true;
}

bool setWide(PyConfig& config, wchar_t** field, const std::string& value,
             std::string& error_out)
{
    const PyStatus status = PyConfig_SetBytesString(&config, field, value.c_str());
    if (PyStatus_Exception(status)) {
        error_out = "could not set " + value + " in the interpreter config";
        return false;
    }
    return true;
}

// The one description of where the runtime lives, shared by the pet and the
// interpreter mode so the two cannot disagree about sys.path or sys.prefix.
//
// The caller has already chosen the config family: isolated for a bundled
// runtime, the ordinary Python config for a development build that borrows
// the system interpreter.
bool configureRuntime(PyConfig& config, const Runtime& rt, std::string& error_out)
{
    config.configure_c_stdio = 1;
    if (!setWide(config, &config.stdio_encoding, "utf-8",            error_out) ||
        !setWide(config, &config.stdio_errors,   "surrogateescape",  error_out) ||
        !setWide(config, &config.program_name,   "gobboclippy",      error_out))
        return false;

    if (!rt.bundled) return true;

    // Everything below is stated, nothing derived. CPython's path logic
    // (Modules/getpath.py) is built around a `home` it searches for landmarks
    // and derives prefix and sys.path from; this runtime does not set one,
    // because a home unconditionally overrides the prefix, and the prefix is
    // the point.
    //
    // sys.prefix is site/, one level below the package root that holds the
    // standard library, and sys.base_prefix is that root. That is CPython's
    // own model of a virtual environment -- stdlib in one prefix, installed
    // packages in another -- and it is used here for two reasons.
    //
    // The first is that it decides where pip installs. Debian's CPython
    // carries a sysconfig patch that answers `local/lib/python3.X/dist-packages`
    // whenever prefix and base_prefix agree, and `lib/python3.X/site-packages`
    // when they differ; vanilla CPython answers the second in both cases. A
    // package built on Debian and one built on a GitHub runner would otherwise
    // install to different places, and only one of them would be on sys.path.
    // Telling both builds they are a venv makes them agree.
    //
    // The second is that site.py then puts site/lib/python3.X/site-packages
    // (site/Lib/site-packages on Windows) on sys.path itself, in both the pet
    // and the interpreter mode: what `python3 -m pip install` puts there is
    // what `--script` can import.
    const std::string site = rt.root + "/site";
    if (!setWide(config, &config.prefix,           site,    error_out) ||
        !setWide(config, &config.exec_prefix,      site,    error_out) ||
        !setWide(config, &config.base_prefix,      rt.root, error_out) ||
        !setWide(config, &config.base_exec_prefix, rt.root, error_out))
        return false;

    // The alias, not this binary: see GC_PYTHON_ALIAS.
    const std::string exe = rt.root + "/" GC_PYTHON_ALIAS;
    if (!setWide(config, &config.executable,      exe, error_out) ||
        !setWide(config, &config.base_executable, exe, error_out))
        return false;

    // sys.path, in the order CPython itself would produce: the zip, the
    // extension modules, the root. site.py appends site-packages after these.
    config.module_search_paths_set = 1;
    const std::string paths[] = { rt.stdlib_zip, rt.dynload, rt.root };
    for (const std::string& path : paths) {
        if (path.empty()) continue;

        // Py_DecodeLocale allocates, and returns NULL on a decoding error
        // or out of memory. Appending NULL would be undefined, and the
        // buffer is ours to release either way.
        wchar_t* wide = Py_DecodeLocale(path.c_str(), nullptr);
        if (!wide) {
            error_out = "could not decode " + path + " for the module search path";
            return false;
        }

        const PyStatus status = PyWideStringList_Append(&config.module_search_paths, wide);
        PyMem_RawFree(wide);

        if (PyStatus_Exception(status)) {
            error_out = "could not add " + path + " to the module search path";
            return false;
        }
    }
    return true;
}

bool initializeFromConfig(PyConfig& config, std::string& error_out)
{
    const PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);

    // `python -h` and `python -V` are answered during initialisation, and
    // come back as an exit request rather than a failure. This is the
    // handler CPython documents for that case: it exits with the code.
    if (PyStatus_IsExit(status)) Py_ExitStatusException(status);

    if (PyStatus_Exception(status)) {
        error_out = std::string("Py_InitializeFromConfig: ") +
                    (status.err_msg ? status.err_msg : "unknown");
        return false;
    }
    return true;
}

// Insert at the front (index 0) or append (index -1) to sys.path.
bool addSysPath(const std::string& dir, int index, std::string& error_out)
{
    PyObject* sys_path = PySys_GetObject("path");     // borrowed
    if (!sys_path) {
        error_out = "sys.path unavailable";
        return false;
    }
    PyObject* p = PyUnicode_FromString(dir.c_str());
    const int rc = !p ? -1
                 : index < 0 ? PyList_Append(sys_path, p)
                 : PyList_Insert(sys_path, index, p);
    Py_XDECREF(p);
    if (rc < 0) {
        error_out = "could not add " + dir + " to sys.path";
        return false;
    }
    return true;
}

// pip ships as its own wheel, and a wheel is importable straight off sys.path
// -- pip's own bootstrap documents running it that way. It goes last, behind
// site-packages, so a pip installed there by `pip install --upgrade pip`
// shadows the shipped one rather than the other way round.
//
// Only a bundled runtime does this. A development build borrows the system
// interpreter and gets the system's pip with it.
bool addShippedPip(const Runtime& rt, std::string& error_out)
{
    if (!rt.bundled) return true;
    return addSysPath(AppPaths::libDir() + "/" GC_PIP_WHEEL, -1, error_out);
}

bool startPython(std::string& error_out)
{
    if (!preInitialize(error_out)) return false;

    const Runtime rt = findRuntime();

    PyConfig config;
    if (rt.bundled) {
        PyConfig_InitIsolatedConfig(&config);
    } else {
        // Development: let CPython find the system interpreter the normal way.
        PyConfig_InitPythonConfig(&config);
    }

    if (!configureRuntime(config, rt, error_out)) {
        PyConfig_Clear(&config);
        return false;
    }
    if (!initializeFromConfig(config, error_out)) return false;

    // scripts/ on sys.path so the entry point can import siblings.
    return addSysPath(AppPaths::scriptDir(), 0, error_out) &&
           addShippedPip(rt, error_out);
}

// Be python. argv[start..] is the interpreter's command line, parsed by
// CPython itself: -c, -m, a script path, - for stdin, nothing for the REPL,
// and every flag python accepts. Py_RunMain then owns the process until the
// interpreter finalises, and its return value is the exit status.
//
// No SDL beyond SDL_GetBasePath, no window, no tray: a pip install runs on a
// machine with no display, and the tray check is fatal by design.
int runInterpreter(int argc, char** argv, int start)
{
    std::string err;
    if (!AppPaths::init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // The pet announces which runtime it found because a wrong one surfaces
    // later as a confusing import error. An interpreter answers the same
    // question through sys.prefix, and python does not narrate its startup.
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_WARN);

    if (!preInitialize(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    const Runtime rt = findRuntime();

    PyConfig config;
    if (rt.bundled) {
        PyConfig_InitIsolatedConfig(&config);
    } else {
        PyConfig_InitPythonConfig(&config);
    }

    // Where the isolated config and "be python" disagree, python wins:
    //   parse_argv     the command line is CPython's to interpret
    //   isolated       off, because CPython reads it as -I, and -I forces
    //                  safe_path: python puts the script's directory (or the
    //                  cwd, for -m) first on sys.path, and -P is still there
    //                  for anyone who wants that back
    //   signals        Ctrl-C in the REPL is KeyboardInterrupt, not death
    // What -I also implied is kept, and now said directly: no PYTHON*
    // environment variables and no user site. This runtime is self-contained
    // by construction, and importing the host's packages into it would be
    // importing modules built for a different interpreter. sys.flags reports
    // both, as python -E -s would.
    config.parse_argv              = 1;
    config.isolated                = 0;
    config.use_environment         = 0;
    config.user_site_directory     = 0;
    config.safe_path               = 0;
    config.install_signal_handlers = 1;

    if (!configureRuntime(config, rt, err)) {
        PyConfig_Clear(&config);
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // The same clippy module the windowed mode gets, with no host ever bound
    // to it. Two things make that the right shape rather than a lie:
    //
    //   * Everything needing the host already checks for it and raises
    //     "host not bound", so `clippy.show()` under --python says what is
    //     wrong instead of AttributeError.
    //   * clippy.pref_path() needs no host at all -- it is SDL_GetPrefPath,
    //     a filesystem call -- and it is the one thing a client program run
    //     under this interpreter genuinely needs, because finding the app's
    //     own directory any other way means hardcoding a platform path.
    //
    // It also stops `import clippy` resolving to scripts/clippy.py when the
    // scripts directory is on sys.path: a registered builtin is found by
    // BuiltinImporter, which runs before any path entry.
    if (!PyClippy::registerModule(err)) {
        PyConfig_Clear(&config);
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    // Nothing will ever be bound to it here, and saying so is what lets the
    // module answer "--python has no window, use --script" rather than
    // guessing at a startup race.
    PyClippy::setInterpreterMode();

    // argv[0] stays: CPython treats it as the program and starts parsing at
    // argv[1]. Everything the pet's own parser would have seen is gone.
    std::vector<char*> py_argv;
    py_argv.push_back(argv[0]);
    for (int i = start; i < argc; ++i) py_argv.push_back(argv[i]);

    PyStatus status = PyConfig_SetBytesArgv(&config, (Py_ssize_t)py_argv.size(), py_argv.data());
    if (PyStatus_Exception(status)) {
        PyConfig_Clear(&config);
        std::fprintf(stderr, "could not hand argv to the interpreter\n");
        return 1;
    }

    if (!initializeFromConfig(config, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    if (!addShippedPip(rt, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        Py_FinalizeEx();
        return 1;
    }

    return Py_RunMain();
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

    // Before anything else, because nothing else applies: no window, no
    // tray, no options of ours.
    if (const int start = pythonArgvStart(argc, argv)) {
        return runInterpreter(argc, argv, start);
    }

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

    // "Configure..." is a question for the script, not an action of the host's:
    // the host has no idea what this pet's settings are. A script with no
    // 'configure' hook gets a menu entry that says so once rather than one that
    // silently does nothing.
    app.tray.on_configure = [&app] {
        if (Settings::open()) return;          // already up; not stacked
        if (!PyClippy::fireConfigure()) {
            SDL_Log("[tray] Configure...: this script registered no "
                    "clippy.on('configure') handler, so there is nothing to "
                    "configure. scripts/clippy.py and scripts/assistant.py "
                    "both do.");
        }
    };

    // The banner's Show button: the preview becomes what it looks like.
    Settings::setOnPreviewShow([&app] { app.setVisible(true); });

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

    // --- microphone -------------------------------------------------------
    // Asked once, reported, and never fatal: a machine with no audio runs the
    // pet perfectly well, and only clippy.mic.start() has grounds to complain.
    // Asking here rather than at the first start() is what lets --capabilities
    // answer "is there a microphone" without opening one.
    std::string mic_err;
    const bool has_mic = !Mic::devices(mic_err).empty();
    app.window.caps_mutable().microphone = has_mic;
    if (!has_mic) {
        app.window.caps_mutable().notes.push_back(
            mic_err.empty() ? "No recording device; clippy.mic will refuse to start."
                            : "Microphone unavailable: " + mic_err);
    }

    if (opts.print_caps_only) {
        std::printf("%s", app.window.caps().report().c_str());
        app.tray.destroy();
        app.window.destroy();
        SDL_Quit();
        return 0;
    }

    SDL_Log("%s", app.window.caps().report().c_str());

    // --- python -----------------------------------------------------------
    if (!PyClippy::registerModule(err) || !startPython(err)) {
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
    //
    // The loop runs without the GIL. Py_InitializeFromConfig leaves it held by
    // this thread, and holding it here means a Python thread only ever gets
    // scheduled during the frame hook -- which is most of a frame spent in
    // SDL_Delay with every other thread stopped. A script that streams audio to
    // a transcriber or waits on an agent needs threads that actually run, so the
    // host gives the GIL up for the whole loop and takes it back only to enter
    // Python. Every such entry point does that for itself: PyClippy's fire*
    // helpers, and the animation completion callbacks in PyDraw.cpp.
    PyThreadState* loop_gil = PyEval_SaveThread();

    Uint64 previous_ns = SDL_GetTicksNS();

    while (app.running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            // Offered to the dialog first, and swallowed if it was the
            // dialog's: a click that landed on a settings window must not also
            // reach the pet as "the pet was clicked".
            if (Settings::handleEvent(e)) continue;

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
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                // The window swallows clicks over its whole area, transparent
                // corners included, so this is "the pet was clicked" and needs
                // no hit test. SDL counts the clicks, which is why there is no
                // double-click timer here.
                PyClippy::fireClick("click", e.button.x, e.button.y,
                                    e.button.button, e.button.clicks);
                if (e.button.clicks == 2) {
                    PyClippy::fireClick("double_click", e.button.x, e.button.y,
                                        e.button.button, -1);
                }
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
        Settings::render();
        SDL_Delay(16);   // ~60fps ceiling; the pet is idle most of the time
    }

    // Before the quit hook and well before Py_FinalizeEx: the spec holds
    // references to four Python callables, and releasing one after the
    // interpreter has gone is a use-after-free.
    Settings::close();

    PyClippy::fire("quit");

    // The device goes before the interpreter does: a script's audio thread is
    // still alive here, and it reads through this stream.
    Mic::stop();

    PyEval_RestoreThread(loop_gil);

    // Drop every animation before the interpreter goes: a completion callback
    // holds a Python reference, and releasing one after Py_FinalizeEx is a
    // use-after-free.
    AnimationManager::instance().clear();
    Stage::instance().clear();

    Py_FinalizeEx();
    Mic::quit();
    app.tray.destroy();
    app.window.destroy();
    SDL_Quit();
    return 0;
}
