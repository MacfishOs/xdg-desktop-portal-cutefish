#include "settingsportal.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusError>
#include <QDebug>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("xdg-desktop-portal-cutefish"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Cutefish Settings portal backend"));
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
    if (!bus.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), &settings,
                            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals
                                    | QDBusConnection::ExportAllProperties)) {
        qCritical() << "Cannot export the Settings portal:" << bus.lastError();
        return 1;
    }

    if (!bus.registerService(QStringLiteral("org.freedesktop.impl.portal.desktop.cutefish"))) {
        qCritical() << "Cannot own the Cutefish portal service:" << bus.lastError();
        return 1;
    }

    return app.exec();
}
