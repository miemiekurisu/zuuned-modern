// CLI gate for the WPD backend: what Windows hands us about a Zune it already
// drives, and what that player will actually accept from a client that is not
// Explorer.
//
// Why a CLI at all: every claim in this file has to be checkable from a script
// and re-checkable after a rebuild, and none of it is testable from a window.
// It is also the instrument that turned two reasonable-sounding guesses into
// measurements - whether libusb can claim a WPD-bound Zune, and what the object
// id of a file sent over WPD really is.
//
// Exit codes: 0 the asked-for thing happened, 1 it did not, 2 the tool could not
// be run at all (no WPD on this Windows, bad arguments).
#if defined(_WIN32)
/* The console renders the active ANSI code page, so a Chinese album name printed
 * below would arrive as '?'.  Declared rather than pulling compat/win through the
 * vendor headers: this tool uses nothing else from it. */
extern "C" void zuuned_win_console_utf8(void);
#endif

#include "DevicePresence.h"
#include "wpd/WpdBackend.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPair>
#include <QString>
#include <QTextStream>
#include <QVector>

#include <libusb.h>

#include <cstdio>

namespace {

QTextStream &out() {
    static QTextStream stream(stdout);
    return stream;
}

void report(const QString &line) {
    out() << line << Qt::endl;
}

void progress(double fraction, void *) {
    const int percent = int(fraction * 100.0);
    static int last = -1;
    if (percent != last) {
        last = percent;
        fprintf(stderr, "\r[wpd] %d%%", percent);
    }
    if (fraction >= 1.0) {
        last = -1;
        fprintf(stderr, "\n");
    }
}

// What the summary of this file promises: the exact libusb answer for a player
// Windows already owns.  Prediction and measurement are two different things and
// only one of them is evidence.
void probeRawUsb(const QString &pnpId) {
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        report(QStringLiteral("raw_open=fail reason=libusb_init"));
        return;
    }
    const QByteArray upper = pnpId.toUpper().toUtf8();
    const int v = upper.indexOf("VID_");
    const int p = upper.indexOf("PID_");
    const quint16 vid = quint16(upper.mid(v + 4, 4).toLongLong(nullptr, 16));
    const quint16 pid = quint16(upper.mid(p + 4, 4).toLongLong(nullptr, 16));

    libusb_device **devices = nullptr;
    const ssize_t found = libusb_get_device_list(ctx, &devices);
    libusb_device_handle *handle = nullptr;
    libusb_device *match = nullptr;
    quint8 bus = 0;
    quint8 address = 0;
    for (ssize_t i = 0; found >= 0 && i < found; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(devices[i], &desc) != 0)
            continue;
        if (desc.idVendor == vid && desc.idProduct == pid) {
            match = devices[i];
            bus = libusb_get_bus_number(match);
            address = libusb_get_device_address(match);
            break;
        }
    }
    report(QStringLiteral("raw_enumerated=%1 raw_sighted=%2 bus=%3 address=%4")
               .arg(found >= 0 ? 1 : 0)
               .arg(match ? 1 : 0)
               .arg(match ? int(bus) : -1)
               .arg(match ? int(address) : -1));
    if (match) {
        int rc = libusb_open(match, &handle);
        report(QStringLiteral("raw_open=%1 (%2)").arg(rc == 0 ? "ok" : "fail").arg(rc));
        if (rc == 0) {
            rc = libusb_claim_interface(handle, 0);
            report(QStringLiteral("raw_claim_interface0=%1 (%2)")
                       .arg(rc == 0 ? "ok" : "fail").arg(rc));
            if (rc == 0)
                libusb_release_interface(handle, 0);
            libusb_close(handle);
        }
    }
    if (devices)
        libusb_free_device_list(devices, 1);
    libusb_exit(ctx);
}

int usage(const QString &why) {
    report(QStringLiteral("VERDICT=usage reason=%1").arg(why));
    report("usage: wpdtool enumerate|info|tree|get|put|thumb|resources|rm|mkdir|rawmtp [args]");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("wpdtool"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));
