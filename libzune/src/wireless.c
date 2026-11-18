/*
 * wireless.c - Zune HD wireless sync: pairing and network enrolment
 *
 * A Zune HD syncs over Wi-Fi without a cable, and it needs no certificate
 * to do it. What it needs is a partnership. The player stores the GUID of
 * the one host it will talk to (property 0xD401), hands out a session GUID
 * (0xD221), and then answers standard PTP/IP - ISO 15740, plain TCP port
 * 15740 - from any host that can present that session GUID in the INIT
 * packet. The passphrase for the access point is the only secret on the
 * link, and the player encrypts it under its OWN RSA-1024 public key,
 * which falls out of the ordinary MTPZ handshake. So the whole arrangement
 * is: one USB session to teach the player who we are, then Ethernet frames.
 *
 * This file is the USB half. The half that carries MTP over TCP is not in this
 * tree yet, so nothing here opens port 15740 and no claim is made that the
 * player has ever been reached without a cable.
 *
 * Protocol notes
 * --------------
 * The vendor operations here (0x92xx) are what the two reference
 * implementations record about the Zune desktop software's traffic, and are
 * documented in docs/WIRELESS_SYNC.md. This tree holds no capture of its own,
 * so a shape below is the references' reading of somebody else's capture.
 * What it does hold is one Zune HD on the cable that answers 0xD401, 0xD220,
 * 0xD221, 0xD225, 0xD21C and 0xD217 and hands back a network list from
 * 0x9225; that is the whole of our own wire evidence, and it is recorded in
 * the Status section of that document.
 *
 * These parameters and payload shapes are load-bearing: the 260-byte
 * request that rides with 0x922B, the 258 zero bytes for 0x9231, and the
 * 324-byte network profile for 0x9227 are not interchangeable with anything
 * shorter. Every shape this file sends is traceable to a reference call, and
 * where a reference gives no support for a step this file does not send it.
 * That rule is not stylistic. An earlier draft sent a close-out that no
 * reference accounts for, and the Zune HD on the cable stopped answering its
 * bulk OUT endpoint in that same sitting. Whether the extra operations
 * caused it is not established - the identical symptom was previously
 * measured on a build with no wireless code in it at all, where a power cycle
 * was the answer. Pairing is two property writes and nothing else because
 * that is all one reference does, and the steps this file drops have never
 * been shown to do anything in either direction.
 *
 * Portions of the wire behaviour recorded here were cross-checked against
 * XuneSyncLibrary (GNU LGPL-2.1) and the android-file-transfer-linux fork it
 * vendors (GNU LGPL-2.1, and by its own file headers "or at your option any
 * later version"), and against nothing else. UPSTREAMS.md carries the pins,
 * the licence reading, and the three things deliberately left behind.
 * Only the shapes of these requests came across; no source line did, and the
 * code below is this project's own, written against this library's PTP layer
 * and this library's error convention.
 */

#include "zune_internal.h"

#include <gcrypt.h>

/* ---- Property the pairing reads but no other caller uses ---- */
#define ZUNE_DPC_SyncProbe         0xD217

/* The 0x922B subsystem gate carries a fixed 260-byte request whose first
 * four bytes name the subsystem. `aftl` sends exactly this and records that a
 * shorter or empty payload makes the device refuse the gate. Both halves of
 * that are its observation: this file has never sent 0x922B to a player, so
 * neither the acceptance nor the refusal has been seen from this side. */
static int build_op922b_payload(uint8_t *out /* 260 bytes */)
{
    memset(out, 0, 260);
    out[0] = 0x02;
    out[1] = 0x52;
    out[2] = 0xc0;
    out[3] = 0x00;
    return 260;
}

/* ===================================================================
 * GUID helpers
 *
 * A GUID travels as an MTP string: one byte holding the character count
 * including the terminator, then UTF-16LE. The player wants it braced and
 * uppercase - that is what the desktop software writes and what it reads
 * back, and the comparison it does is a byte compare.
 * =================================================================== */

#define GUID_BRACED_CHARS 38   /* {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} */

