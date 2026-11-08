/*
 * librarytool — CLI gate for the library core.
 *
 *   librarytool scan <folder> <tmpdb>
 *     Registers <folder> as a music watch folder in <tmpdb>, scans it
 *     (synchronously, on this thread), prints per-chunk progress, final
 *     counts, and 5 sample rows.
 *
 *   librarytool videoscan <folder> <tmpdb> [type]
 *     Phase 6 gate: registers <folder> as a video watch folder
 *     (type: movies|tv|anime, default tv), scans, then runs the TMDB
 *     matcher SYNCHRONOUSLY on every unmatched row and prints what
 *     resolved — series, TMDB titles, poster paths.
 *
 *   librarytool names
 *     VideoNaming self-test: the parsing cases the mac fixed one bug at
 *     a time, frozen as expectations.
 *
 *   librarytool paths
 *     Path/text self-test: what a filename may contain on the host OS, how two
 *     spellings of one file compare, how a playlist line resolves, and how
 *     playlist bytes decode.  Expectations are branched where the right answer
 *     genuinely differs by platform, so one source gates both.
 *
 *   librarytool folders
 *     Watch-folder self-test: what a folder owns when its name is not ASCII,
 *     and which folder a download is written into -- the one the user marked,
 *     else the first of that kind the library holds.
 *
 *   librarytool videofloor
 *     Video refusal self-test: which video files a watch folder is allowed to
 *     discard on size, the one folder it may never discard them in, and the
 *     note the scan owes a user for every file it did discard.
 */
/* Keep first on Windows: the compat shim declares the POSIX surface this
 * file uses (64-bit struct stat, dirent, poll, ...) and the console code page
 * helper below.  Whichever <sys/stat.h> the compiler parses first decides the
 * struct layout, and the Qt headers include one.  See compat/win/posix_shim.h.
 * Other platforms build without it.  */
#if defined(_WIN32)
#include "wincompat.h"
#endif

#include "LibraryDb.h"
#include "LibraryPaths.h"
#include "LibraryScanner.h"

// The upgrade test rewinds a database by hand to the shape the previous
// build wrote, which is a job for the library the database is made of.
#include <sqlite3.h>
#include "MusicIdentity.h"
#include "MusicIdentityClient.h"
#include "NetworkAccess.h"
#include "PlaylistText.h"
#include "VideoIdentity.h"
#include "VideoMatcher.h"
#include "VideoNaming.h"

#include <QCoreApplication>
#include <QStringList>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QUrl>
#include <QUrlQuery>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QProcess>
#include <sqlite3.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <cstdio>
#include <string>

static int runNamesSelfTest() {
    struct Case {
        const char *what;
        QString got;
        QString expect;
    };
    const QVector<Case> cases = {
        {"normalize: dots→spaces",
         VideoNaming::normalizeSeriesName(QStringLiteral("SpongeBob.SquarePants")),
         QStringLiteral("SpongeBob SquarePants")},
        {"normalize: tracker prefix",
         VideoNaming::normalizeSeriesName(QStringLiteral("www.UIndex.org    -    ONE PIECE")),
         QStringLiteral("ONE PIECE")},
        {"normalize: [group] prefix",
         VideoNaming::normalizeSeriesName(QStringLiteral("[SubsPlease] Frieren")),
         QStringLiteral("Frieren")},
        {"folder junk truncation",
         VideoNaming::cleanFolderSeriesName(
             QStringLiteral("Adventure Time S01 1080p HMAX WEBRip DD2.0 x265-Kappa")),
         QStringLiteral("Adventure Time")},
        {"folder junk keeps clean names",
         VideoNaming::cleanFolderSeriesName(QStringLiteral("The Office (US) (2005)")),
         QStringLiteral("The Office (US) (2005)")},
        {"derive: season container",
         VideoNaming::deriveSeriesFromPath(
             QStringLiteral("/tv/Breaking Bad/Season 1/Breaking.Bad.S01E01.mkv")),
         QStringLiteral("Breaking Bad")},
        {"derive: episode-dir layout",
         VideoNaming::deriveSeriesFromPath(
             QStringLiteral("/tv/The Wire/The.Wire.S02E03.720p/The.Wire.S02E03.mkv")),
         QStringLiteral("The Wire")},
        {"derive: specials",
         VideoNaming::deriveSeriesFromPath(
             QStringLiteral("/tv/Doctor Who/Specials/xmas.mkv")),
         QStringLiteral("Doctor Who")},
        {"clean: scene movie name",
         VideoNaming::cleanFilenameForSearch(
             QStringLiteral("Die.Hard.1988.1080p.BluRay.x264-SARTRE.mkv")),
         QStringLiteral("Die Hard 1988")},
        {"clean: keeps real subtitle",
         VideoNaming::cleanFilenameForSearch(
             QStringLiteral("Mission Impossible - Fallout.mkv")),
         QStringLiteral("Mission Impossible Fallout")},
        {"clean: paren year preserved",
         VideoNaming::cleanFilenameForSearch(
             QStringLiteral("Hackers (1995) [1080p].mp4")),
         QStringLiteral("Hackers 1995")},
        {"clean: trailing disc idx after year",
         VideoNaming::cleanFilenameForSearch(
             QStringLiteral("Back To The Future Part III 1990 1.mkv")),
         QStringLiteral("Back To The Future Part III 1990")},
        {"clean: Apollo 13 stays intact",
         VideoNaming::cleanFilenameForSearch(QStringLiteral("Apollo 13.mkv")),
         QStringLiteral("Apollo 13")},
        {"squash equality",
         VideoNaming::squash(QStringLiteral("Bob's Burgers")),
         QStringLiteral("bobsburgers")},
    };

    int failed = 0;
    for (const Case &c : cases) {
        const bool ok = c.got == c.expect;
        if (!ok)
            failed++;
        printf("  %s %s: got '%s'%s\n", ok ? "PASS" : "FAIL", c.what,
               c.got.toUtf8().constData(),
               ok ? "" : (QStringLiteral(" expected '") + c.expect
                          + QLatin1Char('\'')).toUtf8().constData());
    }

    // Scoring sanity: exact squash match wins; junk stays below threshold.
    const double exact = VideoNaming::scoreCandidate(
        QStringLiteral("bobs burgers"), QString(),
        QStringLiteral("Bob's Burgers"), QStringLiteral("2011"), 100);
    const double junk = VideoNaming::scoreCandidate(
        QStringLiteral("some totally different show"), QString(),
        QStringLiteral("Bob's Burgers"), QStringLiteral("2011"), 100);
    printf("  %s score exact-squash (%.2f >= 1.0)\n",
           exact >= 1.0 ? "PASS" : "FAIL", exact);
    printf("  %s score junk rejected (%.2f < %.2f)\n",
           junk < VideoNaming::kAcceptThreshold ? "PASS" : "FAIL", junk,
           VideoNaming::kAcceptThreshold);
    if (exact < 1.0 || junk >= VideoNaming::kAcceptThreshold)
        failed++;

    // Absolute anime mapping: 3 seasons of 10 → ep 25 = S3E5.
    QHash<int, int> counts{{1, 10}, {2, 10}, {3, 10}};
    int s = 0, e = 0;
    const bool mapped = VideoMatcher::mapAbsoluteEpisode(25, counts, &s, &e);
    const bool mapOk = mapped && s == 3 && e == 5;
    printf("  %s absolute episode 25 → S%dE%d\n", mapOk ? "PASS" : "FAIL", s, e);
    if (!mapOk)
        failed++;

    printf("names self-test: %s (%d failures)\n",
           failed == 0 ? "ALL PASS" : "FAILED", failed);
    return failed == 0 ? 0 : 1;
}

// Exercise the actual commit SQL with an old matcher snapshot; no HTTP.
struct VideoMatcherCustomizationTest {
    static void writeStaleResult(VideoMatcher &matcher, const LibVideo &old) {
        VideoMatcher::SeriesResolution meta;
        meta.tmdbId = 99; meta.title = QStringLiteral("Automatic title");
        meta.posterLocalPath = QStringLiteral("/fixture/automatic.jpg");
        matcher.writeMovieRow(old.id, meta);
        matcher.writeTvRow(old, meta, nullptr, {}, -1, -1);
    }
};

static int runCustomizeSelfTest() {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir scratch;
    LibraryDb db;
    if (!scratch.isValid() || !db.open(scratch.filePath(QStringLiteral("library.db"))))
        return 1;
    int failed = 0;
    auto check = [&failed](bool ok, const char *label) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failed;
    };
    LibVideo seed;
    seed.filepath = QStringLiteral("/fixture/series/episode.mkv");
    seed.filename = QStringLiteral("episode.mkv");
    seed.category = QStringLiteral("tv"); seed.series = QStringLiteral("Original");
    seed.season = 2; seed.episode = 7; seed.durationMs = 1800000;
    check(db.insertVideo(seed), "video fixture inserted");
    LibVideo video = db.allVideos().first();
    db.updateVideoPosition(video.id, 42000);
    db.setVideosWatched({video.id}, true);
    video.tmdbId = -1; video.tmdbTitle = QStringLiteral("My fan edit");
    video.tmdbPoster = QStringLiteral("/fixture/picked-cover.jpg");
    video.customPoster = true;
    video.tmdbCached = true; video.userEdited = true;
    check(db.begin() && db.updateVideoCustomization(video) && db.commit(),
          "manual identity and chosen art commit together");
    video = db.allVideos().first();
    check(video.tmdbId == -1 && video.userEdited && video.tmdbCached
          && video.tmdbPoster == QStringLiteral("/fixture/picked-cover.jpg"),
          "manual identity and artwork remain pinned");
    check(video.season == 2 && video.episode == 7 && video.watched
          && video.lastPositionMs == 42000,
          "customizing preserves episode numbering and playback state");
    VideoMatcher matcher(scratch.filePath(QStringLiteral("library.db")));
    VideoMatcherCustomizationTest::writeStaleResult(matcher, video);
    check(db.allVideos().first().tmdbTitle == QStringLiteral("My fan edit")
          && db.allVideos().first().tmdbPoster == QStringLiteral("/fixture/picked-cover.jpg"),
          "already-running matcher cannot overwrite a manual identity or poster");

    db.begin();
    video.tmdbTitle = QStringLiteral("Must roll back");
    check(db.updateVideoCustomization(video), "first row staged in transaction");
    LibVideo missing = video; missing.id += 1000;
    check(!db.updateVideoCustomization(missing), "missing target rejects the batch");
    check(db.rollback() && db.allVideos().first().tmdbTitle == QStringLiteral("My fan edit"),
          "failed batch restores earlier row edits");

    video = db.allVideos().first();
    video.tmdbId = 0; video.tmdbCached = false; video.userEdited = false;
    video.tmdbTitle.clear(); video.tmdbPoster.clear();
    video.customPoster = false;
    check(db.begin() && db.updateVideoCustomization(video) && db.commit(),
          "reset commits automatic state");
    video = db.allVideos().first();
    check(video.tmdbId == 0 && !video.tmdbCached && !video.userEdited
          && video.tmdbPoster.isEmpty(), "reset releases the matcher user-edit guard");
    // Reset identity, then choose artwork: identity can match while the
    // separately pinned image survives both movie and TV matcher paths.
    video.tmdbPoster = QStringLiteral("/fixture/independent.jpg"); video.customPoster = true;
    check(db.begin() && db.updateVideoCustomization(video) && db.commit(),
          "reset identity plus chosen art commits independently");
    VideoMatcherCustomizationTest::writeStaleResult(matcher, video);
    video = db.allVideos().first();
    check(video.tmdbId == 99 && video.tmdbTitle == QStringLiteral("Automatic title")
          && video.tmdbPoster == QStringLiteral("/fixture/independent.jpg") && video.customPoster,
          "automatic identity lookup preserves independent custom artwork");
    const auto parsed = VideoNaming::parseIdentity(
        QStringLiteral("/tv/Original Show/Season 2/Original.Show.S02E07.mkv"), QStringLiteral("tv"));
    check(parsed.series == QStringLiteral("Original Show") && parsed.season == 2 && parsed.episode == 7,
          "reset uses scanner folder truth and filename episode numbers");

    LibTrack track;
    track.filepath = QStringLiteral("/fixture/song.flac");
    track.title = QStringLiteral("Song"); track.artist = QStringLiteral("Performer");
    track.albumartist = QStringLiteral("Owner"); track.album = QStringLiteral("Old sleeve");
    check(db.upsertTrack(track), "music fixture inserted");
    LibTrack edited = db.allTracks().first();
    edited.albumartist = QStringLiteral("New owner"); edited.album = QStringLiteral("New sleeve");
    check(db.begin() && db.updateTrackMetadata(edited.id, edited, true) && db.commit(),
          "album identity commits with canonical owner");
    track.mtime = 12345;
    check(db.upsertTrack(track), "scanner revisits the original file metadata");
    edited = db.allTracks().first();
    check(edited.albumartist == QStringLiteral("New owner")
          && edited.album == QStringLiteral("New sleeve") && edited.userEdited
          && edited.mtime == 12345, "rescan refreshes file stats and preserves chosen identity");
    // Reconstruct the actual pre-Customize schema, including pre-slot
    // playlist tables. Merely lowering user_version on a current DB
    // leaves future tables behind and does not represent an old library.
    const QByteArray legacySchema =
        // Rewind every later schema era, not just the Customize ones: a
        // current-schema database still carries the v6 columns, the v7 photo-
        // album tables, the v9 fingerprint, the v10 import default and the v11
        // per-kind marks table, which would make the real migration chain fail
        // on columns it never dropped.  Each era is dropped in full -- dropping
        // three of the four v6 columns and leaving discnumber behind reads as
        // "duplicate column name: discnumber", and the chain looks broken in
        // LibraryDb rather than in this list.  import_defaults goes too: leaving
        // it behind would let a "v2" database answer the v11 probe, and this
        // fixture's whole claim is that it holds nothing newer than its label.
        "DROP TABLE import_defaults;"
        "ALTER TABLE watch_folders DROP COLUMN import_default;"
        "ALTER TABLE tracks DROP COLUMN discnumber;"
        "ALTER TABLE tracks DROP COLUMN probe_mtime_ns;"
        "ALTER TABLE tracks DROP COLUMN probe_ctime_ns;"
        "ALTER TABLE tracks DROP COLUMN probe_version;"
        "ALTER TABLE tracks DROP COLUMN audio_fingerprint;"
        "DROP TABLE music_probe_failures;"
        "DROP TABLE photo_album_photos; DROP TABLE photo_albums;"
        "ALTER TABLE videos DROP COLUMN custom_poster;"
        "DROP TABLE collection_customizations;"
        "DROP TABLE playlist_tracks; DROP TABLE playlists;"
        "CREATE TABLE playlists(id INTEGER PRIMARY KEY,name TEXT NOT NULL);"
        "CREATE TABLE playlist_tracks(id INTEGER PRIMARY KEY,playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,"
        "track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,position INTEGER NOT NULL);"
        "CREATE INDEX idx_plt ON playlist_tracks(playlist_id,position);";
    const QString legacyPath = scratch.filePath(QStringLiteral("legacy.db"));
    LibraryDb legacy;
    char *legacyError = nullptr;
    check(legacy.open(legacyPath) && legacy.insertVideo(seed), "migration fixture created");
    auto legacyVideo = legacy.allVideos().first();
    legacyVideo.tmdbPoster = QStringLiteral("/cache/vidthumbs/custom_1.jpg");
    legacy.updateVideoCustomization(legacyVideo);
    legacy.close();
    sqlite3 *raw = nullptr;
    bool prepared = sqlite3_open(legacyPath.toUtf8().constData(), &raw) == SQLITE_OK;
    if (prepared)
        prepared = sqlite3_exec(raw, (legacySchema + "PRAGMA user_version=2;").constData(),
                                  nullptr, nullptr, &legacyError) == SQLITE_OK;
    if (!prepared)
        printf("      legacy setup failed: %s\n",
               legacyError ? legacyError : (raw ? sqlite3_errmsg(raw) : "fixture open failed"));
    sqlite3_free(legacyError);
    legacyError = nullptr;
    if (raw) sqlite3_close(raw);
    if (prepared && !legacy.open(legacyPath))
        printf("      open failed: %s\n", qUtf8Printable(legacy.lastError()));
    check(prepared && legacy.open(legacyPath), "v2 library upgrades to separate artwork pins");
    check(!legacy.allVideos().isEmpty() && legacy.allVideos().first().customPoster
          && QFile::exists(legacyPath + QStringLiteral(".v2-backup")),
          "migration retains old custom covers and creates the upgrade backup");
    legacy.close();
    raw = nullptr;
    prepared = sqlite3_open(legacyPath.toUtf8().constData(), &raw) == SQLITE_OK;
    if (prepared)
        prepared = sqlite3_exec(raw, (legacySchema + "PRAGMA user_version=0;").constData(),
                                  nullptr, nullptr, &legacyError) == SQLITE_OK;
    if (!prepared)
        printf("      v0 reset failed: %s\n",
               legacyError ? legacyError : (raw ? sqlite3_errmsg(raw) : "fixture open failed"));
    sqlite3_free(legacyError);
    if (raw) sqlite3_close(raw);
    check(prepared && legacy.open(legacyPath) && !legacy.allVideos().isEmpty()
          && legacy.allVideos().first().customPoster,
          "historical unversioned libraries receive the artwork-pin migration too");
    printf("customize self-test: %s (%d failures)\n", failed ? "FAILED" : "ALL PASS", failed);
    return failed ? 1 : 0;
}

