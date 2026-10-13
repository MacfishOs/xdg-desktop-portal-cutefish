#include "filechooserportal.h"
#include "filechoosertypes.h"
#include "requesterwatch.h"
#include "requestrouter.h"

#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

#include <limits>

static const QString chooserService = QStringLiteral("com.cutefish.FileChooser");
static const QString chooserPath = QStringLiteral("/com/cutefish/FileChooser");

static QJsonObject filterJson(const FileFilter &filter)
{
    QJsonArray rules;
    for (const auto &rule : filter.rules)
        rules.append(QJsonObject {{"type", int(rule.type)}, {"value", rule.value}});
    return {{"name", filter.name}, {"rules", rules}};
}

static QString localPath(const QVariant &value)
{
    QByteArray bytes = qdbus_cast<QByteArray>(value);
    if (bytes.endsWith('\0'))
        bytes.chop(1);
    return QFile::decodeName(bytes);
}

class FileChooserRequest : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")

public:
    FileChooserRequest(const QDBusConnection &bus, const QDBusMessage &message, const QString &path,
                       QObject *parent)
        : QObject(parent)
        , bus(bus)
        , message(message)
        , path(path)
    {
        watchRequester(this, bus, message.service(), [this] {
            finish(1);
        });
    }

    void start(const QByteArray &payload)
    {
        // The session's file manager daemon shows the dialog; the session
        // restarts it when it dies, so nothing else is started for it.
        QDBusMessage call = QDBusMessage::createMethodCall(chooserService, chooserPath,
                                                           chooserService, QStringLiteral("Open"));
        call << path << QString::fromUtf8(payload);
        call.setAutoStartService(false);
        // The dialog stays open as long as the user leaves it.
        auto *watcher = new QDBusPendingCallWatcher(
                bus.asyncCall(call, std::numeric_limits<int>::max()), this);
        open = true;
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
            watcher->deleteLater();
            open = false;
            const QDBusPendingReply<QString> reply = *watcher;
            if (reply.isError()) {
                qWarning() << "File Manager chooser failed:" << reply.error().message();
                finish(2);
                return;
            }
            readResult(reply.value().toUtf8());
        });
    }

    void finish(uint response, const QVariantMap &results = {})
    {
        if (finished)
            return;
        finished = true;
        if (open) {
            QDBusMessage close = QDBusMessage::createMethodCall(
                    chooserService, chooserPath, chooserService, QStringLiteral("Close"));
            close << path;
            close.setAutoStartService(false);
            bus.send(close);
        }
        RequestRouter::remove(path);
        bus.send(message.createReply({response, results}));
        deleteLater();
    }

public slots:

    void Close(const QDBusMessage &call)
    {
        if (call.service() != message.service()) {
            call.setDelayedReply(true);
            bus.send(call.createErrorReply("org.freedesktop.DBus.Error.AccessDenied",
                                           "Only the requesting service may close this dialog"));
            return;
        }
        finish(1);
    }

private:
    void readResult(const QByteArray &result)
    {
        if (finished)
            return;
        const auto document = QJsonDocument::fromJson(result);
        if (result.size() > 1024 * 1024 || !document.isObject()) {
            finish(2);
            return;
        }
        const auto reply = document.object();
        const int response = reply.value("response").toInt(2);
        if (response != 0) {
            finish(response == 1 ? 1 : 2);
            return;
        }
        QStringList uris;
        for (const auto &entry : reply.value("uris").toArray()) {
            const QUrl url(entry.toString());
            if (!url.isLocalFile() || !url.host().isEmpty() || !url.path().startsWith('/')) {
                finish(2);
                return;
            }
            uris.append(url.toString(QUrl::FullyEncoded));
        }
        if (uris.isEmpty() || (single && uris.size() != 1)) {
            finish(2);
            return;
        }
        QVariantMap results {{"uris", uris}};
        const int filterIndex = reply.value("filter_index").toInt(-1);
        if (filterIndex >= 0 && filterIndex < filters.size())
            results.insert("current_filter", QVariant::fromValue(filters.at(filterIndex)));
        QList<ChoiceOption> selected;
        const auto values = reply.value("choices").toObject();
        for (const auto &choice : choices) {
            const QString value = values.value(choice.id).toString();
            bool valid = choice.options.isEmpty() && (value == "true" || value == "false");
            for (const auto &option : choice.options)
                valid |= option.id == value;
            if (!valid) {
                finish(2);
                return;
            }
            selected.append({choice.id, value});
        }
        if (!choices.isEmpty())
            results.insert("choices", QVariant::fromValue(selected));
        finish(0, results);
    }

