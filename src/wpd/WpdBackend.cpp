// WPD backend: a Zune that Windows already drives, reached through Windows.
//
// The whole file is COM with no libusb and no libzune in it.  Every call that
// can fail returns a real reason rather than a bare bool, because the difference
// between "the player refused", "the player went away" and "this build has no
// WPD" is the difference between telling a user to check their cable and telling
// them to check their driver.
//
// Two measurements this code is built around, both taken on the machine this port
// runs on against a Zune at USB\VID_045E&PID_0710 (Class=WPD, problem 0):
// Windows' own driver lets a client create a folder, write a file into it, read
// that file back byte for byte and delete it, with no MTPZ credentials of any
// kind; and libusb does enumerate that player (23 devices, it is one of them) but
// cannot open it, because the WPD driver owns the interface.  So the choice is
// not "libusb sees it or not", it is "who owns the interface" - and when the
// answer is Windows, this is the way in.

#include "WpdBackend.h"
#include "DevicePresence.h"

#include <windows.h>
#include <PortableDevice.h>
#include <PortableDeviceApi.h>

#include <wrl/client.h>

#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QUuid>
#include <QSet>
#include <QVector>
#include <propvarutil.h>

#include <algorithm>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

using Wpd::ObjectInfo;

// Defined further down beside the other property readers; settleCreatedId() needs
// the name before it, and one declaration is cheaper than ordering the file around
// it.  Declared here rather than at global scope so it keeps this file's linkage.
bool readObjectInfo(IPortableDeviceProperties *properties, const QString &id,
                    ObjectInfo *out);

// WPD says S_FALSE as readily as S_OK, and S_FALSE with no objects is not an
// error, so almost every check here is FAILED() rather than "!= S_OK".
bool failed(HRESULT result) { return FAILED(result); }

// The WPD error category for "the device said no", with nothing behind it.  A
// portable device answers with this whenever it dislikes a request and has
// nothing more specific to say, so it is worth naming where it is handled.
constexpr HRESULT kDeviceRefused = static_cast<HRESULT>(0x80040000L);

// One line a user can paste into a bug report.  FormatMessage gives the Windows
// text; the number survives translation, which is why it comes first.
QString explain(HRESULT result) {
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString text;
    if (length)
        text = QString::fromWCharArray(buffer, int(length)).trimmed();
    if (buffer)
        LocalFree(buffer);
    if (result == kDeviceRefused)
        // FormatMessage answers this code with an OLEVERB complaint that has
        // nothing to do with a portable device, so a report quoting it sends
        // whoever reads it looking at the wrong API.  The number still travels.
        return QStringLiteral("Windows error 0x80040000 (the player refused the request and "
                              "gave no reason)");
    const QString code = QStringLiteral("Windows error 0x%1")
                             .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
    return text.isEmpty() ? code : QStringLiteral("%1 (%2)").arg(code, text);
}

// WPD allocates every string it hands back.  Taking ownership in one place is
// what keeps a 400-line COM file leak-free.
QString adopted(LPWSTR raw) {
    if (!raw)
        return QString();
    const QString text = QString::fromWCharArray(raw);
    CoTaskMemFree(raw);
    return text;
}

// WPD names are UTF-16, and Qt 6 hands out its UTF-16 through a pointer that is
// only good while the QString is alive.  Every call here copies into a wstring so
// no pointer can outlive the name it came from, and because wchar_t is UTF-16 on
// Windows an album named in Han characters survives the trip intact.
std::wstring toWide(const QString &text) { return text.toStdWString(); }

// The object id that owns every storage object reads as "DEVICE", not
// "WPD_DEVICE".  Every property in the same header is spelled WPD_DEVICE_something,
// which is how the root gets mis-typed.
QString deviceRoot() { return QString::fromWCharArray(WPD_DEVICE_OBJECT_ID); }

ComPtr<IPortableDeviceValues> newValues() {
    ComPtr<IPortableDeviceValues> values;
    CoCreateInstance(__uuidof(PortableDeviceValues), nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&values));
    return values;
}

ComPtr<IPortableDeviceKeyCollection> newKeys() {
    ComPtr<IPortableDeviceKeyCollection> keys;
    CoCreateInstance(__uuidof(PortableDeviceKeyCollection), nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&keys));
    return keys;
}

QString textAt(IPortableDeviceValues *values, REFPROPERTYKEY key) {
    LPWSTR raw = nullptr;
    if (!values || FAILED(values->GetStringValue(key, &raw)))
        return QString();
    return adopted(raw);
}

quint64 numberAt(IPortableDeviceValues *values, REFPROPERTYKEY key, bool *present = nullptr) {
    ULONGLONG value = 0;
    const bool ok = values && SUCCEEDED(values->GetUnsignedLargeIntegerValue(key, &value));
    if (present)
        *present = ok;
    return ok ? value : 0;
}

quint32 smallAt(IPortableDeviceValues *values, REFPROPERTYKEY key, bool *present = nullptr) {
    DWORD value = 0;
    const bool ok = values && SUCCEEDED(values->GetUnsignedIntegerValue(key, &value));
    if (present)
        *present = ok;
    return ok ? value : 0;
}

bool flagAt(IPortableDeviceValues *values, REFPROPERTYKEY key, bool *present = nullptr) {
    BOOL value = FALSE;
    const bool ok = values && SUCCEEDED(values->GetBoolValue(key, &value));
    if (present)
        *present = ok;
    return ok && value;
}

ComPtr<IPortableDevicePropVariantCollection> newPropVariantCollection() {
    ComPtr<IPortableDevicePropVariantCollection> collection;
    CoCreateInstance(__uuidof(PortableDevicePropVariantCollection), nullptr,
                     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&collection));
    return collection;
}

// Object ids under one parent, with no properties read.  Cheap enough to call
// before and after a create, which is how an upload finds out what it made.
QSet<QString> childIds(IPortableDeviceContent *content, const QString &parentId) {
    QSet<QString> ids;
    if (!content)
        return ids;
    const QString root = parentId.isEmpty() ? deviceRoot() : parentId;
    const std::wstring wide = toWide(root);
    ComPtr<IEnumPortableDeviceObjectIDs> enumerator;
    if (failed(content->EnumObjects(0, wide.c_str(), nullptr, &enumerator)))
        return ids;
    for (;;) {
        LPWSTR rawId = nullptr;
        ULONG fetched = 0;
        const HRESULT hr = enumerator->Next(1, &rawId, &fetched);
        const QString id = adopted(rawId);
        if (fetched == 0 || id.isEmpty())
            break;
        ids.insert(id);
        if (failed(hr))
            break;
    }
    return ids;
}

// Is there anything at all to tell the player about this file's identity?
bool hasMediaProperties(const ObjectInfo &meta) {
    return !meta.title.isEmpty() || !meta.artist.isEmpty() || !meta.album.isEmpty()
        || !meta.albumArtist.isEmpty() || !meta.genre.isEmpty() || meta.durationMs != 0
        || meta.trackNumber != 0;
}

