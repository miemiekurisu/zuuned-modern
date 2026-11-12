#include "DeviceWorker.h"
#include "sync/RecoveryIdentity.h"
#include "library/VideoIdentity.h"
#include "library/MusicIdentity.h"

#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QThread>

#include <vector>
#include <functional>

#include <algorithm>

#include "UiText.h"

#if defined(Q_OS_WIN)
// The second road to the same player: a Zune bound to Windows' own WPD/MTP
// driver instead of to WinUSB.  src/wpd/WpdBackend.h carries why both are needed
// and what this one cannot do.
#include "wpd/WpdBackend.h"
#include "library/LibraryPaths.h"
#endif

extern "C" {
#include "zune.h"
}

namespace {
QString readSpokenInventory(ZuneDevice *dev, QVariantList &podcasts, QVariantList &books, bool enrichStatus = true) {
    ZunePodcastEpisode *episodes = nullptr;
    ZunePodcastSeries *series = nullptr;
    ZuneAudiobookPart *parts = nullptr;
    ZunePodcastEpisodeSource *sources = nullptr;
    ZunePodcastState *states = nullptr;
    int episodeCount = 0, seriesCount = 0, partCount = 0, sourceCount = 0, stateCount = 0;
    QString error;
    if (zune_get_podcast_episodes(dev, &episodes, &episodeCount)
        || zune_get_podcast_series(dev, &series, &seriesCount)
        || zune_get_audiobook_parts(dev, &parts, &partCount)) {
        error = QString::fromUtf8(zune_get_error());
        if (error.isEmpty()) error = UiText::tr("Could not read spoken media from this Zune.");
    } else {
        // Source identities enrich badges; they must not disable otherwise
        // valid device browsing or explicitly confirmed management.
        bool sourcesKnown = episodeCount == 0
            || (enrichStatus && zune_get_podcast_episode_sources(dev, &sources, &sourceCount) == 0);
        if (sourceCount != episodeCount) sourcesKnown = false;
        QHash<quint32, ZunePodcastEpisodeSource> sourceById;
        QSet<QPair<QString, QString>> sourceKeys;
        for (int i = 0; i < sourceCount; ++i) {
            const auto &source = sources[i];
            const auto key = qMakePair(QString::fromUtf8(source.feed_url), QString::fromUtf8(source.source_url));
            if (!source.item_id || sourceById.contains(source.item_id) || key.first.isEmpty()
                || key.second.isEmpty() || sourceKeys.contains(key)) {
                sourcesKnown = false;
                break;
            }
            sourceById.insert(source.item_id, source); sourceKeys.insert(key);
        }
        // Firmware state is optional for browsing. Failure never means unplayed.
        QHash<quint32, ZunePodcastState> stateById;
        if (enrichStatus && episodeCount > 0 && zune_get_podcast_states(dev, &states, &stateCount) == 0) {
            bool valid = true;
            for (int i = 0; i < stateCount; ++i) {
                if (!states[i].item_id || stateById.contains(states[i].item_id)) { valid = false; break; }
                stateById.insert(states[i].item_id, states[i]);
            }
            if (!valid) stateById.clear();
        }
        QHash<quint32, QString> showNames;
        for (int i = 0; i < seriesCount; ++i)
            showNames.insert(series[i].item_id, QString::fromUtf8(series[i].name));
        for (int i = 0; i < episodeCount; ++i) {
            const auto &e = episodes[i];
            const auto showName = showNames.value(e.series_id, QString::fromUtf8(e.series_name));
            const bool sourceKnown = sourcesKnown && sourceById.contains(e.item_id)
                && sourceById.value(e.item_id).series_id == e.series_id
                && sourceById.value(e.item_id).parent_id == e.parent_id;
            const auto source = sourceById.value(e.item_id);
            const auto state = stateById.value(e.item_id);
            const bool playedKnown = stateById.contains(e.item_id) && state.series_id == e.series_id
                && state.duration_ms == e.duration_ms && state.title
                && QString::fromUtf8(state.title) == QString::fromUtf8(e.title);
            podcasts.append(QVariantMap{{"itemId", e.item_id}, {"parentId", e.parent_id},
                {"sourceKnown", sourceKnown},
                {"feedUrl", sourceKnown ? QString::fromUtf8(source.feed_url) : QString()},
                {"sourceUrl", sourceKnown ? QString::fromUtf8(source.source_url) : QString()},
                {"playedKnown", playedKnown}, {"played", playedKnown && state.marked_played && (state.raw_flags & 0x200u)},
                {"bookmarkMs", playedKnown ? state.bookmark_ms : 0u},
                {"seriesId", e.series_id}, {"title", QString::fromUtf8(e.title)},
                {"author", QString::fromUtf8(e.author)}, {"seriesName", showName},
                {"durationMs", e.duration_ms}, {"sizeMB", e.filesize / 1e6},
                {"sizeBytes", QVariant::fromValue<qulonglong>(e.filesize)}, {"artItemId", e.series_id}});
        }
        for (int i = 0; i < partCount; ++i) {
            const auto &p = parts[i];
            books.append(QVariantMap{{"itemId", p.item_id}, {"parentId", p.parent_id},
                {"partNumber", p.part_number}, {"title", QString::fromUtf8(p.title)},
                {"author", QString::fromUtf8(p.author)}, {"bookTitle", QString::fromUtf8(p.book_title)},
                {"durationMs", p.duration_ms}, {"sizeMB", p.filesize / 1e6},
                {"sizeBytes", QVariant::fromValue<qulonglong>(p.filesize)}, {"artItemId", p.item_id}});
        }
    }
    if (!error.isEmpty()) { podcasts.clear(); books.clear(); }
    zune_free_podcast_states(states, stateCount);
    zune_free_podcast_episode_sources(sources, sourceCount);
    zune_free_podcast_episodes(episodes, episodeCount);
    zune_free_podcast_series(series, seriesCount);
    zune_free_audiobook_parts(parts, partCount);
    return error;
}
}

// The special members live here because m_wpd is a Wpd::Session this header only
// forward declares: the Windows backend is complete here and nowhere else, and
// that is the whole point.
DeviceWorker::DeviceWorker() = default;
DeviceWorker::~DeviceWorker() = default;

void DeviceWorker::doReadSpokenMedia(quint64 generation, quint64 request, const QString &serial) {
    QVariantList podcasts, books;
    QString error;
#if defined(Q_OS_WIN)
    if (m_wpdShared.load() && m_wpd) {
        // A show, its feed identity and its play state are ZMDB records; plain
        // MTP has no such thing.  An empty list here would tell the app the
        // episodes had been deleted from the player, which is a worse answer
        // than the truth.
        error = UiText::tr("This player is on Windows' MTP driver, which has no podcast or audiobook library to read.");
    } else
#endif
    if (generation != m_spokenGeneration.load() || !m_dev
        || QString::fromUtf8(zune_get_serial(m_dev)) != serial)
        error = UiText::tr("The Zune disconnected or changed. Reconnect and refresh.");
    else {
        zune_clear_abort(m_dev);
        error = readSpokenInventory(m_dev, podcasts, books);
    }
    emit spokenMediaRead(generation, request, serial, podcasts, books, error);
}

void DeviceWorker::doPurgeSpokenItems(quint64 generation, quint64 request, const QString &serial,
                                     const QString &kind, const QVariantList &rows) {
    QVariantList podcasts, books, purged;
    QString error;
    const auto currentDevice = [this, generation, &serial] {
        return generation == m_spokenGeneration.load() && m_dev
            && QString::fromUtf8(zune_get_serial(m_dev)) == serial;
    };
    if (serial.isEmpty() || !currentDevice())
        error = UiText::tr("The Zune disconnected or changed. Nothing was deleted.");
    else if (kind != QStringLiteral("podcast") && kind != QStringLiteral("audiobook"))
        error = UiText::tr("Unknown spoken media category. Nothing was deleted.");
    else {
        zune_clear_abort(m_dev);
        error = readSpokenInventory(m_dev, podcasts, books, false);
    }
    if (error.isEmpty()) {
        QHash<quint32, QVariantMap> live;
        for (const QVariant &value : kind == QStringLiteral("podcast") ? podcasts : books) {
            const auto row = value.toMap();
            live.insert(row.value(QStringLiteral("itemId")).toUInt(), row);
        }
        // Validate the complete selection before the first write, including
        // its category, parent and metadata. A recycled ID alone is insufficient.
        QSet<quint32> seen;
        for (const QVariant &value : rows) {
            const auto row = value.toMap();
            const quint32 id = RecoveryIdentity::objectId(row.value(QStringLiteral("itemId")));
            auto expectedIdentity = row;
            auto liveIdentity = live.value(id);
            // Compare every original browse identity field. Optional badge and
            // listening enrichments are not deletion identities; transient
            // availability must not prevent management of legacy podcasts/books.
            const bool compareSources = row.value("sourceKnown").toBool() && liveIdentity.value("sourceKnown").toBool();
            for (const auto &key : {"sourceKnown", "playedKnown", "played", "bookmarkMs"}) {
                expectedIdentity.remove(key); liveIdentity.remove(key);
            }
            if (!compareSources) {
                for (const auto &key : {"feedUrl", "sourceUrl"}) {
                    expectedIdentity.remove(key); liveIdentity.remove(key);
                }
            }
            if (!id || id == 0xffffffffu || seen.contains(id) || !live.contains(id) || liveIdentity != expectedIdentity) {
                error = UiText::tr("The device selection changed. Refresh before deleting.");
                break;
            }
            seen.insert(id);
        }
    }
    if (error.isEmpty()) {
        for (int i = 0; i < rows.size(); ++i) {
            if (!currentDevice()) {
                error = UiText::tr("The Zune disconnected during deletion.");
                break;
            }
            emit spokenPurgeProgress(generation, request, serial, i + 1, rows.size());
            const quint32 id = rows[i].toMap().value(QStringLiteral("itemId")).toUInt();
            if (zune_purge_track(m_dev, id) == 0)
                purged.append(id);
            else {
                error = UiText::tr("Some items could not be deleted: %1")
                    .arg(QString::fromUtf8(zune_autopsy_name(zune_autopsy(m_dev))));
                break;
            }
            QThread::msleep(100);
        }
    }
    emit spokenItemsPurged(generation, request, serial, purged, rows.size() - purged.size(), error);
}

