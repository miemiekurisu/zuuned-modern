#pragma once

#include <QColor>
#include <QFile>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include "DeviceWorker.h"
#include "TrackImportBatch.h"
#include "DeviceHistory.h"
#include "TrackModel.h"
#include "library/MusicIdentity.h"
#include "UiText.h"

class UsbWatcher;
class LibraryService;

// UI-thread facade over DeviceWorker — the AppState/MTPService pair of
// this app. Holds reactive state for QML; every libzune call happens on
// the worker thread (see DeviceWorker.h for the serialization contract).
//
// Connection flow (mirrors USBWatcher → AppState.connectDevice on macOS):
// UsbWatcher emits zuneArrived (also at startup for an already-plugged
// device) → short settle delay → breach → ZMDB scan → library populated.
class DeviceService : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool devicePresent READ devicePresent NOTIFY stateChanged)
    // C3: is the udev rule installed? Drives the in-app fix-it card
    // when a Zune is present but can't be opened.
    Q_PROPERTY(bool udevRuleOk READ udevRuleOk NOTIFY udevRuleChanged)
    Q_PROPERTY(QString udevInstallCommand READ udevInstallCommand CONSTANT)
    // udev is a Linux concept. Every rule UI keys off this so Windows never
    // offers an install that could not possibly work there.
    Q_PROPERTY(bool udevRulesApply READ udevRulesApply CONSTANT)
    // One view of "the Zune is here but zuuned cannot open it": a missing udev
    // rule on Linux, a driver Windows never bound on Windows.  The fix-it card
    // and Settings key off these instead of guessing which platform they run on.
    Q_PROPERTY(bool usbAccessProblem READ usbAccessProblem NOTIFY usbAccessChanged)
    Q_PROPERTY(bool usbAccessFixable READ usbAccessFixable CONSTANT)
    Q_PROPERTY(QString usbAccessTitle READ usbAccessTitle NOTIFY usbAccessChanged)
    Q_PROPERTY(QString usbAccessDetail READ usbAccessDetail NOTIFY usbAccessChanged)
    Q_PROPERTY(QString usbAccessCommand READ usbAccessCommand NOTIFY usbAccessChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(QString name READ name NOTIFY stateChanged)
    Q_PROPERTY(QString deviceSerial READ deviceSerial NOTIFY stateChanged)
    // Which protocol this session reached the player over: "libzune" for the
    // ZMDB/MTPZ protocol on a WinUSB-bound interface, "wpd" for Windows'
    // own MediaDevice (MTP) driver.  Empty while nothing is connected, and on
    // Linux, which only has the first road.  The UI uses it to say so rather
    // than to change what it asks for - DeviceWorker already picked the road.
    Q_PROPERTY(QString deviceBackend READ deviceBackend NOTIFY stateChanged)
    Q_PROPERTY(QString model READ model NOTIFY stateChanged)
    // ZuneDeviceFamily (0xD21A): 0x00 Keel/30, 0x02 Scorpius, 0x03 Draco,
    // 0x06 Pavo/HD, 0xFF unknown — Settings shows the matching profile
    Q_PROPERTY(int deviceFamily READ deviceFamily NOTIFY stateChanged)
    // Human model: "Zune 30/4/8/16/80/120/HD" — family + capacity
    // disambiguate (Scorpius covers 4/8/16, Draco 80/120). Never just
    // "Zune" in the UI (design contract).
    Q_PROPERTY(QString modelName READ modelName NOTIFY stateChanged)
    Q_PROPERTY(int battery READ battery NOTIFY stateChanged)
    Q_PROPERTY(double capacityGB READ capacityGB NOTIFY stateChanged)
    Q_PROPERTY(double freeGB READ freeGB NOTIFY stateChanged)
    // Storage breakdown (stats tab): summed from the ZMDB scan.
    Q_PROPERTY(double musicGB READ musicGB NOTIFY stateChanged)
    Q_PROPERTY(double videoGB READ videoGB NOTIFY stateChanged)
    Q_PROPERTY(double photoGB READ photoGB NOTIFY stateChanged)
    // Purge (device delete) live state — drives the "deleting N of M…"
    // indicator so a multi-item delete reads as work, not a freeze.
    Q_PROPERTY(bool purging READ purging NOTIFY purgeChanged)
    Q_PROPERTY(int purgeCurrent READ purgeCurrent NOTIFY purgeChanged)
    Q_PROPERTY(int purgeTotal READ purgeTotal NOTIFY purgeChanged)
    // Pull (device→library extract) live state — drives the "pulling N
    // of M from your zune" indicator so a drag-to-library shows work,
    // its result, and any failures instead of a silent toast.
    Q_PROPERTY(bool pulling READ pulling NOTIFY pullChanged)
    Q_PROPERTY(int pullCurrent READ pullCurrent NOTIFY pullChanged)
    Q_PROPERTY(int pullTotal READ pullTotal NOTIFY pullChanged)
    Q_PROPERTY(QString pullNoun READ pullNoun NOTIFY pullChanged)
    Q_PROPERTY(TrackModel *tracks READ tracks CONSTANT)
    // Every player this profile has connected to, newest sighting first, plus
    // the one on the cable right now (the ordinary name/model/serial
    // properties above).  See DeviceHistory.h for what a row is and is not.
    Q_PROPERTY(QObject *deviceHistory READ deviceHistory CONSTANT)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY stateChanged)
    Q_PROPERTY(int albumCount READ albumCount NOTIFY stateChanged)
    Q_PROPERTY(int artistCount READ artistCount NOTIFY stateChanged)
    Q_PROPERTY(QVariantList artistsList READ artistsList NOTIFY stateChanged)
    Q_PROPERTY(QVariantList albumsList READ albumsList NOTIFY stateChanged)
    Q_PROPERTY(QVariantList genresList READ genresList NOTIFY stateChanged)
    Q_PROPERTY(QVariantList videosList READ videosList NOTIFY stateChanged)
    Q_PROPERTY(QVariantList photosList READ photosList NOTIFY stateChanged)
    // Device photo folder-albums [{itemId, name}] from the ZMDB scan.
    Q_PROPERTY(QVariantList photoAlbumsList READ photoAlbumsList NOTIFY stateChanged)
    Q_PROPERTY(QVariantList playlistsList READ playlistsList NOTIFY stateChanged)
    Q_PROPERTY(QVariantList podcastEpisodesList READ podcastEpisodesList NOTIFY spokenMediaChanged)
    Q_PROPERTY(QVariantList audiobookPartsList READ audiobookPartsList NOTIFY spokenMediaChanged)
    Q_PROPERTY(bool audiobookBrowseSupported READ audiobookBrowseSupported NOTIFY stateChanged)
    Q_PROPERTY(bool spokenMediaLoading READ spokenMediaLoading NOTIFY spokenMediaChanged)
    Q_PROPERTY(QString spokenMediaError READ spokenMediaError NOTIFY spokenMediaChanged)
    Q_PROPERTY(QString podcastStatusWarning READ podcastStatusWarning NOTIFY spokenMediaChanged)
    // itemId (as string key) → "file://..." for fetched album art.
    Q_PROPERTY(QVariantMap artPaths READ artPaths NOTIFY artChanged)
    // lower(artist\talbum\ttitle) → true, for on-device badges + dedup
    Q_PROPERTY(QVariantMap onDeviceKeys READ onDeviceKeys NOTIFY stateChanged)
    Q_PROPERTY(quint64 musicIdentityRevision READ musicIdentityRevision NOTIFY stateChanged)
    // lower(filename stem) → true. Photos are re-armed to .jpg on the
    // way to the device, so badges/dedup match on the extensionless
    // stem, never the full filename.
    Q_PROPERTY(QVariantMap photoOnDeviceKeys READ photoOnDeviceKeys NOTIFY stateChanged)
    // Staged connection progress (mac connectionStage/Progress) + a
    // distinct error slot the status string can't express.
    Q_PROPERTY(QString connectionStage READ connectionStage NOTIFY stateChanged)
    Q_PROPERTY(double connectionProgress READ connectionProgress NOTIFY stateChanged)
    Q_PROPERTY(QString connectionError READ connectionError NOTIFY stateChanged)
    // Device mood color (ZuneColors.deviceMoodColor): genre-biased pick
    // on connect, transparent when disconnected. Drives the environment
    // bloom + grunge accent tint.
    Q_PROPERTY(QColor moodColor READ moodColor NOTIFY stateChanged)
    // Post-sync gate (mac awaitingDisconnect): while true, the device
    // must not be auto-breached on arrival — it may be re-indexing.
    Q_PROPERTY(bool awaitingDisconnect READ awaitingDisconnect
               WRITE setAwaitingDisconnect NOTIFY stateChanged)

