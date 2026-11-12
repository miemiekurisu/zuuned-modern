// Windows answers "is a Zune plugged in" through the devnode, because libusb
// cannot: with no driver bound there is no device interface for libusb to find,
// so the device is invisible to libusb_get_device_list() while the PnP manager
// still holds it with a problem code.  Verified on the machine this port runs on
// with a Zune HD attached: CM problem 28 ("no driver installed"),
// DN_HAS_PROBLEM set, and no libusb entry for it at all.
//
// Two traps this walks around.  CM_Get_Device_ID_List() also returns records for
// hardware that is not attached any more - a Zune 30 that was plugged in weeks
// ago answers CM_Locate_DevNode(CM_LOCATE_DEVNODE_NORMAL) with
// CR_NO_SUCH_DEVNODE - so a record is not a presence and gets located before it
// counts.  And the ids come back in whatever case the enumerator wrote them, so
// the vid/pid match is case-insensitive.

#include "DevicePresence.h"

#include <windows.h>
#include <cfgmgr32.h>

#include <cwchar>
#include <vector>
#include "UiText.h"

namespace {

QString problemTextFor(ULONG problem) {
    switch (problem) {
    case 0:  return UiText::tr("no problem");
    case 1:  return UiText::tr("the device is disabled");
    case 10: return UiText::tr("Windows cannot start the hardware");
    case 14: return UiText::tr("a reboot is required");
    case 18: return UiText::tr("the drivers need to be reinstalled");
    case 21: return UiText::tr("Windows is reinstalling the device");
    case 22: return UiText::tr("the device is disabled or gone");
    case 28: return UiText::tr("no driver is installed");
    case 31: return UiText::tr("the device is not working properly");
    case 37: return UiText::tr("a filter driver failed to load");
    case 38: return UiText::tr("the driver failed to load");
    case 39: return UiText::tr("the driver is corrupt or missing");
    case 45: return UiText::tr("it is not currently connected");
    case 48: return UiText::tr("the software for it cannot load");
    default: return QStringLiteral("problem code %1").arg(problem);
    }
}

// A devnode that Windows is keeping a record for, but which is not on the bus
// now, must not read as "plugged in".
bool countsAsAttached(ULONG problem) {
    return problem != 22 /* CM_PROB_PHANTOM */
           && problem != 45 /* CM_PROB_NOT_CONNECTED */;
}

// Which kernel driver owns the devnode.  Read from the same place Device
// Parameters is read - the devnode's own registry key - because the device
// interface list answers a different question: it names the interfaces a driver
// publishes, and a WPD player publishes ones libusb does not look for.
QString driverOf(DEVINST devNode) {
    WCHAR buffer[256] = {};
    ULONG length = sizeof(buffer);
    if (CM_Get_DevNode_Registry_PropertyW(devNode, CM_DRP_SERVICE, nullptr,
                                         buffer, &length, 0) != CR_SUCCESS)
        return QString();
    return QString::fromWCharArray(buffer).toLower();
}

// Whether Windows publishes a Windows Portable Devices interface for this
// instance.  That interface is the road src/wpd/WpdBackend.cpp travels: the WPD
// enumerator answers from exactly this device-interface class, so one filtered
// call here asks, without COM and without a devnode handle to the WPD driver, the
// same question the backend is about to ask.
//
// The instance id is passed as the list's filter, which is what the API is for -
// an unfiltered list plus a mapping per link would cost a call per phone on the
// machine.  Measured on the Zune HD this port runs on while it sat on Windows'
// own MTP driver: this list answers one link for it and no link for a player
// bound to nothing, which is the pair of answers that separates "use the other
// road" from "install a driver".
bool publishesWpdInterface(const QString &instanceId) {
    // GUID_DEVINTERFACE_WPD, spelled here rather than read from functionsteps.h:
    // one constant is not worth a header-version dependency in a file the Linux
    // build never compiles.
    static const GUID kWpdInterfaceClass = {
        0x6ac27878, 0xa6fa, 0x4155,
        {0xba, 0x85, 0xf9, 0x8f, 0x49, 0x1d, 0x4f, 0x33}};
    PWSTR id = const_cast<PWSTR>(
        reinterpret_cast<const WCHAR *>(instanceId.utf16()));

    // The API wants a non-const GUID pointer for a value it only reads.
    LPGUID iface = const_cast<LPGUID>(&kWpdInterfaceClass);
    // The list call does not report the room it wants - it only refuses a buffer
    // that is too small - so the size comes from its own sibling.  Writing this
    // against the list call alone was measured answering "no interface" for a
    // player that publishes one: the probe left the size at zero and every
    // answer became false.
    ULONG needed = 0;
    if (CM_Get_Device_Interface_List_SizeW(
            &needed, iface, id,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return false;
    // A length of one is the lone double-null terminator: a devnode that
    // publishes no interface of this class.
    if (needed <= 1)
        return false;

    std::vector<WCHAR> buffer(size_t(needed), WCHAR(0));
    if (CM_Get_Device_Interface_ListW(iface, id, buffer.data(), needed,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return false;
    // A double-null list with room to spare means at least one interface is
    // present; an empty answer is a lone terminator.
    return buffer[0] != WCHAR(0);
}

bool parseIds(const QString &instanceId, quint16 &vid, quint16 &pid) {
    const QString upper = instanceId.toUpper();
    const int v = upper.indexOf(QStringLiteral("VID_"));
    const int p = upper.indexOf(QStringLiteral("PID_"));
    if (v < 0 || p < 0)
        return false;
    bool vidOk = false;
    bool pidOk = false;
    const quint64 vFound = upper.mid(v + 4, 4).toULongLong(&vidOk, 16);
    const quint64 pFound = upper.mid(p + 4, 4).toULongLong(&pidOk, 16);
    if (!vidOk || !pidOk)
        return false;
    vid = static_cast<quint16>(vFound);
    pid = static_cast<quint16>(pFound);
    return true;
}

} // namespace

namespace Zuuned {

ZuneSighting scanForZune() {
    ZuneSighting best;
    best.supported = true;

    ULONG needed = 0;
    if (CM_Get_Device_ID_List_SizeW(&needed, L"USB", 0) != CR_SUCCESS || needed < 2)
        return best;

    std::vector<WCHAR> buffer(needed);
    if (CM_Get_Device_ID_ListW(L"USB", buffer.data(), needed, 0) != CR_SUCCESS)
        return best;

    for (const WCHAR *cursor = buffer.data(); *cursor;) {
        const QString instanceId = QString::fromWCharArray(cursor);
        cursor += std::wcslen(cursor) + 1;

        quint16 vid = 0;
        quint16 pid = 0;
        if (!parseIds(instanceId, vid, pid) || !isZuneUsbId(vid, pid))
            continue;

        DEVINST devNode = 0;
        if (CM_Locate_DevNodeW(&devNode,
                               const_cast<PWSTR>(
                                   reinterpret_cast<const WCHAR *>(instanceId.utf16())),
                               CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue; // a record, not a device on the bus

        ULONG status = 0;
        ULONG problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, devNode, 0) != CR_SUCCESS)
            continue;
        if (!countsAsAttached(problem))
            continue;

        ZuneSighting sighting;
        sighting.supported = true;
        sighting.present = true;
        sighting.usable = problem == 0;
        sighting.driver = driverOf(devNode);
        sighting.wpdInterface = publishesWpdInterface(instanceId);
        sighting.vid = vid;
        sighting.pid = pid;
        sighting.instanceId = instanceId;
        sighting.problem = problem;
        sighting.problemText = problemTextFor(problem);
        (void)status;

        // Prefer a device Windows can actually use, then the most informative
        // blocked one, so a machine with an old recovery-mode node still reports
        // the Zune that matters.
        if (!best.present || (!best.usable && sighting.usable))
            best = sighting;
    }

    if (!best.present)
        best.problemText.clear();
    return best;
}

} // namespace Zuuned