void DeviceWorker::doBreach() {
    BreachData r;

    if (m_dev) { // already connected — treat as success, no rescan
        emit breachDone(r);
        return;
    }

    emit stage(UiText::tr("Opening USB + MTPZ authentication…"), 0.15);
    ZuneDevice *dev = zune_breach();
    if (!dev) {
        const char *err = zune_get_error();
#if defined(Q_OS_WIN)
        // No udev here: a Windows tester needs the driver name, not a Linux rule.
        const QString hint = UiText::tr("check USB and bind the Zune to WinUSB");
#elif defined(Q_OS_MACOS)
        // No udev and no driver to bind here either: libusb opens the player through
        // IOKit, so what a macOS tester can check is the cable and the system's own
        // USB permission, not a rule file.
        const QString hint = UiText::tr("check USB and macOS USB access");
#else
        const QString hint = UiText::tr("check USB + udev rule");
#endif
        r.error = err && err[0]
            ? QString::fromUtf8(err)
            : UiText::tr("No Zune found (%1)").arg(hint);
#if defined(Q_OS_WIN)
        // libusb cannot open a player whose interface a kernel driver owns, and
        // that is the ordinary state of a Zune on a Windows machine nobody has
        // given a driver to.  "zune_breach failed" is therefore not "no Zune" -
        // it is "not on this road" - so ask Windows before saying absent.
        if (breachWpd(&r)) {
            emit breachDone(r);
            return;
        }
        r.error += UiText::tr("; and Windows would not hand the player over either");
#endif
        emit breachDone(r);
        return;
    }

    m_dev = dev;
    m_devShared.store(dev);
    emit stage(UiText::tr("Reading device info…"), 0.45);
    r.ok = true;
    // The road this player answered on.  A Zune bound to WinUSB answers here;
    // one left on Windows' own MediaDevice driver answers in breachWpd(), and
    // the UI has to tell the two apart because they do not do the same things.
    r.backend = QStringLiteral("libzune");
    r.name = QString::fromUtf8(zune_get_name(dev));
    r.model = QString::fromUtf8(zune_get_model(dev));
    r.serial = QString::fromUtf8(zune_get_serial(dev));
    r.family = int(zune_get_family(dev));
    fprintf(stderr, "[worker] device family: 0x%02X\n", r.family);
    r.battery = zune_get_battery(dev);
    r.capacityGB = zune_get_capacity(dev) / 1e9;
    r.freeGB = zune_get_headroom(dev) / 1e9;

    // ZMDB scan — the whole library in one vendor operation.
    emit stage(UiText::tr("Scanning library (ZMDB)…"), 0.6);
    ZuneDBLibrary *lib = nullptr;
    if (zune_infiltrate(dev, &lib) == 0 && lib) {
        // Counted rather than assumed.  Whether a Zune really names an ALBUM by
        // its moniker, as opposed to only an artist, is visible exactly here, and
        // the line printed under this loop is the evidence for the next time the
        // question is asked - which the user just asked from the device page.
        int monikerArtistRows = 0, monikerAlbumRows = 0, monikerGenreRows = 0;
        int monikerTitleRows = 0;
        int paddedArtistRows = 0, paddedAlbumRows = 0, paddedGenreRows = 0;
        QString firstMonikerArtist, firstMonikerAlbum, firstMonikerGenre;
        QString firstMonikerTitle;
        QSet<QString> rawArtistNames, rawAlbumNames, rawGenreNames;
        r.tracks.reserve(lib->track_count);
        for (int i = 0; i < lib->track_count; i++) {
            const ZuneTrack &t = lib->tracks[i];
            TrackRow row;
            row.itemId = t.item_id;
            const QString rawTitle = QString::fromUtf8(t.title ? t.title : "");
            if (MusicIdentity::isGuidMoniker(rawTitle)) {
                ++monikerTitleRows;
                if (firstMonikerTitle.isEmpty())
                    firstMonikerTitle = rawTitle;
            }
            row.title = rawTitle;
            // A Zune that never learned an artist or album name stores a GUID
            // moniker in that field, so the row for a tagless rip is named
            // {09316835-3188-481c-a359-0f8d2a937ac8} on screen while the album
            // beside it reads "EGOIST".  From here on that text is treated as
            // the unknown the app already has - the artists/albums pivots, the
            // artwork cache key an imported device cover is stored under, the
            // folder a pull lands in and the artist a send would write back all
            // take the same answer the library gives a file with no tag.
            // MusicIdentity.h carries the observation and the shape.
            const QString rawArtist = QString::fromUtf8(t.artist ? t.artist : "");
            const QString rawAlbum = QString::fromUtf8(t.album ? t.album : "");
            const QString rawGenre = QString::fromUtf8(t.genre ? t.genre : "");
            if (rawArtist != rawArtist.trimmed())
                ++paddedArtistRows;
            if (rawAlbum != rawAlbum.trimmed())
                ++paddedAlbumRows;
            if (MusicIdentity::isGuidMoniker(rawArtist)) {
                ++monikerArtistRows;
                if (firstMonikerArtist.isEmpty())
                    firstMonikerArtist = rawArtist;
            }
            if (MusicIdentity::isGuidMoniker(rawAlbum)) {
                ++monikerAlbumRows;
                if (firstMonikerAlbum.isEmpty())
                    firstMonikerAlbum = rawAlbum;
            }
            if (rawGenre != rawGenre.trimmed())
                ++paddedGenreRows;
            if (MusicIdentity::isGuidMoniker(rawGenre)) {
                ++monikerGenreRows;
                if (firstMonikerGenre.isEmpty())
                    firstMonikerGenre = rawGenre;
            }
            rawArtistNames.insert(rawArtist);
            rawAlbumNames.insert(rawAlbum);
            rawGenreNames.insert(rawGenre);
            row.artist = MusicIdentity::deviceArtist(rawArtist);
            row.album = MusicIdentity::deviceAlbum(rawAlbum);
            row.genre = MusicIdentity::deviceGenre(rawGenre);
            row.durationMs = int(t.duration_ms);
            row.trackNumber = t.tracknumber;
            row.discNumber = t.disc_number;
            // Preserve returned disc metadata. Raw ZMDB track slots on Keel
            // are known to contain garbage; intended send metadata is separate.
            row.trackNumberReliable = false;
            row.identityFromReadback = true;
            row.filesize = qint64(t.filesize);
            r.tracks.append(std::move(row));
        }
        // The wording is load-bearing.  The session-log redactor lists "device name"
        // and "friendly name" as credential keys (src/diagnostics/Redactor.cpp), so a
        // line that reads "device name <value>" has everything after it folded into
        // [redacted] - which this line did, once, taking its own evidence with it.
        fprintf(stderr,
                "[zmdb] GUID moniker fields on %d track rows: artist %d (%s),"
                " album %d (%s), genre %d (%s), title %d (%s);"
                " blank-padded artist %d, album %d, genre %d\n",
                lib->track_count, monikerArtistRows,
                qUtf8Printable(firstMonikerArtist.trimmed().isEmpty()
                                   ? QStringLiteral("-") : firstMonikerArtist.trimmed()),
                monikerAlbumRows,
                qUtf8Printable(firstMonikerAlbum.trimmed().isEmpty()
                                   ? QStringLiteral("-") : firstMonikerAlbum.trimmed()),
                monikerGenreRows,
                qUtf8Printable(firstMonikerGenre.trimmed().isEmpty()
                                   ? QStringLiteral("-") : firstMonikerGenre.trimmed()),
                monikerTitleRows,
                qUtf8Printable(firstMonikerTitle.trimmed().isEmpty()
                                   ? QStringLiteral("-") : firstMonikerTitle.trimmed()),
                paddedArtistRows, paddedAlbumRows, paddedGenreRows);
        // What the player actually calls its albums and artists. The count above
        // only answers "did the one known placeholder shape appear", which cannot
        // explain an id still visible on the device page - and that question was
        // asked from the device page on 2026-10-22. Every distinct string the ZMDB
        // pool handed over is quoted here so blanks and empty names are visible,
        // bounded so a 10k-track library stays a line.
        const auto nameList = [](const QSet<QString> &values) {
            QStringList sorted(values.constBegin(), values.constEnd());
            std::sort(sorted.begin(), sorted.end(),
                      [](const QString &a, const QString &b) {
                return a.compare(b, Qt::CaseInsensitive) < 0;
            });
            QString text;
            for (int i = 0; i < sorted.size(); i++) {
                if (i == 24) {
                    text += QStringLiteral(", ...(+%1)").arg(sorted.size() - 24);
                    break;
                }
                const QString &value = sorted.at(i);
                if (i)
                    text += QStringLiteral(", ");
                text += QStringLiteral("\"%1\"").arg(value.size() > 60
                    ? value.left(57) + QStringLiteral("...") : value);
            }
            return text;
        };
        fprintf(stderr, "[zmdb] distinct artists (%d): %s\n", int(rawArtistNames.size()),
                qUtf8Printable(nameList(rawArtistNames)));
        fprintf(stderr, "[zmdb] distinct albums (%d): %s\n", int(rawAlbumNames.size()),
                qUtf8Printable(nameList(rawAlbumNames)));
        fprintf(stderr, "[zmdb] distinct genres (%d): %s\n", int(rawGenreNames.size()),
                qUtf8Printable(nameList(rawGenreNames)));
        r.albumCount = lib->album_count;
        r.artistCount = lib->artist_count;
        // NOTE: lib stays alive until AFTER the video/photo/playlist
        // loops below — freeing it here was a latent use-after-free
        // that detonated the moment the video loop started allocating
        // (series vendor-prop reads recycled the freed arrays).

        std::sort(r.tracks.begin(), r.tracks.end(),
                  [](const TrackRow &a, const TrackRow &b) {
            int c = a.artist.compare(b.artist, Qt::CaseInsensitive);
            if (c != 0) return c < 0;
            c = a.album.compare(b.album, Qt::CaseInsensitive);
            if (c != 0) return c < 0;
            if (a.trackNumber != b.trackNumber)
                return a.trackNumber < b.trackNumber;
            return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
        });

        // Grouped lists for the artists/albums/genres pivots, derived
        // from tracks so counts always match what's shown. Album entries
        // carry the first track's itemId so the UI can request art.
        QMap<QString, int> artistCounts, genreCounts;
        struct AlbumAgg { QString artist; int count = 0; quint32 artItem = 0; };
        QMap<QString, AlbumAgg> albumAgg;
        for (const TrackRow &t : std::as_const(r.tracks)) {
            const QString artist = t.artist.isEmpty()
                ? QStringLiteral("Unknown Artist") : t.artist;
            artistCounts[artist]++;
            if (!t.album.isEmpty()) {
                AlbumAgg &agg = albumAgg[t.album];
                agg.artist = artist;
                agg.count++;
                if (agg.artItem == 0)
                    agg.artItem = t.itemId;
            }
            if (!t.genre.isEmpty())
                genreCounts[t.genre]++;
        }
        for (auto it = artistCounts.cbegin(); it != artistCounts.cend(); ++it)
            r.artists.append(QVariantMap{{"name", it.key()},
                                         {"count", it.value()}});
        for (auto it = albumAgg.cbegin(); it != albumAgg.cend(); ++it)
            r.albums.append(QVariantMap{{"name", it.key()},
                                        {"subtitle", it.value().artist},
                                        {"count", it.value().count},
                                        {"artItemId", it.value().artItem}});
        for (auto it = genreCounts.cbegin(); it != genreCounts.cend(); ++it)
            r.genres.append(QVariantMap{{"name", it.key()},
                                        {"count", it.value()}});
        // The pivot as the screen will show it, after deviceGenre(). The pool above
        // is what the player stores; this is what the user sees, and the two differ
        // exactly where a name field held a moniker. Asked on 2026-10-22 because a
        // GUID was on screen where a name belongs, this line is what answers it
        // without anybody having to click into a tile to find out.
        QStringList genreTiles;
        for (const QVariant &g : std::as_const(r.genres))
            genreTiles << QStringLiteral("%1 (%2)").arg(
                g.toMap().value(QStringLiteral("name")).toString(),
                g.toMap().value(QStringLiteral("count")).toString());
        fprintf(stderr, "[zmdb] genre tiles after the moniker rule (%d): %s\n",
                int(r.genres.size()), qUtf8Printable(genreTiles.join(QStringLiteral(", "))));
        // And the other two pivots, because "the album tile shows a uuid" is a
        // report about a label, not about a column in ZMDB. What the screen draws
        // is the grouped list, so the grouped list is what gets printed: an album
        // tile carries its owner as a subtitle, and an owner rendered from a
        // moniker is exactly the complaint. Bounded like the album objects above,
        // since this line exists to answer one sighting, not to dump a library.
        QStringList artistTiles, albumTiles;
        for (const QVariant &a : std::as_const(r.artists))
            artistTiles << QStringLiteral("%1 (%2)").arg(
                a.toMap().value(QStringLiteral("name")).toString(),
                a.toMap().value(QStringLiteral("count")).toString());
        for (int i = 0; i < r.albums.size() && i < 24; i++) {
            const QVariantMap a = r.albums.at(i).toMap();
            albumTiles << QStringLiteral("\"%1\" by \"%2\" (%3)").arg(
                a.value(QStringLiteral("name")).toString(),
                a.value(QStringLiteral("subtitle")).toString(),
                a.value(QStringLiteral("count")).toString());
        }
        fprintf(stderr, "[zmdb] artist tiles after the moniker rule (%d): %s\n",
                int(r.artists.size()), qUtf8Printable(artistTiles.join(QStringLiteral(", "))));
        fprintf(stderr, "[zmdb] album tiles after the moniker rule (%d of %d): %s\n",
                albumTiles.size(), int(r.albums.size()),
                qUtf8Printable(albumTiles.join(QStringLiteral(", "))));

        // Device videos / pictures / playlists for the browse pages.
        auto metagenreLabel = [](uint16_t mg) -> QString {
            switch (mg) {
            case 0x23: return QStringLiteral("music video");
            case 0x25: return QStringLiteral("movie");
            case 0x26: return QStringLiteral("tv show");
            default:   return QStringLiteral("video");
            }
        };
        auto buildVideoEntry = [dev, &metagenreLabel](const ZuneVideoFile &v) {
            QVariantMap entry{
                {"itemId", quint32(v.item_id)},
                {"name", QString::fromUtf8(v.title && v.title[0] ? v.title : v.filename ? v.filename : "")},
                {"title", QString::fromUtf8(v.title ? v.title : "")},
                {"filename", QString::fromUtf8(v.object_filename ? v.object_filename : "")},
                {"format", int(v.object_format)},
                {"sizeMB", double(v.filesize) / 1e6},
                {"metagenre", int(v.metagenre)},
                {"kind", metagenreLabel(v.metagenre)}};
            // TV episodes: series/season/episode/title live in Zune
            // vendor properties (0xDA9A/0xDAB5/0xDAB6) — the mac reads
            // these at connect so the device browser can group by show.
            if (v.metagenre == 0x26) {
                char *series = nullptr, *title = nullptr;
                int season = 0, episode = 0;
                if (zune_get_series_info(dev, v.item_id, &series, &season,
                                         &episode, &title) == 0) {
                    entry.insert(QStringLiteral("series"),
                                 QString::fromUtf8(series ? series : ""));
                    entry.insert(QStringLiteral("season"), season);
                    entry.insert(QStringLiteral("episode"), episode);
                    const QString episodeTitle = QString::fromUtf8(title ? title : "");
                    entry.insert(QStringLiteral("episodeTitle"), episodeTitle);
                    if (!episodeTitle.isEmpty()) {
                        entry.insert(QStringLiteral("name"), episodeTitle);
                        entry.insert(QStringLiteral("title"), episodeTitle);
                    }
                }
                free(series);
                free(title);
            }
            return entry;
        };
        for (int i = 0; i < lib->video_count; i++)
            r.videos.append(buildVideoEntry(lib->videos[i]));

        // Zune HD (Pavo): the ZMDB scan carries no video rows — the mac
        // falls back to plain MTP enumeration, and so do we.
        if (r.videos.isEmpty() && r.family == 0x06) {
            emit stage(UiText::tr("Loading videos (HD MTP fallback)…"), 0.8);
            int nvid = 0;
            if (ZuneVideoFile *vids = zune_get_videos(dev, &nvid)) {
                for (int i = 0; i < nvid; i++)
                    r.videos.append(buildVideoEntry(vids[i]));
                zune_free_videos(vids, nvid);
                fprintf(stderr, "[worker] HD MTP video fallback: %d videos\n",
                        nvid);
            }
        }
        // Dead-pipe guard for every per-item probe below: once the
        // transport reports 0x02FF, every further call burns a full
        // 10s USB timeout — ~50 photos turned a failed enumeration
        // into an 8-minute "connection freeze" (Pavo, 2026-08-31).
        // Two consecutive failures → stop probing, keep what we have.
        int probeFailStreak = 0;
        for (int i = 0; i < lib->photo_count; i++) {
            const ZunePhotoFile &p = lib->photos[i];
            QString name = QString::fromUtf8(p.filename ? p.filename : "");
            // Classic ZMDB photo records resolve to empty names —
            // backfill from the MTP object (photos are few; ~50ms each).
            if (name.isEmpty() && probeFailStreak < 2) {
                char *mtpName = nullptr;
                if (zune_probe_object_named(m_dev, p.item_id, nullptr,
                                            nullptr, &mtpName) == 0
                    && mtpName) {
                    name = QString::fromUtf8(mtpName);
                    free(mtpName);
                    probeFailStreak = 0;
                } else {
                    probeFailStreak++;
                    if (probeFailStreak >= 2)
                        fprintf(stderr, "[worker] photo-name backfill: "
                                "transport looks dead — skipping the rest\n");
                }
            }
            r.photos.append(QVariantMap{
                {"itemId", quint32(p.item_id)},
                {"name", name},
                {"parentId", quint32(p.parent_id)},
                {"sizeMB", double(p.filesize) / 1e6}});
        }
        {
            // Stats-tab breakdown depends on these — log what the ZMDB
            // picture records actually carried (HD sizes suspect 0).
            int pz = 0;
            double pmb = 0;
            for (const QVariant &v : std::as_const(r.photos)) {
                const double mb = v.toMap().value(QStringLiteral("sizeMB")).toDouble();
                if (mb <= 0.0) pz++;
                pmb += mb;
            }
            fprintf(stderr, "[worker] photo sizes: %d total, %d zero, %.1f MB summed\n",
                    int(r.photos.size()), pz, pmb);
            int vz = 0;
            double vmb = 0;
            for (const QVariant &v : std::as_const(r.videos)) {
                const double mb = v.toMap().value(QStringLiteral("sizeMB")).toDouble();
                if (mb <= 0.0) vz++;
                vmb += mb;
            }
            fprintf(stderr, "[worker] video sizes: %d total, %d zero, %.1f MB summed\n",
                    int(r.videos.size()), vz, vmb);
            for (int i = 0; i < qMin(3, int(r.videos.size())); i++) {
                const QVariantMap m = r.videos[i].toMap();
                fprintf(stderr, "[worker]   video[%d] '%s' %.1f MB\n", i,
                        m.value(QStringLiteral("name")).toString().toUtf8().constData(),
                        m.value(QStringLiteral("sizeMB")).toDouble());
            }
        }
        // Photo albums = the MTP FOLDER objects the photos actually
        // live in. The ZMDB's photo-album atoms carry different ids
        // than the photos' MTP parent_id, so grouping by them pooled
        // everything under root — resolve real folder names instead.
        {
            QSet<quint32> parents;
            for (int i = 0; i < lib->photo_count; i++)
                if (lib->photos[i].parent_id != 0)
                    parents.insert(quint32(lib->photos[i].parent_id));
            if (!parents.isEmpty()) {
                ZuneFolderEntry *folders = nullptr;
                const int nf = zune_get_folders(m_dev, &folders);
                QHash<quint32, QString> folderName;
                for (int i = 0; i < nf; i++)
                    folderName.insert(quint32(folders[i].id),
                                      QString::fromUtf8(
                                          folders[i].name ? folders[i].name : ""));
                if (folders)
                    zune_free_folders(folders, nf);
                for (quint32 pid : std::as_const(parents)) {
                    const QString name = folderName.value(pid);
                    r.photoAlbums.append(QVariantMap{
                        {"itemId", pid},
                        {"name", name.isEmpty()
                                     ? QStringLiteral("photos") : name}});
                }
            }
        }
        // Playlists via MTP ENUMERATION, not the ZMDB — the ZMDB
        // playlist record layout is undecoded (scan reports 0 on both
        // families even with real playlists present). A format-filtered
        // handle query + per-playlist name/refs is cheap: playlists
        // are few. (Phase 9, hardware-verified Keel + Pavo.)
        {
            int npl = 0;
            ZunePlaylist *pls = zune_get_playlists(m_dev, &npl);
            for (int i = 0; i < npl; i++) {
                QVariantList ids;
                for (uint32_t j = 0; j < pls[i].track_count; j++)
                    ids.append(quint32(pls[i].track_ids[j]));
                r.playlists.append(QVariantMap{
                    {"itemId", quint32(pls[i].playlist_id)},
                    {"name", QString::fromUtf8(pls[i].name ? pls[i].name : "")},
                    {"count", int(pls[i].track_count)},
                    {"trackIds", ids}});
            }
            QSet<QString> playlistNames;
            for (int i = 0; i < npl; i++)
                playlistNames.insert(QString::fromUtf8(pls[i].name ? pls[i].name : ""));
            fprintf(stderr, "[zmdb] device playlists (%d): %s\n", npl,
                    qUtf8Printable(nameList(playlistNames)));
            zune_free_playlists(pls, npl);
        }
        zune_free_scan(lib);

        // Album objects (MTP enumeration) — the mac's getAlbumList()
        emit stage(QStringLiteral("Enumerating albums…"), 0.85);
        // path. Each carries its member track_ids; art requests for any
        // track get redirected to its album object.
        int albumObjCount = 0;
        if (ZuneAlbumObject *objs = zune_get_albums(dev, &albumObjCount)) {
            // The player's own album objects, named as the player names them. An
            // album object whose name property is absent shows its GUID moniker on
            // the device's own screen (libzune src/album.c), which is a shape this
            // app cannot repair by reading a field - so these names are recorded,
            // bounded, rather than assumed to match the ZMDB album strings above.
            for (int i = 0; i < albumObjCount && i < 24; i++)
                fprintf(stderr,
                        "[zmdb] album object %u name=\"%s\" artist=\"%s\" tracks=%u\n",
                        objs[i].album_id, objs[i].name ? objs[i].name : "",
                        objs[i].artist ? objs[i].artist : "", objs[i].track_count);
            for (int i = 0; i < albumObjCount; i++) {
                // The album object's artist is the albumartist hint a pull hands
                // the import batch, so the moniker is dropped here too rather
                // than reaching library metadata and artist-image lookups.
                const QString albumArtist = MusicIdentity::deviceArtist(
                    QString::fromUtf8(objs[i].artist ? objs[i].artist : ""));
                for (uint32_t t = 0; t < objs[i].track_count; t++) {
                    r.trackToAlbumArt.insert(objs[i].track_ids[t],
                                             objs[i].album_id);
                    if (!albumArtist.isEmpty())
                        r.trackToAlbumArtist.insert(objs[i].track_ids[t],
                                                   albumArtist);
                }
            }
            zune_free_albums(objs, albumObjCount);
        }
    }

    emit breachDone(r);
}