/* LibraryPaths and PlaylistText decide what the application is allowed to name a
 * file and what it thinks two files have in common.  Both are header-only, both
 * run at every save, and a wrong answer does not crash anything - it just makes a
 * transfer report "Fail to save", or makes an import report a library full of
 * missing songs.  Those are the failures a hand test on one platform misses, so
 * the cases are frozen here, on both sides of every #ifdef. */
static int runPathsSelfTest() {
    int failed = 0;
    const auto check = [&failed](bool ok, const char *what) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            failed++;
    };
    const auto eq = [&check](const char *what, const QString &got,
                             const QString &expect) {
        const bool ok = got == expect;
        check(ok, what);
        if (!ok)
            printf("      got '%s' expected '%s'\n", got.toUtf8().constData(),
                   expect.toUtf8().constData());
    };

    /* ---- what one filesystem component may be called ---- */
    /* The answer is the question, so the expectation is branched: a colon is an
     * ordinary character on POSIX and one of the eight Windows refuses.  Written
     * unbranched with the Windows answer, this case passed where it was written
     * and failed on every POSIX build -- macOS is the first place it ran. */
    eq("safeComponent: a colon in an album title is a character here, an illegal one on Windows",
       LibraryPaths::safeComponent(
           QStringLiteral("The Dark Side of the Moon: Immersion"),
           QStringLiteral("fallback")),
#ifdef Q_OS_WIN
       QStringLiteral("The Dark Side of the Moon_ Immersion"));
#else
       QStringLiteral("The Dark Side of the Moon: Immersion"));
#endif
    eq("safeComponent: a slash is a separator on every platform",
       LibraryPaths::safeComponent(QStringLiteral("Drake/Mixtape"),
                                   QStringLiteral("fallback")),
       QStringLiteral("Drake_Mixtape"));
    eq("safeComponent: empty, . and .. all take the caller's fallback",
       LibraryPaths::safeComponent(QString(), QStringLiteral("fallback"))
           + QLatin1Char('|')
           + LibraryPaths::safeComponent(QStringLiteral("."), QStringLiteral("fallback"))
           + QLatin1Char('|')
           + LibraryPaths::safeComponent(QStringLiteral(".."), QStringLiteral("fallback")),
       QStringLiteral("fallback|fallback|fallback"));
#ifdef Q_OS_WIN
    eq("safeComponent: the whole shell-illegal set becomes underscore",
       LibraryPaths::safeComponent(QStringLiteral("a<b>c|d?e*f\"g\\h/i:j"),
                                   QStringLiteral("fallback")),
       QStringLiteral("a_b_c_d_e_f_g_h_i_j"));
    eq("safeComponent: a control character cannot survive",
       LibraryPaths::safeComponent(QStringLiteral("bad") + QChar(1)
                                       + QStringLiteral("name"),
                                   QStringLiteral("fallback")),
       QStringLiteral("bad_name"));
    eq("safeComponent: a trailing dot or space goes, Explorer hides them",
       LibraryPaths::safeComponent(QStringLiteral("Trailing.  "),
                                   QStringLiteral("fallback")),
       QStringLiteral("Trailing"));
    eq("safeComponent: NUL and nul.mp3 are both unopenable by path",
       LibraryPaths::safeComponent(QStringLiteral("NUL"), QStringLiteral("fallback"))
           + QLatin1Char('|')
           + LibraryPaths::safeComponent(QStringLiteral("nul.mp3"), QStringLiteral("fallback")),
       QStringLiteral("_NUL|_nul.mp3"));
    eq("safeComponent: CON, aux and COM1.txt take the prefix",
       LibraryPaths::safeComponent(QStringLiteral("CON"), QStringLiteral("fallback"))
           + QLatin1Char('|')
           + LibraryPaths::safeComponent(QStringLiteral("aux"), QStringLiteral("fallback"))
           + QLatin1Char('|')
           + LibraryPaths::safeComponent(QStringLiteral("COM1.txt"), QStringLiteral("fallback")),
       QStringLiteral("_CON|_aux|_COM1.txt"));
    eq("safeComponent: a stem that only starts like a device keeps its name",
       LibraryPaths::safeComponent(QStringLiteral("NULL album"), QStringLiteral("fallback"))
           + QLatin1Char('|')
           + LibraryPaths::safeComponent(QStringLiteral("CONCERT"), QStringLiteral("fallback"))
           + QLatin1Char('|')
           + LibraryPaths::safeComponent(QStringLiteral("CLOCK$"), QStringLiteral("fallback")),
       QStringLiteral("NULL album|CONCERT|_CLOCK$"));
    check(LibraryPaths::isReservedWindowsName(QStringLiteral("LPT9.log"))
              && LibraryPaths::isReservedWindowsName(QStringLiteral("PRN")),
          "isReservedWindowsName: LPTn and PRN are devices");
    check(!LibraryPaths::isReservedWindowsName(QStringLiteral("music"))
              && !LibraryPaths::isReservedWindowsName(QStringLiteral("Noodle")),
          "isReservedWindowsName: ordinary names are not devices");
#else
    eq("safeComponent: POSIX touches nothing but the slash",
       LibraryPaths::safeComponent(QStringLiteral("a<b>c|d?e*f\"g\\h/i:j"),
                                   QStringLiteral("fallback")),
       QStringLiteral("a<b>c|d?e*f\"g\\h_i:j"));
    eq("safeComponent: POSIX keeps a trailing dot and space - they are legal",
       LibraryPaths::safeComponent(QStringLiteral("Trailing.  "),
                                   QStringLiteral("fallback")),
       QStringLiteral("Trailing.  "));
    eq("safeComponent: POSIX has no reserved stems, so NUL stays NUL",
       LibraryPaths::safeComponent(QStringLiteral("nul.mp3"), QStringLiteral("fallback")),
       QStringLiteral("nul.mp3"));
    check(!LibraryPaths::isReservedWindowsName(QStringLiteral("NUL")),
          "isReservedWindowsName: no device namespace on POSIX, so no name gains an underscore");
#endif

    /* ---- what a playlist of the user's own naming is allowed to be called ----
     * The export used to carry its own shorter rule, so a playlist whose name ends
     * in a dot or a space wrote a file Explorer renames behind the app's back, and
     * one called NUL wrote a file that cannot be reopened by path. */
#ifdef Q_OS_WIN
    eq("playlistFileName: a playlist called NUL exports under a name that opens",
       LibraryPaths::playlistFileName(QStringLiteral("NUL"), QStringLiteral("playlist")),
       QStringLiteral("_NUL"));
    eq("playlistFileName: a trailing dot or space would fold two playlists onto one file",
       LibraryPaths::playlistFileName(QStringLiteral("Mixtape. "), QStringLiteral("playlist")),
       QStringLiteral("Mixtape"));
#else
    eq("playlistFileName: POSIX keeps the names every earlier export already wrote",
       LibraryPaths::playlistFileName(QStringLiteral("NUL"), QStringLiteral("playlist"))
           + QLatin1Char('|')
           + LibraryPaths::playlistFileName(QStringLiteral("Mixtape. "), QStringLiteral("playlist")),
       QStringLiteral("NUL|Mixtape. "));
#endif
    eq("playlistFileName: an empty name still writes a file somebody can find",
       LibraryPaths::playlistFileName(QString(), QStringLiteral("playlist")),
       QStringLiteral("playlist"));

    /* ---- two spellings of one file ---- */
#ifdef Q_OS_WIN
    check(LibraryPaths::caseKey(QStringLiteral("E:/Music/A.MP3"))
              == LibraryPaths::caseKey(QStringLiteral("e:/music/a.mp3")),
          "caseKey: a Windows folder cannot hold both spellings, so they are one key");
    check(LibraryPaths::caseKey(QStringLiteral("E:/Music/A.MP3"))
              != LibraryPaths::caseKey(QStringLiteral("E:/Music/B.MP3")),
          "caseKey: folding still separates different files");
#else
    eq("caseKey: POSIX folds nothing, so every stored key stays byte-identical",
       LibraryPaths::caseKey(QStringLiteral("/Music/A.MP3")),
       QStringLiteral("/Music/A.MP3"));
    check(LibraryPaths::caseKey(QStringLiteral("/Music/A.MP3"))
              != LibraryPaths::caseKey(QStringLiteral("/Music/a.mp3")),
          "caseKey: on POSIX case is part of the filename");
