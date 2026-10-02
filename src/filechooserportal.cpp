#include "filechooserportal.h"
#include "filechoosertypes.h"
#include "requesterwatch.h"

#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QUrl>

namespace {
QJsonObject filterJson(const FileFilter &filter)
{
    QJsonArray rules;
    for (const auto &rule : filter.rules)
        rules.append(QJsonObject {{"type", int(rule.type)}, {"value", rule.value}});
    return {{"name", filter.name}, {"rules", rules}};
}

QString localPath(const QVariant &value)
{
    QByteArray bytes = qdbus_cast<QByteArray>(value);
    if (bytes.endsWith('\0'))
        bytes.chop(1);
    return QFile::decodeName(bytes);
}
} // namespace

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
        process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        connect(&process, &QProcess::errorOccurred, this, [this] {
            if (finished)
                return;
            qWarning() << "File Manager chooser failed:" << process.errorString();
            finish(2);
        });
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
            output += process.readAllStandardOutput();
            if (output.size() > 1024 * 1024)
                finish(2);
        });
        connect(&process, &QProcess::finished, this, &FileChooserRequest::readReply);
    }

    ~FileChooserRequest() override
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(1000);
        }
    }

    void finish(uint response, const QVariantMap &results = {})
    {
        if (finished)
            return;
        finished = true;
        bus.unregisterObject(path);
        bus.send(message.createReply({response, results}));
        if (process.state() != QProcess::NotRunning)
            process.kill();
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
    void readReply(int code, QProcess::ExitStatus status)
    {
        if (finished)
            return;
        output += process.readAllStandardOutput();
        const auto document = QJsonDocument::fromJson(output);
        if (code != 0 || status != QProcess::NormalExit || !document.isObject()) {
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
    QProcess process;
    QByteArray output;
    QList<FileFilter> filters;
    QList<FileChoice> choices;
    bool single = true;
    bool finished = false;
};

FileChooserPortal::FileChooserPortal(QObject *parent, const QDBusConnection &bus,
                                     const QString &helper)
    : QDBusAbstractAdaptor(parent)
    , m_bus(bus)
    , m_helper(helper)
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
    if (!m_bus.registerObject(handle.path(), request, QDBusConnection::ExportAllSlots)) {
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
    connect(&request->process, &QProcess::started, request, [request, payload] {
        request->process.write(QJsonDocument(payload).toJson(QJsonDocument::Compact));
        request->process.closeWriteChannel();
    });
    request->process.start(m_helper, {});
    return 0;
}

#include "filechooserportal.moc"