#if defined(Q_OS_WIN)
namespace {

// A WPD object id is the MTP handle as the provider spells it - "s10001",
// "o5000005" on a Zune - while everything above this block speaks quint32.
bool wpdHandleOf(const QString &id, quint32 *out) {
    if (id.size() < 2)
        return false;
    bool ok = false;
    const quint32 value = id.mid(1).toUInt(&ok, 16);
    if (ok && out)
        *out = value;
    return ok;
}

struct WpdSendProgress {
    DeviceWorker *worker;
    const QString *entryId;
    double lastEmitted;
};

// Byte-fraction progress from the WPD stream loop, throttled to whole percents
// because the queue repaints on every signal.  Same discipline as the libzune
// progress thunk on the other road.
void wpdSendProgressThunk(double fraction, void *userdata) {
    auto *ctx = static_cast<WpdSendProgress *>(userdata);
    if (!ctx || !ctx->entryId)
        return;
    const double pct = fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
    if (pct < 1.0 && pct - ctx->lastEmitted < 0.01)
        return;
    ctx->lastEmitted = pct;
    ctx->worker->notifySendProgress(*ctx->entryId, pct);
}

} // namespace

void DeviceWorker::noteWpdUnsupported(const char *feature) {
    fprintf(stderr,
            "[wpd] %s needs a ZMDB/vendor operation.  This player is on Windows'"
            " own MTP driver, which has no such call - reporting failure instead"
            " of answering with a success that did not happen.\n",
            feature ? feature : "that operation");
}

