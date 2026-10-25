/* Keep first on Windows: wincompat.h drags in the POSIX shim, and whichever
 * <sys/stat.h> the compiler parses first decides the struct layout - the Qt
 * headers below include one.  See compat/win/posix_shim.h.
 */
#if defined(_WIN32)
#include "wincompat.h"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QPalette>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QSettings>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
#include <QThreadPool>
#include <QScopeGuard>
#include <cstdio>
#include <cstring>

#include "ZuunedBuildInfo.h"

#include "ApplicationIdentity.h"
#include "ProfilePaths.h"
#include "AppSettings.h"
#include "GrungeMaskProvider.h"
#include "LibraryService.h"
#include "spoken/SpokenMediaService.h"
#include "spoken/PodcastQueuePlanner.h"
#include "library/ArtworkHttp.h"
#if !defined(Q_OS_WIN)
// MPRIS is a D-Bus protocol and the Windows Qt build here has neither the bus
// nor Qt6::DBus, so the media-key adaptor stays off this platform.
#include "player/MprisService.h"
#endif
#include "player/PlayerService.h"
#include "player/VideoPlayerService.h"
#include "DeviceService.h"
#include "DeviceWorker.h"
#include "diagnostics/DiagnosticsService.h"
#include "diagnostics/SessionLog.h"
#include "library/NetworkAccess.h"
#include "sync/SyncEngine.h"

namespace {
// UI language, 2026-10-24.
//
// AppSettings::uiLanguage is the *request* ("system" | "en" | "zh_CN"); this is
// the decision, and it is taken here exactly once because a QTranslator
// installed after the QML engine has evaluated its bindings repaints nothing -
// which is precisely the thing Settings must not imply.  A build without the
// catalogue says so on the log instead of quietly staying English, and the
// answer this process chose is handed to DiagnosticsService so the Language
// card can tell a request apart from what is actually on screen.
void installUiLanguage()
{
    const QString requested = QSettings()
                                  .value(QStringLiteral("uiLanguage"), QStringLiteral("system"))
                                  .toString();
    QString wanted = requested;
    if (wanted != QLatin1String("en") && wanted != QLatin1String("zh_CN")) {
        // "system", and any typo an ini edit can produce, resolves the same way.
        wanted = QLocale::system().language() == QLocale::Chinese
                     ? QStringLiteral("zh_CN")
                     : QStringLiteral("en");
    }
    if (wanted != QLatin1String("zh_CN")) {
        DiagnosticsService::setUiLanguageApplied(QStringLiteral("en"));
        std::fprintf(stderr, "[i18n] UI language en (setting was \"%s\")\n",
                     qUtf8Printable(requested));
        return;
    }

    auto *own = new QTranslator(qApp);
    // A translation is data, not code.  The editable source of truth is
    // i18n/zh_CN.tsv; i18n/build_qm.ps1 turns it into a .qm with lrelease, and a
    // .qm in an i18n folder next to the executable - or in the folder named by
    // ZUUNED_I18N_DIR - is used in preference to the copy compiled into the
    // binary, so rewording the interface is a file edit and a restart rather
    // than a build.  qt_add_translations() still builds i18n/zuuned_zh_CN.ts
    // into :/i18n as the fallback a plain checkout gets; a configure with no
    // LinguistTools has neither, which is a packaging difference, not a runtime
    // one.
    QString catalogue = QStringLiteral("compiled in");
    bool haveOwn = false;
    const QString fromEnv = qEnvironmentVariable("ZUUNED_I18N_DIR");
    const QString beside = QGuiApplication::applicationDirPath() + QStringLiteral("/i18n");
    for (const QString &dir : {fromEnv, beside}) {
        if (dir.isEmpty())
            continue;
        if (own->load(QStringLiteral("zuuned_zh_CN"), dir)) {
            haveOwn = true;
            catalogue = dir;
            break;
        }
    }
    if (!haveOwn)
        haveOwn = own->load(QStringLiteral("zuuned_zh_CN"), QStringLiteral(":/i18n"));
    if (haveOwn) {
        QCoreApplication::installTranslator(own);
    } else {
        delete own;
        std::fprintf(stderr,
                     "[i18n] zh_CN requested but no zuuned_zh_CN catalogue is "
                     "compiled into this build or sitting beside it; staying English\n");
        DiagnosticsService::setUiLanguageApplied(QStringLiteral("en"));
        return;
    }
    // Qt's own words - the standard file dialog buttons, Calendar and the
    // Controls the Basic style still delegates to.  Best effort: whichever
    // base has a shipped zh_CN .qm next to this Qt answers.
    QString qtAnswered;
    for (const char *base : {"qtbase", "qtquickdialogs2", "qtquickcontrols2"}) {
        auto *qt = new QTranslator(qApp);
        if (qt->load(QLocale(QStringLiteral("zh_CN")), QString::fromLatin1(base), "_",
                     QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
            QCoreApplication::installTranslator(qt);
            qtAnswered += QString::fromLatin1(base) + QLatin1Char(' ');
        } else {
            delete qt;
        }
    }
    DiagnosticsService::setUiLanguageApplied(QStringLiteral("zh_CN"));
    std::fprintf(stderr, "[i18n] UI language zh_CN; catalogue %s; Qt catalogues: %s\n",
                 qUtf8Printable(catalogue),
                 qtAnswered.isEmpty() ? "none" : qUtf8Printable(qtAnswered.trimmed()));
}

} // namespace

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
    // Binary stdio mode and unbuffered stderr have to be in place before the
    // first byte reaches a descriptor.  See compat/win/wincompat.h.
    zuuned_win_runtime_init();
#endif
    // Safe release identification: no GUI, scanner, USB or settings startup.
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("Zuuned %s\n", ZuunedBuild::version);
        return 0;
    }
    // Capture native C/Qt startup errors, including failures before a window
    // exists. The guard outlives the engine/application and their workers.
    QCoreApplication::setApplicationName(QStringLiteral("Zuuned"));
    QCoreApplication::setOrganizationName(QStringLiteral("Zuuned"));
