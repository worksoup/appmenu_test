// appmenu_test -- minimal *visual* reproducer for the Krita 6 / Qt 6 bug
// "the Plasma global menu is empty and the window has no menu bar".
//
// The only thing that matters: a throw-away QGuiApplication is created *before*
// the real QApplication (this is what KisOpenGLModeProber::probeFormat() does).
// On Qt 6 the probe's QWindow is destroyed before any event loop runs, so
// QWindow::~QWindow() asks the QPA plugin for its accessibility implementation;
// the resulting at-spi bridge creates the process-wide session-bus connection
// while that throw-away application is still qApp. The connection therefore
// keeps dispatchEnabled == false forever, the exported menu object never
// answers, and the Plasma applet shows nothing. Qt 5 is not affected.
//
// What to look at (nothing is printed unless --verbose is given):
//   * the Plasma "Application Menu" widget in the panel
//   * whether the window itself shows a menu bar
//
// Build: see CMakeLists.txt (targets appmenu_test-qt6 / appmenu_test-qt5).

#include <QApplication>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QOpenGLContext>
#include <QScopedPointer>
#include <QString>
#include <QSurfaceFormat>
#include <QWindow>

#include <cstdarg>
#include <cstdio>

namespace {

// ------------------------------------------------------------------- options

struct Options {
    QString mode = QStringLiteral("plain");
    int probes = -1;                // -1: use the default of the mode
    bool workaround = true;         // QGuiApplication::setDesktopSettingsAware(false)
    bool portalGuard = true;        // QT_NO_XDG_DESKTOP_PORTAL=1 in the probe app
    bool probeCreatesWindow = true; // the probe app creates a QWindow + GL context
    bool blockA11y = false;         // AT_SPI_BUS_ADDRESS set in the probe app
    bool resetA11y = false;         // disconnectFromBus("a11y") after the probes
    bool blockDbus = false;         // DBUS_SESSION_BUS_ADDRESS poisoned in the probe
    bool resetBus = false;          // disconnectFromBus("qt_default_session_bus")
    bool verbose = false;           // phase markers and state on stderr
    bool wayland = false;           // QT_QPA_PLATFORM=wayland instead of xcb
    bool hidden = false;            // keep the window unmapped
};

enum class ParseResult { Ok, HelpPrinted, Error };

bool g_verbose = false;             // only used by verbose()

void verbose(const char *fmt, ...) {
    if (!g_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

// ------------------------------------------------------------------- helpers

// A throw-away application still needs a sane argv: argc == 1 and NULL-terminated.
struct FakeArgv {
    QByteArray name = QByteArrayLiteral("appmenu_test");
    char *argv[2] = { name.data(), nullptr };
    int argc = 1;
};

// RAII env setter (same idea as KisOpenGLModeProber's EnvironmentSetter).
// An empty value unsets the variable; the previous state is restored on exit.
struct EnvironmentSetter {
    EnvironmentSetter(const char *env, const QByteArray &value)
        : m_env(env), m_had(qEnvironmentVariableIsSet(env)), m_old(qgetenv(env)) {
        if (value.isEmpty())
            qunsetenv(env);
        else
            qputenv(env, value);
    }

    ~EnvironmentSetter() {
        if (m_had)
            qputenv(m_env, m_old);
        else
            qunsetenv(m_env);
    }

    const char *m_env;
    bool m_had;
    QByteArray m_old;
};

// --------------------------------------------------------------------- probe

// One iteration of KisOpenGLModeProber::probeFormat(): a complete, throw-away
// QGuiApplication that is destroyed again before the real application exists.
void throwAwayGuiApplication(const Options &opt) {
    const bool applyWorkaround = opt.workaround
                                 && qEnvironmentVariableIsSet("KDE_FULL_SESSION")
                                 && !qEnvironmentVariableIsSet("APPIMAGE");

    // Krita's prober sets QT_NO_XDG_DESKTOP_PORTAL=1 around the probe app; without
    // it the probe opens the session bus through the portal even on Qt 5, and the
    // process ends up with a suspended connection there too.
    QScopedPointer<EnvironmentSetter> portalGuard;
    if (opt.portalGuard)
        portalGuard.reset(new EnvironmentSetter("QT_NO_XDG_DESKTOP_PORTAL", "1"));

    // The at-spi bridge (src/gui/accessible/linux/dbusconnection.cpp) returns early
    // when AT_SPI_BUS_ADDRESS is set, so the probe app never calls
    // QDBusConnection::sessionBus() and cannot leave a suspended connection behind.
    QScopedPointer<EnvironmentSetter> a11yGuard;
    if (opt.blockA11y)
        a11yGuard.reset(new EnvironmentSetter("AT_SPI_BUS_ADDRESS",
                                              "unix:path=/nonexistent-appmenu-a11y"));

    // The blunt variant: Qt caches the *failed* session-bus connection
    // (doConnectToStandardBus() calls setConnection() unconditionally), so this
    // kills the session bus for the whole process instead of fixing anything.
    QScopedPointer<EnvironmentSetter> dbusGuard;
    if (opt.blockDbus)
        dbusGuard.reset(new EnvironmentSetter("DBUS_SESSION_BUS_ADDRESS",
                                              "unix:path=/nonexistent-appmenu-probe"));

    if (applyWorkaround)
        QGuiApplication::setDesktopSettingsAware(false);

    {
        FakeArgv args;
        QGuiApplication probe(args.argc, args.argv);

        if (opt.probeCreatesWindow) {
            const QSurfaceFormat format;
            QWindow surface;
            surface.setFormat(format);
            surface.setSurfaceType(QSurface::OpenGLSurface);
            surface.create();

            QOpenGLContext context;
            context.setFormat(format);
            if (context.create())
                context.makeCurrent(&surface);
        }
    }

    if (applyWorkaround)
        QGuiApplication::setDesktopSettingsAware(true);
}

void throwAwayCoreApplication() {
    FakeArgv args;
    verbose("[verbose] throw-away QCoreApplication\n");
    QCoreApplication throwAway(args.argc, args.argv);
}

// --------------------------------------------------------------------- usage

void printUsage() {
    std::printf(
        "usage: appmenu_test [MODE] [options]\n"
        "\n"
        "MODE (default: plain)\n"
        "  plain        real QApplication only                        (control)\n"
        "  core         + throw-away QCoreApplication                 (harmless)\n"
        "  probe        + throw-away QGuiApplication                  (reproduces the bug)\n"
        "  krita        + throw-away QCoreApplication and N probe QGuiApplications\n"
        "\n"
        "options\n"
        "  --probes N     number of throw-away QGuiApplications (probe: 1, krita: 3)\n"
        "  --no-workaround  do not call QGuiApplication::setDesktopSettingsAware(false)\n"
        "  --no-portal-guard  do not set QT_NO_XDG_DESKTOP_PORTAL=1 in the probe app\n"
        "  --bare-probe   probe app only constructs QGuiApplication (no QWindow/GL;\n"
        "                 this alone does NOT reproduce the bug)\n"
        "  --block-a11y   keep the at-spi bridge off the session bus in the probe app\n"
        "                 (WORKAROUND: with this the bug does not happen at all)\n"
        "  --reset-a11y   disconnectFromBus(\"a11y\") after the probes, so the real app\n"
        "                 is not left with the probe app's dead a11y connection\n"
        "  --block-dbus   point DBUS_SESSION_BUS_ADDRESS at a dead address in the probe\n"
        "                 (does NOT work: Qt caches the failed session-bus connection)\n"
        "  --reset-bus    disconnectFromBus(qt_default_session_bus) after the probes\n"
        "                 (does NOT work: that pointer is pinned in defaultBuses[])\n"
        "  --verbose      phase markers on stderr, plus winId, isNativeMenuBar and the\n"
        "                 /MenuBar/1 registration state (headless diagnostics)\n"
        "  --wayland      force QT_QPA_PLATFORM=wayland (default: xcb, like krita)\n"
        "  --hidden       do not show the window (for headless diagnostics)\n"
        "  --help         print this text and exit\n"
        "\n"
        "Watch the Plasma \"Application Menu\" widget and the window's own menu bar.\n"
        "plain/core: the panel shows File/Edit (no in-window menu bar is normal).\n"
        "probe/krita: BUG, the panel stays empty and there is no in-window menu bar.\n"
        "KDE_NO_GLOBAL_MENU=1 appmenu_test ... always shows the in-window menu bar.\n");
}

// ------------------------------------------------------------------- parsing

ParseResult parseArguments(int argc, char **argv, Options &opt) {
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);

        if (arg == QLatin1String("--help") || arg == QLatin1String("-h")) {
            printUsage();
            return ParseResult::HelpPrinted;
        } else if (arg == QLatin1String("--probes")) {
            if (i + 1 >= argc) {
                std::printf("--probes requires a value\n\n");
                printUsage();
                return ParseResult::Error;
            }
            bool ok = false;
            opt.probes = QString::fromLocal8Bit(argv[++i]).toInt(&ok);
            if (!ok || opt.probes < 1) {
                std::printf("invalid --probes value (expected a positive integer)\n\n");
                printUsage();
                return ParseResult::Error;
            }
        } else if (arg == QLatin1String("--no-workaround")) {
            opt.workaround = false;
        } else if (arg == QLatin1String("--no-portal-guard")) {
            opt.portalGuard = false;
        } else if (arg == QLatin1String("--bare-probe")) {
            opt.probeCreatesWindow = false;
        } else if (arg == QLatin1String("--block-a11y")) {
            opt.blockA11y = true;
        } else if (arg == QLatin1String("--reset-a11y")) {
            opt.resetA11y = true;
        } else if (arg == QLatin1String("--block-dbus")) {
            opt.blockDbus = true;
        } else if (arg == QLatin1String("--reset-bus")) {
            opt.resetBus = true;
        } else if (arg == QLatin1String("--verbose")) {
            opt.verbose = true;
        } else if (arg == QLatin1String("--wayland")) {
            opt.wayland = true;
        } else if (arg == QLatin1String("--hidden")) {
            opt.hidden = true;
        } else if (arg.startsWith(QLatin1String("--"))) {
            std::printf("unknown option: %s\n\n", qPrintable(arg));
            printUsage();
            return ParseResult::Error;
        } else {
            opt.mode = arg;
        }
    }

    if (opt.mode != QLatin1String("plain") && opt.mode != QLatin1String("core")
        && opt.mode != QLatin1String("probe") && opt.mode != QLatin1String("krita")) {
        std::printf("unknown mode: %s\n\n", qPrintable(opt.mode));
        printUsage();
        return ParseResult::Error;
    }

    return ParseResult::Ok;
}

// --------------------------------------------------------------------- label

QString labelText(const Options &opt) {
    const bool buggyMode = opt.mode == QLatin1String("probe")
                           || opt.mode == QLatin1String("krita");

    if (buggyMode && opt.blockA11y) {
        return QStringLiteral("<b>mode: %1 + --block-a11y</b><br><br>"
                              "WORKAROUND ACTIVE: the probe app was kept off the session bus,"
                              " so the bug should <b>not</b> happen.<br>"
                              "The Plasma Application Menu widget should show <b>File / Edit / Help</b>.")
            .arg(opt.mode);
    }
    if (buggyMode) {
        return QStringLiteral("<b>mode: %1</b> (throw-away QGuiApplication first)<br><br>"
                              "Expected BUG: the Plasma Application Menu widget stays <b>empty</b>,"
                              " and this window shows <b>no menu bar</b>.<br>"
                              "Compare with <tt>appmenu_test plain</tt>: the panel then shows File/Edit.<br>"
                              "Add <tt>--block-a11y</tt> to see the workaround working.")
            .arg(opt.mode);
    }
    return QStringLiteral("<b>mode: %1</b> (control)<br><br>"
                          "The Plasma Application Menu widget should show <b>File / Edit / Help</b>"
                          " while this window is focused.<br>"
                          "No menu bar inside the window is normal: Qt gives it to the global menu.")
        .arg(opt.mode);
}

} // namespace

