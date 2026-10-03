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
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QOpenGLContext>
#include <QScopedPointer>
#include <QString>
#include <QSurfaceFormat>
#include <QWindow>

#include <cstdio>

namespace {

bool g_probeCreatesWindow = true;   // like KisOpenGLModeProber: it always create()s a QWindow

// Minimal RAII env setter (same idea as KisOpenGLModeProber's EnvironmentSetter)
struct EnvironmentSetter {
    EnvironmentSetter(const char *env, const QByteArray &value)
        : m_env(env), m_had(qEnvironmentVariableIsSet(env)), m_old(qgetenv(env))
    {
        if (value.isEmpty())
            qunsetenv(env);
        else
            qputenv(env, value);
    }
    ~EnvironmentSetter()
    {
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
void throwAwayGuiApplication(bool workaround, bool portalGuard)
{
    int argc = 1;
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

    if (applyWorkaround)
        QGuiApplication::setDesktopSettingsAware(false);

    {
        QGuiApplication probe(argc, &argv);

        if (g_probeCreatesWindow) {
            QSurfaceFormat format;
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

void printUsage()
{
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

int main(int argc, char **argv)
{
    QString mode = QStringLiteral("plain");
    int probes = -1;
    bool workaround = true;
    bool forceWayland = false;
    bool hidden = false;
    bool portalGuard = true;

    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--help") || a == QLatin1String("-h")) {
            printUsage();
            return 0;
        } else if (a == QLatin1String("--probes") && i + 1 < argc) {
            probes = QString::fromLocal8Bit(argv[++i]).toInt();
        } else if (a == QLatin1String("--no-workaround")) {
            workaround = false;
        } else if (a == QLatin1String("--bare-probe")) {
            g_probeCreatesWindow = false;
        } else if (a == QLatin1String("--wayland")) {
            forceWayland = true;
        } else if (a == QLatin1String("--hidden")) {
            hidden = true;
        } else if (a == QLatin1String("--no-portal-guard")) {
            portalGuard = false;
        } else if (a.startsWith(QLatin1String("--"))) {
            std::printf("unknown option: %s\n\n", qPrintable(a));
            printUsage();
            return 2;
        } else {
            mode = a;
        }
    }

    if (mode != QLatin1String("plain") && mode != QLatin1String("core")
        && mode != QLatin1String("probe") && mode != QLatin1String("krita")) {
        std::printf("unknown mode: %s\n\n", qPrintable(mode));
        printUsage();
        return 2;
    }

    if (probes < 0)
        probes = (mode == QLatin1String("krita")) ? 3 : 1;

    // kXcb: krita forces the xcb (XWayland) platform when nothing is requested
    if (forceWayland)
        qputenv("QT_QPA_PLATFORM", "wayland");
    else if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");

    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts, true);

    if (mode == QLatin1String("core") || mode == QLatin1String("krita")) {
        int argc2 = 1;
        QByteArray name("appmenu_test");
        char *argv2 = name.data();
        QCoreApplication throwAway(argc2, &argv2);
    }

    if (mode == QLatin1String("probe") || mode == QLatin1String("krita")) {
        for (int i = 0; i < probes; ++i)
            throwAwayGuiApplication(workaround, portalGuard);
    }

    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("appmenu_test [Qt %1] mode=%2")
                          .arg(QString::fromLatin1(qVersion()), mode));

    QMenuBar *menuBar = window.menuBar();
    menuBar->addMenu(QStringLiteral("File"))->addAction(QStringLiteral("Open"));
    menuBar->addMenu(QStringLiteral("Edit"))->addAction(QStringLiteral("Undo"));
    menuBar->addMenu(QStringLiteral("Help"))->addAction(QStringLiteral("About"));

    const bool buggyMode = (mode == QLatin1String("probe") || mode == QLatin1String("krita"));
    QLabel *label = new QLabel(buggyMode
        ? QStringLiteral("<b>mode: %1</b> (throw-away QGuiApplication first)<br><br>"
                         "Expected BUG: the Plasma Application Menu widget stays <b>empty</b>,"
                         " and this window shows <b>no menu bar</b>.<br>"
                         "Compare with <tt>appmenu_test plain</tt>: the panel then shows File/Edit.")
              .arg(mode)
        : QStringLiteral("<b>mode: %1</b> (control)<br><br>"
                         "The Plasma Application Menu widget should show <b>File / Edit / Help</b>"
                         " while this window is focused.<br>"
                         "No menu bar inside the window is normal: Qt gives it to the global menu.")
              .arg(mode));
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setMargin(12);
    window.setCentralWidget(label);
    window.resize(620, 220);
    if (!hidden) {
        window.show();
    } else {
        window.winId();          // force the native window, keep it unmapped
    }

    return app.exec();
}
