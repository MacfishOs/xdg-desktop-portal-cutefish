#pragma once

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusVariant>
#include <QList>
#include <QString>
#include <QVariantMap>

// (ii): stream position and size in the compositor coordinate space.
struct IntPair {
    int first = 0;
    int second = 0;
};

// (ua{sv}): a PipeWire node id and its stream properties.
struct StreamInfo {
    uint node = 0;
    QVariantMap properties;
};

// (suv): vendor, format version and private data of a restorable selection.
struct RestoreData {
    QString vendor;
    uint version = 0;
    QDBusVariant data;
};

Q_DECLARE_METATYPE(IntPair)
Q_DECLARE_METATYPE(StreamInfo)
Q_DECLARE_METATYPE(RestoreData)

inline QDBusArgument &operator<<(QDBusArgument &arg, const IntPair &value)
{
    arg.beginStructure();
    arg << value.first << value.second;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, IntPair &value)
{
    arg.beginStructure();
    arg >> value.first >> value.second;
    arg.endStructure();
    return arg;
}

inline QDBusArgument &operator<<(QDBusArgument &arg, const StreamInfo &value)
{
    arg.beginStructure();
    arg << value.node << value.properties;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, StreamInfo &value)
{
    arg.beginStructure();
    arg >> value.node >> value.properties;
    arg.endStructure();
    return arg;
}

inline QDBusArgument &operator<<(QDBusArgument &arg, const RestoreData &value)
{
    arg.beginStructure();
    arg << value.vendor << value.version << value.data;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, RestoreData &value)
{
    arg.beginStructure();
    arg >> value.vendor >> value.version >> value.data;
    arg.endStructure();
    return arg;
}

inline void registerScreenCastTypes()
{
    qDBusRegisterMetaType<IntPair>();
    qDBusRegisterMetaType<StreamInfo>();
    qDBusRegisterMetaType<QList<StreamInfo>>();
    qDBusRegisterMetaType<RestoreData>();
}
