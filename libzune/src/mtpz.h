#ifndef ZUNE_MTPZ_H
#define ZUNE_MTPZ_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ptp.h"

/* Load MTPZ key data from ~/.mtpz-data.
 * Must be called once before mtpz_handshake().
 * Returns 0 on success, -1 if file not found or parse error. */
int mtpz_load_keys(void);

/* Free loaded key data. Call on cleanup. */
void mtpz_free_keys(void);

/* Check if MTPZ keys are loaded and available. */
int mtpz_keys_available(void);

/* Perform the MTPZ authentication handshake.
 * Must be called after ptp_open_session() and before any MTP operations.
 *
 * Steps:
 *   1. Set SessionInitiatorInfo device property
 *   2. Reset handshake (EndTrustedAppSession)
 *   3. Send application certificate
 *   4. Validate handshake response (RSA + AES)
 *   5. Send confirmation
 *   6. Open secure sync session (EnableTrustedFilesOperations)
 *
 * Returns 0 on success, -1 on failure. */
int mtpz_handshake(ptp_session_t *session);

/* The player's RSA-1024 modulus (128 bytes, little-endian) as it arrived
 * inside the authenticated handshake payload. Returns 0 and fills `out`
 * once a handshake has run on a device that supplied one; -1 otherwise.
 * The wireless Wi-Fi passphrase is RSA-encrypted under this key, which is
 * why it outlives the handshake.  `out` may be NULL to ask only whether a
 * handshake on this device actually supplied a key. */
int mtpz_device_rsa_modulus(unsigned char *out);

#ifdef __cplusplus
}
#endif

#endif /* ZUNE_MTPZ_H */