static int guid_to_mtp_string(const char *guid, uint8_t *out, size_t outsz)
{
    char buf[GUID_BRACED_CHARS + 1];
    size_t n;

    if (!guid)
        return -1;

    /* Normalise first, measure second. The player byte-compares what it is
     * given, so the only spelling allowed out of here is braced, exactly
     * GUID_BRACED_CHARS long, and uppercase. */
    if (guid[0] == '{') {
        if (strlen(guid) != GUID_BRACED_CHARS)
            return -1;
        snprintf(buf, sizeof(buf), "%s", guid);
    } else {
        if (strlen(guid) != GUID_BRACED_CHARS - 2)
            return -1;
        snprintf(buf, sizeof(buf), "{%s}", guid);
    }

    n = strlen(buf);

    for (char *p = buf; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');

    /* 1 length byte + (chars incl. terminator) * 2 */
    if (outsz < 1 + (n + 1) * 2)
        return -1;

    out[0] = (uint8_t)(n + 1);
    for (size_t i = 0; i < n; i++) {
        out[1 + i * 2]     = (uint8_t)buf[i];
        out[1 + i * 2 + 1] = 0;
    }
    out[1 + n * 2]     = 0;
    out[1 + n * 2 + 1] = 0;
    return 1 + (int)((n + 1) * 2);
}

/* Read an MTP string back into ASCII, dropping the braces. `out` gets at
 * most outsz-1 characters and always a terminator. */
static void mtp_string_to_ascii(const uint8_t *data, uint32_t len,
                                char *out, size_t outsz)
{
    size_t o = 0;

    if (out && outsz)
        out[0] = 0;
    if (!data || len < 2 || !out)
        return;

    for (uint32_t i = 1; i + 1 < len; i += 2) {
        char c = (char)data[i];
        if (c == 0)
            break;
        if (c == '{' || c == '}')
            continue;
        if (o + 1 >= outsz)
            break;
        out[o++] = c;
    }
    out[o] = 0;
}

char *zune_wireless_new_partner_guid(void)
{
    unsigned char r[16];
    char *out = malloc(64);

    if (!out)
        return NULL;

    gcry_randomize(r, sizeof(r), GCRY_STRONG_RANDOM);
    r[6] = (uint8_t)((r[6] & 0x0f) | 0x40);   /* version 4 */
    r[8] = (uint8_t)((r[8] & 0x3f) | 0x80);   /* RFC 4122 variant */

    snprintf(out, 64,
             "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
             r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    return out;
}

/* ===================================================================
 * Vendor-operation helpers
 * =================================================================== */

/* A descriptor query is how the desktop software warms a property before
 * touching it. The player answers some of them and refuses others, and
 * nothing downstream depends on the answer, so the result is logged and
 * dropped. Naming that explicitly is the point: a silent failure here is
 * indistinguishable from a step that was skipped. */
static void prop_desc_query(ZuneDevice *dev, uint16_t prop)
{
    uint8_t *data = NULL;
    uint32_t len = 0;
    uint16_t rc = 0;
    uint32_t params[1] = { prop };

    int ret = ptp_transaction(&dev->ptp, PTP_OC_GetDevicePropDesc,
                              params, 1, PTP_DP_GETDATA,
                              NULL, 0, &data, &len, &rc, NULL, NULL);
    fprintf(stderr, "[wireless] prop desc 0x%04X: %s%s\n", prop,
            (ret == 0 && rc == PTP_RC_OK) ? "ok" : "refused",
            (ret == 0 && rc != PTP_RC_OK) ? " (ignored)" : "");
    free(data);
}

static int prop_set(ZuneDevice *dev, uint16_t prop, const uint8_t *data, uint32_t len)
{
    if (ptp_set_device_prop_value(&dev->ptp, prop, (uint8_t *)data, len) < 0) {
        zune_set_error("The player refused property 0x%04X", prop);
        return -1;
    }
    return 0;
}

static int prop_set_guid(ZuneDevice *dev, uint16_t prop, const char *guid)
{
    uint8_t buf[80];
    int n = guid_to_mtp_string(guid, buf, sizeof(buf));

    if (n < 0) {
        zune_set_error("Not a GUID: %s", guid ? guid : "(null)");
        return -1;
    }
    return prop_set(dev, prop, buf, (uint32_t)n);
}

static int prop_get(ZuneDevice *dev, uint16_t prop, uint8_t **out, uint32_t *out_len)
{
    uint8_t *data = NULL;
    uint32_t len = 0;
    uint16_t rc = 0;
    uint32_t params[1] = { prop };

    int ret = ptp_transaction(&dev->ptp, PTP_OC_GetDevicePropValue,
                              params, 1, PTP_DP_GETDATA,
                              NULL, 0, &data, &len, &rc, NULL, NULL);
    if (ret != 0 || rc != PTP_RC_OK) {
        free(data);
        zune_set_error("Property 0x%04X read failed (0x%04X)", prop, rc);
        return -1;
    }
    *out = data;
    *out_len = len;
    return 0;
}

/* A bare vendor operation: parameters only, no data phase. */
static int op_nodata(ZuneDevice *dev, uint16_t oc, uint32_t p1, uint32_t p2,
                     uint32_t p3, int nparams)
{
    uint32_t params[3] = { p1, p2, p3 };
    uint16_t rc = 0;

    int ret = ptp_transaction(&dev->ptp, oc, params, nparams, 0,
                              NULL, 0, NULL, NULL, &rc, NULL, NULL);
    if (ret != 0 || rc != PTP_RC_OK) {
        zune_set_error("Operation 0x%04X failed (0x%04X)", oc, rc);
        return -1;
    }
    return 0;
}

/* A vendor operation that carries a request payload. Some of them want two
 * or three parameters alongside the payload and some want none, so the
 * parameter list belongs to the caller. */
static int op_send(ZuneDevice *dev, uint16_t oc, const uint32_t *params,
                   int nparams, const uint8_t *data, uint32_t len)
{
    uint16_t rc = 0;

    int ret = ptp_transaction(&dev->ptp, oc, (uint32_t *)params, nparams,
                              PTP_DP_SENDDATA,
                              (uint8_t *)data, len, NULL, NULL, &rc, NULL, NULL);
    if (ret != 0 || rc != PTP_RC_OK) {
        zune_set_error("Operation 0x%04X failed (0x%04X)", oc, rc);
        return -1;
    }
    return 0;
}

/* A vendor operation that answers with a payload. */
static int op_recv(ZuneDevice *dev, uint16_t oc, uint8_t **out, uint32_t *out_len)
{
    uint8_t *data = NULL;
    uint32_t len = 0;
    uint16_t rc = 0;

    int ret = ptp_transaction(&dev->ptp, oc, NULL, 0, PTP_DP_GETDATA,
                              NULL, 0, &data, &len, &rc, NULL, NULL);
    if (ret != 0 || rc != PTP_RC_OK) {
        free(data);
        zune_set_error("Operation 0x%04X failed (0x%04X)", oc, rc);
        return -1;
    }
    *out = data;
    *out_len = len;
    return 0;
}

/* ===================================================================
 * Phase 1 - become the player's synchronization partner
 * =================================================================== */

int zune_wireless_pair_sync(ZuneDeviceHandle dev, const char *partner_guid,
                            const char *driver_tag, const char *friendly_name,
                            char *guid_out, size_t guid_out_len)
{
    ZuneDevice *d = (ZuneDevice *)dev;
    char *minted = NULL;
    int rc = -1;

    if (!d) {
        zune_set_error("No device");
        return -1;
    }

    if (!partner_guid) {
        minted = zune_wireless_new_partner_guid();
        if (!minted) {
            zune_set_error("Out of memory minting a pairing GUID");
            return -1;
        }
        partner_guid = minted;
    }

    if (guid_out && guid_out_len)
        snprintf(guid_out, guid_out_len, "%s", partner_guid);

    fprintf(stderr, "[wireless] phase 1: claiming the partnership slot %s\n",
            partner_guid);

    /* The player records who is driving. It is informational, and the
     * desktop software puts a version string here, so a recognisable one
     * is worth the round trip - it is what the user sees if they look. */
    {
        uint8_t buf[128];
        const char *tag = driver_tag && *driver_tag
                              ? driver_tag : "Windows/10.0 Zuuned";
        int n = 0;
        size_t l = strlen(tag);
        /* One length byte plus (characters including the terminator) times two
         * has to fit the 128-byte buffer, which puts the ceiling at 62; 58
         * leaves room. A longer tag is shortened rather than skipped -
         * silently dropping this write would leave the player naming whatever
         * host drove it last. */
        if (l > 58) {
            fprintf(stderr, "[wireless] driver tag shortened from %u to 58 characters\n",
                    (unsigned)l);
            l = 58;
        }
        {
            buf[0] = (uint8_t)(l + 1);
            for (size_t i = 0; i < l; i++) {
                buf[1 + i * 2]     = (uint8_t)tag[i];
                buf[1 + i * 2 + 1] = 0;
            }
            buf[1 + l * 2]     = 0;
            buf[1 + l * 2 + 1] = 0;
            n = 1 + (int)((l + 1) * 2);
        }
        if (n > 0 && prop_set(d, MTP_DPC_SessionInitiatorInfo, buf, (uint32_t)n) != 0)
            goto done;
    }

    if (friendly_name && *friendly_name) {
        uint8_t buf[600];
        size_t l = strlen(friendly_name);
        if (l > 120) l = 120;
        buf[0] = (uint8_t)(l + 1);
        for (size_t i = 0; i < l; i++) {
            buf[1 + i * 2]     = (uint8_t)friendly_name[i];
            buf[1 + i * 2 + 1] = 0;
        }
        buf[1 + l * 2]     = 0;
        buf[1 + l * 2 + 1] = 0;
        if (prop_set(d, MTP_DPC_Zune_DeviceFriendlyName, buf,
                     (uint32_t)(1 + (l + 1) * 2)) != 0)
            goto done;
    }

    prop_desc_query(d, 0xD22F);
    prop_desc_query(d, MTP_DPC_Zune_DeviceFriendlyName);
    prop_desc_query(d, 0x5002);   /* device friendly name, standard */
    prop_desc_query(d, 0xD231);
    prop_desc_query(d, 0xD232);
    prop_desc_query(d, MTP_DPC_Zune_SyncFlags);
    prop_desc_query(d, MTP_DPC_Zune_PartnerGUID);
    prop_desc_query(d, MTP_DPC_Zune_SyncPartner);
    prop_desc_query(d, 0xD22C);

    /* Claim the partnership. Two writes, and that is all of it: the same
     * GUID goes into the partnership record and into the property that
     * names which partner the player will accept over the network. That is
     * the whole of the capture-derived pairing tool in `aftl`, plus reading
     * the second write back - which is why the second write is here at all,
     * since an earlier draft of this function never wrote 0xD220 and so
     * paired the player to nothing.
     *
     * No read-back of either write has been recorded yet. On the one player
     * this tree has run a pairing against, the property writes went through
     * and then the run sent a vendor operation it had no business sending.
     * Until `wirelesstool pair` prints both read-back lines this paragraph
     * states an intention, not a measurement.
     *
     * What used to sit here is gone deliberately. Clearing 0xD225, the
     * 0xD21C flags byte, the three sync-counter writes, and the
     * 0x9224 / 0xD217 x3 / 0x9217 / 0x9227 / 0x9218 close-out trace back
     * to no reference: 0x9217 is SyncDeviceDB and belongs to the
     * PPP-and-HTTP route this port does not take, and the counter values
     * were copied from nothing. The player stopped answering its bulk OUT
     * endpoint in the sitting that sent them, which is a coincidence worth
     * respecting rather than a proven cause - see the note at the head of
     * this file. Pairing writes two properties and reads them back, and a
     * read-back is the only proof available here. */
    if (prop_set_guid(d, MTP_DPC_Zune_SyncPartner, partner_guid) != 0)
        goto done;
    if (prop_set_guid(d, MTP_DPC_Zune_PCSessionGUID, partner_guid) != 0)
        goto done;

    {
        uint8_t *back = NULL;
        uint32_t len = 0;
        char text[64];

        if (prop_get(d, MTP_DPC_Zune_SyncPartner, &back, &len) != 0) {
            zune_set_error("The player would not read the partnership record "
                           "back after pairing");
            goto done;
        }
        mtp_string_to_ascii(back, len, text, sizeof(text));
        free(back);
        back = NULL;
        fprintf(stderr, "[wireless] 0xD401 reads back as %s\n", text);

        if (prop_get(d, MTP_DPC_Zune_PCSessionGUID, &back, &len) != 0) {
            zune_set_error("The player would not read its network partner "
                           "slot back after pairing");
            goto done;
        }
        mtp_string_to_ascii(back, len, text, sizeof(text));
        free(back);
        fprintf(stderr, "[wireless] 0xD220 reads back as %s\n", text);
    }

    fprintf(stderr, "[wireless] phase 1 complete: this host is the sync partner\n");
    rc = 0;

done:
    free(minted);
    return rc;
}

/* Bring the player's network stack up.  `XuneSyncLibrary` sends this
 * 258-byte all-zero request first, before it touches the wireless mode or
 * the subsystem gate, which is the only reason this file sends it in the
 * same place.  That the step is load-bearing - that skipping it leaves a
 * device answering MTP but never announcing itself - is an inference from
 * that placement and has never been observed in either direction.  Nothing
 * comes back. */
static int network_subsystem_init(ZuneDevice *dev)
{
    static uint8_t zeros[258];
    uint16_t rc = 0;

    if (ptp_transaction(&dev->ptp, MTP_OC_Zune_Op9231, NULL, 0,
                        PTP_DP_SENDDATA, zeros, sizeof(zeros),
                        NULL, NULL, &rc, NULL, NULL) != 0 || rc != PTP_RC_OK) {
        zune_set_error("The player refused the network subsystem init (0x%04X)", rc);
        return -1;
    }
    return 0;
}

/* ===================================================================
 * Phase 2 - enrol a network and read the session GUID
 * =================================================================== */

int zune_wireless_read_session_guid(ZuneDeviceHandle dev, char *out, size_t out_len)
{
    ZuneDevice *d = (ZuneDevice *)dev;
    uint8_t *data = NULL;
    uint32_t len = 0;

    if (!d || !out || out_len < 38) {
        zune_set_error("Need an open device and room for a GUID");
        return -1;
    }
    if (prop_get(d, MTP_DPC_Zune_WirelessSessionGUID, &data, &len) != 0)
        return -1;

    mtp_string_to_ascii(data, len, out, out_len);
    free(data);

    if (!*out) {
        zune_set_error("Property 0xD221 came back empty - pair over USB first");
        return -1;
    }
    fprintf(stderr, "[wireless] session GUID read from 0xD221\n");
    return 0;
}

/* Read one property purely to find out whether the player answers it, and
 * keep the GUID if it is one.  Silence here is information: a Zune that
 * refuses 0xD217 is a Zune without Wi-Fi, and the caller has to be able to
 * tell that apart from a pairing that has not happened yet. */
static int probe_property(ZuneDevice *dev, uint16_t prop, char *keep,
                          size_t keep_len)
{
    uint8_t *data = NULL;
    uint32_t len = 0;

    if (prop_get(dev, prop, &data, &len) != 0)
        return 0;
    if (keep && keep_len)
        mtp_string_to_ascii(data, len, keep, keep_len);
    free(data);
    return 1;
}

int zune_wireless_status(ZuneDeviceHandle dev, ZuneWirelessStatus *out)
{
    ZuneDevice *d = (ZuneDevice *)dev;
    ZuneWirelessStatus seen;

    if (!d || !out) {
        zune_set_error("No device");
        return -1;
    }

    memset(&seen, 0, sizeof(seen));
    seen.has_device_key = (mtpz_device_rsa_modulus(NULL) == 0);

    /* Read in the order pairing writes them, so a half-finished pairing
     * reads back as the half it is. */
    seen.answers_partner = probe_property(d, MTP_DPC_Zune_SyncPartner,
                                          seen.partner_guid, sizeof(seen.partner_guid));
    seen.answers_pc_session = probe_property(d, MTP_DPC_Zune_PCSessionGUID,
                                             NULL, 0);
    seen.answers_wireless_session = probe_property(d, MTP_DPC_Zune_WirelessSessionGUID,
                                                   seen.session_guid, sizeof(seen.session_guid));
    seen.answers_partner_slot = probe_property(d, MTP_DPC_Zune_PartnerGUID, NULL, 0);
    seen.answers_sync_flags = probe_property(d, MTP_DPC_Zune_SyncFlags, NULL, 0);
    /* Last, because it is the one that separates "no Wi-Fi at all" from
     * "Wi-Fi, but not paired yet". */
    seen.answers_probe = probe_property(d, ZUNE_DPC_SyncProbe, NULL, 0);

    *out = seen;
    return 0;
}

int zune_wireless_scan_wifi(ZuneDeviceHandle dev, uint8_t **out, uint32_t *out_len)
{
    ZuneDevice *d = (ZuneDevice *)dev;

    if (!d || !out || !out_len) {
        zune_set_error("No device");
        return -1;
    }
    return op_recv(d, MTP_OC_Zune_Op9225, out, out_len);
}

/* Find the SSID in the scan blob and lift the three security words that
 * precede it. The entry layout is inferred from where the name sits inside
 * its record, not from a documented header, so a miss falls back to the
 * WPA2 default instead of failing the enrolment. */
static void scan_security_flags(const uint8_t *scan, uint32_t scan_len,
                                const char *ssid, uint32_t flags[3])
{
    size_t l = strlen(ssid);

    flags[0] = 0x00000001;
    flags[1] = 0x00000007;
    flags[2] = 0x00000004;   /* WPA2 - the common case */

    if (!scan || l == 0 || scan_len < l + 24)
        return;

    for (uint32_t off = 0; off + l <= scan_len; off++) {
        if (memcmp(scan + off, ssid, l) != 0)
            continue;
        if (off < 24 || off - 24 + 12 > scan_len)
            continue;
        memcpy(flags, scan + off - 24, 12);
        fprintf(stderr, "[wireless] network found in scan: security %08X "
                        "%08X %08X\n", flags[0], flags[1], flags[2]);
        return;
    }
    fprintf(stderr, "[wireless] network not in the scan result - enrolling "
                    "with the WPA2 default\n");
}

/* RSA-1024 PKCS#1 v1.5, public key only. The modulus arrives
 * little-endian from the handshake, so it is reversed before it becomes
 * an MPI, and the ciphertext goes out big-endian because that is the
 * order the player's own decoder expects. */
static int rsa1024_encrypt(const uint8_t mod_le[128], const uint8_t *msg,
                           size_t msg_len, uint8_t out[128])
{
    unsigned char block[128];
    unsigned char mod_be[128];
    char nhex[257], mhex[258];
    gcry_mpi_t n = NULL, e = NULL, m = NULL, c = NULL;
    int err = -1;
    size_t ps_len;

    if (msg_len + 11 > 128) {
        zune_set_error("Passphrase too long for the player's key");
        return -1;
    }

    /* EM = 00 02 <non-zero padding> 00 <message> */
    block[0] = 0x00;
    block[1] = 0x02;
    ps_len = 128 - msg_len - 3;
    for (size_t i = 0; i < ps_len; ) {
        unsigned char r;
        gcry_randomize(&r, 1, GCRY_STRONG_RANDOM);
        if (r)
            block[2 + i++] = r;
    }
    block[2 + ps_len] = 0x00;
    memcpy(block + 3 + ps_len, msg, msg_len);

    for (int i = 0; i < 128; i++)
        mod_be[i] = mod_le[127 - i];

    for (int i = 0; i < 128; i++) {
        sprintf(nhex + i * 2, "%02X", mod_be[i]);
        sprintf(mhex + i * 2, "%02X", block[i]);
    }
    nhex[256] = 0;
    mhex[256] = 0;

    if (gcry_mpi_scan(&n, GCRYMPI_FMT_HEX, nhex, 0, NULL) ||
        gcry_mpi_scan(&e, GCRYMPI_FMT_HEX, "10001", 0, NULL) ||
        gcry_mpi_scan(&m, GCRYMPI_FMT_HEX, mhex, 0, NULL)) {
        zune_set_error("Could not read the player's RSA key");
        goto out;
    }

    c = gcry_mpi_new(1024);
    if (!c)
        goto out;
    gcry_mpi_powm(c, m, e, n);

    {
        size_t written = 0;
        memset(out, 0, 128);
        if (gcry_mpi_print(GCRYMPI_FMT_USG, out, 128, &written, c)) {
            zune_set_error("Could not export the encrypted passphrase");
            goto out;
        }
        /* A ciphertext whose leading byte happens to fall away comes back
         * short, and the player reads a fixed 128 bytes, so right-align. */
        if (written && written < 128)
            memmove(out + (128 - written), out, written);
    }
    err = 0;

out:
    if (n) gcry_mpi_release(n);
    if (e) gcry_mpi_release(e);
    if (m) gcry_mpi_release(m);
    if (c) gcry_mpi_release(c);
    memset(block, 0, sizeof(block));
    return err;
}

int zune_wireless_set_wifi_network(ZuneDeviceHandle dev, const char *ssid,
                                   const char *password)
{
    ZuneDevice *d = (ZuneDevice *)dev;
    uint8_t config[324];
    uint8_t enc[128];
    uint8_t mod_le[128];
    uint8_t *scan = NULL;
    uint32_t scan_len = 0;
    uint32_t flags[3];
    uint32_t profile_id = 1;
    uint32_t pw_len = 128;
    size_t ssid_len;
    uint16_t rc = 0;

    if (!d || !ssid || !*ssid) {
        zune_set_error("No network name");
        return -1;
    }
    if (!password)
        password = "";

    if (mtpz_device_rsa_modulus(mod_le) != 0) {
        zune_set_error("The player's RSA key was never captured, so its "
                       "passphrase cannot be encrypted. Connect over USB with "
                       "MTPZ credentials present and try again.");
        return -1;
    }

    if (zune_wireless_scan_wifi(dev, &scan, &scan_len) == 0)
        scan_security_flags(scan, scan_len, ssid, flags);
    else
        scan_security_flags(NULL, 0, ssid, flags);
    free(scan);

    ssid_len = strlen(ssid);
    if (ssid_len > 32)
        ssid_len = 32;

    memset(config, 0, sizeof(config));
    memcpy(config + 0, &profile_id, 4);
    memcpy(config + 4, &ssid_len, 4);
    memcpy(config + 8, ssid, ssid_len);
    memcpy(config + 40, flags, 12);
    {
        uint32_t tail[3] = { 1, 0, 0 };
        memcpy(config + 52, tail, 12);
    }

    /* The passphrase travels as UTF-16LE, like everything else the player
     * is given that a human might type. */
    {
        uint8_t utf16[116];
        size_t pl = strlen(password);
        /* PKCS#1 v1.5 has to fit the message plus eleven bytes of padding
         * into one 128-byte block, and the passphrase goes out as UTF-16, so
         * the ceiling is 58 characters. Truncating instead of refusing would
         * enrol a wrong key, which reaches the user as a bad passphrase on a
         * screen that says the network was saved. */
        if (pl > 58) {
            zune_set_error("That passphrase is %u characters; the player's key "
                           "takes at most 58.", (unsigned)pl);
            return -1;
        }
        for (size_t i = 0; i < pl; i++) {
            utf16[i * 2]     = (uint8_t)password[i];
            utf16[i * 2 + 1] = 0;
        }
        if (rsa1024_encrypt(mod_le, utf16, pl * 2, enc) != 0)
            return -1;
    }

    memcpy(config + 64, &pw_len, 4);
    memcpy(config + 68, enc, 128);
    /* 196 used, 324 sent: the rest is the padding the desktop software
     * sends and the player appears to size its parser by. */

    /* The desktop software runs 0x9224 immediately before handing the
     * profile over, and the player reads it as the start of enrolment. */
    {
        uint8_t *prep = NULL;
        uint32_t prep_len = 0;
        if (op_recv(d, MTP_OC_Zune_Op9224, &prep, &prep_len) != 0)
            fprintf(stderr, "[wireless] 0x9224 pre-profile step refused - "
                            "continuing\n");
        free(prep);
    }

    if (ptp_transaction(&d->ptp, MTP_OC_Zune_Op9227, NULL, 0, PTP_DP_SENDDATA,
                        config, sizeof(config), NULL, NULL, &rc, NULL, NULL) != 0 ||
        rc != PTP_RC_OK) {
        zune_set_error("The player refused the network profile (0x%04X)", rc);
        return -1;
    }

    /* Post-profile steps. The order comes from the desktop traffic: one
     * step at 0, then the same call at 2 three times over. */
    if (op_nodata(d, MTP_OC_Zune_Op9228, 0, 0, 0, 1) != 0)
        return -1;
    for (int i = 0; i < 3; i++)
        if (op_nodata(d, MTP_OC_Zune_Op9228, 2, 0, 0, 1) != 0)
            return -1;

    /* The two reads the desktop software closes with. Nothing the host uses
     * comes back; the reference treats them as what carries the player
     * through its own state machine, which is to say as what commits the
     * enrolment. A refusal is therefore not swallowed: the profile went over
     * and this file has no way to know the player kept it, so saying
     * "enrolled" either way would report a state nobody measured. */
    for (int i = 0; i < 2; i++) {
        uint8_t *probe = NULL;
        uint32_t probe_len = 0;
        if (prop_get(d, ZUNE_DPC_SyncProbe, &probe, &probe_len) != 0) {
            zune_set_error("The player took the network profile and then "
                           "refused the closing read, so the enrolment is not "
                           "confirmed. Check the player's own wireless setup "
                           "before assuming the network is there.");
            return -1;
        }
        free(probe);
    }

    fprintf(stderr, "[wireless] network enrolled\n");
    return 0;
}

/* Does the text of a stored GUID name this partner?  Braces do not matter,
 * case does not matter, and the all-zero GUID means "nobody" exactly as
 * loudly as an empty string does. */
static int guid_names(const char *stored, const char *want)
{
    char a[64], b[64];
    size_t n = 0;
    int all_zero = 1;

    for (const char *p = stored ? stored : ""; *p && n + 1 < sizeof(a); p++)
        if (*p != '{' && *p != '}')
            a[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    a[n] = 0;
    if (n == 0)
        return 0;
    for (size_t i = 0; a[i]; i++)
        if (a[i] != '0' && a[i] != '-')
            all_zero = 0;
    if (all_zero)
        return 0;

    if (!want)
        return 1;   /* any partner at all is a partner */

    n = 0;
    for (const char *p = want; *p && n + 1 < sizeof(b); p++)
        if (*p != '{' && *p != '}')
            b[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    b[n] = 0;
    return strcmp(a, b) == 0;
}

int zune_wireless_pair_wifi(ZuneDeviceHandle dev, const char *ssid,
                            const char *password, const char *partner_guid,
                            char *session_guid_out, size_t out_len)
{
    ZuneDevice *d = (ZuneDevice *)dev;
    uint8_t gate[260];
    uint32_t gate_on[3] = { 3, 1, 0 };
    int gate_len;

    if (!d) {
        zune_set_error("No device");
        return -1;
    }

    /* Phase 2 without phase 1 is the failure this function's name used to
     * invite: the reference ordering implies a player with no sync partner
     * still takes a network profile and still announces itself, and then
     * ignores whoever connects.  Nobody has observed that, in either
     * direction - but the guard is one GETPROP, so the cost of believing the
     * reference is one property read.  Ask the player rather than trusting
     * the caller, and do it before touching the network subsystem. */
    {
        uint8_t *partner = NULL;
        uint32_t partner_len = 0;
        char stored[64];

        if (prop_get(d, MTP_DPC_Zune_SyncPartner, &partner, &partner_len) != 0) {
            zune_set_error("The player would not say who its sync partner is, "
                           "so nothing can be set up on top of that. Pair it "
                           "over USB first.");
            return -1;
        }
        mtp_string_to_ascii(partner, partner_len, stored, sizeof(stored));
        free(partner);
        if (!guid_names(stored, partner_guid)) {
            zune_set_error("The player's partnership record names no partner "
                           "matching this host, so it is not paired. Run the "
                           "USB pairing before setting up a network.");
            return -1;
        }
    }

    /* Bring the network subsystem up before anything else touches it. */
    if (network_subsystem_init(d) != 0)
        return -1;
    if (op_nodata(d, MTP_OC_Zune_Op9230, 1, 0, 0, 1) != 0)
        return -1;
    gate_len = build_op922b_payload(gate);
    if (op_send(d, MTP_OC_Zune_Op922B, gate_on, 3, gate,
                (uint32_t)gate_len) != 0) {
        zune_set_error("The player refused the subsystem gate");
        return -1;
    }

    if (partner_guid && *partner_guid &&
        prop_set_guid(d, MTP_DPC_Zune_PCSessionGUID, partner_guid) != 0)
        return -1;

    if (session_guid_out && out_len) {
        if (zune_wireless_read_session_guid(dev, session_guid_out, out_len) != 0)
            return -1;
    }

    if (zune_wireless_set_wifi_network(dev, ssid, password) != 0)
        return -1;

    /* And open the door.  What the references say happens after this is that
     * the player announces itself on the LAN over SSDP - with a device UUID
     * derived from its MAC address, not with the session GUID - and answers
     * PTP/IP on TCP 15740 from a host that presents the session GUID in the
     * init packet.  This tree has never opened that socket, so from here on
     * it is their account and not an observation.
     *
     * The gate call goes last rather than first because that is the order the
     * reference puts it in. */
    if (op_nodata(d, MTP_OC_Zune_Op9230, 1, 0, 0, 1) != 0)
        return -1;
    if (op_nodata(d, MTP_OC_Zune_Op922B, 3, 1, 0, 2) != 0)
        fprintf(stderr, "[wireless] final gate reopen refused - continuing\n");

    fprintf(stderr, "[wireless] phase 2 complete: wireless sync is on\n");
    return 0;
}

int zune_wireless_disable(ZuneDeviceHandle dev)
{
    ZuneDevice *d = (ZuneDevice *)dev;
    int refused = 0;

    if (!d) {
        zune_set_error("No device");
        return -1;
    }

    /* Turning wireless sync off is `aftl`'s seven-step unwinding rather than a
     * single call: reset the network subsystem, take the mode down, close the
     * gate, run the link cleanup, then bring the mode and the gate back up so
     * the next pairing starts from a state the player recognises. Every step
     * here is a bare command, which is how that reference issues all seven in
     * ClearWirelessPairing() - including 0x922F, which the same file reads a
     * payload from on other paths and sends with no data phase on this one.
     * The 260-byte form of 0x922B belongs to enrolment, not to this.
     *
     * Only the mode change itself may fail the call: a player that refuses one
     * of the steps beside it is still a player whose mode went down. What this
     * function will not do is print a clean "off" over the top of refusals, so
     * they are counted and named, and whoever is watching can tell an unwound
     * player from one that merely stopped syncing. */
    if (op_nodata(d, MTP_OC_Zune_Op922F, 0, 0, 0, 0) != 0) {
        fprintf(stderr, "[wireless] subsystem reset refused - continuing\n");
        refused++;
    }

    if (op_nodata(d, MTP_OC_Zune_Op9230, 2, 0, 0, 1) != 0) {
        zune_set_error("Could not turn wireless sync off");
        return -1;
    }
    if (op_nodata(d, MTP_OC_Zune_Op922B, 3, 2, 0, 2) != 0) {
        fprintf(stderr, "[wireless] close gate refused - continuing\n");
        refused++;
    }
    if (op_nodata(d, MTP_OC_Zune_Op922D, 3, 3, 0, 2) != 0) {
        fprintf(stderr, "[wireless] link cleanup refused - continuing\n");
        refused++;
    }
    if (op_nodata(d, MTP_OC_Zune_Op9215, 0, 0, 0, 0) != 0) {
        fprintf(stderr, "[wireless] 0x9215 refused - continuing\n");
        refused++;
    }
    if (op_nodata(d, MTP_OC_Zune_Op9230, 1, 0, 0, 1) != 0) {
        fprintf(stderr, "[wireless] could not reopen the mode - the player may "
                        "have to be paired again\n");
        refused++;
    }
    if (op_nodata(d, MTP_OC_Zune_Op922B, 3, 1, 0, 2) != 0) {
        fprintf(stderr, "[wireless] reopen gate refused - continuing\n");
        refused++;
    }

    if (refused)
        fprintf(stderr, "[wireless] wireless sync mode is off, but %d of the six "
                        "steps beside it were refused - the player may still "
                        "hold a partnership or a network\n", refused);
    else
        fprintf(stderr, "[wireless] wireless sync off\n");
    return 0;
}
