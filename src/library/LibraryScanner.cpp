/* Keep first on Windows: the compat shim declares the POSIX surface this
 * file uses (64-bit struct stat, dirent, poll, ...).  Whichever <sys/stat.h>
 * the compiler parses first decides the struct layout, and the Qt headers
 * include one.  See compat/win/posix_shim.h.  Other platforms build without
 * it.  */
#if defined(_WIN32)
#include "wincompat.h"
#endif

#include "LibraryScanner.h"
#include "LibraryDb.h"
#include "VideoNaming.h"
#include "AudioCopyFingerprint.h"
#include "MusicIdentity.h"
#include "../FileTimestamps.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QVariant>
#include <QSettings>
#include <QtConcurrent/QtConcurrent>
#include <sys/stat.h>
#include <dirent.h>
#include <cerrno>
#include <unistd.h>
#include <set>
#include <utility>

extern "C" {
#include "zune.h"
#include "../UiText.h"
}

// ── Extension sets (LibraryService.swift) ──

static const QSet<QString> &audioExtensions() {
    static const QSet<QString> exts = {
        "mp3", "flac", "ogg", "m4a", "wma", "wav", "aac", "opus", "wv", "ape"
    };
    return exts;
}

static const QSet<QString> &videoExtensions() {
    static const QSet<QString> exts = {
        "mp4", "m4v", "mkv", "avi", "wmv", "mov", "mpeg", "mpg", "webm",
        "flv", "ts", "mts", "m2ts", "vob", "ogv", "3gp", "3g2", "divx",
        "asf", "rm", "rmvb", "f4v", "xvid"
    };
    return exts;
}

static const QSet<QString> &photoExtensions() {
    static const QSet<QString> exts = {
        "jpg", "jpeg", "png", "gif", "bmp", "heic"
    };
    return exts;
}

// ── Metadata resolution (resolveMetadata / extractFolderMetadata /
//    parseFilenameArtistTitle — LibraryService.swift ports) ──