// The media half of a transfer, in one place because it is asked twice: once with
// the create, and once afterwards on the object that exists (see upload()).
// Which of these a player keeps is a fact about the player.
void fillMediaProperties(IPortableDeviceValues *values, const ObjectInfo &meta) {
    if (!values)
        return;
    const std::wstring title = toWide(meta.title);
    const std::wstring artist = toWide(meta.artist);
    const std::wstring album = toWide(meta.album);
    const std::wstring albumArtist = toWide(meta.albumArtist);
    const std::wstring genre = toWide(meta.genre);
    if (!meta.title.isEmpty())
        values->SetStringValue(WPD_MEDIA_TITLE, title.c_str());
    if (!meta.artist.isEmpty())
        values->SetStringValue(WPD_MEDIA_ARTIST, artist.c_str());
    if (!meta.album.isEmpty())
        values->SetStringValue(WPD_MUSIC_ALBUM, album.c_str());
    if (!meta.albumArtist.isEmpty())
        values->SetStringValue(WPD_MEDIA_ALBUM_ARTIST, albumArtist.c_str());
    if (!meta.genre.isEmpty())
        values->SetStringValue(WPD_MEDIA_GENRE, genre.c_str());
    // WPD_MEDIA_DURATION is VT_UI8 in milliseconds, while MTP itself carries
    // seconds and the SDK leaves the conversion to the driver.
    if (meta.durationMs)
        values->SetUnsignedLargeIntegerValue(WPD_MEDIA_DURATION, ULONGLONG(meta.durationMs));
    if (meta.trackNumber)
        values->SetUnsignedIntegerValue(WPD_MUSIC_TRACK, meta.trackNumber);
}

// Settle which object a create produced.  `cookie` is what the provider chose to
// call the transfer and `name` is the object name we asked for; either can be
// empty, and neither is a promise, so a new id is only reported when the player
// itself agrees that the object exists and is spelled what we sent.
QString settleCreatedId(IPortableDeviceContent *content, const QString &parentId,
                        const QSet<QString> &before, const QString &cookie,
                        const QString &name) {
    QSet<QString> fresh = childIds(content, parentId);
    QStringList added;
    for (const QString &id : std::as_const(fresh))
        if (!before.contains(id))
            added.append(id);

    ComPtr<IPortableDeviceProperties> properties;
    if (SUCCEEDED(content->Properties(&properties))) {
        // Prefer a real readback over the diff: a folder that gained something
        // else at the same time is a thing that happens to a player being written
        // to from Explorer.
        if (!name.isEmpty()) {
            for (const QString &id : std::as_const(added)) {
                ObjectInfo probe;
                if (readObjectInfo(properties.Get(), id, &probe) && probe.name == name)
                    return id;
            }
        }
        if (added.size() == 1)
            return added.first();
    }
    if (!cookie.isEmpty()) {
        ObjectInfo probe;
        if (readObjectInfo(properties.Get(), cookie, &probe))
            return cookie;
    }
    return QString();
}

QString guidToText(const GUID &guid) {
    // StringFromGUID2 writes into a buffer the caller owns; it never allocates,
    // so there is nothing to free here and nothing to adopt.
    OLECHAR buffer[80]{};
    if (FAILED(StringFromGUID2(guid, buffer, INT(ARRAYSIZE(buffer)))) || !buffer[0])
        return QString();
    return QString::fromWCharArray(buffer);
}

// The WPD device id for a Zune is the PnP id, which carries VID/PID in either
// separator style depending on which enumerator produced the string.
bool isZunePnpId(const QString &pnpId) {
    const QString upper = pnpId.toUpper();
    const int v = upper.indexOf(QStringLiteral("VID_"));
    const int p = upper.indexOf(QStringLiteral("PID_"));
    if (v < 0 || p < 0)
        return false;
    bool vidOk = false;
    bool pidOk = false;
    const quint64 vid = upper.mid(v + 4, 4).toULongLong(&vidOk, 16);
    const quint64 pid = upper.mid(p + 4, 4).toULongLong(&pidOk, 16);
    return vidOk && pidOk && Zuuned::isZuneUsbId(quint16(vid), quint16(pid));
}

void setReason(QString *error, const QString &text) {
    if (error)
        *error = text;
}

int kindFor(IPortableDeviceValues *values) {
    GUID type{};
    if (values && SUCCEEDED(values->GetGuidValue(WPD_OBJECT_CONTENT_TYPE, &type))) {
        if (IsEqualGUID(type, WPD_CONTENT_TYPE_FOLDER))
            return Wpd::KindFolder;
        if (IsEqualGUID(type, WPD_CONTENT_TYPE_AUDIO))
            return Wpd::KindAudio;
        if (IsEqualGUID(type, WPD_CONTENT_TYPE_IMAGE))
            return Wpd::KindImage;
        if (IsEqualGUID(type, WPD_CONTENT_TYPE_VIDEO))
            return Wpd::KindVideo;
        if (IsEqualGUID(type, WPD_CONTENT_TYPE_FUNCTIONAL_OBJECT))
            return Wpd::KindFunctional;
    }
    // Some players leave the content type off a folder object and only name the
    // format.  A folder is the one misclassification that would actually lose
    // tracks, so it is the one worth a second question.
    GUID format{};
    if (values && SUCCEEDED(values->GetGuidValue(WPD_OBJECT_FORMAT, &format))
        && IsEqualGUID(format, WPD_OBJECT_FORMAT_UNSPECIFIED))
        return Wpd::KindUnknown;
    return Wpd::KindUnknown;
}


