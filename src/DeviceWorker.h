#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>
#include <QVariantList>

#include <atomic>
#include <memory>

#include "TrackModel.h"

struct ZuneDevice;

namespace Wpd {
class Session;
}

// Everything libzune, on one thread — the C++ equivalent of ZuunedMac's
// `actor MTPService`. The worker OWNS the ZuneDevice handle; the UI-thread
// DeviceService only ever talks to it through queued slots/signals, so
// device operations are serialized by construction: breach, scan, art
// grabs, and (later) the sync queue can never race each other on the USB
// pipe.
class DeviceWorker : public QObject {
    Q_OBJECT

public:
    // Both halves out of line.  m_wpd owns a Wpd::Session and this header only
    // forward declares it, and MSVC needs to be able to destroy that member in
    // whichever translation unit builds either the constructor's cleanup path or
    // the destructor - so neither of them may be generated here.
    DeviceWorker();
    ~DeviceWorker() override;

    struct BreachData {
        bool ok = false;
        QString error;
        QString name, model, serial;
        // ZuneDeviceFamily from MTP 0xD21A (0x00=Keel/Zune30, 0x02=Scorpius,
        // 0x03=Draco, 0x06=Pavo/HD, 0xFF=unknown). Drives the video
        // profile auto-detect — the model STRING is just "Zune".
        int family = 0xFF;
        int battery = 0;
        double capacityGB = 0, freeGB = 0;
        QVector<TrackRow> tracks;
        int albumCount = 0, artistCount = 0;
        QVariantList artists, albums, genres;
        // Which road this player came in on: "libzune" for the vendor ZMDB path,
        // "wpd" for Windows' own MTP driver.  Not a cosmetic label - most of what
        // the device page offers exists only on the first one, and the UI has to
        // be able to say which half the user is looking at.
        QString backend;
        QVariantList videos, photos, playlists;
        // Device photo folder-albums: [{itemId, name}] (ZMDB scan)
        QVariantList photoAlbums;
        // Art lives on MTP ALBUM objects, not tracks (the Zune returns
        // "no representative sample" for every track). Map track item →
        // its album object so any art request lands on the right object.
        QHash<quint32, quint32> trackToAlbumArt;
        // Album-object Artist is the best available album owner. Keep it
        // beside the art mapping so imported compilation art uses the same
        // local cache identity as the extracted track metadata when possible.
        QHash<quint32, QString> trackToAlbumArtist;
    };

    // Called from the C progress-callback thunk (worker thread) —
    // signals are protected, so the thunk routes through this.
    void notifySendProgress(const QString &entryId, double frac) {
        emit sendProgress(entryId, frac);
    }

    // Cross-thread abort: zune_abort only flips a volatile flag on the
    // device struct, safe to call from ANY thread (SyncEngine/UI) while
    // the worker is mid-transfer. Everything else on this class is
    // queued-slot only.
    void requestAbort();
    // Invalidate queued spoken reads/deletes when the facade loses its device.
    void setSpokenGeneration(quint64 generation) { m_spokenGeneration.store(generation); }

public slots:
    void doBreach();
    void doReadSpokenMedia(quint64 generation, quint64 request, const QString &serial);
    void doPurgeSpokenItems(quint64 generation, quint64 request, const QString &serial,
                            const QString &kind, const QVariantList &rows);
    void doSever();
    // Fetch art for an item into cachePath (skips USB if file exists).
    void doGrabArt(quint32 itemId, const QString &cachePath);
    // Photo thumbnail (representative sample → thumbnail → download+resize).
    void doGrabPhotoThumb(quint32 itemId, const QString &cachePath);

