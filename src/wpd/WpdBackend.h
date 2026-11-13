#pragma once

// Windows-only device backend that talks to a Zune through the operating
// system's own WPD/MTP driver instead of through libusb.
//
// Why this exists.  A Zune reaches Windows in one of two states and zuuned can
// only drive one of them by itself.  Bound to WinUSB the player is libzune's:
// ZMDB, vendor operations, album objects, playlists, the lot.  Bound to
// Windows' own WPD driver - which is what a tester who has never heard of Zadig
// gets, and what DevicePresence reports as problem 0 with libusb blind - the
// player is *already usable*, just not by us: Explorer and Windows Media
// Player read and write it, and libusb cannot claim an interface a kernel
// driver owns.  Rather than tell that user to steal the device from Windows,
// zuuned asks Windows for the same access Explorer has.  No driver install, no
// Zadig, Explorer keeps working, and one client still manages the player.
//
// What that costs: WPD is plain MTP.  There is no ZMDB, no vendor album or
// playlist forge, no representative-sample art, no podcast/audiobook state, and
// MTPZ authentication is not needed at all (which is why this path also works
// with a release build that ships no credentials).  Everything this backend
// cannot do reports as unsupported; it never answers a request with a lie.
//
// Thread note: one Session belongs to one thread and is not shared.  COM is
// initialised here as multithreaded, not apartment-threaded, because the worker
// thread that owns it has no message pump and WPD marshals across apartments.

#include <atomic>

#include <QString>
#include <QStringList>
#include <QVector>

namespace Wpd {

// What a WPD object is, collapsed onto the handful of distinctions zuuned makes.
// Anything WPD calls something else (a service provider, a functional object
// that is not storage) is KindOther/KindFunctional and is not offered to the UI.
enum ObjectKind {
    KindUnknown = 0,
    KindFolder,
    KindAudio,
    KindImage,
    KindVideo,
    KindFunctional,
    KindOther
};

struct DeviceInfo {
    QString pnpId;             // USB\VID_045E&PID_0710\... - never shown to the user as identity
    QString friendlyName;
    QString model;
    QString manufacturer;
    QString firmware;
    QString serial;
    int battery = -1;          // 0..100, -1 when the player does not say
    QString powerSource;
    quint64 capacityBytes = 0;  // summed over every storage object
    quint64 freeBytes = 0;
    QStringList storageIds;     // writable places, biggest free space first
};

struct ObjectInfo {
    QString id;                // WPD object id - the MTP handle in hex text
    QString name;
    QString parentId;
    quint64 size = 0;
    int kind = KindUnknown;
    QString format;            // WPD format guid text, for logs
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    QString genre;
    quint32 durationMs = 0;
    quint32 trackNumber = 0;
    bool canDelete = true;
};

// Fraction 0..1 of a byte transfer. Called from the worker thread; a queued
// signal is the caller's business, not this header's.
using ProgressFn = void (*)(double fraction, void *userdata);

class Session {
public:
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Every Zune on the bus that Windows has a WPD driver for.  This is also
    // the honest answer to "can zuuned reach it without touching drivers?" for
    // a player libusb cannot see.
    static QStringList zunePnpIds(QString *error);

    bool openFirstZune(QString *error);
    bool open(const QString &pnpId, QString *error);
    void close();
    bool isOpen() const { return m_device != nullptr; }
    QString pnpId() const { return m_pnpId; }

    bool readDevice(DeviceInfo *info, QString *error);
    bool children(const QString &parentId, QVector<ObjectInfo> *out, QString *error);
    bool objectInfo(const QString &id, ObjectInfo *out, QString *error);

    bool download(const QString &objectId, const QString &destPath,
                  ProgressFn fn, void *userdata, QString *error);
    // A player's own picture for an object, if it keeps one.  WPD calls the two
    // places that live THUMBNAIL and ALBUM_ART; the Zune is known for answering
    // "no representative sample" to MTP's own art request, so whether either
    // stream exists here is a question about this player, asked once per object
    // and answered honestly - an empty answer is an empty answer.
    bool readThumbnail(const QString &objectId, const QString &destPath, QString *error);
    // Which resource streams the player says an object has, spelled the way the
    // WPD documentation spells them.  This is the honest probe: an object that
    // names none keeps no picture, and one that names THUMBNAIL and then
    // refuses the stream is a different fault with a different fix.  Names only
    // - the header stays free of Windows types on purpose.
    bool supportedResourceNames(const QString &objectId, QStringList *names, QString *error);
    bool makeFolder(const QString &parentId, const QString &name,
                    QString *objectId, QString *error);
    bool upload(const QString &parentId, const QString &srcPath, const ObjectInfo &meta,
                ProgressFn fn, void *userdata, QString *objectId, QString *error);
    // Deletes one at a time so a player that refuses one object does not take
    // the whole selection with it; `purged` gets the ids it really dropped.
    bool removeObjects(const QStringList &objectIds, QStringList *purged, QString *error);

    // Cancel an in-flight transfer. Safe from another thread: it only flips a
    // flag this class checks between stream chunks, and asks WPD to cancel.
    void requestAbort() { m_abort.store(true); }
    void clearAbort() { m_abort.store(false); }
    bool aborted() const { return m_abort.load(); }

    // A PnP device path with the serial taken out of it.  The instance chunk
    // between the two '#' is the player's serial number in another dress, and a
    // session log is the thing that becomes a support report somebody else
    // reads.  The hardware id and the device interface class either side of it
    // are what a reader actually needs, so those stay and the instance becomes a
    // short stable alias - two log lines about one player still recognisably
    // belong together.  Unrecognised input is withheld rather than passed on.
    static QString maskedPathText(const QString &pnpId);

    static QString kindName(int kind);
    // The WPD format a file should be announced as, from its extension:
    // 1 WMA, 2 MP3, 3 FLAC, 4 WAV, 0 unspecified.
    static int formatForExtension(const QString &path);
    // The other direction, and the one that decides what a pulled file is called:
    // a WPD object's format property is the only place this player states the
    // container it really holds, because the name it displays is a title and a
    // title with no dot reaches us as `.mp3`.  Empty answer for a format this app
    // does not name, which leaves the caller's path exactly as it was.
    static QString extensionForFormat(const QString &formatGuidText);

private:
    bool ensureCom();
    void releaseCom();
    bool childrenOf(const QString &parentId, int depth, int maxDepth, int budget,
                    QVector<ObjectInfo> *out, QString *error);

    void *m_device = nullptr;      // IPortableDevice*
    void *m_content = nullptr;     // IPortableDeviceContent*
    // Which property shape this player took for an uploaded file, or -1 until a
    // transfer has actually been committed.  Measured: the same file is refused by one
    // player and accepted by another on the strength of which optional properties the
    // create carries, so the answer is remembered per connection rather than guessed
    // per file.  See Session::upload().
    int m_uploadShape = -1;
    QString m_pnpId;
    bool m_ownsCom = false;
    std::atomic<bool> m_abort{false};
};

} // namespace Wpd
