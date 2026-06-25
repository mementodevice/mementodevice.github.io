#ifndef DMS_SECRET_H
#define DMS_SECRET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * device_secret: a 32-byte random value bound to THIS physical device.
 *
 * It is mixed into every file-encryption key (key = PBKDF2(pin + device_secret + salt)),
 * so a stolen SD card alone cannot be decrypted -- the matching device is required.
 *
 * PROTOTYPE storage policy:
 *   - Stored in NVS (flash) only. We do NOT enable flash encryption and we NEVER burn
 *     eFuses (both are irreversible). On a prototype the secret is therefore recoverable
 *     from a raw flash dump -- accepted trade-off. Production = enable flash encryption +
 *     secure boot at provisioning; no change to this code is required.
 *
 * Requires nvs_flash_init() to have run before the first call.
 */

#define DMS_NVS_NAMESPACE      "memento_dms"
#define DMS_NVS_KEY_SECRET     "dev_secret"
#define DMS_DEVICE_SECRET_LEN  32

#ifdef __cplusplus
extern "C" {
#endif

/* Loads device_secret into `out` (>= DMS_DEVICE_SECRET_LEN bytes). On first boot it
 * generates a fresh random secret and persists it. If `created` is non-NULL it is set
 * true iff a new secret was generated this call. Returns true on success. */
bool dms_secret_get(uint8_t *out, bool *created);

/* Small persistent u32 counters in the same NVS namespace. These are NOT secret -- they back
 * the wrong-PIN throttle so it survives a reboot or a power cut (a RAM-only counter is reset by
 * exactly the power-cycle an attacker performs between guesses). Key length must be <= 15 chars.
 * _get returns `def` if the key is absent or unreadable; _set persists immediately. */
uint32_t dms_secret_counter_get(const char *key, uint32_t def);
bool     dms_secret_counter_set(const char *key, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif /* DMS_SECRET_H */