// One object's worth of properties, kept apart from Session so a whole directory
// read pays for Properties() once instead of once per track.
bool readObjectInfo(IPortableDeviceProperties *properties, const QString &id,
                    ObjectInfo *out) {
    if (!properties)
        return false;
    const ComPtr<IPortableDeviceKeyCollection> keys = newKeys();
    if (!keys)
        return false;
    keys->Add(WPD_OBJECT_NAME);
    keys->Add(WPD_OBJECT_PARENT_ID);
    keys->Add(WPD_OBJECT_SIZE);
    keys->Add(WPD_OBJECT_CONTENT_TYPE);
    keys->Add(WPD_OBJECT_FORMAT);
    keys->Add(WPD_OBJECT_CAN_DELETE);
    keys->Add(WPD_MEDIA_TITLE);
    keys->Add(WPD_MEDIA_ARTIST);
    keys->Add(WPD_MUSIC_ALBUM);
    keys->Add(WPD_MEDIA_ALBUM_ARTIST);
    keys->Add(WPD_MEDIA_GENRE);
    keys->Add(WPD_MEDIA_DURATION);
    keys->Add(WPD_MUSIC_TRACK);
    ComPtr<IPortableDeviceValues> values;
    const std::wstring wide = toWide(id);
    if (failed(properties->GetValues(wide.c_str(), keys.Get(), &values)))
        return false;
    out->id = id;
    out->name = textAt(values.Get(), WPD_OBJECT_NAME);
    out->parentId = textAt(values.Get(), WPD_OBJECT_PARENT_ID);
    out->size = numberAt(values.Get(), WPD_OBJECT_SIZE);
    out->kind = kindFor(values.Get());
    GUID format{};
    if (values && SUCCEEDED(values->GetGuidValue(WPD_OBJECT_FORMAT, &format)))
        out->format = guidToText(format);
    out->canDelete = flagAt(values.Get(), WPD_OBJECT_CAN_DELETE);
    out->title = textAt(values.Get(), WPD_MEDIA_TITLE);
    out->artist = textAt(values.Get(), WPD_MEDIA_ARTIST);
    out->album = textAt(values.Get(), WPD_MUSIC_ALBUM);
    out->albumArtist = textAt(values.Get(), WPD_MEDIA_ALBUM_ARTIST);
    out->genre = textAt(values.Get(), WPD_MEDIA_GENRE);
    // The contract says WPD_MEDIA_DURATION is milliseconds.  MTP itself carries
    // seconds, so a firmware that forwards the raw value answers 245 for a
    // four-minute track; anything under a second is that firmware, not that short
    // a song.
    // The header says VT_UI8; what this player answers for an object Windows
    // itself wrote is a UI4.  Ask both and keep whichever the provider gave.
    quint32 duration = quint32(numberAt(values.Get(), WPD_MEDIA_DURATION));
    if (duration == 0)
        duration = smallAt(values.Get(), WPD_MEDIA_DURATION);
    if (duration > 0 && duration < 1000)
        duration *= 1000;
    out->durationMs = duration;
    out->trackNumber = smallAt(values.Get(), WPD_MUSIC_TRACK);
    return true;
}

bool sameKey(const PROPERTYKEY &a, const PROPERTYKEY &b) {
    return a.pid == b.pid && IsEqualGUID(a.fmtid, b.fmtid);
}

// The resource keys a player may name for an object, spelled the way the WPD
// documentation spells them, so one log line means the same to a reader as it
// does to the SDK.
// The name a player shows for a file it has never seen.  Measured on the Zune at
// PID 0710: WPD_OBJECT_NAME is the text the player's own browser prints, an object
// created with NAME "Embedded Tone QA3" reads exactly that on the device, and the
// tracks the Zune software synced itself carry a bare title with no extension -
// while the file name belongs to WPD_OBJECT_ORIGINAL_FILE_NAME.  So a title is the
// name when there is one, and an extension that crept into the title goes back
// where the file name already keeps it.
QString displayObjectName(const QString &title, const QString &fileName) {
    static const char *const audio[] = {"mp3", "wma", "flac", "wav", "m4a", "aac"};
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty())
        return fileName;
    const int dot = trimmed.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        const QString suffix = trimmed.mid(dot + 1).toLower();
        for (const char *candidate : audio) {
            if (suffix != QLatin1String(candidate))
                continue;
            const QString stem = trimmed.left(dot).trimmed();
            if (!stem.isEmpty())
                return stem;
            break;   // a title that is nothing but an extension stays as it is
        }
    }
    return trimmed;
}

QString resourceName(const PROPERTYKEY &key) {
    const PROPERTYKEY known[] = {WPD_RESOURCE_DEFAULT, WPD_RESOURCE_THUMBNAIL,
                                 WPD_RESOURCE_ALBUM_ART,  WPD_RESOURCE_ICON,
                                 WPD_RESOURCE_GENERIC,    WPD_RESOURCE_AUDIO_CLIP,
                                 WPD_RESOURCE_VIDEO_CLIP, WPD_RESOURCE_BRANDING_ART,
                                 WPD_RESOURCE_CONTACT_PHOTO};
    const char *names[] = {"DEFAULT",     "THUMBNAIL",   "ALBUM_ART",
                           "ICON",        "GENERIC",     "AUDIO_CLIP",
                           "VIDEO_CLIP",  "BRANDING_ART", "CONTACT_PHOTO"};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (sameKey(key, known[i]))
            return QString::fromLatin1(names[i]);
    }
    return QStringLiteral("unknown(%1:%2)").arg(QUuid(key.fmtid).toString()).arg(key.pid);
}
} // namespace
namespace Wpd {

Session::Session() = default;

Session::~Session() {
    close();
    releaseCom();
}

bool Session::ensureCom() {
    if (m_ownsCom)
        return true;
    // Multithreaded, not apartment-threaded: the thread that owns a Session is a
    // QThread with no message pump, and an STA with no pump deadlocks the first
    // time WPD marshals a callback back into it.  This is the same choice the
    // Windows "enumerate from a service" guidance makes for the same reason.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        m_ownsCom = true;   // S_FALSE means COM was already up on this thread; leave it alone
        return true;
    }
    return hr == RPC_E_CHANGED_MODE;   // someone else chose the model; COM is still usable
}

void Session::releaseCom() {
    if (m_ownsCom) {
        CoUninitialize();
        m_ownsCom = false;
    }
}

QStringList Session::zunePnpIds(QString *error) {
    QStringList ids;
    setReason(error, QString());

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        setReason(error, explain(init));
        return ids;
    }

    {
        ComPtr<IPortableDeviceManager> manager;
        HRESULT hr = CoCreateInstance(__uuidof(PortableDeviceManager), nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
        if (failed(hr)) {
            // S_OK here is the proof that this machine has the WPD provider at
            // all.  It is absent from some Server SKUs, and then the honest
            // message is "this Windows cannot talk to a portable device", not
            // "no Zune found".
            setReason(error, QStringLiteral("Windows Portable Device API unavailable: %1")
                                 .arg(explain(hr)));
        } else {
            DWORD count = 0;
            hr = manager->GetDevices(nullptr, &count);
            if (failed(hr)) {
                setReason(error, QStringLiteral("Could not ask Windows for its portable devices: %1")
                                     .arg(explain(hr)));
            } else if (count) {
                QVector<LPWSTR> raw(int(count), nullptr);
                hr = manager->GetDevices(raw.data(), &count);
                if (failed(hr)) {
                    setReason(error, QStringLiteral("Could not read the portable device list: %1")
                                         .arg(explain(hr)));
                } else {
                    for (DWORD i = 0; i < count; ++i) {
                        const QString pnp = adopted(raw[int(i)]);
                        raw[int(i)] = nullptr;
                        if (isZunePnpId(pnp))
                            ids.append(pnp);
                    }
                }
                for (LPWSTR leftover : raw)
                    CoTaskMemFree(leftover);
            }
        }
    }

    if (SUCCEEDED(init))
        CoUninitialize();
    return ids;
}

bool Session::openFirstZune(QString *error) {
    const QStringList ids = zunePnpIds(error);
    if (ids.isEmpty()) {
        if (error && error->isEmpty())
            setReason(error, QStringLiteral(
                "No Zune that Windows has a portable-device driver for is plugged in."));
        return false;
    }
    return open(ids.first(), error);
}

