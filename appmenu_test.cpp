// appmenu_test -- minimal *visual* reproducer for the Krita 6 / Qt 6
// "Plasma global menu is empty and the menubar is invisible" bug.
//
// The only thing that matters is: a throw-away QGuiApplication is created
// *before* the real QApplication (this is what KisOpenGLModeProber does).
// On Qt 6 that leaves the process' session-bus connection with delivery
// permanently suspended, so the exported menu object never answers and the
// Plasma applet shows nothing. Qt 5 is not affected.
//
// What to look at (no console output on purpose):
//   * the Plasma "Application Menu" widget in the panel
//   * whether the window itself shows a menu bar
//
// Build: see build.sh (produces appmenu_test-qt6 and appmenu_test-qt5)

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
    // like KisOpenGLModeProber: it always create()s a QWindow for the GL probe
    bool g_probeCreatesWindow = true;
    // --block-dbus: poison DBUS_SESSION_BUS_ADDRESS while the probe app is alive
    bool g_blockDbus = false;
    // --reset-bus: drop the cached session-bus connection after the probes
    bool g_resetBus = false;
    // --block-a11y: keep the at-spi bridge off the session bus inside the probe app.
    // Setting AT_SPI_BUS_ADDRESS non-empty makes Qt's accessibility bridge use that
    // address and never call QDBusConnection::sessionBus() at all.
    bool g_blockA11y = false;
    // --reset-a11y: drop the cached "a11y" connection after the probes
    bool g_resetA11y = false;
    // --verbose: phase markers on stderr (for QDBUS_DEBUG style investigation)
    bool g_verbose = false;

    void verbose(const char *fmt, ...) {
        if (!g_verbose) return;
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fflush(stderr);
    }

    // Minimal RAII env setter (same idea as KisOpenGLModeProber's EnvironmentSetter)
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

    // One iteration of KisOpenGLModeProber::probeFormat(): a complete, throw-away
    // QGuiApplication that is destroyed again before the real application exists.
    void throwAwayGuiApplication(const bool workaround, const bool portalGuard) {
        QByteArray name("appmenu_test");
        char *argv = name.data();

        const bool applyWorkaround = workaround
                                     && qEnvironmentVariableIsSet("KDE_FULL_SESSION")
                                     && !qEnvironmentVariableIsSet("APPIMAGE");

        // Krita's prober sets QT_NO_XDG_DESKTOP_PORTAL=1 around the probe app;
        // without it the probe app opens the session bus through the portal even on
        // Qt 5 and the process ends up with a suspended connection there too.
        QScopedPointer<EnvironmentSetter> portal;
        if (portalGuard)
            portal.reset(new EnvironmentSetter("QT_NO_XDG_DESKTOP_PORTAL", "1"));

        // The at-spi bridge (src/gui/accessible/linux/dbusconnection.cpp) returns
        // early when AT_SPI_BUS_ADDRESS is set, so the probe app never calls
        // QDBusConnection::sessionBus() and cannot leave a suspended connection
        // behind. Restored when this scope ends.
        QScopedPointer<EnvironmentSetter> a11yGuard;
        if (g_blockA11y)
            a11yGuard.reset(new EnvironmentSetter("AT_SPI_BUS_ADDRESS",
                                                  "unix:path=/nonexistent-appmenu-a11y"));

        // --block-dbus: the blunt version. Note that Qt caches the *failed*
        // session-bus connection (QDBusConnectionManager::doConnectToStandardBus()
        // calls setConnection() unconditionally), so this kills the session bus for
        // the whole process instead of fixing anything.
        QScopedPointer<EnvironmentSetter> dbusGuard;
        if (g_blockDbus)
            dbusGuard.reset(new EnvironmentSetter("DBUS_SESSION_BUS_ADDRESS",
                                                  "unix:path=/nonexistent-appmenu-probe"));

        if (applyWorkaround)
            QGuiApplication::setDesktopSettingsAware(false);

        {
            int argc = 1;
            QGuiApplication probe(argc, &argv);

            if (g_probeCreatesWindow) {
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
            "  --verbose      phase markers on stderr (for QDBUS_DEBUG style investigation)\n"
            "  --wayland      force QT_QPA_PLATFORM=wayland (default: xcb, like krita)\n"
            "  --hidden       do not show the window (for headless diagnostics)\n"
            "  --help\n"
            "\n"
            "Watch the Plasma \"Application Menu\" widget and the window's own menu bar.\n"
            "plain/core: the panel shows File/Edit (no in-window menu bar is normal).\n"
            "probe/krita: BUG, the panel stays empty and there is no in-window menu bar.\n"
            "KDE_NO_GLOBAL_MENU=1 appmenu_test ... always shows the in-window menu bar.\n");
    }
} // namespace

