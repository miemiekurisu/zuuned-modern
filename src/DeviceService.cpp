#include "DeviceService.h"
#include "ProfilePaths.h"
#include "ThreadDrain.h"
#include "TrackImportPath.h"
#include "DevicePlaylistState.h"
#include "UsbWatcher.h"
#include "UdevSetup.h"
#include "LibraryService.h"
#include "library/LibraryPaths.h"
#include "library/MusicIdentity.h"
#include "library/VideoIdentity.h"
#include "sync/RecoveryIdentity.h"
#include "UiText.h"

#include <QDir>
#include <QProcess>
#include <QFile>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

DeviceService::DeviceService(QObject *parent) : DeviceService(parent, true) {}

DeviceService::DeviceService(QObject *parent, bool watchDevice) : QObject(parent) {
    m_spokenArtSession = QUuid::createUuid().toString(QUuid::Id128);
    m_artCacheDir = Zuuned::ProfilePaths::writableLocation(QStandardPaths::CacheLocation)
                    + QStringLiteral("/art");
    QDir().mkpath(m_artCacheDir);

    // Every player this machine has talked to.  It has to exist before the
    // first connection: onBreachDone() records the player it just opened, and
    // with nothing here that was a null dereference on connect - the crash an
    // attached Zune produced about fifteen seconds after launch - and
    // Settings -> Device read properties off a null deviceHistory.
    m_deviceHistory = new DeviceHistory(this);

    m_worker = new DeviceWorker;
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    // UI → worker (queued across threads)
    connect(this, &DeviceService::workerBreach, m_worker, &DeviceWorker::doBreach);
    connect(this, &DeviceService::workerSever, m_worker, &DeviceWorker::doSever);
    connect(this, &DeviceService::workerGrabArt, m_worker, &DeviceWorker::doGrabArt);
    connect(this, &DeviceService::workerGrabPhotoThumb, m_worker, &DeviceWorker::doGrabPhotoThumb);
    connect(this, &DeviceService::workerReadSpokenMedia, m_worker, &DeviceWorker::doReadSpokenMedia);
    connect(this, &DeviceService::workerPurgeSpokenItems, m_worker, &DeviceWorker::doPurgeSpokenItems);
    connect(m_worker, &DeviceWorker::spokenPurgeProgress, this,
            [this](quint64 generation, quint64 request, const QString &serial, int current, int total) {
        if (generation != m_spokenGeneration || request != m_spokenRequest || !m_connected || serial != m_serial)
            return;
        m_purgeCurrent = current;
        m_purgeTotal = total;
        m_status = QStringLiteral("Deleting %1 of %2…").arg(current).arg(total);
        emit purgeChanged();
        emit stateChanged();
    });
    connect(m_worker, &DeviceWorker::spokenMediaRead, this,
            [this](quint64 generation, quint64 request, const QString &serial, const QVariantList &podcasts,
                   const QVariantList &books, const QString &error) {
        if (generation != m_spokenGeneration || request != m_spokenRequest || !m_connected || serial != m_serial)
            return;
        m_spokenMediaLoading = false;
        m_spokenMediaError = error;
        m_podcastDeviceStatus.clear();
        m_podcastMembershipKnown = false;
        m_podcastStatusWarning.clear();
        if (error.isEmpty()) {
            // Device IDs can be recycled after deletion. Each successful
            // inventory gets a fresh spoken-art namespace, including disk cache.
            ++m_spokenArtRevision;
            for (quint32 id : std::as_const(m_spokenArtIds)) {
                m_artPaths.remove(QString::number(id));
                m_artRequested.remove(id);
                m_artAliases.remove(id);
            }
            m_podcastEpisodesList = podcasts;
            m_audiobookPartsList = books;
            rebuildPodcastDeviceStatus();
            if (audiobookBrowseSupported())
                fprintf(stderr, "[device] spoken inventory: %lld podcast episodes, %lld audiobook parts\n",
                        static_cast<long long>(podcasts.size()), static_cast<long long>(books.size()));
            else
                fprintf(stderr, "[device] spoken inventory: %lld podcast episodes; audiobook inventory unsupported on family 0x%02x\n",
                        static_cast<long long>(podcasts.size()), m_family);
            m_spokenArtIds.clear();
            for (const auto &list : {podcasts, books})
                for (const QVariant &value : list)
                    m_spokenArtIds.insert(value.toMap().value(QStringLiteral("artItemId")).toUInt());
            for (quint32 id : std::as_const(m_spokenArtIds)) {
                m_artPaths.remove(QString::number(id));
                m_artRequested.remove(id);
                m_artAliases.remove(id);
            }
        }
        emit spokenMediaChanged();
        if (error.isEmpty()) emit artChanged();
        emit stateChanged();
    });
    connect(m_worker, &DeviceWorker::spokenItemsPurged, this,
            [this](quint64 generation, quint64 request, const QString &serial, const QVariantList &ids,
                   int failCount, const QString &error) {
        if (generation != m_spokenGeneration || request != m_spokenRequest || !m_connected || serial != m_serial)
            return;
        m_spokenMediaError = error;
        onPurgeDone(ids.size(), failCount, ids);
    });

    // worker → UI
    connect(m_worker, &DeviceWorker::breachDone, this, &DeviceService::onBreachDone);
    connect(m_worker, &DeviceWorker::severDone, this, &DeviceService::onSeverDone);
    connect(m_worker, &DeviceWorker::artDone, this, &DeviceService::onArtDone);
    connect(m_worker, &DeviceWorker::stage, this, &DeviceService::onWorkerStage);
    connect(this, &DeviceService::workerFinalize, m_worker, &DeviceWorker::doFinalize);
    connect(this, &DeviceService::workerPurgeItems, m_worker, &DeviceWorker::doPurgeItems);
    connect(this, &DeviceService::workerPurgeInterruptedVideo, m_worker, &DeviceWorker::doPurgeInterruptedVideo);
    connect(m_worker, &DeviceWorker::interruptedVideoPurged, this, &DeviceService::interruptedVideoPurged);
    connect(this, &DeviceService::workerPurgeInterruptedObject, m_worker, &DeviceWorker::doPurgeInterruptedObject);
    connect(m_worker, &DeviceWorker::interruptedObjectPurged, this, &DeviceService::interruptedObjectPurged);
    connect(this, &DeviceService::workerExtractVideo, m_worker, &DeviceWorker::doExtractVideo);
    connect(m_worker, &DeviceWorker::extractVideoDone, this,
            [this](quint32, const QString &destPath, bool ok) {
        emit videoSaved(QFileInfo(destPath).fileName(), ok);
        notePullDone(ok);
    });
    connect(this, &DeviceService::workerExtractPhoto, m_worker, &DeviceWorker::doExtractPhoto);
    connect(this, &DeviceService::workerExtractTrack, m_worker, &DeviceWorker::doExtractTrack);
    connect(m_worker, &DeviceWorker::extractPhotoDone, this,
            [this](quint32 itemId, const QString &destPath, bool ok) {
        // Cache-dir extracts serve the gallery resolver; imports-dir
        // extracts are Save to Library.
        if (destPath.startsWith(m_artCacheDir)) {
            m_photoFullPending.remove(itemId);
            if (ok)
                emit photoFullReady(itemId,
                                    QUrl::fromLocalFile(destPath).toString());
        } else {
            emit photoSaved(QFileInfo(destPath).fileName(), ok);
            notePullDone(ok);   // imports-dir extract = a Save to Library
        }
    });
    connect(m_worker, &DeviceWorker::extractTrackDone, this,
            [this](quint32 itemId, const QString &destPath, bool ok) {
        m_pendingTrackDestinations.remove(TrackImportPath::key(destPath));
        emit trackSaved(QFileInfo(destPath).fileName(), ok);
        const auto batch = m_trackImportBatch.finished(itemId, ok);
        if (batch && batch->hasSuccessfulTrack) {
            for (const auto &album : batch->albums)
                requestImportedAlbumArt(album.itemId, album.artist,
                                        album.title);
            emit musicImportsReady(batch->artists);
        }
        notePullDone(ok);
    });
    connect(this, &DeviceService::workerForgePlaylist,
            m_worker, &DeviceWorker::doForgePlaylist);
    connect(m_worker, &DeviceWorker::forgePlaylistDone, this,
            [this](const QString &name, quint32 playlistId, bool ok) {
        if (ok) {
            m_playlistsList.append(QVariantMap{{"itemId", playlistId},
                                               {"name", name},
                                               {"count", 0},
                                               {"trackIds", QVariantList{}}});
            emit stateChanged();
        }
        emit playlistCreated(name, ok);
    });
    connect(this, &DeviceService::workerRenameItem, m_worker, &DeviceWorker::doRenameItem);
    connect(this, &DeviceService::workerRenameDevice, m_worker, &DeviceWorker::doRenameDevice);
    connect(this, &DeviceService::workerRefreshStorage, m_worker, &DeviceWorker::doRefreshStorage);
    connect(m_worker, &DeviceWorker::storageRefreshed, this,
            [this](double capacityGB, double freeGB) {
        m_capacityGB = capacityGB;
        m_freeGB = freeGB;
        emit stateChanged();
    });
    connect(m_worker, &DeviceWorker::renameDeviceDone, this,
            [this](const QString &newName, bool ok) {
        if (ok) {
            m_name = newName;
            emit stateChanged();
        }
        emit renameFinished(0, newName, ok);
    });
    connect(m_worker, &DeviceWorker::renameItemDone, this,
            [this](quint32 itemId, const QString &newName, bool ok) {
        if (ok) {
            // Mirror into the browse lists so the page updates without
            // a re-breach. (The Name prop is what the device shows;
            // our lists carry the filename — for videos the rename is
            // the more meaningful display, so adopt it.)
            for (int i = 0; i < m_videosList.size(); i++) {
                QVariantMap e = m_videosList[i].toMap();
                if (e.value(QStringLiteral("itemId")).toUInt() == itemId) {
                    e[QStringLiteral("name")] = newName;
                    e[QStringLiteral("title")] = newName;
                    if (e.contains(QStringLiteral("episodeTitle")))
                        e[QStringLiteral("episodeTitle")] = newName;
                    m_videosList[i] = e;
                    break;
                }
            }
            emit stateChanged();
        }
        emit renameFinished(itemId, newName, ok);
    });
    connect(m_worker, &DeviceWorker::purgeProgress, this,
            [this](int current, int total) {
        m_purgeCurrent = current;
        m_purgeTotal = total;
        m_status = QStringLiteral("Deleting %1 of %2…").arg(current).arg(total);
        emit purgeChanged();
        emit stateChanged();
    });
    connect(m_worker, &DeviceWorker::purgeDone, this, &DeviceService::onPurgeDone);
    connect(m_worker, &DeviceWorker::finalizeDone, this, &DeviceService::onFinalizeDone);
    m_reprobeTimer.setSingleShot(true);
    connect(&m_reprobeTimer, &QTimer::timeout, this, &DeviceService::reprobeNow);

    const auto retrySpokenRead = [this] {
        if (!m_spokenReadDeferred || !m_connected || m_spokenMediaLoading || m_busy || m_purging
            || m_pulling || m_syncActive || m_ejecting || m_awaitingDisconnect) return;
        m_spokenReadDeferred = false;
        QTimer::singleShot(0, this, &DeviceService::refreshSpokenMedia);
    };
    connect(this, &DeviceService::stateChanged, this, retrySpokenRead);
    connect(this, &DeviceService::purgeChanged, this, retrySpokenRead);
    connect(this, &DeviceService::pullChanged, this, retrySpokenRead);
    m_workerThread.start();

    if (watchDevice) {
        m_watcher = new UsbWatcher(this);
        connect(m_watcher, &UsbWatcher::zuneArrived, this, &DeviceService::onZuneArrived);
        connect(m_watcher, &UsbWatcher::zuneLeft, this, &DeviceService::onZuneLeft);
        connect(m_watcher, &UsbWatcher::zuneBlocked, this, &DeviceService::onZuneBlocked);
        connect(m_watcher, &UsbWatcher::zuneUnblocked, this, &DeviceService::onZuneUnblocked);
        m_watcher->start();
    }
}

