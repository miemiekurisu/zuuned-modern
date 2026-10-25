#pragma once

#include "Redactor.h"
#include "SupportReport.h"
#include <QFuture>
#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>
#include <functional>

class DiagnosticsService : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool loggingAvailable READ loggingAvailable NOTIFY statusChanged)
    Q_PROPERTY(QString logDirectory READ logDirectory CONSTANT)
    Q_PROPERTY(QString logError READ logError NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QUrl lastReportUrl READ lastReportUrl NOTIFY reportChanged)
    Q_PROPERTY(bool audioSelfTestRunning READ audioSelfTestRunning NOTIFY audioSelfTestChanged)
public:
    explicit DiagnosticsService(QObject *parent = nullptr);
    ~DiagnosticsService() override;
    bool loggingAvailable() const;
    QString logDirectory() const;
    QString logError() const;
    bool busy() const { return m_busy; }
    bool audioSelfTestRunning() const { return m_audioSelfTestRunning; }
    QUrl lastReportUrl() const { return m_lastReportUrl; }
    static Zuuned::Diagnostics::Redactor::Context redactionContext();
    void setContextProvider(std::function<QJsonObject()> provider) { m_contextProvider = std::move(provider); }
    // What this build can actually convert, answered by asking the libraries
    // rather than by a table that can drift. `bundledLibav` names the libav the
    // app links in-process; `ffmpegPath`/`ffprobePath` are the external commands
    // libzune shells out to for photo thumbnails and a few probes, empty when
    // they are not on PATH. `downloadUrl` is where to get them.
    Q_INVOKABLE QVariantMap conversionTools() const;
    // Which UI language this process is actually painting in, and what
    // "Follow system" means on this machine. Both are decided by the launcher
    // (src/main.cpp), the only place that can know, so Settings can tell a
    // request apart from what is on screen instead of implying that picking a
    // language repaints the page you are standing on.
    Q_INVOKABLE QString uiLanguage() const;
    Q_INVOKABLE QString systemUiLanguage() const;
    static void setUiLanguageApplied(const QString &code);
    Q_INVOKABLE QUrl suggestedReportUrl() const;
    Q_INVOKABLE void exportReport(const QUrl &destination);
    // Prove the audio conversion this page advertises, on this machine, in this
    // build.  `conversionTools` can only say that a library is linked; a user
    // asking "is audio conversion actually usable?" deserves a measurement, not
    // an offer.  It writes a two-second tone to a temporary WAV, encodes it to
    // the configured bit rate with each encoder this build carries, probes both
    // results and sniffs their first bytes -- a file named for one container and
    // carrying another is a failure mode this project has already been taught by
    // a device pull.  Temporary files go away whatever the outcome.
    //
    // Refuses while a transfer is in flight, because one transcode at a time is
    // a property of the encoder layer rather than a preference, and a result
    // measured beside a running sync would describe neither.
    Q_INVOKABLE void runAudioSelfTest();
    Q_INVOKABLE void openLogFolder() const;
signals:
    void statusChanged();
    void busyChanged();
    void reportChanged();
    void exportFinished(bool success, const QUrl &reportUrl, const QString &error);
    // `result` is machine-readable on purpose: {ok, code, bitrate, quality,
    // mp3:{available,ok,bytes,duration,container,code}, wma:{...}}.  The words
    // belong to the page that shows them, not to a worker thread.
    void audioSelfTestChanged();
    void audioSelfTestFinished(const QVariantMap &result);
private:
    static QString s_uiLanguageApplied;
    bool m_busy = false;
    bool m_audioSelfTestRunning = false;
    QFuture<QVariantMap> m_audioSelfTest;
    bool m_lastAvailable = false;
    QString m_lastLogError;
    QUrl m_lastReportUrl;
    std::function<QJsonObject()> m_contextProvider;
    QFuture<Zuuned::Diagnostics::ReportResult> m_export;
};