bool DeviceWorker::breachWpd(BreachData *result) {
    if (!result)
        return false;
    if (!m_wpd)
        m_wpd = std::make_unique<Wpd::Session>();
    Wpd::Session &wpd = *m_wpd;

    // "Will Windows hand the player over?" is a different question from "can
    // libusb open it?", and it has a different answer on every machine where
    // nobody has run Zadig - which is every machine a tester arrives on.
    emit stage(UiText::tr("Asking Windows for the player…"), 0.2);
    QString error;
    if (!wpd.openFirstZune(&error)) {
        fprintf(stderr, "[wpd] Windows would not hand the player over: %s\n",
                qUtf8Printable(error));
        return false;
    }
    wpd.clearAbort();
    m_wpdShared.store(true);
    m_wpdIds.clear();
    m_wpdFolders.clear();
    m_wpdStorage.clear();
    m_wpdArtReported = false;

    emit stage(UiText::tr("Reading device info…"), 0.45);
    Wpd::DeviceInfo info;
    if (!wpd.readDevice(&info, &error) || info.storageIds.isEmpty()) {
        fprintf(stderr, "[wpd] opened, then could not be read: %s\n",
                qUtf8Printable(error));
        m_wpdShared.store(false);
        wpd.close();
        return false;
    }
    // readDevice sorts storage by free space, so this is also "the roomiest
    // place to put a file", which is the choice a user would make by hand.
    m_wpdStorage = info.storageIds.first();

    // Plain MTP enumeration instead of a ZMDB scan: one listing per folder, and
    // the folders are the library.  There is no vendor database behind this road,
    // which is also why no MTPZ credential is involved and a build that ships
    // none works here.
    emit stage(UiText::tr("Listing files over MTP…"), 0.6);
    struct Found {
        Wpd::ObjectInfo object;
        QString folder;   // breadcrumb of the parent, e.g. "/Music/Artist/Album"
        quint32 handle = 0;
    };
    QVector<Found> audio, video, photo;
    QHash<quint32, QString> folderNames;
    QString firstFailure;
    int walked = 0;
    bool truncated = false;
    bool cancelled = false;
    const int maxDepth = 12;
    const int maxObjects = 30000;

    std::function<bool(const QString &, int, const QString &)> descend;
    descend = [&](const QString &parent, int depth, const QString &path) -> bool {
        if (wpd.aborted()) {
            cancelled = true;
            return false;
        }
        if (depth > maxDepth || walked >= maxObjects) {
            truncated = true;
            return false;
        }
        QVector<Wpd::ObjectInfo> kids;
        QString why;
        const QString label = path.isEmpty() ? parent : path;
        if (!wpd.children(parent, &kids, &why)) {
            // One folder the player refuses is not a reason to report an empty
            // library; it is a reason to say what was not read.
            if (firstFailure.isEmpty())
                firstFailure = QStringLiteral("%1: %2").arg(label, why);
            fprintf(stderr, "[wpd] listing %s failed: %s\n", qUtf8Printable(label),
                    qUtf8Printable(why));
            return false;
        }
        for (const Wpd::ObjectInfo &kid : std::as_const(kids)) {
            if (wpd.aborted()) {
                cancelled = true;
                return false;
            }
            if (++walked > maxObjects) {
                truncated = true;
                return false;
            }
            quint32 handle = 0;
            if (!wpdHandleOf(kid.id, &handle))
                continue;
            m_wpdIds.insert(handle, kid.id);
            const QString childPath = path + QLatin1Char('/') + kid.name;
            if (kid.kind == Wpd::KindFolder) {
                folderNames.insert(handle, kid.name);
                descend(kid.id, depth + 1, childPath);
            } else if (kid.kind == Wpd::KindAudio || kid.kind == Wpd::KindVideo
                       || kid.kind == Wpd::KindImage) {
                Found found;
                found.object = kid;
                found.folder = path;
                found.handle = handle;
                if (kid.kind == Wpd::KindAudio)
                    audio.append(found);
                else if (kid.kind == Wpd::KindVideo)
                    video.append(found);
                else
                    photo.append(found);
            }
        }
        return true;
    };
    for (const QString &storage : std::as_const(info.storageIds))
        descend(storage, 1, QString());

    QString scope;
    if (truncated)
        scope += QStringLiteral(" [stopped at the walk budget]");
    if (cancelled)
        scope += QStringLiteral(" [cancelled]");
    if (!firstFailure.isEmpty())
        scope += QStringLiteral(" first failure: ") + firstFailure;
    fprintf(stderr,
            "[wpd] walked %d object(s): %d audio, %d video, %d image%s;"
            " writing to storage %s of %d\n",
            walked, int(audio.size()), int(video.size()), int(photo.size()),
            qUtf8Printable(scope), qUtf8Printable(m_wpdStorage),
            int(info.storageIds.size()));

    BreachData r;
    r.ok = true;
    r.backend = QStringLiteral("wpd");
    r.name = info.friendlyName;
    r.model = info.model;
    // No serial, on purpose: Session never asks WPD for one, because the serial
    // and the PnP id name a person's player and this project keeps device
    // identity out of logs and files.  Nothing on this road needs it.
    r.serial = info.serial;
    // 0xFF because it is none of the libzune families.  Anything that branches
    // on 0x05/0x06 would be asking this player to behave like a ZMDB device it is
    // not currently connected to.
    r.family = 0xFF;
    r.battery = info.battery;
    r.capacityGB = double(info.capacityBytes) / 1e9;
    r.freeGB = double(info.freeBytes) / 1e9;

    for (const Found &found : std::as_const(audio)) {
        const Wpd::ObjectInfo &o = found.object;
        TrackRow row;
        row.itemId = found.handle;
        // WPD has two name-ish properties and a provider fills either.  The Zune
        // software leaves NAME as the bare title for its own syncs and keeps the
        // file name in ORIGINAL_FILE_NAME, so both are candidates, and a moniker
        // in either is the same "no name" the rest of the app already means.
        const QString fromTitle = MusicIdentity::usableName(o.title);
        const QString name = fromTitle.isEmpty()
            ? MusicIdentity::usableName(o.name) : fromTitle;
        row.title = name.isEmpty() ? UiText::tr("Unknown Title") : name;
        row.artist = MusicIdentity::deviceArtist(o.artist);
        row.album = MusicIdentity::deviceAlbum(o.album);
        row.genre = MusicIdentity::deviceGenre(o.genre);
        // Measured on this player: it stores and reports artist, duration and
        // track number and drops album, genre and title on upload, while the
        // objects the Zune software synced itself carry all of them.  So the
        // folders this app wrote on the way in are the album record that
        // survives - which is what the local scanner reads from a watch folder
        // too: <storage>/Music/<artist>/<album>/file.
        const QStringList below = found.folder.split(QLatin1Char('/'),
                                                     Qt::SkipEmptyParts).mid(1);
        if (row.album.isEmpty() && !below.isEmpty())
            row.album = MusicIdentity::deviceAlbum(below.last());
        if (row.artist.isEmpty() && below.size() >= 2)
            row.artist = MusicIdentity::deviceArtist(below.at(below.size() - 2));
        row.durationMs = int(o.durationMs);
        row.trackNumber = int(o.trackNumber);
        // Unlike a classic ZMDB track slot, this number came back out of the
        // player's own property store; there is no disc property in MTP at all.
        row.trackNumberReliable = true;
        row.discNumberReliable = false;
        row.identityFromReadback = true;
        row.filesize = qint64(o.size);
        r.tracks.append(std::move(row));
    }
    // Verbatim the libzune ordering, so one library lists the same way whichever
    // road it arrived on.
    std::sort(r.tracks.begin(), r.tracks.end(),
              [](const TrackRow &a, const TrackRow &b) {
        int c = a.artist.compare(b.artist, Qt::CaseInsensitive);
        if (c != 0) return c < 0;
        c = a.album.compare(b.album, Qt::CaseInsensitive);
        if (c != 0) return c < 0;
        if (a.trackNumber != b.trackNumber)
            return a.trackNumber < b.trackNumber;
        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
    });

    QMap<QString, int> artistCounts, genreCounts;
    struct AlbumAgg { QString artist; int count = 0; };
    QMap<QString, AlbumAgg> albumAgg;
    for (const TrackRow &t : std::as_const(r.tracks)) {
        const QString artist = t.artist.isEmpty()
            ? QStringLiteral("Unknown Artist") : t.artist;
        artistCounts[artist]++;
        if (!t.album.isEmpty()) {
            AlbumAgg &agg = albumAgg[t.album];
            agg.artist = artist;
            agg.count++;
        }
        if (!t.genre.isEmpty())
            genreCounts[t.genre]++;
    }
    for (auto it = artistCounts.cbegin(); it != artistCounts.cend(); ++it)
        r.artists.append(QVariantMap{{"name", it.key()},
                                     {"count", it.value()}});
    for (auto it = albumAgg.cbegin(); it != albumAgg.cend(); ++it)
        r.albums.append(QVariantMap{{"name", it.key()},
                                    {"subtitle", it.value().artist},
                                    {"count", it.value().count},
                                    // Always 0 on this road.  The player gives no
                                    // picture for an audio object at all - asked
                                    // directly: GetSupportedResources answers
                                    // DEFAULT only, and a THUMBNAIL or ALBUM_ART
                                    // stream fails with 0x8007138E.  An art request
                                    // for handle 0 returns without touching the
                                    // bus, so a 200-album library does not pay 200
                                    // guaranteed failures for a picture that is
                                    // not on the player.
                                    {"artItemId", quint32(0)}});
    for (auto it = genreCounts.cbegin(); it != genreCounts.cend(); ++it)
        r.genres.append(QVariantMap{{"name", it.key()},
                                    {"count", it.value()}});
    // Derived, not read: ZMDB states an artist and album count of its own and
    // there is nothing to state those numbers here.  The pivots above are built
    // from the rows, so these agree with what the page can show.
    r.albumCount = r.albums.size();
    r.artistCount = r.artists.size();

    for (const Found &found : std::as_const(video)) {
        const Wpd::ObjectInfo &o = found.object;
        // No metagenre and no series properties: those are Zune vendor
        // extensions, and a plain MTP client neither writes nor reads them.  A
        // video therefore arrives as a video, which is the honest depth here.
        r.videos.append(QVariantMap{
            {"itemId", found.handle},
            {"name", o.name},
            {"title", o.title.isEmpty() ? o.name : o.title},
            {"filename", o.name},
            {"format", 0},
            {"sizeMB", double(o.size) / 1e6},
            {"metagenre", 0},
            {"kind", QStringLiteral("video")}});
    }
    QSet<quint32> photoParents;
    for (const Found &found : std::as_const(photo)) {
        const Wpd::ObjectInfo &o = found.object;
        quint32 parent = 0;
        wpdHandleOf(o.parentId, &parent);
        if (parent)
            photoParents.insert(parent);
        r.photos.append(QVariantMap{
            {"itemId", found.handle},
            {"name", o.name},
            {"parentId", parent},
            {"sizeMB", double(o.size) / 1e6}});
    }
    // Photo albums over MTP are the folders the pictures sit in - the same
    // conclusion the ZMDB path reached for a different reason (see the photo
    // album block in doBreach), so the two roads agree on what an album is.
    for (quint32 parent : std::as_const(photoParents)) {
        const QString name = folderNames.value(parent);
        r.photoAlbums.append(QVariantMap{
            {"itemId", parent},
            {"name", name.isEmpty() ? QStringLiteral("photos") : name}});
    }
    // No playlists.  A Zune keeps them in its own metadata and MTP gives a
    // generic client no way to read or build them, so the page stays empty
    // rather than showing something that is not the player's list.
    fprintf(stderr, "[wpd] library: %d tracks, %d albums, %d artists,"
            " %d videos, %d photos, %d photo folders, 0 playlists\n",
            int(r.tracks.size()), int(r.albums.size()), int(r.artists.size()),
            int(r.videos.size()), int(r.photos.size()), int(r.photoAlbums.size()));

    *result = r;
    return true;
}

QString DeviceWorker::wpdObjectIdFor(quint32 itemId) const {
    const auto hit = m_wpdIds.constFind(itemId);
    if (hit != m_wpdIds.constEnd())
        return hit.value();
    // Only reachable for a handle that never came from a listing or a create.
    // The provider's own string is always preferred: a Zune writes no leading
    // zeros ("o5000005", not "o05000005"), and guessing its spelling is exactly
    // the kind of assumption this file keeps getting caught out by.
    return QStringLiteral("o%1").arg(itemId, 0, 16).toUpper();
}

bool DeviceWorker::ensureWpdFolder(const QString &parentId, const QString &name,
                                   QString *objectId, QString *error) {
    if (parentId.isEmpty() || name.isEmpty()) {
        if (error)
            *error = UiText::tr("A folder needs a parent and a name.");
        return false;
    }
    // Windows' own case-insensitive matching is what the player emulates, so a
    // second track for "Abba" must reuse "ABBA" rather than make a near-twin.
    const QString cacheKey = parentId + QLatin1Char('|') + name.toLower();
    const auto hit = m_wpdFolders.constFind(cacheKey);
    if (hit != m_wpdFolders.constEnd()) {
        if (objectId)
            *objectId = hit.value();
        return true;
    }
    Wpd::Session &wpd = *m_wpd;
    QVector<Wpd::ObjectInfo> kids;
    if (!wpd.children(parentId, &kids, error))
        return false;
    for (const Wpd::ObjectInfo &kid : std::as_const(kids)) {
        if (kid.kind != Wpd::KindFolder)
            continue;
        if (kid.name.compare(name, Qt::CaseInsensitive) != 0)
            continue;
        quint32 handle = 0;
        if (wpdHandleOf(kid.id, &handle))
            m_wpdIds.insert(handle, kid.id);
        m_wpdFolders.insert(cacheKey, kid.id);
        if (objectId)
            *objectId = kid.id;
        return true;
    }
    QString created;
    if (!wpd.makeFolder(parentId, name, &created, error))
        return false;
    quint32 handle = 0;
    if (wpdHandleOf(created, &handle))
        m_wpdIds.insert(handle, created);
    m_wpdFolders.insert(cacheKey, created);
    fprintf(stderr, "[wpd] made folder '%s' in %s as %s\n", qUtf8Printable(name),
            qUtf8Printable(parentId), qUtf8Printable(created));
    if (objectId)
        *objectId = created;
    return true;
}

