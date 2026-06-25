#ifndef DMS_CRYPTO_H
#define DMS_CRYPTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Crypto primitives for the Memento vault.
 *
 *   key material:  KEK / PK  = PBKDF2-HMAC-SHA256(pin || device_secret, salt, iters)
 *   manifest key             = PBKDF2-HMAC-SHA256(device_secret, fixed salt, iters)
 *   bulk encryption          = AES-256-GCM (12-byte IV, 16-byte tag)
 *
 * device_secret is fetched once from NVS (see dms_secret). See README threat model:
 * on a prototype the secret is recoverable from a flash dump, so PINs are an access
 * gate + protection against SD-card-only theft, not protection against device compromise.
 */

#define DMS_KEY_LEN   32   /* AES-256 */
#define DMS_IV_LEN    12
#define DMS_TAG_LEN   16
#define DMS_SALT_LEN  16
#define DMS_VERIFIER_LEN 16
/* PBKDF2 rounds. Kept low (4096) on purpose: in this design device_secret is mixed into
 * every PIN derivation AND each PK is also wrapped under the manifest key, so an attacker
 * with device_secret recovers all content WITHOUT the PIN -- a high iteration count buys
 * almost nothing here, while a low one keeps on-device PIN entry snappy (KDF runs once per
 * stored PIN on each attempt). The real protection is flash encryption (backlog). */
#define DMS_KDF_ITERS 4096

#ifdef __cplusplus
extern "C" {
#endif

/* Loads device_secret from NVS into a static buffer. Call once after nvs_flash_init().
 * Returns false if the secret could not be read/created. */
bool dms_crypto_init(void);

/* Fill buf with cryptographically strong random bytes. */
void dms_random(uint8_t *buf, size_t len);

/* PBKDF2-HMAC-SHA256 over (pin || device_secret) with the given salt -> out[outlen]. */
bool dms_kdf_pin(const char *pin, const uint8_t *salt, size_t saltlen,
                 uint8_t *out, size_t outlen);

/* Manifest key: derived from device_secret alone (PIN-independent). out must be DMS_KEY_LEN. */
bool dms_manifest_key(uint8_t out[DMS_KEY_LEN]);

/* SHA-256(in) -> out[32]. */
bool dms_sha256(const uint8_t *in, size_t len, uint8_t out[32]);

/* AES-256-GCM. out must hold `len` bytes; tag is DMS_TAG_LEN. aad may be NULL. */
bool dms_gcm_encrypt(const uint8_t key[DMS_KEY_LEN], const uint8_t iv[DMS_IV_LEN],
                     const uint8_t *aad, size_t aadlen,
                     const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[DMS_TAG_LEN]);
bool dms_gcm_decrypt(const uint8_t key[DMS_KEY_LEN], const uint8_t iv[DMS_IV_LEN],
                     const uint8_t *aad, size_t aadlen,
                     const uint8_t *in, size_t len, const uint8_t tag[DMS_TAG_LEN], uint8_t *out);

/* Constant-time compare. */
bool dms_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

/* base64. Return bytes written, or -1 on overflow/error. */
int dms_b64_encode(const uint8_t *in, size_t len, char *out, size_t outcap);
int dms_b64_decode(const char *in, uint8_t *out, size_t outcap);

#ifdef __cplusplus
}
#endif

#endif /* DMS_CRYPTO_H */