#endif

    /* ---- one playlist line, resolved ---- */
    const QString base = QStringLiteral("/music/playlists");
    eq("resolveEntry: an absolute entry is left alone",
       LibraryPaths::resolveEntry(base, QStringLiteral("/music/A.mp3")),
       QStringLiteral("/music/A.mp3"));
    eq("resolveEntry: a bare name belongs to the playlist's own folder",
       LibraryPaths::resolveEntry(base, QStringLiteral("A.mp3")),
       QStringLiteral("/music/playlists/A.mp3"));
    eq("resolveEntry: a relative entry is cleaned, not left with a traversal in it",
       LibraryPaths::resolveEntry(base, QStringLiteral("sub/../A.flac")),
       QStringLiteral("/music/playlists/A.flac"));
    eq("resolveEntry: a parent escape resolves and stays resolved",
       LibraryPaths::resolveEntry(base, QStringLiteral("../A.mp3")),
       QStringLiteral("/music/A.mp3"));
#ifdef Q_OS_WIN
    eq("resolveEntry: a Windows playlist spells it with backslashes and a drive",
       LibraryPaths::resolveEntry(base, QStringLiteral("E:\\Music\\A.mp3")),
       QStringLiteral("E:/Music/A.mp3"));
    eq("resolveEntry: an iTunes file:// URL with a drive decodes",
       LibraryPaths::resolveEntry(base, QStringLiteral("file:///E:/Music/A.mp3")),
       QStringLiteral("E:/Music/A.mp3"));
#else
    eq("resolveEntry: a backslash on POSIX is an ordinary filename character",
       LibraryPaths::resolveEntry(base, QStringLiteral("E:\\Music\\A.mp3")),
       QStringLiteral("/music/playlists/E:\\Music\\A.mp3"));
#endif

    /* ---- playlist bytes -> text ---- */
    eq("decode: plain ASCII is the text it already was",
       PlaylistText::decode(QByteArrayLiteral("01 - Track.mp3\n")),
       QStringLiteral("01 - Track.mp3\n"));
    check(PlaylistText::decode(QByteArray()).isEmpty(),
          "decode: an empty file is empty text, not a missing entry");
    /* No non-ASCII literal here: MSVC reads a source file in the active code page
     * unless /utf-8 is passed, so the bytes are spelled out instead.  E5 91 A8
     * E6 B8 B8 is two CJK characters, ten bytes of valid UTF-8. */
    const QByteArray utf8Name("\xE5\x91\xA8\xE6\xB8\xB8.mp3");
    eq("decode: valid UTF-8 multibyte decodes as UTF-8",
       PlaylistText::decode(utf8Name), QString::fromUtf8(utf8Name));
    const QString afterBom
        = PlaylistText::decode(QByteArray("\xEF\xBB\xBF") + utf8Name);
    check(!afterBom.startsWith(QChar(0xFEFF)) && afterBom == QString::fromUtf8(utf8Name),
          "decode: a UTF-8 BOM is stripped instead of corrupting the first entry");
    /* C4 E3 C3 C7 is two GBK characters and is not valid UTF-8 - 0xC4 demands a
     * continuation byte and gets 0xE3.  Notepad's "ANSI" save, Windows Media
     * Player and most converters emit exactly this shape. */
    const QByteArray ansiName("\xC4\xE3\xC3\xC7.mp3");
    eq("decode: a file that is not UTF-8 falls back to the locale code page",
       PlaylistText::decode(ansiName), QString::fromLocal8Bit(ansiName));
#ifdef Q_OS_WIN
    check(!PlaylistText::decode(ansiName).contains(QChar(0xFFFD)),
          "decode: an ANSI playlist does not arrive as replacement characters");
#endif
    check(PlaylistText::decode(QByteArrayLiteral("A.mp3\r\nB.mp3\r\n"))
                  .split(QLatin1Char('\n'))
                  .at(0)
                  .trimmed()
              == QLatin1String("A.mp3"),
          "decode: a CRLF playlist still yields the path and nothing else");
    /* Notepad's retired "Unicode" save and PowerShell's -Encoding unicode write
     * UTF-16 with a BOM.  Decoded as anything else every entry arrives with a NUL
     * between its characters, matches no library row, and the import answers with
     * a count of misses about a playlist that named nothing at all. */
    {
        const QString name = QString::fromUtf8("\xE5\x91\xA8\xE6\xB8\xB8")
                             + QStringLiteral(" - Track.mp3\n");
        const auto toUtf16 = [](const QString &text, bool bigEndian) {
            QByteArray out;
            for (qsizetype i = 0; i < text.size(); ++i) {
                const ushort unit = text.at(int(i)).unicode();
                const char low = char(unit & 0xFF);
                const char high = char((uint(unit) >> 8) & 0xFF);
                out.append(bigEndian ? high : low);
                out.append(bigEndian ? low : high);
            }
            return out;
        };
        const QByteArray little = QByteArray("\xFF\xFE") + toUtf16(name, false);
        eq("decode: a UTF-16LE playlist is the playlist, not a NUL between every character",
           PlaylistText::decode(little), name);
        const QByteArray big = QByteArray("\xFE\xFF") + toUtf16(name, true);
        eq("decode: a big-endian BOM is honoured rather than byte-swapped away",
           PlaylistText::decode(big), name);
        const QString back = PlaylistText::decode(little);
        check(back.at(0) != QChar(0xFEFF) && !back.contains(QChar(0)),
              "decode: neither the BOM nor a NUL survives into the first entry");
    }

    /* ---- the video import name shares the same device-name rule ---- */
    eq("safeFilePart: an ordinary device object name is untouched",
       VideoIdentity::safeFilePart(QStringLiteral("Show A - S01E01 - Pilot.wmv")),
       QStringLiteral("Show A - S01E01 - Pilot.wmv"));
#ifdef Q_OS_WIN
    eq("safeFilePart: a device object called NUL.wmv imports to a file that opens",
       VideoIdentity::safeFilePart(QStringLiteral("NUL.wmv")),
       QStringLiteral("_NUL.wmv"));
#else
    eq("safeFilePart: POSIX imports NUL.wmv under that exact name",
       VideoIdentity::safeFilePart(QStringLiteral("NUL.wmv")),
       QStringLiteral("NUL.wmv"));