void DeviceWorker::sendTrackWpd(const QString &entryId, const QString &filepath,
                                const QString &title, const QString &albumartist,
                                const QString &album, const QString &genre,
                                int trackNumber, int durationMs) {
    if (!m_wpd) {
        emit sendDone(entryId, false, 0, QStringLiteral("NotConnected"));
        return;
    }
    Wpd::Session &wpd = *m_wpd;
    wpd.clearAbort();
    if (m_wpdStorage.isEmpty()) {
        // A storage list is two property reads; if the connection predates this
        // class knowing where to write, ask now rather than fail the send.
        Wpd::DeviceInfo info;
        QString why;
        if (wpd.readDevice(&info, &why) && !info.storageIds.isEmpty())
            m_wpdStorage = info.storageIds.first();
        else
            fprintf(stderr, "[wpd] no storage to write to: %s\n", qUtf8Printable(why));
    }
    if (m_wpdStorage.isEmpty()) {
        emit sendDone(entryId, false, 0, QStringLiteral("NoStorage"));
        return;
    }

    // The same shape the libzune road makes, and for the same reason the name
    // goes through LibraryPaths::safeComponent(): a Zune-supplied album called
    // "P:44 / Vol. 2" is a path traversal waiting to happen, not a folder name.
    // On this road the folders ARE the album grouping - there is no album object
    // to forge - so skipping them would lose the album on the player's screen.
    QString parent = m_wpdStorage;
    QString error;
    const QStringList chain{
        QStringLiteral("Music"),
        LibraryPaths::safeComponent(albumartist, QString()),
        LibraryPaths::safeComponent(album, QString())};
    for (const QString &part : chain) {
        if (part.isEmpty())
            continue;   // no artist or no album: the level above is the right shelf
        QString folder;
        if (!ensureWpdFolder(parent, part, &folder, &error)) {
            emit sendDone(entryId, false, 0, QStringLiteral("Wpd: ") + error);
            return;
        }
        parent = folder;
    }

    Wpd::ObjectInfo meta;
    meta.title = title;
    // The wire artist is the albumartist here too, which is the rule the libzune
    // path already follows: what groups an album is the album's owner.
    meta.artist = albumartist;
    meta.albumArtist = albumartist;
    meta.album = album;
    meta.genre = genre;
    meta.durationMs = quint32(durationMs > 0 ? durationMs : 0);
    meta.trackNumber = quint32(trackNumber > 0 ? trackNumber : 0);

    QString objectId;
    WpdSendProgress ctx{this, &entryId, 0.0};
    if (!wpd.upload(parent, filepath, meta, wpdSendProgressThunk, &ctx,
                    &objectId, &error)) {
        emit sendDone(entryId, false, 0, QStringLiteral("Wpd: ") + error);
        return;
    }
    quint32 handle = 0;
    if (!wpdHandleOf(objectId, &handle))
        handle = 0;
    if (handle)
        m_wpdIds.insert(handle, objectId);
    emit sendDone(entryId, true, handle, QString());
}

bool DeviceWorker::downloadWpd(quint32 itemId, const QString &destPath,
                               QString *written, QString *error) {
    if (!m_wpd) {
        if (error)
            *error = UiText::tr("No player is connected over MTP.");
        return false;
    }
    Wpd::Session &wpd = *m_wpd;
    const QString id = wpdObjectIdFor(itemId);
    fprintf(stderr, "[wpd] extract %u (%s) -> %s\n", itemId, qUtf8Printable(id),
            qUtf8Printable(destPath));

    // The extension is settled here because this is the only place the
    // container is actually in hand.  A WPD object's display name is a title, a
    // title with no dot reaches DeviceService as a bare name that gets `.mp3`
    // bolted on, and this very player has been measured storing ASF bytes behind
    // an `.mp3` name on the WinUSB road - so the format property wins when the
    // two disagree.
    //
    // One known cost, deliberately paid: DeviceService clears its pending-name
    // set with the key of the path handed back (src/DeviceService.cpp), so a
    // corrected extension leaves the original key pending until the profile
    // restarts.  That bumps one candidate name later; a file that lies about
    // what it is stays wrong forever.
    QString target = destPath;
    Wpd::ObjectInfo info;
    QString unreadable;
    if (wpd.objectInfo(id, &info, &unreadable)) {
        const QString want = Wpd::Session::extensionForFormat(info.format);
        const QFileInfo requested(destPath);
        if (!want.isEmpty()
            && requested.suffix().compare(want, Qt::CaseInsensitive) != 0) {
            const QString corrected = requested.path() + QLatin1Char('.') + want;
            if (QFile::exists(corrected)) {
                // Never overwrite something the user already has to improve a
                // guess; the requested name is the one this call was promised.
                fprintf(stderr, "[wpd] the player says .%s but %s already exists"
                        " - keeping the requested name\n", qUtf8Printable(want),
                        qUtf8Printable(corrected));
            } else {
                fprintf(stderr, "[wpd] the player says this object is .%s, not .%s:"
                        " saving as %s\n", qUtf8Printable(want),
                        qPrintable(requested.suffix()), qUtf8Printable(corrected));
                target = corrected;
            }
        }
    }

    if (!wpd.download(id, target, nullptr, nullptr, error)) {
        QFile::remove(target + QStringLiteral(".wpd-part"));
        fprintf(stderr, "[wpd] extract %u failed: %s\n", itemId,
                qUtf8Printable(error ? *error : QString()));
        return false;
    }
    m_wpdIds.insert(itemId, id);
    if (written)
        *written = target;
    return true;
}
#endif

void DeviceWorker::doSever() {
    if (m_dev) {
        m_devShared.store(nullptr);
        zune_sever(m_dev);
        m_dev = nullptr;
    }
#if defined(Q_OS_WIN)
    if (m_wpd) {
        // m_wpdShared goes false BEFORE the session closes, which is the same
        // handshake m_devShared uses: a reader that saw true may still touch a
        // live object, and a reader that saw false never touches a dead one.
        m_wpdShared.store(false);
        m_wpd->close();
        m_wpdIds.clear();
        m_wpdFolders.clear();
        m_wpdStorage.clear();
    }
#endif
    emit severDone();
}

void DeviceWorker::requestAbort() {
    // zune_abort just sets the device's volatile cancel flag — safe from
    // any thread. The handle can only vanish through doSever, which
    // clears m_devShared BEFORE freeing.
    if (ZuneDevice *dev = m_devShared.load())
        zune_abort(dev);
#if defined(Q_OS_WIN)
    // The session is allocated once and never freed while this object lives, so
    // the pointer is good even if the UI thread races a sever; m_wpdShared only
    // says whether flipping it would reach an open connection.
    if (m_wpdShared.load() && m_wpd)
        m_wpd->requestAbort();
#endif
}

void DeviceWorker::doPurgeItems(const QVariantList &itemIds) {
    int ok = 0, fail = 0;
    QVariantList purged;
    const int total = itemIds.size();
    fprintf(stderr, "[worker] purge: %d item(s) requested\n", total);
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        QStringList ids, gone;
        for (const QVariant &v : itemIds) {
            const quint32 handle = v.toUInt();
            ids << wpdObjectIdFor(handle);
            m_wpdIds.remove(handle);
        }
        QString why;
        // One object per delete call, so a player that refuses one object does
        // not take the rest of the selection with it.
        const bool allGone = m_wpd->removeObjects(ids, &gone, &why);
        ok = gone.size();
        fail = ids.size() - ok;
        for (const QString &id : std::as_const(gone)) {
            quint32 handle = 0;
            if (wpdHandleOf(id, &handle))
                purged.append(handle);
        }
        fprintf(stderr, "[wpd] purge: %d of %d deleted%s\n", ok, int(ids.size()),
                allGone ? "" : qPrintable(QStringLiteral(" - ") + why));
        emit purgeDone(ok, fail, purged);
        return;
    }
#endif
    for (int i = 0; i < total; i++) {
        if (!m_dev) { fail = total - i; break; }
        const quint32 id = itemIds[i].toUInt();
        emit purgeProgress(i + 1, total);
        if (zune_purge_track(m_dev, id) == 0) {  // generic DeleteObject
            fprintf(stderr, "[worker] purged %u ok\n", id);
            ok++;
            purged.append(id);
        } else {
            fprintf(stderr, "[worker] purge failed for %u (%s)\n", id,
                    zune_autopsy_name(zune_autopsy(m_dev)));
            fail++;
        }
        QThread::msleep(100); // firmware breathing room
    }
    emit purgeDone(ok, fail, purged);
}

void DeviceWorker::doPurgeInterruptedVideo(const QString &filename) {
    // Resolve ObjectFileName directly only when recovery is requested.
    // ZMDB exposes Name instead; matching that could delete an unrelated
    // episode called "Pilot". Refuse ambiguous filename matches too.
    QVariantList ids;
    if (m_dev && !filename.isEmpty()) {
        int count = 0;
        ZuneVideoFile *videos = zune_get_videos(m_dev, &count);
        for (int i = 0; videos && i < count; ++i) {
            const QString actual = QString::fromUtf8(videos[i].object_filename
                                                       ? videos[i].object_filename : "");
            if (VideoIdentity::matchesInterruptedFile({{"filename", actual}}, filename))
                ids.append(quint32(videos[i].item_id));
        }
        zune_free_videos(videos, count);
    }
    bool ok = false;
    if (ids.size() == 1) {
        emit purgeProgress(1, 1);
        ok = zune_purge_video(m_dev, ids.first().toUInt()) == 0;
    }
    fprintf(stderr, "[worker] interrupted video '%s': %d exact filename matches, purge %s\n",
            qUtf8Printable(filename), int(ids.size()), ok ? "OK" : "refused/failed");
    emit purgeDone(ok ? 1 : 0, ok ? 0 : 1, ok ? ids : QVariantList());
    emit interruptedVideoPurged(filename, ok);
}

void DeviceWorker::doPurgeInterruptedObject(const QVariantMap &record) {
    bool ok = false;
    const auto id = RecoveryIdentity::objectId(record.value("itemId"));
    if (m_dev && RecoveryIdentity::refusal(record, QString::fromUtf8(zune_get_serial(m_dev))).isEmpty()) {
        QVariantList matches;
        int count = 0;
        if (record.value("type") == "music") {
            auto *tracks = zune_get_tracks(m_dev, &count);
            for (int i = 0; tracks && i < count; ++i) {
                const auto &t = tracks[i];
                QVariantMap actual{{"itemId", t.item_id}, {"title", QString::fromUtf8(t.title)},
                    {"artist", QString::fromUtf8(t.artist)}, {"album", QString::fromUtf8(t.album)},
                    {"trackNumber", t.tracknumber}, {"discNumber", t.disc_number}};
                if (RecoveryIdentity::matches(record, actual)) matches.append(id);
            }
            zune_free_tracks(tracks, count);
        } else {
            auto *photos = zune_get_photos(m_dev, &count);
            for (int i = 0; photos && i < count; ++i)
                if (RecoveryIdentity::matches(record, {{"itemId", photos[i].item_id},
                        {"filename", QString::fromUtf8(photos[i].filename)}})) matches.append(id);
            zune_free_photos(photos, count);
        }
        // Re-read through the live serialized worker before deletion. Stale
        // UI rows, another device or an ID reused for other media cannot pass.
        if (matches.size() == 1) ok = zune_purge_track(m_dev, id) == 0;
    }
    emit purgeDone(ok ? 1 : 0, ok ? 0 : 1, ok ? QVariantList{id} : QVariantList{});
    emit interruptedObjectPurged(record.value("token").toString(), ok);
}

// ── Sync (Phase 5) ──

namespace {
struct SendProgressCtx {
    DeviceWorker *worker;
    const QString *entryId;
    double lastEmitted;
};

void sendProgressThunk(uint64_t sent, uint64_t total, void *userdata) {
    auto *ctx = static_cast<SendProgressCtx *>(userdata);
    if (total == 0)
        return;
    const double frac = double(sent) / double(total);
    // Throttle: a 100MB file at 64KB chunks would emit thousands of
    // queued signals — 1% steps are plenty for an 8px bar.
    if (frac - ctx->lastEmitted >= 0.01 || frac >= 1.0) {
        ctx->lastEmitted = frac;
        ctx->worker->notifySendProgress(*ctx->entryId, frac);
    }
}
} // namespace

void DeviceWorker::doSendTrack(const QString &entryId, const QString &filepath,
                               const QString &title, const QString &albumartist,
                               const QString &album, const QString &genre,
                               int trackNumber, int durationMs) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        sendTrackWpd(entryId, filepath, title, albumartist, album, genre,
                     trackNumber, durationMs);
        return;
    }
#endif
    if (!m_dev) {
        emit sendDone(entryId, false, 0, QStringLiteral("NotConnected"));
        return;
    }

    zune_clear_abort(m_dev);

    SendProgressCtx ctx{this, &entryId, 0.0};
    uint32_t itemId = 0;
    // Mac rule: the wire artist IS the albumartist — track-level artist
    // display comes from the artist objects forged in phase 2.
    const int rc = zune_smuggle_track_ex(
        m_dev, filepath.toUtf8().constData(),
        title.isEmpty() ? nullptr : title.toUtf8().constData(),
        albumartist.isEmpty() ? nullptr : albumartist.toUtf8().constData(),
        album.isEmpty() ? nullptr : album.toUtf8().constData(),
        genre.isEmpty() ? nullptr : genre.toUtf8().constData(),
        uint16_t(std::clamp(trackNumber, 0, 0xFFFF)),
        uint32_t(std::max(0, durationMs)),
        sendProgressThunk, &ctx, &itemId);

    const QString autopsy =
        QString::fromUtf8(zune_autopsy_name(zune_autopsy(m_dev)));

    if (rc != 0) {
        emit sendDone(entryId, false, itemId, autopsy);
        return;
    }

    // Verify readback — warning-only, never deletes (mac posture).
    //
    // The size argument used to be 0, which is the sentinel libzune reads as
    // "do not check the size" (track.c guards the whole comparison with
    // `expected_size > 0`), so the one property of a send that a byte count can
    // settle was never settled: a SendObject that came back short, or an object
    // the player truncated, was reported as a successful transfer.  The bytes we
    // handed the sender are the only honest expectation, and libzune compares
    // them against GetObjectInfo.object_compressed_size, which is the player's
    // own answer about what it kept rather than a restatement of ours.
    //
    // Still warn-only: there is no measured case here of a Zune keeping a wrong
    // object, and deleting on a suspicion is how a listener loses a file because
    // of a bug in the code doing the checking.  libzune prints both byte counts.
    const qint64 sentBytes = QFileInfo(filepath).size();
    if (zune_verify(m_dev, itemId,
                    title.isEmpty() ? nullptr : title.toUtf8().constData(),
                    nullptr, quint64(std::max<qint64>(0, sentBytes))) != 0) {
        fprintf(stderr, "[worker] verify mismatch for %s (item %u, %lld bytes sent)"
                        " — keeping\n",
                qUtf8Printable(title), itemId, (long long)sentBytes);
    }

    emit sendDone(entryId, true, itemId, autopsy);
}

