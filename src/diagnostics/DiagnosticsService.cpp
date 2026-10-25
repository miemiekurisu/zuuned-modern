#include "DiagnosticsService.h"
#include "../ProfilePaths.h"
#include "SessionLog.h"
#include "SupportReport.h"
#include "ZuunedBuildInfo.h"
#include "../../transcode/libav_transcode.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDesktopServices>
#include <QDir>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent>

#include <cmath>
#include <cstdlib>

using namespace Zuuned::Diagnostics;

QString DiagnosticsService::s_uiLanguageApplied = QStringLiteral("en");

QString DiagnosticsService::uiLanguage() const
{
    // The language src/main.cpp actually installed a catalogue for, which is
    // not necessarily what the ini asks for: "system" resolves, and a build
    // without the zh_CN catalogue stays in English and says so on the log.
    return s_uiLanguageApplied;
}

QString DiagnosticsService::systemUiLanguage() const
{
    return QLocale::system().language() == QLocale::Chinese
               ? QStringLiteral("zh_CN")
               : QStringLiteral("en");
}

void DiagnosticsService::setUiLanguageApplied(const QString &code)
{
    s_uiLanguageApplied = code;
}

namespace {

// Where a command-line tool actually came from.  The folder named in Settings is
// tried first and PATH second: QStandardPaths::findExecutable searches only the
// paths it is handed, so asking it about a folder and nothing else would ignore a
// PATH that is perfectly good, while a Windows FFmpeg download is one bin
// directory that carries both tools anyway.  "none" is the honest third answer.
QString resolveTool(const QString &name, const QString &folder, QString *source)
{
    if (!folder.isEmpty()) {
        const QString hit = QStandardPaths::findExecutable(name, {folder});
        if (!hit.isEmpty()) {
            *source = QStringLiteral("folder");
            return hit;
        }
    }
    const QString hit = QStandardPaths::findExecutable(name);
    *source = hit.isEmpty() ? QStringLiteral("none") : QStringLiteral("PATH");
    return hit;
}

} // namespace

QVariantMap DiagnosticsService::conversionTools() const
{
    /* Asked of the linked libraries, never of a table: a build without a WMA
     * encoder has to be able to say so, because Settings offers WMA on the
     * strength of this answer and nothing else. */
    QSettings st;
    const QString folder = st.value(QStringLiteral("ffmpegDir")).toString().trimmed();
    QString ffmpegSource;
    QString ffprobeSource;
    const QString ffmpeg = resolveTool(QStringLiteral("ffmpeg"), folder, &ffmpegSource);
    const QString ffprobe = resolveTool(QStringLiteral("ffprobe"), folder, &ffprobeSource);
    return QVariantMap{
        {QStringLiteral("bundledLibav"),
         QString::fromLatin1(zuuned_ffmpeg_version())},
        {QStringLiteral("mp3Encoder"),
         zuuned_audio_encoder_available(ZUUNED_AUDIO_MP3) != 0},
        {QStringLiteral("wmaEncoder"),
         zuuned_audio_encoder_available(ZUUNED_AUDIO_WMA) != 0},
        // Which LAME is linked.  A card that only prints the word "LAME" asks the
        // user to believe a name; a version is something a report can compare.
        {QStringLiteral("mp3EncoderVersion"),
         QString::fromLatin1(zuuned_mp3_encoder_version())},
        {QStringLiteral("mp3Quality"), zuuned_mp3_quality()},
        {QStringLiteral("ffmpegPath"), ffmpeg},
        {QStringLiteral("ffprobePath"), ffprobe},
        {QStringLiteral("ffmpegSource"), ffmpegSource},
        {QStringLiteral("ffprobeSource"), ffprobeSource},
        {QStringLiteral("commandLineTools"), !ffmpeg.isEmpty() && !ffprobe.isEmpty()},
        // The folder the user named and whether it is a folder at all.  Those two
        // need different advice: a typo is fixed, an empty box means PATH.
        {QStringLiteral("ffmpegDir"), folder},
        {QStringLiteral("ffmpegDirGiven"), !folder.isEmpty()},
        {QStringLiteral("ffmpegDirOk"), !folder.isEmpty() && QFileInfo(folder).isDir()},
        {QStringLiteral("downloadUrl"),
         QStringLiteral("https://www.gyan.dev/ffmpeg/builds/")}};
}


