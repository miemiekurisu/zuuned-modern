#pragma once

// A Zuuned profile is one folder: library.db, spoken.db, artwork and thumbnail
// caches, session logs, the single-instance lock and the settings file.
//
// On Linux $HOME relocates the whole thing, and that is how every native gate
// runs the application against a throwaway profile.  Windows has no such lever:
// QStandardPaths asks the shell known folders and ignores LOCALAPPDATA/APPDATA
// overrides, so an instrumented launch reads and writes the real profile and
// loses the diagnostic-capture lock to the instance that is already running.
// ZUUNED_PROFILE_DIR names the folder explicitly, which equally gives a Windows
// user a portable profile.
//
// Unset, every call here is exactly what QStandardPaths::writableLocation
// returns, so the Linux line and every existing gate keep their behaviour.  The
// implementation lives in the header on purpose: src/library/LibraryDb.cpp is
// compiled both into the application and by stand-alone gate scripts that link a
// single translation unit, and a separate .cpp would break those compile lines
// for no benefit.
//
// Only folders a Zuuned profile owns are redirected.  The user's own media
// folders, the system temp dir and the home folder stay wherever the platform
// puts them, so a test profile can still be pointed at real media on purpose.

#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QString>

#include <cstdio>

namespace Zuuned {
namespace ProfilePaths {

// The profile folder, or empty when ZUUNED_PROFILE_DIR is unset or unusable.
// A relative value is refused rather than resolved against whatever the current
// directory happens to be, because silently writing next to the build tree is
// worse than a test that fails with a visible reason.
inline QString root() {
    static const QString cached = [] {
        const QString declared = qEnvironmentVariable("ZUUNED_PROFILE_DIR");
        if (declared.isEmpty())
            return QString();
        const QString path = QDir::cleanPath(declared);
        if (!QDir::isAbsolutePath(path)) {
            std::fprintf(stderr, "[profile] ignoring relative ZUUNED_PROFILE_DIR \"%s\"\n",
                         qUtf8Printable(declared));
            return QString();
        }
        if (!QDir().mkpath(path))
            std::fprintf(stderr,
                         "[profile] could not create \"%s\"; profile writes stay there "
                         "and will fail rather than touch the real profile\n",
                         qUtf8Printable(path));
        else
            std::fprintf(stderr, "[profile] every profile file goes to \"%s\"\n", qUtf8Printable(path));
        return path;
    }();
    return cached;
}

// Drop-in for QStandardPaths::writableLocation for the locations a profile owns.
// Like Qt, the returned directory is created when possible.
inline QString writableLocation(QStandardPaths::StandardLocation type) {
    const QString base = root();
    if (base.isEmpty())
        return QStandardPaths::writableLocation(type);
    QString path;
    switch (type) {
    case QStandardPaths::AppDataLocation:
    case QStandardPaths::GenericDataLocation:
    case QStandardPaths::AppConfigLocation:
    case QStandardPaths::GenericConfigLocation:
        path = base;
        break;
    case QStandardPaths::CacheLocation:
    case QStandardPaths::GenericCacheLocation:
        path = base + QStringLiteral("/cache");
        break;
    case QStandardPaths::StateLocation:
    case QStandardPaths::GenericStateLocation:
        path = base + QStringLiteral("/State");
        break;
    case QStandardPaths::RuntimeLocation:
        path = base + QStringLiteral("/run");
        break;
    default:
        return QStandardPaths::writableLocation(type);
    }
    QDir().mkpath(path);
    return path;
}

// The application data and cache folders, named for a process that is *not*
// the application.
//
// QStandardPaths::AppDataLocation and CacheLocation answer for this process,
// so a second executable that sets its own organisation and application names -
// tools/artwork-preview runs as ZuunedPreview / "Artwork Print Lab" - cannot use
// them to ask where Zuuned keeps its files: Qt would name the tool's folder.
// The obvious shortcut is the generic location plus a literal "/Zuuned/Zuuned",
// and on Linux that is exactly the application's folder, because Qt puts
// AppDataLocation under XDG_DATA_HOME and CacheLocation under XDG_CACHE_HOME.
// On Windows it is wrong twice over: the generic data location is
// C:\ProgramData, where Zuuned has never written a file, and the generic cache
// location is the shell's local app-data folder, one level above the
// <LocalAppData>\Zuuned\Zuuned\cache that holds art, artistart, stills and
// vidthumbs.  A tool built that way finds no artwork and no library on Windows
// while working on Linux, which is how it survived.
//
// ZUUNED_PROFILE_DIR wins on both platforms, so an instrumented tool reads the
// same throwaway profile the application wrote - the whole point of the variable.
// With it unset these are read-only answers: neither creates the folder, because
// a tool looking for someone else's library has no business making one.
inline QString applicationDataLocation() {
    const QString base = root();
    if (!base.isEmpty())
        return base;
#ifdef Q_OS_WIN
    // Qt asks the shell known folder here.  This variable is what the shell
    // folder was built from, so the two agree unless someone redirects one and
    // not the other; if it is unset at all, fall back to Qt's own answer for
    // this process rather than invent a path out of nothing.
    const QString roaming = qEnvironmentVariable("APPDATA");
    if (!roaming.isEmpty())
        return QDir::cleanPath(roaming + QStringLiteral("/Zuuned/Zuuned"));
#elif defined(Q_OS_MACOS)
    // Not the XDG line.  Measured on macOS 26 with Qt 6.12: AppDataLocation is
    // ~/Library/Application Support/Zuuned/Zuuned and GenericDataLocation is
    // its parent, so generic plus the two names is the application's own
    // folder here too.  It still has to be built that way rather than asked
    // for as AppDataLocation, for the reason this function exists: a tool that
    // sets its own application name would get the tool's folder back.
    // macOS has no XDG_DATA_HOME equivalent, and ~/Library/Application Support
    // is where the folder actually lives, so nothing is being overridden.
    const QString support = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!support.isEmpty())
        return QDir::cleanPath(support + QStringLiteral("/Zuuned/Zuuned"));
#else
    const QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (!dataHome.isEmpty())
        return QDir::cleanPath(dataHome + QStringLiteral("/Zuuned/Zuuned"));
    return QDir::homePath() + QStringLiteral("/.local/share/Zuuned/Zuuned");
#endif
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

inline QString applicationCacheLocation() {
    const QString base = root();
    if (!base.isEmpty())
        return base + QStringLiteral("/cache");
#ifdef Q_OS_WIN
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (!local.isEmpty())
        return QDir::cleanPath(local + QStringLiteral("/Zuuned/Zuuned/cache"));
#elif defined(Q_OS_MACOS)
    // Same measurement: CacheLocation is ~/Library/Caches/Zuuned/Zuuned and
    // GenericCacheLocation is ~/Library/Caches.  The trailing /cache keeps the
    // folder one level below the application's, which is what art, artistart,
    // stills and vidthumbs expect to sit inside on every platform.
    const QString caches = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (!caches.isEmpty())
        return QDir::cleanPath(caches + QStringLiteral("/Zuuned/Zuuned/cache"));
#else
    const QString cacheHome = qEnvironmentVariable("XDG_CACHE_HOME");
    if (!cacheHome.isEmpty())
        return QDir::cleanPath(cacheHome + QStringLiteral("/Zuuned/Zuuned"));
    return QDir::homePath() + QStringLiteral("/.cache/Zuuned/Zuuned");
#endif
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

// Route default-constructed QSettings into root() as well, so the settings file
// is part of the profile folder instead of %APPDATA%.  Qt appends the
// organisation and application names, which puts the file at
// <root>/Zuuned/Zuuned.ini.  Call before the first QSettings object exists.
inline void applySettingsPath() {
    const QString base = root();
    if (base.isEmpty())
        return;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, base);
    const QString file = QSettings().fileName();
    std::fprintf(stderr, "[profile] settings live at \"%s\"\n", qUtf8Printable(file));
    if (!file.startsWith(base))
        std::fprintf(stderr,
                     "[profile] ZUUNED_PROFILE_DIR did not capture the settings file; "
                     "profile isolation is incomplete\n");
}

}  // namespace ProfilePaths
}  // namespace Zuuned