DeviceService::~DeviceService() {
    if (m_watcher) m_watcher->stop();
    if (m_connected)
        emit workerSever();
    Zuuned::drainThread(m_workerThread, "device");
}

void DeviceService::onZuneArrived() {
    m_devicePresent = true;
    // A real arrival owns the reconnect.  A ladder left running by a finished
    // sync must not race it for the same device.
    m_reprobeTimer.stop();
    m_reprobeAttempt = 0;
    m_reprobePending = false;
    // libusb can open it, which is the end of any access problem this platform
    // was reporting.
    if (!m_usbBlockDetail.isEmpty() || m_blockedPresence) {
        m_usbBlockDetail.clear();
        m_blockedPresence = false;
        emit usbAccessChanged();
    }
    if (m_awaitingDisconnect) {
        // Post-sync: the device may be re-indexing — do NOT breach into
        // it (mac connectDevice guards on awaitingDisconnect).
        m_status = UiText::tr("Sync complete — unplug the Zune to apply changes");
        emit stateChanged();
        return;
    }
    if (m_connected || m_busy) {
        emit stateChanged();
        return;
    }
    m_status = QStringLiteral("Zune detected…");
    emit stateChanged();
    // Let enumeration/udev settle before opening — a breach in the first
    // moments after plug-in races the device's own MTP bring-up.
    QTimer::singleShot(1500, this, [this] {
        if (m_devicePresent && !m_connected && !m_busy)
            breach();
    });
}

void DeviceService::onZuneLeft() {
    m_devicePresent = false;
    m_awaitingDisconnect = false;  // physical unplug completes the cycle
    m_reprobeTimer.stop();
    m_reprobeAttempt = 0;
    m_reprobePending = false;
    if (m_connected) {
        // Device is gone — sever frees state; CloseSession fails fast
        // (NO_DEVICE), which is expected and logged, not an error.
        m_connected = false;
        emit workerSever();
    }
    clearDeviceState();
    m_status = UiText::tr("Zune unplugged");
    emit stateChanged();
}

void DeviceService::onZuneBlocked(const QString &detail) {
    const QString next = detail.isEmpty()
                             ? UiText::tr("plugged in, but unusable")
                             : detail;
    const bool changed = next != m_usbBlockDetail;
    m_usbBlockDetail = next;
    if (m_connected || m_busy || m_awaitingDisconnect) {
        // A transfer is live or the device is already ours: whatever Windows said,
        // do not tear down a working connection over a devnode opinion.
        if (changed)
            emit usbAccessChanged();
        return;
    }
    // Presence is honest here - the device IS on the bus - so the fix-it card can
    // appear.  What must not happen is a breach: the watcher only reaches this
    // path when libusb cannot even enumerate the thing, so there is nothing to
    // open and a retry loop would just be noise.
    m_blockedPresence = true;
    m_devicePresent = true;
    m_status = UiText::tr("Zune plugged in, but no driver zuuned can use");
    emit stateChanged();
    if (changed)
        emit usbAccessChanged();
}