bool Session::open(const QString &pnpId, QString *error) {
    setReason(error, QString());
    if (m_device) {
        if (m_pnpId == pnpId)
            return true;
        close();
    }
    if (!ensureCom()) {
        setReason(error, QStringLiteral("Could not initialise COM on this thread."));
        return false;
    }

    ComPtr<IPortableDevice> device;
    HRESULT hr = CoCreateInstance(__uuidof(PortableDevice), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&device));
    if (failed(hr)) {
        setReason(error, explain(hr));
        return false;
    }

    const ComPtr<IPortableDeviceValues> clientInfo = newValues();
    if (!clientInfo) {
        setReason(error, QStringLiteral("Could not build the WPD client description."));
        return false;
    }
    // WPD_CLIENT_NAME is what the player shows as "being synced by", and the
    // version quartet is a claim about which WPD contract we honour - not a
    // version of this application.
    clientInfo->SetStringValue(WPD_CLIENT_NAME, L"zuuned");
    clientInfo->SetUnsignedIntegerValue(WPD_CLIENT_MAJOR_VERSION, 1);
    clientInfo->SetUnsignedIntegerValue(WPD_CLIENT_MINOR_VERSION, 0);
    clientInfo->SetUnsignedIntegerValue(WPD_CLIENT_REVISION, 0);

    const std::wstring wide = toWide(pnpId);
    hr = device->Open(wide.c_str(), clientInfo.Get());
    if (failed(hr)) {
        setReason(error, QStringLiteral("Windows refused to hand over the player: %1")
                             .arg(explain(hr)));
        return false;
    }

    ComPtr<IPortableDeviceContent> content;
    hr = device->Content(&content);
    if (failed(hr)) {
        setReason(error, explain(hr));
        return false;
    }

    m_device = device.Detach();
    m_content = content.Detach();
    m_pnpId = pnpId;
    clearAbort();
    fprintf(stderr, "[wpd] opened %s\n", qUtf8Printable(pnpId));
    return true;
}

void Session::close() {
    if (m_content) {
        static_cast<IPortableDeviceContent *>(m_content)->Release();
        m_content = nullptr;
    }
    if (m_device) {
        auto *device = static_cast<IPortableDevice *>(m_device);
        device->Close();
        device->Release();
        m_device = nullptr;
    }
    m_pnpId.clear();
}

bool Session::readDevice(DeviceInfo *info, QString *error) {
    *info = DeviceInfo();
    info->pnpId = m_pnpId;
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    ComPtr<IPortableDeviceProperties> properties;
    HRESULT hr = content->Properties(&properties);
    if (failed(hr)) {
        setReason(error, explain(hr));
        return false;
    }

    const ComPtr<IPortableDeviceKeyCollection> keys = newKeys();
    if (!keys) {
        setReason(error, QStringLiteral("Could not build the WPD property request."));
        return false;
    }
    keys->Add(WPD_DEVICE_FRIENDLY_NAME);
    keys->Add(WPD_DEVICE_MODEL);
    keys->Add(WPD_DEVICE_MANUFACTURER);
    keys->Add(WPD_DEVICE_FIRMWARE_VERSION);
    keys->Add(WPD_DEVICE_POWER_LEVEL);
    keys->Add(WPD_DEVICE_POWER_SOURCE);
    ComPtr<IPortableDeviceValues> readBack;
    hr = properties->GetValues(WPD_DEVICE_OBJECT_ID, keys.Get(), &readBack);
    if (failed(hr)) {
        setReason(error, explain(hr));
        return false;
    }
    info->friendlyName = textAt(readBack.Get(), WPD_DEVICE_FRIENDLY_NAME);
    info->model = textAt(readBack.Get(), WPD_DEVICE_MODEL);
    info->manufacturer = textAt(readBack.Get(), WPD_DEVICE_MANUFACTURER);
    info->firmware = textAt(readBack.Get(), WPD_DEVICE_FIRMWARE_VERSION);
    bool hasBattery = false;
    const quint32 level = smallAt(readBack.Get(), WPD_DEVICE_POWER_LEVEL, &hasBattery);
    info->battery = hasBattery ? int(level) : -1;
    info->powerSource = smallAt(readBack.Get(), WPD_DEVICE_POWER_SOURCE) == 1
        ? QStringLiteral("external") : QStringLiteral("battery");
    // WPD_DEVICE_SERIAL_NUMBER is deliberately not asked for.  It is the one
    // property here that names a person's player, and nothing needs it: device
    // history is keyed by libzune's serial, and a WPD session must not write a
    // second identity into the profile.

    // Capacity is a per-storage number, so the device answer is the sum over the
    // storage functional objects.  A Zune has one; the loop does not assume it.
    QVector<ObjectInfo> top;
    if (!children(deviceRoot(), &top, error))
        return false;
    QVector<QPair<quint64, QString>> byFree;
    for (const ObjectInfo &object : std::as_const(top)) {
        if (object.kind != KindFunctional)
            continue;
        const ComPtr<IPortableDeviceKeyCollection> storageKeys = newKeys();
        if (!storageKeys)
            continue;
        storageKeys->Add(WPD_FUNCTIONAL_OBJECT_CATEGORY);
        storageKeys->Add(WPD_STORAGE_CAPACITY);
        storageKeys->Add(WPD_STORAGE_FREE_SPACE_IN_BYTES);
        ComPtr<IPortableDeviceValues> storageValues;
        const std::wstring storageId = toWide(object.id);
        if (failed(properties->GetValues(storageId.c_str(), storageKeys.Get(), &storageValues)))
            continue;
        GUID category{};
        if (failed(storageValues->GetGuidValue(WPD_FUNCTIONAL_OBJECT_CATEGORY, &category))
            || !IsEqualGUID(category, WPD_FUNCTIONAL_CATEGORY_STORAGE))
            continue;
        const quint64 capacity = numberAt(storageValues.Get(), WPD_STORAGE_CAPACITY);
        const quint64 free = numberAt(storageValues.Get(), WPD_STORAGE_FREE_SPACE_IN_BYTES);
        info->capacityBytes += capacity;
        info->freeBytes += free;
        byFree.append(qMakePair(free, object.id));
    }
    std::sort(byFree.begin(), byFree.end(),
              [](const QPair<quint64, QString> &a, const QPair<quint64, QString> &b) {
        return a.first > b.first;
    });
    for (const QPair<quint64, QString> &entry : std::as_const(byFree))
        info->storageIds.append(entry.second);
    return true;
}