int main(int argc, char **argv) {
    auto mode = QStringLiteral("plain");
    int probes = -1;
    bool workaround = true;
    bool forceWayland = false;
    bool hidden = false;
    bool portalGuard = true;

    // Parse cmdline args starts.
    for (int i = 1; i < argc; ++i) {
        if (const QString arg_str = QString::fromLocal8Bit(argv[i]);
            arg_str == QLatin1String("--help") || arg_str == QLatin1String("-h")) {
            printUsage();
            return 0;
        } else if (arg_str == QLatin1String("--probes") && i + 1 < argc) {
            probes = QString::fromLocal8Bit(argv[++i]).toInt();
        } else if (arg_str == QLatin1String("--no-workaround")) {
            workaround = false;
        } else if (arg_str == QLatin1String("--bare-probe")) {
            g_probeCreatesWindow = false;
        } else if (arg_str == QLatin1String("--wayland")) {
            forceWayland = true;
        } else if (arg_str == QLatin1String("--hidden")) {
            hidden = true;
        } else if (arg_str == QLatin1String("--no-portal-guard")) {
            portalGuard = false;
        } else if (arg_str == QLatin1String("--block-a11y")) {
            g_blockA11y = true;
        } else if (arg_str == QLatin1String("--reset-a11y")) {
            g_resetA11y = true;
        } else if (arg_str == QLatin1String("--block-dbus")) {
            g_blockDbus = true;
        } else if (arg_str == QLatin1String("--reset-bus")) {
            g_resetBus = true;
        } else if (arg_str == QLatin1String("--verbose")) {
            g_verbose = true;
        } else if (arg_str.startsWith(QLatin1String("--"))) {
            std::printf("unknown option: %s\n\n", qPrintable(arg_str));
            printUsage();
            return 2;
        } else {
            mode = arg_str;
        }
    }

    if (mode != QLatin1String("plain") && mode != QLatin1String("core")
        && mode != QLatin1String("probe") && mode != QLatin1String("krita")) {
        std::printf("unknown mode: %s\n\n", qPrintable(mode));
        printUsage();
        return 2;
    }
    // Parse cmdline args finished.

    if (probes < 0)
        probes = mode == QLatin1String("krita") ? 3 : 1;

    // kXcb: krita forces the xcb (XWayland) platform when nothing is requested
    if (forceWayland)
        qputenv("QT_QPA_PLATFORM", "wayland");
    else if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");

    verbose("[verbose] flags: workaround=%d portalGuard=%d blockA11y=%d resetA11y=%d "
            "blockDbus=%d resetBus=%d probeCreatesWindow=%d mode=%s\n",
            workaround, portalGuard, g_blockA11y, g_resetA11y, g_blockDbus, g_resetBus,
            g_probeCreatesWindow, qPrintable(mode));

    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);

    if (mode == QLatin1String("core") || mode == QLatin1String("krita")) {
        int argc2 = 1;
        QByteArray name("appmenu_test");
        char *argv2 = name.data();
        verbose("[verbose] throw-away QCoreApplication\n");
        QCoreApplication throwAway(argc2, &argv2);
    }

    if (mode == QLatin1String("probe") || mode == QLatin1String("krita")) {
        for (int i = 0; i < probes; ++i) {
            verbose("[verbose] probe %d/%d begin\n", i + 1, probes);
            throwAwayGuiApplication(workaround, portalGuard);
            verbose("[verbose] probe %d/%d end\n", i + 1, probes);
        }
    }

    if (g_resetA11y) {
        // The probe app's bridge connected to the dummy address, leaving a dead
        // "a11y" connection in the global cache. Unlike the session bus, that name
        // is not pinned in QDBusConnectionManager::defaultBuses[], so it is really
        // dropped and the real app can connect to the accessibility bus normally.
        verbose("[verbose] disconnectFromBus(a11y)\n");
        QDBusConnection::disconnectFromBus(QStringLiteral("a11y"));
    }

    if (g_resetBus) {
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
        .arg(QString::fromLatin1(qVersion()), mode));

    QMenuBar *menuBar = window.menuBar();
    menuBar->addMenu(QStringLiteral("File"))->addAction(QStringLiteral("Open"));
    menuBar->addMenu(QStringLiteral("Edit"))->addAction(QStringLiteral("Undo"));
    menuBar->addMenu(QStringLiteral("Help"))->addAction(QStringLiteral("About"));

    const bool buggyMode = mode == QLatin1String("probe") || mode == QLatin1String("krita");

    QString labelText;
    if (buggyMode && g_blockA11y) {
        labelText = QStringLiteral("<b>mode: %1 + --block-a11y</b><br><br>"
                    "WORKAROUND ACTIVE: the probe app was kept off the session bus,"
                    " so the bug should <b>not</b> happen.<br>"
                    "The Plasma Application Menu widget should show <b>File / Edit / Help</b>.")
                .arg(mode);
    } else if (buggyMode) {
        labelText = QStringLiteral("<b>mode: %1</b> (throw-away QGuiApplication first)<br><br>"
                    "Expected BUG: the Plasma Application Menu widget stays <b>empty</b>,"
                    " and this window shows <b>no menu bar</b>.<br>"
                    "Compare with <tt>appmenu_test plain</tt>: the panel then shows File/Edit.<br>"
                    "Add <tt>--block-a11y</tt> to see the workaround working.")
                .arg(mode);
    } else {
        labelText = QStringLiteral("<b>mode: %1</b> (control)<br><br>"
                    "The Plasma Application Menu widget should show <b>File / Edit / Help</b>"
                    " while this window is focused.<br>"
                    "No menu bar inside the window is normal: Qt gives it to the global menu.")
                .arg(mode);
    }
    auto *label = new QLabel(labelText);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setMargin(12);
    window.setCentralWidget(label);
    window.resize(620, 220);
    if (!hidden) {
        window.show();
    } else {
        auto _ = window.winId(); // force the native window, keep it unmapped
    }
    verbose("[verbose] winId=0x%lx\n", static_cast<unsigned long>(window.winId()));
    verbose("[verbose] isNativeMenuBar=%d /MenuBar/1 registered=%d\n",
            menuBar->isNativeMenuBar(),
            QDBusConnection::sessionBus().objectRegisteredAt(QStringLiteral("/MenuBar/1")) != nullptr);

    return QApplication::exec();
}
