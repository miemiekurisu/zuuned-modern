#pragma once

#include <QObject>
#include <QSettings>
#include <QString>
#include <QtQml/qqmlregistration.h>

// Persistent settings — every key from the mac app (@AppStorage) with the
// same names and defaults, so behavior matches screen-for-screen. QML
// binds reactively; writes hit QSettings immediately.
#define ZUNE_SETTING(TYPE, NAME, SETTER, KEY, DEFAULT, READER)               \
    Q_PROPERTY(TYPE NAME READ NAME WRITE SETTER NOTIFY changed)              \
public:                                                                      \
    TYPE NAME() const { return m_store.value(KEY, DEFAULT).READER(); }       \
    void SETTER(const TYPE &v) {                                             \
        if (NAME() == v) return;                                             \
        m_store.setValue(KEY, v);                                            \
        emit changed();                                                      \
    }

class AppSettings : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    ZUNE_SETTING(bool, onboardingComplete, setOnboardingComplete,
                 "onboardingComplete", false, toBool)
    ZUNE_SETTING(bool, podcastAutoCleanup, setPodcastAutoCleanup,
                 "podcastAutoCleanup", false, toBool)
    ZUNE_SETTING(bool, podcastAutoQueue, setPodcastAutoQueue,
                 "podcastAutoQueue", false, toBool)

    // What music becomes on the way to the player: "mp3", "wma" or
    // "wma-lossless".
    //
    // MP3 is the default because it is the one target proven on hardware here:
    // the Windows round trip sent MP3 and read it back. WMA is offered because
    // the linked libavcodec carries wmav2 — measured, not assumed — but no Zune
    // has ever played a WMA this application made, so it stays opt-in.
    //
    // There is deliberately no VBR option: the MP3 this app writes carries a
    // hand-built ID3v2.3 tag and no Xing/Info header, so a VBR file would seek
    // by guesswork. See the measured account in transcode/libav_transcode.c.
    //
    // WMA Lossless is the third target and the only one whose encoder is not
    // FFmpeg's: it is the Windows Media Audio 9.2 Lossless codec in the Windows
    // Media Format runtime, driven in process, so nothing is vendored and no
    // upstream project is forked to get it. The price is that it exists on Windows
    // and nowhere else, so the settings page asks the build instead of assuming.
    // Two-channel WMA Lossless is documented for the Zune HD and for no other
    // model, which the page says out loud, and no player has ever been heard
    // playing a file this application wrote.
    //
    // A lossless WMA the user already owns is still carried byte for byte while
    // copyNativeAudio is on, and that route needs no encoder at all.
    Q_PROPERTY(QString syncAudioFormat READ syncAudioFormat WRITE setSyncAudioFormat NOTIFY changed)