bool Session::children(const QString &parentId, QVector<ObjectInfo> *out, QString *error) {
    out->clear();
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    ComPtr<IPortableDeviceProperties> properties;
    HRESULT hr = content->Properties(&properties);
    if (failed(hr)) {
        setReason(error, explain(hr));
        return false;
    }
    const QString root = parentId.isEmpty() ? deviceRoot() : parentId;
    const std::wstring wide = toWide(root);
    ComPtr<IEnumPortableDeviceObjectIDs> enumerator;
    hr = content->EnumObjects(0, wide.c_str(), nullptr, &enumerator);
    if (hr == S_FALSE)
        return true;   // an empty folder is not a failure
    if (failed(hr)) {
        setReason(error, QStringLiteral("Could not list %1: %2").arg(root, explain(hr)));
        return false;
    }
    int skipped = 0;
    for (;;) {
        LPWSTR rawId = nullptr;
        ULONG fetched = 0;
        hr = enumerator->Next(1, &rawId, &fetched);
        const QString id = adopted(rawId);
        if (fetched == 0 || id.isEmpty())
            break;
        ObjectInfo info;
        if (readObjectInfo(properties.Get(), id, &info))
            out->append(info);
        else
            ++skipped;
        if (failed(hr))
            break;
    }
    if (skipped)
        fprintf(stderr, "[wpd] %s: %d object(s) answered no properties and are not shown\n",
                qUtf8Printable(root), skipped);
    return true;
}

bool Session::objectInfo(const QString &id, ObjectInfo *out, QString *error) {
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    ComPtr<IPortableDeviceProperties> properties;
    HRESULT hr = content->Properties(&properties);
    if (failed(hr)) {
        setReason(error, explain(hr));
        return false;
    }
    if (!readObjectInfo(properties.Get(), id, out)) {
        setReason(error, QStringLiteral("The player answered nothing about object %1.").arg(id));
        return false;
    }
    return true;
}

bool Session::download(const QString &objectId, const QString &destPath, ProgressFn fn,
                       void *userdata, QString *error) {
    setReason(error, QString());
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    if (aborted()) {
        setReason(error, QStringLiteral("Cancelled before it started."));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    const std::wstring wide = toWide(objectId);

    // WPD has no `GetObject`.  The content interface hands out a resources
    // interface and the bytes live behind a resource key on that one, so a
    // download is two calls, not one.
    ComPtr<IPortableDeviceResources> resources;
    HRESULT hr = content->Transfer(&resources);
    if (failed(hr)) {
        setReason(error, QStringLiteral("The player would not open a transfer: %1")
                             .arg(explain(hr)));
        return false;
    }
    ComPtr<IStream> stream;
    DWORD optimal = 0;
    hr = resources->GetStream(wide.c_str(), WPD_RESOURCE_DEFAULT, 0, &optimal, &stream);
    if (failed(hr)) {
        setReason(error, QStringLiteral("The player would not give up object %1: %2")
                             .arg(objectId, explain(hr)));
        return false;
    }

    // A partial file that survives a failed download is worse than none: the
    // library scanner would index it.  Write beside the target and move on
    // success, exactly like the libzune path the app already trusts.
    const QString tempPath = destPath + QStringLiteral(".wpd-part");
    QFile out(tempPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setReason(error, QStringLiteral("Could not open %1 for writing.").arg(destPath));
        return false;
    }

    // The player's own declared size, not IStream::Stat: STATSTG spells that
    // field differently depending on which Windows header came first, and the
    // number that matters for a readback check is the one in WPD_OBJECT_SIZE.
    ObjectInfo declared;
    const quint64 total = objectInfo(objectId, &declared, nullptr) ? declared.size : 0;

    QByteArray chunk(1 << 18, Qt::Uninitialized);
    quint64 written = 0;
    bool ok = true;
    QString why;
    for (;;) {
        if (aborted()) {
            ok = false;
            why = QStringLiteral("Cancelled.");
            break;
        }
        ULONG got = 0;
        hr = stream->Read(chunk.data(), ULONG(chunk.size()), &got);
        if (failed(hr)) {
            ok = false;
            why = explain(hr);
            break;
        }
        if (got == 0)
            break;
        if (out.write(chunk.constData(), qint64(got)) != qint64(got)) {
            ok = false;
            why = out.errorString();
            break;
        }
        written += got;
        if (fn && total)
            fn(double(written) / double(total), userdata);
    }
    out.close();

    if (!ok || written == 0) {
        out.remove();
        if (ok)
            why = QStringLiteral("The player sent no bytes.");
        setReason(error, why);
        return false;
    }
    if (total && written != total) {
        out.remove();
        setReason(error, QStringLiteral("Short transfer: %1 of %2 bytes.")
                             .arg(written)
                             .arg(total));
        return false;
    }
    QFile::remove(destPath);
    if (!out.rename(destPath)) {
        out.remove();
        setReason(error, QStringLiteral("Could not put the finished file at %1.").arg(destPath));
        return false;
    }
    if (fn)
        fn(1.0, userdata);
    fprintf(stderr, "[wpd] downloaded %s -> %s (%llu bytes)\n", qUtf8Printable(objectId),
            qUtf8Printable(destPath), quint64(written));
    return true;
}

bool Session::supportedResourceNames(const QString &objectId, QStringList *names,
                                     QString *error) {
    setReason(error, QString());
    if (names)
        names->clear();
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    ComPtr<IPortableDeviceResources> resources;
    const HRESULT hrTransfer =
        static_cast<IPortableDeviceContent *>(m_content)->Transfer(&resources);
    if (failed(hrTransfer) || !resources) {
        setReason(error, QStringLiteral("This player offers no resource streams: %1")
                             .arg(explain(hrTransfer)));
        return false;
    }
    ComPtr<IPortableDeviceKeyCollection> keys;
    const HRESULT hr =
        resources->GetSupportedResources(toWide(objectId).c_str(), &keys);
    if (failed(hr)) {
        setReason(error, QStringLiteral("The player would not say what %1 has: %2")
                             .arg(objectId, explain(hr)));
        return false;
    }
    DWORD count = 0;
    if (keys)
        keys->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        PROPERTYKEY key{};
        if (SUCCEEDED(keys->GetAt(i, &key)) && names)
            names->append(resourceName(key));
    }
    return true;
}