void DeviceWorker::doSendVideo(const QString &entryId, const QString &filepath,
                               const QString &objectFilename, const QString &title, int metagenre,
                               const QString &description,
                               const QString &posterPath,
                               const QString &series, int season, int episode) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        // A Zune files a video by vendor metadata (metagenre, and 0xDA9A/0xDAB5/
        // 0xDAB6 for a series episode) and takes its poster as a representative
        // sample.  Plain MTP can put bytes in a folder; it cannot make the
        // player's video library understand them, and a file the browser does
        // not list is a sync that lied.
        noteWpdUnsupported("video transfer");
        emit sendVideoDone(entryId, false, 0,
                           UiText::tr("This player is on Windows' MTP driver, which cannot add videos to the Zune's video library. Connect it to WinUSB for video sync."));
        return;
    }
#endif
    if (!m_dev) {
        emit sendVideoDone(entryId, false, 0, QStringLiteral("NotConnected"));
        return;
    }

    zune_clear_abort(m_dev);

    QByteArray poster;
    if (!posterPath.isEmpty()) {
        QFile f(posterPath);
        if (f.open(QIODevice::ReadOnly))
            poster = f.readAll();
    }
    const uint8_t *posterData =
        poster.isEmpty() ? nullptr
                         : reinterpret_cast<const uint8_t *>(poster.constData());

    fprintf(stderr, "[worker] sending video: %s (file=%s metagenre=0x%02X)\n",
            qUtf8Printable(title), qUtf8Printable(objectFilename), metagenre);
    const QByteArray title8 = title.toUtf8();
    uint32_t itemId = 0;
    const int rc = zune_smuggle_video_named(m_dev, filepath.toUtf8().constData(),
        objectFilename.toUtf8().constData(), title8.constData(), uint16_t(metagenre),
        series.toUtf8().constData(), season, episode,
        description.toUtf8().constData(), posterData, size_t(poster.size()), &itemId);
    // A completed upload with metadata trouble must never be re-uploaded
    // automatically. Keep the real object ID and surface a repairable warning.
    QString warning;
    if (rc < 0) {
        emit sendVideoDone(entryId, false, itemId,
            QString::fromUtf8(zune_get_error()));
        return;
    }
    if (rc == ZUNE_VIDEO_METADATA_INCOMPLETE)
        warning = QStringLiteral("uploaded; ") + QString::fromUtf8(zune_get_error());
    // Name now has its own contract, so verify it as well as byte size.
    if (zune_verify(m_dev, itemId, title8.constData(), nullptr,
                    quint64(QFileInfo(filepath).size())) != 0) {
        if (!warning.isEmpty()) warning += QStringLiteral("; ");
        warning += UiText::tr("uploaded; title or size readback could not be verified");
    }
    emit sendVideoDone(entryId, true, itemId, warning);
}

void DeviceWorker::doForgePlaylist(const QString &name,
                                   const QVariantList &trackIds) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        noteWpdUnsupported("playlist creation");
        emit forgePlaylistDone(name, 0, false);
        return;
    }
#endif
    if (!m_dev || name.isEmpty()) {
        emit forgePlaylistDone(name, 0, false);
        return;
    }
    zune_clear_abort(m_dev);
    QVector<uint32_t> ids;
    ids.reserve(trackIds.size());
    for (const QVariant &v : trackIds)
        ids.append(v.toUInt());
    const quint32 plid = zune_forge_playlist(
        m_dev, name.toUtf8().constData(), ids.data(), int(ids.size()));
    if (plid == 0)
        fprintf(stderr, "[worker] playlist forge failed (%s)\n",
                zune_autopsy_name(zune_autopsy(m_dev)));
    emit forgePlaylistDone(name, plid, plid != 0);
}

void DeviceWorker::doSendPhoto(const QString &entryId, const QString &filepath,
                               const QString &albumName) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        // The libzune call arms and resizes the picture for the player before it
        // sends, and the device album is ZMDB state.  Dropping a raw JPEG into a
        // folder is a different operation that has not been measured, so it is
        // not offered as if it were the same feature.
        noteWpdUnsupported("photo transfer");
        emit sendPhotoDone(entryId, false, 0,
                           UiText::tr("This player is on Windows' MTP driver, which cannot add photos to the Zune's photo albums. Connect it to WinUSB for photo sync."));
        return;
    }
#endif
    if (!m_dev) {
        emit sendPhotoDone(entryId, false, 0, UiText::tr("no device"));
        return;
    }
    zune_clear_abort(m_dev);
    fprintf(stderr, "[worker] sending photo: %s (album='%s')\n",
            qUtf8Printable(QFileInfo(filepath).fileName()), qUtf8Printable(albumName));
    const int rc = zune_smuggle_photo(
        m_dev, filepath.toUtf8().constData(),
        albumName.isEmpty() ? nullptr : albumName.toUtf8().constData());
    const QString autopsy = rc == 0
        ? QString()
        : QString::fromUtf8(zune_autopsy_name(zune_autopsy(m_dev)));
    emit sendPhotoDone(entryId, rc == 0, 0, autopsy);
}

void DeviceWorker::doExtractPhoto(quint32 itemId, const QString &destPath) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        QString written, why;
        const bool ok = downloadWpd(itemId, destPath, &written, &why);
        emit extractPhotoDone(itemId, written.isEmpty() ? destPath : written, ok);
        return;
    }
#endif
    if (!m_dev) {
        emit extractPhotoDone(itemId, destPath, false);
        return;
    }
    zune_clear_abort(m_dev);
    const int rc = zune_extract_photo(m_dev, itemId,
                                      destPath.toUtf8().constData());
    if (rc != 0)
        fprintf(stderr, "[worker] photo extract failed for %u (%s)\n", itemId,
                zune_autopsy_name(zune_autopsy(m_dev)));
    emit extractPhotoDone(itemId, destPath, rc == 0);
}

void DeviceWorker::doExtractTrack(quint32 itemId, const QString &destPath) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        QString written, why;
        const bool ok = downloadWpd(itemId, destPath, &written, &why);
        emit extractTrackDone(itemId, written.isEmpty() ? destPath : written, ok);
        return;
    }
#endif
    if (!m_dev) {
        emit extractTrackDone(itemId, destPath, false);
        return;
    }
    zune_clear_abort(m_dev);
    fprintf(stderr, "[worker] extracting track %u → %s\n", itemId,
            qUtf8Printable(destPath));
    const int rc = zune_extract_track(m_dev, itemId,
                                      destPath.toUtf8().constData());
    if (rc != 0)
        fprintf(stderr, "[worker] track extract failed (%s)\n",
                zune_autopsy_name(zune_autopsy(m_dev)));
    emit extractTrackDone(itemId, destPath, rc == 0);
}

void DeviceWorker::doExtractVideo(quint32 itemId, const QString &destPath) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        // The container is the interesting part of a pull here: this player
        // states a format property, and the name it displays is a title.
        QString written, why;
        const bool ok = downloadWpd(itemId, destPath, &written, &why);
        emit extractVideoDone(itemId, written.isEmpty() ? destPath : written, ok);
        return;
    }
#endif
    if (!m_dev) {
        emit extractVideoDone(itemId, destPath, false);
        return;
    }
    zune_clear_abort(m_dev);
    fprintf(stderr, "[worker] extracting video %u → %s\n", itemId,
            qUtf8Printable(destPath));
    const int rc = zune_extract_video(m_dev, itemId,
                                      destPath.toUtf8().constData());
    if (rc != 0)
        fprintf(stderr, "[worker] video extract failed (%s)\n",
                zune_autopsy_name(zune_autopsy(m_dev)));
    emit extractVideoDone(itemId, destPath, rc == 0);
}

void DeviceWorker::doRenameItem(quint32 itemId, const QString &newName) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        noteWpdUnsupported("item rename");
        emit renameItemDone(itemId, newName, false);
        return;
    }
#endif
    if (!m_dev || newName.isEmpty()) {
        emit renameItemDone(itemId, newName, false);
        return;
    }
    const int rc = zune_rename_item(m_dev, itemId,
                                    newName.toUtf8().constData());
    emit renameItemDone(itemId, newName, rc == 0);
}

void DeviceWorker::doRefreshStorage() {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        // The player's own storage properties, which is the same ground truth
        // the other road gets from GetStorageInfo.
        Wpd::DeviceInfo info;
        QString why;
        if (m_wpd->readDevice(&info, &why)) {
            const double cap = double(info.capacityBytes) / 1e9;
            const double free = double(info.freeBytes) / 1e9;
            fprintf(stderr, "[wpd] storage refresh: %.1f GB free of %.1f GB\n",
                    free, cap);
            emit storageRefreshed(cap, free);
        } else {
            fprintf(stderr, "[wpd] storage refresh failed: %s\n", qUtf8Printable(why));
        }
        return;
    }
#endif
    if (!m_dev)
        return;
    const double cap = zune_get_capacity(m_dev) / 1e9;
    const double free = zune_get_headroom(m_dev) / 1e9;
    fprintf(stderr, "[worker] storage refresh: %.1f GB free of %.1f GB\n",
            free, cap);
    emit storageRefreshed(cap, free);
}

void DeviceWorker::doRenameDevice(const QString &newName) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        // Renaming the player is vendor property 0xD402.  WPD has a friendly
        // name property too; whether a Zune provider obeys a write to it has
        // never been asked here, and asking is how an untested write is learned
        // about, so this stays a refusal until someone measures it.
        noteWpdUnsupported("device rename");
        emit renameDeviceDone(newName, false);
        return;
    }
#endif
    if (!m_dev || newName.isEmpty()) {
        emit renameDeviceDone(newName, false);
        return;
    }
    const int rc = zune_rename(m_dev, newName.toUtf8().constData());
    fprintf(stderr, "[worker] rename device -> '%s': %s\n",
            qUtf8Printable(newName), rc == 0 ? "OK" : "FAILED");
    emit renameDeviceDone(newName, rc == 0);
}

void DeviceWorker::doSyncNotify(const QString &name, int itemIndex,
                                int totalItems) {
    if (!m_dev)
        return;
    // Wire semantics (capture-bulkreading.pcapng): op_kind 0 = writing
    // TO the device (what the display reacts to), progress fields are
    // whole-batch PERCENTAGES, item index is 1-based. itemIndex ==
    // totalItems is the terminal "sync complete" notify (100/100);
    // the wire index clamps to the batch size.
    const int total = qMax(1, totalItems);
    const quint32 before = quint32(qMin(100, itemIndex * 100 / total));
    const quint32 after = quint32(qMin(100, (itemIndex + 1) * 100 / total));
    zune_sync_notify(m_dev, name.toUtf8().constData(), 0,
                     quint32(qMin(itemIndex + 1, total)), quint32(total),
                     before, after);
}

void DeviceWorker::doForgeAlbums(const QVariantList &buckets) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        // Album objects are ZMDB records, so there is nothing to forge here:
        // sendTrackWpd() has already filed every file of this batch under
        // <artist>/<album>/, and on a plain MTP player those folders are the
        // grouping.  The zeros are not politeness - SyncEngine::onForgeDone reads
        // a non-zero failCount as an error and would mark a good sync broken.
        fprintf(stderr, "[wpd] album forge skipped: over MTP the folders are the"
                " album grouping (%d bucket(s))\n", int(buckets.size()));
        emit forgeDone(0, 0);
        return;
    }