void DeviceService::onZuneUnblocked() {
    if (m_usbBlockDetail.isEmpty() && !m_blockedPresence)
        return;
    m_usbBlockDetail.clear();
    const bool onlyBlockedPresence = m_blockedPresence;
    m_blockedPresence = false;
    emit usbAccessChanged();
    if (!onlyBlockedPresence || m_connected || m_busy)
        return;
    // The blocked sighting is gone and libusb never had the device, so the
    // truthful reading is that nothing usable is plugged in.  If Windows handed it
    // to a real driver instead, zuneArrived() follows in a moment and puts
    // presence back - the wording below holds either way.
    m_devicePresent = false;
    m_awaitingDisconnect = false;
    m_status = UiText::tr("Waiting for a Zune…");
    emit stateChanged();
}

void DeviceService::clearDeviceState() {
    ++m_spokenGeneration;
    m_worker->setSpokenGeneration(m_spokenGeneration);
    m_podcastEpisodesList.clear();
    m_podcastDeviceStatus.clear();
    m_podcastMembershipKnown = false;
    m_podcastStatusWarning.clear();
    m_spokenRequested = false;
    m_spokenReadDeferred = false;
    m_audiobookPartsList.clear();
    m_spokenArtIds.clear();
    m_spokenMediaLoading = false;
    m_spokenMediaError.clear();
    m_purging = false;
    m_purgeCurrent = m_purgeTotal = 0;
    m_artPaths.clear();
    emit spokenMediaChanged();
    emit purgeChanged();
    emit artChanged();
    m_moodColor = QColor();
    m_connectionStage.clear();
    m_connectionProgress = 0;
    m_name.clear();
    m_model.clear();
    m_battery = 0;
    m_capacityGB = m_freeGB = 0;
    m_musicGB = m_videoGB = m_photoGB = 0;
    m_trackCount = m_albumCount = m_artistCount = 0;
    m_artistsList.clear();
    m_albumsList.clear();
    m_genresList.clear();
    m_videosList.clear();
    m_photosList.clear();
    m_playlistsList.clear();
    m_photoAlbumsList.clear();
    m_photoFullPending.clear();
    m_artRequested.clear();
    m_artAliases.clear();
    m_importedAlbumArtTargets.clear();
    m_trackToAlbumArt.clear();
    m_trackToAlbumArtist.clear();
    m_tracks.setRows({});
}

void DeviceService::breach() {
    if (m_busy || m_connected)
        return;
    m_busy = true;
    m_status = UiText::tr("Connecting (breach + MTPZ auth)…");
    emit stateChanged();
    emit workerBreach();
}

void DeviceService::onWorkerStage(const QString &label, double progress) {
    m_connectionStage = label;
    m_connectionProgress = progress;
    emit stateChanged();
}

// Port of ZuneColors.genreColor + deviceMoodColor: bias color by the
// device's dominant genre, 75% genre pick / 25% random surprise. The
// palette deliberately excludes brand pink/orange (the device is a
// visitor).
QColor DeviceService::pickMoodColor(const QVariantList &genres) {
    static const QColor palette[] = {
        QColor::fromRgbF(0.20, 0.50, 1.00), // electric blue
        QColor::fromRgbF(0.45, 0.95, 0.30), // acid green
        QColor::fromRgbF(0.55, 0.20, 0.90), // deep purple
        QColor::fromRgbF(0.95, 0.15, 0.20), // hot red
        QColor::fromRgbF(0.10, 0.85, 0.85), // cyan
        QColor::fromRgbF(0.95, 0.78, 0.20), // gold
        QColor::fromRgbF(0.95, 0.40, 0.70), // electric pink
    };
    const int n = int(std::size(palette));
    const QColor random = palette[QRandomGenerator::global()->bounded(n)];

    QString dominant;
    int best = -1;
    for (const QVariant &v : genres) {
        const QVariantMap m = v.toMap();
        if (m.value("count").toInt() > best) {
            best = m.value("count").toInt();
            dominant = m.value("name").toString();
        }
    }
    const QString g = dominant.toLower();
    QColor bias = palette[QRandomGenerator::global()->bounded(n)];
    if (g.contains("rock") || g.contains("metal") || g.contains("punk")
        || g.contains("hardcore") || g.contains("classical")
        || g.contains("jazz") || g.contains("blues") || g.contains("soul"))
        bias = palette[2]; // deep purple
    else if (g.contains("pop"))
        bias = palette[6]; // electric pink
    else if (g.contains("electro") || g.contains("edm") || g.contains("dance")
             || g.contains("house") || g.contains("techno"))
        bias = palette[4]; // cyan
    else if (g.contains("hip") || g.contains("hop") || g.contains("rap")
             || g.contains("r&b") || g.contains("rnb"))
        bias = palette[5]; // gold
    else if (g.contains("indie") || g.contains("alternative") || g.contains("folk"))
        bias = palette[1]; // acid green
    else if (g.contains("soundtrack") || g.contains("score") || g.contains("game"))
        bias = palette[0]; // electric blue

    const bool useGenre = QRandomGenerator::global()->bounded(100) < 75;
    return useGenre ? bias : random;
}

void DeviceService::onBreachDone(const DeviceWorker::BreachData &r) {
    const bool wasReprobe = m_reprobePending;
    m_reprobePending = false;
    m_busy = false;
    m_connectionStage.clear();
    m_connectionProgress = r.ok ? 1.0 : 0.0;
    if (!r.ok) {
        m_status = r.error;
        m_connectionError = r.error;
        // A re-indexing player answers late rather than never, so the ladder
        // tries again.  Only its own attempt does: a manual connect that
        // failed stays failed, because repeating it would be a retry loop on
        // a device the user can see is not answering.
        if (wasReprobe) {
            m_reprobeTimer.stop();
            scheduleReprobeAfterReindex();
        }
    } else {
        m_reprobeAttempt = 0;
        m_connectionError.clear();
        m_connected = true;
        m_name = r.name;
        m_model = r.model;
        m_serial = r.serial;
        m_family = r.family;
        m_battery = r.battery;
        m_capacityGB = r.capacityGB;
        m_freeGB = r.freeGB;
        m_trackCount = r.tracks.size();
        m_albumCount = r.albumCount;
        m_artistCount = r.artistCount;
        // Remember the player, not just this session.  A serial is the only
        // thing that tells two players of the same model apart, and libzune
        // answers no firmware version, so a row carries no firmware field.
        m_deviceHistory->record(m_serial, m_name, modelName(), m_family,
                               m_capacityGB);
        m_artistsList = r.artists;
        m_albumsList = r.albums;
        m_genresList = r.genres;
        m_videosList = r.videos;
        m_photosList = r.photos;
        m_photoAlbumsList = r.photoAlbums;
        m_playlistsList = r.playlists;
        m_trackToAlbumArt = r.trackToAlbumArt;
        m_trackToAlbumArtist = r.trackToAlbumArtist;
        m_tracks.setRows(r.tracks);
        // Storage breakdown for the stats tab — HD (Pavo) ONLY. Every
        // other generation's ZMDB carries no sizes at these offsets
        // (B1, verified on the 120's dump: track +16 is DURATION,
        // photo +12 is a FILETIME — summing those "sizes" produced
        // petabytes of phantom photos). Classic families show counts.
        // TODO(B1): batched GetObjectPropList ObjectSize sweep would
        // light real sizes up on every family.
        if (r.family == 0x06) {
            qint64 musicBytes = 0;
            for (const TrackRow &t : r.tracks)
                musicBytes += t.filesize;
            double videoMB = 0, photoMB = 0;
            for (const QVariant &v : std::as_const(m_videosList))
                videoMB += v.toMap().value(QStringLiteral("sizeMB")).toDouble();
            for (const QVariant &v : std::as_const(m_photosList))
                photoMB += v.toMap().value(QStringLiteral("sizeMB")).toDouble();
            m_musicGB = musicBytes / 1e9;
            m_videoGB = videoMB / 1e3;
            m_photoGB = photoMB / 1e3;
            fprintf(stderr, "[device] storage: %.1f/%.1f GB (used/total), "
                    "attributed: music %.2f + video %.2f + photo %.2f GB\n",
                    m_capacityGB - m_freeGB, m_capacityGB,
                    m_musicGB, m_videoGB, m_photoGB);
        } else {
            m_musicGB = m_videoGB = m_photoGB = 0;
            fprintf(stderr, "[device] storage: %.1f/%.1f GB (used/total) — "
                    "family 0x%02x ZMDB carries no per-item sizes; "
                    "stats show counts\n",
                    m_capacityGB - m_freeGB, m_capacityGB, r.family);
        }
        rebuildOnDeviceKeys();
        m_moodColor = pickMoodColor(r.genres);
        m_status = QStringLiteral("Connected");
    }
    emit stateChanged();
}