bool Session::readThumbnail(const QString &objectId, const QString &destPath,
                           QString *error) {
    setReason(error, QString());
    ComPtr<IPortableDeviceResources> resources;
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    // There is no `Resources()` on the content interface.  The same accessor
    // a download uses hands out the resources interface, and the picture
    // lives behind a resource key on it.
    const HRESULT hrResource =
        static_cast<IPortableDeviceContent *>(m_content)->Transfer(&resources);
    if (failed(hrResource) || !resources) {
        setReason(error, QStringLiteral("This player offers no resource streams: %1")
                             .arg(explain(hrResource)));
        return false;
    }
    // THUMBNAIL first because it is the one a music player maintains for the
    // tiles in its own browser; ALBUM_ART is where a still-image library keeps
    // its picture, so a photo object answers there and not at THUMBNAIL.
    const PROPERTYKEY keys[] = {WPD_RESOURCE_THUMBNAIL, WPD_RESOURCE_ALBUM_ART};
    const std::wstring wide = toWide(objectId);
    QString lastError;
    for (const PROPERTYKEY &key : keys) {
        DWORD size = 0;
        ComPtr<IStream> stream;
        const HRESULT hr = resources->GetStream(wide.c_str(), key, STGM_READ, &size, &stream);
        if (failed(hr) || !stream) {
            // Which key answered no is the whole diagnostic here: a player
            // that maintains no thumbnails at all refuses both, and one that
            // only stores album art refuses THUMBNAIL alone.
            lastError = explain(hr);
            fprintf(stderr, "[wpd] no %s for %s: %s\n", qUtf8Printable(resourceName(key)),
                    qUtf8Printable(objectId), qUtf8Printable(lastError));
            continue;
        }
        QFile out(destPath);
        if (!out.open(QIODevice::WriteOnly)) {
            setReason(error, QStringLiteral("Could not write %1: %2")
                                 .arg(destPath, out.errorString()));
            return false;
        }
        const size_t chunkSize = 1 << 16;
        QByteArray chunk(int(chunkSize), Qt::Uninitialized);
        quint64 written = 0;
        bool ok = true;
        QString why;
        for (;;) {
            if (aborted()) {
                ok = false;
                why = QStringLiteral("Cancelled.");
                break;
            }
            ULONG got = 0;
            const HRESULT read = stream->Read(chunk.data(), ULONG(chunkSize), &got);
            if (failed(read)) {
                ok = false;
                why = explain(read);
                break;
            }
            if (got == 0)
                break;
            if (out.write(chunk.constData(), qint64(got)) != qint64(got)) {
                ok = false;
                why = out.errorString();
                break;
            }
            written += got;
        }
        out.close();
        if (!ok) {
            out.remove();
            setReason(error, why);
            return false;
        }
        if (written == 0) {
            out.remove();
            lastError = QStringLiteral("empty");
            continue;
        }
        fprintf(stderr, "[wpd] picture for %s came from resource %s: %llu bytes\n",
                qUtf8Printable(objectId),
                key.fmtid == WPD_RESOURCE_THUMBNAIL.fmtid && key.pid == WPD_RESOURCE_THUMBNAIL.pid
                    ? "THUMBNAIL"
                    : "ALBUM_ART",
                written);
        return true;
    }
    // Say what the player offered rather than only that it refused: "offers no
    // resources at all" is a fact about this player, "offers THUMBNAIL and
    // returns an error" is a fault worth digging into.
    QStringList offered;
    supportedResourceNames(objectId, &offered, nullptr);
    setReason(error, QStringLiteral("This player keeps no picture for %1 (%2; it offers %3)")
                         .arg(objectId, lastError,
                              offered.isEmpty() ? QStringLiteral("nothing")
                                                : offered.join(QLatin1Char('+'))));
    return false;
}

bool Session::makeFolder(const QString &parentId, const QString &name, QString *objectId,
                         QString *error) {
    setReason(error, QString());
    if (objectId)
        objectId->clear();
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    if (name.trimmed().isEmpty()) {
        setReason(error, QStringLiteral("A folder needs a name."));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    const ComPtr<IPortableDeviceValues> values = newValues();
    if (!values) {
        setReason(error, QStringLiteral("Could not build the WPD folder request."));
        return false;
    }
    const QString parent = parentId.isEmpty() ? deviceRoot() : parentId;
    const std::wstring parentWide = toWide(parent);
    const std::wstring nameWide = toWide(name);
    values->SetStringValue(WPD_OBJECT_PARENT_ID, parentWide.c_str());
    values->SetStringValue(WPD_OBJECT_NAME, nameWide.c_str());
    values->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, WPD_CONTENT_TYPE_FOLDER);

    LPWSTR created = nullptr;
    HRESULT hr = content->CreateObjectWithPropertiesOnly(values.Get(), &created);
    QString id = adopted(created);
    if (hr == kDeviceRefused) {
        // Measured on a Zune: the spec-pure folder request is refused with the
        // undifferentiated device-category error, and the retry below is what a
        // real player takes.  Explorer sends the extra property on every device
        // and other players do not need it, so asking the cheap way first keeps
        // it off the devices that would rather not be given it.
        const HRESULT first = hr;
        created = nullptr;
        values->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, nameWide.c_str());
        hr = content->CreateObjectWithPropertiesOnly(values.Get(), &created);
        id = adopted(created);
        if (failed(hr)) {
            setReason(error, QStringLiteral("The player refused to create '%1': %2, and again "
                                            "with its file name: %3")
                                 .arg(name, explain(first), explain(hr)));
            return false;
        }
        fprintf(stderr, "[wpd] this player needs WPD_OBJECT_ORIGINAL_FILE_NAME on a new folder\n");
    } else if (failed(hr)) {
        setReason(error, QStringLiteral("The player refused to create '%1': %2")
                             .arg(name, explain(hr)));
        return false;
    }
    if (objectId)
        *objectId = id;
    fprintf(stderr, "[wpd] folder '%s' under %s is object %s\n", qUtf8Printable(name),
            qUtf8Printable(parent), qUtf8Printable(id));
    return true;
}