public:
    explicit DeviceService(QObject *parent = nullptr);
    ~DeviceService() override;

    bool connected() const { return m_connected; }
    bool busy() const { return m_busy || m_spokenMediaLoading || m_purging; }
    void setSyncActive(bool active);
    // A pull needs somewhere to write, and that is the library's answer, not
    // a path this class remembers: see importsDir() and its two siblings.
    void setLibraryService(LibraryService *library) { m_library = library; }
    bool devicePresent() const { return m_devicePresent; }
    bool udevRuleOk() const;
    QString udevInstallCommand() const;
    bool udevRulesApply() const;
    bool usbAccessProblem() const;
    bool usbAccessFixable() const;
    QString usbAccessTitle() const;
    QString usbAccessDetail() const;
    QString usbAccessCommand() const;
    // W5/#28: install the udev rule with one click via pkexec — the
    // desktop's native password dialog, no terminal. Writes the rule,
    // reloads udev, and re-triggers so an already-plugged device picks
    // it up without a replug. Emits udevRuleInstalled(ok, message).
    Q_INVOKABLE void installUdevRule();
    QString status() const { return m_status; }
    QString name() const { return m_name; }
    QString model() const { return m_model; }
    // ZuneDeviceFamily (MTP 0xD21A): 0x00 Keel/Zune30, 0x02 Scorpius,
    // 0x03 Draco, 0x06 Pavo/HD, 0xFF unknown. The model string is just
    // "Zune" on real hardware — family is the only reliable generation
    // signal (video profile auto-detect).
    int deviceFamily() const { return m_family; }
    QString modelName() const {
        const double gb = m_capacityGB;
        switch (m_family) {
        case 0x00: return QStringLiteral("Zune 30");
        case 0x02:  // Scorpius — 4/8/16 by capacity
            return gb > 12 ? QStringLiteral("Zune 16")
                 : gb > 6  ? QStringLiteral("Zune 8")
                           : QStringLiteral("Zune 4");
        case 0x03:  // Draco — 80/120 by capacity
            return gb > 100 ? QStringLiteral("Zune 120")
                            : QStringLiteral("Zune 80");
        case 0x06: return QStringLiteral("Zune HD");
        default:   return QStringLiteral("Zune");
        }
    }
    int battery() const { return m_battery; }
    double capacityGB() const { return m_capacityGB; }
    double freeGB() const { return m_freeGB; }
    double musicGB() const { return m_musicGB; }
    double videoGB() const { return m_videoGB; }
    double photoGB() const { return m_photoGB; }
    bool purging() const { return m_purging; }
    int purgeCurrent() const { return m_purgeCurrent; }
    int purgeTotal() const { return m_purgeTotal; }
    bool pulling() const { return m_pulling; }
    int pullCurrent() const { return m_pullDone; }
    int pullTotal() const { return m_pullTotal; }
    QString pullNoun() const { return m_pullNoun; }
    // QML calls this at the START of a drag-to-library batch: `total` is
    // how many items will actually be extracted (misses excluded),
    // `existing` how many were skipped as already-owned. The extract-done
    // signals then drive the bar to completion.
    Q_INVOKABLE void beginPull(const QString &noun, int total, int existing);
    // Sync engine: a file just landed — decrement free space locally so
    // the storage bar and capacity gate track reality mid-session (the
    // device only reports fresh numbers at the next breach).
    void noteBytesWritten(qint64 bytes) {
        if (bytes <= 0)
            return;
        m_freeGB = qMax(0.0, m_freeGB - bytes / 1e9);
        emit stateChanged();
    }
    // W3: the inverse — a purge just freed space. The device won't
    // report the new free number until its CleanDataStore re-index
    // (at eject), so credit it back optimistically NOW or the capacity
    // gate keeps refusing resyncs against a stale "full".
    void noteBytesFreed(qint64 bytes) {
        if (bytes <= 0)
            return;
        m_freeGB = qMin(m_capacityGB, m_freeGB + bytes / 1e9);
        emit stateChanged();
    }
    // Re-read capacity/free from the DEVICE (ground truth). Fired after
    // purges and completed syncs — local accounting only ever
    // subtracts, so deletes left the capacity gate thinking "full".
    Q_INVOKABLE void refreshStorage() {
        if (m_connected)
            emit workerRefreshStorage();
    }
    TrackModel *tracks() { return &m_tracks; }
    QObject *deviceHistory() { return m_deviceHistory; }
    int trackCount() const { return m_trackCount; }
    int albumCount() const { return m_albumCount; }
    int artistCount() const { return m_artistCount; }
    QVariantList artistsList() const { return m_artistsList; }
    QVariantList albumsList() const { return m_albumsList; }
    QVariantList genresList() const { return m_genresList; }
    QVariantList videosList() const { return m_videosList; }
    QVariantList photosList() const { return m_photosList; }
    QVariantList photoAlbumsList() const { return m_photoAlbumsList; }
    QVariantList playlistsList() const { return m_playlistsList; }
    QVariantList podcastEpisodesList() const { return m_podcastEpisodesList; }
    QVariantList audiobookPartsList() const { return m_audiobookPartsList; }
    bool audiobookBrowseSupported() const { return m_connected && (m_family == 0x00 || m_family == 0x03 || m_family == 0x06); }
    bool spokenMediaLoading() const { return m_spokenMediaLoading; }
    bool podcastInventoryReady() const { return m_connected && !m_spokenMediaLoading &&
        m_spokenMediaError.isEmpty() && m_podcastMembershipKnown; }
    quint64 spokenGeneration() const { return m_spokenGeneration; }
    void applyPodcastCleanup(quint64 generation, const QString &serial, const QVariantList &rows);
    Q_INVOKABLE QVariantMap podcastDeviceStatus(const QString &feedUrl, const QString &sourceUrl) const;
    void notePodcastOnDevice(quint64 generation, const QString &serial,
                             const QVariantMap &episode, quint32 itemId);
    QString spokenMediaError() const { return m_spokenMediaError; }
    QString podcastStatusWarning() const { return m_podcastStatusWarning; }
    Q_INVOKABLE void ensureSpokenMedia();
    Q_INVOKABLE void refreshSpokenMedia();
    Q_INVOKABLE void purgeSpokenItems(const QString &kind, const QVariantList &itemIds);
    QVariantMap artPaths() const { return m_artPaths; }

    QString connectionStage() const { return m_connectionStage; }
    double connectionProgress() const { return m_connectionProgress; }
    QString connectionError() const { return m_connectionError; }
    QColor moodColor() const { return m_moodColor; }

    Q_INVOKABLE void breach();   // connect + MTPZ auth + library scan
    Q_INVOKABLE void sever();    // disconnect
    // Request album art for an item; artPaths updates when it lands.
    Q_INVOKABLE void requestArt(quint32 itemId);
    // Request a photo thumbnail; lands in artPaths like album art.
    Q_INVOKABLE void requestPhotoThumb(quint32 itemId);
    // Delete objects from the device (tracks by itemId); models update
    // locally on completion — full truth returns at next breach.
    Q_INVOKABLE void purgeItems(const QVariantList &itemIds);
    bool purgeInterruptedVideo(const QString &filename);
    bool purgeInterruptedObject(const QVariantMap &record);
    QString deviceSerial() const { return m_connected ? m_serial : QString(); }
    QString deviceBackend() const { return m_connected ? m_backend : QString(); }
    QVariantMap onDeviceKeys() const { return m_onDeviceKeys; }
    quint64 musicIdentityRevision() const { return m_musicIdentityRevision; }
    Q_INVOKABLE QVariantList musicIdentityPeers(const QVariantMap &track) const;
    Q_INVOKABLE bool hasDeviceTrack(const QVariantMap &track, const QVariantList &sourcePeers = {}) const;
    QVariantMap photoOnDeviceKeys() const { return m_photoOnDeviceKeys; }
    static QString photoStem(const QString &name) {
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        return (dot > 0 ? name.left(dot) : name).toLower();
    }
    // Sync engine callback: a photo just landed — badges + dedup stay
    // fresh mid-session.
    void noteSyncedPhoto(const QString &name);
    // Sync engine callback: a track just landed on the device.
    void noteSyncedTrack(const QString &title, const QString &artist,
                         const QString &album, quint32 itemId, int discNumber = 0,
                         int trackNumber = 0, int durationMs = 0);
    // Sync engine callback: a video just landed on the device — keeps
    // videosList (browse page + on-device badges) fresh mid-session.
    void noteSyncedVideo(const QVariantMap &video);
    // Save a device video into the library's video import root[/subdir]/
    // (mac's Save to Library). Sequential on the device thread; videoSaved
    // fires per item. The root is already a scanned folder, so nothing has to
    // be registered after the fact.
    Q_INVOKABLE void saveVideoToLibrary(quint32 itemId, const QString &filename,
                                        const QString &subdir = QString());
    // The three roots below are the library's, not constants: the folder the
    // user made the default for that kind, else the first folder they added
    // that the scanner reads that kind from, else ~/Videos|Pictures|Music/
    // Zuuned Imports -- which LibraryService registers as a watch folder on
    // the way out, so an import is never written where nothing looks.
    Q_INVOKABLE QString importsDir();
    // ── Phase 8: photos + music downloads (parity with videos) ──
    // Save a device photo into the photo import root[/subdir]/.
    Q_INVOKABLE void savePhotoToLibrary(quint32 itemId, const QString &filename,
                                        const QString &subdir = QString());
    Q_INVOKABLE QString photoImportsDir();
    // Save a device track into the music import root/Artist/Album/.
    Q_INVOKABLE void saveTrackToLibrary(quint32 itemId, const QString &filename,
                                        const QString &artist = QString(),
                                        const QString &album = QString());
    Q_INVOKABLE QString musicImportsDir();
    // Gallery full-res resolver: extract to the cache dir (skips USB if
    // cached); photoFullReady fires with a file:// url.
    Q_INVOKABLE void requestPhotoFull(quint32 itemId);
    // ── Phase 9: playlists ──
    // Create a playlist on the device from DEVICE track ids.
    // playlistCreated fires with the verdict; playlistsList updates
    // locally on success.
    Q_INVOKABLE void createPlaylist(const QString &name,
                                    const QVariantList &trackIds);
    // Resolve a device track id → {title, artist, album} (playlist
    // detail rows). Empty map when unknown.
    Q_INVOKABLE QVariantMap trackInfo(quint32 itemId) const;
    // Flat [{itemId, title, artist, album}] of every device track —
    // the create-sheet's multiselect source.
    Q_INVOKABLE QVariantList deviceTrackRows() const;
    // Rename an item's display Name on the device (0xDC44). videosList
    // updates locally on success; renameFinished carries the verdict.
    Q_INVOKABLE void renameItem(quint32 itemId, const QString &newName);
    // Rename the DEVICE (Friendly Name 0xD402) — click the name in the
    // panel. `name` property updates on success.
    Q_INVOKABLE void renameDevice(const QString &newName);
    // Eject (mac disconnectDevice): finalize (vendor ops → device
    // re-index) then sever. SyncEngine must forceStop first.
    Q_INVOKABLE void eject();
    // Post-sync auto-release (mac LIBMTP_Release_Device pattern):
    // finalize + sever WITHOUT ejectStarted, so the sync queue view
    // survives. Closing the session is what moves the device out of
    // sync mode and into its restart/re-index cycle — leaving the
    // session open parks its screen at 100% forever.
    void finalizeAndRelease();

    bool awaitingDisconnect() const { return m_awaitingDisconnect; }
    void setAwaitingDisconnect(bool v);
    // A Zune that re-indexes without leaving the bus offers no hotplug
    // arrival, and on Windows that is every Zune: WinUSB keeps the interface
    // for the whole re-index.  The finalize chain therefore arms its own
    // reconnect - a bounded ladder, because the only honest readiness probe
    // is an OpenSession and a mid-re-index player answers late rather than
    // never.
    void scheduleReprobeAfterReindex();
    void reprobeNow();
    // SyncEngine connects DIRECTLY to worker signals and invokes its
    // sync slots via QMetaObject::invokeMethod(Qt::QueuedConnection) —
    // the worker lives on the device thread; NEVER call its slots
    // synchronously from another thread (requestAbort is the one
    // documented cross-thread-safe call).
    DeviceWorker *worker() const { return m_worker; }