#endif

    /* ---- a device name that is really a moniker ----
     * A Zune that never learned an artist does not leave the field empty, it
     * stores a GUID and displays that: one album on the player used for the
     * Windows port reports its artist as
     * {09316835-3188-481c-a359-0f8d2a937ac8} because its files carry no Author
     * attribute, while the album beside it, ripped with an AlbumArtist, reads
     * "EGOIST".  Read as a name that string becomes a folder, a library artist,
     * an artwork cache key and the artist a send writes back.  The rule is about
     * the player, not the platform, so the same cases gate on both sides. */
    check(MusicIdentity::isGuidMoniker(
              QStringLiteral("{09316835-3188-481c-a359-0f8d2a937ac8}")),
          "isGuidMoniker: the artist a real Zune HD reports is a moniker");
    check(MusicIdentity::isGuidMoniker(
              QStringLiteral("09316835-3188-481c-a359-0f8d2a937ac8")),
          "isGuidMoniker: the same value without its braces is still a moniker");
    check(MusicIdentity::isGuidMoniker(
              QStringLiteral("  {09316835-3188-481C-A359-0F8D2A937AC8} ")),
          "isGuidMoniker: case and surrounding spaces change nothing");
    check(!MusicIdentity::isGuidMoniker(QStringLiteral("Daft Punk"))
              && !MusicIdentity::isGuidMoniker(
                  QStringLiteral("ARIA The NATURAL VOCAL SONG COLLECTION"))
              && !MusicIdentity::isGuidMoniker(QString()),
          "isGuidMoniker: a real name, an album title and an absent name are not monikers");
    check(!MusicIdentity::isGuidMoniker(
              QStringLiteral("09316835-3188-481c-a359-0f8d2a937ac8 remix")),
          "isGuidMoniker: text that only contains a moniker is still text");
    check(!MusicIdentity::isGuidMoniker(
              QStringLiteral("09316835-3188-481c-a359-0f8d2a937ac")),
          "isGuidMoniker: one character short of a GUID is a name nobody would choose");
    eq("deviceArtist: a moniker becomes the unknown the library already uses",
       MusicIdentity::deviceArtist(
           QStringLiteral("{09316835-3188-481c-a359-0f8d2a937ac8}")),
       QStringLiteral("Unknown Artist"));
    eq("deviceAlbum: a moniker album follows the same convention",
       MusicIdentity::deviceAlbum(
           QStringLiteral("{09316835-3188-481c-a359-0f8d2a937ac8}")),
       QStringLiteral("Unknown Album"));
    eq("deviceArtist: a tagged album keeps the artist it really has",
       MusicIdentity::deviceArtist(QStringLiteral("EGOIST")),
       QStringLiteral("EGOIST"));
    /* The whole point: the device row and the tagless file it was pulled from
     * must answer the same artist, or the album cover fetched off the player is
     * cached under a key the local album can never ask for. */
    check(MusicIdentity::knownArtist(MusicIdentity::deviceArtist(
              QStringLiteral("{09316835-3188-481c-a359-0f8d2a937ac8}")))
              .isEmpty()
          && MusicIdentity::knownArtist(QStringLiteral("Unknown Artist")).isEmpty(),
          "identity: a moniker artist and a tagless local file are one artist");
    /* usableName() is the same predicate from the other side: not "what should
     * the UI show" but "may this text be a name at all", which is what a scan
     * asking for a candidate has to ask. */
    eq("usableName: a moniker is no name at all",
       MusicIdentity::usableName(
           QStringLiteral("{09316835-3188-481c-a359-0f8d2a937ac8}")),
       QString());
    eq("usableName: a real name passes through untouched",
       MusicIdentity::usableName(QStringLiteral("EGOIST")),
       QStringLiteral("EGOIST"));
    /* Measured on the port's Zune HD, not guessed: ZMDB hands over the moniker
     * with leading blanks, so a device name is only usable once trimmed. */
    eq("deviceArtist: the blanks a Zune stores around a name are not part of it",
       MusicIdentity::deviceArtist(QStringLiteral("   EGOIST  ")),
       QStringLiteral("EGOIST"));
    eq("deviceAlbum: a padded moniker is still recognised as one",
       MusicIdentity::deviceAlbum(
           QStringLiteral("                {09316835-3188-481c-a359-0f8d2a937ac8}")),
       QStringLiteral("Unknown Album"));
    /* The defect this exists for.  <imports>/<GUID>/<album>/file is a path this
     * app wrote while device fields were still read raw, and the folder inference
     * read that folder back as the artist of a file with no tag - so the answer
     * a Zune-authored path offers for "who is the artist" has to be no answer. */
    check(MusicIdentity::usableName(
              QStringLiteral("{09316835-3188-481c-a359-0f8d2a937ac8}")).isEmpty()
              && !MusicIdentity::usableName(
                  QStringLiteral("ARIA The NATURAL VOCAL SONG COLLECTION")).isEmpty(),
          "usableName: a Zune-authored import folder offers no artist candidate");

    /* ---- the same moniker in the genre field ----
     * Asked on 2026-10-22 why one album on the player was named by a GUID, the
     * device answered with numbers: of 189 track rows 0 carried a moniker album
     * and all 15 album strings were real, while the 27 rows whose artist is the
     * moniker carry that same value in genre, and genre was the one name field
     * copied over without a rule.  So a GUID was rendered as a tile name on the
     * Genres pivot.  Genre has no "Unknown Genre" convention to yield - both
     * pivots skip an empty genre, which is what an untagged file already answers -
     * so a moniker genre has to become nothing, the same thing usableName() makes
     * it on the local side. */
    eq("deviceGenre: a moniker genre is no genre at all",
       MusicIdentity::deviceGenre(
           QStringLiteral("                {09316835-3188-481c-a359-0f8d2a937ac8}")),
       QString());
    eq("deviceGenre: an unbraced moniker is no genre either",
       MusicIdentity::deviceGenre(QStringLiteral("09316835-3188-481c-a359-0f8d2a937ac8")),
       QString());
    eq("deviceGenre: a real genre keeps its own spelling and script",
       MusicIdentity::deviceGenre(QStringLiteral("\u30A2\u30CB\u30E1")),
       QStringLiteral("\u30A2\u30CB\u30E1"));
    eq("deviceGenre: the blanks a Zune stores around a genre are not part of it",
       MusicIdentity::deviceGenre(QStringLiteral("   JPop  ")),
       QStringLiteral("JPop"));
    /* The player really does have a genre it spells "Unknown Genre", seen in that
     * same pool beside the moniker.  That is the device's own text and not a
     * placeholder this app invented, so rewriting it would be editing the player. */
    eq("deviceGenre: the device's own Unknown Genre is content, not a moniker",
       MusicIdentity::deviceGenre(QStringLiteral("Unknown Genre")),
       QStringLiteral("Unknown Genre"));
    check(MusicIdentity::deviceGenre(
              QStringLiteral("{09316835-3188-481c-a359-0f8d2a937ac8}")).isEmpty()
              && MusicIdentity::usableName(
                  QStringLiteral("{09316835-3188-481c-a359-0f8d2a937ac8}")).isEmpty(),
          "identity: a moniker genre reads the same off the player as off a tag");

    /* ---- one path past MAX_PATH ----
     * 260 characters is not a limit Windows still enforces, it is a limit a
     * process opts out of: the file APIs stop refusing a long path for an image
     * whose manifest declares longPathAware, on a machine whose registry enables
     * long paths.  Without that declaration the library keeps working until
     * somebody's album folder happens to sit deep enough, and then the same song
     * is sitting there in Explorer and missing from the app.  Every component of
     * this fixture is short enough to please the filesystem, so what is under test
     * is the total length and nothing else. */
    {
        QTemporaryDir sandbox;
        check(sandbox.isValid(), "deep: a temporary tree could be made");
        const QString level =
            QStringLiteral("DeepAlbumFolderLevelAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
        QString deep = sandbox.path();
        for (int i = 0; i < 6; ++i)
            deep += QLatin1Char('/') + level;
        const QString leaf = deep + QStringLiteral("/Distant Song.mp3");
        check(leaf.size() > 260, "deep: the fixture really is past 260 characters");
        const bool built = QDir().mkpath(deep);
        bool written = false;
        if (built) {
            QFile song(leaf);
            written = song.open(QIODevice::WriteOnly) && song.write("x") == 1;
            song.close();
        }
        check(written, "deep: a song past 260 characters can be created and written");
        bool read = false;
        if (written) {
            QFile song(leaf);
            read = song.open(QIODevice::ReadOnly) && song.readAll() == QByteArrayLiteral("x");
        }
        check(read && QFileInfo(leaf).size() == 1,
              "deep: the same song reads back through the ordinary file API");
#ifdef Q_OS_WIN
        /* The rest of this file talks to the disk through Qt, and Qt 6 gets a long
         * path through on its own.  What does not go through Qt is the transcode
         * layer: FFmpeg, LAME and SQLite all reach a file with the C runtime, and
         * the CRT hands the path straight to kernel32, where the 260-character
         * ceiling is lifted per executable by the manifest rather than by the
         * registry alone.  A temporary .wmv under a deep folder is exactly the file
         * that would teach this the hard way, mid-sync, on a machine that already
         * enabled long paths in the registry and expects it to mean something. */
        {
            const std::wstring wide = leaf.toStdWString();
            bool crtWritten = false;
            if (FILE *f = _wfopen(wide.c_str(), L"wb")) {
                crtWritten = std::fwrite("y", 1, 1, f) == 1;
                std::fclose(f);
            }
            check(crtWritten,
                  "deep crt: the C runtime writes a song past 260 characters");
            char probe = 0;
            bool crtRead = false;
            if (FILE *f = _wfopen(wide.c_str(), L"rb")) {
                crtRead = std::fread(&probe, 1, 1, f) == 1 && probe == 'y';
                std::fclose(f);
            }
            check(crtRead, "deep crt: the same song reads back through the C runtime");
        }
#endif
        int handedOver = 0;
        const auto walked = LibraryScan::walkFiles(
            deep, QDir::Files | QDir::NoDotAndDotDot,
            [&handedOver](const QFileInfo &) { handedOver++; });
        check(handedOver == 1 && walked.files == 1 && walked.complete,
              "deep: the watch-folder walk reaches it and certifies the root");
        QDir(sandbox.path()).removeRecursively();
    }

    /* ---- one watch folder, walked exactly once ----
     * A Windows junction reports itself as an ordinary directory - a plain
     * FILE_ATTRIBUTE_DIRECTORY with isSymLink() false - so before LibraryScan
     * carried an identity check of its own, a music folder holding a link back
     * into itself was walked 41 levels deep before Qt stopped at the
     * 260-character path limit, filing a dozen phantom copies of every song on
     * the way.  The link is built here rather than assumed, and the sibling link
     * matters just as much: it proves the new guard stops the loop, not the
     * scan. */
    {
        QTemporaryDir sandbox;
        check(sandbox.isValid(), "walk: a temporary tree could be made");
        const QString box = sandbox.path();
        const QString music = box + QLatin1String("/music");
        const QString elsewhere = box + QLatin1String("/elsewhere");
        bool built = QDir().mkpath(music + QLatin1String("/Artist/Album"))
                     && QDir().mkpath(music + QLatin1String("/Extra"))
                     && QDir().mkpath(elsewhere);
        const QStringList songs = {
            music + QLatin1String("/Artist/Album/One.mp3"),
            music + QLatin1String("/Artist/Album/Two.mp3"),
            music + QLatin1String("/Extra/Three.mp3"),
            elsewhere + QLatin1String("/Four.mp3"),
        };
        for (const QString &song : songs) {
            QFile file(song);
            built = built && file.open(QIODevice::WriteOnly) && file.write("x") == 1;
            file.close();
        }
        check(built, "walk: the fixture tree and its four songs exist");

        const QString loop = music + QLatin1String("/loop");
        const QString escape = music + QLatin1String("/out");
        // What a walk that follows the link out has to report: the link's own
        // path, not the target's.  Equal checks, opposite meaning.
        const QString throughLink = escape + QLatin1String("/Four.mp3");
#ifdef Q_OS_WIN
        /* mklink is a cmd.exe built-in and /J makes a junction: unlike /D it
         * needs no elevation, which is the only reason this is testable from an
         * ordinary account.  QProcess quotes the two paths itself, which matters
         * because the temporary directory here sits under a profile whose name
         * contains a space. */
        const auto addLink = [](const QString &link, const QString &target) {
            const QStringList arguments{
                QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"),
                QDir::toNativeSeparators(link), QDir::toNativeSeparators(target)};
            return QProcess::execute(QStringLiteral("cmd.exe"), arguments) == 0;
        };
#else
        const auto addLink = [](const QString &link, const QString &target) {
            return ::symlink(target.toUtf8().constData(), link.toUtf8().constData()) == 0;
        };
#endif
        const bool linked = addLink(loop, music) && addLink(escape, elsewhere);
        check(linked, "walk: the link back into the root and the link out were created");

        QHash<QString, int> visits;
        int handedOver = 0;
        const auto walked = LibraryScan::walkFiles(
            music, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
            [&](const QFileInfo &entry) {
                visits[entry.filePath()]++;
                handedOver++;
            });
        const auto exactlyOnce = [&visits](const QString &song) {
            return visits.value(song) == 1;
        };
        check(exactlyOnce(songs.at(0)) && exactlyOnce(songs.at(1))
                  && exactlyOnce(songs.at(2)),
              "walk: every song really inside the root is offered exactly once");
        check(walked.complete && !walked.cancelled,
              "walk: a self-referencing watch folder still reports a complete walk");
#ifdef Q_OS_WIN
        check(visits.value(throughLink) == 1 && !visits.contains(songs.at(3)),
              "walk: a junction onto new files is followed once, under the link path");
        check(walked.duplicates >= 1,
              "walk: the junction back into the root is counted, never descended");
        check(visits.size() == 4 && handedOver == 4,
              "walk: four songs in, four songs out - not 41 phantom copies");
#else
        check(!visits.contains(songs.at(3)),
              "walk: a POSIX symlink to a directory is still not descended");
        check(walked.duplicates == 0 && visits.size() == 3 && handedOver == 3,
              "walk: three songs in, three songs out where there are no junctions");
#endif

        /* The links come out before anything recursive, deliberately: with a
         * junction in place a recursive delete also sees the original tree
         * through the link and takes the real files with it. */
#ifdef Q_OS_WIN
        const bool linksOut = QDir().rmdir(loop) && QDir().rmdir(escape);
#else
        const bool linksOut = QFile::remove(loop) && QFile::remove(escape);
#endif
        check(linksOut, "walk: the fixture links came back out");
        QDir(box).removeRecursively();
        check(!QFileInfo::exists(box), "walk: the fixture tree is gone again");
    }

    /* ---- a root that is not there certifies nothing ---- */
    {
        QTemporaryDir retired;
        const QString retiredPath = retired.path();
        retired.remove();
        int handedOver = 0;
        const auto missing = LibraryScan::walkFiles(
            retiredPath, QDir::Files | QDir::Readable,
            [&handedOver](const QFileInfo &) { handedOver++; });
        check(handedOver == 0 && missing.files == 0 && !missing.complete
                  && !missing.cancelled,
              "walk: an unreadable root yields no files and certifies no root");
    }

    printf("paths self-test: %s (%d failures)\n",
           failed == 0 ? "ALL PASS" : "FAILED", failed);
    return failed == 0 ? 0 : 1;
}

/* ---- what route this process would use to reach a provider ----
 *
 * "HTTP 0, Qt error 4" says only that no reply arrived: Qt 6.11 numbers that as
 * QNetworkReply::TimeoutError (QtNetwork/qnetworkreply.h, verified against the
 * installed header - NoError 0, ConnectionRefused 1, RemoteHostClosed 2,
 * HostNotFound 3, Timeout 4), and a status of 0 does not distinguish a network
 * that drops the connection from a proxy that refused it.  That is why the route
 * is in the log line now.
 *
 * The diagnosis this file first carried was wrong and is corrected here.  It said
 * the artwork requests died because the network reaches Deezer and Google only
 * through the proxy in Windows' own settings and nothing in the process had asked
 * about it.  Measured instead (NetworkAccess.h holds the four numbers): curl from
 * this machine fails to connect to api.deezer.com and www.google.com in 21 s and
 * reaches api.github.com in 0.39 s, and the same two failures answer 200 and 204
 * in under a second through http://192.168.2.100:8117 - so direct egress really
 * is partly blocked here.  But this shell answers ProxyEnable=0, and a fresh Qt
 * 6.11 process reports usesSystemConfiguration=1, so Qt already follows the shell
 * and correctly goes direct: the machine has its proxy switched off, which is a
 * fact about the network, not a missing call in the client.  ZUUNED_PROXY exists
 * for exactly that case.
 *
 * Neither Windows gate can reach a C++ header, so the rules live here, and they
 * are written so that none of them depends on what the operator's network happens
 * to be: the system-proxy assertions are about which answer Qt was asked to
 * consult, and the explicit-proxy assertions install a route that no traffic is
 * ever sent through.
 */
static int runNetworkSelfTest() {
    int failed = 0;
    const auto check = [&failed](bool ok, const char *what) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            failed++;
    };
    const auto eq = [&check](const char *what, const QString &got, const QString &expect) {
        const bool ok = got == expect;
        check(ok, what);
        if (!ok)
            printf("      got '%s' expected '%s'\n", got.toUtf8().constData(),
                   expect.toUtf8().constData());
    };

    /* ---- what one route is allowed to be called in a log ---- */
    eq("describe: an unset proxy is direct", NetworkAccess::describe(QNetworkProxy()),
       QStringLiteral("direct"));
    eq("describe: an explicit NoProxy is direct",
       NetworkAccess::describe(QNetworkProxy(QNetworkProxy::NoProxy)),
       QStringLiteral("direct"));
    eq("describe: an http proxy names its endpoint",
       NetworkAccess::describe(QNetworkProxy(QNetworkProxy::HttpProxy,
                                             QStringLiteral("10.0.0.1"), 8080)),
       QStringLiteral("http 10.0.0.1:8080"));
    eq("describe: a socks proxy does not claim to be http",
       NetworkAccess::describe(QNetworkProxy(QNetworkProxy::Socks5Proxy,
                                             QStringLiteral("127.0.0.1"), 1080)),
       QStringLiteral("socks5 127.0.0.1:1080"));

    /* ---- what an operator may write into ZUUNED_PROXY ---- */
    const auto refuses = [](const QString &spec) {
        bool accepted = false;
        NetworkAccess::parse(spec, &accepted);
        return !accepted;
    };
    bool ok = false;
    QNetworkProxy proxy = NetworkAccess::parse(QStringLiteral("127.0.0.1:8117"), &ok);
    check(ok && proxy.type() == QNetworkProxy::HttpProxy
              && proxy.hostName() == QStringLiteral("127.0.0.1") && proxy.port() == 8117,
          "parse: a bare host:port is an http proxy");
    proxy = NetworkAccess::parse(QStringLiteral("http://192.168.2.100:8117"), &ok);
    check(ok && proxy.hostName() == QStringLiteral("192.168.2.100") && proxy.port() == 8117,
          "parse: the scheme a browser uses is accepted");
    proxy = NetworkAccess::parse(QStringLiteral("socks5://127.0.0.1:1080"), &ok);
    check(ok && proxy.type() == QNetworkProxy::Socks5Proxy,
          "parse: socks5 asks for a socks proxy, not an http one");
    proxy = NetworkAccess::parse(QStringLiteral("HTTPS://[::1]:8117"), &ok);
    check(ok && proxy.hostName() == QStringLiteral("[::1]") && proxy.port() == 8117,
          "parse: a bracketed IPv6 host keeps its brackets and its port");
    proxy = NetworkAccess::parse(QStringLiteral("http://zuuned:s3cr3t@127.0.0.1:3128"), &ok);
    check(ok && proxy.user() == QStringLiteral("zuuned")
              && proxy.password() == QStringLiteral("s3cr3t"),
          "parse: credentials reach the proxy object that will authenticate");
    check(!NetworkAccess::describe(proxy).contains(QStringLiteral("s3cr3t"))
              && !NetworkAccess::describe(proxy).contains(QStringLiteral("zuuned")),
          "describe: a proxy credential never reaches the line that gets logged");
    check(refuses(QStringLiteral("not a proxy")),
          "parse: prose is not a route");
    check(refuses(QStringLiteral("http://proxy.internal")),
          "parse: a proxy with no port is refused, not guessed at");
    check(refuses(QStringLiteral("socks4://127.0.0.1:1080")),
          "parse: a dialect this application cannot speak is refused");
    check(refuses(QStringLiteral("http://:8117")),
          "parse: a port with no host behind it is refused");
    check(refuses(QStringLiteral("127.0.0.1:99999")),
          "parse: a port outside the wire's range is refused");
    check(refuses(QStringLiteral("http://a@:8117")),
          "parse: an empty account name is refused");
    check(refuses(QStringLiteral("::1")),
          "parse: an ambiguous host is refused rather than guessed to be IPv6");

    /* ---- the override reaches Qt, and the default is left alone ----
     *
     * The Windows assertions record a measurement, not a wish.  A fresh Qt 6.11
     * process on this platform was asked before it had done anything at all and
     * answered usesSystemConfiguration=1, so Qt consults the shell by itself and
     * nothing in this application installs that - pinning the fact is what keeps
     * a later edit from "fixing" a proxy problem that is really a switched-off
     * proxy on a network that drops everything else.  The override assertions
     * install a route and never send a byte through it.
     */
