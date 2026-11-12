// Platform-neutral half of the presence layer: the USB id set and the wording.
// The scan itself is per-platform (DevicePresenceWin / DevicePresenceUnix).

#include "DevicePresence.h"
#include "UiText.h"

namespace Zuuned {

bool isZuneUsbId(quint16 vid, quint16 pid) {
    return vid == 0x045e
           && (pid == 0x0710 || pid == 0x063e || (pid >= 0x0711 && pid <= 0x0714));
}

QString describeSighting(const ZuneSighting &s, bool libusbSighted) {
    if (!s.supported || !s.present)
        return QString();
    const QString ids = QStringLiteral("pid=0x%1")
                            .arg(s.pid, 4, 16, QLatin1Char('0'));
    if (libusbSighted)
        return UiText::tr("Zune %1 is on the bus and libusb can see it (%2)")
            .arg(ids, s.instanceId);
    if (!s.usable)
        return UiText::tr("Zune %1 is on the bus, invisible to libusb, and Windows has no driver for it (problem %2: %3) - %4")
            .arg(ids)
            .arg(s.problem)
            .arg(s.problemText, s.instanceId);
    // Problem 0 with libusb blind used to have exactly one reading: Windows gave
    // the player to WPD and zuuned was locked out.  It now has two.  Windows'
    // MediaDevice driver is the second road src/wpd/WpdBackend.h walks, and
    // WinUSB is the first, so the sentence has to say which situation the user is
    // looking at rather than assuming the fixable one.
    if (zuunedCanReach(s, libusbSighted))
        return UiText::tr("Zune %1 is on the bus, invisible to libusb, and Windows drives it in a way zuuned can use (%2)")
                .arg(ids, s.instanceId);
    return UiText::tr("Zune %1 is on the bus, invisible to libusb, and Windows says a driver owns it - so what owns it is not one zuuned can use (%2)")
            .arg(ids, s.instanceId);
}

bool zuunedCanReach(const ZuneSighting &sighting, bool libusbSighted) {
    if (!sighting.present)
        return false;
    if (libusbSighted)
        return true;
    // The second road, and the only one that exists when libusb is blind: the
    // player answered Windows as a portable device, so src/wpd/WpdBackend.cpp can
    // ask it the same questions.
    //
    // A driver name is deliberately not part of this.  The name measured here is
    // `wudfwpdmtp`, and the name that would have been guessed from the WPD docs
    // before anyone measured is not that; a whitelist built the second way would
    // tell a user on the ordinary Windows configuration to go install a driver
    // that zuuned does not need.  Nor is "bound to WinUSB" a road of its own: a
    // WinUSB player is enumerated by libusb, so if libusb did not list it there is
    // no handle to open at the end of that assumption either.
    return sighting.wpdInterface;
}

} // namespace Zuuned
