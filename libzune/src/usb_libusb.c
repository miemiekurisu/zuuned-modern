/*
 * usb_libusb.c — libusb-1.0 USB backend for libzune
 *
 * Built on Linux and on macOS.  On macOS this is the only backend: the
 * alternative in this directory, driverkit_usb.c, talks to a DriverKit
 * extension, and a DEXT cannot be loaded unless Apple signed it, so it is
 * unreachable without a paid Developer account.  libusb reaches the player
 * through IOKit from user space instead, which is what every other Zune tool
 * on macOS does.  Where macOS behaviour differs from Linux it is marked
 * __APPLE__ below and the reason is written out at the call.
 *
 * Implements zune_usb_backend_t from usb.h.
 */

#include "usb.h"
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    libusb_context       *ctx;
    libusb_device_handle *handle;
    uint8_t               interface_num;
} libusb_backend_data_t;

static libusb_backend_data_t *get_data(zune_usb_handle_t *h) {
    return (libusb_backend_data_t *)h->backend_data;
}

static int libusb_backend_open(zune_usb_handle_t *handle, uint16_t vid, uint16_t pid)
{
    libusb_backend_data_t *bd = calloc(1, sizeof(libusb_backend_data_t));
    if (!bd) return ZUNE_USB_ERROR;

    if (libusb_init(&bd->ctx) != 0) {
        fprintf(stderr, "[libusb] init failed\n");
        free(bd);
        return ZUNE_USB_ERROR;
    }

    bd->handle = libusb_open_device_with_vid_pid(bd->ctx, vid, pid);
    if (!bd->handle) {
        fprintf(stderr, "[libusb] device not found (vid=0x%04X pid=0x%04X)\n", vid, pid);
        libusb_exit(bd->ctx);
        free(bd);
        return ZUNE_USB_ERROR;
    }

#ifndef __APPLE__
    /* Detach kernel driver if attached.
     *
     * Linux only, and not merely because macOS has nothing to detach.
     * darwin_kernel_driver_active() answers "does this interface's IORegistry
     * entry have a child", and darwin_detach_kernel_driver() without the
     * com.apple.vm.device-access entitlement or root returns
     * LIBUSB_ERROR_ACCESS (os/darwin_usb.c, get_capture_entitlements /
     * geteuid).  This build carries no entitlement — that key is signed, so it
     * costs an Apple Developer account — so the honest macOS answer to "may I
     * take this interface from its driver" is "I am not allowed to ask", and
     * asking produces a failure no caller can act on.  It also does not need
     * to: macOS 26 ships no driver for a Zune at all (no PTPCamera, no
     * ptpcamerad, no still-image class driver in /System/Library/
     * DriverExtensions), so interface 0 arrives free and the claim below is
     * the only question that matters. */
    if (libusb_kernel_driver_active(bd->handle, 0) == 1) {
        libusb_detach_kernel_driver(bd->handle, 0);
    }

    /* Set configuration */
    libusb_set_configuration(bd->handle, 1);
#endif

    /* Claim interface 0 */
    int ret = libusb_claim_interface(bd->handle, 0);
    if (ret != 0) {
        fprintf(stderr, "[libusb] claim_interface failed: %s\n", libusb_strerror(ret));
#ifdef __APPLE__
        /* The mechanism libusb reports is kIOReturnExclusiveAccess: something
         * already owns the interface.  On macOS that is usually another
         * process, and the thing to try next is a replug plus the macOS
         * accessory allowance, because since macOS 13 Apple silicon asks the
         * user to permit a new USB accessory and an unallowed one answers
         * ordinary I/O - reads and writes, not this call - as though the
         * player were deaf.  Naming both beats a bare error code, because
         * neither is visible anywhere else in the log.  Which of the two this
         * is stays unmeasured here: no Zune has been claimed on macOS yet. */
        fprintf(stderr, "[libusb] on macOS: another process may hold the player, or"
                        " macOS has not allowed this USB accessory yet — check"
                        " System Settings → Privacy & Security → USB\n");
#endif
        libusb_close(bd->handle);
        libusb_exit(bd->ctx);
        free(bd);
        return ZUNE_USB_ERROR;
    }
    bd->interface_num = 0;

    /* Discover endpoints */
    libusb_device *dev = libusb_get_device(bd->handle);
    struct libusb_config_descriptor *config;
    if (libusb_get_active_config_descriptor(dev, &config) == 0) {
        if (config->bNumInterfaces > 0) {
            const struct libusb_interface_descriptor *iface =
                &config->interface[0].altsetting[0];
            for (int i = 0; i < iface->bNumEndpoints; i++) {
                const struct libusb_endpoint_descriptor *ep = &iface->endpoint[i];
                uint8_t type = ep->bmAttributes & 0x03;
                if (type == LIBUSB_TRANSFER_TYPE_BULK) {
                    if (ep->bEndpointAddress & 0x80) {
                        handle->bulk_in_ep = ep->bEndpointAddress;
                        handle->bulk_in_maxpacket = ep->wMaxPacketSize;
                    } else {
                        handle->bulk_out_ep = ep->bEndpointAddress;
                        handle->bulk_out_maxpacket = ep->wMaxPacketSize;
                    }
                } else if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                    if (ep->bEndpointAddress & 0x80)
                        handle->interrupt_ep = ep->bEndpointAddress;
                }
            }
        }
        libusb_free_config_descriptor(config);
    }

    handle->backend_data = bd;

    fprintf(stderr, "[libusb] opened: bulk_in=0x%02X(%u) bulk_out=0x%02X(%u) int=0x%02X\n",
            handle->bulk_in_ep, handle->bulk_in_maxpacket,
            handle->bulk_out_ep, handle->bulk_out_maxpacket,
            handle->interrupt_ep);

    return ZUNE_USB_OK;
}