#ifdef Q_OS_WIN
    check(QNetworkProxyFactory::usesSystemConfiguration(),
          "policy: a fresh Windows process already answers from the shell, so no "
          "call is needed here to make provider requests follow it");
#endif
    const QString defaultRoute = NetworkAccess::effectiveHttps();
    qunsetenv("ZUUNED_PROXY");
    const NetworkAccess::Decision plain = NetworkAccess::apply();
    check(plain.policy == NetworkAccess::Policy::Unchanged,
          "policy: with nothing configured apply() decides nothing");
    eq("policy: and the route a request would use is exactly what it was",
       NetworkAccess::effectiveHttps(), defaultRoute);

    qputenv("ZUUNED_PROXY", "not a proxy");
    const NetworkAccess::Decision refused = NetworkAccess::apply();
    check(refused.policy == NetworkAccess::Policy::Rejected
              && refused.note.contains(QStringLiteral("not a proxy")),
          "policy: an unusable ZUUNED_PROXY is refused and quoted back");
    eq("policy: a refusal installs no route at all",
       NetworkAccess::effectiveHttps(), defaultRoute);
    // A refusal quotes the value back, so say out loud that a credential inside
    // an unusable value does not travel with it.  No port here, which is what
    // makes the value unusable in the first place.
    qputenv("ZUUNED_PROXY", "http://zuuned-secret-user:zuuned-secret-pass@proxy.invalid");
    const NetworkAccess::Decision secret = NetworkAccess::apply();
    check(secret.policy == NetworkAccess::Policy::Rejected
              && !secret.note.contains("zuuned-secret-user")
              && !secret.note.contains("zuuned-secret-pass")
              && secret.note.contains("proxy.invalid"),
          "policy: a refused value keeps its host and loses its credential");
    qunsetenv("ZUUNED_PROXY");
    NetworkAccess::apply();

    qputenv("ZUUNED_PROXY", "http://127.0.0.1:8117");
    const NetworkAccess::Decision configured = NetworkAccess::apply();
    check(configured.policy == NetworkAccess::Policy::Environment,
          "policy: ZUUNED_PROXY outranks the platform default");
    eq("policy: an HTTPS request would use the configured route",
       NetworkAccess::effectiveHttps(), QStringLiteral("http 127.0.0.1:8117"));
    check(NetworkAccess::summary(configured).contains(QStringLiteral("HTTPS goes")),
          "summary: the startup line names the route it settled on");
    qunsetenv("ZUUNED_PROXY");
    const NetworkAccess::Decision cleared = NetworkAccess::apply();
    check(cleared.policy == NetworkAccess::Policy::Unchanged,
          "policy: an override is read once at startup, not remembered by the app");
    printf("      note: after clearing ZUUNED_PROXY in the same process the route "
           "reads \"%s\"; zuuned calls apply() once per launch\n",
           NetworkAccess::effectiveHttps().toUtf8().constData());

    printf("network self-test: %s (%d failures)\n",
           failed == 0 ? "ALL PASS" : "FAILED", failed);
    return failed == 0 ? 0 : 1;
}

/*
 * Album cover lookup, pinned offline.
 *
 * Asking a music store for a cover is the one lookup a library runs by itself,
 * so it is the one most likely to be changed by accident, and neither Windows
 * gate reaches a C++ header.  Every rule is answered here from a canned reply
 * per catalogue: no socket is opened, and what the fetch counter saw is what
 * proves the ladder stops early rather than whatever a live store happens to
 * carry today.
 */
/* ---- a scan retires rows whose file is gone, and only where it may ----
 *
 * This is the one scan behaviour that can destroy a library by accident, so it
 * is gated on the shape of the absence rather than on a file count.  Three
 * kinds of missing file look identical to `stat()` and are not identical at
 * all: gone from a folder this pass read to the end, gone from a watch folder
 * that is not mounted (or not plugged in), and outside every watch folder so
 * that nothing can say whether it should be here.  Only the first is retired.
 * The audio file is deliberately not decodable - retirement is a filesystem
 * question, and a gate that needed a working encoder would not answer it.
 */
static int runVanishSelfTest() {
    int failed = 0;
    const auto check = [&failed](bool ok, const char *what) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            failed++;
    };

    QTemporaryDir scratch;
    check(scratch.isValid(), "vanish: scratch tree");
    if (!scratch.isValid())
        return 1;

    const QString root = scratch.path() + QStringLiteral("/music");
    const QString offline = scratch.path() + QStringLiteral("/offline");
    const QString foreign = scratch.path() + QStringLiteral("/foreign");
    const QString here = root + QStringLiteral("/here.mp3");
    const QString gone = root + QStringLiteral("/gone.mp3");
    const QString unmounted = offline + QStringLiteral("/offline-album.mp3");
    const QString unowned = foreign + QStringLiteral("/foreign-album.mp3");
    QDir().mkpath(root);
    QDir().mkpath(foreign);
    // `offline` is never created: it is a drive that is not there today.

    QFile present(here);
    bool wrote = present.open(QIODevice::WriteOnly);
    if (wrote) {
        present.write("not decodable audio, and retirement does not care\n");
        present.close();
    }
    check(wrote, "vanish: a file that really is on disk");

    const auto seed = [](const QString &path) {
        LibTrack t;
        t.filepath = path;
        t.title = QStringLiteral("Track");
        t.artist = QStringLiteral("Artist");
        t.album = QStringLiteral("Album");
        t.filesize = 1;
        t.probeVersion = 1;
        return t;
    };

    const QString dbPath = scratch.path() + QStringLiteral("/vanish.db");
    {
        LibraryDb db;
        if (!db.open(dbPath)) {
            printf("  FAIL vanish: db open: %s\n", db.lastError().toUtf8().constData());
            return 1;
        }
        db.addWatchFolder(root, QStringLiteral("music"));
        db.addWatchFolder(offline, QStringLiteral("music"));
        db.upsertTrack(seed(here));
        db.upsertTrack(seed(gone));
        db.upsertTrack(seed(unmounted));
        db.upsertTrack(seed(unowned));
    }

    LibraryScanner scanner;
    scanner.setDbPath(dbPath);
    scanner.scan();   // synchronous on this thread

    QStringList left;
    {
        LibraryDb db;
        if (!db.open(dbPath))
            return 1;
        for (const LibTrack &t : db.allTracks())
            left << t.filepath;
    }
    const auto owns = [&left](const QString &path) {
        return left.contains(path);
    };

    check(owns(here),
          "vanish: a row whose file is present survives, even one that will not decode");
    check(!owns(gone),
          "vanish: a row under a watch folder this pass read to the end is retired");
    check(owns(unmounted),
          "vanish: a row under a watch folder that is not mounted is kept");
    check(owns(unowned),
          "vanish: a row no watch folder owns is kept");
    check(left.size() == 3,
          "vanish: exactly one row went, and nothing else moved");

    // A second pass must have nothing left to retire: the survivors are
    // survivors because the rule says so, not because they were not looked at.
    LibraryScanner again;
    again.setDbPath(dbPath);
    again.scan();
    int second = 0;
    {
        LibraryDb db;
        if (db.open(dbPath))
            second = int(db.allTracks().size());
    }
    check(second == 3, "vanish: a repeat scan retires nothing further");

    printf("vanish self-test: %s (%d failures)\n",
           failed == 0 ? "ALL PASS" : "FAILED", failed);
    return failed ? 1 : 0;
}