    // ── Sync (Phase 5) ──
    // Send one audio file (already MP3/WMA — transcode happens on the
    // engine's pool BEFORE this). artist arg on the wire = albumartist
    // (mac rule). Emits sendProgress during transfer, then sendDone.
    void doInspectAudiobooks(const QString &token, const QString &serial, const QVariantList &parts);
    void doSendAudiobook(const QString &entryId, const QString &filepath, const QVariantMap &part);
    void doInspectPodcastCleanup(quint64 generation, const QString &token, const QString &serial,
                                 const QVariantList &episodes);
    void doPurgePlayedPodcasts(quint64 generation, const QString &token, const QString &serial,
                               const QVariantList &rows);
    void doInspectPodcasts(const QString &token, const QString &serial, const QVariantList &episodes);
    void doSendPodcast(const QString &entryId, const QString &filepath, const QVariantMap &episode);
    void doSendTrack(const QString &entryId, const QString &filepath,
                     const QString &title, const QString &albumartist,
                     const QString &album, const QString &genre,
                     int trackNumber, int durationMs);
    // ── Sync (Phase 6) ──
    // Send one already-transcoded video. Routing (mac MTPService rule):
    // series+episode → smuggle_episode (vendor props 0xDA9A/0xDAB5/0xDAB6),
    // else metagenre 0x23 → clip, 0x21 → other, default → movie.
    // posterPath: local JPEG sent as representative sample ("" = none).
    void doSendVideo(const QString &entryId, const QString &filepath,
                     const QString &objectFilename, const QString &title, int metagenre,
                     const QString &description, const QString &posterPath,
                     const QString &series, int season, int episode);
    // Mac sync phase 2 — buckets: {albumartist, artist, album, genre,
    // trackIds: QVariantList<uint>, artJpegPath: QString?}. Forges
    // artists, merges/creates album objects, links artist to album AND
    // to each track, brands album art.
    void doForgeAlbums(const QVariantList &buckets);
    // ── Phase 6: device video browsing ──
    // Download one video off the device (Save to Library). Serialized
    // with everything else on this thread — saves queue behind syncs.
    void doExtractVideo(quint32 itemId, const QString &destPath);
    // ── Phase 8: photos both directions ──
    // Send one photo (arm/resize happens inside libzune) into a named
    // device album ("" = root).
    void doSendPhoto(const QString &entryId, const QString &filepath,
                     const QString &albumName);
    // Download one photo / one track off the device (Save to Library +
    // the gallery's full-res resolver).
    void doExtractPhoto(quint32 itemId, const QString &destPath);
    void doExtractTrack(quint32 itemId, const QString &destPath);
    // ── Phase 9: playlists ──
    // Create a playlist on the device (0x9808 path — survives
    // re-index, Keel+Pavo verified). trackIds are DEVICE item ids.
    void doForgePlaylist(const QString &name, const QVariantList &trackIds);
    // Rename an item's display Name (0xDC44) — Zune HD test feature.
    void doRenameItem(quint32 itemId, const QString &newName);
    // Rename the DEVICE (Friendly Name prop 0xD402) — "rename zune".
    void doRenameDevice(const QString &newName);
    // 0x922A pre-transfer notification: item name + batch progress for
    // the device's own sync display. Queued right before each send on
    // this thread, so ordering is inherent. Best-effort.
    void doSyncNotify(const QString &name, int itemIndex, int totalItems);
    // Re-read capacity/free from the DEVICE (GetStorageInfo) — the
    // ground truth after purges, when local accounting has gone stale.
    void doRefreshStorage();
    // Vendor finalize (CleanDataStore → device re-index).
    void doFinalize();
    // Delete objects (tracks/albums/photos — MTP DeleteObject) by id.
    void doPurgeItems(const QVariantList &itemIds);
    void doPurgeInterruptedVideo(const QString &filename);
    void doPurgeInterruptedObject(const QVariantMap &record);

signals:
    void breachDone(const DeviceWorker::BreachData &data);
    void spokenMediaRead(quint64 generation, quint64 request, const QString &serial,
                        const QVariantList &podcasts, const QVariantList &books,
                        const QString &error);
    void spokenItemsPurged(quint64 generation, quint64 request, const QString &serial,
                           const QVariantList &purgedIds, int failCount,
                           const QString &error);
    void spokenPurgeProgress(quint64 generation, quint64 request, const QString &serial,
                             int current, int total);
    void severDone();
    void artDone(quint32 itemId, const QString &path, bool ok);
    // Staged connection progress ("MTPZ authentication…", 0.4) for the
    // panel's connection bar — mirrors mac connectionStage/Progress.
    void stage(const QString &label, double progress);