#if defined(_WIN32)
    zuuned_win_console_utf8();
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Talk to a Zune through Windows' own portable-device driver."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("enumerate | info | tree | get | put | mkdir | rm"));
    parser.addPositionalArgument(QStringLiteral("args"),
                                 QStringLiteral("command arguments"), QStringLiteral("[args...]"));
    // No default on purpose: an empty parent means "the device root", which
    // the backend spells for itself.  Hard-coding an id here is how the walk
    // starts at an object the player has never heard of.
    QCommandLineOption parentOption(
        QStringLiteral("parent"),
        QStringLiteral("parent object id (default: device root)"),
        QStringLiteral("id"));
    QCommandLineOption depthOption(QStringLiteral("depth"),
                                   QStringLiteral("how deep tree walks"), QStringLiteral("n"),
                                   QStringLiteral("4"));
    QCommandLineOption artistOption(QStringLiteral("artist"), QStringLiteral("artist"),
                                    QStringLiteral("text"));
    QCommandLineOption albumOption(QStringLiteral("album"), QStringLiteral("album"),
                                   QStringLiteral("text"));
    QCommandLineOption titleOption(QStringLiteral("title"), QStringLiteral("title"),
                                   QStringLiteral("text"));
    QCommandLineOption albumArtistOption(QStringLiteral("album-artist"),
                                         QStringLiteral("album artist"),
                                         QStringLiteral("text"));
    QCommandLineOption nameOption(QStringLiteral("name"),
                                  QStringLiteral("object name to announce, not the file name"),
                                  QStringLiteral("text"));
    QCommandLineOption durationOption(QStringLiteral("duration"),
                                      QStringLiteral("duration in milliseconds"),
                                      QStringLiteral("ms"));
    QCommandLineOption trackOption(QStringLiteral("track"), QStringLiteral("track number"),
                                   QStringLiteral("n"));
    parser.addOption(parentOption);
    parser.addOption(depthOption);
    parser.addOption(artistOption);
    parser.addOption(albumOption);
    parser.addOption(titleOption);
    parser.addOption(albumArtistOption);
    parser.addOption(durationOption);
    parser.addOption(nameOption);
    parser.addOption(trackOption);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty())
        return usage(QStringLiteral("no command"));
    const QString command = args.first().toLower();
    QString error;

    if (command == QStringLiteral("enumerate")) {
        const QStringList ids = Wpd::Session::zunePnpIds(&error);
        if (!error.isEmpty()) {
            report(QStringLiteral("VERDICT=error reason=%1").arg(error));
            return 2;
        }
        report(QStringLiteral("wpd_zunes=%1").arg(ids.size()));
        for (const QString &id : ids)
            report(QStringLiteral("pnp=%1").arg(id));
        report(QStringLiteral("VERDICT=%1").arg(ids.isEmpty() ? QStringLiteral("absent")
                                                              : QStringLiteral("present")));
        return ids.isEmpty() ? 1 : 0;
    }

    if (command == QStringLiteral("rawmtp")) {
        const QStringList ids = Wpd::Session::zunePnpIds(&error);
        if (ids.isEmpty()) {
            report(QStringLiteral("VERDICT=absent reason=%1").arg(error));
            return 1;
        }
        probeRawUsb(ids.first());
        return 0;
    }

    Wpd::Session session;
    if (!session.openFirstZune(&error)) {
        report(QStringLiteral("VERDICT=refused reason=%1").arg(error));
        return 1;
    }

    if (command == QStringLiteral("info")) {
        Wpd::DeviceInfo info;
        if (!session.readDevice(&info, &error)) {
            report(QStringLiteral("VERDICT=fail reason=%1").arg(error));
            return 1;
        }
        report(QStringLiteral("name=%1").arg(info.friendlyName));
        report(QStringLiteral("model=%1").arg(info.model));
        report(QStringLiteral("manufacturer=%1").arg(info.manufacturer));
        report(QStringLiteral("firmware=%1").arg(info.firmware));
        report(QStringLiteral("battery=%1 power=%2").arg(info.battery).arg(info.powerSource));
        report(QStringLiteral("capacity_bytes=%1 free_bytes=%2 storages=%3")
                   .arg(info.capacityBytes)
                   .arg(info.freeBytes)
                   .arg(info.storageIds.size()));
        for (const QString &id : std::as_const(info.storageIds))
            report(QStringLiteral("storage=%1").arg(id));
        report(QStringLiteral("VERDICT=ok"));
        return 0;
    }

    if (command == QStringLiteral("tree")) {
        const int maxDepth = parser.value(depthOption).toInt();
        QElapsedTimer clock;
        clock.start();
        int audio = 0;
        int folders = 0;
        QVector<Wpd::ObjectInfo> stack;
        QVector<QPair<QString, int>> frontier{{parser.value(parentOption), 0}};
        while (!frontier.isEmpty()) {
            const auto node = frontier.takeFirst();
            QVector<Wpd::ObjectInfo> children;
            if (!session.children(node.first, &children, &error)) {
                report(QStringLiteral("skip=%1 reason=%2").arg(node.first, error));
                continue;
            }
            for (const Wpd::ObjectInfo &object : std::as_const(children)) {
                if (object.kind == Wpd::KindAudio)
                    ++audio;
                if (object.kind == Wpd::KindFolder)
                    ++folders;
                // What the player filed the object under, on the same line:
                // whether an upload's media properties landed at all is the
                // question this tool exists to answer.
                QString filed;
                if (!object.artist.isEmpty())
                    filed += QStringLiteral(" artist=%1").arg(object.artist);
                if (!object.album.isEmpty())
                    filed += QStringLiteral(" album=%1").arg(object.album);
                if (object.trackNumber)
                    filed += QStringLiteral(" track=%1").arg(object.trackNumber);
                if (!object.title.isEmpty() && object.title != object.name)
                    filed += QStringLiteral(" title=%1").arg(object.title);
                if (!object.albumArtist.isEmpty() && object.albumArtist != object.artist)
                    filed += QStringLiteral(" albumArtist=%1").arg(object.albumArtist);
                if (!object.genre.isEmpty())
                    filed += QStringLiteral(" genre=%1").arg(object.genre);
                report(QStringLiteral("%1 d=%2 id=%3 size=%4 dur=%5%6 %7")
                           .arg(Wpd::Session::kindName(object.kind))
                           .arg(node.second + 1)
                           .arg(object.id)
                           .arg(object.size)
                           .arg(object.durationMs)
                           .arg(filed)
                           .arg(object.name));
                if (object.kind == Wpd::KindFolder || object.kind == Wpd::KindFunctional) {
                    if (node.second + 1 < maxDepth)
                        frontier.append(qMakePair(object.id, node.second + 1));
                }
            }
        }
        report(QStringLiteral("audio=%1 folders=%2 ms=%3 VERDICT=ok")
                   .arg(audio)
                   .arg(folders)
                   .arg(clock.elapsed()));
        return 0;
    }

    if (command == QStringLiteral("get") && args.size() >= 3) {
        if (!session.download(args[1], args[2], progress, nullptr, &error)) {
            report(QStringLiteral("VERDICT=fail reason=%1").arg(error));
            return 1;
        }
        report(QStringLiteral("bytes=%1").arg(QFileInfo(args[2]).size()));
        report(QStringLiteral("VERDICT=ok"));
        return 0;
    }

    if (command == QStringLiteral("mkdir") && args.size() >= 2) {
        // The name is positional; the parent is an option, so it is not args[2].
        QString id;
        if (!session.makeFolder(parser.value(parentOption), args[1], &id, &error)) {
            report(QStringLiteral("VERDICT=fail reason=%1").arg(error));
            return 1;
        }
        report(QStringLiteral("object=%1").arg(id));
        report(QStringLiteral("VERDICT=ok"));
        return 0;
    }

    if (command == QStringLiteral("resources") && args.size() >= 2) {
        QStringList names;
        if (!session.supportedResourceNames(args[1], &names, &error)) {
            report(QStringLiteral("VERDICT=fail reason=%1").arg(error));
            return 1;
        }
        report(QStringLiteral("object=%1 resources=%2").arg(args[1],
                                                            names.join(QLatin1Char('+'))));
        report(QStringLiteral("VERDICT=%1").arg(names.isEmpty() ? QStringLiteral("none")
                                                                : QStringLiteral("ok")));
        return 0;
    }

    if (command == QStringLiteral("thumb") && args.size() >= 3) {
        if (!session.readThumbnail(args[1], args[2], &error)) {
            report(QStringLiteral("VERDICT=fail reason=%1").arg(error));
            return 1;
        }
        report(QStringLiteral("bytes=%1").arg(QFileInfo(args[2]).size()));
        report(QStringLiteral("VERDICT=ok"));
        return 0;
    }

    if (command == QStringLiteral("put") && args.size() >= 2) {
        Wpd::ObjectInfo meta;
        meta.name = parser.value(nameOption).isEmpty() ? QFileInfo(args[1]).fileName()
                                                       : parser.value(nameOption);
        meta.artist = parser.value(artistOption);
        meta.album = parser.value(albumOption);
        meta.title = parser.value(titleOption).isEmpty() ? meta.name
                                                         : parser.value(titleOption);
        meta.albumArtist = parser.value(albumArtistOption);
        meta.durationMs = parser.value(durationOption).toUInt();
        meta.trackNumber = parser.value(trackOption).toUInt();
        QString id;
        if (!session.upload(parser.value(parentOption), args[1], meta, progress, nullptr, &id,
                            &error)) {
            report(QStringLiteral("VERDICT=fail reason=%1").arg(error));
            return 1;
        }
        report(QStringLiteral("object=%1").arg(id));
        report(QStringLiteral("VERDICT=ok"));
        return 0;
    }

    if (command == QStringLiteral("rm") && args.size() >= 2) {
        QStringList purged;
        if (!session.removeObjects(args.mid(1), &purged, &error)) {
            report(QStringLiteral("VERDICT=fail reason=%1").arg(error));
            return 1;
        }
        for (const QString &id : std::as_const(purged))
            report(QStringLiteral("purged=%1").arg(id));
        report(QStringLiteral("VERDICT=ok purged=%1").arg(purged.size()));
        return 0;
    }

    return usage(QStringLiteral("unknown command '%1'").arg(command));
}