bool Session::upload(const QString &parentId, const QString &srcPath, const ObjectInfo &meta,
                     ProgressFn fn, void *userdata, QString *objectId, QString *error) {
    setReason(error, QString());
    if (objectId)
        objectId->clear();
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    if (aborted()) {
        setReason(error, QStringLiteral("Cancelled before it started."));
        return false;
    }
    QFileInfo info(srcPath);
    if (!info.exists() || !info.isFile()) {
        setReason(error, QStringLiteral("%1 is not a file.").arg(srcPath));
        return false;
    }
    const quint64 size = quint64(info.size());
    QFile in(srcPath);
    if (!in.open(QIODevice::ReadOnly)) {
        setReason(error, QStringLiteral("Could not read %1: %2").arg(srcPath, in.errorString()));
        return false;
    }

    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    const QString parent = parentId.isEmpty() ? deviceRoot() : parentId;
    const QString fileName = meta.name.isEmpty() ? info.fileName() : meta.name;
    const QString displayName = displayObjectName(meta.title, fileName);
    const std::wstring parentWide = toWide(parent);
    const std::wstring fileNameWide = toWide(fileName);
    const std::wstring nameWide = toWide(displayName);
    // What the player files this file under.  Without these the Zune indexes real
    // music as untitled and Unknown Artist - measured on PID 0710 against the
    // same folder: the file Windows Explorer copied carries a duration, artist
    // and title, the one we copied without them carried none - and the library on
    // the player is the product a user actually sees.
    const bool hasMedia = hasMediaProperties(meta);
    GUID format = WPD_OBJECT_FORMAT_UNSPECIFIED;
    switch (formatForExtension(srcPath)) {
    case 1: format = WPD_OBJECT_FORMAT_WMA; break;
    case 2: format = WPD_OBJECT_FORMAT_MP3; break;
    case 3: format = WPD_OBJECT_FORMAT_FLAC; break;
    case 4: format = WPD_OBJECT_FORMAT_WAVE; break;
    default: break;
    }

    // What a player will take for a new file is a fact about the player, and the
    // only honest way to learn it is to ask in order.  Measured on the Zune at
    // PID 0710 through Windows' own WPD driver: the spec-pure create is refused
    // outright - the same refusal this player gives a new folder unless the
    // request names the object - so the shape carrying
    // WPD_OBJECT_ORIGINAL_FILE_NAME leads, and the spec-pure one stays further
    // down because a different player may reject an optional property instead of
    // requiring it.  A shape is judged on the whole transfer and not on the
    // create alone, because a provider can accept a create and then refuse the
    // first block of data; a ladder that only retried the create would stop there
    // forever.  A Session remembers which shape this player took, so only the
    // first file of a sync pays the question.
    struct Shape {
        GUID contentType;
        bool withContentType;
        bool withFormat;
        bool withSize;
        bool withFileName;
        bool withMedia;
        const char *label;
    };
    const GUID noContentType = GUID();
    // A player that takes the media properties gets a Zune library entry with a
    // name on it; one that refuses them still gets the file.  The stripped shape
    // is not a fallback for laziness, it is the shape measured to work on PID
    // 0710 before any media property was ever sent, so a player that chokes on
    // WPD_MEDIA_* still receives its music.
    const Shape shapes[] = {
        {WPD_CONTENT_TYPE_AUDIO, true, true, true, true, true,
         "audio/format/size/file name/media"},
        {WPD_CONTENT_TYPE_AUDIO, true, true, true, true, false, "audio/format/size/file name"},
        {WPD_CONTENT_TYPE_GENERIC_FILE, true, true, true, true, true,
         "generic file/format/size/name/media"},
        {WPD_CONTENT_TYPE_GENERIC_FILE, true, true, true, true, false,
         "generic file/format/size/name"},
        {WPD_CONTENT_TYPE_AUDIO, true, true, true, false, false, "audio/format/size"},
        {WPD_CONTENT_TYPE_AUDIO, true, false, true, true, true, "audio/size/file name/media"},
        {WPD_CONTENT_TYPE_AUDIO, true, true, false, true, true, "audio/format/file name/media"},
        {WPD_CONTENT_TYPE_GENERIC_FILE, true, false, false, true, true,
         "generic file/file name/media"},
        {WPD_CONTENT_TYPE_AUDIO, true, false, false, true, true, "audio/file name/media"},
        {noContentType, false, true, true, true, true, "format/size/file name/media"},
    };
    const int shapeCount = int(sizeof(shapes) / sizeof(shapes[0]));

    // Snapshot the destination once, before anything is created; see
    // settleCreatedId().  A transfer abandoned with Revert makes nothing, so
    // retrying shapes does not pollute the diff.
    const QSet<QString> before = childIds(content, parent);

    ComPtr<IStream> stream;
    QString cookie;
    const char *taken = "";
    QString why;
    bool settled = false;
    QVector<QPair<QString, QString>> refused;
    for (int index = 0; index < shapeCount; ++index) {
        if (m_uploadShape >= 0 && index != m_uploadShape)
            continue;
        const Shape &shape = shapes[index];
        if (shape.withMedia && !hasMedia)
            continue;   // nothing to say, so do not spend a shape saying it
        const ComPtr<IPortableDeviceValues> values = newValues();
        if (!values) {
            why = QStringLiteral("Could not build the WPD transfer request.");
            break;
        }
        values->SetStringValue(WPD_OBJECT_PARENT_ID, parentWide.c_str());
        values->SetStringValue(WPD_OBJECT_NAME, nameWide.c_str());
        if (shape.withContentType)
            values->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, shape.contentType);
        if (shape.withFormat)
            values->SetGuidValue(WPD_OBJECT_FORMAT, format);
        if (shape.withSize)
            values->SetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, size);
        if (shape.withFileName)
            values->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, fileNameWide.c_str());
        if (shape.withMedia)
            fillMediaProperties(values.Get(), meta);

        stream.Reset();
        DWORD bufferSize = 0;
        LPWSTR rawCookie = nullptr;
        const HRESULT hr = content->CreateObjectWithPropertiesAndData(values.Get(), &stream,
                                                                     &bufferSize, &rawCookie);
        cookie = adopted(rawCookie);
        if (failed(hr) || !stream) {
            refused.append(qMakePair(QString::fromLatin1(shape.label),
                                     QStringLiteral("create: %1").arg(explain(hr))));
            if (hr != kDeviceRefused)
                // Anything but the category error is the player actually saying
                // something, and no other shape will be told differently.
                break;
            continue;
        }
        if (bufferSize == 0)
            bufferSize = 1 << 16;
        QByteArray chunk(int(bufferSize), Qt::Uninitialized);
        if (!in.seek(0)) {
            why = QStringLiteral("Could not rewind %1 to send it again.").arg(srcPath);
            break;
        }

        quint64 sent = 0;
        bool ok = true;
        why.clear();
        while (sent < size) {
            if (aborted()) {
                ok = false;
                why = QStringLiteral("Cancelled.");
                break;
            }
            const qint64 got = in.read(chunk.data(), chunk.size());
            if (got < 0) {
                ok = false;
                why = in.errorString();
                break;
            }
            if (got == 0)
                break;
            ULONG written = 0;
            const HRESULT write = stream->Write(chunk.constData(), ULONG(got), &written);
            if (failed(write) || written != ULONG(got)) {
                ok = false;
                why = QStringLiteral("the player refused %1 of its %2 bytes at offset %3 in "
                                     "blocks of %4: %5")
                          .arg(got)
                          .arg(size)
                          .arg(sent)
                          .arg(bufferSize)
                          .arg(explain(write));
                break;
            }
            sent += quint64(got);
            if (fn && size)
                fn(double(sent) / double(size), userdata);
        }

        if (ok) {
            // Closing the stream is the transfer, not a cleanup: until Commit the
            // player holds a partial object.  A cancel path deliberately does not
            // commit.
            const HRESULT commit = stream->Commit(STGC_DEFAULT);
            if (failed(commit)) {
                ok = false;
                why = QStringLiteral("the player did not accept the finished file: %1")
                          .arg(explain(commit));
            }
        }
        if (ok) {
            taken = shape.label;
            m_uploadShape = index;
            settled = true;
            break;
        }
        refused.append(qMakePair(QString::fromLatin1(shape.label),
                                 QStringLiteral("data: %1").arg(why)));
        // Abandon this transfer before asking another way, so a refused file does
        // not leave half of itself on the player.  Revert is documented for
        // exactly this, and its own failure is not worth a second complaint.
        stream->Revert();
        stream.Reset();
        if (m_uploadShape >= 0)
            break;   // the shape this player already took just failed: say so once
        if (aborted())
            break;
    }
    in.close();
    // Every shape the player refused, printed whether or not a later shape worked.
    // "It would not take the file" tells a user nothing, and a transfer that
    // succeeds while saying what it had to add is the measurement that turns the
    // next player into a reordering of that table rather than a guess.
    for (const auto &attempt : std::as_const(refused))
        fprintf(stderr, "[wpd] refused [%s] %s\n", qUtf8Printable(attempt.first),
                qUtf8Printable(attempt.second));
    if (!settled) {
        if (why.isEmpty())
            why = explain(kDeviceRefused);
        setReason(error, QStringLiteral("The player would not accept '%1': %2")
                             .arg(fileName, why));
        return false;
    }
    if (fn)
        fn(1.0, userdata);

    // Which object did that become?  The cookie is the transfer handle, and what
    // it names is a property of the WPD provider rather than of this file, so the
    // id is settled the only way that cannot be wrong: ask the player what is in
    // the folder now and take what was not there before.  The cookie is logged
    // beside it so the next person can see what it really was.
    const QString created = settleCreatedId(content, parent, before, cookie, fileName);
    if (objectId)
        *objectId = created;

    // A create that is accepted is not a create that is obeyed.  Measured on the
    // player at PID 0710: artist, duration and track number survived the create,
    // album, genre and title did not, while the objects the Zune software synced
    // itself carry all of them - the provider answered part of the request and
    // said nothing.  So ask a second time for the object that now exists, which is
    // what IPortableDeviceProperties::SetValues is for, and print what the player
    // ended up filing either way: that line is the difference between a track a
    // user can find in the player's own album view and one filed under Unknown.
    if (!created.isEmpty() && hasMedia) {
        ObjectInfo filed;
        QString unreadable;
        QString createdWideSource = created;
        ComPtr<IPortableDeviceProperties> properties;
        if (SUCCEEDED(content->Properties(&properties)) && properties) {
            const ComPtr<IPortableDeviceValues> patch = newValues();
            fillMediaProperties(patch.Get(), meta);
            ComPtr<IPortableDeviceValues> refusedValues;
            const std::wstring createdWide = toWide(createdWideSource);
            const HRESULT set =
                properties->SetValues(createdWide.c_str(), patch.Get(), &refusedValues);
            if (failed(set))
                fprintf(stderr, "[wpd] the player would not restamp properties on %s: %s\n",
                        qUtf8Printable(created), qUtf8Printable(explain(set)));
        }
        if (objectInfo(created, &filed, &unreadable))
            fprintf(stderr,
                    "[wpd] player filed %s as artist='%s' album='%s' title='%s' "
                    "track=%u duration=%u\n",
                    qUtf8Printable(created), qUtf8Printable(filed.artist),
                    qUtf8Printable(filed.album), qUtf8Printable(filed.title),
                    filed.trackNumber, filed.durationMs);
    }
    fprintf(stderr, "[wpd] uploaded %s (%llu bytes) into %s as object %s (cookie %s, %s)\n",
            qUtf8Printable(fileName), size, qUtf8Printable(parent),
            qUtf8Printable(created.isEmpty() ? QStringLiteral("unknown") : created),
            qUtf8Printable(cookie.isEmpty() ? QStringLiteral("-") : cookie), taken);
    return true;
}