// Where a download lands, and what a folder owns.  Both rules were decided by
// nobody until 2026-10-24: a prefix comparison that counted UTF-8 bytes where
// SQLite counts characters, and an importer that wrote into a fixed folder
// beside the library instead of inside it.
//
// The Han name is built from code points, not literals: /utf-8 reaches the C
// targets only, so a UTF-8 source literal in this file would be re-encoded to
// the ANSI code page of whatever machine compiles it.
static int runFoldersSelfTest() {
    int failed = 0;
    const auto check = [&failed](bool ok, const char *what) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            failed++;
    };
    const auto eq = [&check](const char *what, const QString &got, const QString &expect) {
        const bool ok = got == expect;
        check(ok, what);
        if (!ok)
            printf("      got '%s' expected '%s'\n", got.toUtf8().constData(),
                   expect.toUtf8().constData());
    };

    QTemporaryDir scratch;
    check(scratch.isValid(), "folders: scratch tree");
    if (!scratch.isValid())
        return 1;

    // "test music", "artist", "album", "track" -- four Han characters, three
    // bytes each, so the folder is eight bytes wider than it is long.
    const QString cjkRoot = scratch.path() + QStringLiteral("/\u6d4b\u8bd5\u97f3\u4e50");
    const QString cjkFile = cjkRoot + QStringLiteral("/\u827a\u4eba/\u4e13\u8f91/\u66f2\u76ee.mp3");
    const QString asciiRoot = scratch.path() + QStringLiteral("/music");
    const QString asciiFile = asciiRoot + QStringLiteral("/Artist/Album/Song.mp3");

    const auto seed = [](const QString &path) {
        LibTrack t;
        t.filepath = path;
        t.title = QStringLiteral("Track");
        t.artist = QStringLiteral("Artist");
        t.album = QStringLiteral("Album");
        t.filesize = 1;
        return t;
    };

    const QString dbPath = scratch.path() + QStringLiteral("/folders.db");
    bool opened = false;
    {
        LibraryDb db;
        opened = db.open(dbPath);
        if (!opened) {
            printf("  FAIL folders: db open: %s\n", db.lastError().toUtf8().constData());
            return 1;
        }
        db.addWatchFolder(cjkRoot, QStringLiteral("music"));
        db.addWatchFolder(asciiRoot, QStringLiteral("music"));
        db.upsertTrack(seed(cjkFile));
        db.upsertTrack(seed(asciiFile));

        int tracks = 0, videos = 0, photos = 0;
        db.countsUnder(cjkRoot, &tracks, &videos, &photos);
        check(tracks == 1,
              "folders: a folder named in Chinese counts the media it holds");
        db.countsUnder(asciiRoot, &tracks, &videos, &photos);
        check(tracks == 1,
              "folders: an ASCII folder beside it counts its own and nothing else");
        check(db.purgeFolder(cjkRoot) == 1,
              "folders: releasing a Chinese-named folder really takes its rows");
        int left = 0;
        for (const LibTrack &t : db.allTracks())
            left++;
        check(left == 1, "folders: exactly the folder's own row went");
    }

    // ── The import root ──
    // Its own database: the folders above are music roots as well, and the
    // Han-named one is the first added, so it would answer every question
    // below about which folder comes first.
    const QString rootsPath = scratch.path() + QStringLiteral("/roots.db");
    LibraryDb db;
    if (!db.open(rootsPath))
        return 1;
    // The first folder of the kind is planted in this database rather than
    // inherited from the one above: "added first" is a fact about the rows of
    // the database being asked, and that is the rule under test.
    db.addWatchFolder(asciiRoot, QStringLiteral("music"));
    const auto idOf = [&db](const QString &path) {
        for (const WatchFolder &f : db.watchFolders())
            if (f.path == path)
                return f.id;
        return qint64(-1);
    };

    eq("folders: a kind nobody added a folder for has no import root",
       db.importRoot(QStringLiteral("photos")), QString());
    eq("folders: with no default chosen the first folder added imports",
       db.importRoot(QStringLiteral("music")), asciiRoot);

    const QString later = scratch.path() + QStringLiteral("/music-2");
    db.addWatchFolder(later, QStringLiteral("music"));
    eq("folders: adding a second folder of a kind does not move the import root",
       db.importRoot(QStringLiteral("music")), asciiRoot);

    check(db.setImportDefault(idOf(later)), "folders: marking a default is accepted");
    eq("folders: the folder the user marked is where downloads go",
       db.importRoot(QStringLiteral("music")), later);

    int marked = 0;
    for (const WatchFolder &f : db.watchFolders())
        if (f.importDefault)
            marked++;
    check(marked == 1,
          "folders: exactly one row reports itself the default, so the list cannot "
          "show two");

    const QString shots = scratch.path() + QStringLiteral("/photos");
    db.addWatchFolder(shots, QStringLiteral("photos"));
    check(db.setImportDefault(idOf(shots)),
          "folders: a photo folder can be marked the default");
    eq("folders: a photo default is the photo import root",
       db.importRoot(QStringLiteral("photos")), shots);
    eq("folders: and never the music one -- the type still decides what competes",
       db.importRoot(QStringLiteral("music")), later);

    // Per kind, because there is one destination per kind. Marking the photos
    // folder may not move the music one: a library is allowed to hold a mark
    // for each kind at once, and the Settings list renders both as the default.
    int markedNow = 0;
    for (const WatchFolder &f : db.watchFolders())
        if (f.importDefault)
            markedNow++;
    check(markedNow == 2,
          "folders: a photo default and a music default coexist, so choosing "
          "one never moves the other");

    const QString everything = scratch.path() + QStringLiteral("/everything");
    db.addWatchFolder(everything, QStringLiteral("all"));
    eq("folders: a folder typed all serves a kind nothing else covers",
       db.importRoot(QStringLiteral("video")), everything);
    eq("folders: a marked folder still beats an unmarked folder typed all",
       db.importRoot(QStringLiteral("music")), later);

    check(db.removeWatchFolder(idOf(later)), "folders: releasing the default");
    eq("folders: with the default gone the first remaining folder of that kind takes over",
       db.importRoot(QStringLiteral("music")), asciiRoot);

    // The row flags are what Settings renders as a status instead of a button,
    // and the rule behind them is per kind.  THREE rows in one library are the
    // default at the same time here: the one that receives songs, the one that
    // receives pictures, and the folder typed "all" that receives the videos
    // nobody was declared for -- because a library has three destinations, not
    // one, and a kind always has one of them.
    bool flagsAgree = true;
    for (const WatchFolder &f : db.watchFolders())
        flagsAgree &= f.importDefault == (f.path == shots || f.path == asciiRoot
                                          || f.path == everything);
    check(flagsAgree,
          "folders: watchFolders() reports every row that is some kind's default");

    // ── Marks that cannot disturb each other ──
    // The defect a per-row flag could not express: a folder typed "all"
    // competes for every kind, so marking one folder unmarked it for all three,
    // and choosing where songs go moved where pictures went.  A library is
    // supposed to be one place, and a destination the user never chose is not
    // part of that place.
    const QString allRoot = scratch.path() + QStringLiteral("/everything-2");
    const QString songRoot = scratch.path() + QStringLiteral("/songs-2");
    const QString quietPath = scratch.path() + QStringLiteral("/quiet.db");
    LibraryDb quiet;
    if (!quiet.open(quietPath))
        return 1;
    quiet.addWatchFolder(scratch.path() + QStringLiteral("/shots-2"),
                         QStringLiteral("photos"));
    quiet.addWatchFolder(allRoot, QStringLiteral("all"));
    const auto idIn = [](LibraryDb &d, const QString &p) {
        for (const WatchFolder &f : d.watchFolders())
            if (f.path == p)
                return f.id;
        return qint64(-1);
    };
    quiet.setImportDefault(idIn(quiet, allRoot));
    eq("folders: marking a folder typed all makes it receive every kind",
       quiet.importRoot(QStringLiteral("photos")), allRoot);
    eq("folders: music too", quiet.importRoot(QStringLiteral("music")), allRoot);
    eq("folders: and video", quiet.importRoot(QStringLiteral("video")), allRoot);

    quiet.addWatchFolder(songRoot, QStringLiteral("music"));
    quiet.setImportDefault(idIn(quiet, songRoot));
    eq("folders: marking a music folder moves where songs land",
       quiet.importRoot(QStringLiteral("music")), songRoot);
    eq("folders: and leaves the kind the other folder was receiving where it was",
       quiet.importRoot(QStringLiteral("photos")), allRoot);
    eq("folders: and the third kind with it",
       quiet.importRoot(QStringLiteral("video")), allRoot);

    // ── A declared folder beats a folder that merely holds everything ──
    // The other half of "the library is one place": a music download goes into
    // the music folder even in a library whose first root promised to hold all
    // media, because the point of naming a folder for a kind is that the kind
    // goes there.
    const QString generic = scratch.path() + QStringLiteral("/generic");
    const QString declaredMusic = scratch.path() + QStringLiteral("/music-3");
    const QString declaredClips = scratch.path() + QStringLiteral("/movies-3");
    LibraryDb ordered;
    if (!ordered.open(scratch.path() + QStringLiteral("/order.db")))
        return 1;
    ordered.addWatchFolder(generic, QStringLiteral("all"));
    ordered.addWatchFolder(declaredMusic, QStringLiteral("music"));
    ordered.addWatchFolder(declaredClips, QStringLiteral("movies"));
    eq("folders: a folder added first but typed all does not take a kind "
       "another folder was declared for",
       ordered.importRoot(QStringLiteral("music")), declaredMusic);
    eq("folders: nor a kind a movie folder was declared for",
       ordered.importRoot(QStringLiteral("video")), declaredClips);
    eq("folders: a kind nobody declared still goes to the folder that "
       "holds everything",
       ordered.importRoot(QStringLiteral("photos")), generic);

    // ── A library written before the mark was per kind ──
    // Rewound by hand to exactly what the v10 app wrote -- one marked column,
    // no import_defaults table, user_version 10 -- because an upgrade is where
    // a destination the user chose is either carried across or quietly lost.
    const QString oldPath = scratch.path() + QStringLiteral("/old.db");
    const QString oldPhotos = scratch.path() + QStringLiteral("/old-photos");
    const QString oldMusic = scratch.path() + QStringLiteral("/old-music");
    {
        LibraryDb old;
        if (!old.open(oldPath))
            return 1;
        old.addWatchFolder(oldPhotos, QStringLiteral("photos"));
        old.addWatchFolder(oldMusic, QStringLiteral("music"));
    }
    const auto raw = [&oldPath](const char *sql) -> bool {
        sqlite3 *handle = nullptr;
        if (sqlite3_open(oldPath.toUtf8().constData(), &handle) != SQLITE_OK) {
            if (handle) sqlite3_close(handle);
            return false;
        }
        char *err = nullptr;
        const bool ok = sqlite3_exec(handle, sql, nullptr, nullptr, &err) == SQLITE_OK;
        sqlite3_free(err);
        sqlite3_close(handle);
        return ok;
    };
    const auto rawFirst = [&oldPath](const char *sql) -> QString {
        sqlite3 *handle = nullptr;
        if (sqlite3_open(oldPath.toUtf8().constData(), &handle) != SQLITE_OK) {
            if (handle) sqlite3_close(handle);
            return QString();
        }
        QString out;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(handle, sql, -1, &st, nullptr) == SQLITE_OK
            && sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *t = sqlite3_column_text(st, 0);
            out = t ? QString::fromUtf8(reinterpret_cast<const char *>(t)) : QString();
        }
        if (st) sqlite3_finalize(st);
        sqlite3_close(handle);
        return out;
    };
    check(raw("DROP TABLE import_defaults;"
              "UPDATE watch_folders SET import_default=1 WHERE type='music';"
              "PRAGMA user_version=10;"),
          "folders: a library can be rewound to the v10 shape");
    eq("folders: the rewound library really reports v10",
       rawFirst("PRAGMA user_version"), QStringLiteral("10"));
    {
        LibraryDb upgraded;
        if (!upgraded.open(oldPath)) {
            printf("  FAIL folders: the v10 library refused to open: %s\n",
                   upgraded.lastError().toUtf8().constData());
            failed++;
        }
        eq("folders: the folder a v10 library marked survives the upgrade",
           upgraded.importRoot(QStringLiteral("music")), oldMusic);
        eq("folders: a kind v10 never marked keeps falling back to the first "
           "folder added",
           upgraded.importRoot(QStringLiteral("photos")), oldPhotos);
    }
    eq("folders: and the upgraded library is v11 again",
       rawFirst("PRAGMA user_version"), QStringLiteral("11"));
    // Only the mark the user made is copied.  Seeding the rest would freeze
    // today's fallback into a choice nobody made, and the row would read as a
    // default the user never picked.
    check(rawFirst("SELECT COUNT(*) FROM import_defaults") == QStringLiteral("1"),
          "folders: the upgrade copies marks and invents none");

    printf("folders self-test: %s (%d failures)\n",
           failed == 0 ? "ALL PASS" : "FAILED", failed);
    return failed ? 1 : 0;
}

// Which video files a watch folder may throw away, which one it may not, and
// what it owes the user for the ones it did.
//
// The size floor is a guess: sub-50MB video inside a folder declared to hold
// movies is usually trailer debris.  A guess is only tolerable when the scan
// says out loud what it guessed away, and it was never aimed at a file the app
// itself wrote into the folder that receives its downloads -- Zune video is
// small by nature, so the floor was refusing the app's own purpose.  Both
// halves need a walk that actually walked and a database that actually has rows,
// because "this folder refused one file" is a fact about a scan, not about a
// string.
static int runVideoFloorSelfTest() {
    int failed = 0;
    const auto check = [&failed](bool ok, const char *what) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            failed++;
    };
    using VideoNaming::VideoSkip;
    const qint64 MB = 1024 * 1024;

    check(VideoNaming::videoSkipReason("clip.mp4", 20 * MB, "movies", 0, false)
              == VideoSkip::sizeFloor,
          "floor: a movie folder really does throw away a 20 MB file on size");
    check(VideoNaming::videoSkipReason("clip.mp4", 20 * MB, "movies", 0, true)
              == VideoSkip::none,
          "floor: the same file the app pulled off a player is kept");
    check(VideoNaming::videoSkipReason("clip.mp4", 20 * MB, "all", 0, false)
              == VideoSkip::none,
          "floor: a folder that promised everything keeps a 20 MB clip");
    check(VideoNaming::videoSkipReason("clip.mp4", 4 * MB, "tv", 0, false)
              == VideoSkip::sizeFloor,
          "floor: a tv folder keeps its own 5 MB floor");
    check(VideoNaming::videoSkipReason("clip.mp4", 4 * MB, "tv", 1, false)
              == VideoSkip::none,
          "floor: videoMinSizeMB overrides whatever the type says");
    check(VideoNaming::videoSkipReason("trailer.mp4", 900 * MB, "movies", 0, true)
              == VideoSkip::bonusName,
          "floor: no download exemption rescues a file named trailer");
    check(VideoNaming::videoSkipReason("clip.mp4", 0, "movies", 0, false)
              == VideoSkip::none,
          "floor: a size nobody managed to read is not a reason to refuse");

    // ── The same rules through one real walk of two real folders ──
    QTemporaryDir scratch;
    check(scratch.isValid(), "floor: scratch tree");
    if (!scratch.isValid())
        return 1;

    const QString first = scratch.path() + QStringLiteral("/first-added");
    const QString marked = scratch.path() + QStringLiteral("/marked-default");
    const QString firstClip = first + QStringLiteral("/clip.mp4");
    const QString markedClip = marked + QStringLiteral("/short.mp4");
    const auto plant = [](const QString &path) {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(QByteArray(2048, 'x'));
        f.close();
        return true;
    };
    check(plant(firstClip) && plant(markedClip),
          "floor: two movie folders that each hold one real 2 KB file");

    const QString dbPath = scratch.path() + QStringLiteral("/floor.db");
    const auto mark = [&dbPath](const QString &path) {
        LibraryDb db;
        if (!db.open(dbPath))
            return false;
        for (const WatchFolder &f : db.watchFolders())
            if (f.path == path)
                return db.setImportDefault(f.id);
        return false;
    };
    {
        LibraryDb db;
        if (!db.open(dbPath)) {
            printf("  FAIL floor: db open: %s\n", db.lastError().toUtf8().constData());
            return 1;
        }
        db.addWatchFolder(first, QStringLiteral("movies"));
        db.addWatchFolder(marked, QStringLiteral("movies"));
    }
    // Marked rather than merely added first: the scan has to follow the folder
    // the user chose, not whichever row happens to sort early.
    check(mark(marked), "floor: the marked folder is the video download folder");

    QVariantList notes;
    {
        LibraryScanner scanner;
        scanner.setDbPath(dbPath);
        QObject::connect(&scanner, &LibraryScanner::mediaTypeNotes,
                         [&notes](const QVariantList &n) { notes = n; });
        scanner.scan();
    }

    QHash<QString, QPair<qint64, qint64>> rows;
    {
        LibraryDb db;
        if (!db.open(dbPath))
            return 1;
        rows = db.videoPathIndex();
    }
    check(rows.contains(markedClip),
          "floor: the clip the app would have downloaded is indexed anyway");
    check(!rows.contains(firstClip),
          "floor: the same clip in the other movie folder is not");
    check(rows.size() == 1, "floor: and nothing else about that walk was indexed");

    QString complained;
    int complainedAbout = 0;
    int otherCounts = 0;
    for (const QVariant &note : notes) {
        const QVariantMap m = note.toMap();
        complainedAbout++;
        if (m.value(QStringLiteral("sizeSkipped")).toInt() > 0)
            complained = m.value(QStringLiteral("folder")).toString();
        otherCounts += m.value(QStringLiteral("videos")).toInt()
            + m.value(QStringLiteral("photos")).toInt()
            + m.value(QStringLiteral("audio")).toInt();
    }
    // The whole point of the note: a folder that holds a video and shows no
    // video is otherwise indistinguishable from an empty folder, and this root
    // holds nothing of the wrong kind, so only a refusal could ever name it.
    check(complainedAbout == 1,
          "floor: the refusing folder is named exactly once, the exempt one never");
    check(complained == first, "floor: named by the folder that refused the file");
    check(otherCounts == 0,
          "floor: a size refusal is not filed as a wrong-type complaint");

    // Move the mark and the complaint has to move with it -- which also proves
    // the scan re-derives the folder every pass instead of remembering one.
    const bool moved = mark(first);
    QVariantList notesAgain;
    {
        LibraryScanner scanner;
        scanner.setDbPath(dbPath);
        QObject::connect(&scanner, &LibraryScanner::mediaTypeNotes,
                         [&notesAgain](const QVariantList &n) { notesAgain = n; });
        scanner.scan();
    }
    {
        LibraryDb db;
        if (db.open(dbPath))
            rows = db.videoPathIndex();
    }
    check(moved, "floor: the mark can move to the other folder of the same kind");
    check(rows.contains(firstClip) && rows.size() == 2,
          "floor: and the file that folder used to refuse is indexed");
    check(notesAgain.isEmpty(),
          "floor: and the complaint goes away instead of accumulating a second one");

    printf("videofloor self-test: %s (%d failures)\n",
           failed == 0 ? "ALL PASS" : "FAILED", failed);
    return failed ? 1 : 0;
}

