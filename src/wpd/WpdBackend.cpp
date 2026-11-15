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

#include <QCryptographicHash>
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
        if (IsEqualGUID(type, WPD_CONTENT_TYPE_PLAYLIST))
            return Wpd::KindPlaylist;
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

QString Session::maskedPathText(const QString &pnpId) {
    // \?\usb#vid_045e&pid_0710#<instance>#{interface class} - the only shape a
    // PnP path comes in, so anything else is not a path this code should be
    // printing at all.
    const int first = pnpId.indexOf(QLatin1Char('#'));
    const int second = pnpId.indexOf(QLatin1Char('#'), first + 1);
    if (first < 1 || second < 0)
        return QStringLiteral("[device path withheld]");
    const int third = pnpId.indexOf(QLatin1Char('#'), second + 1);
    const QString head = pnpId.left(second + 1);
    const QString tail = third < 0 ? QString() : pnpId.mid(third);
    const QString instance = third < 0
        ? pnpId.mid(second + 1)
        : pnpId.mid(second + 1, third - second - 1);
    const QByteArray alias = QCryptographicHash::hash(instance.toUtf8(),
                                                      QCryptographicHash::Sha1)
                                 .left(3).toHex();
    return head + QStringLiteral("serial-") + QString::fromLatin1(alias) + tail;
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
    // Masked, because this line is in every session log and the instance chunk
    // of a PnP path is the player's serial number.
    fprintf(stderr, "[wpd] opened %s\n", qUtf8Printable(maskedPathText(pnpId)));
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

namespace {

// Renders one property value as a person needs to read it.  PropVariantToString
// already knows the types WPD puts on an object - strings, numbers, GUIDs, the
// few key vectors - so this only handles what that call refuses, and says which
// variant type it was rather than pretending to have decoded it.  The point of
// this is to name what a player stored, not to be a serialiser.
// A vector is how a player hands over a list of other objects, which is exactly
// the question a playlist asks, so "vt=0x1048" is not an answer.  The element
// types WPD actually puts on an object are named here; anything else says how
// many there were and what it was called, which is still more than nothing.
QString describeVector(const PROPVARIANT &value) {
    const unsigned element = unsigned(value.vt & ~unsigned(VT_VECTOR));
    QString rendered;
    unsigned count = 0;
    switch (element) {
    case VT_CLSID:
        count = unsigned(value.cauuid.cElems);
        for (unsigned i = 0; i < count && i < 64; ++i)
            rendered += (i ? QStringLiteral(" ") : QString())
                        + QUuid(value.cauuid.pElems[i]).toString();
        break;
    case VT_LPWSTR:
        count = unsigned(value.calpwstr.cElems);
        for (unsigned i = 0; i < count && i < 64; ++i)
            rendered += (i ? QStringLiteral(" ") : QString())
                        + QString::fromWCharArray(value.calpwstr.pElems[i]
                                                      ? value.calpwstr.pElems[i]
                                                      : L"");
        break;
    case VT_UI4:
        count = unsigned(value.caul.cElems);
        for (unsigned i = 0; i < count && i < 64; ++i)
            rendered += QString::asprintf(i ? " %u" : "%u", unsigned(value.caul.pElems[i]));
        break;
    case VT_I4:
        count = unsigned(value.cal.cElems);
        for (unsigned i = 0; i < count && i < 64; ++i)
            rendered += QString::asprintf(i ? " %d" : "%d", int(value.cal.pElems[i]));
        break;
    case VT_UI2:
        count = unsigned(value.caui.cElems);
        for (unsigned i = 0; i < count && i < 64; ++i)
            rendered += QString::asprintf(i ? " %u" : "%u", unsigned(value.caui.pElems[i]));
        break;
    case VT_BOOL:
        count = unsigned(value.cabool.cElems);
        for (unsigned i = 0; i < count && i < 64; ++i)
            rendered += value.cabool.pElems[i] ? QStringLiteral(" true")
                                               : QStringLiteral(" false");
        break;
    default:
        return QString::asprintf("(unrendered vt=0x%04X vector)", unsigned(value.vt));
    }
    const QString tail = count > 64 ? QStringLiteral(" ...") : QString();
    return QString::asprintf("[%u] %s%s", count, qPrintable(rendered), qPrintable(tail));
}

QString describeVariant(const PROPVARIANT &value) {
    if (value.vt == VT_EMPTY)
        return QStringLiteral("(empty)");
    if (value.vt & VT_VECTOR)
        return describeVector(value);
    if (value.vt == VT_BLOB) {
        // Membership of a library record hides in a blob, so the bytes are the
        // answer and their length is not.  Both readings that matter go out: the
        // hex, and the UTF-16 decoding WPD names themselves tend to be.
        const unsigned char *bytes =
            static_cast<const unsigned char *>(value.blob.pBlobData);
        const unsigned n = unsigned(value.blob.cbSize);
        const unsigned shown = n < 320u ? n : 320u;
        QString hex;
        QString wide;
        for (unsigned i = 0; i < shown; ++i)
            hex += QString::asprintf(i ? " %02X" : "%02X", bytes[i]);
        for (unsigned i = 0; i + 1 < shown; i += 2) {
            const ushort unit = ushort(bytes[i] | (unsigned(bytes[i + 1]) << 8));
            wide += unit >= 0x20 && unit < 0xFFFD ? QChar(unit) : QChar('.');
        }
        return QString::asprintf("(%u byte blob) ", n) + hex
               + (n > shown ? QStringLiteral(" ...") : QString())
               + QStringLiteral("  utf16[%1]").arg(wide);
    }
    if (value.vt == VT_CLSID && value.puuid)
        return QUuid(*value.puuid).toString();
    if (value.vt == VT_ERROR)
        // In a results collection WPD puts the per-property verdict here, so the
        // scode IS the answer: "not set", "not supported", "access denied".
        return QString::asprintf("(no value: 0x%08X %s)", unsigned(value.scode),
                                 qPrintable(explain(value.scode)));
    if (value.vt == VT_UNKNOWN && value.punkVal) {
        // WPD promises WPD_OBJECT_REFERENCES is a CLSID array and this player
        // answers with an interface instead.  Ask it what it actually is rather
        // than guessing, and print the members if it is a collection - a
        // playlist is exactly a member list, so this is the fact the whole
        // feature turns on.
        ComPtr<IPortableDevicePropVariantCollection> bag;
        if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&bag)))) {
            DWORD count = 0;
            bag->GetCount(&count);
            QString items;
            for (DWORD i = 0; i < count && i < 64; ++i) {
                PROPVARIANT member{};
                if (FAILED(bag->GetAt(i, &member)))
                    continue;
                items += (items.isEmpty() ? QString() : QStringLiteral(", "))
                         + describeVariant(member);
                PropVariantClear(&member);
            }
            return QStringLiteral("IPortableDevicePropVariantCollection[%1] {%2}%3")
                .arg(count).arg(items, count > 64 ? QStringLiteral(" ...") : QString());
        }
        ComPtr<IPortableDeviceValues> table;
        if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&table)))) {
            DWORD count = 0;
            table->GetCount(&count);
            return QStringLiteral("IPortableDeviceValues[%1]").arg(count);
        }
        ComPtr<IPortableDeviceKeyCollection> keys;
        if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&keys)))) {
            DWORD count = 0;
            keys->GetCount(&count);
            return QStringLiteral("IPortableDeviceKeyCollection[%1]").arg(count);
        }
        ComPtr<IEnumUnknown> each;
        if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&each)))) {
            DWORD count = 0;
            ComPtr<IUnknown> item;
            while (each->Next(1, &item, nullptr) == S_OK && count < 4096) {
                ++count;
                item.Reset();
            }
            return QStringLiteral("IEnumUnknown[%1+]").arg(count);
        }
        return QStringLiteral("(vt=0x000D interface, none of the WPD collections)");
    }
    WCHAR text[512] = {};
    if (SUCCEEDED(PropVariantToString(value, text, ARRAYSIZE(text))))
        return QString::fromWCharArray(text);
    return QString::asprintf("(vt=0x%04X)", unsigned(value.vt));
}