void DeviceService::sever() {
    if (m_busy || !m_connected)
        return;
    m_connected = false;
    clearDeviceState();
    m_status = m_devicePresent ? UiText::tr("Disconnected (Zune still plugged in)")
                               : UiText::tr("Waiting for a Zune…");
    emit stateChanged();
    emit workerSever();
}

void DeviceService::onSeverDone() {
    if (m_ejecting) {
        m_ejecting = false;
        m_awaitingDisconnect = false;  // auto-detect may reconnect now (mac)
        m_status = UiText::tr("Disconnected — the Zune is re-indexing");
        emit stateChanged();
        // On Linux the re-index re-enumerates the player and zuneArrived()
        // does the rest.  A Zune on Windows keeps its WinUSB interface the
        // whole time, so nothing ever arrives and this panel would sit here
        // for good until the user noticed the connect button.
        if (m_devicePresent && !m_connected && !m_busy)
            scheduleReprobeAfterReindex();
    }
    // Otherwise state was already cleared on request; nothing further.
}

void DeviceService::setAwaitingDisconnect(bool v) {
    if (m_awaitingDisconnect == v)
        return;
    m_awaitingDisconnect = v;
    if (v)
        m_status = UiText::tr("Sync complete — eject to apply changes");
    emit stateChanged();
}

// A player that re-indexes without leaving the bus produces no arrival, so the
// finalize chain asks again itself.  The gaps grow because each attempt IS the
// readiness probe — an OpenSession a Zune may take a while to answer - and
// because a breach already auto-resets a wedged session once, hammering it every
// second would trade one wedge for another.  When the ladder runs out the panel
// keeps the truth and `connect` stays where the user left it.
void DeviceService::scheduleReprobeAfterReindex() {
    static const int kDelaysMs[] = {6000, 12000, 20000, 30000, 45000, 60000};
    const int budget = int(sizeof(kDelaysMs) / sizeof(kDelaysMs[0]));
    if (m_reprobeAttempt >= budget) {
        fprintf(stderr, "[device] re-index reconnect ladder exhausted - connect manually\n");
        return;
    }
    const int delay = kDelaysMs[m_reprobeAttempt];
    m_status = UiText::tr("Disconnected — the Zune is re-indexing, reconnecting in %1s")
                   .arg(delay / 1000);
    emit stateChanged();
    fprintf(stderr, "[device] re-index reconnect: attempt %d of %d in %dms\n",
            m_reprobeAttempt + 1, budget, delay);
    m_reprobeTimer.start(delay);
}

void DeviceService::reprobeNow() {
    if (!m_devicePresent || m_connected || m_blockedPresence) {
        m_reprobeAttempt = 0;
        return;
    }
    if (m_busy || m_ejecting || m_awaitingDisconnect || m_syncActive
        || m_purging || m_pulling) {
        // Something else owns the device now, and however that flow ends it
        // arms its own reconnect.  Two ladders on one player is a bug.
        m_reprobeAttempt = 0;
        return;
    }
    ++m_reprobeAttempt;
    m_reprobePending = true;
    breach();
}

void DeviceService::eject() {
    emit ejectStarted();
    if (!m_connected || m_ejecting)
        return;
    // Mac disconnectDevice(): (sync forceStop is SyncEngine's duty),
    // finalize → sever → clear awaitingDisconnect.
    m_ejecting = true;
    m_connected = false;
    clearDeviceState();
    m_status = UiText::tr("Finalizing — device will re-index…");
    emit stateChanged();
    emit workerFinalize();
}

void DeviceService::finalizeAndRelease() {
    if (!m_connected || m_ejecting)
        return;
    // Fresh sync, fresh reconnect budget.
    m_reprobeTimer.stop();
    m_reprobeAttempt = 0;
    m_reprobePending = false;
    m_ejecting = true;  // reuse the finalize → sever chain
    m_connected = false;
    clearDeviceState();
    m_status = UiText::tr("Sync complete — the Zune is re-indexing…");
    emit stateChanged();
    emit workerFinalize();
}

void DeviceService::onFinalizeDone(bool ok) {
    if (!m_ejecting)
        return;  // finalize during a sync flow — SyncEngine handles it
    if (!ok)
        fprintf(stderr, "[device] finalize reported failure — severing anyway\n");
    emit workerSever();
}

void DeviceService::requestArt(quint32 itemId) {
    if (itemId == 0 || !m_connected || m_artRequested.contains(itemId))
        return;
    m_artRequested.insert(itemId);

    // Track ids carry no art on the Zune — redirect to the MTP album
    // object that owns the track (the mac's getAlbumList() route).
    const bool spoken = m_spokenArtIds.contains(itemId);
    const quint32 target = spoken ? itemId : m_trackToAlbumArt.value(itemId, itemId);

    const QString key = QString::number(target);
    if (m_artPaths.contains(key)) { // album already fetched — just alias
        m_artPaths.insert(QString::number(itemId), m_artPaths.value(key));
        emit artChanged();
        return;
    }

    m_artAliases[target].append(itemId);
    if (m_artAliases[target].size() > 1)
        return; // fetch already in flight for this album

    const QString path = m_artCacheDir + (spoken
        ? QStringLiteral("/spoken_%1_%2_%3_%4.jpg").arg(m_spokenArtSession)
            .arg(m_spokenGeneration).arg(m_spokenArtRevision).arg(target)
        : QStringLiteral("/%1.jpg").arg(target));
    emit workerGrabArt(target, path);
}

void DeviceService::requestImportedAlbumArt(quint32 itemId,
                                            const QString &artist,
                                            const QString &album) {
    if (itemId == 0 || album.trimmed().isEmpty() || !m_connected)
        return;
    const quint32 target = m_trackToAlbumArt.value(itemId, itemId);
    const QString targetKey = QString::number(target);
    const QString existingUrl = m_artPaths.value(targetKey).toString();
    if (!existingUrl.isEmpty()) {
        const QString path = QUrl(existingUrl).toLocalFile();
        if (!path.isEmpty())
            emit musicImportAlbumArtReady(artist, album, path);
        return;
    }

    QVariantList &waiting = m_importedAlbumArtTargets[target];
    const QString identity = artist.toCaseFolded() + QLatin1Char('\n')
        + album.toCaseFolded();
    bool alreadyWaiting = false;
    for (const QVariant &value : std::as_const(waiting)) {
        if (value.toMap().value(QStringLiteral("identity")).toString()
                == identity) {
            alreadyWaiting = true;
            break;
        }
    }
    if (!alreadyWaiting) {
        waiting.append(QVariantMap{{QStringLiteral("identity"), identity},
                                   {QStringLiteral("artist"), artist},
                                   {QStringLiteral("album"), album}});
    }

    if (m_artAliases.contains(target))
        return; // a device-browser request is already fetching this album

    // A prior browse miss must not suppress the import's one fresh attempt.
    m_artRequested.remove(itemId);
    requestArt(itemId);
}

QVariantList DeviceService::musicIdentityPeers(const QVariantMap &track) const {
    QVariantList out;
    for (const auto &peer : m_deviceMusicIndex.identitiesFor(MusicIdentity::fromMap(track))) out.append(MusicIdentity::toMap(peer));
    return out;
}