// What a scan may complain about, and what it has to index anyway.
//
// Every walk hands its visitor every file under its root and throws away the
// kinds that root was not typed for, then reports the count out loud, because a
// folder holding videos that shows no videos is otherwise indistinguishable
// from an empty one.  Both halves of that promise need a walk that actually
// walked: a root typed "all" sits in all three root sets and therefore excludes
// nothing, so naming it is a lie -- and the music walk used to file the videos
// and pictures it had already indexed against exactly such a root, offering to
// set its type to the type it already had.  A typed root, meanwhile, must name
// each foreign kind once and only once, from the one walk that visits it.
static int runCensusSelfTest() {
    int failed = 0;
    const auto check = [&failed](bool ok, const char *what) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            failed++;
    };

    QTemporaryDir scratch;
    check(scratch.isValid(), "census: scratch tree");
    if (!scratch.isValid())
        return 1;

    // One shape planted four times, under four declared types: a song, a clip
    // and a picture.  The clip clears the 5 MB floor a folder typed "all"
    // carries, so indexing it is the walk's own answer and not an exemption's.
    struct Planted {
        QString root, audio, video, photo;
        bool ok = false;
    };
    const auto plant = [&scratch](const QString &name, bool withClip) {
        Planted p;
        p.root = scratch.path() + QLatin1Char('/') + name;
        p.audio = p.root + QStringLiteral("/Artist/Album/Song.mp3");
        p.video = p.root + QStringLiteral("/Show/Season 1/Episode.mp4");
        p.photo = p.root + QStringLiteral("/Album/Shot.jpg");
        const auto write = [](const QString &where, int bytes) {
            if (!QDir().mkpath(QFileInfo(where).absolutePath()))
                return false;
            QFile f(where);
            if (!f.open(QIODevice::WriteOnly))
                return false;
            f.write(QByteArray(bytes, 'x'));
            f.close();
            return true;
        };
        p.ok = write(p.audio, 2048) && write(p.photo, 2048)
            && (!withClip || write(p.video, 6 * 1024 * 1024));
        return p;
    };
    const Planted every = plant("everything", true);
    const Planted songs = plant("songs", true);
    const Planted shots = plant("shots", true);
    const Planted films = plant("films", false);
    check(every.ok && songs.ok && shots.ok && films.ok,
          "census: four roots that each hold a song, a picture and -- but for "
          "the movie folder -- a clip");

    const QString dbPath = scratch.path() + QStringLiteral("/census.db");
    {
        LibraryDb db;
        if (!db.open(dbPath)) {
            printf("  FAIL census: db open: %s\n", db.lastError().toUtf8().constData());
            return 1;
        }
        db.addWatchFolder(every.root, QStringLiteral("all"));
        db.addWatchFolder(songs.root, QStringLiteral("music"));
        db.addWatchFolder(shots.root, QStringLiteral("photos"));
        db.addWatchFolder(films.root, QStringLiteral("movies"));
    }

    QVariantList notes;
    {
        LibraryScanner scanner;
        scanner.setDbPath(dbPath);
        QObject::connect(&scanner, &LibraryScanner::mediaTypeNotes,
                         [&notes](const QVariantList &n) { notes = n; });
        scanner.scan();
    }

    QHash<QString, QVariantMap> byRoot;
    for (const QVariant &note : notes) {
        const QVariantMap m = note.toMap();
        byRoot.insert(m.value(QStringLiteral("folder")).toString(), m);
    }
    const auto countIn = [&byRoot](const QString &root, const char *field) {
        return byRoot.value(root).value(QLatin1String(field)).toInt();
    };

    // The fault this exists for.  A folder declared to hold everything indexes
    // everything, so it has no foreign kind to report and no setting left to fix.
    check(!byRoot.contains(every.root),
          "census: a folder typed all is named by no complaint at all");
    check(countIn(songs.root, "videos") == 1
              && countIn(songs.root, "photos") == 1
              && countIn(songs.root, "audio") == 0,
          "census: a music folder names the clip and the picture it skips, and "
          "not the song it kept");
    check(countIn(shots.root, "videos") == 1
              && countIn(shots.root, "audio") == 1
              && countIn(shots.root, "photos") == 0,
          "census: a photo folder names the song and the clip it skips");
    check(countIn(films.root, "audio") == 1
              && countIn(films.root, "photos") == 1
              && countIn(films.root, "videos") == 0,
          "census: a movie folder names the song and the picture it skips");
    check(notes.size() == 3,
          "census: one complaint per folder that skips something, and no second "
          "opinion from another walk");
    int sizeRefused = 0;
    for (const QVariant &note : notes)
        sizeRefused += note.toMap().value(QStringLiteral("sizeSkipped")).toInt();
    check(sizeRefused == 0, "census: nothing was refused by a size floor here");

    // And the other half: the folder that holds everything really did index all
    // three kinds, so the silence above is accuracy and not a census that never
    // ran.
    {
        LibraryDb db;
        if (!db.open(dbPath))
            return 1;
        const int clips = db.videoPathIndex().size();
        const int pictures = db.photoPathIndex().size();
        check(clips == 1,
              "census: the clip inside the folder typed all is indexed, not "
              "just counted");
        check(pictures == 2,
              "census: and so is the picture in it, beside the one in the photo "
              "folder");
    }

    printf("census self-test: %s (%d failures)\n",
           failed == 0 ? "ALL PASS" : "FAILED", failed);
    return failed ? 1 : 0;
}

