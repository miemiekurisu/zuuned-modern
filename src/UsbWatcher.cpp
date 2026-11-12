#include "UsbWatcher.h"

#include "DevicePresence.h"

#include <libusb.h>
#include <cstdio>

namespace {
constexpr uint16_t kZuneVid = 0x045e;

bool isZunePid(uint16_t pid) {
    // One id set for the whole app, shared with the platform presence scan.
    return Zuuned::isZuneUsbId(kZuneVid, pid);
}

// A libusb build without OS hotplug notifications still has to notice a Zune
// that appears or disappears.  Windows is the case that matters: since 1.0.24
// its backend enumerates on demand -- windows_common.c installs
// get_device_list with a NULL hotplug_poll -- which keeps
// LIBUSB_CAP_HAS_HOTPLUG permanently 0 there and leaves
// libusb_hotplug_register_callback() able to answer only NOT_SUPPORTED.
// Diffing a leisurely enumeration is the portable stand-in.  The signals carry
// presence rather than identity, so nothing has to be tracked between passes
// except the last confirmed answer.
//
// An enumeration that fails is an unknown, never an absence: onZuneLeft()
// severs a live worker, so a single flaky pass must not tear down a transfer.
// Even a confirmed absence waits for a second opinion half a second later.
void pollPresence(libusb_context *ctx, const std::atomic<bool> &stop,
                  UsbWatcher *self) {
    constexpr int kIntervalMs = 500;
    constexpr int kAbsencesBeforeLeft = 2;

    fprintf(stderr, "[usbwatcher] polling for Zune every %d ms (vid=0x%04x)\n",
            kIntervalMs, kZuneVid);

    bool present = false;
    bool sighted = false;
    int absences = 0;
    QString blockedDetail;   // what the platform last said about it; empty = nothing
#ifdef Q_OS_WIN
    // Every fourth pass is every two seconds: fast enough to notice a replug,
    // slow enough to stay off the enumeration path.  Counted separately because
    // only the passes where libusb saw nothing reach the platform scan.
    constexpr int kPresenceScanEvery = 4;
    unsigned passes = 0;
    // Whether the platform scan has an unclaimed arrival outstanding.  libusb is
    // blind to a player Windows kept for its own MediaDevice driver, so the
    // arrival and the departure of that player exist only in this loop, and
    // `present` alone cannot carry them: it is libusb's own state and would stay
    // false for the whole lifetime of a session the user can see working.
    bool platformPresent = false;
#endif
    while (!stop) {
        libusb_device **devices = nullptr;
        const ssize_t found = libusb_get_device_list(ctx, &devices);
        if (found >= 0) {
            sighted = false;
            for (ssize_t i = 0; i < found && !sighted; ++i) {
                libusb_device_descriptor desc{};
                if (libusb_get_device_descriptor(devices[i], &desc) != 0)
                    continue;
                sighted = desc.idVendor == kZuneVid && isZunePid(desc.idProduct);
            }
            libusb_free_device_list(devices, 1);
            absences = sighted ? 0 : absences + 1;
            if (sighted && !present) {
                present = true;
                fprintf(stderr, "[usbwatcher] Zune arrived (poll)\n");
                emit self->zuneArrived();
                if (!blockedDetail.isEmpty()) {
                    blockedDetail.clear();
                    emit self->zuneUnblocked();
                }
            } else if (absences >= kAbsencesBeforeLeft && present) {
                present = false;
                fprintf(stderr, "[usbwatcher] Zune left (poll)\n");
                emit self->zuneLeft();
            }
        }

#ifdef Q_OS_WIN
        // libusb's silence here says nothing about the device, so ask Windows,
        // and speak up only when the answer changes.  A Zune that gains a driver
        // becomes visible to libusb on its own, which clears this through the
        // arrival path above; a two-second cadence is fast enough to notice a
        // replug and slow enough to stay off the enumeration path.
        if (!sighted && ++passes % kPresenceScanEvery == 0) {
            const Zuuned::ZuneSighting sighting = Zuuned::scanForZune();
            const bool reachable = Zuuned::zuunedCanReach(sighting, sighted);
            const QString detail = Zuuned::describeSighting(sighting, sighted);
            // An access problem the user can act on, and nothing else.  A player
            // on Windows' MTP driver is not blocked: DeviceWorker reaches it
            // through the WPD backend, so saying otherwise would tell the user to
            // go install a driver for no reason.
            const QString blocked = reachable ? QString() : detail;
            if (blocked != blockedDetail) {
                blockedDetail = blocked;
                if (!blockedDetail.isEmpty()) {
                    fprintf(stderr, "[usbwatcher] %s\n", qUtf8Printable(blockedDetail));
                    emit self->zuneBlocked(blockedDetail);
                } else {
                    fprintf(stderr, "[usbwatcher] the platform no longer reports a "
                                    "Zune zuuned cannot reach\n");
                    emit self->zuneUnblocked();
                }
            }
            // Presence for the road libusb cannot see.  Emitted after the block
            // state above settles, because a device that changed roads without
            // leaving the bus must not be reported gone on its way in.
            if (reachable && !platformPresent) {
                platformPresent = true;
                present = true;
                absences = 0;
                fprintf(stderr, "[usbwatcher] Zune arrived on a driver zuuned "
                                "can use (%s)\n",
                        sighting.driver.toUtf8().constData());
                emit self->zuneArrived();
            } else if (!sighting.present && platformPresent) {
                platformPresent = false;
                present = false;
                fprintf(stderr, "[usbwatcher] Zune left (platform scan)\n");
                emit self->zuneLeft();
            }
        }
#endif

        for (int waited = 0; waited < kIntervalMs && !stop; waited += 50)
            QThread::msleep(50);
    }
}
} // namespace

