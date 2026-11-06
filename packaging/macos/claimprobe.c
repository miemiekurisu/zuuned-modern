/*
 * The macOS answer to the question `packaging/windows/check-driver.ps1` answers on
 * Windows: can userspace actually own this player's USB interface?
 *
 * macOS ships no driver for a Zune and needs none - the point of this probe is to
 * measure that rather than assume it.  It opens the first 045e:063e device libusb
 * enumerates, asks whether the kernel holds interface 0, prints the endpoint set,
 * then claims and releases that interface.  No PTP traffic, no bulk I/O, so it
 * cannot corrupt anything; it can only fail to claim.
 *
 * Run it outside any sandbox.  A seatbelt/sandboxed shell denies USB and libusb
 * enumerates zero devices, which looks exactly like an absent player and is not one.
 *
 *   cc -o /tmp/claimprobe packaging/macos/claimprobe.c \
 *      $(pkg-config --cflags --libs libusb-1.0)
 *   /tmp/claimprobe
 *
 * `src/devicetool` is the committed tool for the presence half of this question and
 * answers on all three platforms; this file is for the platform-specific half it does
 * not answer, which is the claim.  VERDICT lines are the contract:
 * `claim_ok`, `claim_failed`, `open_failed`, `target_absent`, `no_libusb`.
 */
#include <libusb.h>
#include <stdio.h>

#define ZUNE_VID 0x045e
#define PAVO_PID 0x063e

int main(void) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) {
        printf("VERDICT=no_libusb\n");
        return 2;
    }

    const struct libusb_version *v = libusb_get_version();
    printf("libusb=%u.%u.%u%s hotplug=%d\n", v->major, v->minor, v->micro,
           v->rc ? v->rc : "", libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG));

    libusb_device **list = NULL;
    int n = libusb_get_device_list(ctx, &list);
    if (n < 0) {
        printf("enumerated_error=%d (%s)\n", n, libusb_strerror(n));
        printf("VERDICT=no_libusb\n");
        libusb_exit(ctx);
        return 2;
    }
    printf("enumerated=%d\n", n);

    libusb_device *target = NULL;
    for (int i = 0; i < n; ++i) {
        struct libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0)
            continue;
        if (d.idVendor != ZUNE_VID)
            continue;
        printf("  sighted bus=%d addr=%d %04x:%04x class=%02x ncfg=%d\n",
               libusb_get_bus_number(list[i]), libusb_get_device_address(list[i]),
               d.idVendor, d.idProduct, d.bDeviceClass, d.bNumConfigurations);
        if (d.idProduct == PAVO_PID && !target)
            target = list[i];
    }
    if (!target) {
        printf("VERDICT=target_absent\n");
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return 1;
    }

    libusb_device_handle *h = NULL;
    int r = libusb_open(target, &h);
    printf("open=%d (%s)\n", r, libusb_strerror(r));
    if (r != 0) {
        printf("VERDICT=open_failed\n");
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return 1;
    }

    int active = libusb_kernel_driver_active(h, 0);
    printf("kernel_driver_active(0)=%d (%s)\n", active, libusb_strerror(active));

    struct libusb_config_descriptor *cfg = NULL;
    r = libusb_get_active_config_descriptor(target, &cfg);
    printf("active_config=%d (%s)\n", r, libusb_strerror(r));
    if (r == 0) {
        printf("  config %d: interfaces=%d maxpower=%dmA\n", cfg->bConfigurationValue,
               cfg->bNumInterfaces, cfg->MaxPower);
        for (int i = 0; i < cfg->bNumInterfaces; ++i) {
            const struct libusb_interface *in = &cfg->interface[i];
            for (int a = 0; a < in->num_altsetting; ++a) {
                const struct libusb_interface_descriptor *d = &in->altsetting[a];
                printf("  iface %d alt%d class=%02x/%02x/%02x eps=%d\n", d->bInterfaceNumber,
                       d->bAlternateSetting, d->bInterfaceClass, d->bInterfaceSubClass,
                       d->bInterfaceProtocol, d->bNumEndpoints);
                for (int e = 0; e < d->bNumEndpoints; ++e) {
                    const struct libusb_endpoint_descriptor *ep = &d->endpoint[e];
                    printf("      ep addr=%02x xfer=%02x maxpkt=%d interval=%d\n",
                           ep->bEndpointAddress, ep->bmAttributes & 0x3, ep->wMaxPacketSize,
                           ep->bInterval);
                }
            }
        }
        libusb_free_config_descriptor(cfg);
    }

    r = libusb_claim_interface(h, 0);
    printf("claim_interface(0)=%d (%s)\n", r, libusb_strerror(r));
    if (r != 0) {
        printf("VERDICT=claim_failed\n");
        libusb_close(h);
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return 1;
    }
    printf("VERDICT=claim_ok\n");
    int rel = libusb_release_interface(h, 0);
    printf("release_interface(0)=%d (%s)\n", rel, libusb_strerror(rel));
    libusb_close(h);
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}