namespace {

constexpr double kSelfTestSeconds = 2.0;
constexpr double kSelfTestPi = 3.14159265358979323846;

// A two-second tone as plain 16-bit PCM WAV.  Two seconds because anything
// shorter is a couple of LAME frames and proves nothing about a header, and short
// because a settings page must not look like it hung.  Plain WAV because the
// encoder is what is under test: feeding it a compressed source would measure the
// decoder too and credit the encoder for someone else's work.  Ramps at both ends
// keep a hard click out of the first frame, and a click is a transient an encoder
// spends bits on that a tone does not.
bool writeTestTone(const QString &path, QString *error)
{
    const int rate = 44100;
    const int frames = int(rate * kSelfTestSeconds);
    const int channels = 2;
    const int blockAlign = channels * 2;
    const quint32 dataSize = quint32(frames) * blockAlign;

    auto put32 = [](QByteArray *out, quint32 v) {
        out->append(char(v & 0xff));
        out->append(char((v >> 8) & 0xff));
        out->append(char((v >> 16) & 0xff));
        out->append(char((v >> 24) & 0xff));
    };
    auto put16 = [](QByteArray *out, quint16 v) {
        out->append(char(v & 0xff));
        out->append(char((v >> 8) & 0xff));
    };

    QByteArray header;
    header.append("RIFF");
    put32(&header, 36 + dataSize);
    header.append("WAVE");
    header.append("fmt ");
    put32(&header, 16);
    put16(&header, 1);                        // PCM, not a codec
    put16(&header, quint16(channels));
    put32(&header, quint32(rate));
    put32(&header, quint32(rate) * blockAlign);
    put16(&header, quint16(blockAlign));
    put16(&header, 16);
    header.append("data");
    put32(&header, dataSize);

    QByteArray pcm;
    pcm.reserve(int(dataSize));
    const double fade = 0.01;
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / rate;
        double envelope = 0.28;
        if (t < fade) envelope *= t / fade;
        else if (t > kSelfTestSeconds - fade) envelope *= (kSelfTestSeconds - t) / fade;
        const auto sample = quint16(qint16(qRound(std::sin(2.0 * kSelfTestPi * 440.0 * t)
                                                 * 32767.0 * envelope)));
        for (int c = 0; c < channels; ++c) {
            pcm.append(char(sample & 0xff));
            pcm.append(char((sample >> 8) & 0xff));
        }
    }

    QFile tone(path);
    if (!tone.open(QIODevice::WriteOnly)) {
        *error = tone.errorString();
        return false;
    }
    tone.write(header);
    tone.write(pcm);
    tone.close();
    return true;
}

// What the first four bytes actually say, in words a settings row can echo.  The
// MP3 this build writes carries a hand-built ID3v2.3 tag, so "ID3" is its honest
// opening; a bare MPEG frame sync is the other.  A WMA must open on the ASF GUID
// prefix -- and a file named for one container whose bytes say another is exactly
// the fault a device pull taught this project to check for.
QString sniffContainer(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("unreadable");
    const QByteArray head = file.read(4);
    if (head.startsWith("ID3")) return QStringLiteral("ID3-tagged MPEG audio");
    if (head.size() == 4 && quint8(head.at(0)) == 0xff && (quint8(head.at(1)) & 0xe0) == 0xe0)
        return QStringLiteral("MPEG audio");
    if (head.size() == 4 && quint8(head.at(0)) == 0x30 && quint8(head.at(1)) == 0x26
        && quint8(head.at(2)) == 0xb2 && quint8(head.at(3)) == 0x75)
        return QStringLiteral("ASF");
    return QStringLiteral("unrecognised");
}


// One encoder, measured.  `code` is machine-readable ("ok", "no-encoder",
// "refused", "cancelled", "container", "unprobeable", "duration", "size") so the
// page that shows the answer owns the wording and a worker thread never has to
// guess which language the user is reading.
QVariantMap encodeTone(const QString &tonePath, int target, int bitrate)
{
    QVariantMap result;
    const bool available = zuuned_audio_encoder_available(target) != 0;
    result.insert(QStringLiteral("available"), available);
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("bytes"), 0);
    result.insert(QStringLiteral("duration"), 0.0);
    result.insert(QStringLiteral("container"), QString());
    if (!available) {
        result.insert(QStringLiteral("code"), QStringLiteral("no-encoder"));
        return result;
    }

    // A cancel left over from an aborted run would be reported as a refusal
    // instead of what it is.  Read the flag, clear it for the measurement, and put
    // it back: a cancellation the user asked for is never lost to a test.
    const int pendingCancel = zuuned_transcode_abort_requested();
    zuuned_transcode_clear_abort();
    char *producedPath = zuuned_transcode_audio_ex(tonePath.toUtf8().constData(),
                                                   "Zuuned self test", 0, target, bitrate, 0,
                                                   nullptr, nullptr);
    if (pendingCancel) zuuned_transcode_abort();
    if (!producedPath) {
        result.insert(QStringLiteral("code"), pendingCancel ? QStringLiteral("cancelled")
                                                           : QStringLiteral("refused"));
        return result;
    }
    const QString produced = QString::fromUtf8(producedPath);
    std::free(producedPath);

    const qint64 bytes = QFileInfo(produced).size();
    const QString container = sniffContainer(produced);
    double seconds = 0.0;
    const int probed = zuuned_probe_duration(produced.toUtf8().constData(), &seconds);
    result.insert(QStringLiteral("bytes"), bytes);
    result.insert(QStringLiteral("container"), container);
    result.insert(QStringLiteral("duration"), probed == 0 ? seconds : 0.0);

    const bool containerOk = target == ZUUNED_AUDIO_MP3
                                 ? container == QStringLiteral("MPEG audio")
                                       || container == QStringLiteral("ID3-tagged MPEG audio")
                                 : container == QStringLiteral("ASF");
    const bool durationOk =
        probed == 0 && seconds > kSelfTestSeconds * 0.85 && seconds < kSelfTestSeconds * 1.15;
    const double expectedBytes = double(bitrate) * 1000.0 / 8.0 * kSelfTestSeconds;
    const bool sizeOk = bytes > qint64(expectedBytes * 0.5);
    result.insert(QStringLiteral("ok"), containerOk && durationOk && sizeOk);
    if (!containerOk) result.insert(QStringLiteral("code"), QStringLiteral("container"));
    else if (probed != 0) result.insert(QStringLiteral("code"), QStringLiteral("unprobeable"));
    else if (!durationOk) result.insert(QStringLiteral("code"), QStringLiteral("duration"));
    else if (!sizeOk) result.insert(QStringLiteral("code"), QStringLiteral("size"));
    else result.insert(QStringLiteral("code"), QStringLiteral("ok"));

    QFile::remove(produced);
    return result;
}

} // namespace