    // ── Sync (Phase 5) ──
    void sendProgress(const QString &entryId, double frac);
    void audiobooksInspected(const QString &token, const QString &serial, const QVariantList &parts, const QString &error);
    void audiobookSendDone(const QString &entryId, bool ok, quint32 itemId, const QString &error, bool reused);
    void podcastCleanupInspected(quint64 generation, const QString &token, const QString &serial,
                                 const QVariantList &rows, const QString &error);
    void podcastCleanupProgress(quint64 generation, const QString &token, const QString &serial,
                                quint32 itemId, bool removed);
    void playedPodcastsPurged(quint64 generation, const QString &token, const QString &serial,
                              const QVariantList &purgedRows, const QString &error);
    void podcastsInspected(const QString &token, const QString &serial, const QVariantList &episodes, const QString &error);
    // recoveryRequired may be false only after explicit confirmed cleanup.
    void podcastSendDone(const QString &entryId, bool ok, quint32 itemId, const QString &error, bool reused, bool recoveryRequired = true);
    void sendDone(const QString &entryId, bool ok, quint32 itemId,
                  const QString &autopsyName);
    void sendVideoDone(const QString &entryId, bool ok, quint32 itemId,
                       const QString &autopsyName);
    void extractVideoDone(quint32 itemId, const QString &destPath, bool ok);
    void sendPhotoDone(const QString &entryId, bool ok, quint32 itemId,
                       const QString &autopsyName);
    void extractPhotoDone(quint32 itemId, const QString &destPath, bool ok);
    void extractTrackDone(quint32 itemId, const QString &destPath, bool ok);
    void renameItemDone(quint32 itemId, const QString &newName, bool ok);
    void forgePlaylistDone(const QString &name, quint32 playlistId, bool ok);
    void renameDeviceDone(const QString &newName, bool ok);
    void storageRefreshed(double capacityGB, double freeGB);
    void forgeProgress(int current, int total);
    void forgeDone(int okCount, int failCount);
    void finalizeDone(bool ok);
    void purgeProgress(int current, int total);
    void purgeDone(int okCount, int failCount, const QVariantList &purgedIds);
    void interruptedVideoPurged(const QString &filename, bool ok);
    void interruptedObjectPurged(const QString &token, bool ok);

private:
    friend struct PodcastCleanupWorkerTestAccess;
    ZuneDevice *m_dev = nullptr;
    std::atomic<quint64> m_spokenGeneration{1};
    // Mirror of m_dev for the cross-thread requestAbort() read.
    std::atomic<ZuneDevice *> m_devShared{nullptr};

#if defined(Q_OS_WIN)
    // A Zune that Windows already owns - bound to Windows' own WPD/MTP driver
    // rather than to WinUSB - is reachable only through Windows.  This backend is
    // Windows-only by construction; everywhere else the player has one road.
    //
    // The session is allocated once and never freed while the worker lives, so
    // requestAbort() on the UI thread only reads a pointer that outlives it.
    // m_wpdShared says whether that session is open right now and is cleared
    // before the session closes - the handshake m_devShared already uses.
    bool breachWpd(BreachData *r);
    QString wpdObjectIdFor(quint32 itemId) const;
    // One file onto the player, into <artist>/<album>/ under the storage this
    // connection picked.  Emits sendDone itself because the failure text differs
    // from libzune's autopsy names: there is no vendor autopsy on this road.
    void sendTrackWpd(const QString &entryId, const QString &filepath,
                      const QString &title, const QString &albumartist,
                      const QString &album, const QString &genre,
                      int trackNumber, int durationMs);
    // Downloads one object and reports the path it really wrote.  A WPD object's
    // format property is the only place this player states the container it holds -
    // the name it displays is a title, and a bare name reaches us as `.mp3` - so the
    // extension is settled here, where the format is actually in hand.
    bool downloadWpd(quint32 itemId, const QString &destPath, QString *written,
                     QString *error);
    // Resolve one folder under a parent, creating it only when the parent's own
    // listing says it is not there.  Cached for the connection: a 200-track sync
    // otherwise re-enumerates the same artist folder 200 times.
    bool ensureWpdFolder(const QString &parentId, const QString &name,
                         QString *objectId, QString *error);
    QString m_wpdStorage;            // storage a send writes to
    QHash<QString, QString> m_wpdFolders;   // "parent/name" -> folder object id
    std::unique_ptr<Wpd::Session> m_wpd;
    std::atomic<bool> m_wpdShared{false};
    // The object ids this player really used, keyed by the quint32 handle the
    // rest of this class speaks.  A WPD id is the MTP handle behind a one-letter
    // namespace, but it is the provider's own string, so the string that worked
    // is remembered rather than rebuilt from a rule.
    QHash<quint32, QString> m_wpdIds;
    // One line per connection is enough to explain why the device page has no
    // pictures on this road; a per-request line would be noise.
    bool m_wpdArtReported = false;
    // Says out loud that a request needs a vendor operation this road does not
    // have.  Every caller of it still emits its failure signal: the app reports
    // "not possible over MTP" rather than pretending a ZMDB-only operation ran.
    static void noteWpdUnsupported(const char *feature);
#endif
};

Q_DECLARE_METATYPE(DeviceWorker::BreachData)