#ifdef Q_OS_WIN
    // Windows' "native" settings format is the registry, but the rest of a
    // Zuuned profile (library, logs, cache, instance lock) is one folder under
    // %LOCALAPPDATA%. Keep settings in that model: an ini in %APPDATA%\Zuuned,
    // so a profile can be inspected, moved, deleted or isolated by a test the
    // way HOME isolates it on Linux. No Windows build shipped a registry
    // profile, so the only values to honour are pre-file ones (see below).
    QSettings::setDefaultFormat(QSettings::IniFormat);
#endif
    // ZUUNED_PROFILE_DIR, when set, owns every profile file including the
    // settings file from here on, before the first QSettings object exists.  It
    // has to follow setDefaultFormat, which decides the file the path applies
    // to.  Unset, this is a no-op and the platform layout is exactly as before.
    Zuuned::ProfilePaths::applySettingsPath();
    auto &sessionLog = Zuuned::Diagnostics::SessionLog::instance();
    sessionLog.setRedactionContext(DiagnosticsService::redactionContext());
    const QString logRoot = Zuuned::ProfilePaths::writableLocation(QStandardPaths::StateLocation);
    if (!sessionLog.start(logRoot.isEmpty() ? QString() : logRoot + QStringLiteral("/logs")))
        std::fprintf(stderr, "[diagnostics] Local logs unavailable: %s\n", qUtf8Printable(sessionLog.error()));
    struct LogLifetime {
        ~LogLifetime() { Zuuned::Diagnostics::SessionLog::instance().stop(); }
    } logLifetime;
    // File/folder pickers belong to the desktop, including its chosen theme.
    // Native installs may supply their own Qt integration; otherwise prefer
    // the desktop portal. AppRun pins the matching bundled portal plugin so
    // a host Qt theme plugin cannot fail against the AppImage's Qt version.