bool DeviceService::hasDeviceTrack(const QVariantMap &track, const QVariantList &sourcePeers) const {
    if (!m_connected) return false;
    QVector<MusicIdentity::Track> peers;
    peers.reserve(sourcePeers.size());
    for (const auto &peer : sourcePeers) peers.append(MusicIdentity::fromMap(peer.toMap()));
    return m_deviceMusicIndex.resolve(MusicIdentity::fromMap(track), peers).matched();
}

void DeviceService::rebuildOnDeviceKeys() {
    m_onDeviceKeys.clear();
    m_deviceMusicIndex.clear();
    const auto rows = m_tracks.rowsSnapshot();
    for (const auto &value : rows) {
        const auto row = value.toMap();
        m_deviceMusicIndex.add(MusicIdentity::fromMap(row), row.value(QStringLiteral("itemId")).toLongLong());
    }
    ++m_musicIdentityRevision;
    // Compatibility map only. New consumers use hasDeviceTrack with source
    // peers, so unknown readback cannot hide another disc with the same title.
    for (const auto &value : rows) {
        auto identity = MusicIdentity::fromMap(value.toMap());
        identity.discNumber = identity.trackNumber = 0;
        if (m_deviceMusicIndex.resolve(identity).matched())
            m_onDeviceKeys.insert(identity.artist + QLatin1Char('\t') + identity.album
                + QLatin1Char('\t') + identity.title, true);
    }
    m_photoOnDeviceKeys.clear();
    for (const QVariant &v : std::as_const(m_photosList))
        m_photoOnDeviceKeys.insert(
            photoStem(v.toMap().value(QStringLiteral("name")).toString()),
            true);
    if (!m_photoOnDeviceKeys.isEmpty())
        fprintf(stderr, "[device] %d photo stems on device, e.g. '%s'\n",
                int(m_photoOnDeviceKeys.size()),
                qUtf8Printable(m_photoOnDeviceKeys.constBegin().key()));
}

void DeviceService::noteSyncedPhoto(const QString &name) {
    m_photosList.append(QVariantMap{{"itemId", 0u},
                                    {"name", name},
                                    {"parentId", 0u},
                                    {"sizeMB", 0.0}});
    m_photoOnDeviceKeys.insert(photoStem(name), true);
    emit stateChanged();
}

void DeviceService::noteSyncedTrack(const QString &title, const QString &artist,
                                    const QString &album, quint32 itemId, int discNumber,
                                    int trackNumber, int durationMs) {
    TrackRow row;
    row.itemId = itemId;
    row.title = title;
    row.artist = artist;
    row.album = album;
    row.discNumber = qMax(0,discNumber);
    row.trackNumber = qMax(0,trackNumber);
    row.durationMs = qMax(0,durationMs);
    row.trackNumberReliable = true;
    row.identityFromReadback = false;
    m_tracks.appendRow(row);
    m_trackCount = m_tracks.rowCount();
    rebuildOnDeviceKeys();
    emit stateChanged();
}

void DeviceService::renameItem(quint32 itemId, const QString &newName) {
    if (!m_connected || itemId == 0 || newName.trimmed().isEmpty())
        return;
    emit workerRenameItem(itemId, newName.trimmed());
}

void DeviceService::renameDevice(const QString &newName) {
    if (!m_connected || newName.trimmed().isEmpty())
        return;
    emit workerRenameDevice(newName.trimmed());
}

QString DeviceService::importsDir() {
    return m_library
        ? m_library->ensureImportRoot(QStringLiteral("video"))
        : QString();
}

void DeviceService::saveVideoToLibrary(quint32 itemId, const QString &filename,
                                       const QString &subdir) {
    if (!m_connected || itemId == 0)
        return;
    QString dir = importsDir();
    if (dir.isEmpty()) {
        fprintf(stderr, "[device] nowhere to save a video -- no library\n");
        emit videoSaved(filename, false);
        return;
    }
    if (!subdir.isEmpty()) {
        dir += QLatin1Char('/')
            + LibraryPaths::safeComponent(subdir,
                                         QStringLiteral("video_%1").arg(itemId));
    }
    QDir().mkpath(dir);
    QVariantMap video{{"itemId", itemId}, {"name", filename}};
    for (const QVariant &entry : std::as_const(m_videosList)) {
        if (entry.toMap().value(QStringLiteral("itemId")).toUInt() == itemId) {
            video = entry.toMap();
            break;
        }
    }
    const QString fname = VideoIdentity::importFilename(video);
    emit workerExtractVideo(itemId, dir + QLatin1Char('/') + fname);
}

QString DeviceService::photoImportsDir() {
    return m_library
        ? m_library->ensureImportRoot(QStringLiteral("photos"))
        : QString();
}

QString DeviceService::musicImportsDir() {
    return m_library
        ? m_library->ensureImportRoot(QStringLiteral("music"))
        : QString();
}

void DeviceService::savePhotoToLibrary(quint32 itemId, const QString &filename,
                                       const QString &subdir) {
    if (!m_connected || itemId == 0)
        return;
    QString dir = photoImportsDir();
    if (dir.isEmpty()) {
        fprintf(stderr, "[device] nowhere to save a photo -- no library\n");
        emit photoSaved(filename, false);
        return;
    }
    if (!subdir.isEmpty()) {
        dir += QLatin1Char('/')
            + LibraryPaths::safeComponent(subdir,
                                         QStringLiteral("album_%1").arg(itemId));
    }
    QDir().mkpath(dir);
    const QString photoFallback = QStringLiteral("photo_%1.jpg").arg(itemId);
    const QString fname = filename.isEmpty()
        ? photoFallback : LibraryPaths::safeComponent(filename, photoFallback);
    emit workerExtractPhoto(itemId, dir + QLatin1Char('/') + fname);
}

void DeviceService::saveTrackToLibrary(quint32 itemId, const QString &filename,
                                       const QString &artist,
                                       const QString &album) {
    if (!m_connected || itemId == 0)
        return;
    // The browse rows already answer "Unknown Artist" for a device that stored a
    // GUID moniker (DeviceWorker's scan); this is the same rule at the door, so a
    // row that arrived from somewhere else cannot put {09316835-3188-481c-…} in
    // the importer's folder tree, in the albumartist hint a pull hands the local
    // library, or in the artist-image lookup that hint primes.
    const QString knownArtist = MusicIdentity::deviceArtist(artist);
    const QString knownAlbum = MusicIdentity::deviceAlbum(album);
    QString dir = musicImportsDir();
    if (dir.isEmpty()) {
        fprintf(stderr, "[device] nowhere to save a track -- no library\n");
        emit trackSaved(filename, false);
        return;
    }
    for (const QString &part : {knownArtist, knownAlbum}) {
        if (part.isEmpty())
            continue;
        dir += QLatin1Char('/')
            + LibraryPaths::safeComponent(part, QStringLiteral("item_%1").arg(itemId));
    }
    QDir().mkpath(dir);
    const QString trackFallback = QStringLiteral("track_%1.mp3").arg(itemId);
    QString fname = filename.isEmpty()
        ? trackFallback : LibraryPaths::safeComponent(filename, trackFallback);
    // Device names carry no extension — the Zune stores MP3/WMA; MP3 is
    // what we smuggle, so default to it when the name is bare.
    if (!fname.contains(QLatin1Char('.')))
        fname += QStringLiteral(".mp3");
    const QString destPath = TrackImportPath::available(
        dir + QLatin1Char('/') + fname, m_pendingTrackDestinations);
    m_pendingTrackDestinations.insert(TrackImportPath::key(destPath));
    m_trackImportBatch.started(itemId, knownArtist, knownAlbum,
                               m_trackToAlbumArtist.value(itemId, knownArtist));
    emit workerExtractTrack(itemId, destPath);
}

