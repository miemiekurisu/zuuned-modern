// CLI gate for the presence layer: what libusb can see, and what the operating
// system says about a Zune that libusb cannot see.
//
// On Windows these are two different questions with two different answers, and
// the app's whole "waiting for a zune..." behaviour depends on which one it is
// asking.  A window cannot be tested from a script; this can.  No profile, no
// QML, no device access beyond reading the bus.
//
// VERDICT=agree    libusb can open a Zune, and the platform agrees it is there
// VERDICT=blocked  the platform sees a Zune that libusb cannot reach (exit 0:
//                  this is the expected first run on Windows, not a failure)
// VERDICT=absent   neither source sees a Zune
// VERDICT=contradictory / =unknown  the sources disagree, or the bus could not
//                  be read at all -> exit 1/2 so a gate stops here
#if defined(_WIN32)
/* The console renders the active ANSI code page, so every UTF-8 path and device
 * string printed below would arrive to the operator as '?'.  This tool uses none
 * of the POSIX surface, so only the console helper is declared here rather than
 * forcing compat/win/wincompat.h through the Qt and vendor headers. */
extern "C" void zuuned_win_console_utf8(void);
#endif

#include "DevicePresence.h"

#include <QCoreApplication>
#include <QString>
#include <libusb.h>

#include <cstdio>

namespace {

struct LibusbView {
    bool enumerated = false;
    bool sighted = false;
    int devices = 0;
    int hotplug = 0;
};

LibusbView readLibusb() {
    LibusbView view;
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0)
        return view;
    view.hotplug = libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) ? 1 : 0;
    libusb_device **devices = nullptr;
    const ssize_t found = libusb_get_device_list(ctx, &devices);
    if (found >= 0) {
        view.enumerated = true;
        view.devices = static_cast<int>(found);
        for (ssize_t i = 0; i < found; ++i) {
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(devices[i], &desc) != 0)
                continue;
            if (Zuuned::isZuneUsbId(desc.idVendor, desc.idProduct))
                view.sighted = true;
        }
        libusb_free_device_list(devices, 1);
    }
    libusb_exit(ctx);
    return view;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
#if defined(_WIN32)
    zuuned_win_console_utf8();
#endif
    const LibusbView usb = readLibusb();
    const Zuuned::ZuneSighting os = Zuuned::scanForZune();

    printf("libusb_enumerated=%d libusb_devices=%d libusb_hotplug=%d libusb_sighted=%d\n",
           usb.enumerated ? 1 : 0, usb.devices, usb.hotplug, usb.sighted ? 1 : 0);
    printf("os_supported=%d os_present=%d os_usable=%d os_problem=%u\n",
           os.supported ? 1 : 0, os.present ? 1 : 0, os.usable ? 1 : 0, os.problem);
    printf("os_driver=%s os_wpd_interface=%d\n", os.driver.toUtf8().constData(),
           os.wpdInterface ? 1 : 0);
    if (!os.instanceId.isEmpty())
        printf("os_instance=%s\n", os.instanceId.toUtf8().constData());
    const QString detail = Zuuned::describeSighting(os, usb.sighted);
    if (!detail.isEmpty())
        printf("os_saying=%s\n", detail.toUtf8().constData());

    if (!usb.enumerated) {
        printf("VERDICT=unknown libusb could not enumerate the bus\n");
        return 2;
    }
    // Where the platform declines to answer (Linux: libusb is authoritative
    // there), the only available answer is libusb's own, so agreement follows.
    if (!os.supported) {
        printf("VERDICT=%s platform scan declines to answer\n",
               usb.sighted ? "agree" : "absent");
        return 0;
    }
    if (usb.sighted && !os.present) {
        printf("VERDICT=contradictory libusb opened a device the platform says is gone\n");
        return 1;
    }
    if (usb.sighted) {
        printf("VERDICT=agree\n");
        return 0;
    }
    if (os.present) {
        // libusb is blind and the devnode is real.  Since the WPD backend that is
        // the second road to the same player, that pair no longer means blocked
        // on its own - which driver Windows bound decides it, and the caller
        // should see the difference rather than infer it from a problem code.
        if (Zuuned::zuunedCanReach(os, false)) {
            printf("VERDICT=agree via=%s\n", os.driver.toUtf8().constData());
            return 0;
        }
        printf("VERDICT=blocked\n");
        return 0;
    }
    printf("VERDICT=absent\n");
    return 0;
}