// The properties worth reading when the question is "what did the player decide
// this object is".  Not every property WPD names: the identity and type pair, the
// flags by which a player says whether a file is its own library or only bytes on
// the volume, and the audio, video and Zune metadata fields that decision gets
// made out of.  A player is allowed to rewrite what it was sent, and this is the
// only way to tell that from having announced it badly.
struct NamedKey {
    const char *name;
    PROPERTYKEY key;
};

const NamedKey probeKeys[] = {
    {"object.id", WPD_OBJECT_ID},
    {"object.name", WPD_OBJECT_NAME},
    {"object.original-file-name", WPD_OBJECT_ORIGINAL_FILE_NAME},
    {"object.parent-id", WPD_OBJECT_PARENT_ID},
    {"object.size", WPD_OBJECT_SIZE},
    {"object.content-type", WPD_OBJECT_CONTENT_TYPE},
    {"object.format", WPD_OBJECT_FORMAT},
    {"object.can-delete", WPD_OBJECT_CAN_DELETE},
    {"object.is-hidden", WPD_OBJECT_ISHIDDEN},
    {"object.is-system", WPD_OBJECT_ISSYSTEM},
    {"object.non-consumable", WPD_OBJECT_NON_CONSUMABLE},
    {"object.drm-protected", WPD_OBJECT_IS_DRM_PROTECTED},
    {"object.persistent-unique-id", WPD_OBJECT_PERSISTENT_UNIQUE_ID},
    {"object.sync-id", WPD_OBJECT_SYNC_ID},
    {"object.date-created", WPD_OBJECT_DATE_CREATED},
    {"object.date-modified", WPD_OBJECT_DATE_MODIFIED},
    {"object.hint-location-display-name", WPD_OBJECT_HINT_LOCATION_DISPLAY_NAME},
    {"object.back-references", WPD_OBJECT_BACK_REFERENCES},
    {"object.references", WPD_OBJECT_REFERENCES},
    {"object.supported-units", WPD_OBJECT_SUPPORTED_UNITS},
    {"media.title", WPD_MEDIA_TITLE},
    {"media.artist", WPD_MEDIA_ARTIST},
    {"media.album-artist", WPD_MEDIA_ALBUM_ARTIST},
    {"music.album", WPD_MUSIC_ALBUM},
    {"media.genre", WPD_MEDIA_GENRE},
    {"music.track", WPD_MUSIC_TRACK},
    {"media.duration", WPD_MEDIA_DURATION},
    {"media.width", WPD_MEDIA_WIDTH},
    {"media.height", WPD_MEDIA_HEIGHT},
    {"media.description", WPD_MEDIA_DESCRIPTION},
    {"media.sub-title", WPD_MEDIA_SUB_TITLE},
    {"media.meta-genre", WPD_MEDIA_META_GENRE},
    {"media.composer", WPD_MEDIA_COMPOSER},
    {"media.byte-bookmark", WPD_MEDIA_BYTE_BOOKMARK},
    {"media.time-bookmark", WPD_MEDIA_TIME_BOOKMARK},
    {"media.parental-rating", WPD_MEDIA_PARENTAL_RATING},
    {"media.star-rating", WPD_MEDIA_STAR_RATING},
    {"media.use-count", WPD_MEDIA_USE_COUNT},
    {"media.skip-count", WPD_MEDIA_SKIP_COUNT},
    {"video.author", WPD_VIDEO_AUTHOR},
    {"video.framerate", WPD_VIDEO_FRAMERATE},
    {"video.fourcc-code", WPD_VIDEO_FOURCC_CODE},
    {"video.bitrate", WPD_VIDEO_BITRATE},
    {"video.scan-type", WPD_VIDEO_SCAN_TYPE},
    {"video.quality-setting", WPD_VIDEO_QUALITY_SETTING},
    {"image.bitdepth", WPD_IMAGE_BITDEPTH},
    {"image.horizontal-resolution", WPD_IMAGE_HORIZONTAL_RESOLUTION},
};

} // namespace