#endif
    if (!m_dev || buckets.isEmpty()) {
        emit forgeDone(0, m_dev ? 0 : buckets.size());
        return;
    }

    int okCount = 0, failCount = 0;

    // (a) libzune reuses existing device artists; this map additionally
    // coalesces repeated album owners within the current batch.
    QHash<QString, quint32> artistHandles; // lower(name) → handle
    for (const QVariant &v : buckets) {
        const QString aa = v.toMap().value("albumartist").toString();
        const QString key = aa.toLower();
        if (aa.isEmpty() || artistHandles.contains(key))
            continue;
        const uint32_t h = zune_forge_artist(m_dev, aa.toUtf8().constData());
        artistHandles.insert(key, h); // 0 = failed; links skipped below
        if (h == 0)
            fprintf(stderr, "[worker] forge_artist returned 0 for %s "
                    "(unsupported, unreadable inventory or creation failure; "
                    "album artist metadata is retained)\n", qUtf8Printable(aa));
    }
    QThread::msleep(500); // firmware breathing room after artist forging

    // (b) Existing device albums for merge, keyed lower(artist\tname)
    // PLUS a name-only index: albums synced by older clients carry
    // unpredictable Artist strings (pre-albumartist-fix era), and the
    // device identifies albums by NAME anyway — creating a same-named
    // album fails 0x2002. Hardware-observed on the first sync gate.
    struct Existing { quint32 id; QVector<quint32> trackIds; };
    QHash<QString, Existing> existing;
    QHash<QString, Existing> existingByName;
    {
        int n = 0;
        if (ZuneAlbumObject *objs = zune_get_albums(m_dev, &n)) {
            for (int i = 0; i < n; i++) {
                const QString name =
                    QString::fromUtf8(objs[i].name ? objs[i].name : "");
                const QString key =
                    (QString::fromUtf8(objs[i].artist ? objs[i].artist : "")
                     + QLatin1Char('\t') + name).toLower();
                Existing e{objs[i].album_id, {}};
                for (uint32_t t = 0; t < objs[i].track_count; t++)
                    e.trackIds.append(objs[i].track_ids[t]);
                existing.insert(key, e);
                if (!existingByName.contains(name.toLower()))
                    existingByName.insert(name.toLower(), e);
            }
            zune_free_albums(objs, n);
        }
    }

    // (c) Per bucket: merge-or-create, link, brand
    const int total = buckets.size();
    for (int b = 0; b < total; b++) {
        if (zune_is_aborted(m_dev)) {
            failCount += total - b;
            break;
        }
        emit forgeProgress(b + 1, total);

        const QVariantMap bucket = buckets[b].toMap();
        const QString albumartist = bucket.value("albumartist").toString();
        const QString album = bucket.value("album").toString();
        const QString genre = bucket.value("genre").toString();
        const QString artPath = bucket.value("artJpegPath").toString();

        QVector<quint32> trackIds;
        for (const QVariant &t : bucket.value("trackIds").toList())
            trackIds.append(t.toUInt());
        if (album.isEmpty() || trackIds.isEmpty()) {
            failCount++;
            continue;
        }

        quint32 albumId = 0;
        const QString mergeKey =
            (albumartist + QLatin1Char('\t') + album).toLower();
        const Existing *match = nullptr;
        if (existing.contains(mergeKey))
            match = &existing[mergeKey];
        else if (existingByName.contains(album.toLower())) {
            match = &existingByName[album.toLower()];
            fprintf(stderr, "[worker] album '%s' matched by NAME only "
                    "(device artist string differs) — merging\n",
                    qUtf8Printable(album));
        }
        if (match) {
            // Merge: existing ids + new ones, deduped, order preserved
            QVector<quint32> merged = match->trackIds;
            for (quint32 id : std::as_const(trackIds))
                if (!merged.contains(id))
                    merged.append(id);
            std::vector<uint32_t> ids(merged.begin(), merged.end());
            if (zune_rewire_album(m_dev, match->id, album.toUtf8().constData(),
                                  albumartist.toUtf8().constData(),
                                  ids.data(), int(ids.size())) == 0) {
                albumId = match->id;
            }
        } else {
            std::vector<uint32_t> ids(trackIds.begin(), trackIds.end());
            albumId = zune_forge_album(m_dev, album.toUtf8().constData(),
                                       albumartist.toUtf8().constData(),
                                       genre.isEmpty() ? nullptr
                                                       : genre.toUtf8().constData(),
                                       ids.data(), int(ids.size()));
        }

        if (albumId == 0) {
            fprintf(stderr, "[worker] forge/rewire album failed: %s (%s)\n",
                    qUtf8Printable(album),
                    zune_autopsy_name(zune_autopsy(m_dev)));
            failCount++;
            QThread::msleep(300);
            continue;
        }

        // (d) Artist links: album AND each track (ArtistId 0xDAB9)
        const quint32 artistHandle = artistHandles.value(albumartist.toLower(), 0);
        if (artistHandle != 0) {
            zune_link_artist(m_dev, albumId, artistHandle);
            for (quint32 tid : std::as_const(trackIds))
                zune_link_artist(m_dev, tid, artistHandle);
        }

        // (f) Album art via representative sample
        if (!artPath.isEmpty()) {
            QFile f(artPath);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray jpeg = f.readAll();
                if (!jpeg.isEmpty())
                    zune_brand(m_dev, albumId,
                               reinterpret_cast<const uint8_t *>(jpeg.constData()),
                               size_t(jpeg.size()));
            }
        }

        okCount++;
        QThread::msleep(300); // mac's inter-album breathing room
    }

    emit forgeDone(okCount, failCount);
}

void DeviceWorker::doFinalize() {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        // Nothing to commit.  A ZMDB finalize asks the player to fold vendor
        // records into its library; over MTP a file has been in its folder since
        // the transfer closed.  Read this as "nothing was queued", which is not
        // the same claim as "the player re-indexed" - that was never measured.
        fprintf(stderr, "[wpd] finalize: nothing pending - MTP has no vendor"
                " re-index to wait for\n");
        emit finalizeDone(true);
        return;
    }
#endif
    if (!m_dev) {
        emit finalizeDone(false);
        return;
    }
    emit finalizeDone(zune_finalize(m_dev) == 0);
}

void DeviceWorker::doGrabPhotoThumb(quint32 itemId, const QString &cachePath) {
    if (QFile::exists(cachePath)) {
        emit artDone(itemId, cachePath, true);
        return;
    }
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        if (!m_wpdArtReported) {
            m_wpdArtReported = true;
            fprintf(stderr, "[wpd] this player keeps no picture for its objects"
                    " over MTP: GetSupportedResources names DEFAULT only, and a"
                    " THUMBNAIL or ALBUM_ART stream fails with 0x8007138E. Art"
                    " comes from a file's own tag instead.\n");
        }
        emit artDone(itemId, cachePath, false);
        return;
    }
#endif
    if (!m_dev) {
        emit artDone(itemId, cachePath, false);
        return;
    }
    int rc = zune_grab_photo_thumb(m_dev, itemId, cachePath.toUtf8().constData());
    emit artDone(itemId, cachePath, rc == 0 && QFile::exists(cachePath));
}

void DeviceWorker::doGrabArt(quint32 itemId, const QString &cachePath) {
    if (QFile::exists(cachePath)) {
        emit artDone(itemId, cachePath, true);
        return;
    }
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        // Same measured answer as the photo thumbnail above, for the same
        // player: no stream exists to ask for.  The album's picture is the one
        // embedded in the audio file, which the library already reads.
        if (!m_wpdArtReported) {
            m_wpdArtReported = true;
            fprintf(stderr, "[wpd] no album art over MTP on this player (see the"
                    " thumbnail note above)\n");
        }
        emit artDone(itemId, cachePath, false);
        return;
    }
#endif
    if (!m_dev) {
        emit artDone(itemId, cachePath, false);
        return;
    }
    int rc = zune_grab_thumb(m_dev, itemId, cachePath.toUtf8().constData());
    emit artDone(itemId, cachePath, rc == 0 && QFile::exists(cachePath));
}

namespace {
// One complete read plus exact feed/enclosure resolution. Unknown state remains
// absent from the plan. All failures discard the entire plan before any write.
QString inspectPlayedPodcasts(ZuneDevice *dev, const QVariantList &episodes,
                              const std::function<bool()> &current,
                              QVariantList &result) {
    result.clear();
    if (episodes.size() > 5000)
        return UiText::tr("Too many podcast episodes selected for cleanup.");
    QSet<QString> localIds;
    QSet<QPair<QString, QString>> identities, sources;
    for (const auto &value : episodes) {
        const auto row = value.toMap();
        const auto id = row.value("spokenId").toString();
        const auto identity = row.value("identity").toString();
        const auto feed = row.value("feedUrl").toString();
        const auto source = row.value("sourceUrl").toString();
        const auto valid = [](const QString &s) { return !s.trimmed().isEmpty() && !s.contains(QChar(0)); };
        if (!valid(id) || !valid(identity) || !valid(feed) || !valid(source)
            || localIds.contains(id) || identities.contains({feed, identity}) || sources.contains({feed, source}))
            return UiText::tr("Podcast cleanup identities are missing or ambiguous.");
        localIds.insert(id); identities.insert({feed, identity}); sources.insert({feed, source});
    }
    const auto active = [&] { return current() && !zune_is_aborted(dev); };
    if (!active()) return UiText::tr("Podcast cleanup was cancelled or the Zune changed.");
    struct Snapshot {
        ZunePodcastState *states = nullptr;
        ZunePodcastEpisode *episodes = nullptr;
        ZunePodcastEpisodeSource *sources = nullptr;
        int stateCount = 0, episodeCount = 0, sourceCount = 0;
        ~Snapshot() {
            zune_free_podcast_states(states, stateCount);
            zune_free_podcast_episodes(episodes, episodeCount);
            zune_free_podcast_episode_sources(sources, sourceCount);
        }
    } snapshot;
    // A complete empty live inventory is sufficient to prove there is nothing
    // to clean up. Do not require firmware listening-state support in that case.
    if (zune_get_podcast_episodes(dev, &snapshot.episodes, &snapshot.episodeCount) || !active())
        return UiText::tr("Could not read complete podcast inventory. Nothing was deleted.");
    if (snapshot.episodeCount == 0) return {};
    if (zune_get_podcast_states(dev, &snapshot.states, &snapshot.stateCount) || !active()) {
        const auto family = zune_get_family(dev);
        if (active() && family != ZUNE_FAMILY_KEEL && family != ZUNE_FAMILY_DRACO
            && family != ZUNE_FAMILY_PAVO)
            return UiText::tr("Played-podcast cleanup is not supported on this Zune model yet. Episodes were kept; syncing can continue.");
        return UiText::tr("Could not read complete podcast listening state. Nothing was deleted.");
    }
    QHash<quint32, ZunePodcastState> states;
    QHash<quint32, ZunePodcastEpisode> live;
    for (int i = 0; i < snapshot.stateCount; ++i) {
        const auto &state = snapshot.states[i];
        if (!state.item_id || state.item_id == 0xffffffffu || states.contains(state.item_id))
            return UiText::tr("Podcast listening state contains ambiguous device IDs.");
        states.insert(state.item_id, state);
    }
    for (int i = 0; i < snapshot.episodeCount; ++i) {
        const auto &episode = snapshot.episodes[i];
        if (!episode.item_id || episode.item_id == 0xffffffffu || live.contains(episode.item_id))
            return UiText::tr("Podcast inventory contains ambiguous device IDs.");
        live.insert(episode.item_id, episode);
    }
    bool anyPlayed = false;
    for (const auto &state : states)
        anyPlayed = anyPlayed || (state.marked_played && (state.raw_flags & 0x200u));
    // Most syncs have nothing finished. Avoid repeated exact USB identity
    // searches when the complete snapshot proves there are no candidates.
    if (!anyPlayed) {
        if (!active()) return UiText::tr("Podcast cleanup was cancelled or the Zune changed.");
        return {};
    }
    if (zune_get_podcast_episode_sources(dev, &snapshot.sources, &snapshot.sourceCount) || !active())
        return UiText::tr("Could not verify complete podcast source identities. Nothing was deleted.");
    QHash<QPair<QString, QString>, ZunePodcastEpisodeSource> deviceSources;
    QSet<quint32> sourceIds;
    for (int i = 0; i < snapshot.sourceCount; ++i) {
        const auto &source = snapshot.sources[i];
        const auto key = qMakePair(QString::fromUtf8(source.feed_url), QString::fromUtf8(source.source_url));
        if (!source.item_id || source.item_id == 0xffffffffu || !source.series_id || source.series_id == 0xffffffffu
            || key.first.isEmpty() || key.second.isEmpty() || sourceIds.contains(source.item_id) || deviceSources.contains(key))
            return UiText::tr("Podcast source inventory contains ambiguous identities.");
        sourceIds.insert(source.item_id);
        deviceSources.insert(key, source);
    }
    QVariantList planned;
    QSet<quint32> mappedIds;
    for (const auto &value : episodes) {
        if (!active()) return UiText::tr("Podcast cleanup was cancelled or the Zune changed.");
        auto row = value.toMap();
        if (row.value("keepOnDevice").toBool()) continue;
        const auto key = qMakePair(row.value("feedUrl").toString(), row.value("sourceUrl").toString());
        if (!deviceSources.contains(key)) continue;
        const auto source = deviceSources.value(key);
        const quint32 episode = source.item_id, show = source.series_id;
        if (!show || show == 0xffffffffu || mappedIds.contains(episode))
            return UiText::tr("Podcast cleanup matched an ambiguous device identity.");
        mappedIds.insert(episode);
        if (!states.contains(episode)) continue; // Missing is unknown, never played.
        const auto state = states.value(episode);
        if (!live.contains(episode) || state.series_id != show || live.value(episode).series_id != show
            || source.parent_id != live.value(episode).parent_id
            || state.duration_ms != live.value(episode).duration_ms
            || !state.title || QString::fromUtf8(state.title) != QString::fromUtf8(live.value(episode).title))
            return UiText::tr("Podcast identity and listening inventory disagree. Nothing was deleted.");
        if (!state.marked_played || !(state.raw_flags & 0x200u)) continue;
        const auto e = live.value(episode);
        row["itemId"] = episode; row["seriesId"] = show;
        row["durationMs"] = state.duration_ms; row["bookmarkMs"] = state.bookmark_ms;
        row["rawFlags"] = state.raw_flags;
        row["parentId"] = e.parent_id;
        row["sizeBytes"] = QVariant::fromValue<qulonglong>(e.filesize);
        row["deviceTitle"] = QString::fromUtf8(e.title);
        row["deviceAuthor"] = QString::fromUtf8(e.author);
        row["deviceDurationMs"] = e.duration_ms;
        planned.append(row);
    }
    if (!active()) return UiText::tr("Podcast cleanup was cancelled or the Zune changed.");
    result = planned;
    return {};
}
}