static int libusb_backend_close(zune_usb_handle_t *handle)
{
    if (!handle || !handle->backend_data) return ZUNE_USB_OK;
    libusb_backend_data_t *bd = get_data(handle);

    libusb_release_interface(bd->handle, bd->interface_num);
    libusb_close(bd->handle);
    libusb_exit(bd->ctx);
    free(bd);
    handle->backend_data = NULL;

    fprintf(stderr, "[libusb] closed\n");
    return ZUNE_USB_OK;
}

static int libusb_backend_bulk_read(zune_usb_handle_t *handle, uint8_t endpoint,
                                     uint8_t *data, uint32_t length,
                                     uint32_t *actual_length, uint32_t timeout_ms)
{
    libusb_backend_data_t *bd = get_data(handle);
    int transferred = 0;
    int ret = libusb_bulk_transfer(bd->handle, endpoint, data, (int)length,
                                   &transferred, timeout_ms);
    if (actual_length) *actual_length = (uint32_t)transferred;

    /* A timeout with NOTHING received is a failure — reporting it as
     * "success, 0 bytes" made Linux timeouts indistinguishable from a
     * device ZLP and masked every hang as a mysterious short read.
     * A timeout after PARTIAL data still returns what arrived. */
    if (ret == LIBUSB_ERROR_TIMEOUT && transferred == 0) {
        fprintf(stderr, "[libusb] bulk_read timeout: ep=0x%02X (%u ms)\n",
                endpoint, timeout_ms);
        return ZUNE_USB_ERROR;
    }
    if (ret != 0 && ret != LIBUSB_ERROR_TIMEOUT) {
        fprintf(stderr, "[libusb] bulk_read failed: ep=0x%02X err=%s\n",
                endpoint, libusb_strerror(ret));
        return ZUNE_USB_ERROR;
    }
    return ZUNE_USB_OK;
}

static int libusb_backend_bulk_write(zune_usb_handle_t *handle, uint8_t endpoint,
                                      const uint8_t *data, uint32_t length,
                                      uint32_t *actual_length, uint32_t timeout_ms)
{
    libusb_backend_data_t *bd = get_data(handle);
    int transferred = 0;
    int ret = libusb_bulk_transfer(bd->handle, endpoint, (uint8_t *)data, (int)length,
                                   &transferred, timeout_ms);
    if (actual_length) *actual_length = (uint32_t)transferred;

    if (ret != 0) {
        fprintf(stderr, "[libusb] bulk_write failed: ep=0x%02X err=%s\n",
                endpoint, libusb_strerror(ret));
        return ZUNE_USB_ERROR;
    }
    return ZUNE_USB_OK;
}

static int libusb_backend_clear_halt(zune_usb_handle_t *handle, uint8_t endpoint)
{
    libusb_backend_data_t *bd = get_data(handle);
    return libusb_clear_halt(bd->handle, endpoint) == 0 ? ZUNE_USB_OK : ZUNE_USB_ERROR;
}

static int libusb_backend_reset(zune_usb_handle_t *handle)
{
    libusb_backend_data_t *bd = get_data(handle);
    return libusb_reset_device(bd->handle) == 0 ? ZUNE_USB_OK : ZUNE_USB_ERROR;
}

static const zune_usb_backend_t libusb_backend = {
    .open       = libusb_backend_open,
    .close      = libusb_backend_close,
    .bulk_read  = libusb_backend_bulk_read,
    .bulk_write = libusb_backend_bulk_write,
    .clear_halt = libusb_backend_clear_halt,
    .reset      = libusb_backend_reset,
};

const zune_usb_backend_t *zune_usb_get_backend(void) {
    return &libusb_backend;
}

int zune_usb_find_device(zune_usb_handle_t *handle)
{
    memset(handle, 0, sizeof(*handle));
    handle->backend = zune_usb_get_backend();

    if (handle->backend->open(handle, ZUNE_USB_VID, ZUNE_USB_PID_ZUNE) == ZUNE_USB_OK)
        return ZUNE_USB_OK;

    if (handle->backend->open(handle, ZUNE_USB_VID, ZUNE_USB_PID_ZUNE_HD) == ZUNE_USB_OK)
        return ZUNE_USB_OK;

    fprintf(stderr, "[libusb] no Zune device found\n");
    return ZUNE_USB_ERROR;
}