bool Session::describe(const QString &id, QVector<QString> *lines, QString *error) {
    setReason(error, QString());
    if (!lines) {
        setReason(error, QStringLiteral("No place to put the answer."));
        return false;
    }
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    ComPtr<IPortableDeviceProperties> properties;
    const HRESULT listed = content->Properties(&properties);
    if (failed(listed)) {
        setReason(error, explain(listed));
        return false;
    }
    const std::wstring wide = toWide(id);
    // One property per question.  A single GetValues for a long list lets one
    // unsupported key be the player's excuse to answer nothing, and "this player
    // has no such property" is precisely the fact worth keeping here.
    for (const NamedKey &entry : probeKeys) {
        const ComPtr<IPortableDeviceKeyCollection> one = newKeys();
        if (!one)
            break;
        one->Add(entry.key);
        ComPtr<IPortableDeviceValues> values;
        const HRESULT asked = properties->GetValues(wide.c_str(), one.Get(), &values);
        if (failed(asked)) {
            lines->append(QStringLiteral("%1 = no answer (%2)")
                              .arg(QString::fromLatin1(entry.name), explain(asked)));
            continue;
        }
        PROPVARIANT value{};
        const HRESULT got = values->GetValue(entry.key, &value);
        if (failed(got)) {
            PropVariantClear(&value);
            lines->append(QStringLiteral("%1 = not stored (%2)")
                              .arg(QString::fromLatin1(entry.name), explain(got)));
            continue;
        }
        lines->append(QStringLiteral("%1 = %2")
                          .arg(QString::fromLatin1(entry.name), describeVariant(value)));
        PropVariantClear(&value);
    }
    return true;
}
// Everything this object has, named by the player rather than by this file.
//
// `describe` asks a list of keys this code thought of; a Zune keeps its library
// records in properties nobody guessed, so when the question is "what makes a
// playlist a playlist" the only honest ask is the player's own list.  Ask for
// them in one batch - `GetAllProperties` has already named the supported ones,
// so there is no unsupported key for the player to hide behind - and fall back
// to one question at a time if the batch comes back empty.
bool Session::describeAll(const QString &id, QVector<QString> *lines, QString *error) {
    setReason(error, QString());
    if (!lines) {
        setReason(error, QStringLiteral("No place to put the answer."));
        return false;
    }
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    ComPtr<IPortableDeviceProperties> properties;
    if (failed(content->Properties(&properties))) {
        setReason(error, explain(E_FAIL));
        return false;
    }
    const std::wstring wide = toWide(id);
    ComPtr<IPortableDeviceKeyCollection> keys;
    if (failed(properties->GetSupportedProperties(wide.c_str(), &keys)) || !keys) {
        setReason(error, QStringLiteral("The player would not list the properties"
                                        " it keeps for object %1.").arg(id));
        return false;
    }
    DWORD count = 0;
    keys->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        PROPERTYKEY key{};
        if (failed(keys->GetAt(i, &key)))
            continue;
        const ComPtr<IPortableDeviceKeyCollection> one = newKeys();
        if (!one)
            break;
        one->Add(key);
        ComPtr<IPortableDeviceValues> values;
        if (failed(properties->GetValues(wide.c_str(), one.Get(), &values)))
            continue;
        PROPVARIANT value{};
        if (failed(values->GetValue(key, &value))) {
            PropVariantClear(&value);
            continue;
        }
        lines->append(QStringLiteral("%1/%2 = %3")
                          .arg(QUuid(key.fmtid).toString().toUpper())
                          .arg(key.pid)
                          .arg(describeVariant(value)));
        PropVariantClear(&value);
    }
    return true;
}

// What this player says about a format it has no name for, asked before anything
// is written to it.
//
// WPD has no format codes, only format GUIDs, and for the vendor-extended range
// the MTP code sits in the top half of the first field with the WPD format
// namespace behind it - MTP 0xB001 is {B0010000-AE6C-4804-98BA-C57B46965FE7}.  A
// Zune playlist is MTP 0xBA05, which the SDK does not name: it names the siblings
// BA06, BA0B and BB81 and skips the one this port needs.  So the code becomes a
// GUID here and the GUID gets asked about.  Three questions decide whether a
// playlist can be built over WPD at all - does this player take the format, which
// properties does it keep for it, and will it let a client write each one - and
// every one of them is a capability call rather than a write.
namespace {

// The WPD format namespace, which is every format GUID except the part carrying
// the code.  Microsoft's own legacy format ids share the tail (WMA is
// {00021000-...}) and do not carry an MTP code up top, so this recognises "a
// format in this namespace", never "this high word is a code".
bool wpdFormatNamespace(const GUID &guid) {
    return guid.Data2 == 0xAE6C && guid.Data3 == 0x4804 && guid.Data4[0] == 0x98
        && guid.Data4[1] == 0xBA && guid.Data4[2] == 0xC5 && guid.Data4[3] == 0x7B
        && guid.Data4[4] == 0x46 && guid.Data4[5] == 0x96 && guid.Data4[6] == 0x5F
        && guid.Data4[7] == 0xE7;
}

GUID formatGuidFromCode(quint32 code) {
    return GUID{DWORD((code & 0xFFFFu) << 16), 0xAE6C, 0x4804,
                {0x98, 0xBA, 0xC5, 0x7B, 0x46, 0x96, 0x5F, 0xE7}};
}

// "BA05", "0xBA05" or a whole GUID: either way, the GUID the player is asked for.
bool parseFormatText(const QString &text, GUID *guid, QString *why) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        setReason(why, QStringLiteral("No format to ask about."));
        return false;
    }
    if (trimmed.startsWith(QLatin1Char('{')) || trimmed.size() >= 32) {
        const QUuid uuid(trimmed);
        if (uuid.isNull()) {
            setReason(why, QStringLiteral("%1 is not a format GUID.").arg(text));
            return false;
        }
        *guid = GUID{uuid.data1, uuid.data2, uuid.data3,
                     {uchar(uuid.data4[0]), uchar(uuid.data4[1]), uchar(uuid.data4[2]),
                      uchar(uuid.data4[3]), uchar(uuid.data4[4]), uchar(uuid.data4[5]),
                      uchar(uuid.data4[6]), uchar(uuid.data4[7])}};
        return true;
    }
    const QString digits = trimmed.startsWith(QLatin1String("0x")) ? trimmed.mid(2) : trimmed;
    bool ok = false;
    const quint64 code = digits.toULongLong(&ok, 16);
    if (!ok || code > 0xFFFFu) {
        setReason(why, QStringLiteral("%1 is neither an MTP format code nor a format GUID.")
                           .arg(text));
        return false;
    }
    *guid = formatGuidFromCode(quint32(code));
    return true;
}