namespace {

constexpr int musicParserVersion = 1;

struct WorkItem {
    QString filepath;
    qint64 filesize = 0;
    qint64 mtime = 0;
    qint64 mtimeNs = 0;
    qint64 ctimeNs = 0;
    bool isNew = true;
};

/* Every byte string this file hands the POSIX surface must be UTF-8 - which is
 * what a POSIX filename already is on Linux, and what compat/win/wincompat.c
 * decodes first on Windows.  QFile::encodeName() is NOT that: it is Qt's local
 * 8-bit codec, the ANSI code page on Windows, so any name the active code page
 * cannot spell arrives as a run of '?'.  Measured on a CP936 host: of five album
 * folders the ASCII one and the Chinese one scanned and the Hebrew, Hangul and
 * emoji ones vanished - stat() failed, fingerprint() returned false, and the
 * scan finished clean and complete with two tracks in it.  The Chinese folder
 * survived only because CP936 happens to cover Han, which is precisely the case
 * a Chinese host cannot see and an English host hits on its first Hebrew album. */
QByteArray pathBytes(const QString &path) { return path.toUtf8(); }

bool fingerprint(const QString &path, WorkItem &out) {
    struct stat info {};
    if (::stat(pathBytes(path).constData(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
    out.filepath = path;
    out.filesize = info.st_size;
    out.mtime = Zuuned::FileTimestamps::mtimeSec(info);
    out.mtimeNs = Zuuned::FileTimestamps::mtimeNs(info);
    out.ctimeNs = Zuuned::FileTimestamps::ctimeNs(info);
    return true;
}

bool accessibleDirectory(const QString &path, struct stat *identity = nullptr) {
    const QByteArray encoded = pathBytes(path);
    DIR *directory = ::opendir(encoded.constData());
    if (!directory) return false;
    struct stat current {};
#if defined(_WIN32)
    /* opendir() above already resolved this path as UTF-16 through the shim,
     * extended-length prefixes and all, and on Windows that is exactly the
     * read-and-traverse permission this function is asking about.  Asking CRT
     * access() as well would undo it: it is ANSI-only, so a folder with a
     * non-ASCII name reads as unreadable and gets skipped, and mode bit 1
     * (X_OK) is an invalid parameter there, which fast-fails the process
     * instead of returning EACCES. */
    const bool ok = ::fstat(::dirfd(directory), &current) == 0;
#else
    const bool ok = ::access(encoded.constData(), R_OK | X_OK) == 0
        && ::fstat(::dirfd(directory), &current) == 0;
#endif
    ::closedir(directory);
    if (ok && identity) *identity = current;
    return ok;
}

struct FolderMetadata { QString artist, album; int disc = 0; };
struct TrackResult { LibTrack track; QString error; };

QString normalizePath(QString p) {
    while (p.size() > 1 && p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

// Root/Artist/Album/file → (Artist, Album); Root/Album/file → (∅, Album).
// A parent folder that is itself a disc marker ("CD 1", "Disc 2") is
// skipped so the album name comes from the grandparent.
FolderMetadata extractFolderMetadata(const QString &filepath,
                                              const QSet<QString> &roots) {
    static const QRegularExpression discDir(
        QStringLiteral("^(?:cd|disc|disk)\\s*([1-9]\\d*)$"),
        QRegularExpression::CaseInsensitiveOption);

    QString parentDir = QFileInfo(filepath).absolutePath();
    if (roots.contains(normalizePath(parentDir)))
        return {};

    const auto match = discDir.match(QFileInfo(parentDir).fileName());
    const int disc = match.hasMatch() ? match.captured(1).toInt() : 0;
    if (disc > 0) {
        parentDir = QFileInfo(parentDir).absolutePath();
        if (roots.contains(normalizePath(parentDir)))
            return {QString(), QString(), disc};
    }

    const QString albumCandidate = QFileInfo(parentDir).fileName();
    const QString grandparentDir = QFileInfo(parentDir).absolutePath();
    if (roots.contains(normalizePath(grandparentDir)))
        return {QString(), albumCandidate, disc};

    const QString greatGrandparentDir = QFileInfo(grandparentDir).absolutePath();
    if (roots.contains(normalizePath(greatGrandparentDir)))
        return {QFileInfo(grandparentDir).fileName(), albumCandidate, disc};

    return {QString(), albumCandidate, disc};
}

// "Artist - Title" → (artist, title); "01 - Title" → (∅, title).
QPair<QString, QString> parseFilenameArtistTitle(const QString &nameNoExt) {
    const int idx = nameNoExt.indexOf(QStringLiteral(" - "));
    if (idx < 0)
        return {};
    const QString before = nameNoExt.left(idx);
    const QString after = nameNoExt.mid(idx + 3);
    bool allDigits = !before.isEmpty();
    for (const QChar c : before)
        if (!c.isDigit() && c != QLatin1Char('.')) { allDigits = false; break; }
    if (allDigits)
        return {QString(), after};
    return {before, after};
}

TrackResult resolveTrack(const WorkItem &work, const QSet<QString> &roots) {
    LibTrack t;
    t.filepath = work.filepath;
    t.filesize = work.filesize;
    t.mtime = work.mtime;
    t.probeMtimeNs = work.mtimeNs;
    t.probeCtimeNs = work.ctimeNs;
    t.probeVersion = musicParserVersion;

    ZuneMetadata meta {};
    if (zune_probe(work.filepath.toUtf8().constData(), &meta) == 0) {
        if (meta.title) t.title = QString::fromUtf8(meta.title).trimmed();
        // A tag can carry the Zune moniker too - a file tagged from a device dump
        // does - and a moniker is not a name (MusicIdentity.h).
        if (meta.artist) t.artist = MusicIdentity::usableName(QString::fromUtf8(meta.artist).trimmed());
        if (meta.albumartist) t.albumartist = MusicIdentity::usableName(QString::fromUtf8(meta.albumartist).trimmed());
        if (meta.album) t.album = MusicIdentity::usableName(QString::fromUtf8(meta.album).trimmed());
        if (meta.genre) t.genre = MusicIdentity::usableName(QString::fromUtf8(meta.genre).trimmed());
        t.trackNumber = meta.tracknumber;
        t.discNumber = meta.discnumber;
        t.year = meta.year;
        t.durationMs = int(meta.duration_ms);
        zune_free_metadata(&meta);
    } else {
        zune_free_metadata(&meta);
        return {{}, UiText::tr("Could not read this song. The next scan will retry.")};
    }
    WorkItem after;
    if (!fingerprint(work.filepath, after) || after.filesize != work.filesize
        || after.mtimeNs != work.mtimeNs || after.ctimeNs != work.ctimeNs)
        return {{}, UiText::tr("This song changed while it was being read. The next scan will retry.")};

    // A reliable tagged performer outranks an unverified folder owner.
    if (t.albumartist.isEmpty() && !t.artist.isEmpty()) t.albumartist = t.artist;
    const auto folder = extractFolderMetadata(work.filepath, roots);
    // Folder names are the one candidate a Zune-authored path can poison: the
    // imports folder a pull created used to be the raw device artist.
    if (t.artist.isEmpty()) t.artist = MusicIdentity::usableName(folder.artist);
    if (t.albumartist.isEmpty()) t.albumartist = MusicIdentity::usableName(folder.artist);
    if (t.album.isEmpty()) t.album = MusicIdentity::usableName(folder.album);
    if (t.discNumber <= 0) t.discNumber = folder.disc;

    // Filename "Artist - Title" fallback
    const QString nameNoExt = QFileInfo(work.filepath).completeBaseName();
    if (t.artist.isEmpty() || t.title.isEmpty()) {
        const auto [fnArtist, fnTitle] = parseFilenameArtistTitle(nameNoExt);
        if (t.title.isEmpty()) t.title = fnTitle.isEmpty() ? nameNoExt : fnTitle;
        if (t.artist.isEmpty()) t.artist = MusicIdentity::usableName(fnArtist);
    }

    if (t.title.isEmpty()) t.title = nameNoExt;
    if (t.artist.isEmpty()) t.artist = QStringLiteral("Unknown Artist");
    if (t.album.isEmpty()) t.album = QStringLiteral("Unknown Album");

    // Multi-disc Pattern B: strip trailing disc suffix from album names
    static const QRegularExpression discSuffix(
        QStringLiteral("(?:\\s*[\\(\\[]\\s*(?:cd|disc|disk)\\s*([1-9]\\d*)\\s*[\\)\\]]|\\s+(?:[-–]\\s*)?(?:cd|disc|disk)\\s*([1-9]\\d*))\\s*$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto suffix = discSuffix.match(t.album);
    const int suffixDisc = (suffix.captured(1).isEmpty() ? suffix.captured(2) : suffix.captured(1)).toInt();
    if (suffixDisc > 0 && !t.album.left(suffix.capturedStart()).trimmed().isEmpty()) {
        if (t.discNumber <= 0) t.discNumber = suffixDisc;
        t.album = t.album.left(suffix.capturedStart()).trimmed();
    }

    // Canonical: no TPE2 → track artist IS the album artist
    if (t.albumartist.isEmpty()) t.albumartist = t.artist;

    return {t, {}};
}

} // namespace

/* One watch folder, walked once, by every media phase.
 *
 * Each phase used to ask a recursive QDirIterator to do this, which is right on
 * a POSIX filesystem and quietly wrong on Windows.  A junction is not a symlink
 * as QFileInfo sees it -- isSymLink() is false, the filesystem reports
 * FILE_ATTRIBUTE_DIRECTORY and nothing else -- so a folder that links back into
 * one of its own ancestors looks exactly like a folder.  Measured on Qt 6.11
 * against  music/loop -> music: 160 entries, 96 folders, isSymLink() false for
 * every one of them, 41 levels deep when the walk finally stopped at the
 * 260-character path limit.  Nothing hung, which is the nasty part.  The scan
 * finished clean and had just filed the same handful of songs under 41
 * phantom paths, so the library goes on to show a dozen copies of every album.
 *
 * QDirIterator::FollowSymlinks is not the answer.  Qt documents that symlink
 * loops under it are automatically detected and ignored, and that detection
 * lives only in that mode -- the default mode never follows a link, so it never
 * needed to detect anything, and a junction is not a link as far as it cares.
 * The walk therefore needs an identity of its own for every folder.
 *
 * Identity comes from accessibleDirectory(): opendir + fstat, which through
 * compat/win is a real volume serial number plus a 64-bit file index on
 * Windows.  Opening a junction resolves it, so a link to an ancestor answers
 * with the ancestor's identity and cannot pass for somewhere new.  A filesystem
 * that reports zero in both fields still gets walked; it just gives up the
 * once-only guarantee it could not offer before this change either.  */
LibraryScan::WalkOutcome LibraryScan::walkFiles(
    const QString &root, QDir::Filters filters,
    const std::function<void(const QFileInfo &)> &visitFile,
    const std::atomic<bool> *cancel) {
    WalkOutcome outcome;
    struct stat rootStat {};
    if (!accessibleDirectory(root, &rootStat))
        return outcome;  // unreadable root: nothing walked, nothing certified

    outcome.complete = true;
    const auto identityOf = [](const struct stat &s) {
        return std::make_pair(quint64(s.st_dev), quint64(s.st_ino));
    };
    // Zero in both fields means this filesystem will not say which folder is
    // which, so there is nothing to deduplicate against.  Walk anyway.
    const auto usable = [](const struct stat &s) {
        return s.st_dev != 0 && s.st_ino != 0;
    };

    std::set<std::pair<quint64, quint64>> walked;
    if (usable(rootStat))
        walked.insert(identityOf(rootStat));

    // An explicit stack rather than recursion: how deep a watch folder goes is
    // the user's business and must not be able to cost us the process.
    QVector<QString> pending;
    pending.append(root);

    /* The type bits of a QDir::Filters value OR together, so adding Dirs to a
     * caller that asked for Files asks for both.  Every other bit -- Readable,
     * Hidden, System -- carries through and is applied by Qt exactly as it was
     * for QDirIterator, which is what keeps each phase's file set unchanged. */
    const QDir::Filters listing = filters | QDir::Dirs | QDir::NoDotAndDotDot;

    while (!pending.isEmpty()) {
        if (cancel && cancel->load()) {
            outcome.cancelled = true;
            outcome.complete = false;
            return outcome;
        }
        const QFileInfoList entries
            = QDir(pending.takeLast()).entryInfoList(listing, QDir::NoSort);
        outcome.directories++;
        for (const QFileInfo &entry : entries) {
            if (!entry.isDir()) {
                outcome.files++;
                visitFile(entry);
                continue;
            }
            /* A real symlink keeps the treatment QDirIterator gave it without
             * FollowSymlinks: listed, never entered.  isDir() above already
             * resolved it, so descending here is the one way to loop on POSIX. */
            if (entry.isSymLink())
                continue;
            struct stat folderStat {};
            if (!accessibleDirectory(entry.filePath(), &folderStat)) {
                /* A folder we cannot open is a hole in this walk.  The music
                 * phase reads complete to decide whether a stored probe
                 * failure is genuinely gone, so the hole has to reach it. */
                outcome.complete = false;
                continue;
            }
            const auto folderIdentity = identityOf(folderStat);
            if (walked.count(folderIdentity) > 0) {
                outcome.duplicates++;
                continue;
            }
            if (usable(folderStat))
                walked.insert(folderIdentity);
            pending.append(entry.filePath());
        }
    }
    return outcome;
}

// ── Scanner ──

LibraryScanner::LibraryScanner(QObject *parent) : QObject(parent) {}

void LibraryScanner::cancel() { m_cancel = true; }

void LibraryScanner::scan() {
    if (m_running.exchange(true))
        return;
    m_cancel = false;

    LibraryDb db;
    if (!db.open(m_dbPath)) {
        emit failed(UiText::tr("library db open failed: %1").arg(db.lastError()));
        m_running = false;
        return;
    }

    QSet<QString> musicRoots, photoRoots;
    /* Every walk here hands the visitor EVERY file under its root and then
     * discards the kinds that root was not typed for, which is why a folder
     * holding music, video and pictures that was typed "music" produced an
     * empty Videos page, an empty Photos page, and not one word about either.
     * The type is a real choice - a 40,000-file music library must not be
     * crawled for movie posters - so the walk stays as it is and the silence
     * goes instead: one counter per excluded kind, filled by the walk that is
     * already running, reported once per root.  A root is censused only by the
     * walk that visits it and a root typed "all" excludes nothing of another
     * kind, so no file is ever counted twice. */
    struct Excluded {
        int videos = 0;
        int photos = 0;
        int audio = 0;
        // Video of the right kind that the folder's size floor still refused.
        // A different complaint from the three above and a different fix, so it
        // keeps its own count rather than hiding inside `videos`.
        int sizeRefused = 0;
    };
    QHash<QString, Excluded> excluded;
    QHash<QString, QString> rootType;   // normalized path -> declared type
    // Video roots keep their folder TYPE — it forces category at insert
    // time and picks the size floor (5MB tv/anime, 50MB movies).
    QVector<WatchFolder> videoRoots;
    for (const WatchFolder &f : db.watchFolders()) {
        const QString norm = normalizePath(f.path);
        rootType.insert(norm, f.type);
        if (f.type == QLatin1String("music") || f.type == QLatin1String("all"))
            musicRoots.insert(norm);
        if (f.type == QLatin1String("photos") || f.type == QLatin1String("all"))
            photoRoots.insert(norm);
        if (f.type == QLatin1String("movies") || f.type == QLatin1String("tv")
            || f.type == QLatin1String("anime") || f.type == QLatin1String("all")) {
            WatchFolder vf = f;
            vf.path = norm;
            videoRoots.append(vf);
        }
    }

    int tracksAdded = 0, tracksUpdated = 0, photosAdded = 0;

    // Video work item — hoisted to function scope so the single unified
    // processing pass (further down) can still see it after every walk
    // has run.
    struct VideoWork {
        QString filepath;
        QString filename;
        qint64 filesize = 0;
        qint64 mtime = 0;
        qint64 existingId = -1;
        qint64 existingSize = 0;
        QString folderType;
    };
    const int videoMinSizeOverrideMB =
        QSettings().value(QStringLiteral("videoMinSizeMB"), 0).toInt();
    // The one folder a video download is allowed to land in, per the rule in
    // LibraryDb::importRoot().  Its contents are requests, not debris, so the
    // size floor below is not allowed to refuse them; every other refusal this
    // pass makes is counted and named instead of kept quiet about.
    const QString videoDownloadRoot =
        normalizePath(db.importRoot(QStringLiteral("video")));

    // ── Walk every root FIRST, collecting the work for all three media
    // kinds. The walks are cheap (enumerate + stat + mtime fast-path);
    // doing them up front yields a real GRAND TOTAL, so the progress bar
    // below is ONE continuous sweep instead of a per-phase bar that fills
    // to 100%, resets, and reads as "frozen / found nothing" — worst of
    // all on a music-heavy library, where music is the slow part. ──

    // Music work (Phase 1 — serial prep: enumerate + stat + excluded +
    // mtime fast-path; user_edited protection lives in the upsert SQL).
    QVector<WorkItem> musicWork;
    QHash<QString, QPair<quint64, quint64>> completedMusicRoots;
    {
        const auto index = db.trackProbeIndex();
        QSet<QString> retryPaths;
        for (const auto &failure : db.musicProbeFailures())
            retryPaths.insert(failure.toMap().value(QStringLiteral("filepath")).toString());
        QSet<QString> seen;
        for (const QString &root : musicRoots) {
            struct stat initialRoot {};
            if (m_cancel || !accessibleDirectory(root, &initialRoot)) continue;
            /* The identity guard inside the walk is what stops a junction from
             * being walked twice; seen stays as the cheap second opinion for a
             * filesystem that will not report a folder identity at all. */
            Excluded &hide = excluded[root];
            const auto walked = LibraryScan::walkFiles(
                root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                [&](const QFileInfo &fi) {
                    const QString path = fi.filePath();
                    const QString suffix = fi.suffix().toLower();
                    if (!audioExtensions().contains(suffix)) {
                        // Only a root that excludes this kind has anything to
                        // complain about.  The video and photo walks carry the
                        // same guard; without it a root typed "all" -- which
                        // indexes every kind, and is what the video import
                        // fallback registers -- was censused for the videos and
                        // pictures it had already indexed, and Settings offered
                        // to set its type to the type it already had.
                        if (rootType.value(root) != QLatin1String("all")) {
                            if (videoExtensions().contains(suffix)) hide.videos++;
                            else if (photoExtensions().contains(suffix)) hide.photos++;
                        }
                        return;
                    }
                    if (seen.contains(path))
                        return;
                    seen.insert(path);
                    if (db.isExcluded(path))
                        return;
                    WorkItem w;
                    if (!fingerprint(path, w)) return;
                    const auto known = index.constFind(path);
                    if (known != index.constEnd() && !retryPaths.contains(path) && known->version == musicParserVersion
                        && known->mtimeNs == w.mtimeNs && known->ctimeNs == w.ctimeNs
                        && known->filesize == w.filesize)
                        return; // unchanged fast-path
                    w.isNew = (known == index.constEnd());
                    musicWork.append(w);
                },
                &m_cancel);
            if (walked.duplicates > 0)
                fprintf(stderr,
                        "[scan] %s: %d folder(s) skipped as already walked\n",
                        qUtf8Printable(root), walked.duplicates);
            struct stat finalRoot {};
            if (!m_cancel && walked.complete && accessibleDirectory(root, &finalRoot)
                && initialRoot.st_dev == finalRoot.st_dev && initialRoot.st_ino == finalRoot.st_ino)
                completedMusicRoots.insert(root, {quint64(finalRoot.st_dev), quint64(finalRoot.st_ino)});
        }
    }

    // ═══ Videos (filename-decode only; libav probe is DEFERRED to the
    //     app's probe consumer — SMB header reads are 200-500ms/file) ═══
    QVector<VideoWork> videoWork;
    if (!videoRoots.isEmpty() && !m_cancel) {
        const auto index = db.videoPathIndex();
        const auto sizes = db.videoSizeIndex();
        for (const WatchFolder &root : videoRoots) {
            if (m_cancel) break;
            LibraryScan::walkFiles(
                root.path, QDir::Files | QDir::Readable,
                [&](const QFileInfo &fi) {
                    const QString suffix = fi.suffix().toLower();
                    const bool mine = videoExtensions().contains(suffix);
                    if (!mine && root.type != QLatin1String("all")) {
                        Excluded &hide = excluded[root.path];
                        if (photoExtensions().contains(suffix)) hide.photos++;
                        else if (audioExtensions().contains(suffix)) hide.audio++;
                    }
                    if (!mine)
                        return;
                    const QString path = fi.filePath();
                    // Plex/Jellyfin bonus-content dirs anywhere under the root
                    const QString rel = path.mid(root.path.size() + 1);
                    const QStringList parts = rel.split(QLatin1Char('/'));
                    bool inSkippedDir = false;
                    for (int part = 0; part < parts.size() - 1; part++)
                        if (VideoNaming::isSkippedDir(parts[part])) { inSkippedDir = true; break; }
                    if (inSkippedDir)
                        return;
                    const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
                    const auto known = index.constFind(path);
                    if (known != index.constEnd() && known->second == mtime)
                        return; // unchanged fast-path
                    // The skip rules come after the unchanged fast-path on
                    // purpose: a file the library already holds a row for is not
                    // something the folder refused, so counting it as one would
                    // tell a user to fix a folder that is already showing its
                    // video on the Videos page.
                    const bool appDownload = !videoDownloadRoot.isEmpty()
                        && (path == videoDownloadRoot
                            || path.startsWith(videoDownloadRoot + QLatin1Char('/')));
                    const auto skip = VideoNaming::videoSkipReason(
                        fi.fileName(), fi.size(), root.type,
                        videoMinSizeOverrideMB, appDownload);
                    if (skip == VideoNaming::VideoSkip::sizeFloor)
                        excluded[root.path].sizeRefused++;
                    if (skip != VideoNaming::VideoSkip::none)
                        return;
                    if (db.isExcluded(path))
                        return;
                    VideoWork w;
                    w.filepath = path;
                    w.filename = fi.fileName();
                    w.filesize = fi.size();
                    w.mtime = mtime;
                    if (known != index.constEnd()) {
                        w.existingId = known->first;
                        w.existingSize = sizes.value(known->first, 0);
                    }
                    w.folderType = root.type;
                    videoWork.append(w);
                },
                &m_cancel);
        }
    }

    // ═══ Photos (stat-only) ═══
    QVector<LibPhoto> photoWork;
    {
        const auto index = db.photoPathIndex();
        for (const QString &root : photoRoots) {
            if (m_cancel) break;
            Excluded &hide = excluded[root];
            LibraryScan::walkFiles(
                root, QDir::Files | QDir::Readable,
                [&](const QFileInfo &fi) {
                    const QString suffix = fi.suffix().toLower();
                    const bool mine = photoExtensions().contains(suffix);
                    if (!mine && rootType.value(root) != QLatin1String("all")) {
                        if (videoExtensions().contains(suffix)) hide.videos++;
                        else if (audioExtensions().contains(suffix)) hide.audio++;
                    }
                    if (!mine)
                        return;
                    const QString path = fi.filePath();
                    if (db.isExcluded(path))
                        return;
                    const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
                    const auto known = index.constFind(path);
                    if (known != index.constEnd() && known->second == mtime)
                        return;
                    LibPhoto shot;
                    shot.filepath = path;
                    shot.filename = fi.fileName();
                    shot.filesize = fi.size();
                    shot.mtime = mtime;
                    photoWork.append(shot);
                },
                &m_cancel);
        }
    }

    // ── One continuous progress sweep across every phase. `grandTotal`
    // is the real amount of work up front; `globalDone` only ever climbs,
    // so the bar fills once, 0→100%, weighted by where the work actually
    // is (music-heavy libraries spend the bar mostly on music). ──
    const int grandTotal =
        int(musicWork.size()) + int(videoWork.size()) + int(photoWork.size());
    int globalDone = 0;

    // ═══ Music — parallel probe in chunks of 64, serial transactional
    //     upsert per chunk, streaming refresh. ═══
    {
        const int chunkSize = 64;
        for (int off = 0; off < musicWork.size() && !m_cancel; off += chunkSize) {
            const int n = qMin(chunkSize, int(musicWork.size()) - off);
            QVector<WorkItem> chunk(musicWork.begin() + off,
                                    musicWork.begin() + off + n);

            const QVector<TrackResult> resolved = QtConcurrent::blockingMapped(
                chunk, std::function<TrackResult(const WorkItem &)>(
                    [&musicRoots](const WorkItem &w) {
                        return resolveTrack(w, musicRoots);
                    }));

            db.begin();
            bool changed = false;
            for (int i = 0; i < resolved.size(); i++) {
                if (!resolved[i].error.isEmpty()) {
                    bool copyInvalidated = false;
                    db.recordMusicProbeFailure(chunk[i].filepath, resolved[i].error, &copyInvalidated);
                    changed |= copyInvalidated;
                    fprintf(stderr, "[scanner] retry song '%s': %s\n", qUtf8Printable(chunk[i].filepath), qUtf8Printable(resolved[i].error));
                    continue;
                }
                if (db.upsertTrack(resolved[i].track)) {
                    db.clearMusicProbeFailure(chunk[i].filepath);
                    changed = true;
                    if (chunk[i].isNew) tracksAdded++;
                    else tracksUpdated++;
                }
            }
            db.commit();

            globalDone += n;
            emit progress(QStringLiteral("music"), globalDone, grandTotal,
                          chunk.last().filepath);
            if (changed) emit chunkCommitted();
            emit musicProbeFailuresChanged();
        }
    }

    // ═══ Videos — filename decode + insert/repair, chunks of 64. ═══
    {
        const int chunkSize = 64;
        for (int off = 0; off < videoWork.size() && !m_cancel; off += chunkSize) {
            const int n = qMin(chunkSize, int(videoWork.size()) - off);
            db.begin();
            for (int i = off; i < off + n; i++) {
                const VideoWork &w = videoWork[i];

                const auto parsed = VideoNaming::parseIdentity(w.filepath, w.folderType);
                const QString &seriesName = parsed.series;
                const QString &category = parsed.category;
                const int parsedSeason = parsed.season, parsedEpisode = parsed.episode;

                if (w.existingId >= 0) {
                    if (w.filesize != w.existingSize)
                        db.touchVideoChanged(w.existingId, w.filesize, w.mtime);
                    else
                        db.touchVideoMtime(w.existingId, w.mtime);
                    // Rename-in-place repair — guarded by user_edited in SQL
                    if (!category.isEmpty())
                        db.updateVideoParse(w.existingId, seriesName,
                                            parsedSeason, parsedEpisode, category);
                } else {
                    LibVideo v;
                    v.filepath = w.filepath;
                    v.filename = w.filename;
                    v.filesize = w.filesize;
                    v.mtime = w.mtime;
                    v.series = seriesName;
                    v.season = parsedSeason;
                    v.episode = parsedEpisode;
                    v.category = category;
                    db.insertVideo(v);
                }
            }
            db.commit();

            globalDone += n;
            emit progress(QStringLiteral("videos"), globalDone, grandTotal,
                          videoWork[off + n - 1].filepath);
            emit chunkCommitted();
        }
    }

    // ═══ Photos — stat-only upsert, chunks of 128. ═══
    {
        const int chunkSize = 128;
        for (int off = 0; off < photoWork.size() && !m_cancel; off += chunkSize) {
            const int n = qMin(chunkSize, int(photoWork.size()) - off);
            db.begin();
            for (int i = off; i < off + n; i++)
                if (db.upsertPhoto(photoWork[i]))
                    photosAdded++;
            db.commit();
            globalDone += n;
            emit progress(QStringLiteral("photos"), globalDone, grandTotal,
                          photoWork[off + n - 1].filepath);
            emit chunkCommitted();
        }
    }

    // Reconcile quiet retry diagnostics only after a completed, accessible
    // scan. ENOENT/ENOTDIR means a file went away; permission/network errors
    // do not. The most specific watched root wins when roots overlap, so an
    // offline nested watch folder cannot be cleared by its readable parent.
    if (!m_cancel) {
        QStringList staleFailures;
        for (const auto &failure : db.musicProbeFailures()) {
            if (m_cancel) break;
            const QString path = failure.toMap().value(QStringLiteral("filepath")).toString();
            QString owner;
            for (const QString &root : musicRoots) {
                const QString prefix = root == QLatin1String("/") ? root : root + QLatin1Char('/');
                if (path.startsWith(prefix) && root.size() > owner.size()) owner = root;
            }
            struct stat currentRoot {};
            if (owner.isEmpty() || !completedMusicRoots.contains(owner) || !accessibleDirectory(owner, &currentRoot)) continue;
            if (completedMusicRoots.value(owner) != qMakePair(quint64(currentRoot.st_dev), quint64(currentRoot.st_ino))) continue;
            struct stat file {};
            const bool absent = ::stat(pathBytes(path).constData(), &file) != 0
                && (errno == ENOENT || errno == ENOTDIR);
            if (absent || db.isExcluded(path)) staleFailures.append(path);
        }
        if (!m_cancel && !staleFailures.isEmpty() && db.begin()) {
            bool ok = true;
            for (const QString &path : staleFailures) {
                if (m_cancel || !db.clearMusicProbeFailure(path)) { ok = false; break; }
            }
            if (ok && !m_cancel && db.commit()) emit musicProbeFailuresChanged();
            else db.rollback();
        }
    }

    // Retire indexed tracks whose file is gone.  Only a root this pass read to
    // the end, and that is still the same folder it was when the walk started,
    // may certify an absence: a watch folder that could not be opened, a walk
    // that was cancelled or could not read one of its subfolders, and a root
    // that changed identity mid-scan (a remount, a replaced junction) all
    // certify nothing, so an offline drive can never empty the library.  The
    // most specific watched root wins, exactly as it does for a retry
    // diagnostic, so an offline nested folder is not cleared by its readable
    // parent.  Absence means ENOENT/ENOTDIR: a permission or network error
    // leaves the row alone, as it always has.  Rows go, files never do - put
    // the file back and the next pass indexes it again with the tags that live
    // inside it.  Without this the row outlived its music forever: the album
    // wall kept painting a coverless tile for an album that was no longer on
    // the machine, and no lookup could ever fill it because there is nothing
    // left to read.
    int tracksRetired = 0;
    if (!m_cancel) {
        QVector<QPair<qint64, QString>> vanished;
        for (const LibTrack &track : db.allTracks()) {
            if (m_cancel) break;
            QString owner;
            for (const QString &root : musicRoots) {
                const QString prefix =
                    root == QLatin1String("/") ? root : root + QLatin1Char('/');
                if (track.filepath.startsWith(prefix) && root.size() > owner.size())
                    owner = root;
            }
            // No music watch folder owns it - a spoken import, a path from a
            // folder the user removed from the library - so nothing can speak
            // for whether its file should be here.
            if (owner.isEmpty()) continue;
            struct stat currentRoot {};
            if (!completedMusicRoots.contains(owner)
                || !accessibleDirectory(owner, &currentRoot)) continue;
            if (completedMusicRoots.value(owner)
                != qMakePair(quint64(currentRoot.st_dev), quint64(currentRoot.st_ino)))
                continue;
            struct stat file {};
            if (::stat(pathBytes(track.filepath).constData(), &file) == 0
                || (errno != ENOENT && errno != ENOTDIR))
                continue;
            vanished.append(qMakePair(track.id, track.filepath));
        }
        if (!vanished.isEmpty() && !m_cancel && db.begin()) {
            bool ok = true;
            int removed = 0;
            for (const auto &row : vanished) {
                if (m_cancel || !db.removeTrack(row.first, false)) { ok = false; break; }
                removed++;
            }
            if (ok && !m_cancel && db.commit()) {
                tracksRetired = removed;
                fprintf(stderr, "[scan] retired %d vanished track(s)\n", tracksRetired);
                emit chunkCommitted();
            } else {
                db.rollback();
            }
        }
    }

    // Inspect every indexed source: missing or inconsistent tags must not
    // prevent payload evidence from discovering a copy in another folder.
    // allTracks is deterministically ordered; unchanged sources reuse the
    // persisted hash, while new/changed sources pay one worker-side full read.
    if (!m_cancel) {
        const auto sources = db.allTracks();
        const int copyTotal = int(sources.size());
        int copiesVisited = 0;
        QString lastCopyPath;
        bool changed = false;
        for (const LibTrack &track : sources) {
            if (m_cancel) break;
            lastCopyPath = track.filepath;
            emit progress(UiText::tr("checking copies"), copiesVisited++, copyTotal, lastCopyPath);
            WorkItem before;
            if (!fingerprint(track.filepath, before)) {
                if (!track.audioFingerprint.isEmpty())
                    changed |= db.updateAudioFingerprint(track, {});
                continue;
            }
            // A failed metadata probe must not reuse obsolete copy evidence.
            if (before.filesize != track.filesize || before.mtimeNs != track.probeMtimeNs
                || before.ctimeNs != track.probeCtimeNs) {
                if (!track.audioFingerprint.isEmpty())
                    changed |= db.updateAudioFingerprint(track, {});
                continue;
            }
            if (!track.audioFingerprint.isEmpty()) continue;
            const QString hash = AudioCopyFingerprint::read(track.filepath, m_cancel);
            WorkItem after;
            if (m_cancel || hash.isEmpty() || !fingerprint(track.filepath, after)
                || before.filesize != after.filesize || before.mtimeNs != after.mtimeNs
                || before.ctimeNs != after.ctimeNs) continue;
            changed |= db.updateAudioFingerprint(track, hash);
        }
        if (!m_cancel && copyTotal > 0)
            emit progress(UiText::tr("checking copies"), copiesVisited, copyTotal, lastCopyPath);
        if (changed) emit chunkCommitted();
    }

    // Name the hidden media once per root, in the log and to the UI.  A count
    // without an action would just be a second way to be ignored, so the line
    // says which setting to change and to what.
    QVariantList notes;
    QStringList censusRoots = excluded.keys();
    censusRoots.sort();
    for (const QString &root : censusRoots) {
        const Excluded hide = excluded.value(root);
        QStringList parts;
        if (hide.videos) parts << QStringLiteral("%1 video file(s)").arg(hide.videos);
        if (hide.photos) parts << QStringLiteral("%1 photo file(s)").arg(hide.photos);
        if (hide.audio) parts << QStringLiteral("%1 audio file(s)").arg(hide.audio);
        const QString type = rootType.value(root);
        if (!parts.isEmpty())
            fprintf(stderr,
                    "[scan] %s holds %s that the type \"%s\" will not index"
                    " - Settings > Library > treat as \"all\"\n",
                    qUtf8Printable(root), qUtf8Printable(parts.join(QStringLiteral(", "))),
                    qUtf8Printable(type));
        // The other complaint, with its own sentence because it has its own fix:
        // here the folder's type is right and a size guess said no.  Marking the
        // folder the download folder ends it; so does a lower videoMinSizeMB.
        if (hide.sizeRefused)
            fprintf(stderr,
                    "[scan] %s holds %d video file(s) under the size floor"
                    " - Settings > Device > set as the download folder,"
                    " or lower videoMinSizeMB\n",
                    qUtf8Printable(root), hide.sizeRefused);
        if (parts.isEmpty() && hide.sizeRefused == 0)
            continue;
        notes.append(QVariantMap{{QStringLiteral("folder"), root},
                                 {QStringLiteral("type"), type},
                                 {QStringLiteral("videos"), hide.videos},
                                 {QStringLiteral("photos"), hide.photos},
                                 {QStringLiteral("audio"), hide.audio},
                                 {QStringLiteral("sizeSkipped"), hide.sizeRefused}});
    }
    emit mediaTypeNotes(notes);

    emit finished(tracksAdded, tracksUpdated, photosAdded);
    m_running = false;
}