void DeviceService::beginPull(const QString &noun, int total, int existing) {
    m_pullNoun = noun.isEmpty() ? QStringLiteral("item") : noun;
    m_pullExisting = qMax(0, existing);
    m_pullFailed = 0;
    m_pullDone = 0;
    m_pullTotal = qMax(0, total);
    m_pulling = m_pullTotal > 0;
    if (m_pulling) {
        m_status = UiText::tr("Pulling %1 %2%3 from your Zune…")
                       .arg(m_pullTotal)
                       .arg(m_pullNoun, m_pullTotal == 1 ? QString()
                                                         : QStringLiteral("s"));
    } else if (m_pullExisting > 0) {
        // Nothing to extract — everything dropped was already owned.
        m_status = m_pullExisting == 1
            ? UiText::tr("That %1's already in your library").arg(m_pullNoun)
            : UiText::tr("%1 %2s already in your library")
                  .arg(m_pullExisting).arg(m_pullNoun);
        emit pullToast(m_status);
    }
    emit pullChanged();
    emit stateChanged();
}

void DeviceService::notePullDone(bool ok) {
    if (!m_pulling)
        return;
    if (!ok)
        m_pullFailed++;
    m_pullDone++;
    if (m_pullDone < m_pullTotal) {
        emit pullChanged();
        return;
    }
    // Batch complete — an honest summary: pulled, failed, already-had.
    m_pulling = false;
    const int okCount = m_pullTotal - m_pullFailed;
    QString msg = UiText::tr("Pulled %1 %2%3 to library")
                      .arg(okCount)
                      .arg(m_pullNoun, okCount == 1 ? QString()
                                                    : QStringLiteral("s"));
    if (m_pullFailed > 0)
        msg += QStringLiteral(" · %1 failed").arg(m_pullFailed);
    if (m_pullExisting > 0)
        msg += QStringLiteral(" · %1 already had").arg(m_pullExisting);
    m_status = msg;
    emit pullToast(msg);
    emit pullChanged();
    emit stateChanged();
}

QVariantMap DeviceService::trackInfo(quint32 itemId) const {
    for (int i = 0; i < m_tracks.rowCount(); ++i) {
        const auto index = m_tracks.index(i, 0);
        if (m_tracks.data(index, TrackModel::ItemIdRole).toUInt() != itemId) continue;
        return {{"itemId",itemId}, {"title",m_tracks.data(index,TrackModel::TitleRole)},
            {"artist",m_tracks.data(index,TrackModel::ArtistRole)},
            {"album",m_tracks.data(index,TrackModel::AlbumRole)},
            {"durationMs",m_tracks.data(index,TrackModel::DurationMsRole)},
            {"trackNumber",m_tracks.data(index,TrackModel::TrackNumberRole)},
            {"discNumber",m_tracks.data(index,TrackModel::DiscNumberRole)},
            {"trackNumberReliable",m_tracks.data(index,TrackModel::TrackNumberReliableRole)},
            {"discNumberReliable",m_tracks.data(index,TrackModel::DiscNumberReliableRole)},
            {"identityFromReadback",m_tracks.data(index,TrackModel::IdentityFromReadbackRole)}};
    }
    return {};
}

QVariantList DeviceService::deviceTrackRows() const { return m_tracks.rowsSnapshot(); }

void DeviceService::createPlaylist(const QString &name,
                                   const QVariantList &trackIds) {
    if (!m_connected || name.isEmpty())
        return;
    emit workerForgePlaylist(name, trackIds);
}

void DeviceService::requestPhotoFull(quint32 itemId) {
    if (!m_connected || itemId == 0 || m_photoFullPending.contains(itemId))
        return;
    const QString path =
        m_artCacheDir + QStringLiteral("/photofull_%1.jpg").arg(itemId);
    if (QFile::exists(path)) {
        emit photoFullReady(itemId, QUrl::fromLocalFile(path).toString());
        return;
    }
    m_photoFullPending.insert(itemId);
    emit workerExtractPhoto(itemId, path);
}

void DeviceService::noteSyncedVideo(const QVariantMap &video) {
    m_videosList.append(video);
    emit stateChanged();
}

bool DeviceService::udevRuleOk() const {
    return UdevSetup::installed(UdevSetup::rules());
}

bool DeviceService::udevRulesApply() const {
#if defined(Q_OS_WIN)
    // Windows claims the Zune with a WinUSB driver; there is no udev here.
    return false;
#elif defined(Q_OS_MACOS)
    // macOS has no udev and no rules directory at all: libusb reaches the player
    // through IOKit, and whether a process may open it is not decided by a file this
    // app could own.  A one-click installer here would promise something that does
    // not exist, so the fix-it card must not offer it.
    return false;
#else
    return true;
#endif
}

bool DeviceService::usbAccessProblem() const {
    if (!m_usbBlockDetail.isEmpty())
        return true;
    return udevRulesApply() && !udevRuleOk();
}

bool DeviceService::usbAccessFixable() const {
    // One click means pkexec plus a rule file this app owns.  Where that is not a
    // thing - Windows, macOS - the card must not offer it.
    return udevRulesApply();
}

QString DeviceService::usbAccessTitle() const {
    if (!m_usbBlockDetail.isEmpty()) {
#if defined(Q_OS_MACOS)
        // Unreachable today: zuneBlocked() is only emitted from the Windows-only
        // pollPresence pass.  Branch it anyway so the day a macOS presence pass
        // lands, this card cannot print the word "windows" on a Mac.
        return tr("your zune is plugged in but macos won't give it to zuuned");
#else
        return tr("your zune is plugged in but windows cannot use it");
#endif
    }
    return tr("your zune needs a USB permission rule");
}

QString DeviceService::usbAccessDetail() const {
    if (!m_usbBlockDetail.isEmpty()) {
#if defined(Q_OS_MACOS)
        return tr("macos needs no driver installed for zuuned to reach the player - if the player is still shut out, look for a USB prompt to allow in System Settings > Privacy & Security, then replug. zuuned never installs a driver itself.");
#else
        return tr("windows has not bound a driver zuuned can reach. give it WinUSB or libusbK with the Zadig utility, then replug. the device id below is what to pick there - zuuned never installs a driver itself.");
#endif
    }
    return tr("one click installs it — you'll get your system's password prompt, no terminal needed.");
}

QString DeviceService::usbAccessCommand() const {
    if (!m_usbBlockDetail.isEmpty())
        return m_usbBlockDetail;
    return udevInstallCommand();
}

QString DeviceService::udevInstallCommand() const {
    if (!udevRulesApply())
        return QString();
    const QString script = UdevSetup::installScript(UdevSetup::rules());
    return script.isEmpty() ? QString() : QStringLiteral("sudo sh -c ") + UdevSetup::shellQuote(script);
}

void DeviceService::installUdevRule() {
    if (!udevRulesApply()) {
        emit udevRuleInstalled(false,
#if defined(Q_OS_MACOS)
            // No udev, no rules directory, and no system extension zuuned may
            // install: say what is true instead of naming a tool that cannot help.
            UiText::tr("macOS has no udev rules - zuuned reaches the Zune through libusb and needs nothing installed"));
#else
            // No udev and no pkexec on Windows: name what actually fixes it.
            UiText::tr("Windows needs a WinUSB driver for the Zune rather than a udev rule - install WinUSB with Zadig, then replug"));
#endif
        return;
    }

    const QString script = UdevSetup::installScript(UdevSetup::rules());
    if (script.isEmpty()) {
        emit udevRuleInstalled(false, UiText::tr("USB rule is missing from this build"));
        return;
    }

    auto *proc = new QProcess(this);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus) {
        const bool ok = (code == 0) && udevRuleOk();
        if (ok) {
            emit udevRuleChanged();
            emit usbAccessChanged();
            emit udevRuleInstalled(true,
                UiText::tr("USB rule installed — replug the Zune if it doesn't connect"));
        } else {
            // 126/127 = pkexec dismissed / no auth agent.
            emit udevRuleInstalled(false,
                (code == 126 || code == 127)
                    ? UiText::tr("cancelled — run the command shown instead")
                    : UiText::tr("couldn't install the rule (exit %1)")
                          .arg(code));
        }
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        emit udevRuleInstalled(false,
            UiText::tr("pkexec not available — run the command shown"));
        proc->deleteLater();
    });
    proc->start(QStringLiteral("pkexec"),
                {QStringLiteral("sh"), QStringLiteral("-c"),
                 script});
}

