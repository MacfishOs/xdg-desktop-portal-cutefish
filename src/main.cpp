#include "filechooserportal.h"
#include "inhibitportal.h"
#include "screencastportal.h"
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
    parser.setApplicationDescription(QStringLiteral("Cutefish desktop portal backend"));
    parser.addOption({QStringLiteral("file-chooser-helper"),
                      QStringLiteral("File Manager chooser executable"), QStringLiteral("path"),
                      QStringLiteral(FILE_CHOOSER_HELPER)});
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
    new FileChooserPortal(&settings, bus, parser.value(QStringLiteral("file-chooser-helper")));
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
