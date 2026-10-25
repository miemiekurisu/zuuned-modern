#include "DeviceHistory.h"

#include "ProfilePaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace {

const char *kStoreFile = "devices.json";

QString isoNow()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

}  // namespace

DeviceHistory::DeviceHistory(QObject *parent) : QObject(parent) {}

QString DeviceHistory::storePath() const
{
    return Zuuned::ProfilePaths::writableLocation(QStandardPaths::AppDataLocation)
           + QLatin1Char('/') + QLatin1String(kStoreFile);
}

void DeviceHistory::load()
{
    m_loaded = true;
    if (!m_rows.isEmpty())
        return;

    QFile f(storePath());
    if (!f.open(QIODevice::ReadOnly))
        return;  // no history yet is the normal case, not an error
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parse);
    f.close();
    if (parse.error != QJsonParseError::NoError || !doc.isArray()) {
        qWarning("[device-history] %s is not readable as a history; starting empty",
                 qUtf8Printable(storePath()));
        return;
    }
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        Row r;
        r.key = o.value(QLatin1String("key")).toString();
        if (r.key.isEmpty())
            continue;
        r.serial = o.value(QLatin1String("serial")).toString();
        r.name = o.value(QLatin1String("name")).toString();
        r.model = o.value(QLatin1String("model")).toString();
        r.family = o.value(QLatin1String("family")).toInt(-1);
        r.capacityGB = o.value(QLatin1String("capacityGB")).toDouble();
        r.firstSeen = o.value(QLatin1String("firstSeen")).toString();
        r.lastSeen = o.value(QLatin1String("lastSeen")).toString();
        r.connects = o.value(QLatin1String("connects")).toInt(1);
        m_rows.append(r);
    }
}

bool DeviceHistory::save() const
{
    QJsonArray rows;
    for (const Row &r : m_rows) {
        QJsonObject o;
        o.insert(QLatin1String("key"), r.key);
        o.insert(QLatin1String("serial"), r.serial);
        o.insert(QLatin1String("name"), r.name);
        o.insert(QLatin1String("model"), r.model);
        o.insert(QLatin1String("family"), r.family);
        o.insert(QLatin1String("capacityGB"), r.capacityGB);
        o.insert(QLatin1String("firstSeen"), r.firstSeen);
        o.insert(QLatin1String("lastSeen"), r.lastSeen);
        o.insert(QLatin1String("connects"), r.connects);
        rows.append(o);
    }

    const QString path = storePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        qWarning("[device-history] cannot write %s", qUtf8Printable(path));
        return false;
    }
    out.write(QJsonDocument(rows).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        qWarning("[device-history] commit failed for %s", qUtf8Printable(path));
        return false;
    }
    return true;
}

int DeviceHistory::indexOfKey(const QString &key) const
{
    for (int i = 0; i < m_rows.size(); i++)
        if (m_rows.at(i).key == key)
            return i;
    return -1;
}

QVariantList DeviceHistory::devices() const
{
    // Newest sighting first: the player a user is looking at should be the one
    // at the top of the list.
    QList<Row> sorted = m_rows;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Row &a, const Row &b) { return a.lastSeen > b.lastSeen; });

    QVariantList list;
    for (const Row &r : sorted) {
        QVariantMap m;
        m.insert(QStringLiteral("key"), r.key);
        m.insert(QStringLiteral("serial"), r.serial);
        m.insert(QStringLiteral("name"), r.name);
        m.insert(QStringLiteral("model"), r.model);
        m.insert(QStringLiteral("family"), r.family);
        m.insert(QStringLiteral("capacityGB"), r.capacityGB);
        m.insert(QStringLiteral("firstSeen"), r.firstSeen);
        m.insert(QStringLiteral("lastSeen"), r.lastSeen);
        m.insert(QStringLiteral("connects"), r.connects);
        // The date only. The full timestamp is in the store for anyone who
        // wants it; a settings row that reads "Last seen 2026-10-24T07:13:02"
        // is longer than the question.
        m.insert(QStringLiteral("lastSeenDate"), r.lastSeen.left(10));
        list.append(m);
    }
    return list;
}

QVariantMap DeviceHistory::latest() const
{
    // `devices()` already puts the newest sighting first, so its first row is
    // the answer.  Handing back the same map shape keeps one vocabulary between
    // the history list and the identity card above it.
    const QVariantList list = devices();
    return list.isEmpty() ? QVariantMap() : list.first().toMap();
}

void DeviceHistory::record(const QString &serial, const QString &name,
                           const QString &model, int family, double capacityGB)
{
    load();

    // A player that answers neither a serial nor a name gives nothing to key
    // on, and a row of blanks is worse than no row.
    if (serial.isEmpty() && name.isEmpty() && model.isEmpty())
        return;

    const QString key = serial.isEmpty()
        ? QStringLiteral("%1|%2").arg(model, name)
        : serial;
    const QString now = isoNow();

    int i = indexOfKey(key);
    if (i < 0) {
        Row r;
        r.key = key;
        r.serial = serial;
        r.name = name;
        r.model = model;
        r.family = family;
        r.capacityGB = capacityGB;
        r.firstSeen = now;
        r.lastSeen = now;
        r.connects = 1;
        m_rows.append(r);
        m_countedKey = key;
        save();
        emit changed();
        return;
    }

    Row &r = m_rows[i];
    const bool newSession = m_countedKey != key;
    r.lastSeen = now;
    if (!serial.isEmpty())
        r.serial = serial;
    if (!name.isEmpty())
        r.name = name;
    if (!model.isEmpty())
        r.model = model;
    if (family >= 0)
        r.family = family;
    if (capacityGB > 0.0)
        r.capacityGB = capacityGB;
    // One count per identity per process, however many attempts got it back.
    if (newSession)
        r.connects += 1;
    m_countedKey = key;
    save();
    emit changed();
}

void DeviceHistory::forget(const QString &key)
{
    load();
    const int i = indexOfKey(key);
    if (i < 0)
        return;
    m_rows.removeAt(i);
    if (m_countedKey == key)
        m_countedKey.clear();
    save();
    emit changed();
}

void DeviceHistory::clearAll()
{
    load();
    if (m_rows.isEmpty())
        return;
    m_rows.clear();
    m_countedKey.clear();
    save();
    emit changed();
}

QVariantMap DeviceHistory::find(const QString &serial) const
{
    const int i = indexOfKey(serial);
    if (i < 0)
        return QVariantMap();
    const Row &r = m_rows.at(i);
    QVariantMap m;
    m.insert(QStringLiteral("key"), r.key);
    m.insert(QStringLiteral("serial"), r.serial);
    m.insert(QStringLiteral("name"), r.name);
    m.insert(QStringLiteral("model"), r.model);
    m.insert(QStringLiteral("family"), r.family);
    m.insert(QStringLiteral("capacityGB"), r.capacityGB);
    m.insert(QStringLiteral("firstSeen"), r.firstSeen);
    m.insert(QStringLiteral("lastSeen"), r.lastSeen);
    m.insert(QStringLiteral("connects"), r.connects);
    return m;
}