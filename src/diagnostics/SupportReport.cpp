/* Keep first on Windows: the compat shim declares the POSIX surface this
 * file uses (64-bit struct stat, dirent, poll, ...).  Whichever <sys/stat.h>
 * the compiler parses first decides the struct layout, and the Qt headers
 * include one.  See compat/win/posix_shim.h.  Other platforms build without
 * it.  */
#if defined(_WIN32)
#include "wincompat.h"
#endif

#include "SupportReport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
#include <libusb.h>
#endif
#include <unistd.h>

namespace Zuuned::Diagnostics {
namespace {
QString field(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly)
        ? QString::fromUtf8(file.read(1024)).trimmed() : QString();
}

bool within(const QString &path, const QString &directory) {
    return path == directory || path.startsWith(directory + QLatin1Char('/'));
}
}

QJsonObject systemInformation() {
    QJsonObject info {
        {QStringLiteral("generated_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("kernel"), QSysInfo::kernelType()},
        {QStringLiteral("kernel_version"), QSysInfo::kernelVersion()},
        {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("qt_runtime"), QString::fromLatin1(qVersion())},
        {QStringLiteral("qt_build"), QStringLiteral(QT_VERSION_STR)}
    };
    // Do not collect the full environment, host/user names or machine IDs.
    for (const auto &name : {"XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP"})
        info.insert(QString::fromLatin1(name).toLower(),
                    QString::fromLocal8Bit(qgetenv(name)).left(160));

    QJsonArray graphics;
    const QDir drm(QStringLiteral("/sys/class/drm"));
    const QRegularExpression cardName(QStringLiteral("^card[0-9]+$"));
    for (const QString &card : drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!cardName.match(card).hasMatch()) continue;
        const QString device = drm.filePath(card + QStringLiteral("/device/"));
        graphics.append(QJsonObject {
            {QStringLiteral("vendor_id"), field(device + QStringLiteral("vendor"))},
            {QStringLiteral("device_id"), field(device + QStringLiteral("device"))},
            {QStringLiteral("driver"), QFileInfo(QFileInfo(device + QStringLiteral("driver")).symLinkTarget()).fileName()},
            {QStringLiteral("driver_version"), field(device + QStringLiteral("driver/module/version"))}
        });
    }
    info.insert(QStringLiteral("graphics_devices"), graphics);

    // Enumerated one way per platform below, reported the same way on every one.
    QJsonArray usb;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // No sysfs on Windows.  libusb enumeration is the closest honest
    // equivalent, and descriptors alone are enough: opening a device here
    // would contend with a transfer the app may already own, so this reports
    // identity and location only - never whether the device is claimable.
    // macOS has no sysfs either and reaches the same libusb backend libzune
    // uses, so the same answer comes from the same source.
    libusb_context *context = nullptr;
    if (libusb_init(&context) == 0) {
        libusb_device **found = nullptr;
        const int count = static_cast<int>(libusb_get_device_list(context, &found));
        for (int i = 0; i < count; ++i) {
            libusb_device_descriptor descriptor {};
            if (libusb_get_device_descriptor(found[i], &descriptor) != 0) continue;
            if (descriptor.idVendor != 0x045e) continue;
            if (descriptor.idProduct != 0x0710 && descriptor.idProduct != 0x063e) continue;
            QJsonObject item {{QStringLiteral("vendor_id"), QStringLiteral("045e")},
                              {QStringLiteral("product_id"),
                               QString::number(descriptor.idProduct, 16).rightJustified(4, QLatin1Char('0'))}};
            const int bus = libusb_get_bus_number(found[i]);
            const int address = libusb_get_device_address(found[i]);
            if (bus > 0) item.insert(QStringLiteral("bus_number"), bus);
            if (address > 0) item.insert(QStringLiteral("device_address"), address);
            usb.append(item);
        }
        if (found) libusb_free_device_list(found, 1);
        libusb_exit(context);
    }
#else
    const QDir devices(QStringLiteral("/sys/bus/usb/devices"));
    for (const QString &entry : devices.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString path = devices.filePath(entry) + QLatin1Char('/');
        if (field(path + QStringLiteral("idVendor")) != QStringLiteral("045e")) continue;
        const QString product = field(path + QStringLiteral("idProduct"));
        if (product != QStringLiteral("0710") && product != QStringLiteral("063e")) continue;
        bool busValid = false, deviceValid = false;
        const int bus = field(path + QStringLiteral("busnum")).toInt(&busValid);
        const int device = field(path + QStringLiteral("devnum")).toInt(&deviceValid);
        QJsonObject item {{QStringLiteral("vendor_id"), QStringLiteral("045e")},
                          {QStringLiteral("product_id"), product}};
        if (busValid && deviceValid && bus > 0 && device > 0) {
            const QString node = QStringLiteral("/dev/bus/usb/%1/%2")
                .arg(bus, 3, 10, QLatin1Char('0')).arg(device, 3, 10, QLatin1Char('0'));
            item.insert(QStringLiteral("user_can_read_write"),
                        ::access(QFile::encodeName(node).constData(), R_OK | W_OK) == 0);
        }
        usb.append(item);
    }
#endif
    // Any branch may fill it, and a Windows or macOS report needs it most: the
    // USB identity is the first thing a driver problem is answered with.
    info.insert(QStringLiteral("zune_usb_devices"), usb);
    return info;
}

QByteArray formatReport(const QJsonObject &information, const QByteArray &logs,
                        const Redactor::Context &redaction) {
    const Redactor scrubber(redaction);
    QString report = QStringLiteral(
        "ZUUNED DEBUG REPORT\n"
        "Describe what happened and the steps to reproduce it when sharing this file.\n"
        "Common credentials, home paths and device identifiers are masked.\n"
        "Media names or other personal text may remain; review before sharing.\n"
        "This report contains recent activity, not a crash dump or a full history.\n\n"
        "SYSTEM AND APPLICATION\n");
    report += QString::fromUtf8(QJsonDocument(information).toJson(QJsonDocument::Indented));
    report += QStringLiteral("\nRECENT SESSION LOGS\n");
    report += logs.isEmpty() ? QStringLiteral("No saved session logs are available.\n")
                             : QString::fromUtf8(logs);
    return scrubber.sanitize(report).toUtf8();
}

ReportResult saveReport(const QUrl &destination, const QByteArray &report,
                        const QStringList &protectedDirectories) {
    if (report.size() > 6 * 1024 * 1024)
        return {false, {}, QStringLiteral("The debug report exceeds the size limit.")};
    if (!destination.isLocalFile() || !destination.host().isEmpty())
        return {false, {}, QStringLiteral("Choose a file on this computer.")};
    const QFileInfo target(destination.toLocalFile());
    if (!target.isAbsolute() || target.fileName().isEmpty() || target.isSymLink()
        || (target.exists() && !target.isFile()))
        return {false, {}, QStringLiteral("Choose a regular report file.")};
    const QString parent = target.dir().canonicalPath();
    if (parent.isEmpty())
        return {false, {}, QStringLiteral("That folder is no longer available.")};
    const QString resolved = QDir(parent).filePath(target.fileName());
    for (const QString &directory : protectedDirectories) {
        const QString canonical = QFileInfo(directory).canonicalFilePath();
        if (!canonical.isEmpty() && within(resolved, canonical))
            return {false, {}, QStringLiteral("Save the report outside ZUUNED's working folders.")};
    }
    QSaveFile file(resolved);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return {false, {}, QStringLiteral("Couldn't create the report: ") + file.errorString()};
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(report) != report.size() || !file.commit())
        return {false, {}, QStringLiteral("Couldn't save the report: ") + file.errorString()};
    return {true, QUrl::fromLocalFile(resolved), {}};
}
} // namespace Zuuned::Diagnostics