signals:
    void workerPurgeInterruptedVideo(const QString &filename);
    void interruptedVideoPurged(const QString &filename, bool ok);
    void workerPurgeInterruptedObject(const QVariantMap &record);
    void interruptedObjectPurged(const QString &token, bool ok);
    void stateChanged();
    void spokenMediaChanged();
    void workerReadSpokenMedia(quint64 generation, quint64 request, const QString &serial);
    void workerPurgeSpokenItems(quint64 generation, quint64 request, const QString &serial,
                                const QString &kind, const QVariantList &rows);
    void purgeChanged();
    void pullChanged();
    // A one-line pull result for the toast host (completion, or the
    // "you already own all of these" no-op case).
    void pullToast(const QString &message);
    // Fired at eject start — the sync engine clears its queue on this
    // (mac disconnectDevice: forceStop + clearQueue + finalize).
    void ejectStarted();
    void artChanged();

    // Internal → worker (queued). Not for QML.
    void workerFinalize();
    void workerBreach();
    void workerSever();
    void workerGrabArt(quint32 itemId, const QString &cachePath);
    void workerExtractVideo(quint32 itemId, const QString &destPath);
    void workerExtractPhoto(quint32 itemId, const QString &destPath);
    void workerExtractTrack(quint32 itemId, const QString &destPath);
    // ok=false → download failed
    void photoSaved(const QString &filename, bool ok);
    void trackSaved(const QString &filename, bool ok);
    // A serial device music-import burst has completed. Main registers the
    // imports root, rescans it once and primes these distinct artist matches.
    void musicImportsReady(const QStringList &artists);
    // Album art lives separately on the Zune's abstract album object. A
    // successful music pull promotes that JPEG into the local album cache.
    void musicImportAlbumArtReady(const QString &artist, const QString &album,
                                  const QString &path);
    void photoFullReady(quint32 itemId, const QString &url);
    void workerForgePlaylist(const QString &name, const QVariantList &trackIds);
    void playlistCreated(const QString &name, bool ok);
    void workerRenameItem(quint32 itemId, const QString &newName);
    void workerRenameDevice(const QString &newName);
    void workerRefreshStorage();
    // ok=false → download failed (autopsy in the log)
    void videoSaved(const QString &filename, bool ok);
    void renameFinished(quint32 itemId, const QString &newName, bool ok);
    void workerGrabPhotoThumb(quint32 itemId, const QString &cachePath);
    void udevRuleChanged();
    void udevRuleInstalled(bool ok, const QString &message);
    void usbAccessChanged();
    void workerPurgeItems(const QVariantList &itemIds);

