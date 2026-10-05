#include "filechooserportal.h"
#include "inhibitportal.h"
#include "screencastportal.h"
#include "settingsportal.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusError>
#include <QDebug>
#include <QSocketNotifier>

#include <csignal>

#include <sys/socket.h>
#include <unistd.h>

static int g_signalFds[2] = {-1, -1};

static void onSignal(int)
{
    const char byte = 0;
    (void)!write(g_signalFds[0], &byte, 1);
}

// A termination signal ends the event loop instead of the process, so the
// portals and the leak checker of a sanitizer build get their shutdown.
static void quitOnSignal(QCoreApplication *app)
{
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, g_signalFds) != 0)
        return;
    auto *notifier = new QSocketNotifier(g_signalFds[1], QSocketNotifier::Read, app);
    QObject::connect(notifier, &QSocketNotifier::activated, app, [app] {
        char byte;
        (void)!read(g_signalFds[1], &byte, 1);
        app->quit();
    });
    struct sigaction action = {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    quitOnSignal(&app);
    QCoreApplication::setApplicationName(QStringLiteral("xdg-desktop-portal-cutefish"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Cutefish desktop portal backend"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        qCritical() << "Cannot connect to the session bus:" << bus.lastError();
        return 1;
    }

    bus.connect(QString(), QString(), QStringLiteral("org.freedesktop.DBus.Local"),
                QStringLiteral("Disconnected"), &app, SLOT(quit()));

    SettingsPortal settings(bus);
    new InhibitPortal(&settings, bus);
    new FileChooserPortal(&settings, bus);
    new ScreenCastPortal(&settings, bus);
    if (!bus.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &settings,
                            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals
                                    | QDBusConnection::ExportAllProperties
                                    | QDBusConnection::ExportAdaptors)) {
        qCritical() << "Cannot export the Settings portal:" << bus.lastError();
        return 1;
    }

    if (!bus.registerService(QStringLiteral("org.freedesktop.impl.portal.desktop.cutefish"))) {
        qCritical() << "Cannot own the Cutefish portal service:" << bus.lastError();
        return 1;
    }

    return app.exec();
}