struct UsbWatcherThunk {
    static int LIBUSB_CALL hotplug(libusb_context *, libusb_device *device,
                                   libusb_hotplug_event event, void *user) {
        auto *self = static_cast<UsbWatcher *>(user);
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(device, &desc) != 0)
            return 0;
        if (desc.idVendor != kZuneVid || !isZunePid(desc.idProduct))
            return 0;

        if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
            fprintf(stderr, "[usbwatcher] Zune arrived (pid=0x%04x)\n", desc.idProduct);
            emit self->zuneArrived();
        } else {
            fprintf(stderr, "[usbwatcher] Zune left\n");
            emit self->zuneLeft();
        }
        return 0; // keep the callback registered
    }
};

UsbWatcher::UsbWatcher(QObject *parent) : QThread(parent) {}

UsbWatcher::~UsbWatcher() {
    stop();
}

void UsbWatcher::stop() {
    m_stop = true;
    if (!wait(3000)) {
        fprintf(stderr, "[usbwatcher] event loop did not stop, terminating\n");
        terminate();
        wait(1000);
    }
}

void UsbWatcher::run() {
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "[usbwatcher] libusb_init failed — no hotplug\n");
        return;
    }

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        // No OS hotplug notifications in this libusb build, so watch for the
        // Zune by enumerating instead.  See pollPresence().
        pollPresence(ctx, m_stop, this);
        libusb_exit(ctx);
        return;
    }

    libusb_hotplug_callback_handle handle = 0;
    int rc = libusb_hotplug_register_callback(
        ctx,
        static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                          LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
        LIBUSB_HOTPLUG_ENUMERATE,
        kZuneVid, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
        &UsbWatcherThunk::hotplug, this, &handle);
    if (rc != 0) {
        fprintf(stderr, "[usbwatcher] hotplug registration failed: %s\n",
                libusb_strerror(rc));
        libusb_exit(ctx);
        return;
    }

    fprintf(stderr, "[usbwatcher] watching for Zune (vid=0x%04x)\n", kZuneVid);

    while (!m_stop) {
        timeval tv{0, 250000}; // 250 ms tick so stop() stays responsive
        libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
    }

    libusb_hotplug_deregister_callback(ctx, handle);
    libusb_exit(ctx);
}