static int runCoverSelfTest() {
    int failed = 0;
    const auto check = [&failed](bool ok, const char *what) {
        printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            failed++;
    };
    const auto eq = [&check](const char *what, const QString &got, const QString &expect) {
        const bool ok = got == expect;
        check(ok, what);
        if (!ok)
            printf("      got '%s' expected '%s'\n", got.toUtf8().constData(),
                   expect.toUtf8().constData());
    };

    /* ---- the only answer a store may turn into a download ---- */
    eq("cover url: a response names 100px and the local cache keeps more",
       MusicIdentityClient::storeArtworkUrl(
           QStringLiteral("https://is1-ssl.mzstatic.com/image/thumb/Music125/v4/1a/2b/3c/disc/100x100bb.jpg")),
       QStringLiteral("https://is1-ssl.mzstatic.com/image/thumb/Music125/v4/1a/2b/3c/disc/600x600bb.jpg"));
    check(MusicIdentityClient::storeArtworkUrl(
              QStringLiteral("http://is1-ssl.mzstatic.com/image/thumb/x/100x100bb.jpg")).isEmpty(),
          "cover url: plain http is not artwork");
    check(MusicIdentityClient::storeArtworkUrl(
              QStringLiteral("https://cdn.example.com/image/thumb/x/100x100bb.jpg")).isEmpty(),
          "cover url: another host is not the store's CDN");
    check(MusicIdentityClient::storeArtworkUrl(
              QStringLiteral("https://mzstatic.com.example.net/x/100x100bb.jpg")).isEmpty(),
          "cover url: a host that merely contains the CDN name is refused");
    check(MusicIdentityClient::storeArtworkUrl(
              QStringLiteral("https://is1-ssl.mzstatic.com/image/thumb/x/cover.png")).isEmpty(),
          "cover url: a file name without the size pattern is not resized");

    /* ---- which request the transport is allowed to queue ---- */
    // The pacing lives in ArtworkHttp, which no injected fetch can see, so the
    // rule that decides it is pinned here instead: the search endpoint waits,
    // the picture it names does not, and neither is decided by a substring.
    check(ArtworkHttp::isStoreSearch(QUrl(QStringLiteral(
              "https://itunes.apple.com/search?media=music&entity=album&country=jp&term=Anri"))),
          "pacing: a store search is the request a store rate limits");
    check(!ArtworkHttp::isStoreSearch(QUrl(QStringLiteral(
              "https://is1-ssl.mzstatic.com/image/thumb/Music/v4/1a/2b/3c/disc/600x600bb.jpg"))),
          "pacing: the cover itself is one picture, not a search");
    check(!ArtworkHttp::isStoreSearch(QUrl(
              QStringLiteral("https://itunes.apple.com.example.net/search"))),
          "pacing: a host that merely contains the store name is not the store");
    check(!ArtworkHttp::isStoreSearch(QUrl(
              "https://musicbrainz.org/ws/2/release-group/?fmt=json")),
          "pacing: MusicBrainz keeps its own throttle rather than this one");

    QTemporaryDir scratch;
    check(scratch.isValid(), "cover: a scratch cache directory to keep the canned answers in");
    if (!scratch.isValid())
        return 1;

    struct Catalogue {
        QHash<QString, QByteArray> bodies;
        bool failing = false;
        int asked = 0;
        QStringList order;
    } catalogue;

    const auto disc = [](const QString &artist, const QString &album, qint64 id,
                         const QString &date,
                         const QString &wrapper = QStringLiteral("collection")) {
        QJsonObject object;
        object.insert(QStringLiteral("wrapperType"), wrapper);
        object.insert(QStringLiteral("kind"), QStringLiteral("album"));
        object.insert(QStringLiteral("artistName"), artist);
        object.insert(QStringLiteral("collectionName"), album);
        // A store id arrives as a JSON number, which is the one shape a
        // MusicBrainz id never takes; it has to survive as text.
        object.insert(QStringLiteral("collectionId"), id);
        object.insert(QStringLiteral("releaseDate"), date);
        object.insert(QStringLiteral("artworkUrl100"),
                      QStringLiteral("https://is1-ssl.mzstatic.com/image/thumb/Music/v4/%1/100x100bb.jpg").arg(id));
        return QJsonValue(object);
    };
    const auto body = [](const QJsonArray &results) {
        QJsonObject object;
        object.insert(QStringLiteral("resultCount"), int(results.size()));
        object.insert(QStringLiteral("results"), results);
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    };
    const QByteArray nothing = body(QJsonArray());

    MusicIdentityClient client([&catalogue, nothing](const QUrl &url, bool) {
        ArtworkHttp::Response response;
        const QString country = QUrlQuery(url).queryItemValue(QStringLiteral("country"));
        ++catalogue.asked;
        catalogue.order.append(country);
        if (catalogue.failing) { response.status = 503; return response; }
        response.status = 200;
        response.body = catalogue.bodies.value(country, nothing);
        return response;
    }, scratch.path());
    const auto reset = [&catalogue] {
        catalogue = Catalogue();
    };

    /* ---- one exact record, found in the second catalogue ---- */
    catalogue.bodies.insert(QStringLiteral("us"), body(QJsonArray{
        disc(QStringLiteral("Pen-Dulum"), QStringLiteral("Watercolour"), 111, QStringLiteral("2004-05-05")),
        disc(QStringLiteral("Cherish the Ladies"), QStringLiteral("Watercolour - EP"), 222,
             QStringLiteral("2001-01-01"))}));
    catalogue.bodies.insert(QStringLiteral("jp"), body(QJsonArray{
        disc(QStringLiteral("Noriko Sakai"), QStringLiteral("Watercolour"), 1258492988,
             QStringLiteral("2009-03-04"))}));
    auto result = client.resolveStoreAlbumArtwork(QStringLiteral("Noriko Sakai"),
                                                  QStringLiteral("Watercolour"));
    eq("match: an American decoy does not stop the ladder", result.status, QStringLiteral("ready"));
    eq("match: a numeric catalogue id becomes text",
       result.identity.value(QStringLiteral("providerId")).toString(),
       QStringLiteral("1258492988"));
    eq("match: the catalogue that answered is recorded",
       result.identity.value(QStringLiteral("country")).toString(), QStringLiteral("jp"));
    eq("match: the picture is provenanced, not anonymous",
       result.identity.value(QStringLiteral("provider")).toString(), QStringLiteral("store"));
    check(result.imageUrl.endsWith(QStringLiteral("/600x600bb.jpg")),
          "match: the cover downloaded is the larger render of the same picture");
    eq("match: the ladder stopped where the record was", catalogue.order.join(QLatin1Char(',')),
       QStringLiteral("us,jp"));
    check(result.candidates.isEmpty(),
          "match: store rows are never offered as Customize candidates");
    reset();

    /* ---- the first catalogue that carries it ends the search ---- */
    catalogue.bodies.insert(QStringLiteral("us"), body(QJsonArray{
        disc(QStringLiteral("EGOIST"), QStringLiteral("Voyage"), 555, QStringLiteral("2012-06-06"))}));
    result = client.resolveStoreAlbumArtwork(QStringLiteral("EGOIST"), QStringLiteral("Voyage"));
    eq("ladder: a hit in the first catalogue is the answer", result.status, QStringLiteral("ready"));
    eq("ladder: no further catalogue is asked once one has matched",
       catalogue.order.join(QLatin1Char(',')), QStringLiteral("us"));
    reset();

    /* ---- several editions of one record, one deterministic cover ---- */
    catalogue.bodies.insert(QStringLiteral("us"), body(QJsonArray{
        disc(QStringLiteral("EGOIST"), QStringLiteral("Anthology"), 300, QStringLiteral("2009-01-07")),
        disc(QStringLiteral("EGOIST"), QStringLiteral("Anthology"), 200, QStringLiteral("2009-01-07")),
        disc(QStringLiteral("EGOIST"), QStringLiteral("Anthology"), 100, QStringLiteral("2011-05-05"))}));
    result = client.resolveStoreAlbumArtwork(QStringLiteral("EGOIST"), QStringLiteral("Anthology"));
    eq("edition: earliest release wins, and the smallest id breaks a tie",
       result.identity.value(QStringLiteral("providerId")).toString(), QStringLiteral("200"));
    reset();

    /* ---- a single is not an album ---- */
    catalogue.bodies.insert(QStringLiteral("jp"), body(QJsonArray{
        disc(QStringLiteral("Yura Hayashi"), QStringLiteral("Single"), 400, QStringLiteral("2015-02-02"),
             QStringLiteral("track"))}));
    result = client.resolveStoreAlbumArtwork(QStringLiteral("Yura Hayashi"), QStringLiteral("Single"));
    // The row carries a cover and the exact name, and it is still refused: the
    // store is describing a single, and single artwork is not this album's
    // artwork.  "No cover available" is the honest verdict then - "needs a
    // match" would send the user to Customize, where a store row cannot be
    // picked anyway because only MusicBrainz offers anything there.
    eq("wrapper: a matching track row is not an album cover", result.status,
       QStringLiteral("noArtwork"));
    check(result.imageUrl.isEmpty() && result.identity.isEmpty(),
          "wrapper: a single's cover is never taken for the album's");
    check(!result.reason.isEmpty(), "wrapper: the card can say the store had no album");
    reset();

    /* ---- rows everywhere, no name anywhere ---- */
    for (const char *country : {"us", "jp", "tw", "hk", "gb", "de"})
        catalogue.bodies.insert(QString::fromLatin1(country), body(QJsonArray{
            disc(QStringLiteral("Someone Else"), QStringLiteral("Greatest"), 600,
                 QStringLiteral("1999-01-01"))}));
    result = client.resolveStoreAlbumArtwork(QStringLiteral("Test Artist"), QStringLiteral("Greatest"));
    eq("unmatched: a store full of near misses is a match to make, not a guess",
       result.status, QStringLiteral("needsMatch"));
    check(result.imageUrl.isEmpty() && result.identity.isEmpty(),
          "unmatched: a near miss is never half-applied");
    eq("unmatched: every catalogue is asked before giving up",
       QString::number(catalogue.asked),
       QStringLiteral("6"));
    reset();

    /* ---- nothing anywhere, which is terminal rather than retryable ---- */
    result = client.resolveStoreAlbumArtwork(QStringLiteral("Test Artist"), QStringLiteral("Unobtainium"));
    eq("absent: an empty answer from every catalogue is no artwork", result.status,
       QStringLiteral("noArtwork"));
    check(!result.reason.isEmpty(), "absent: the card can say why it stayed blank");
    reset();

    /* ---- the store itself is unreachable ---- */
    catalogue.failing = true;
    result = client.resolveStoreAlbumArtwork(QStringLiteral("Test Artist"), QStringLiteral("Offline"));
    eq("outage: a store that never answers is not an album with no cover", result.status,
       QStringLiteral("providerUnavailable"));
    check(!result.reason.isEmpty(), "outage: the reason names the provider, not the album");
    // The refusal came from the endpoint, not from one country's catalogue, so
    // the five rungs behind it are the same question asked five more times.
    // This is the assertion that keeps a rate limit from becoming a burst.
    eq("outage: one unanswered catalogue is enough to learn it",
       QString::number(catalogue.asked), QStringLiteral("1"));
    reset();

    /* ---- a library row that cannot identify anything ---- */
    result = client.resolveStoreAlbumArtwork(QStringLiteral("Unknown Artist"), QStringLiteral("Untitled"));
    eq("unknown: a placeholder name is never sent to a store", result.status,
       QStringLiteral("needsMatch"));
    eq("unknown: and not one request left the machine", QString::number(catalogue.asked),
       QStringLiteral("0"));

    printf("album cover self-test: %s (%d failures)\n",
           failed == 0 ? "ALL PASS" : "FAILED", failed);
    return failed == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
#if defined(_WIN32)
    /* The console renders the active ANSI code page, so every UTF-8 path this
     * gate prints - the ones with Chinese, Hebrew or emoji in them - came out
     * as '?'.  The arguments were already fixed by reading them back through
     * Qt; this fixes what the operator can actually read. */
    zuuned_win_console_utf8();
#endif

    /* Read the arguments back through Qt rather than argv.  On Windows the CRT
     * builds narrow argv from the wide command line with the ANSI code page, so
     * QString::fromUtf8(argv[n]) decoded every non-ASCII library folder into
     * '?'-filled garbage; the scan then truthfully reported zero tracks and
     * exited 0, which reads as an empty library rather than a broken argument.
     * QCoreApplication re-reads GetCommandLineW() and survives the name. */
    const QStringList arguments = app.arguments();

    const QString mode = arguments.size() >= 2 ? arguments.at(1) : QString();

    if (mode == QLatin1String("names"))
        return runNamesSelfTest();
    if (mode == QLatin1String("customize"))
        return runCustomizeSelfTest();
    if (mode == QLatin1String("paths"))
        return runPathsSelfTest();
    if (mode == QLatin1String("net"))
        return runNetworkSelfTest();
    if (mode == QLatin1String("vanish"))
        return runVanishSelfTest();
    if (mode == QLatin1String("cover"))
        return runCoverSelfTest();
    if (mode == QLatin1String("folders"))
        return runFoldersSelfTest();
    if (mode == QLatin1String("videofloor"))
        return runVideoFloorSelfTest();
    if (mode == QLatin1String("census"))
        return runCensusSelfTest();

    const bool videoMode = mode == QLatin1String("videoscan");
    const bool photoMode = mode == QLatin1String("photoscan");
    if (arguments.size() < 4
        || (mode != QLatin1String("scan") && !videoMode && !photoMode)) {
        fprintf(stderr,
                "librarytool — library core gate\n"
                "  librarytool scan <folder> <tmpdb>\n"
                "  librarytool videoscan <folder> <tmpdb> [type: tv (default), movies,\n"
                "      anime, or any watch-folder type the scan should force]\n"
                "  librarytool photoscan <folder> <tmpdb>\n"
                "  librarytool names\n"
                "  librarytool customize\n"
                "  librarytool paths\n"
                "  librarytool net\n"
                "  librarytool cover\n"
                "  librarytool vanish\n"
                "  librarytool folders\n"
                "  librarytool videofloor\n"
                "  librarytool census\n");
        return 2;
    }
    const QString folder = arguments.at(2);
    const QString dbPath = arguments.at(3);
    const QString folderType = videoMode
        ? (arguments.size() >= 5 ? arguments.at(4) : QStringLiteral("tv"))
        : photoMode ? QStringLiteral("photos") : QStringLiteral("music");

    {
        LibraryDb db;
        if (!db.open(dbPath)) {
            fprintf(stderr, "FAIL: db open: %s\n",
                    db.lastError().toUtf8().constData());
            return 1;
        }
        db.addWatchFolder(folder, folderType);
    }

    LibraryScanner scanner;
    scanner.setDbPath(dbPath);

    QObject::connect(&scanner, &LibraryScanner::progress,
        [](const QString &stage, int cur, int total, const QString &file) {
            fprintf(stderr, "[scan] %s %d/%d  %s\n",
                    stage.toUtf8().constData(), cur, total,
                    file.toUtf8().constData());
        });
    int added = -1, updated = -1, photos = -1;
    QObject::connect(&scanner, &LibraryScanner::finished,
        [&](int a, int u, int p) { added = a; updated = u; photos = p; });
    QObject::connect(&scanner, &LibraryScanner::failed,
        [](const QString &err) {
            fprintf(stderr, "FAIL: %s\n", err.toUtf8().constData());
        });

    scanner.scan(); // synchronous on this thread

    if (added < 0)
        return 1;

    printf("scan done: %d tracks added, %d updated, %d photos\n",
           added, updated, photos);

    LibraryDb db;
    if (!db.open(dbPath))
        return 1;

    if (photoMode) {
        const auto photos = db.allPhotos();
        printf("total photos in db: %d\n", int(photos.size()));
        for (const LibPhoto &p : photos)
            printf("  [%lld] '%s' | name='%s' | %lld bytes\n",
                   (long long)p.id, p.filepath.toUtf8().constData(),
                   p.filename.toUtf8().constData(), (long long)p.filesize);
        return 0;
    }

    if (!videoMode) {
        const auto tracks = db.allTracks();
        printf("total tracks in db: %d\n", int(tracks.size()));
        const int n = qMin(5, int(tracks.size()));
        for (int i = 0; i < n; i++) {
            const LibTrack &t = tracks[i];
            printf("  [%lld] '%s' | artist='%s' | albumartist='%s' | album='%s'"
                   " | genre='%s' | track=%d | dur=%dms\n",
                   (long long)t.id, t.title.toUtf8().constData(),
                   t.artist.toUtf8().constData(),
                   t.albumartist.toUtf8().constData(),
                   t.album.toUtf8().constData(), t.genre.toUtf8().constData(),
                   t.trackNumber, t.durationMs);
        }
        return 0;
    }

    // ── Phase 6 gate: run the matcher synchronously on the scan output ──
    auto videos = db.allVideos();
    printf("total videos in db: %d\n", int(videos.size()));
    for (const LibVideo &v : videos)
        printf("  [%lld] %s | series='%s' S%02dE%02d cat=%s\n",
               (long long)v.id, v.filename.toUtf8().constData(),
               v.series.toUtf8().constData(), v.season, v.episode,
               v.category.toUtf8().constData());

    QVector<LibVideo> uncached;
    for (const LibVideo &v : videos)
        if (!v.tmdbCached)
            uncached.append(v);

    if (!uncached.isEmpty()) {
        printf("matching %d videos against TMDB...\n", int(uncached.size()));
        VideoMatcher matcher(dbPath);
        QObject::connect(&matcher, &VideoMatcher::updatesCommitted,
            [](int n) { fprintf(stderr, "[match] %d rows committed\n", n); });
        matcher.matchBatch(uncached, QStringLiteral("tmdb")); // synchronous here
    }

    videos = db.allVideos();
    int matched = 0, needsMatch = 0, posters = 0;
    for (const LibVideo &v : videos) {
        if (v.tmdbId > 0) matched++;
        else if (v.tmdbCached) needsMatch++;
        if (!v.tmdbPoster.isEmpty()) posters++;
        printf("  [%lld] %s\n        → tmdb='%s' (%s) S%02dE%02d ep='%s'"
               " poster=%s still=%s\n",
               (long long)v.id, v.filename.toUtf8().constData(),
               v.tmdbTitle.toUtf8().constData(), v.tmdbYear.toUtf8().constData(),
               v.season, v.episode, v.episodeTitle.toUtf8().constData(),
               v.tmdbPoster.isEmpty() ? "NO" : "yes",
               v.episodeStill.isEmpty() ? "no" : "yes");
    }
    printf("videoscan done: %d matched, %d needs-match, %d with posters\n",
           matched, needsMatch, posters);
    return 0;
}