void DeviceWorker::doInspectPodcastCleanup(quint64 generation, const QString &token,
                                           const QString &serial, const QVariantList &episodes) {
    QVariantList rows;
    QString error;
    const auto current = [this, generation, &serial] {
        return m_dev && generation == m_spokenGeneration.load() && !serial.isEmpty()
            && QString::fromUtf8(zune_get_serial(m_dev)) == serial;
    };
    if (token.isEmpty() || !current())
        error = UiText::tr("The Zune disconnected or changed. Podcast cleanup was skipped.");
    else {
        zune_clear_abort(m_dev);
        error = inspectPlayedPodcasts(m_dev, episodes, current, rows);
    }
    emit podcastCleanupInspected(generation, token, serial, rows, error);
}

void DeviceWorker::doPurgePlayedPodcasts(quint64 generation, const QString &token,
                                         const QString &serial, const QVariantList &rows) {
    QVariantList live, purged;
    QString error;
    const auto current = [this, generation, &serial] {
        return m_dev && generation == m_spokenGeneration.load() && !serial.isEmpty()
            && QString::fromUtf8(zune_get_serial(m_dev)) == serial;
    };
    if (token.isEmpty() || !current())
        error = UiText::tr("The Zune disconnected or changed. Nothing was deleted.");
    else {
        // Do not clear abort here: cancellation between planning and this queued
        // write must survive. Revalidate ALL rows before the first deletion.
        error = inspectPlayedPodcasts(m_dev, rows, current, live);
        if (error.isEmpty() && live != rows)
            error = UiText::tr("Podcast listening state or identity changed. Nothing was deleted.");
    }
    if (error.isEmpty()) {
        for (const auto &value : rows) {
            if (!current() || zune_is_aborted(m_dev)) {
                error = UiText::tr("Podcast cleanup was cancelled or the Zune changed.");
                break;
            }
            const auto id = RecoveryIdentity::objectId(value.toMap().value("itemId"));
            emit podcastCleanupProgress(generation, token, serial, id, false);
            if (!current() || zune_is_aborted(m_dev)) {
                error = UiText::tr("Podcast cleanup was cancelled or the Zune changed.");
                break;
            }
            if (!id || id == 0xffffffffu || zune_purge_track(m_dev, id) != 0) {
                error = UiText::tr("The Zune could not confirm deletion of a played podcast.");
                break;
            }
            purged.append(value); // Only a successful device response retires it.
            emit podcastCleanupProgress(generation, token, serial, id, true);
        }
    }
    emit playedPodcastsPurged(generation, token, serial, purged, error);
}

void DeviceWorker::doInspectPodcasts(const QString &token, const QString &serial,
                                     const QVariantList &episodes) {
    QVariantList result;
    QString error;
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        noteWpdUnsupported("podcast episode lookup");
        emit podcastsInspected(token, serial, result,
                               UiText::tr("This player is on Windows' MTP driver, which has no podcast or audiobook library to read."));
        return;
    }
#endif
    if (!m_dev || QString::fromUtf8(zune_get_serial(m_dev)) != serial)
        error = UiText::tr("The Zune disconnected or changed; add the episodes again.");
    else {
        zune_clear_abort(m_dev);
        for (const auto &value : episodes) {
            auto row = value.toMap();
            uint32_t folder = 0, show = 0, episode = 0;
            const auto feed = row.value("feedUrl").toString().toUtf8();
            const auto source = row.value("sourceUrl").toString().toUtf8();
            if (zune_find_podcast_episode(m_dev, feed.constData(), source.constData(), &folder, &show, &episode)) {
                error = QString::fromUtf8(zune_get_error());
                break;
            }
            row["onDevice"] = episode != 0;
            result.append(row);
        }
    }
    emit podcastsInspected(token, serial, result, error);
}

void DeviceWorker::doSendPodcast(const QString &entryId, const QString &filepath,
                                  const QVariantMap &episode) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        // A podcast folder and show object are forged with vendor calls, and the
        // feed/source identity lives in ZMDB, so there is nowhere to put an
        // episode that the player would still recognise as one.
        noteWpdUnsupported("podcast transfer");
        emit podcastSendDone(entryId, false, 0,
                             UiText::tr("This player is on Windows' MTP driver, which has no podcast library to add to. Connect it to WinUSB for podcasts."),
                             false);
        return;
    }
#endif
    if (!m_dev) {
        emit podcastSendDone(entryId, false, 0, UiText::tr("Zune disconnected"), false);
        return;
    }
    zune_clear_abort(m_dev);
    uint32_t folder = 0, show = 0, id = 0;
    const auto feed = episode.value("feedUrl").toString().toUtf8();
    const auto source = episode.value("sourceUrl").toString().toUtf8();
    auto fail = [&](int result = -1) {
        emit podcastSendDone(entryId, false, id, QString::fromUtf8(zune_get_error()), false,
                             result != ZUNE_PODCAST_FAILED_CLEANED);
    };
    if (zune_find_podcast_episode(m_dev, feed.constData(), source.constData(), &folder, &show, &id)) {
        fail(); return;
    }
    // Live preflight repeats after preparation. Existing objects are never sent
    // again or rewritten, including a previous run whose receipt was lost.
    if (id) {
        emit podcastSendDone(entryId, true, id, QString(), true); return;
    }
    const auto title = episode.value("title").toString().toUtf8();
    const auto author = episode.value("artist").toString().toUtf8();
    auto name = episode.value("album").toString().toUtf8();
    const auto description = episode.value("description").toString().toUtf8();
    const auto date = episode.value("dateAuthored").toString().toUtf8();
    if (!show) {
        const auto folderName = (QStringLiteral("Zuuned Podcasts ") + entryId).toUtf8();
        folder = zune_forge_folder(m_dev, folderName.constData(), UINT32_MAX);
        if (!folder || folder == UINT32_MAX) { fail(); return; }
        const int result = zune_forge_podcast_series(m_dev, folder, "Podcast.ser", name.constData(),
                                      author.constData(), feed.constData(), &show);
        if (result) {
            id = show; fail(result); return;
        }
    } else {
        // Feed display titles can change. Preserve the live device show's name.
        ZunePodcastSeries *series = nullptr; int count = 0;
        if (zune_get_podcast_series(m_dev, &series, &count)) { fail(); return; }
        for (int i = 0; i < count; ++i)
            if (series[i].item_id == show) name = series[i].name;
        zune_free_podcast_series(series, count);
    }
    ZunePodcastEpisodeInput input{};
    input.filename = "Episode.mp3";
    input.title = title.constData(); input.author = author.constData();
    input.series_name = name.constData(); input.source_url = source.constData();
    input.description = description.constData(); input.date_authored = date.constData();
    input.series_id = show; input.duration_ms = episode.value("durationMs").toUInt();
    const auto path = filepath.toUtf8();
    emit sendProgress(entryId, 0);
    const int result = zune_smuggle_podcast_episode(m_dev, folder, path.constData(), &input, &id);
    if (result) {
        fail(result); return;
    }
    emit sendProgress(entryId, 1);
    // Representative-sample artwork is optional. Refusal must not turn a
    // verified audio upload into a failed item that the user might retry.
    QString warning;
    QFile art(episode.value("posterPath").toString());
    if (art.exists() && art.size() > 0 && art.size() <= 10 * 1024 * 1024 && art.open(QIODevice::ReadOnly)) {
        const auto bytes = art.readAll();
        if (zune_brand(m_dev, show, reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size())))
            warning = UiText::tr("Episode synced; this device did not accept the show cover.");
    }
    emit podcastSendDone(entryId, true, id, warning, false);
}

namespace {
struct BookTransferInput {
    QByteArray key, name, author, title;
    ZuneAudiobookPartInput input{};
    explicit BookTransferInput(const QVariantMap &row)
        : key(row.value("bookKey").toString().toUtf8()), name(row.value("album").toString().toUtf8()),
          author(row.value("artist").toString().toUtf8()), title(row.value("title").toString().toUtf8()) {
        input.book_key=key.constData(); input.book_title=name.constData();
        input.author=author.constData(); input.title=title.constData();
        input.part_number=quint16(row.value("partNumber").toUInt());
        input.duration_ms=row.value("durationMs").toUInt();
    }
};
}
void DeviceWorker::doInspectAudiobooks(const QString &token, const QString &serial, const QVariantList &parts) {
    QVariantList result; QString error;
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        noteWpdUnsupported("audiobook part lookup");
        emit audiobooksInspected(token, serial, result,
                                 UiText::tr("This player is on Windows' MTP driver, which has no podcast or audiobook library to read."));
        return;
    }
#endif
    if (!m_dev || QString::fromUtf8(zune_get_serial(m_dev)) != serial)
        error=UiText::tr("The Zune disconnected or changed; add the book again.");
    else {
        zune_clear_abort(m_dev);
        for (const auto &value : parts) {
            auto row=value.toMap(); BookTransferInput p(row); uint32_t folder=0,id=0;
            if (row.value("partNumber").toInt()<1 || row.value("partNumber").toInt()>65535 ||
                zune_find_audiobook_part(m_dev,&p.input,&folder,&id)) {
                error=QString::fromUtf8(zune_get_error()); break;
            }
            row["onDevice"]=id!=0;result.append(row);
        }
    }
    emit audiobooksInspected(token,serial,result,error);
}
void DeviceWorker::doSendAudiobook(const QString &entryId, const QString &filepath, const QVariantMap &part) {
#if defined(Q_OS_WIN)
    if (!m_dev && m_wpdShared.load() && m_wpd) {
        noteWpdUnsupported("audiobook transfer");
        emit audiobookSendDone(entryId, false, 0,
                               UiText::tr("This player is on Windows' MTP driver, which has no audiobook library to add to. Connect it to WinUSB for audiobooks."),
                               false);
        return;
    }
#endif
    if (!m_dev) { emit audiobookSendDone(entryId,false,0,"Zune disconnected",false); return; }
    zune_clear_abort(m_dev);
    if (part.value("partNumber").toInt()<1 || part.value("partNumber").toInt()>65535) {
        emit audiobookSendDone(entryId,false,0,UiText::tr("Invalid audiobook part order"),false);return;
    }
    BookTransferInput p(part); uint32_t folder=0,id=0;
    if (zune_find_audiobook_part(m_dev,&p.input,&folder,&id)) {
        emit audiobookSendDone(entryId,false,0,QString::fromUtf8(zune_get_error()),false);return;
    }
    if(id) { emit audiobookSendDone(entryId,true,id,QString(),true);return; }
    emit sendProgress(entryId,0);
    if(zune_smuggle_audiobook_part(m_dev,filepath.toUtf8().constData(),&p.input,&id)) {
        emit audiobookSendDone(entryId,false,id,QString::fromUtf8(zune_get_error()),false);return;
    }
    emit sendProgress(entryId,1);
    QString warning;
    QFile art(part.value("posterPath").toString());
    if(art.exists() && art.size()>0 && art.size()<=10*1024*1024 && art.open(QIODevice::ReadOnly)) {
        const auto bytes=art.readAll();
        if(zune_brand(m_dev,id,reinterpret_cast<const uint8_t*>(bytes.constData()),size_t(bytes.size())))
            warning=UiText::tr("Book part synced; this device did not accept its cover.");
    }
    emit audiobookSendDone(entryId,true,id,warning,false);
}
