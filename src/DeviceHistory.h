#pragma once

// Which players this machine has actually talked to.
//
// Settings -> Device used to answer "no device connected" and nothing else, so
// a user who plugs in a second player, or who reconnects after a driver change,
// could not tell whether the app was looking at the same hardware as last time.
// libzune exposes no firmware version (zune.h has zune_get_serial and no
// firmware call), so a row is name + model + family + capacity + serial and the
// two dates - no firmware field is invented here.
//
// The file lives beside syncqueue.json in the profile's data folder, which is
// what ProfilePaths::writableLocation(AppDataLocation) names, so an isolated
// test profile gets its own history and a run that means to leave the real
// profile alone really does.
//
// One row per device, keyed by serial and falling back to model + name when a
// player answers no serial.  `connects` counts sessions in this process, not
// reconnect attempts: a finished sync arms a reconnect ladder whose rungs are
// each a full OpenSession, and counting those would report a player as handled
// a dozen times because one sync needed six probes.

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

class DeviceHistory : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    Q_PROPERTY(QVariantMap latest READ latest NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString storePath READ storePath CONSTANT)
public:
    struct Row {
        QString key;
        QString serial;
        QString name;
        QString model;      // marketing name, e.g. "Zune HD"
        int family = -1;
        double capacityGB = 0.0;
        QString firstSeen;  // UTC, ISO date-time
        QString lastSeen;
        int connects = 0;
    };

    explicit DeviceHistory(QObject *parent = nullptr);

    QVariantList devices() const;
    int count() const { return m_rows.size(); }
    QString storePath() const;

    // Called by DeviceService on a successful connection.  `model` is the
    // marketing name the service already resolves; pass an empty serial only
    // when the player genuinely answered none.
    void record(const QString &serial, const QString &name, const QString &model,
                int family, double capacityGB);

    Q_INVOKABLE void forget(const QString &key);
    Q_INVOKABLE void clearAll();

    // The row this machine saw most recently, in the same shape as a `devices`
    // entry, or an empty map when nothing has ever connected.  Settings -> Device
    // answers "which Zune is this" with it while no player is open: a name, a
    // model and a capacity do not change between visits, and a card that renders
    // nothing at all is read as a broken app rather than as an absent player.
    Q_INVOKABLE QVariantMap latest() const;

    // Test/diagnostic helper: the row for a serial, or an empty map.
    Q_INVOKABLE QVariantMap find(const QString &serial) const;

signals:
    void changed();

private:
    void load();
    bool save() const;
    int indexOfKey(const QString &key) const;

    QList<Row> m_rows;
    bool m_loaded = false;
    // The last identity this process counted, so a reconnect ladder touching
    // the same player does not add a dozen connections.
    QString m_countedKey;
};