// One GUID out of a GUID collection, which is how WPD hands over categories,
// content types and formats.
bool guidAt(IPortableDevicePropVariantCollection *collection, DWORD index, GUID *guid) {
    PROPVARIANT value{};
    if (!collection || FAILED(collection->GetAt(index, &value))) {
        PropVariantClear(&value);
        return false;
    }
    const bool ok = value.vt == VT_CLSID && value.puuid != nullptr;
    if (ok)
        *guid = *value.puuid;
    PropVariantClear(&value);
    return ok;
}

// How many answers a collection holds, for the two kinds this probe walks.  A
// collection the player declined to hand over counts as none.
DWORD countOf(IPortableDevicePropVariantCollection *collection) {
    DWORD count = 0;
    if (collection)
        collection->GetCount(&count);
    return count;
}

DWORD countOf(IPortableDeviceKeyCollection *collection) {
    DWORD count = 0;
    if (collection)
        collection->GetCount(&count);
    return count;
}

DWORD countOf(IPortableDeviceValues *collection) {
    DWORD count = 0;
    if (collection)
        collection->GetCount(&count);
    return count;
}

// The MTP-extension verb set, spelled out rather than included.  The SDK header
// for these carries no include guard and its keys are not in the guid library this
// target links, so the numbers come from it by hand: one GUID for every key, the
// verbs at pid 11 to 18 and the operands from 1001.
constexpr GUID kMtpExtensions{0x4d545058, 0x1a2e, 0x4106,
                              {0xa3, 0x57, 0x77, 0x1e, 0x08, 0x19, 0xfc, 0x56}};

const char *mtpExtensionVerbName(unsigned long pid) {
    switch (pid) {
    case 11: return "get-supported-vendor-opcodes";
    case 12: return "execute-without-data-phase";
    case 13: return "execute-with-data-to-read";
    case 14: return "execute-with-data-to-write";
    case 15: return "read-data";
    case 16: return "write-data";
    case 17: return "end-data-transfer";
    case 18: return "get-vendor-extension-description";
    default: return nullptr;
    }
}

// The four facts worth having about one property of one format, on one line: what
// the player calls it, what type it wants, and whether a client may read, write
// and delete it.  A fact the player declines to state is a question mark and not a
// no: `SetValues` is refused per property on exactly this attribute, so "unstated"
// and "refused" are different things to tell a user about.
QString formatAttributeText(IPortableDeviceValues *attributes) {
    if (!attributes)
        return QStringLiteral("(no answer)");
    bool stated = false;
    QString text;
    const QString name = textAt(attributes, WPD_PROPERTY_ATTRIBUTE_NAME);
    if (!name.isEmpty())
        text += QStringLiteral("name=%1 ").arg(name);
    const quint32 varType = smallAt(attributes, WPD_PROPERTY_ATTRIBUTE_VARTYPE, &stated);
    text += stated ? QString::asprintf("vartype=0x%04X ", varType)
                   : QStringLiteral("vartype=? ");
    struct Flag {
        const char *label;
        PROPERTYKEY key;
    };
    const Flag flags[] = {{"can_read", WPD_PROPERTY_ATTRIBUTE_CAN_READ},
                          {"can_write", WPD_PROPERTY_ATTRIBUTE_CAN_WRITE},
                          {"can_delete", WPD_PROPERTY_ATTRIBUTE_CAN_DELETE}};
    for (const Flag &flag : flags) {
        bool answered = false;
        const bool value = flagAt(attributes, flag.key, &answered);
        text += QStringLiteral("%1=%2 ")
                    .arg(QString::fromLatin1(flag.label),
                         !answered ? QStringLiteral("?")
                                   : (value ? QStringLiteral("yes") : QStringLiteral("no")));
    }
    return text.trimmed();
}

} // namespace

