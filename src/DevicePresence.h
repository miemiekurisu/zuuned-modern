#pragma once

#include <QString>

// What the operating system itself says about a plugged-in Zune, independent of
// whether libusb can use it.
//
// The distinction only bites on Windows.  libusb's Windows backend publishes a
// device through the device interface its driver installs, so a Zune that
// Windows has never given a driver to is not merely unopenable - it is absent
// from libusb_get_device_list().  Measured with libusb 1.0.29 on the machine
// this port runs on: seven enumerated devices, LIBUSB_CAP_HAS_HOTPLUG 0, and the
// Zune HD sitting on the bus was not one of the seven.  A poll of libusb alone
// therefore cannot tell "nothing is plugged in" from "your Zune is plugged in
// and Windows has no driver for it", which is the single most common first run
// on this platform.  The devnode answers it.
//
// On Linux a Zune is visible to libusb as soon as it is enumerated, so the
// watcher's existing polling is authoritative there and this scan declines to
// answer (supported = false) rather than adding a second, weaker presence signal.
namespace Zuuned {

struct ZuneSighting {
    bool supported = false;   // can this platform answer the question at all?
    bool present = false;     // a live Zune devnode exists right now
    // Windows believes a driver owns it (problem code 0).  That is NOT proof
    // libusb can open it: a Zune handed to Windows' own WPD/MTP driver reads
    // problem 0 and still never appears in libusb_get_device_list().  The fact
    // the app can act on is the pair (present, libusb blind), not this flag.
    bool usable = false;
    // The kernel driver Windows bound to the devnode, lower-cased, as
    // CM_DRP_SERVICE names it.  Diagnostic only: the one name this project has
    // actually measured on a working Zune is `wudfwpdmtp`, the UMDF host for
    // Windows' own MTP minidriver, and nothing here decides a road by this
    // string.  Always empty on a platform that declines to answer.
    QString driver;
    // Whether Windows publishes a Windows Portable Devices interface for this
    // devnode, which is the door the WPD backend walks.  A capability read from
    // the same list IPortableDeviceManager enumerates, not a guess from a driver's
    // name.  Always false on a platform that has no such list.
    bool wpdInterface = false;
    quint16 vid = 0;
    quint16 pid = 0;
    QString instanceId;       // platform device id, e.g. USB\VID_045E&PID_063E\...
    quint32 problem = 0;      // platform problem code, 0 = none
    QString problemText;      // short human wording for `problem`
};

// 0x0710 = Zune 4/8/16/30/80/120, 0x063e = Zune HD.  0x0711-0x0714 appear in
// recovery/other modes (the zune_usb DKMS module matches them too) - accept them
// so presence at least can be shown.  One definition, used by the libusb watcher
// and by the platform scan alike.
bool isZuneUsbId(quint16 vid, quint16 pid);

// Ask the platform about a Zune.  Cheap enough for a leisurely poll on Windows;
// never blocks on the device.
ZuneSighting scanForZune();

// One log-ready sentence about a sighting, with no [prefix].  Empty when the
// platform declines to answer or saw nothing.  `libusbSighted` is the watcher's
// own enumeration result, because the honest wording differs: a device with no
// driver at all and a device bound to a driver zuuned cannot use are two
// different things to tell a user about, and only the second has problem 0.
QString describeSighting(const ZuneSighting &sighting, bool libusbSighted);

// Whether zuuned has a road to a player the platform swears is attached, given
// what libusb itself managed to enumerate.  Two roads exist on Windows and one
// everywhere else, and "libusb cannot see it" stopped being the same sentence as
// "zuuned cannot use it" the day the second one was wired up.  The second road is
// answered by the WPD device interface, because that is the object the WPD backend
// binds - a driver's name is Windows' implementation detail and two Zunes on two
// Windows builds do not have to agree on it.
bool zuunedCanReach(const ZuneSighting &sighting, bool libusbSighted);

} // namespace Zuuned
