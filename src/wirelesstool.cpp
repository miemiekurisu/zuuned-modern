// CLI gate for Zune HD wireless sync: pairing, network enrolment, and the
// one question that decides whether any of it is real - does this player
// answer the wireless properties at all?
//
// Wireless sync is a Zune HD only capability, and the player stores exactly
// one partner.  Running this tool against a player replaces whatever the
// Zune desktop software left there, which is the point of `disable` and the
// reason `status` is the default command: look before you overwrite.
//
//   status                      read the pairing state, change nothing
//   pair                        claim the partnership slot (phase 1)
//   guid                        read the wireless session GUID (0xD221)
//   scan                        list what the player itself can hear
//   wifi <ssid> <passphrase>    enrol one access point
//   full <ssid> <passphrase>    phase 1, then enrol, then read the GUID
//   disable                     forget the wireless pairing
//
// The passphrase is read from the command line and never printed.  A network
// name is printed only because the operator asked for the list; nothing here
// writes any of it to disk.
//
// VERDICT=hd-paired     a Zune HD, and a partnership is already stored
// VERDICT=hd-unpaired   a Zune HD, wireless-capable, nobody paired to it
// VERDICT=no-wifi       the player answered nothing wireless at all: not a
//                       Zune HD, or MTPZ never ran so its key is unknown
// VERDICT=absent        no player on the bus (exit 3)
// VERDICT=failed        a step the player refused (exit 1)
#if defined(_WIN32)
extern "C" void zuuned_win_console_utf8(void);
#endif

#include "zune.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void usage()
{
    printf("usage: wirelesstool [status|pair|guid|scan|wifi <ssid> <pass>"
           "|full <ssid> <pass>|disable]\n");
}

bool printStatus(ZuneDeviceHandle dev)
{
    ZuneWirelessStatus st;

    if (zune_wireless_status(dev, &st) != 0) {
        printf("VERDICT=failed status read: %s\n", zune_get_error());
        return false;
    }

    printf("d401_partner=%d d220_pc_session=%d d221_wireless_session=%d "
           "d225_slot=%d d21c_flags=%d d217_probe=%d device_key=%d\n",
           st.answers_partner, st.answers_pc_session,
           st.answers_wireless_session, st.answers_partner_slot,
           st.answers_sync_flags, st.answers_probe, st.has_device_key);
    if (st.partner_guid[0])
        printf("partner_guid=%s\n", st.partner_guid);
    if (st.session_guid[0])
        printf("session_guid=%s\n", st.session_guid);

    if (!st.answers_probe && !st.answers_partner) {
        /* The honest reading, and the one a tester will hit first: a player
         * that never completed MTPZ has no captured RSA key and answers
         * none of this, which looks identical to a model with no Wi-Fi from
         * out here.  Say both and let the operator decide. */
        printf("VERDICT=no-wifi (no wireless property answered; either this "
               "is not a Zune HD or MTPZ did not authenticate)\n");
        return true;
    }
    if (st.answers_wireless_session && st.session_guid[0]) {
        printf("VERDICT=hd-paired\n");
        return true;
    }
    printf("VERDICT=hd-unpaired\n");
    return true;
}

/* The SSID length marker sits in front of each scan entry, exactly as the
 * desktop software leaves it.  Validation is a length window plus printable
 * ASCII, because the blob has no header to trust. */
void printNetworks(const uint8_t *data, uint32_t len)
{
    uint32_t off = 0;
    int shown = 0;

    while (off + 5 <= len) {
        uint32_t ssid_len;
        memcpy(&ssid_len, data + off, 4);
        if (ssid_len == 0 || ssid_len > 32 || off + 4 + ssid_len > len) {
            off++;
            continue;
        }
        bool printable = true;
        for (uint32_t i = 0; i < ssid_len; i++) {
            const char c = (char)data[off + 4 + i];
            if (c < 32 || c > 126) {
                printable = false;
                break;
            }
        }
        if (!printable) {
            off++;
            continue;
        }
        printf("network %d: \"", ++shown);
        fwrite(data + off + 4, 1, ssid_len, stdout);
        printf("\"\n");
        off += 4 + ssid_len;
    }
    printf("networks=%d payload_bytes=%u\n", shown, len);
}

} // namespace

int main(int argc, char **argv)
{
#if defined(_WIN32)
    zuuned_win_console_utf8();
#endif
    std::string command = "status";
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage();
            return 0;
        }
        command = argv[1];
    }
    const bool needs_network = command == "wifi" || command == "full";
    if (needs_network && argc < 4) {
        usage();
        printf("VERDICT=failed an access point needs both a name and a passphrase\n");
        return 1;
    }

    ZuneDeviceHandle dev = zune_breach();
    if (!dev) {
        printf("VERDICT=absent %s\n", zune_get_error());
        return 3;
    }

    /* Only what the player actually told us at connect time.  Whether MTPZ
     * authenticated is not a thing libzune reports, and `status` answers it
     * honestly by saying whether the device RSA key came out of it. */
    printf("model=%s family=0x%02X\n",
           zune_get_model(dev) ? zune_get_model(dev) : "?",
           (unsigned)zune_get_family(dev));

    int exit_code = 0;
    bool ok = true;

    if (command == "status") {
        ok = printStatus(dev);
    } else if (command == "pair") {
        char guid[64] = { 0 };
        ok = zune_wireless_pair_sync(dev, nullptr, "Windows Zuuned", nullptr,
                                     guid, sizeof(guid)) == 0;
        if (ok)
            printf("partnership=%s\nVERDICT=paired\n", guid);
        else
            printf("VERDICT=failed %s\n", zune_get_error());
    } else if (command == "guid") {
        char guid[64] = { 0 };
        ok = zune_wireless_read_session_guid(dev, guid, sizeof(guid)) == 0;
        if (ok)
            printf("session_guid=%s\nVERDICT=have-guid\n", guid);
        else
            printf("VERDICT=failed %s\n", zune_get_error());
    } else if (command == "scan") {
        uint8_t *payload = nullptr;
        uint32_t len = 0;
        ok = zune_wireless_scan_wifi(dev, &payload, &len) == 0;
        if (ok) {
            printNetworks(payload, len);
            printf("VERDICT=scanned\n");
        } else {
            printf("VERDICT=failed %s\n", zune_get_error());
        }
        free(payload);
    } else if (command == "wifi" || command == "full") {
        const char *ssid = argv[2];
        const char *pass = argv[3];
        char partner[64] = { 0 };
        char session[64] = { 0 };

        if (command == "full") {
            ok = zune_wireless_pair_sync(dev, nullptr, "Windows Zuuned", nullptr,
                                         partner, sizeof(partner)) == 0;
            if (!ok)
                printf("VERDICT=failed phase 1: %s\n", zune_get_error());
        }
        if (ok) {
            ok = zune_wireless_pair_wifi(dev, ssid, pass, partner,
                                         session, sizeof(session)) == 0;
            if (ok)
                printf("session_guid=%s\nVERDICT=enrolled\n", session);
            else
                printf("VERDICT=failed %s\n", zune_get_error());
        }
    } else if (command == "disable") {
        ok = zune_wireless_disable(dev) == 0;
        if (ok)
            printf("VERDICT=disabled\n");
        else
            printf("VERDICT=failed %s\n", zune_get_error());
    } else {
        usage();
        printf("VERDICT=failed unknown command\n");
        zune_sever(dev);
        return 1;
    }

    exit_code = ok ? 0 : 1;
    zune_sever(dev);
    return exit_code;
}
