#pragma once

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QList>
#include <QString>

struct FilterRule {
    uint type;
    QString value;
};

struct FileFilter {
    QString name;
    QList<FilterRule> rules;
};

struct ChoiceOption {
    QString id;
    QString label;
};

struct FileChoice {
    QString id;
    QString label;
    QList<ChoiceOption> options;
    QString selected;
};

Q_DECLARE_METATYPE(FilterRule)
Q_DECLARE_METATYPE(FileFilter)
Q_DECLARE_METATYPE(ChoiceOption)
Q_DECLARE_METATYPE(FileChoice)

inline QDBusArgument &operator<<(QDBusArgument &arg, const FilterRule &value)
{
    arg.beginStructure();
    arg << value.type << value.value;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, FilterRule &value)
{
    arg.beginStructure();
    arg >> value.type >> value.value;
    arg.endStructure();
    return arg;
}

inline QDBusArgument &operator<<(QDBusArgument &arg, const FileFilter &value)
{
    arg.beginStructure();
    arg << value.name << value.rules;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, FileFilter &value)
{
    arg.beginStructure();
    arg >> value.name >> value.rules;
    arg.endStructure();
    return arg;
}

inline QDBusArgument &operator<<(QDBusArgument &arg, const ChoiceOption &value)
{
    arg.beginStructure();
    arg << value.id << value.label;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, ChoiceOption &value)
{
    arg.beginStructure();
    arg >> value.id >> value.label;
    arg.endStructure();
    return arg;
}

inline QDBusArgument &operator<<(QDBusArgument &arg, const FileChoice &value)
{
    arg.beginStructure();
    arg << value.id << value.label << value.options << value.selected;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, FileChoice &value)
{
    arg.beginStructure();
    arg >> value.id >> value.label >> value.options >> value.selected;
    arg.endStructure();
    return arg;
}