bool Session::probeFormat(const QString &formatText, QVector<QString> *lines,
                          QString *error, const QString &objectId) {
    setReason(error, QString());
    if (!lines) {
        setReason(error, QStringLiteral("No place to put the answer."));
        return false;
    }
    if (!m_device) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    GUID target{};
    QString why;
    if (!parseFormatText(formatText, &target, &why)) {
        setReason(error, why);
        return false;
    }
    auto *device = static_cast<IPortableDevice *>(m_device);
    ComPtr<IPortableDeviceCapabilities> capabilities;
    const HRESULT got = device->Capabilities(&capabilities);
    if (failed(got) || !capabilities) {
        setReason(error, explain(got));
        return false;
    }

    lines->append(QStringLiteral("format.requested = %1").arg(formatText.trimmed()));
    lines->append(QStringLiteral("format.guid = %1").arg(guidToText(target)));
    if (wpdFormatNamespace(target)) {
        lines->append(QString::asprintf("format.high_word = 0x%04X",
                                        uint(target.Data1 >> 16)));
    }

    // Does the player take this format at all?  WPD answers that per content type,
    // so the walk is categories, then each one's content types, then their
    // formats.  What comes back is counted rather than dumped - a conformant
    // player names dozens of formats per type - and the two things worth keeping
    // are whether the target appears and which vendor-extended codes it does name.
    bool advertised = false;
    ComPtr<IPortableDevicePropVariantCollection> categories;
    const HRESULT listed = capabilities->GetFunctionalCategories(&categories);
    if (failed(listed) || !categories) {
        lines->append(QStringLiteral("categories = no answer (%1)").arg(explain(listed)));
    } else {
        QSet<QString> vendorFormats;
        const DWORD count = countOf(categories.Get());
        quint32 total = 0;
        for (DWORD c = 0; c < count; ++c) {
            GUID category{};
            if (!guidAt(categories.Get(), c, &category))
                continue;
            ComPtr<IPortableDevicePropVariantCollection> types;
            capabilities->GetSupportedContentTypes(category, &types);
            const DWORD typeCount = countOf(types.Get());
            quint32 here = 0;
            for (DWORD t = 0; t < typeCount; ++t) {
                GUID type{};
                if (!guidAt(types.Get(), t, &type))
                    continue;
                ComPtr<IPortableDevicePropVariantCollection> formats;
                if (failed(capabilities->GetSupportedFormats(type, &formats)))
                    continue;
                const DWORD formatCount = countOf(formats.Get());
                here += formatCount;
                for (DWORD f = 0; f < formatCount; ++f) {
                    GUID format{};
                    if (!guidAt(formats.Get(), f, &format))
                        continue;
                    if (IsEqualGUID(format, target))
                        advertised = true;
                    if (!wpdFormatNamespace(format))
                        continue;
                    const uint code = uint(format.Data1 >> 16);
                    // Below 0x8000 the high word is a Microsoft legacy id and not
                    // an MTP code, so printing it as one would be a lie.
                    if (code < 0x8000)
                        continue;
                    vendorFormats.insert(QString::asprintf("0x%04X ", code)
                                         + guidToText(format));
                }
            }
            total += here;
            lines->append(QString::asprintf("category[%u] = %s contentTypes=%u formats=%u%s",
                                            uint(c), qPrintable(guidToText(category)),
                                            uint(typeCount), here,
                                            IsEqualGUID(category, kMtpExtensions)
                                                ? " (mtp-extensions)" : ""));
        }
        QVector<QString> sorted = vendorFormats.values();
        std::sort(sorted.begin(), sorted.end());
        for (int i = 0; i < sorted.size() && i < 160; ++i)
            lines->append(QStringLiteral("format.vendor = %1").arg(sorted.at(i)));
        lines->append(QString::asprintf("formats.total = %u", total));
    }
    lines->append(QStringLiteral("format.advertised = %1")
                      .arg(advertised ? QStringLiteral("yes") : QStringLiteral("no")));

    // The other door to a playlist: a player whose driver forwards MTP operations
    // could be given the vendor recipe with no format GUID involved at all, so the
    // verb list belongs to the same decision as the format list.
    ComPtr<IPortableDeviceKeyCollection> verbs;
    const HRESULT verbList = capabilities->GetSupportedCommands(&verbs);
    if (failed(verbList) || !verbs) {
        lines->append(QStringLiteral("commands = no answer (%1)").arg(explain(verbList)));
    } else {
        const DWORD verbCount = countOf(verbs.Get());
        for (DWORD i = 0; i < verbCount; ++i) {
            PROPERTYKEY key{};
            if (failed(verbs->GetAt(i, &key)) || !IsEqualGUID(key.fmtid, kMtpExtensions))
                continue;
            const char *name = mtpExtensionVerbName(key.pid);
            lines->append(QString::asprintf("command.mtp-ext = %u ", uint(key.pid))
                          + QString::fromLatin1(name ? name : "unnamed"));
        }
        lines->append(QString::asprintf("commands.total = %u", uint(verbCount)));
    }

    // Which properties the player keeps for this format, and what it says about
    // writing each one.  An unknown format answers this too, which is exactly why
    // the answer is worth reading before a create is attempted.
    ComPtr<IPortableDeviceKeyCollection> keys;
    const HRESULT keyList = capabilities->GetSupportedFormatProperties(target, &keys);
    if (failed(keyList) || !keys) {
        lines->append(QStringLiteral("format.properties = no answer (%1)")
                          .arg(explain(keyList)));
    } else {
        const DWORD keyCount = countOf(keys.Get());
        lines->append(QString::asprintf("format.properties = %u", uint(keyCount)));
        for (DWORD i = 0; i < keyCount; ++i) {
            PROPERTYKEY key{};
            if (failed(keys->GetAt(i, &key)))
                continue;
            ComPtr<IPortableDeviceValues> attributes;
            const HRESULT fixedAttributes =
                capabilities->GetFixedPropertyAttributes(target, key, &attributes);
            lines->append(QStringLiteral("format.property[%1] = %2/%3 %4")
                              .arg(i)
                              .arg(QUuid(key.fmtid).toString().toUpper())
                              .arg(key.pid)
                              .arg(failed(fixedAttributes)
                                       ? explain(fixedAttributes)
                                       : formatAttributeText(attributes.Get())));
        }
    }

    // The reference list is the whole feature, so it gets asked about whether or
    // not the player chose to list it among the format's properties.
    struct Asked {
        const char *name;
        PROPERTYKEY key;
    };
    const Asked wanted[] = {{"object.references", WPD_OBJECT_REFERENCES},
                            {"object.back-references", WPD_OBJECT_BACK_REFERENCES},
                            {"object.name", WPD_OBJECT_NAME},
                            {"object.original-file-name", WPD_OBJECT_ORIGINAL_FILE_NAME},
                            {"object.size", WPD_OBJECT_SIZE},
                            {"object.content-type", WPD_OBJECT_CONTENT_TYPE},
                            {"object.parent-id", WPD_OBJECT_PARENT_ID}};
    for (const Asked &entry : wanted) {
        ComPtr<IPortableDeviceValues> attributes;
        const HRESULT fixedAttributes =
            capabilities->GetFixedPropertyAttributes(target, entry.key, &attributes);
        lines->append(QStringLiteral("fixed.%1 = %2")
                          .arg(QString::fromLatin1(entry.name),
                               failed(fixedAttributes) ? explain(fixedAttributes)
                                                       : formatAttributeText(attributes.Get())));
    }

    // "This format accepts references" and "this object's references may be
    // rewritten" are two answers a player is free to give separately, so when an
    // object is named the same keys are asked about it as well.  The format-wide
    // answers say what the player allows; these say what it kept for one object.
    if (!objectId.isEmpty()) {
        if (!m_content) {
            lines->append(QStringLiteral("object.attributes = no content interface"));
            return true;
        }
        auto *content = static_cast<IPortableDeviceContent *>(m_content);
        ComPtr<IPortableDeviceProperties> properties;
        const HRESULT haveProperties = content->Properties(&properties);
        if (failed(haveProperties) || !properties) {
            lines->append(QStringLiteral("object.attributes = no answer (%1)")
                              .arg(explain(haveProperties)));
            return true;
        }
        const std::wstring wide = toWide(objectId);
        for (const Asked &entry : wanted) {
            ComPtr<IPortableDeviceValues> attributes;
            const HRESULT stated = properties->GetPropertyAttributes(wide.c_str(), entry.key,
                                                                     &attributes);
            lines->append(QStringLiteral("object.%1 = %2")
                              .arg(QString::fromLatin1(entry.name),
                                   failed(stated) ? explain(stated)
                                                  : formatAttributeText(attributes.Get())));
        }
    }
    return true;
}
// Who a playlist object says it contains, read back off the player.
//
// WPD documents WPD_OBJECT_REFERENCES as an interface holding a collection of
// object id strings, so that is the shape asked for.  The collection is then read
// one element at a time rather than trusted, because "the player answered with a
// vector of strings" or "element 3 was a number" are answers to a related question
// and not to this one, and the difference is what a playlist-with-no-songs gets
// blamed for.  A shape this client cannot read is reported as such.
bool Session::objectReferences(const QString &objectId, QStringList *memberIds,
                               QString *error) {
    setReason(error, QString());
    if (memberIds)
        memberIds->clear();
    if (!memberIds) {
        setReason(error, QStringLiteral("No place to put the answer."));
        return false;
    }
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    ComPtr<IPortableDeviceProperties> properties;
    HRESULT hr = content->Properties(&properties);
    if (failed(hr) || !properties) {
        setReason(error, explain(hr));
        return false;
    }
    const ComPtr<IPortableDeviceKeyCollection> one = newKeys();
    if (!one) {
        setReason(error, QStringLiteral("Could not build the request."));
        return false;
    }
    one->Add(WPD_OBJECT_REFERENCES);
    ComPtr<IPortableDeviceValues> values;
    const std::wstring wide = toWide(objectId);
    hr = properties->GetValues(wide.c_str(), one.Get(), &values);
    if (failed(hr) || !values) {
        setReason(error, explain(hr));
        return false;
    }
    PROPVARIANT value{};
    hr = values->GetValue(WPD_OBJECT_REFERENCES, &value);
    if (failed(hr)) {
        PropVariantClear(&value);
        setReason(error, QStringLiteral("The player keeps no member list for object %1 (%2)")
                             .arg(objectId, explain(hr)));
        return false;
    }
    QStringList ids;
    QString note;
    if (value.vt == VT_UNKNOWN && value.punkVal) {
        ComPtr<IPortableDevicePropVariantCollection> collection;
        if (SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&collection))) && collection) {
            DWORD count = 0;
            collection->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                PROPVARIANT item{};
                if (FAILED(collection->GetAt(i, &item)))
                    continue;
                if (item.vt == VT_LPWSTR && item.pwszVal)
                    ids.append(QString::fromWCharArray(item.pwszVal));
                else
                    note = QString::asprintf("element %u came back as vt=0x%04X",
                                             uint(i), uint(item.vt));
                PropVariantClear(&item);
            }
        } else {
            note = QStringLiteral("the answer is an interface that is not a property collection");
        }
    } else if (value.vt == USHORT(VT_VECTOR | VT_LPWSTR)) {
        for (ULONG i = 0; i < value.calpwstr.cElems; ++i)
            ids.append(QString::fromWCharArray(value.calpwstr.pElems[i]
                                                   ? value.calpwstr.pElems[i]
                                                   : L""));
        note = QStringLiteral("answered as a string vector and not a collection");
    } else {
        note = QString::asprintf("vt=0x%04X", uint(value.vt));
    }
    PropVariantClear(&value);
    if (ids.isEmpty() && !note.isEmpty()) {
        setReason(error, QStringLiteral("Object %1 answered the member list in a shape this"
                                        " client does not read: %2")
                             .arg(objectId, note));
        return false;
    }
    *memberIds = ids;
    if (!note.isEmpty()) {
        fprintf(stderr, "[wpd] %s member list: %s\n", qUtf8Printable(objectId),
                qPrintable(note));
    }
    return true;
}