public:
    QString syncAudioFormat() const {
        return canonicalAudioFormat(m_store.value("syncAudioFormat",
                                                  QStringLiteral("mp3")).toString());
    }
    void setSyncAudioFormat(const QString &value) {
        const auto canonical = canonicalAudioFormat(value);
        if (syncAudioFormat() == canonical) return;
        m_store.setValue("syncAudioFormat", canonical);
        emit changed();
    }
    // One rule for what a format name is, because a stored value and a value
    // arriving from QML reach the same three answers by different routes, and a
    // half-spelled or stale value has to land on the default rather than become a
    // fourth target the sync engine has never been told about.
    static QString canonicalAudioFormat(const QString &value) {
        if (value.compare(QLatin1String("wma"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("wma");
        if (value.compare(QLatin1String("wma-lossless"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("wma-lossless");
        return QStringLiteral("mp3");
    }
    Q_PROPERTY(int syncAudioBitrate READ syncAudioBitrate WRITE setSyncAudioBitrate NOTIFY changed)
public:
    int syncAudioBitrate() const {
        bool valid = false;
        const int kbps = m_store.value("syncAudioBitrate", 320).toInt(&valid);
        return valid ? qBound(32, kbps, 320) : 320;
    }
    void setSyncAudioBitrate(int value) {
        const int kbps = qBound(32, value, 320);
        if (syncAudioBitrate() == kbps) return;
        m_store.setValue("syncAudioBitrate", kbps);
        emit changed();
    }
    // Send audio that is already in the chosen format byte for byte instead of
    // decoding and re-encoding it. On by default. It is how a WMA Lossless file
    // the user already owns survives a lossy sync - nothing that encodes can put
    // those samples back - and it is the fast path when the target is WMA Lossless
    // too, because a file that is already the destination format only gets
    // recompressed by an encoder. Turn it off to force a re-encode, which is what
    // the original client always did.
    ZUNE_SETTING(bool, copyNativeAudio, setCopyNativeAudio,
                 "copyNativeAudio", true, toBool)
    // LAME's own 0-9 effort scale, where 0 spends the most CPU for the best
    // output and 9 the least.  Default 2 is the constant this build always
    // used, so an ini without the key encodes exactly as before.  It changes
    // how carefully the encoder spends the bits, not how many it gets: bit
    // rate is syncAudioBitrate above.  Clamped here and again in the encoder
    // because both ends are reachable on their own.
    Q_PROPERTY(int mp3Quality READ mp3Quality WRITE setMp3Quality NOTIFY changed)
public:
    int mp3Quality() const {
        bool valid = false;
        const int q = m_store.value("mp3Quality", 2).toInt(&valid);
        return valid ? qBound(0, q, 9) : 2;
    }
    void setMp3Quality(int value) {
        const int q = qBound(0, value, 9);
        if (mp3Quality() == q) return;
        m_store.setValue("mp3Quality", q);
        emit changed();
    }
    // Where the ffmpeg/ffprobe command-line tools live, when they are not on
    // PATH.  A folder rather than two file pickers because a Windows FFmpeg
    // download is one bin directory that carries both, and because the one
    // remaining shell-out (a device photo thumbnail, through libzune) asks the
    // OS for a bare `ffmpeg` and can only be answered by PATH -- src/main.cpp
    // prepends this folder there at launch.  Empty means "use whatever PATH
    // already has", which is every machine that installed FFmpeg properly.
    // Transcoding inside the app needs none of this: it links libav* itself.
    ZUNE_SETTING(QString, ffmpegDir, setFfmpegDir,
                 "ffmpegDir", QString(), toString)
    Q_PROPERTY(int podcastAutoQueueCount READ podcastAutoQueueCount WRITE setPodcastAutoQueueCount NOTIFY changed)
public:
    int podcastAutoQueueCount() const {
        bool valid = false;
        const int count = m_store.value("podcastAutoQueueCount", 10).toInt(&valid);
        return valid ? qBound(1, count, 5000) : 10;
    }
    void setPodcastAutoQueueCount(int value) {
        const int count = qBound(1, value, 5000);
        if (podcastAutoQueueCount() == count) return;
        m_store.setValue("podcastAutoQueueCount", count);
        emit changed();
    }
    // Display font for identity moments (header, splash, empty states).
    // Preserve existing graffiti selections when replacing the retired font.
    Q_PROPERTY(QString headerFont READ headerFont WRITE setHeaderFont NOTIFY changed)
public:
    QString headerFont() const {
        const auto value = m_store.value("headerFont", "permanentMarker").toString();
        return value == "misdemeanor" ? QStringLiteral("specimenAlien") : value;
    }
    void setHeaderFont(const QString &value) {
        const auto canonical = value == "misdemeanor" ? QStringLiteral("specimenAlien") : value;
        if (m_store.value("headerFont", "permanentMarker").toString() == canonical) return;
        m_store.setValue("headerFont", canonical);
        emit changed();
    }
    ZUNE_SETTING(QString, backgroundImagePath, setBackgroundImagePath,
                 "backgroundImagePath", QString(), toString)
    // Backdrop preset behind the whole app when no custom image is set:
    // "none" | "grime" | "dusk" (the W15 mock's veil pills).
    ZUNE_SETTING(QString, backdropPreset, setBackdropPreset,
                 "backdropPreset", QStringLiteral("none"), toString)
    // OPT-IN A–Z section headers inside the gallery grids. Default OFF:
    // the clean unbroken grid is the design; the side rail FILTERS.
    // (Orson, 2026-09-06 — breaks fragment the grid.)
    ZUNE_SETTING(bool, letterBreaks, setLetterBreaks,
                 "letterBreaks", false, toBool)
    ZUNE_SETTING(double, backgroundOpacity, setBackgroundOpacity,
                 "backgroundOpacity", 0.3, toDouble)
    ZUNE_SETTING(double, backgroundBlurAmount, setBackgroundBlurAmount,
                 "backgroundBlurAmount", 25.0, toDouble)
    // -1=auto (from connected device model), 0=Zune30 WMV,
    // 1=classic H.264 320x240, 2=ZuneHD 480x272, 3=ZuneHD 720p
    ZUNE_SETTING(int, videoProfile, setVideoProfile,
                 "videoProfile", -1, toInt)
    ZUNE_SETTING(QString, preferredAudioLang, setPreferredAudioLang,
                 "preferredAudioLang", QStringLiteral("eng"), toString)
    ZUNE_SETTING(QString, posterSource, setPosterSource,
                 "posterSource", QStringLiteral("tmdb"), toString)
    // When an album ships FLAC + MP3 of the same tracks: "keepBoth" |
    // "flac" | "mp3". Read by the scanner's duplicate resolver.
    ZUNE_SETTING(QString, duplicateFormats, setDuplicateFormats,
                 "duplicateFormats", QStringLiteral("keepBoth"), toString)
    ZUNE_SETTING(QString, tmdbApiKey, setTmdbApiKey,
                 "tmdbApiKey", QString(), toString)
    ZUNE_SETTING(QString, fanarttvApiKey, setFanarttvApiKey,
                 "fanarttvApiKey", QString(), toString)
    ZUNE_SETTING(int, videoMinSizeMB, setVideoMinSizeMB,
                 "videoMinSizeMB", 0, toInt)
    // Transcode acceleration (Settings → Device pills): "auto" = NVDEC
    // when present with SW fallback, "nvenc" = NVIDIA, "cpu" = force
    // software. "amd" is reserved (VAAPI decode not implemented; its
    // pill is disabled). Supersedes the old bool "hwDecode" key —
    // SyncEngine still honors a legacy hwDecode=false as "cpu".
    ZUNE_SETTING(QString, transcodeAccel, setTranscodeAccel,
                 "transcodeAccel", QStringLiteral("auto"), toString)
    // Ambient background spray/bloom (the grime particles). Toggleable
    // in Settings → Appearance; default on.
    ZUNE_SETTING(bool, ambientSpray, setAmbientSpray,
                 "ambientSpray", true, toBool)
    // Which language Zuuned writes its own words in: "system" follows the OS,
    // otherwise "en" or "zh_CN". Read once at launch by src/main.cpp, because a
    // QML string is translated when its binding is first evaluated -- there is
    // nothing to retranslate afterwards, so the card that writes this says
    // "next launch" rather than implying it repaints. Anything unrecognised is
    // "system": a typo in an ini must not silently pick a language nobody chose.
    Q_PROPERTY(QString uiLanguage READ uiLanguage WRITE setUiLanguage NOTIFY changed)
public:
    QString uiLanguage() const {
        const auto value = m_store.value("uiLanguage", QStringLiteral("system")).toString();
        return value == QLatin1String("en") || value == QLatin1String("zh_CN")
                   ? value : QStringLiteral("system");
    }
    void setUiLanguage(const QString &value) {
        const auto canonical =
            value == QLatin1String("en") || value == QLatin1String("zh_CN")
                ? value : QStringLiteral("system");
        if (uiLanguage() == canonical) return;
        m_store.setValue("uiLanguage", canonical);
        emit changed();
    }
    // Music player volume (0-100) — restored at launch, saved on change
    ZUNE_SETTING(double, playerVolume, setPlayerVolume,
                 "playerVolume", 100.0, toDouble)
    // Retired 2026-10-24 by the Settings audit (docs/SETTINGS_INVENTORY.md):
    // "miniPlayerFrame" and "videoControlsStyle" were declared, defaulted, and
    // then neither written by a control nor read by anybody -- scaffolding a
    // previous session left behind. A dead key is worse than no key, because it
    // survives in a user's ini looking like a setting. A leftover value there is
    // simply never read again.

    // Online metadata/artwork lookup is opt-in: the user asks, per item.
    // An automatic pass (library load, import, browsing the artist grid)
    // reads only the user's own files. A provider request needs either this
    // switch or an explicit right-click lookup on one album/artist, or the
    // Settings retry button. Default OFF - no launch contacts a provider by
    // itself, because a file that already carries a cover never needed one.
    ZUNE_SETTING(bool, autoMetadataLookup, setAutoMetadataLookup,
                 "autoMetadataLookup", false, toBool)
    // Album covers are off by default too, and now say so: the lookup itself
    // has shipped off since the port stopped asking a provider on its own, and
    // this declaration disagreed with it. A library whose files carry their own
    // art needs no answer from anybody, and a lookup that comes back empty
    // leaves every artless card reading "Needs a match" — which looks like work
    // still pending, not like files that have no cover and were never asked
    // about. Asking is the user's call: an album's right-click menu, an
    // artist's, and Settings → Library "find missing artwork" all bypass this.
    ZUNE_SETTING(bool, albumCoverLookup, setAlbumCoverLookup,
                 "albumCoverLookup", false, toBool)

    // Artwork is a reversible display preference. Each print keeps its own
    // tuning; changing looks never rewrites source artwork or metadata.
    Q_PROPERTY(QString artworkStyle READ artworkStyle WRITE setArtworkStyle NOTIFY changed)
    Q_PROPERTY(int artworkCleanDetail READ artworkCleanDetail WRITE setArtworkCleanDetail NOTIFY changed)
    Q_PROPERTY(int artworkHalftoneTexture READ artworkHalftoneTexture WRITE setArtworkHalftoneTexture NOTIFY changed)
    Q_PROPERTY(int artworkHalftoneDotSize READ artworkHalftoneDotSize WRITE setArtworkHalftoneDotSize NOTIFY changed)
    Q_PROPERTY(bool artworkHalftoneMonochrome READ artworkHalftoneMonochrome WRITE setArtworkHalftoneMonochrome NOTIFY changed)
    Q_PROPERTY(int artworkWornTexture READ artworkWornTexture WRITE setArtworkWornTexture NOTIFY changed)

public:
    explicit AppSettings(QObject *parent = nullptr) : QObject(parent) {}

    QString artworkStyle() const {
        return validArtworkStyle(m_store.value("artworkStyle", "original").toString());
    }
    void setArtworkStyle(const QString &value) {
        const auto style = validArtworkStyle(value);
        if (artworkStyle() == style) return;
        m_store.setValue("artworkStyle", style);
        emit changed();
    }
    int artworkCleanDetail() const { return artworkPercent("artworkCleanDetail", 100); }
    int artworkHalftoneTexture() const { return artworkPercent("artworkHalftoneTexture", 14); }
    int artworkHalftoneDotSize() const { return artworkPercent("artworkHalftoneDotSize", 17); }
    int artworkWornTexture() const { return artworkPercent("artworkWornTexture", 14); }
    bool artworkHalftoneMonochrome() const {
        const auto value = m_store.value("artworkHalftoneMonochrome", false).toString().toLower();
        return value == "true" || value == "1";
    }
    void setArtworkCleanDetail(int value) { setArtworkPercent("artworkCleanDetail", value, 100); }
    void setArtworkHalftoneTexture(int value) { setArtworkPercent("artworkHalftoneTexture", value, 14); }
    void setArtworkHalftoneDotSize(int value) { setArtworkPercent("artworkHalftoneDotSize", value, 17); }
    void setArtworkWornTexture(int value) { setArtworkPercent("artworkWornTexture", value, 14); }
    void setArtworkHalftoneMonochrome(bool value) {
        if (artworkHalftoneMonochrome() == value) return;
        m_store.setValue("artworkHalftoneMonochrome", value);
        emit changed();
    }
    Q_INVOKABLE void resetArtworkStyle() {
        const auto style = artworkStyle();
        // A reset is one settings change, so consumers never render a partly
        // reset Halftone. Original has no tuning and changes nothing.
        if (style == "cleanInk") {
            m_store.remove("artworkCleanDetail");
        } else if (style == "halftone") {
            m_store.remove("artworkHalftoneTexture");
            m_store.remove("artworkHalftoneDotSize");
            m_store.remove("artworkHalftoneMonochrome");
        } else if (style == "wornPrint") {
            m_store.remove("artworkWornTexture");
        } else {
            return;
        }
        emit changed();
    }

signals:
    void changed();

private:
    static QString validArtworkStyle(const QString &value) {
        if (value == "cleanInk" || value == "halftone" || value == "wornPrint") return value;
        return QStringLiteral("original");
    }
    int artworkPercent(const char *key, int fallback) const {
        bool valid = false;
        const auto value = m_store.value(key, fallback).toInt(&valid);
        return valid ? qBound(0, value, 100) : fallback;
    }
    void setArtworkPercent(const char *key, int value, int fallback) {
        value = qBound(0, value, 100);
        if (artworkPercent(key, fallback) == value) return;
        m_store.setValue(key, value);
        emit changed();
    }
    QSettings m_store;
};

#undef ZUNE_SETTING