bool DeviceService::purgeInterruptedVideo(const QString &filename) {
    if (!m_connected || m_busy || m_purging || m_pulling || filename.isEmpty()) return false;
    m_purging = true;
    m_purgeCurrent = 0;
    m_purgeTotal = 1;
    emit purgeChanged();
    emit workerPurgeInterruptedVideo(filename);
    return true;
}

void DeviceService::setSyncActive(bool active) {
    if (m_syncActive == active)
        return;
    m_syncActive = active;
    if (!active && m_spokenRequested && m_connected)
        QTimer::singleShot(0, this, &DeviceService::refreshSpokenMedia);
}

void DeviceService::rebuildPodcastDeviceStatus() {
    m_podcastDeviceStatus.clear();
    m_podcastMembershipKnown = true;
    m_podcastStatusWarning.clear();
    QSet<quint32> ids;
    for (const auto &value : m_podcastEpisodesList) {
        const auto row = value.toMap();
        const auto key = qMakePair(row.value("feedUrl").toString(), row.value("sourceUrl").toString());
        const auto id = RecoveryIdentity::objectId(row.value("itemId"));
        if (!id || id == 0xffffffffu || ids.contains(id) || m_podcastDeviceStatus.contains(key)) {
            m_podcastDeviceStatus.clear(); m_podcastMembershipKnown = false;
            m_podcastStatusWarning = UiText::tr("Podcast source identities are ambiguous; device badges are unavailable.");
            return;
        }
        ids.insert(id);
        if (!row.value("sourceKnown", !key.first.isEmpty() && !key.second.isEmpty()).toBool()
            || key.first.isEmpty() || key.second.isEmpty()) {
            m_podcastMembershipKnown = false;
            m_podcastStatusWarning = UiText::tr("Some podcast source identities are unavailable; device badges may be incomplete.");
            continue;
        }
        m_podcastDeviceStatus.insert(key, row);
    }
}

QVariantMap DeviceService::podcastDeviceStatus(const QString &feedUrl, const QString &sourceUrl) const {
    QVariantMap result{{"known", false}, {"onDevice", false}, {"playedKnown", false}, {"played", false}, {"bookmarkMs", 0u}};
    if (!m_connected || feedUrl.isEmpty() || sourceUrl.isEmpty()) return result;
    const auto key = qMakePair(feedUrl, sourceUrl);
    if (m_podcastDeviceStatus.contains(key)) {
        const auto row = m_podcastDeviceStatus.value(key);
        result["known"] = true; result["onDevice"] = true;
        const bool known = row.value("playedKnown").toBool();
        result["playedKnown"] = known;
        result["played"] = known && row.value("played").toBool();
        result["bookmarkMs"] = known ? row.value("bookmarkMs").toUInt() : 0u;
    } else result["known"] = m_podcastMembershipKnown;
    return result;
}

void DeviceService::prunePodcastDeviceStatus(const QSet<quint32> &ids) {
    for (auto it = m_podcastDeviceStatus.begin(); it != m_podcastDeviceStatus.end();)
        if (ids.contains(it.value().value("itemId").toUInt())) it = m_podcastDeviceStatus.erase(it);
        else ++it;
}

void DeviceService::notePodcastOnDevice(quint64 generation, const QString &serial,
                                       const QVariantMap &episode, quint32 itemId) {
    if (!m_connected || generation != m_spokenGeneration || serial != m_serial
        || !itemId || itemId == 0xffffffffu) return;
    const auto key = qMakePair(episode.value("feedUrl").toString(), episode.value("sourceUrl").toString());
    if (key.first.isEmpty() || key.second.isEmpty() || key.first.contains(QChar(0)) || key.second.contains(QChar(0))) return;
    auto row = m_podcastDeviceStatus.value(key);
    if (row.value("itemId").toUInt() != itemId) {
        // A confirmed new/reused identity proves presence, not playback state.
        row = {{"itemId", itemId}, {"playedKnown", false}, {"played", false}, {"bookmarkMs", 0u}};
    }
    for (auto it = m_podcastDeviceStatus.begin(); it != m_podcastDeviceStatus.end();)
        if (it.key() != key && it.value().value("itemId").toUInt() == itemId) {
            m_podcastMembershipKnown = false;
            it = m_podcastDeviceStatus.erase(it);
        } else ++it;
    m_podcastDeviceStatus.insert(key, row);
    if (m_spokenMediaLoading) { ++m_spokenRequest; m_spokenMediaLoading = false; }
    emit spokenMediaChanged();
}

void DeviceService::ensureSpokenMedia() {
    if (m_connected && !m_spokenRequested) refreshSpokenMedia();
}

void DeviceService::refreshSpokenMedia() {
    if (!m_connected || m_spokenMediaLoading) return;
    m_spokenRequested = true;
    if (m_busy || m_purging || m_pulling || m_syncActive || m_ejecting || m_awaitingDisconnect) {
        m_spokenReadDeferred = true;
        m_spokenMediaError = UiText::tr("Wait for the current device operation, then refresh.");
        emit spokenMediaChanged();
        return;
    }
    m_spokenReadDeferred = false;
    m_spokenMediaLoading = true;
    m_spokenMediaError.clear();
    emit spokenMediaChanged();
    emit stateChanged();
    emit workerReadSpokenMedia(m_spokenGeneration, ++m_spokenRequest, m_serial);
}

// Called only after SyncEngine correlates a successful worker deletion result.
// Keep the browser truthful even when cleanup is cancelled after partial success.
void DeviceService::applyPodcastCleanup(quint64 generation, const QString &serial,
                                        const QVariantList &rows) {
    if (!m_connected || generation != m_spokenGeneration || serial != m_serial) return;
    QSet<quint32> ids;
    qint64 freed = 0;
    for (const auto &value : rows) {
        const auto row = value.toMap();
        const auto id = RecoveryIdentity::objectId(row.value("itemId"));
        if (!id || id == 0xffffffffu || ids.contains(id)) continue;
        ids.insert(id);
        freed += qBound<qint64>(0, row.value("sizeBytes").toLongLong(), UINT32_MAX);
    }
    for (int i = m_podcastEpisodesList.size() - 1; i >= 0; --i)
        if (ids.contains(m_podcastEpisodesList[i].toMap().value("itemId").toUInt()))
            m_podcastEpisodesList.removeAt(i);
    prunePodcastDeviceStatus(ids);
    noteBytesFreed(freed);
    emit spokenMediaChanged();
}