private:
    friend struct DeviceSpokenTestAccess;
    DeviceService(QObject *parent, bool watchDevice);
    void onBreachDone(const DeviceWorker::BreachData &r);
    void onWorkerStage(const QString &label, double progress);
    static QColor pickMoodColor(const QVariantList &genres);
    void onSeverDone();
    void onFinalizeDone(bool ok);
    void onArtDone(quint32 itemId, const QString &path, bool ok);
    void onPurgeDone(int okCount, int failCount, const QVariantList &purgedIds);
    void onZuneArrived();
    void onZuneLeft();
    // The operating system can see a Zune that libusb cannot (Windows with no
    // driver bound).  Presence is then real but useless: say so, and do NOT
    // schedule a breach into a device that cannot answer.
    void onZuneBlocked(const QString &detail);
    void onZuneUnblocked();
    void clearDeviceState();
    void requestImportedAlbumArt(quint32 itemId, const QString &artist,
                                 const QString &album);

    QThread m_workerThread;
    DeviceWorker *m_worker = nullptr;
    UsbWatcher *m_watcher = nullptr;

    bool m_connected = false;
    bool m_busy = false;
    bool m_devicePresent = false;
    // Presence the operating system asserted while libusb stayed blind, and its
    // evidence.  Kept apart from m_devicePresent so a real arrival is never
    // mistaken for the blocked case, or the other way round.
    QString m_usbBlockDetail;
    bool m_blockedPresence = false;
    QString m_status = UiText::tr("Waiting for a Zune…");
    QString m_name, m_model;
    QString m_serial;
    QString m_backend;
    int m_family = 0xFF;
    int m_battery = 0;
    double m_capacityGB = 0, m_freeGB = 0;
    double m_musicGB = 0, m_videoGB = 0, m_photoGB = 0;
    bool m_purging = false;
    int m_purgeCurrent = 0, m_purgeTotal = 0;
    bool m_pulling = false;
    int m_pullTotal = 0, m_pullDone = 0, m_pullFailed = 0, m_pullExisting = 0;
    QString m_pullNoun = QStringLiteral("item");
    void notePullDone(bool ok);   // one extract finished (import path)
    TrackModel m_tracks;
    DeviceHistory *m_deviceHistory = nullptr;
    int m_trackCount = 0, m_albumCount = 0, m_artistCount = 0;
    QVariantList m_artistsList, m_albumsList, m_genresList;
    QVariantList m_videosList, m_photosList, m_playlistsList;
    QVariantList m_photoAlbumsList;
    QVariantList m_podcastEpisodesList, m_audiobookPartsList;
    QHash<QPair<QString, QString>, QVariantMap> m_podcastDeviceStatus;
    bool m_podcastMembershipKnown = false;
    QString m_podcastStatusWarning;
    void rebuildPodcastDeviceStatus();
    void prunePodcastDeviceStatus(const QSet<quint32> &ids);
    bool m_spokenMediaLoading = false, m_spokenRequested = false, m_spokenReadDeferred = false;
    bool m_syncActive = false;
    LibraryService *m_library = nullptr;   // import roots; not owned
    QString m_spokenMediaError;
    quint64 m_spokenGeneration = 1, m_spokenRequest = 0;
    QString m_spokenArtSession;
    quint64 m_spokenArtRevision = 0;
    QSet<quint32> m_spokenArtIds;
    // requestPhotoFull extracts pending → suppress duplicate requests
    QSet<quint32> m_photoFullPending;
    QVariantMap m_artPaths;
    QSet<quint32> m_artRequested;
    QString m_artCacheDir;
    QVariantMap m_onDeviceKeys;
    MusicIdentity::Index m_deviceMusicIndex;
    quint64 m_musicIdentityRevision = 0;
    QVariantMap m_photoOnDeviceKeys;
    void rebuildOnDeviceKeys();
    QString m_connectionStage;
    double m_connectionProgress = 0;
    QString m_connectionError;
    QColor m_moodColor; // invalid/transparent when disconnected
    bool m_awaitingDisconnect = false;
    bool m_ejecting = false;
    // Post-re-index reconnect ladder (see scheduleReprobeAfterReindex).
    QTimer m_reprobeTimer;
    int m_reprobeAttempt = 0;
    bool m_reprobePending = false;
    // Track item → album object holding its art (see DeviceWorker).
    QHash<quint32, quint32> m_trackToAlbumArt;
    QHash<quint32, QString> m_trackToAlbumArtist;
    // Album object fetch in flight → every id that asked for it.
    QHash<quint32, QList<quint32>> m_artAliases;
    // Album object → local {artist,album} cache identities waiting for art.
    QHash<quint32, QVariantList> m_importedAlbumArtTargets;
    TrackImportBatch m_trackImportBatch;
    QSet<QString> m_pendingTrackDestinations;
};