void DiagnosticsService::runAudioSelfTest()
{
    if (m_audioSelfTestRunning) return;
    if (m_contextProvider) {
        const QJsonObject state = m_contextProvider();
        // One transcode at a time is a property of the encoder layer, not a
        // preference, and a measurement taken beside a live transfer would
        // describe neither.  "busy" is the truth, and the page says so.
        if (state.value(QStringLiteral("transfer_running")).toBool()
            || state.value(QStringLiteral("zune_busy")).toBool()) {
            emit audioSelfTestFinished(QVariantMap{
                {QStringLiteral("ok"), false}, {QStringLiteral("code"), QStringLiteral("busy")}});
            return;
        }
    }

    QSettings st;
    bool valid = false;
    const int bitRateValue = st.value(QStringLiteral("syncAudioBitrate"), 320).toInt(&valid);
    const int bitrate = valid ? qBound(32, bitRateValue, 320) : 320;
    valid = false;
    const int qualityValue = st.value(QStringLiteral("mp3Quality"), 2).toInt(&valid);
    const int quality = valid ? qBound(0, qualityValue, 9) : 2;
    // Measure the configuration the user picked, or the answer is about a
    // configuration nobody asked about.
    zuuned_mp3_set_quality(quality);

    m_audioSelfTestRunning = true;
    emit audioSelfTestChanged();

    auto *watcher = new QFutureWatcher<QVariantMap>(this);
    connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [this, watcher] {
        const QVariantMap result = watcher->result();
        watcher->deleteLater();
        m_audioSelfTestRunning = false;
        emit audioSelfTestChanged();
        emit audioSelfTestFinished(result);
    });
    m_audioSelfTest = QtConcurrent::run([bitrate, quality] {
        QVariantMap out;
        out.insert(QStringLiteral("bitrate"), bitrate);
        out.insert(QStringLiteral("quality"), quality);
        const QString tonePath = QDir(QDir::tempPath()).filePath(
            QStringLiteral("zuuned-selftest-%1.wav").arg(QCoreApplication::applicationPid()));
        QString toneError;
        if (!writeTestTone(tonePath, &toneError)) {
            out.insert(QStringLiteral("ok"), false);
            out.insert(QStringLiteral("code"), QStringLiteral("tone"));
            out.insert(QStringLiteral("detail"), toneError);
            return out;
        }
        const QVariantMap mp3 = encodeTone(tonePath, ZUUNED_AUDIO_MP3, bitrate);
        const QVariantMap wma = encodeTone(tonePath, ZUUNED_AUDIO_WMA, bitrate);
        QFile::remove(tonePath);
        out.insert(QStringLiteral("mp3"), mp3);
        out.insert(QStringLiteral("wma"), wma);
        const bool mp3Ok = mp3.value(QStringLiteral("ok")).toBool();
        // An absent WMA encoder is a fact about the build, not a failed test; the
        // card already lists it in red.  A present one that cannot produce a
        // two-second tone is exactly the failure the user asked about.
        const bool wmaOk = wma.value(QStringLiteral("available")).toBool()
                               ? wma.value(QStringLiteral("ok")).toBool()
                               : true;
        out.insert(QStringLiteral("ok"), mp3Ok && wmaOk);
        out.insert(QStringLiteral("code"),
                   !mp3Ok ? QStringLiteral("mp3") : (wmaOk ? QStringLiteral("ok")
                                                           : QStringLiteral("wma")));
        return out;
    });
    watcher->setFuture(m_audioSelfTest);
}