void DeviceService::purgeSpokenItems(const QString &kind, const QVariantList &itemIds) {
    if (!m_connected || busy() || m_pulling || m_syncActive || m_ejecting || m_awaitingDisconnect)
        return;
    if (m_serial.isEmpty()) {
        m_spokenMediaError = UiText::tr("The Zune has no readable serial number. Reconnect before deleting.");
        emit spokenMediaChanged();
        return;
    }
    if ((kind != QStringLiteral("podcast") && kind != QStringLiteral("audiobook")) || itemIds.isEmpty())
        return;
    if (kind == QStringLiteral("audiobook") && !audiobookBrowseSupported()) {
        m_spokenMediaError = UiText::tr("Audiobook browsing is not supported on this Zune model yet. Nothing was deleted.");
        emit spokenMediaChanged();
        return;
    }
    const QVariantList &inventory = kind == QStringLiteral("podcast")
        ? m_podcastEpisodesList : m_audiobookPartsList;
    QHash<quint32, QVariantMap> known;
    for (const auto &value : inventory) {
        const auto row = value.toMap();
        known.insert(row.value(QStringLiteral("itemId")).toUInt(), row);
    }
    QVariantList selected;
    QSet<quint32> seen;
    for (const auto &value : itemIds) {
        const quint32 id = RecoveryIdentity::objectId(value);
        if (!id || id == 0xffffffffu || !known.contains(id)) {
            m_spokenMediaError = UiText::tr("The selection changed. Refresh the Zune and select it again.");
            emit spokenMediaChanged();
            return;
        }
        if (!seen.contains(id)) {
            seen.insert(id);
            selected.append(known.value(id));
        }
    }
    m_spokenMediaError.clear();
    m_purging = true;
    m_purgeCurrent = 0;
    m_purgeTotal = selected.size();
    m_status = QStringLiteral("Deleting %1 item(s)…").arg(selected.size());
    emit spokenMediaChanged();
    emit purgeChanged();
    emit stateChanged();
    emit workerPurgeSpokenItems(m_spokenGeneration, ++m_spokenRequest, m_serial, kind, selected);
}

void DeviceService::purgeItems(const QVariantList &itemIds) {
    if (!m_connected || busy() || m_pulling || m_syncActive || itemIds.isEmpty())
        return;
    m_purging = true;
    m_purgeCurrent = 0;
    m_purgeTotal = itemIds.size();
    m_status = QStringLiteral("Deleting %1 item(s)…").arg(itemIds.size());
    emit purgeChanged();
    emit stateChanged();
    emit workerPurgeItems(itemIds);
}

bool DeviceService::purgeInterruptedObject(const QVariantMap &record) {
    if (!m_connected || m_busy || m_purging || record.value("serial").toString() != deviceSerial()) return false;
    m_purging = true;
    m_purgeCurrent = 0;
    m_purgeTotal = 1;
    emit purgeChanged();
    emit workerPurgeInterruptedObject(record);
    return true;
}

void DeviceService::onPurgeDone(int okCount, int failCount,
                                const QVariantList &purgedIds) {
    QSet<quint32> ids;
    for (const QVariant &v : purgedIds)
        ids.insert(v.toUInt());
    // W3: sum the freed bytes BEFORE the rows are dropped, then credit
    // them back so the capacity gate reflects the deletion immediately
    // (the device's own number won't update until the eject re-index).
    qint64 freed = m_tracks.sumFilesizeForIds(ids);
    for (const QVariant &v : std::as_const(m_videosList)) {
        const QVariantMap m = v.toMap();
        if (ids.contains(m.value(QStringLiteral("itemId")).toUInt()))
            freed += qint64(m.value(QStringLiteral("sizeMB")).toDouble() * 1e6);
    }
    for (const QVariant &v : std::as_const(m_photosList)) {
        const QVariantMap m = v.toMap();
        if (ids.contains(m.value(QStringLiteral("itemId")).toUInt()))
            freed += qint64(m.value(QStringLiteral("sizeMB")).toDouble() * 1e6);
    }
    for (const auto &list : {m_podcastEpisodesList, m_audiobookPartsList})
        for (const QVariant &v : list) {
            const auto row = v.toMap();
            if (ids.contains(row.value(QStringLiteral("itemId")).toUInt()))
                freed += row.value(QStringLiteral("sizeBytes")).toLongLong();
        }
    noteBytesFreed(freed);
    m_tracks.removeByItemIds(ids);
    m_trackCount = m_tracks.rowCount();
    // Videos/photos live in flat lists — drop purged rows there too so
    // the device browse pages update without a re-breach.
    const auto dropPurged = [&ids](QVariantList &list) {
        for (int i = list.size() - 1; i >= 0; i--)
            if (ids.contains(list[i].toMap()
                                 .value(QStringLiteral("itemId")).toUInt()))
                list.removeAt(i);
    };
    dropPurged(m_videosList);
    dropPurged(m_photosList);
    dropPurged(m_podcastEpisodesList);
    dropPurged(m_audiobookPartsList);
    prunePodcastDeviceStatus(ids);
    QSet<quint32> remainingArt;
    for (const auto &list : {m_podcastEpisodesList, m_audiobookPartsList})
        for (const QVariant &value : list)
            remainingArt.insert(value.toMap().value(QStringLiteral("artItemId")).toUInt());
    for (quint32 id : std::as_const(m_spokenArtIds)) {
        if (remainingArt.contains(id)) continue;
        m_artPaths.remove(QString::number(id));
        m_artRequested.remove(id);
        m_artAliases.remove(id);
    }
    m_spokenArtIds = remainingArt;
    emit artChanged();
    emit spokenMediaChanged();
    pruneDevicePlaylists(m_playlistsList, ids);
    rebuildOnDeviceKeys();
    // Do NOT re-read the device here: right after DeleteObject the Zune
    // still reports its PRE-delete free space (it doesn't re-index until
    // the eject-time CleanDataStore), so a refresh would clobber the
    // noteBytesFreed credit above and the gauge would snap back to
    // "full". The optimistic credit IS the accurate number until the
    // next breach re-reads ground truth.
    m_purging = false;
    m_purgeCurrent = m_purgeTotal = 0;
    const QString freedStr = freed >= 1000000000LL
        ? QStringLiteral("%1 GB").arg(freed / 1e9, 0, 'f', 1)
        : QStringLiteral("%1 MB").arg(qRound(freed / 1e6));
    m_status = failCount == 0
        ? (freed > 0 ? UiText::tr("Deleted %1 item(s) — freed %2")
                           .arg(okCount).arg(freedStr)
                     : QStringLiteral("Deleted %1 item(s)").arg(okCount))
        : QStringLiteral("Deleted %1, failed %2").arg(okCount).arg(failCount);
    emit purgeChanged();
    emit stateChanged();
}

void DeviceService::requestPhotoThumb(quint32 itemId) {
    if (itemId == 0 || !m_connected || m_artRequested.contains(itemId))
        return;
    m_artRequested.insert(itemId);
    const QString path = m_artCacheDir + QStringLiteral("/photo_%1.jpg").arg(itemId);
    emit workerGrabPhotoThumb(itemId, path);
}

void DeviceService::onArtDone(quint32 itemId, const QString &path, bool ok) {
    if (QFileInfo(path).fileName().startsWith(QStringLiteral("spoken_"))
        && (!m_connected || !m_spokenArtIds.contains(itemId) || !QFileInfo(path).fileName().startsWith(
            QStringLiteral("spoken_%1_%2_%3_").arg(m_spokenArtSession)
                .arg(m_spokenGeneration).arg(m_spokenArtRevision))))
        return;
    const QList<quint32> requesters = m_artAliases.take(itemId);
    const QVariantList importedAlbums = m_importedAlbumArtTargets.take(itemId);
    if (!ok)
        return; // no art — QML keeps the fallback rendering
    const QString url = QUrl::fromLocalFile(path).toString();
    m_artPaths.insert(QString::number(itemId), url);
    for (quint32 rid : requesters)
        m_artPaths.insert(QString::number(rid), url);
    emit artChanged();
    for (const QVariant &value : importedAlbums) {
        const QVariantMap album = value.toMap();
        emit musicImportAlbumArtReady(
            album.value(QStringLiteral("artist")).toString(),
            album.value(QStringLiteral("album")).toString(), path);
    }
}