public:
    QDBusConnection bus;
    QDBusMessage message;
    QString path;
    QList<FileFilter> filters;
    QList<FileChoice> choices;
    bool single = true;
    bool open = false;
    bool finished = false;
};

FileChooserPortal::FileChooserPortal(QObject *parent, const QDBusConnection &bus)
    : QDBusAbstractAdaptor(parent)
    , m_bus(bus)
{
    qDBusRegisterMetaType<FilterRule>();
    qDBusRegisterMetaType<QList<FilterRule>>();
    qDBusRegisterMetaType<FileFilter>();
    qDBusRegisterMetaType<QList<FileFilter>>();
    qDBusRegisterMetaType<ChoiceOption>();
    qDBusRegisterMetaType<QList<ChoiceOption>>();
    qDBusRegisterMetaType<FileChoice>();
    qDBusRegisterMetaType<QList<FileChoice>>();
}

uint FileChooserPortal::OpenFile(const QDBusObjectPath &handle, const QString &appId,
                                 const QString &parentWindow, const QString &title,
                                 const QVariantMap &options, const QDBusMessage &message,
                                 QVariantMap &results)
{
    Q_UNUSED(results)
    return choose("OpenFile", handle, appId, parentWindow, title, options, message);
}

uint FileChooserPortal::SaveFile(const QDBusObjectPath &handle, const QString &appId,
                                 const QString &parentWindow, const QString &title,
                                 const QVariantMap &options, const QDBusMessage &message,
                                 QVariantMap &results)
{
    Q_UNUSED(results)
    return choose("SaveFile", handle, appId, parentWindow, title, options, message);
}

uint FileChooserPortal::SaveFiles(const QDBusObjectPath &handle, const QString &appId,
                                  const QString &parentWindow, const QString &title,
                                  const QVariantMap &options, const QDBusMessage &message,
                                  QVariantMap &results)
{
    Q_UNUSED(results)
    return choose("SaveFiles", handle, appId, parentWindow, title, options, message);
}

uint FileChooserPortal::choose(const QString &mode, const QDBusObjectPath &handle,
                               const QString &appId, const QString &parentWindow,
                               const QString &title, const QVariantMap &options,
                               const QDBusMessage &message)
{
    auto *request = new FileChooserRequest(m_bus, message, handle.path(), this);
    if (!RequestRouter::add(handle.path(), request)) {
        delete request;
        return 2;
    }
    message.setDelayedReply(true);
    const auto preferredFilter = qdbus_cast<FileFilter>(options.value("current_filter"));
    request->filters = qdbus_cast<QList<FileFilter>>(options.value("filters"));
    if (request->filters.isEmpty() && options.contains("current_filter"))
        request->filters.append(preferredFilter);
    request->choices = qdbus_cast<QList<FileChoice>>(options.value("choices"));
    request->single =
            mode == "SaveFile" || (mode == "OpenFile" && !options.value("multiple").toBool());
    QJsonArray filters;
    int currentFilter = 0;
    const auto preferred = filterJson(preferredFilter);
    for (const auto &filter : request->filters) {
        const auto json = filterJson(filter);
        if (json == preferred)
            currentFilter = filters.size();
        filters.append(json);
    }
    QJsonArray choices;
    for (const auto &choice : request->choices) {
        QJsonArray values;
        for (const auto &option : choice.options)
            values.append(QJsonObject {{"id", option.id}, {"label", option.label}});
        choices.append(QJsonObject {{"id", choice.id},
                                    {"label", choice.label},
                                    {"options", values},
                                    {"selected", choice.selected}});
    }
    QJsonArray files;
    for (const auto &name : qdbus_cast<QList<QByteArray>>(options.value("files")))
        files.append(localPath(name));
    const QJsonObject payload {{"mode", mode},
                               {"title", title},
                               {"app_id", appId},
                               {"parent_window", parentWindow},
                               {"modal", options.value("modal", true).toBool()},
                               {"multiple", options.value("multiple", false).toBool()},
                               {"directory", options.value("directory", false).toBool()},
                               {"accept_label", options.value("accept_label").toString()},
                               {"current_folder", localPath(options.value("current_folder"))},
                               {"current_file", localPath(options.value("current_file"))},
                               {"current_name", options.value("current_name").toString()},
                               {"filters", filters},
                               {"filter_index", currentFilter},
                               {"choices", choices},
                               {"files", files}};
    request->start(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    return 0;
}

#include "filechooserportal.moc"