// Creates a playlist: an object with no payload whose entire content is a list of
// other objects.
//
// The recipe the Zune is known to take comes from the libusb line, where it is
// measured on two players: an object in MTP format 0xBA05, named `<name>.zpl`,
// declaring a size it does not have, with its members attached afterwards.  Over
// WPD none of that is written down - the format has no SDK name, and whether a
// reference list may be sent in the create or only after it is a fact about this
// driver.  So the shapes are tried in the order a conformant playlist is described
// and the first one the player takes is recorded as the answer, in the same idiom
// this file already uses to send a file.  Two independent things can go wrong - the
// create can be refused, and the create can be taken and the member list dropped,
// which is a fault this player has already been measured committing with media
// properties - so the members are read back and, if the create kept none of them,
// sent once on their own.
bool Session::makePlaylist(const QString &parentId, const QString &name,
                          const QStringList &memberIds, QString *objectId,
                          QString *error) {
    setReason(error, QString());
    if (objectId)
        objectId->clear();
    if (!m_content) {
        setReason(error, QStringLiteral("Not connected."));
        return false;
    }
    if (name.trimmed().isEmpty()) {
        setReason(error, QStringLiteral("A playlist needs a name."));
        return false;
    }
    if (memberIds.isEmpty()) {
        setReason(error, QStringLiteral("'%1' has no tracks in it, which is not a playlist"
                                        " worth making.").arg(name));
        return false;
    }
    auto *content = static_cast<IPortableDeviceContent *>(m_content);
    const QString parent = parentId.isEmpty() ? deviceRoot() : parentId;
    const std::wstring parentWide = toWide(parent);
    const std::wstring nameWide = toWide(name);
    // The name the player shows and the name the object is filed under are two
    // fields, and the suffix is the only thing a player that has never met this
    // format has to go on.
    const QString fileName = name.endsWith(QLatin1String(".zpl"), Qt::CaseInsensitive)
                                 ? name
                                 : name + QStringLiteral(".zpl");
    const std::wstring fileWide = toWide(fileName);

    const ComPtr<IPortableDevicePropVariantCollection> references = newPropVariantCollection();
    if (!references) {
        setReason(error, QStringLiteral("Could not build the member list."));
        return false;
    }
    for (const QString &member : memberIds) {
        const std::wstring memberWide = toWide(member);
        PROPVARIANT item{};
        if (FAILED(InitPropVariantFromString(memberWide.c_str(), &item)))
            continue;
        if (FAILED(references->Add(&item)))
            PropVariantClear(&item);
    }
    DWORD memberCount = 0;
    references->GetCount(&memberCount);
    if (memberCount == 0) {
        setReason(error, QStringLiteral("None of the %1 requested members could be put in"
                                        " the member list.").arg(memberIds.size()));
        return false;
    }

    struct Shape {
        GUID contentType;
        bool withContentType;
        bool withFormat;
        bool withSize;
        bool withFileName;
        bool withReferences;
        const char *label;
    };
    // References in the create leads: a playlist whose members are attached in a
    // second call spends a moment on the player holding an empty playlist, and if
    // that second call is refused the player keeps a playlist with no songs in it.
    // The album content type is last because it is a different kind of object that
    // would be filed somewhere else, and is here only so that a player which takes
    // nothing else still gets an answer recorded.
    const Shape shapes[] = {
        {WPD_CONTENT_TYPE_PLAYLIST, true, true, true, true, true,
         "playlist/format/size/file name/references"},
        {WPD_CONTENT_TYPE_PLAYLIST, true, true, true, true, false,
         "playlist/format/size/file name"},
        {WPD_CONTENT_TYPE_PLAYLIST, true, true, false, true, false,
         "playlist/format/file name"},
        {WPD_CONTENT_TYPE_PLAYLIST, true, false, false, true, false,
         "playlist/file name"},
        {WPD_CONTENT_TYPE_GENERIC_FILE, true, true, true, true, true,
         "generic file/format/size/file name/references"},
        {WPD_CONTENT_TYPE_GENERIC_FILE, true, true, true, true, false,
         "generic file/format/size/file name"},
        {WPD_CONTENT_TYPE_MIXED_CONTENT_ALBUM, true, true, true, true, false,
         "album/format/size/file name"},
    };
    QString id;
    QString taken;
    bool membersAtCreate = false;
    QString firstFailure;
    for (const Shape &shape : shapes) {
        const ComPtr<IPortableDeviceValues> values = newValues();
        if (!values)
            break;
        values->SetStringValue(WPD_OBJECT_PARENT_ID, parentWide.c_str());
        values->SetStringValue(WPD_OBJECT_NAME, nameWide.c_str());
        if (shape.withFileName)
            values->SetStringValue(WPD_OBJECT_ORIGINAL_FILE_NAME, fileWide.c_str());
        if (shape.withContentType)
            values->SetGuidValue(WPD_OBJECT_CONTENT_TYPE, shape.contentType);
        if (shape.withFormat)
            values->SetGuidValue(WPD_OBJECT_FORMAT, formatGuidFromCode(0xBA05u));
        if (shape.withSize)
            values->SetUnsignedLargeIntegerValue(WPD_OBJECT_SIZE, 0);
        if (shape.withReferences)
            values->SetIPortableDevicePropVariantCollectionValue(WPD_OBJECT_REFERENCES,
                                                                 references.Get());
        LPWSTR created = nullptr;
        const HRESULT hr = content->CreateObjectWithPropertiesOnly(values.Get(), &created);
        const QString made = adopted(created);
        if (SUCCEEDED(hr) && !made.isEmpty()) {
            id = made;
            taken = QString::fromLatin1(shape.label);
            membersAtCreate = shape.withReferences;
            break;
        }
        if (firstFailure.isEmpty())
            firstFailure = QStringLiteral("%1: %2")
                               .arg(QString::fromLatin1(shape.label), explain(hr));
        fprintf(stderr, "[wpd] playlist shape '%s' was refused: %s\n", shape.label,
                qPrintable(explain(hr)));
    }
    if (id.isEmpty()) {
        setReason(error, QStringLiteral("The player refused to create the playlist '%1' in"
                                        " every shape tried.  First answer was %2")
                             .arg(name, firstFailure));
        return false;
    }
    if (objectId)
        *objectId = id;
    fprintf(stderr, "[wpd] playlist '%s' under %s is object %s (shape: %s)\n",
            qUtf8Printable(name), qUtf8Printable(parent), qUtf8Printable(id),
            qPrintable(taken));

    // A create that lands and then fails leaves the player holding an object named
    // like a playlist with nothing in it, which is worse than no playlist: it shows
    // up in the browser, it plays nothing, and the next attempt makes a second one.
    // So every way out that is not a success takes the object back off the player
    // and takes the id back off the caller, measured this session when a create that
    // took no members was left behind by a failure and then deleted again.
    struct ReapUnfinished {
        Session *self;
        QString *out;
        QStringList one;
        QString label;
        bool keep = false;
        ~ReapUnfinished() {
            if (keep || one.isEmpty())
                return;
            if (out)
                out->clear();
            QString why;
            QStringList purged;
            if (self->removeObjects(one, &purged, &why))
                fprintf(stderr, "[wpd] playlist '%s' failed after it was created, so the"
                                " unfinished object %s was taken back off the player\n",
                        qUtf8Printable(label), qUtf8Printable(one.first()));
            else
                fprintf(stderr, "[wpd] playlist '%s' failed after it was created and the"
                                " unfinished object %s stayed on the player: %s\n",
                        qUtf8Printable(label), qUtf8Printable(one.first()),
                        qPrintable(why));
        }
    } reap{this, objectId, QStringList{id}, name};

    // Whether the members landed is a separate question from whether the object
    // did, so it is asked separately.
    QStringList kept;
    QString why;
    if (!objectReferences(id, &kept, &why)) {
        kept.clear();
        fprintf(stderr, "[wpd] the new playlist has no readable member list: %s\n",
                qPrintable(why));
    }
    if (kept.size() == memberIds.size()) {
        fprintf(stderr, "[wpd] playlist %s holds all %d members\n", qUtf8Printable(id),
                int(memberIds.size()));
        reap.keep = true;
        return true;
    }
    fprintf(stderr, "[wpd] playlist %s kept %lld of %d members%s - sending the list on its"
                    " own\n", qUtf8Printable(id), qlonglong(kept.size()),
            int(memberIds.size()),
            membersAtCreate ? " even though the create was given them" : "");

    ComPtr<IPortableDeviceProperties> properties;
    HRESULT hr = content->Properties(&properties);
    if (failed(hr) || !properties) {
        setReason(error, QStringLiteral("'%1' was created as object %2 but the player would"
                                        " not let its members be read or written: %3")
                             .arg(name, id, explain(hr)));
        return false;
    }
    const ComPtr<IPortableDeviceValues> again = newValues();
    if (!again) {
        setReason(error, QStringLiteral("Could not build the member list for object %1.")
                             .arg(id));
        return false;
    }
    again->SetIPortableDevicePropVariantCollectionValue(WPD_OBJECT_REFERENCES,
                                                        references.Get());
    const std::wstring idWide = toWide(id);
    ComPtr<IPortableDeviceValues> refused;
    hr = properties->SetValues(idWide.c_str(), again.Get(), &refused);
    if (refused)
        fprintf(stderr, "[wpd] the player refused %lu of the playlist properties\n",
                countOf(refused.Get()));
    else if (FAILED(hr))
        fprintf(stderr, "[wpd] playlist member list answer: %s\n",
                qPrintable(explain(hr)));
    if (failed(hr)) {
        setReason(error, QStringLiteral("'%1' was created as object %2 but the player"
                                        " refused its member list: %3")
                             .arg(name, id, explain(hr)));
        return false;
    }
    if (!objectReferences(id, &kept, &why)) {
        setReason(error, QStringLiteral("'%1' was created as object %2, its member list was"
                                        " accepted, and then the player would not read it"
                                        " back: %3")
                             .arg(name, id, why));
        return false;
    }
    if (kept.size() != memberIds.size()) {
        setReason(error, QStringLiteral("'%1' was created as object %2 and the player keeps"
                                        " %3 of its %4 tracks.")
                             .arg(name, id).arg(kept.size()).arg(memberIds.size()));
        return false;
    }
    fprintf(stderr, "[wpd] playlist %s holds all %d members after SetValues\n",
            qUtf8Printable(id), int(memberIds.size()));
    reap.keep = true;
    return true;
}
bool Session::download(const QString &objectId, const QString &destPath, ProgressFn fn,
                       void *userdata, QString *error, quint64 limitBytes) {
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

    // A cap is measured against the object's declared size so that asking for
    // the front of a small object still reads all of it, and so that the
    // readback check below judges the bytes that were actually asked for.
    const quint64 cap = limitBytes && (total == 0 || limitBytes < total) ? limitBytes : 0;

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
        ULONG want = ULONG(chunk.size());
        if (cap && written + want > cap)
            want = ULONG(cap - written);
        if (want == 0)
            break;
        ULONG got = 0;
        hr = stream->Read(chunk.data(), want, &got);
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
        if (cap && written >= cap)
            break;
        if (fn && total && !cap)
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
    if (!cap && total && written != total) {
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
    fprintf(stderr, "[wpd] %s %s -> %s (%llu bytes%s)\n",
            cap ? "head-read" : "downloaded", qUtf8Printable(objectId),
            qUtf8Printable(destPath), quint64(written),
            cap ? ", capped" : "");
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
    const int announced = formatForExtension(srcPath);
    GUID format = WPD_OBJECT_FORMAT_UNSPECIFIED;
    switch (announced) {
    case 1: format = WPD_OBJECT_FORMAT_WMA; break;
    case 2: format = WPD_OBJECT_FORMAT_MP3; break;
    case 3: format = WPD_OBJECT_FORMAT_FLAC; break;
    case 4: format = WPD_OBJECT_FORMAT_WAVE; break;
    case 5: format = WPD_OBJECT_FORMAT_MP4; break;
    case 6: format = WPD_OBJECT_FORMAT_ASF; break;
    default: break;
    }
    const bool video = isVideoFormat(announced);

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
    const Shape audioShapes[] = {
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
    // A video is the same transfer with one different word in it.  WPD tells a
    // player what an object *is* through WPD_OBJECT_CONTENT_TYPE, so an .mp4
    // announced as audio is filed as music however well it decodes.  The rungs are
    // the same because what a given player needs in the create is a fact about the
    // player, not about the kind of file.  Whether this player then files a video
    // it was given this way in its own video library is a separate question, and
    // this table makes no claim about its answer.
    const Shape videoShapes[] = {
        {WPD_CONTENT_TYPE_VIDEO, true, true, true, true, true,
         "video/format/size/file name/media"},
        {WPD_CONTENT_TYPE_VIDEO, true, true, true, true, false,
         "video/format/size/file name"},
        {WPD_CONTENT_TYPE_GENERIC_FILE, true, true, true, true, true,
         "generic file/format/size/name/media"},
        {WPD_CONTENT_TYPE_GENERIC_FILE, true, true, true, true, false,
         "generic file/format/size/name"},
        {WPD_CONTENT_TYPE_VIDEO, true, true, true, false, false, "video/format/size"},
        {WPD_CONTENT_TYPE_VIDEO, true, false, true, true, true,
         "video/size/file name/media"},
        {WPD_CONTENT_TYPE_VIDEO, true, true, false, true, true,
         "video/format/file name/media"},
        {noContentType, false, true, true, true, true, "format/size/file name/media"},
    };
    const Shape *shapes = audioShapes;
    int shapeCount = int(sizeof(audioShapes) / sizeof(audioShapes[0]));
    if (video) {
        shapes = videoShapes;
        shapeCount = int(sizeof(videoShapes) / sizeof(videoShapes[0]));
    }
    int &whichShape = video ? m_uploadVideoShape : m_uploadShape;

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
        if (whichShape >= 0 && index != whichShape)
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
            whichShape = index;
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
        if (whichShape >= 0)
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
    case KindPlaylist: return QStringLiteral("playlist");
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
    // The two containers this application produces video into: H.264/AAC in
    // MP4 for a player that takes that shape, Windows Media in ASF for one that
    // takes that instead.  This says only what we announce; whether a given
    // player plays what it accepted is answered by playback, not here.
    if (suffix == QLatin1String("mp4") || suffix == QLatin1String("m4v"))
        return 5;
    if (suffix == QLatin1String("wmv") || suffix == QLatin1String("asf"))
        return 6;
    return 0;
}

// 5 and 6, spelled once so the shape table and the caller of the shape cache
// cannot drift apart about which uploads are video uploads.
bool Session::isVideoFormat(int format) {
    return format == 5 || format == 6;
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
