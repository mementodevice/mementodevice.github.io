#include "dms_crypto.h"
#include "dms_secret.h"

#include "esp_random.h"
#include "esp_log.h"
#include <string.h>

#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/base64.h"

static const char *TAG = "dms_crypto";

static uint8_t  s_secret[DMS_DEVICE_SECRET_LEN];
static bool     s_have_secret = false;

/* fixed salt for the manifest key (device_secret supplies the entropy) */
static const uint8_t MANIFEST_SALT[] = "memento.manifest.key.v1";

bool dms_crypto_init(void)
{
    bool created = false;
    if (!dms_secret_get(s_secret, &created)) {
        ESP_LOGE(TAG, "device_secret unavailable");
        s_have_secret = false;
        return false;
    }
    s_have_secret = true;
    return true;
}

void dms_random(uint8_t *buf, size_t len)
{
    esp_fill_random(buf, len);
}

static bool pbkdf2(const uint8_t *pwd, size_t pwdlen, const uint8_t *salt, size_t saltlen,
                   uint8_t *out, size_t outlen)
{
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, pwd, pwdlen,
                                           salt, saltlen, DMS_KDF_ITERS, outlen, out);
    if (rc != 0) {
        ESP_LOGE(TAG, "pbkdf2 failed: -0x%04x", -rc);
        return false;
    }
    return true;
}

bool dms_kdf_pin(const char *pin, const uint8_t *salt, size_t saltlen,
                 uint8_t *out, size_t outlen)
{
    if (!s_have_secret) return false;
    /* password = pin bytes || device_secret */
    uint8_t pwd[64];
    size_t plen = strlen(pin);
    if (plen + DMS_DEVICE_SECRET_LEN > sizeof(pwd)) return false;
    memcpy(pwd, pin, plen);
    memcpy(pwd + plen, s_secret, DMS_DEVICE_SECRET_LEN);
    bool ok = pbkdf2(pwd, plen + DMS_DEVICE_SECRET_LEN, salt, saltlen, out, outlen);
    memset(pwd, 0, sizeof(pwd));
    return ok;
}

bool dms_manifest_key(uint8_t out[DMS_KEY_LEN])
{
    if (!s_have_secret) return false;
    return pbkdf2(s_secret, DMS_DEVICE_SECRET_LEN,
                  MANIFEST_SALT, sizeof(MANIFEST_SALT) - 1, out, DMS_KEY_LEN);
}

bool dms_sha256(const uint8_t *in, size_t len, uint8_t out[32])
{
    return mbedtls_sha256(in, len, out, 0) == 0;
}

bool dms_gcm_encrypt(const uint8_t key[DMS_KEY_LEN], const uint8_t iv[DMS_IV_LEN],
                     const uint8_t *aad, size_t aadlen,
                     const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[DMS_TAG_LEN])
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = false;
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, DMS_KEY_LEN * 8) == 0) {
        ok = (mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, iv, DMS_IV_LEN,
                                        aad, aadlen, in, out, DMS_TAG_LEN, tag) == 0);
    }
    mbedtls_gcm_free(&gcm);
    return ok;
}

bool dms_gcm_decrypt(const uint8_t key[DMS_KEY_LEN], const uint8_t iv[DMS_IV_LEN],
                     const uint8_t *aad, size_t aadlen,
                     const uint8_t *in, size_t len, const uint8_t tag[DMS_TAG_LEN], uint8_t *out)
{
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = false;
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, DMS_KEY_LEN * 8) == 0) {
        ok = (mbedtls_gcm_auth_decrypt(&gcm, len, iv, DMS_IV_LEN, aad, aadlen,
                                       tag, DMS_TAG_LEN, in, out) == 0);
    }
    mbedtls_gcm_free(&gcm);
    return ok;   /* false on tag mismatch -> tamper/wrong key */
}

bool dms_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

int dms_b64_encode(const uint8_t *in, size_t len, char *out, size_t outcap)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, outcap, &olen, in, len) != 0) return -1;
    return (int)olen;
}

int dms_b64_decode(const char *in, uint8_t *out, size_t outcap)
{
    size_t olen = 0;
    if (mbedtls_base64_decode(out, outcap, &olen, (const unsigned char *)in, strlen(in)) != 0)
        return -1;
    return (int)olen;
}