#if !defined(Q_OS_WIN)
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME"))
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
#endif

    // MpvVideoItem (QQuickFramebufferObject + mpv_render_context) is
    // OpenGL-only — pin the RHI backend before any window exists.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    // Qt's automatic shader/pipeline cache is the one profile file this binary
    // does not open: Qt6Quick asks QStandardPaths::CacheLocation directly, so it
    // would land in %LOCALAPPDATA%\Zuuned even when ZUUNED_PROFILE_DIR put the
    // rest of the profile elsewhere.  Name both ends of it explicitly so a
    // throwaway profile neither reads nor writes the real one's cache.  The
    // filename matches Qt's own per-backend name; OpenGL is pinned above, so it
    // is the only backend this process can have.  A brand new profile logs one
    // "Could not open pipeline cache source file" warning before Qt has written
    // the file once: that is the isolation working, not a fault.  With
    // ZUUNED_PROFILE_DIR unset nothing here runs and Qt keeps its default.
    if (!Zuuned::ProfilePaths::root().isEmpty()) {
        const QString pipelineCache = Zuuned::ProfilePaths::writableLocation(QStandardPaths::CacheLocation)
                                      + QStringLiteral("/qqpc_opengl");
        qputenv("QSG_RHI_PIPELINE_CACHE_LOAD", pipelineCache.toUtf8());
        qputenv("QSG_RHI_PIPELINE_CACHE_SAVE", pipelineCache.toUtf8());
        std::fprintf(stderr, "[profile] Qt pipeline cache goes to \"%s\"\n", qUtf8Printable(pipelineCache));
    }

    // Say which route provider requests take, once, at startup.  A
    // "[customize-http] HTTP 0, Qt error 4" line cannot be read without it:
    // status 0 and TimeoutError look identical whether the request went direct
    // into a network that drops Deezer and Google or through a proxy that
    // refused it, and only one of those is the application's business
    // (NetworkAccess.h holds the measurements, including the one that says Qt
    // already follows the Windows proxy settings without any help from here).
    {
        const auto network = NetworkAccess::apply();
        std::fprintf(stderr, "[network] %s\n",
                     qUtf8Printable(NetworkAccess::summary(network)));
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("Zuuned");
    app.setOrganizationName("Zuuned");
    app.setApplicationVersion(QString::fromLatin1(ZuunedBuild::version));
#ifdef Q_OS_WIN
    {
        // One-time import for a profile that only ever lived in the registry.
        QSettings fileSettings;
        if (!QFile::exists(fileSettings.fileName())) {
            // NativeFormat is the registry on Windows; Qt 6 has no alias.
            QSettings registry(QSettings::NativeFormat, QSettings::UserScope,
                               QStringLiteral("Zuuned"), QStringLiteral("Zuuned"));
            const QStringList keys = registry.allKeys();
            for (const QString &key : keys)
                fileSettings.setValue(key, registry.value(key));
            if (!keys.isEmpty()) {
                fileSettings.sync();
                std::fprintf(stderr,
                             "[settings] imported %d value(s) from the Windows "
                             "registry into %s\n",
                             int(keys.size()), qUtf8Printable(fileSettings.fileName()));
            }
        }
    }
#endif
    // A FFmpeg folder named in Settings is honoured before anything can ask for
    // a command, because the two ways this process finds one read different
    // places.  QStandardPaths::findExecutable also takes the folder directly
    // (DiagnosticsService::conversionTools does that, so Settings reports the
    // folder even when a Han-named path does not survive the environment block),
    // but the one shell-out this application cannot hand a path to -- libzune's
    // device photo thumbnail, which runs a bare `ffmpeg` through the shell --
    // only ever finds a tool through PATH.  So the folder goes in front of PATH
    // here, once, before any worker starts.  With the key unset or the folder
    // gone, nothing changes and PATH is exactly what the OS handed us.
    {
        const QString ffmpegDir =
            QSettings().value(QStringLiteral("ffmpegDir")).toString().trimmed();
        if (ffmpegDir.isEmpty()) {
            // Nothing to do; the log line stays silent so a normal profile does
            // not carry a line about a tool it never asked for.
        } else if (QFileInfo(ffmpegDir).isDir()) {
            const QString pathEnv = qEnvironmentVariable("PATH");
            qputenv("PATH",
                    (ffmpegDir + QDir::listSeparator() + pathEnv).toLocal8Bit());
            std::fprintf(stderr,
                         "[ffmpeg] Settings folder \"%s\" put in front of PATH; "
                         "ffmpeg resolves to \"%s\"\n",
                         qUtf8Printable(ffmpegDir),
                         qUtf8Printable(QStandardPaths::findExecutable(
                             QStringLiteral("ffmpeg"), {ffmpegDir})));
        } else {
            std::fprintf(stderr,
                         "[ffmpeg] Settings folder \"%s\" is not a folder; "
                         "falling back to PATH\n",
                         qUtf8Printable(ffmpegDir));
        }
    }

    installUiLanguage();
    Zuuned::configureApplicationIdentity(app);
    std::fprintf(stderr, "[zuuned] %s; Qt runtime %s\n", ZuunedBuild::version, qVersion());

    // The application palette mirrors Theme.qml. External desktop dialogs
    // use the desktop's palette and are not restyled by ZUUNED.
    {
        QPalette pal;
        const QColor bg(0x0a, 0x0a, 0x0a), surface(0x14, 0x14, 0x14);
        const QColor text(255, 255, 255, 235), dim(255, 255, 255, 140);
        const QColor pink(0xd4, 0x36, 0x7a);
        pal.setColor(QPalette::Window, bg);
        pal.setColor(QPalette::WindowText, text);
        pal.setColor(QPalette::Base, surface);
        pal.setColor(QPalette::AlternateBase, bg);
        pal.setColor(QPalette::Text, text);
        pal.setColor(QPalette::PlaceholderText, dim);
        pal.setColor(QPalette::Button, surface);
        pal.setColor(QPalette::ButtonText, text);
        pal.setColor(QPalette::Highlight, pink);
        pal.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
        pal.setColor(QPalette::ToolTipBase, surface);
        pal.setColor(QPalette::ToolTipText, text);
        app.setPalette(pal);
    }

    // C2: single instance. Two instances share the library DB and fight
    // over the USB claim — a data hazard, not a convenience question.
    const QString runDir =
        Zuuned::ProfilePaths::writableLocation(QStandardPaths::RuntimeLocation);
    // Test harness (scale profiles): ZUUNED_LOCK_SUFFIX isolates the
    // lock so an instrumented instance can run beside the daily one.
    QLockFile instanceLock(
        (runDir.isEmpty() ? QDir::tempPath() : runDir) + "/zuuned"
        + qEnvironmentVariable("ZUUNED_LOCK_SUFFIX") + ".lock");
    if (!instanceLock.tryLock(0)) {
        fprintf(stderr, "[zuuned] another instance is already running — "
                        "this one is bowing out\n");
        return 0;
    }

    QQmlApplicationEngine engine;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [] {
        ArtworkHttp::beginShutdown();
    });
    // Drain while both the QML-owned services and QApplication still exist.
    // A watcher disappearing does not cancel its QtConcurrent callable; a
    // provider worker must never construct an event loop after app teardown.
    const auto finishWorkers = qScopeGuard([] {
        ArtworkHttp::beginShutdown();
        QThreadPool::globalInstance()->waitForDone();
    });
    engine.addImageProvider(QStringLiteral("grungemask"), new GrungeMaskProvider);
    // Surface QML load errors on stderr. Without this, a bad import or
    // attached-type reference fails objectCreationFailed silently — the
    // app just exits 1 with no clue which component broke.
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     &app, [](const QList<QQmlError> &ws) {
                         for (const auto &w : ws)
                             fprintf(stderr, "[qml-warn] %s\n",
                                     qUtf8Printable(w.toString()));
                     });
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Zuuned", "Main");

    // Wire the sync engine to the device worker (engine orchestrates on
    // the UI thread; sends run on the device thread).
    auto *device = engine.singletonInstance<DeviceService *>("Zuuned", "DeviceService");
    auto *sync = engine.singletonInstance<SyncEngine *>("Zuuned", "SyncEngine");
    if (device && sync) {
        DeviceWorker *worker = device->worker();
        sync->attachDevice(device, worker);
        QObject::connect(sync, &SyncEngine::workerInspectPodcasts,
                         worker, &DeviceWorker::doInspectPodcasts, Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::podcastsInspected,
                         sync, &SyncEngine::onPodcastsInspected);
        QObject::connect(sync, &SyncEngine::workerSendPodcast,
                         worker, &DeviceWorker::doSendPodcast, Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::podcastSendDone,
                         sync, &SyncEngine::onPodcastSendDone);
        QObject::connect(sync, &SyncEngine::workerInspectAudiobooks,
                         worker, &DeviceWorker::doInspectAudiobooks, Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::audiobooksInspected,
                         sync, &SyncEngine::onAudiobooksInspected);
        QObject::connect(sync, &SyncEngine::workerSendAudiobook,
                         worker, &DeviceWorker::doSendAudiobook, Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::audiobookSendDone,
                         sync, &SyncEngine::onAudiobookSendDone);
        QObject::connect(sync, &SyncEngine::inspectPodcastCleanup,
                         worker, &DeviceWorker::doInspectPodcastCleanup, Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::podcastCleanupInspected,
                         sync, &SyncEngine::onPodcastCleanupInspected);
        QObject::connect(sync, &SyncEngine::purgePlayedPodcasts,
                         worker, &DeviceWorker::doPurgePlayedPodcasts, Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::podcastCleanupProgress,
                         sync, &SyncEngine::onPodcastCleanupProgress);
        QObject::connect(worker, &DeviceWorker::playedPodcastsPurged,
                         sync, &SyncEngine::onPlayedPodcastsPurged);
        QObject::connect(sync, &SyncEngine::workerSendTrack,
                         worker, &DeviceWorker::doSendTrack, Qt::QueuedConnection);
        QObject::connect(sync, &SyncEngine::workerForgeAlbums,
                         worker, &DeviceWorker::doForgeAlbums, Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::sendProgress, sync, &SyncEngine::onSendProgress);
        QObject::connect(worker, &DeviceWorker::sendDone, sync, &SyncEngine::onSendDone);
        QObject::connect(worker, &DeviceWorker::forgeProgress, sync, &SyncEngine::onForgeProgress);
        QObject::connect(worker, &DeviceWorker::forgeDone, sync, &SyncEngine::onForgeDone);
        // Phase 6: video sends (after the album forge)
        QObject::connect(sync, &SyncEngine::workerSendVideo,
                         worker, &DeviceWorker::doSendVideo, Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::sendVideoDone,
                         sync, &SyncEngine::onVideoSendDone);
        // Phase 8: photo sends (between forge and video phases)
        QObject::connect(sync, &SyncEngine::workerSendPhoto,
                         worker, &DeviceWorker::doSendPhoto, Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::sendPhotoDone,
                         sync, &SyncEngine::onPhotoSendDone);
        // UX-2: playlist forge phase (after videos, before finalize)
        QObject::connect(sync, &SyncEngine::workerForgePlaylist,
                         worker, &DeviceWorker::doForgePlaylist,
                         Qt::QueuedConnection);
        QObject::connect(worker, &DeviceWorker::forgePlaylistDone,
                         sync, &SyncEngine::onPlaylistForgeDone);
        // 0x922A device-side sync display (queued ahead of each send)
        QObject::connect(sync, &SyncEngine::workerSyncNotify,
                         worker, &DeviceWorker::doSyncNotify, Qt::QueuedConnection);
        // Eject clears the sync queue (mac disconnectDevice parity)
        QObject::connect(device, &DeviceService::ejectStarted,
                         sync, &SyncEngine::clearQueue);
        // Mid-sync unplug aborts cleanly instead of hanging the pipeline
        QObject::connect(device, &DeviceService::stateChanged, sync, [device, sync] {
            if (!device->connected() && sync->isSyncing())
                sync->cancelSync();
        });
    }

    // Wire the player's 5s position-save into the library DB.
    auto *player = engine.singletonInstance<PlayerService *>("Zuuned", "PlayerService");
    auto *library = engine.singletonInstance<LibraryService *>("Zuuned", "LibraryService");
    if (sync) sync->attachLibrary(library);
    // A download needs somewhere to write and the library is what knows: its
    // default watch folder for the kind, else the first folder of that kind,
    // else the ~/…/Zuuned Imports it registers on the way.
    if (device) device->setLibraryService(library);
    auto *spoken = engine.singletonInstance<SpokenMediaService *>("Zuuned", "SpokenMediaService");
    if (spoken) spoken->attachPlayer(player);
    if (spoken && device && sync) {
        sync->attachPodcastCleanup([spoken] { return spoken->podcastCleanupCandidates(); });
        QObject::connect(spoken, &SpokenMediaService::changed, sync, &SyncEngine::refreshPodcastCleanupPreview, Qt::QueuedConnection);
        auto *settings = engine.singletonInstance<AppSettings *>("Zuuned", "AppSettings");
        if (settings) QObject::connect(settings, &AppSettings::changed, sync, &SyncEngine::refreshPodcastCleanupPreview, Qt::QueuedConnection);
        QTimer::singleShot(0, sync, &SyncEngine::refreshPodcastCleanupPreview);
        QObject::connect(sync, &SyncEngine::recordDevicePlayed, spoken, &SpokenMediaService::recordDevicePlayed);
        QObject::connect(spoken, &SpokenMediaService::devicePlayedRecorded, sync, &SyncEngine::onDevicePlayedRecorded);
        spoken->attachDeviceQueue([device, sync] { return device->connected() && !sync->isSyncing(); });
        QObject::connect(spoken, &SpokenMediaService::podcastTransferReady, sync, &SyncEngine::addPodcasts);
        QObject::connect(spoken, &SpokenMediaService::audiobookTransferReady, sync, &SyncEngine::addAudiobooks);
        QObject::connect(sync, &SyncEngine::prepareSpokenTransfer, spoken, &SpokenMediaService::prepareTransfer);
        QObject::connect(sync, &SyncEngine::cancelSpokenTransfer, spoken, &SpokenMediaService::cancelTransfer);
        QObject::connect(spoken, &SpokenMediaService::transferPrepared, sync, &SyncEngine::onSpokenPrepared);
        QObject::connect(sync, &SyncEngine::podcastQueueNotice, spoken, &SpokenMediaService::notification);
        if (settings) {
            auto *planner = new PodcastQueuePlanner(spoken, {
                [device, sync, settings] {
                    PodcastQueuePlanner::Context c;
                    c.connected = device->connected(); c.serial = device->deviceSerial();
                    c.generation = device->spokenGeneration(); c.syncing = sync->isSyncing();
                    c.queueMismatch = sync->queueDeviceMismatch();
                    c.inventoryReady = device->podcastInventoryReady();
                    c.inventoryLoading = device->spokenMediaLoading();
                    c.inventoryError = device->spokenMediaError().isEmpty()
                        ? device->podcastStatusWarning() : device->spokenMediaError();
                    c.enabled = settings->podcastAutoQueue(); c.count = settings->podcastAutoQueueCount();
                    return c;
                },
                [device](const QString &feed, const QString &source) { return device->podcastDeviceStatus(feed, source); },
                [device] { device->ensureSpokenMedia(); }
            }, spoken);
            QObject::connect(device, &DeviceService::stateChanged, planner, &PodcastQueuePlanner::update);
            QObject::connect(device, &DeviceService::spokenMediaChanged, planner, &PodcastQueuePlanner::update);
            QObject::connect(sync, &SyncEngine::syncStateChanged, planner, &PodcastQueuePlanner::update);
            QObject::connect(sync, &SyncEngine::queueDeviceChanged, planner, &PodcastQueuePlanner::update);
            QObject::connect(sync, &SyncEngine::queueClearedByUser, planner, &PodcastQueuePlanner::dismissPending);
            QObject::connect(settings, &AppSettings::changed, planner, &PodcastQueuePlanner::update);
        }
    }

    // Device music extracts are invisible to the local library until its
    // import root is registered and scanned. Prime portrait matching from the
    // same completed batch so an imported album's artist arrives with it.
    if (device && library)
        QObject::connect(device, &DeviceService::musicImportsReady,
                         library, [library](const QStringList &artists) {
            // Idempotent, and it no longer hardcodes a folder: pulls land in
            // whatever root the user chose, and that root is already scanned.
            library->ensureImportRoot(QStringLiteral("music"));
            for (const QString &artist : artists)
                library->requestArtistImage(artist);
        });
    if (device && library)
        QObject::connect(device, &DeviceService::musicImportAlbumArtReady,
                         library, &LibraryService::importDeviceAlbumArt);

    auto *diagnostics = engine.singletonInstance<DiagnosticsService *>("Zuuned", "DiagnosticsService");
    if (diagnostics) {
        diagnostics->setContextProvider([device, library, sync] {
            QJsonObject state;
            if (device) {
                state.insert(QStringLiteral("zune_present"), device->devicePresent());
                state.insert(QStringLiteral("zune_connected"), device->connected());
                // The audio self-test on Settings -> Device encodes a tone with
                // the same encoder a sync uses, so it has to know when the player
                // is already busy rather than measure beside a running operation.
                state.insert(QStringLiteral("zune_busy"), device->busy());
                state.insert(QStringLiteral("zune_model"), device->modelName());
                state.insert(QStringLiteral("zune_family"), device->deviceFamily());
                state.insert(QStringLiteral("zuuned_rules_applicable"), device->udevRulesApply());
                // Windows has no udev rules, so absence there is not a fault.
                if (device->udevRulesApply())
                    state.insert(QStringLiteral("zuuned_rules_installed"), device->udevRuleOk());
            }
            if (library) {
                state.insert(QStringLiteral("library_tracks"), library->trackCount());
                state.insert(QStringLiteral("library_videos"), library->videoCount());
                state.insert(QStringLiteral("library_photos"), library->photoCount());
            }
            if (sync) state.insert(QStringLiteral("transfer_running"), sync->isSyncing());
            return state;
        });
    }

    // Test harness hook (scale passes, CI): kick a full scan at launch.
    if (library && qEnvironmentVariableIsSet("ZUUNED_RESCAN_ON_LAUNCH"))
        library->rescan();

#if !defined(Q_OS_WIN)
    // C1: MPRIS2 — media keys, sound applets, now-playing widgets.
    if (player && library)
        new MprisService(player,
            [library](const QString &artist, const QString &album) {
                return library->artPaths()
                    .value(LibraryService::artKey(artist, album)).toString();
            }, player);
#endif
    if (player && library)
        player->setSavePositionHook([library](qint64 id, int ms) {
            library->savePosition(id, ms);
        });

    // Same for video: position + the 90% watched rule.
    auto *videoPlayer =
        engine.singletonInstance<VideoPlayerService *>("Zuuned", "VideoPlayerService");
    if (videoPlayer && library)
        videoPlayer->setSavePositionHook(
            [library](qint64 id, int posMs, int durMs) {
                library->saveVideoPosition(id, posMs, durMs);
            });

    // Music ducks while video plays (mac auto-pause/resume handoff).
    if (videoPlayer && player)
        QObject::connect(videoPlayer, &VideoPlayerService::stateChanged,
                         player, [videoPlayer, player] {
                             if (videoPlayer->playing() && player->playing())
                                 player->playPause();
                         });

    return app.exec();
}