int main(int argc, char **argv) {
    Options opt;

    switch (parseArguments(argc, argv, opt)) {
    case ParseResult::HelpPrinted:
        return 0;
    case ParseResult::Error:
        return 2;
    case ParseResult::Ok:
        break;
    }

    if (opt.probes < 0)
        opt.probes = opt.mode == QLatin1String("krita") ? 3 : 1;

    g_verbose = opt.verbose;

    // Krita forces the xcb (XWayland) platform when nothing is requested.
    if (opt.wayland)
        qputenv("QT_QPA_PLATFORM", "wayland");
    else if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");

    verbose("[verbose] flags: workaround=%d portalGuard=%d blockA11y=%d resetA11y=%d "
            "blockDbus=%d resetBus=%d probeCreatesWindow=%d mode=%s\n",
            opt.workaround, opt.portalGuard, opt.blockA11y, opt.resetA11y, opt.blockDbus,
            opt.resetBus, opt.probeCreatesWindow, qPrintable(opt.mode));

    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);

    if (opt.mode == QLatin1String("core") || opt.mode == QLatin1String("krita"))
        throwAwayCoreApplication();

    if (opt.mode == QLatin1String("probe") || opt.mode == QLatin1String("krita")) {
        for (int i = 0; i < opt.probes; ++i) {
            verbose("[verbose] probe %d/%d begin\n", i + 1, opt.probes);
            throwAwayGuiApplication(opt);
            verbose("[verbose] probe %d/%d end\n", i + 1, opt.probes);
        }
    }

    if (opt.resetA11y) {
        // The probe app's bridge connected to the dummy address, leaving a dead
        // "a11y" connection in the global cache. Unlike the session bus, that name
        // is not pinned in QDBusConnectionManager::defaultBuses[], so it is really
        // dropped and the real app can connect to the accessibility bus normally.
        verbose("[verbose] disconnectFromBus(a11y)\n");
        QDBusConnection::disconnectFromBus(QStringLiteral("a11y"));
    }

    if (opt.resetBus) {
        // The session-bus connection object is cached in the global
        // QDBusConnectionManager. If it was created while a throw-away app was
        // alive it stays 'delivery suspended' forever, so drop it here and let
        // the real application create a fresh one.
        verbose("[verbose] disconnectFromBus(qt_default_session_bus)\n");
        QDBusConnection::disconnectFromBus(QStringLiteral("qt_default_session_bus"));
    }

    verbose("[verbose] constructing real QApplication\n");
    QApplication app(argc, argv);
    verbose("[verbose] real QApplication constructed\n");

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("appmenu_test [Qt %1] mode=%2")
        .arg(QString::fromLatin1(qVersion()), opt.mode));

    QMenuBar *menuBar = window.menuBar();
    menuBar->addMenu(QStringLiteral("File"))->addAction(QStringLiteral("Open"));
    menuBar->addMenu(QStringLiteral("Edit"))->addAction(QStringLiteral("Undo"));
    menuBar->addMenu(QStringLiteral("Help"))->addAction(QStringLiteral("About"));

    auto *label = new QLabel(labelText(opt));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setContentsMargins(12, 12, 12, 12);
    window.setCentralWidget(label);
    window.resize(620, 220);

    if (opt.hidden) {
        auto _ = window.winId(); // force the native window, keep it unmapped
    } else {
        window.show();
    }

    verbose("[verbose] winId=0x%lx\n", static_cast<unsigned long>(window.winId()));
    verbose("[verbose] isNativeMenuBar=%d /MenuBar/1 registered=%d\n",
            menuBar->isNativeMenuBar(),
            QDBusConnection::sessionBus().objectRegisteredAt(QStringLiteral("/MenuBar/1")) != nullptr);

    return QApplication::exec();
}