bool Session::removeObjects(const QStringList &objectIds, QStringList *purged,
                            QString *error) {
    setReason(error, QString());
    if (purged)
        purged->clear();
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    int failures = 0;
    QString lastFailure;
    for (const QString &id : objectIds) {
        const ComPtr<IPortableDevicePropVariantCollection> ids =
            newPropVariantCollection();
        if (!ids) {
            setReason(error, QStringLiteral("Could not build the WPD delete request."));
            return false;
        }
        const std::wstring idWide = toWide(id);
        PROPVARIANT value{};
        if (failed(InitPropVariantFromString(idWide.c_str(), &value))) {
            ++failures;
            continue;
        }
        const HRESULT added = ids->Add(&value);
        PropVariantClear(&value);
        if (failed(added)) {
            ++failures;
            lastFailure = explain(added);
            continue;
        }
        ComPtr<IPortableDevicePropVariantCollection> results;
        const HRESULT hr = content->Delete(0, ids.Get(), &results);
        if (failed(hr)) {
            ++failures;
            lastFailure = QStringLiteral("%1: %2").arg(id, explain(hr));
            fprintf(stderr, "[wpd] delete %s failed: %s\n", qUtf8Printable(id),
                    qUtf8Printable(lastFailure));
            continue;
        }
        // A provider can answer S_FALSE when one object in a batch was refused.
        // One id per call, so the return code is about the object we asked about.
        if (hr == S_FALSE) {
            ++failures;
            lastFailure = QStringLiteral("%1: the player declined to delete it").arg(id);
            continue;
        }
        if (purged)
            purged->append(id);
        fprintf(stderr, "[wpd] deleted %s\n", qUtf8Printable(id));
    }
    if (purged && purged->isEmpty() && failures) {
        setReason(error, lastFailure.isEmpty() ? QStringLiteral("Nothing was deleted.")
                                               : lastFailure);
        return false;
    }
    return true;
}

QString Session::kindName(int kind) {
    switch (kind) {
    case KindFolder: return QStringLiteral("folder");
    case KindAudio: return QStringLiteral("audio");
    case KindImage: return QStringLiteral("image");
    case KindVideo: return QStringLiteral("video");
    case KindFunctional: return QStringLiteral("functional");
    case KindOther: return QStringLiteral("other");
    default: return QStringLiteral("unknown");
    }
}

int Session::formatForExtension(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("wma"))
        return 1;
    if (suffix == QLatin1String("mp3"))
        return 2;
    if (suffix == QLatin1String("flac"))
        return 3;
    if (suffix == QLatin1String("wav"))
        return 4;
    return 0;
}

QString Session::extensionForFormat(const QString &formatGuidText) {
    if (formatGuidText.isEmpty())
        return QString();
    // Compare as GUIDs, not as text.  The player answers WPD_OBJECT_FORMAT with a
    // GUID and StringFromGUID2 spells it with braces and upper case; a string
    // compare here would make the extension of a downloaded file depend on how a
    // provider happened to be printed rather than on what the file is.
    const QUuid parsed(formatGuidText);
    if (parsed.isNull())
        return QString();
    // Qt 6 gives QUuid an implicit conversion to the platform GUID and dropped
    // the named accessor Qt 5 had, so spell the conversion out.
    const GUID format = static_cast<GUID>(parsed);
    if (IsEqualGUID(format, WPD_OBJECT_FORMAT_WMA))
        return QStringLiteral("wma");
    if (IsEqualGUID(format, WPD_OBJECT_FORMAT_MP3))
        return QStringLiteral("mp3");
    if (IsEqualGUID(format, WPD_OBJECT_FORMAT_FLAC))
        return QStringLiteral("flac");
    if (IsEqualGUID(format, WPD_OBJECT_FORMAT_WAVE))
        return QStringLiteral("wav");
    return QString();
}

} // namespace Wpd