DiagnosticsService::DiagnosticsService(QObject *parent) : QObject(parent) {
    m_lastAvailable = loggingAvailable();
    m_lastLogError = logError();
    auto *status = new QTimer(this);
    status->setInterval(2000);
    connect(status, &QTimer::timeout, this, [this] {
        const bool available = loggingAvailable();
        const QString error = logError();
        if (available != m_lastAvailable || error != m_lastLogError) {
            m_lastAvailable = available;
            m_lastLogError = error;
            emit statusChanged();
        }
    });
    status->start();
}

DiagnosticsService::~DiagnosticsService() {
    // Finish the bounded file export before the process-wide logger shuts down.
    m_export.waitForFinished();
}

bool DiagnosticsService::loggingAvailable() const { return SessionLog::instance().isAvailable(); }
QString DiagnosticsService::logDirectory() const { return SessionLog::instance().directory(); }
QString DiagnosticsService::logError() const { return SessionLog::instance().error(); }

Redactor::Context DiagnosticsService::redactionContext() {
    Redactor::Context context;
    context.homePath = QDir::homePath();
    context.userName = qEnvironmentVariable("USER");
    // Windows has no USER: a report exported there would keep the real name.
    if (context.userName.isEmpty()) context.userName = qEnvironmentVariable("USERNAME");
    QSettings settings;
    const QRegularExpression secretKey(QStringLiteral("key|token|password|secret|credential"),
                                      QRegularExpression::CaseInsensitiveOption);
    // Values only reach the scrubber, never the report metadata or a log line.
    for (const auto &key : settings.allKeys()) {
        if (!secretKey.match(key).hasMatch()) continue;
        const QString secret = settings.value(key).toString();
        if (!secret.isEmpty()) context.secrets.append(secret);
    }
    return context;
}

QUrl DiagnosticsService::suggestedReportUrl() const {
    QString folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (folder.isEmpty() || !QDir(folder).exists()) folder = QDir::homePath();
    return QUrl::fromLocalFile(QDir(folder).filePath(QStringLiteral("zuuned-debug-%1.txt")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss")))));
}

void DiagnosticsService::openLogFolder() const {
    if (QDir(logDirectory()).exists() && !logDirectory().isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(logDirectory()));
}

void DiagnosticsService::exportReport(const QUrl &destination) {
    if (m_busy) return;
    m_busy = true;
    emit busyChanged();
    emit statusChanged();
    const auto redaction = redactionContext();
    SessionLog::instance().setRedactionContext(redaction);
    QJsonObject appContext = m_contextProvider ? m_contextProvider() : QJsonObject();
    appContext.insert(QStringLiteral("build"), QString::fromLatin1(ZuunedBuild::version));
    appContext.insert(QStringLiteral("app_revision"), QString::fromLatin1(ZuunedBuild::appRevision));
    appContext.insert(QStringLiteral("libzune_revision"), QString::fromLatin1(ZuunedBuild::libzuneRevision));
    appContext.insert(QStringLiteral("platform"), QGuiApplication::platformName());
    appContext.insert(QStringLiteral("logging_available"), loggingAvailable());
    QJsonArray screens;
    for (const auto *screen : QGuiApplication::screens())
        screens.append(QJsonObject {{QStringLiteral("width"), screen->size().width()},
                                    {QStringLiteral("height"), screen->size().height()},
                                    {QStringLiteral("scale"), screen->devicePixelRatio()}});
    appContext.insert(QStringLiteral("displays"), screens);
    QStringList protectedDirectories;
    for (const auto location : {QStandardPaths::AppDataLocation, QStandardPaths::AppConfigLocation,
                                QStandardPaths::CacheLocation, QStandardPaths::StateLocation})
        protectedDirectories.append(Zuuned::ProfilePaths::writableLocation(location));
    protectedDirectories.append(logDirectory());
    auto *watcher = new QFutureWatcher<ReportResult>(this);
    connect(watcher, &QFutureWatcher<ReportResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_busy = false;
        if (result.success) {
            m_lastReportUrl = result.url;
            emit reportChanged();
        }
        emit busyChanged();
        emit statusChanged();
        emit exportFinished(result.success, result.url, result.error);
    });
    m_export = QtConcurrent::run([destination, redaction, appContext, protectedDirectories] {
        auto info = systemInformation();
        info.insert(QStringLiteral("application"), appContext);
        const auto logs = SessionLog::instance().snapshot();
        return saveReport(destination, formatReport(info, logs, redaction), protectedDirectories);
    });
    watcher->setFuture(m_export);
}
