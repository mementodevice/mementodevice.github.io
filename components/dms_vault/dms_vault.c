#include "dms_vault.h"
#include "dms_crypto.h"

#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h> /* gettimeofday: the ESP32's own clock, used as an independent witness */
#include <unistd.h>   /* fsync */

static const char *TAG = "dms_vault";

#define DMS_DIR_PATH     "/sdcard/dms"
#define DMS_FILES_PATH   "/sdcard/dms/files"
#define DMS_MANIFEST     "/sdcard/dms/manifest.enc"
#define DMS_STATE        "/sdcard/dms/state.enc"
#define DMS_BLOB_MAXLEN  64
#define DMS_MANIFEST_MAX 65536   /* read buffer == write cap; save refuses to exceed this */

/* #4 countdown anti-tamper tuning (see credit_delta) */
#define DMS_MONO_SLACK_SEC       60     /* tolerated clock skew over the monotonic witness          */
#define DMS_FALLBACK_WAKE_CREDIT 3600   /* per-wake credit cap, used ONLY when the witness is        */
                                        /* unavailable (the device was fully power-cycled)           */
#define DMS_TAMPER_JUMP_SEC      3600   /* an external-RTC forward jump this far beyond real elapsed  */
                                        /* time is flagged to the owner as suspected clock tampering  */

static cJSON *s_manifest = NULL;   /* {version,pins:[],files:[],next_pin_id,next_file_seq,recovery?} */
static cJSON *s_state    = NULL;   /* {armed:[{pin,accrued,anchor,manc}],tamper?} */
/* #4: per-wake credit cap. Used only on the fallback path (no monotonic witness, i.e. after a full
 * power cycle); honest operation is bounded by the witness instead. A constant, NOT derived from the
 * editable config -- so tampering with config.json cannot loosen this anti-forward-clock bound. */
static uint32_t s_max_wake_credit = DMS_FALLBACK_WAKE_CREDIT;

/* forward decls (used by pending_check, defined later in the dead-man section) */
static uint32_t entry_elapsed(cJSON *e, uint32_t now);

/* ----------------------------------------------------------------- low-level file I/O */

static int read_all(const char *path, uint8_t *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (int)n;
}

static bool write_all(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGE(TAG, "open for write failed: %s", path); return false; }
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    return n == len;
}

/* Crash-safe write for the manifest/state: bytes go to path.tmp (flushed to media), then we
 * rotate the current file to path.bak and rename path.tmp -> path. A power loss therefore
 * leaves either the previous file (recoverable from .bak) or the complete new one -- never a
 * half-written, unparseable manifest that would otherwise be treated as an empty vault. */
static bool write_all_atomic(const char *path, const uint8_t *buf, size_t len)
{
    char tmp[96], bak[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) { ESP_LOGE(TAG, "open tmp failed: %s", tmp); return false; }
    size_t n = fwrite(buf, 1, len, f);
    fflush(f);
    fsync(fileno(f));               /* force the bytes to the SD/flash before we swap */
    fclose(f);
    if (n != len) { ESP_LOGE(TAG, "short write: %s", tmp); remove(tmp); return false; }

    remove(bak);                    /* drop the old backup (ignore if absent)        */
    rename(path, bak);              /* keep the last good copy as .bak (ok if absent) */
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed", tmp, path);
        rename(bak, path);          /* best-effort restore of the previous file       */
        return false;
    }
    return true;
}

/* Decrypt one container file: [12 iv][16 tag][ciphertext] of a JSON string -> cJSON, or NULL. */
static cJSON *decrypt_container(const char *path)
{
    uint8_t *raw = malloc(DMS_MANIFEST_MAX);
    if (!raw) return NULL;
    int n = read_all(path, raw, DMS_MANIFEST_MAX);
    cJSON *out = NULL;
    if (n >= DMS_MANIFEST_MAX) {   /* filled the whole buffer -> file may be truncated */
        ESP_LOGE(TAG, "%s: larger than %d-byte buffer, refusing (would be truncated)",
                 path, DMS_MANIFEST_MAX);
    } else if (n > (DMS_IV_LEN + DMS_TAG_LEN)) {
        uint8_t mk[DMS_KEY_LEN];
        if (dms_manifest_key(mk)) {
            size_t ctlen = n - DMS_IV_LEN - DMS_TAG_LEN;
            uint8_t *pt = malloc(ctlen + 1);
            if (pt) {
                const uint8_t *iv  = raw;
                const uint8_t *tag = raw + DMS_IV_LEN;
                const uint8_t *ct  = raw + DMS_IV_LEN + DMS_TAG_LEN;
                if (dms_gcm_decrypt(mk, iv, NULL, 0, ct, ctlen, tag, pt)) {
                    pt[ctlen] = '\0';
                    out = cJSON_Parse((const char *)pt);
                } else {
                    ESP_LOGE(TAG, "%s: decrypt/auth failed (tamper or wrong device)", path);
                }
                free(pt);
            }
        }
    }
    free(raw);
    return out;
}

/* Load a container, transparently falling back to the .bak left by write_all_atomic if the
 * primary file is missing/corrupt (e.g. a power loss happened during the previous write). */
static cJSON *load_encrypted(const char *path)
{
    cJSON *out = decrypt_container(path);
    if (!out) {
        char bak[96];
        snprintf(bak, sizeof(bak), "%s.bak", path);
        out = decrypt_container(bak);
        if (out) ESP_LOGW(TAG, "%s: recovered from backup (.bak)", path);
    }
    return out;
}

static bool save_encrypted(const char *path, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    if (!str) return false;
    size_t len = strlen(str);
    bool ok = false;
    size_t total = DMS_IV_LEN + DMS_TAG_LEN + len;
    if (total > DMS_MANIFEST_MAX) {
        /* Refuse rather than write something we can't read back (would look like an empty
         * vault on the next boot). Existing on-disk data stays intact. */
        ESP_LOGE(TAG, "%s: %u B exceeds %d-byte limit; not saving", path,
                 (unsigned)total, DMS_MANIFEST_MAX);
        cJSON_free(str);
        return false;
    }
    uint8_t mk[DMS_KEY_LEN];
    uint8_t *blob = malloc(total);
    if (blob && dms_manifest_key(mk)) {
        uint8_t *iv  = blob;
        uint8_t *tag = blob + DMS_IV_LEN;
        uint8_t *ct  = blob + DMS_IV_LEN + DMS_TAG_LEN;
        dms_random(iv, DMS_IV_LEN);
        if (dms_gcm_encrypt(mk, iv, NULL, 0, (const uint8_t *)str, len, ct, tag)) {
            ok = write_all_atomic(path, blob, total);
        }
    }
    free(blob);
    cJSON_free(str);
    return ok;
}

/* ----------------------------------------------------------------- cJSON b64 helpers */

static void add_b64(cJSON *o, const char *key, const uint8_t *bytes, size_t len)
{
    char b64[256];
    int n = dms_b64_encode(bytes, len, b64, sizeof(b64));
    if (n >= 0) { b64[n] = '\0'; cJSON_AddStringToObject(o, key, b64); }
}

static int get_b64(const cJSON *o, const char *key, uint8_t *out, size_t cap)
{
    const cJSON *it = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsString(it)) return -1;
    return dms_b64_decode(it->valuestring, out, cap);
}

/* ----------------------------------------------------------------- manifest accessors */

/* NULL-safe integer field read: returns def if the key is missing or not a number, so a
 * malformed manifest entry is skipped rather than dereferencing NULL and crashing. */
static int jint(const cJSON *o, const char *key, int def)
{
    const cJSON *it = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(it) ? it->valueint : def;
}

/* Copy a pin's "name" label into dst[DMS_NAME_MAX] ("" if unset/not a string). */
static void copy_pin_name(const cJSON *o, char *dst)
{
    const cJSON *n = cJSON_GetObjectItem(o, "name");
    if (cJSON_IsString(n) && n->valuestring) {
        strncpy(dst, n->valuestring, DMS_NAME_MAX - 1);
        dst[DMS_NAME_MAX - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

static cJSON *pins_arr(void)  { return cJSON_GetObjectItem(s_manifest, "pins"); }
static cJSON *files_arr(void) { return cJSON_GetObjectItem(s_manifest, "files"); }

static cJSON *find_pin(int id)
{
    cJSON *it;
    cJSON_ArrayForEach(it, pins_arr()) {
        if (jint(it, "id", -1) == id) return it;
    }
    return NULL;
}

static cJSON *find_file(const char *name)
{
    cJSON *it;
    cJSON_ArrayForEach(it, files_arr()) {
        const cJSON *n = cJSON_GetObjectItem(it, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0) return it;
    }
    return NULL;
}

static int master_pin_id(void)
{
    cJSON *it;
    cJSON_ArrayForEach(it, pins_arr()) {
        if (jint(it, "role", -1) == DMS_ROLE_MASTER)
            return jint(it, "id", -1);
    }
    return -1;
}

/* Recover a PIN's PK (32 bytes) from its manifest-key-wrapped copy. */
static bool recover_pk(const cJSON *pin, uint8_t pk[DMS_KEY_LEN])
{
    uint8_t mk[DMS_KEY_LEN], iv[DMS_IV_LEN], tag[DMS_TAG_LEN], wrapped[DMS_KEY_LEN];
    if (!dms_manifest_key(mk)) return false;
    if (get_b64(pin, "pk_iv", iv, sizeof(iv)) != DMS_IV_LEN) return false;
    if (get_b64(pin, "pk_tag", tag, sizeof(tag)) != DMS_TAG_LEN) return false;
    if (get_b64(pin, "pk_wrapped", wrapped, sizeof(wrapped)) != DMS_KEY_LEN) return false;
    return dms_gcm_decrypt(mk, iv, NULL, 0, wrapped, DMS_KEY_LEN, tag, pk);
}

/* ----------------------------------------------------------------- init / load */

bool dms_vault_init(void)
{
    mkdir(DMS_DIR_PATH, 0775);
    mkdir(DMS_FILES_PATH, 0775);

    s_manifest = load_encrypted(DMS_MANIFEST);
    if (!s_manifest) {
        ESP_LOGW(TAG, "no manifest; creating empty vault");
        s_manifest = cJSON_CreateObject();
        cJSON_AddNumberToObject(s_manifest, "version", 1);
        cJSON_AddItemToObject(s_manifest, "pins", cJSON_CreateArray());
        cJSON_AddItemToObject(s_manifest, "files", cJSON_CreateArray());
        cJSON_AddNumberToObject(s_manifest, "next_pin_id", 0);
        cJSON_AddNumberToObject(s_manifest, "next_file_seq", 0);
        if (!save_encrypted(DMS_MANIFEST, s_manifest)) return false;
    }

    s_state = load_encrypted(DMS_STATE);
    if (!s_state) {
        s_state = cJSON_CreateObject();
        cJSON_AddItemToObject(s_state, "armed", cJSON_CreateArray());
        save_encrypted(DMS_STATE, s_state);
    }

    /* Seed the default master PIN on first run so a fresh device is usable out of the box.
     * The master PIN is "123" until the owner changes it via the web UI (the UI can change
     * the master PIN but never add/delete it, so a default must exist). */
    if (master_pin_id() < 0) {
        int id = -1;
        if (dms_vault_add_pin("123", DMS_ROLE_MASTER, 0, &id)) {
            dms_vault_set_pin_name(id, "Master");
            ESP_LOGW(TAG, "seeded default master PIN (id %d) - change it via the web UI", id);
        } else {
            ESP_LOGE(TAG, "failed to seed default master PIN");
        }
    }
    return true;
}

bool dms_vault_has_master(void)
{
    return s_manifest && master_pin_id() >= 0;
}

/* ----------------------------------------------------------------- pin verify */

dms_role_t dms_vault_verify_pin(const char *pin, dms_pin_t *out)
{
    if (!s_manifest) return DMS_ROLE_NONE;
    cJSON *it;
    cJSON_ArrayForEach(it, pins_arr()) {
        uint8_t salt[DMS_SALT_LEN], pk[DMS_KEY_LEN], hash[32], verifier[DMS_VERIFIER_LEN];
        if (get_b64(it, "salt", salt, sizeof(salt)) != DMS_SALT_LEN) continue;
        if (get_b64(it, "verifier", verifier, sizeof(verifier)) != DMS_VERIFIER_LEN) continue;
        if (!dms_kdf_pin(pin, salt, DMS_SALT_LEN, pk, sizeof(pk))) continue;
        if (!dms_sha256(pk, sizeof(pk), hash)) continue;
        if (dms_ct_equal(hash, verifier, DMS_VERIFIER_LEN)) {
            if (out) {
                out->id            = jint(it, "id", -1);
                out->role          = (dms_role_t)jint(it, "role", DMS_ROLE_NONE);
                cJSON *cd          = cJSON_GetObjectItem(it, "countdown");
                out->countdown_sec = cJSON_IsNumber(cd) ? (uint32_t)cd->valuedouble : 0;
                copy_pin_name(it, out->name);
            }
            return (dms_role_t)jint(it, "role", DMS_ROLE_NONE);
        }
    }
    return DMS_ROLE_NONE;
}

/* ----------------------------------------------------------------- file listing/read */

static bool file_has_pin(const cJSON *file, int pin_id)
{
    cJSON *k;
    cJSON_ArrayForEach(k, cJSON_GetObjectItem(file, "keys")) {
        if (jint(k, "pin", -1) == pin_id) return true;
    }
    return false;
}

int dms_vault_files_for_pin(int pin_id, char names[][DMS_NAME_MAX], int maxn)
{
    int n = 0;
    cJSON *f;
    cJSON_ArrayForEach(f, files_arr()) {
        if (n >= maxn) break;
        if (file_has_pin(f, pin_id)) {
            const cJSON *nm = cJSON_GetObjectItem(f, "name");
            if (cJSON_IsString(nm)) {
                strncpy(names[n], nm->valuestring, DMS_NAME_MAX - 1);
                names[n][DMS_NAME_MAX - 1] = '\0';
                n++;
            }
        }
    }
    return n;
}

static cJSON *file_key_for_pin(cJSON *file, int pin_id)
{
    cJSON *k;
    cJSON_ArrayForEach(k, cJSON_GetObjectItem(file, "keys"))
        if (jint(k, "pin", -1) == pin_id) return k;
    return NULL;
}

/* Unwrap the file's content key with `pk`, then read + decrypt the blob into buf. */
static bool decrypt_with_pk(cJSON *file, cJSON *keyent, const uint8_t pk[DMS_KEY_LEN],
                            char *buf, size_t cap, size_t *outlen)
{
    uint8_t kiv[DMS_IV_LEN], ktag[DMS_TAG_LEN], wrapped[DMS_KEY_LEN], ck[DMS_KEY_LEN];
    if (get_b64(keyent, "iv", kiv, sizeof(kiv)) != DMS_IV_LEN) return false;
    if (get_b64(keyent, "tag", ktag, sizeof(ktag)) != DMS_TAG_LEN) return false;
    if (get_b64(keyent, "wrapped", wrapped, sizeof(wrapped)) != DMS_KEY_LEN) return false;
    if (!dms_gcm_decrypt(pk, kiv, NULL, 0, wrapped, DMS_KEY_LEN, ktag, ck)) return false;

    const cJSON *blob = cJSON_GetObjectItem(file, "blob");
    int len = jint(file, "len", 0);
    if (!cJSON_IsString(blob) || len <= 0 || (size_t)len > cap) return false;

    uint8_t civ[DMS_IV_LEN], ctag[DMS_TAG_LEN];
    if (get_b64(file, "iv", civ, sizeof(civ)) != DMS_IV_LEN) return false;
    if (get_b64(file, "tag", ctag, sizeof(ctag)) != DMS_TAG_LEN) return false;

    uint8_t *ct = malloc(len);
    if (!ct) return false;
    char path[DMS_BLOB_MAXLEN + 16];
    snprintf(path, sizeof(path), "/sdcard/dms/%s", blob->valuestring);
    int got = read_all(path, ct, len);
    bool ok = (got == len) &&
              dms_gcm_decrypt(ck, civ, NULL, 0, ct, len, ctag, (uint8_t *)buf);
    free(ct);
    if (ok && outlen) *outlen = (size_t)len;
    return ok;
}

bool dms_vault_read_file(const char *pin, int pin_id, const char *name,
                         char *buf, size_t cap, size_t *outlen)
{
    cJSON *file = find_file(name);
    if (!file) return false;
    cJSON *keyent = file_key_for_pin(file, pin_id);
    if (!keyent) return false;
    cJSON *pinobj = find_pin(pin_id);
    if (!pinobj) return false;

    uint8_t salt[DMS_SALT_LEN], pk[DMS_KEY_LEN];
    if (get_b64(pinobj, "salt", salt, sizeof(salt)) != DMS_SALT_LEN) return false;
    if (!dms_kdf_pin(pin, salt, DMS_SALT_LEN, pk, sizeof(pk))) return false;
    return decrypt_with_pk(file, keyent, pk, buf, cap, outlen);
}

int dms_vault_pending_check(uint32_t now_unix, int *pin_id, uint32_t *remaining_sec)
{
    if (!s_state) return 0;
    cJSON *armed = cJSON_GetObjectItem(s_state, "armed");
    int      due_pin  = -1;                          /* completed countdown not yet viewed */
    int      pend_pin = -1;                           /* soonest still-counting entry       */
    uint32_t best_rem = 0xFFFFFFFFu;
    cJSON *e;
    cJSON_ArrayForEach(e, armed) {
        int pid = jint(e, "pin", -1);
        cJSON *pinobj = find_pin(pid);
        if (!pinobj) continue;                       /* pin was deleted / bad entry */
        cJSON *cd = cJSON_GetObjectItem(pinobj, "countdown");
        uint32_t countdown = cJSON_IsNumber(cd) ? (uint32_t)cd->valuedouble : 0;
        uint32_t elapsed = entry_elapsed(e, now_unix);
        if (elapsed >= countdown) {
            /* Completed. Once an alt has actually viewed its files (revealed=1) we stop
             * reporting it as "due" -- otherwise that one finished alt would permanently
             * mask any OTHER alt that is still counting down (the 222-hides-111 bug). */
            if (!jint(e, "revealed", 0) && due_pin < 0) due_pin = pid;
        } else {
            uint32_t rem = countdown - elapsed;
            if (rem < best_rem) { best_rem = rem; pend_pin = pid; }
        }
    }
    if (due_pin >= 0) {                              /* a fresh reveal takes priority */
        if (pin_id) *pin_id = due_pin;
        if (remaining_sec) *remaining_sec = 0;
        return 2;
    }
    if (pend_pin >= 0) {
        if (pin_id) *pin_id = pend_pin;
        if (remaining_sec) *remaining_sec = best_rem;
        return 1;
    }
    return 0;
}

/* ----------------------------------------------------------------- management */

bool dms_vault_add_pin(const char *pin, dms_role_t role, uint32_t countdown_sec, int *new_id)
{
    if (!s_manifest) return false;
    if (cJSON_GetArraySize(pins_arr()) >= DMS_MAX_PINS) return false;
    if (role == DMS_ROLE_MASTER && master_pin_id() >= 0) {
        ESP_LOGE(TAG, "master already exists"); return false;
    }
    if (dms_vault_verify_pin(pin, NULL) != DMS_ROLE_NONE) {
        ESP_LOGW(TAG, "PIN already in use; refusing duplicate"); return false;
    }

    uint8_t salt[DMS_SALT_LEN], pk[DMS_KEY_LEN], hash[32];
    dms_random(salt, sizeof(salt));
    if (!dms_kdf_pin(pin, salt, sizeof(salt), pk, sizeof(pk))) return false;
    if (!dms_sha256(pk, sizeof(pk), hash)) return false;

    /* wrap PK under the manifest key so management can re-wrap content keys later */
    uint8_t mk[DMS_KEY_LEN], pk_iv[DMS_IV_LEN], pk_tag[DMS_TAG_LEN], pk_wrapped[DMS_KEY_LEN];
    if (!dms_manifest_key(mk)) return false;
    dms_random(pk_iv, sizeof(pk_iv));
    if (!dms_gcm_encrypt(mk, pk_iv, NULL, 0, pk, DMS_KEY_LEN, pk_wrapped, pk_tag)) return false;

    int id = cJSON_GetObjectItem(s_manifest, "next_pin_id")->valueint;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "id", id);
    cJSON_AddNumberToObject(p, "role", role);
    cJSON_AddNumberToObject(p, "countdown", countdown_sec);
    add_b64(p, "salt", salt, sizeof(salt));
    add_b64(p, "verifier", hash, DMS_VERIFIER_LEN);
    add_b64(p, "pk_iv", pk_iv, sizeof(pk_iv));
    add_b64(p, "pk_tag", pk_tag, sizeof(pk_tag));
    add_b64(p, "pk_wrapped", pk_wrapped, sizeof(pk_wrapped));
    cJSON_AddItemToArray(pins_arr(), p);

    cJSON_SetNumberValue(cJSON_GetObjectItem(s_manifest, "next_pin_id"), id + 1);
    if (new_id) *new_id = id;
    return save_encrypted(DMS_MANIFEST, s_manifest);
}

bool dms_vault_del_pin(int pin_id)
{
    if (!s_manifest) return false;
    /* remove key entries referencing this pin from every file */
    cJSON *f;
    cJSON_ArrayForEach(f, files_arr()) {
        cJSON *keys = cJSON_GetObjectItem(f, "keys");
        for (int i = cJSON_GetArraySize(keys) - 1; i >= 0; i--) {
            cJSON *k = cJSON_GetArrayItem(keys, i);
            if (jint(k, "pin", -1) == pin_id)
                cJSON_DeleteItemFromArray(keys, i);
        }
    }
    /* remove the pin */
    cJSON *pins = pins_arr();
    for (int i = cJSON_GetArraySize(pins) - 1; i >= 0; i--) {
        if (jint(cJSON_GetArrayItem(pins, i), "id", -1) == pin_id)
            cJSON_DeleteItemFromArray(pins, i);
    }
    /* drop any armed entry too */
    cJSON *armed = cJSON_GetObjectItem(s_state, "armed");
    for (int i = cJSON_GetArraySize(armed) - 1; i >= 0; i--) {
        if (jint(cJSON_GetArrayItem(armed, i), "pin", -1) == pin_id)
            cJSON_DeleteItemFromArray(armed, i);
    }
    save_encrypted(DMS_STATE, s_state);
    return save_encrypted(DMS_MANIFEST, s_manifest);
}

/* Change the master PIN in place, keeping access to every existing file. The master's
 * content-key wraps are re-encrypted from the OLD PK to the NEW one; the old PK is recovered
 * via the manifest key, so the old PIN is not required. Refuses if new_pin is already used by
 * a different PIN. (The master is never added/removed from the web UI, only re-keyed here.) */
bool dms_vault_change_master_pin(const char *new_pin)
{
    if (!s_manifest || !new_pin || !new_pin[0]) return false;
    int mid = master_pin_id();
    if (mid < 0) return false;
    cJSON *mp = find_pin(mid);
    if (!mp) return false;

    dms_pin_t hit;
    if (dms_vault_verify_pin(new_pin, &hit) != DMS_ROLE_NONE && hit.id != mid) {
        ESP_LOGW(TAG, "new master PIN already in use by pin %d", hit.id);
        return false;
    }

    uint8_t old_pk[DMS_KEY_LEN];
    if (!recover_pk(mp, old_pk)) return false;

    /* new salt + PK + verifier + manifest-wrapped PK */
    uint8_t salt[DMS_SALT_LEN], new_pk[DMS_KEY_LEN], hash[32];
    dms_random(salt, sizeof(salt));
    if (!dms_kdf_pin(new_pin, salt, sizeof(salt), new_pk, sizeof(new_pk))) return false;
    if (!dms_sha256(new_pk, sizeof(new_pk), hash)) return false;
    uint8_t mk[DMS_KEY_LEN], pk_iv[DMS_IV_LEN], pk_tag[DMS_TAG_LEN], pk_wrapped[DMS_KEY_LEN];
    if (!dms_manifest_key(mk)) return false;
    dms_random(pk_iv, sizeof(pk_iv));
    if (!dms_gcm_encrypt(mk, pk_iv, NULL, 0, new_pk, DMS_KEY_LEN, pk_wrapped, pk_tag)) return false;

    /* re-wrap every file's master content key: old_pk -> new_pk */
    cJSON *f;
    cJSON_ArrayForEach(f, files_arr()) {
        cJSON *ke = file_key_for_pin(f, mid);
        if (!ke) continue;
        uint8_t kiv[DMS_IV_LEN], ktag[DMS_TAG_LEN], wrapped[DMS_KEY_LEN], ck[DMS_KEY_LEN];
        if (get_b64(ke, "iv", kiv, sizeof(kiv)) != DMS_IV_LEN) continue;
        if (get_b64(ke, "tag", ktag, sizeof(ktag)) != DMS_TAG_LEN) continue;
        if (get_b64(ke, "wrapped", wrapped, sizeof(wrapped)) != DMS_KEY_LEN) continue;
        if (!dms_gcm_decrypt(old_pk, kiv, NULL, 0, wrapped, DMS_KEY_LEN, ktag, ck)) continue;
        uint8_t niv[DMS_IV_LEN], ntag[DMS_TAG_LEN], nwrapped[DMS_KEY_LEN];
        dms_random(niv, sizeof(niv));
        if (!dms_gcm_encrypt(new_pk, niv, NULL, 0, ck, DMS_KEY_LEN, nwrapped, ntag)) continue;
        cJSON_DeleteItemFromObject(ke, "iv");
        cJSON_DeleteItemFromObject(ke, "tag");
        cJSON_DeleteItemFromObject(ke, "wrapped");
        add_b64(ke, "iv", niv, sizeof(niv));
        add_b64(ke, "tag", ntag, sizeof(ntag));
        add_b64(ke, "wrapped", nwrapped, sizeof(nwrapped));
    }

    /* swap in the new master key material */
    cJSON_DeleteItemFromObject(mp, "salt");
    cJSON_DeleteItemFromObject(mp, "verifier");
    cJSON_DeleteItemFromObject(mp, "pk_iv");
    cJSON_DeleteItemFromObject(mp, "pk_tag");
    cJSON_DeleteItemFromObject(mp, "pk_wrapped");
    add_b64(mp, "salt", salt, sizeof(salt));
    add_b64(mp, "verifier", hash, DMS_VERIFIER_LEN);
    add_b64(mp, "pk_iv", pk_iv, sizeof(pk_iv));
    add_b64(mp, "pk_tag", pk_tag, sizeof(pk_tag));
    add_b64(mp, "pk_wrapped", pk_wrapped, sizeof(pk_wrapped));

    ESP_LOGW(TAG, "master PIN changed (id %d, re-keyed)", mid);
    return save_encrypted(DMS_MANIFEST, s_manifest);
}

int dms_vault_list_pins(dms_pin_t *out, int maxn)
{
    int n = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, pins_arr()) {
        if (n >= maxn) break;
        out[n].id            = jint(it, "id", -1);
        out[n].role          = (dms_role_t)jint(it, "role", DMS_ROLE_NONE);
        cJSON *cd            = cJSON_GetObjectItem(it, "countdown");
        out[n].countdown_sec = cJSON_IsNumber(cd) ? (uint32_t)cd->valuedouble : 0;
        copy_pin_name(it, out[n].name);
        n++;
    }
    return n;
}

/* Set/replace a PIN's display label (any role). Empty string clears it. */
bool dms_vault_set_pin_name(int pin_id, const char *name)
{
    if (!s_manifest) return false;
    cJSON *p = find_pin(pin_id);
    if (!p) return false;
    cJSON_DeleteItemFromObject(p, "name");        /* no-op if absent */
    if (name && name[0]) {
        char buf[DMS_NAME_MAX];
        strncpy(buf, name, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        cJSON_AddStringToObject(p, "name", buf);
    }
    return save_encrypted(DMS_MANIFEST, s_manifest);
}

/* wrap content key CK for one pin -> append a key entry to `keys` */
static bool wrap_for_pin(cJSON *keys, const cJSON *pin, const uint8_t ck[DMS_KEY_LEN])
{
    uint8_t pk[DMS_KEY_LEN], iv[DMS_IV_LEN], tag[DMS_TAG_LEN], wrapped[DMS_KEY_LEN];
    if (!recover_pk(pin, pk)) return false;
    dms_random(iv, sizeof(iv));
    if (!dms_gcm_encrypt(pk, iv, NULL, 0, ck, DMS_KEY_LEN, wrapped, tag)) return false;
    cJSON *k = cJSON_CreateObject();
    cJSON_AddNumberToObject(k, "pin", jint(pin, "id", -1));
    add_b64(k, "iv", iv, sizeof(iv));
    add_b64(k, "tag", tag, sizeof(tag));
    add_b64(k, "wrapped", wrapped, sizeof(wrapped));
    cJSON_AddItemToArray(keys, k);
    return true;
}

bool dms_vault_add_file(const char *name, const char *content, size_t len,
                        const int *pin_ids, int npins)
{
    if (!s_manifest) return false;
    if (len == 0 || len > DMS_FILE_MAX_BYTES) return false;
    if (cJSON_GetArraySize(files_arr()) >= DMS_MAX_FILES) return false;
    if (find_file(name)) { ESP_LOGE(TAG, "file exists: %s", name); return false; }

    /* random content key, encrypt content -> blob */
    uint8_t ck[DMS_KEY_LEN], civ[DMS_IV_LEN], ctag[DMS_TAG_LEN];
    dms_random(ck, sizeof(ck));
    dms_random(civ, sizeof(civ));
    uint8_t *ct = malloc(len);
    if (!ct) return false;
    if (!dms_gcm_encrypt(ck, civ, NULL, 0, (const uint8_t *)content, len, ct, ctag)) {
        free(ct); return false;
    }

    int seq = cJSON_GetObjectItem(s_manifest, "next_file_seq")->valueint;
    char blob[DMS_BLOB_MAXLEN], path[DMS_BLOB_MAXLEN + 16];
    snprintf(blob, sizeof(blob), "files/%04d.enc", seq);
    snprintf(path, sizeof(path), "/sdcard/dms/%s", blob);
    bool wrote = write_all(path, ct, len);
    free(ct);
    if (!wrote) return false;

    cJSON *file = cJSON_CreateObject();
    cJSON_AddStringToObject(file, "name", name);
    cJSON_AddStringToObject(file, "blob", blob);
    cJSON_AddNumberToObject(file, "len", (double)len);
    add_b64(file, "iv", civ, sizeof(civ));
    add_b64(file, "tag", ctag, sizeof(ctag));
    cJSON *keys = cJSON_CreateArray();
    cJSON_AddItemToObject(file, "keys", keys);

    /* always include the master so the owner can read everything */
    int mid = master_pin_id();
    bool master_done = false;
    for (int i = 0; i < npins; i++) {
        cJSON *pin = find_pin(pin_ids[i]);
        if (pin && wrap_for_pin(keys, pin, ck)) {
            if (pin_ids[i] == mid) master_done = true;
        }
    }
    if (!master_done && mid >= 0) {
        cJSON *mp = find_pin(mid);
        if (mp) wrap_for_pin(keys, mp, ck);
    }

    cJSON_AddItemToArray(files_arr(), file);
    cJSON_SetNumberValue(cJSON_GetObjectItem(s_manifest, "next_file_seq"), seq + 1);
    return save_encrypted(DMS_MANIFEST, s_manifest);
}

bool dms_vault_del_file(const char *name)
{
    if (!s_manifest) return false;
    cJSON *file = find_file(name);
    if (!file) return false;
    const cJSON *blob = cJSON_GetObjectItem(file, "blob");
    if (cJSON_IsString(blob)) {
        char path[DMS_BLOB_MAXLEN + 16];
        snprintf(path, sizeof(path), "/sdcard/dms/%s", blob->valuestring);
        remove(path);
    }
    cJSON *files = files_arr();
    for (int i = cJSON_GetArraySize(files) - 1; i >= 0; i--) {
        cJSON *f = cJSON_GetArrayItem(files, i);
        const cJSON *n = cJSON_GetObjectItem(f, "name");
        if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
            cJSON_DeleteItemFromArray(files, i);
    }
    return save_encrypted(DMS_MANIFEST, s_manifest);
}

bool dms_vault_file_has_reader(const char *name, int pin_id)
{
    cJSON *f = find_file(name);
    return f ? file_has_pin(f, pin_id) : false;
}

/* Re-key a file for a new reader set. The content key is recovered via the master's wrapped
 * copy (no PIN needed), then re-wrapped for the master + each selected alt. */
bool dms_vault_set_file_readers(const char *name, const int *pin_ids, int npins)
{
    if (!s_manifest) return false;
    cJSON *file = find_file(name);
    if (!file) return false;
    int mid = master_pin_id();
    if (mid < 0) return false;
    cJSON *mp = find_pin(mid);
    cJSON *mke = file_key_for_pin(file, mid);
    if (!mp || !mke) return false;

    /* recover the content key with the master PK */
    uint8_t mpk[DMS_KEY_LEN], kiv[DMS_IV_LEN], ktag[DMS_TAG_LEN], wrapped[DMS_KEY_LEN], ck[DMS_KEY_LEN];
    if (!recover_pk(mp, mpk)) return false;
    if (get_b64(mke, "iv", kiv, sizeof(kiv)) != DMS_IV_LEN) return false;
    if (get_b64(mke, "tag", ktag, sizeof(ktag)) != DMS_TAG_LEN) return false;
    if (get_b64(mke, "wrapped", wrapped, sizeof(wrapped)) != DMS_KEY_LEN) return false;
    if (!dms_gcm_decrypt(mpk, kiv, NULL, 0, wrapped, DMS_KEY_LEN, ktag, ck)) return false;

    /* rebuild the keys array: master always, plus each requested alt */
    cJSON *keys = cJSON_CreateArray();
    wrap_for_pin(keys, mp, ck);
    for (int i = 0; i < npins; i++) {
        if (pin_ids[i] == mid) continue;            /* master already included */
        cJSON *pin = find_pin(pin_ids[i]);
        if (pin) wrap_for_pin(keys, pin, ck);
    }
    cJSON_ReplaceItemInObject(file, "keys", keys);
    return save_encrypted(DMS_MANIFEST, s_manifest);
}

/* Read a note's plaintext WITHOUT a PIN, for master management (e.g. editing): recover the
 * master's PK via the manifest key, unwrap the file's content key, decrypt the blob. */
bool dms_vault_read_file_mgmt(const char *name, char *buf, size_t cap, size_t *outlen)
{
    if (!s_manifest) return false;
    cJSON *file = find_file(name);
    if (!file) return false;
    int mid = master_pin_id();
    if (mid < 0) return false;
    cJSON *mp  = find_pin(mid);
    cJSON *mke = file_key_for_pin(file, mid);
    if (!mp || !mke) return false;
    uint8_t mpk[DMS_KEY_LEN];
    if (!recover_pk(mp, mpk)) return false;
    return decrypt_with_pk(file, mke, mpk, buf, cap, outlen);
}

/* Replace a note's CONTENT, keeping its current reader set. Crash-safe: a fresh random content key
 * encrypts the new text into a NEW blob, the manifest is then atomically pointed at it, and only
 * after that is the OLD blob removed. So a power loss mid-save leaves the old blob + old manifest
 * intact (the note is never lost). No PINs are required (uses the manifest-key path). */
bool dms_vault_set_file_content(const char *name, const char *content, size_t len)
{
    if (!s_manifest) return false;
    if (len == 0 || len > DMS_FILE_MAX_BYTES) return false;
    cJSON *file = find_file(name);
    if (!file) return false;
    const cJSON *blobj = cJSON_GetObjectItem(file, "blob");
    if (!cJSON_IsString(blobj)) return false;
    char oldblob[DMS_BLOB_MAXLEN];                 /* remember the old blob to delete after commit */
    strncpy(oldblob, blobj->valuestring, sizeof(oldblob) - 1);
    oldblob[sizeof(oldblob) - 1] = '\0';

    /* capture the current reader pin ids */
    int ids[DMS_MAX_PINS]; int n = 0;
    cJSON *k;
    cJSON_ArrayForEach(k, cJSON_GetObjectItem(file, "keys")) {
        int pid = jint(k, "pin", -1);
        if (pid >= 0 && n < DMS_MAX_PINS) ids[n++] = pid;
    }

    /* fresh content key; encrypt into a NEW blob file (next_file_seq), never overwriting the old */
    uint8_t ck[DMS_KEY_LEN], civ[DMS_IV_LEN], ctag[DMS_TAG_LEN];
    dms_random(ck, sizeof(ck));
    dms_random(civ, sizeof(civ));
    uint8_t *ct = malloc(len);
    if (!ct) return false;
    if (!dms_gcm_encrypt(ck, civ, NULL, 0, (const uint8_t *)content, len, ct, ctag)) { free(ct); return false; }
    cJSON *seqo = cJSON_GetObjectItem(s_manifest, "next_file_seq");
    int seq = cJSON_IsNumber(seqo) ? seqo->valueint : 0;
    char newblob[DMS_BLOB_MAXLEN], newpath[DMS_BLOB_MAXLEN + 16];
    snprintf(newblob, sizeof(newblob), "files/%04d.enc", seq);
    snprintf(newpath, sizeof(newpath), "/sdcard/dms/%s", newblob);
    bool wrote = write_all(newpath, ct, len);
    free(ct);
    if (!wrote) return false;

    /* point the manifest at the new blob + new key material */
    cJSON_DeleteItemFromObject(file, "blob");
    cJSON_AddStringToObject(file, "blob", newblob);
    cJSON_DeleteItemFromObject(file, "iv");
    cJSON_DeleteItemFromObject(file, "tag");
    add_b64(file, "iv", civ, sizeof(civ));
    add_b64(file, "tag", ctag, sizeof(ctag));
    cJSON_DeleteItemFromObject(file, "len");
    cJSON_AddNumberToObject(file, "len", (double)len);

    /* re-wrap the new content key for the same readers (always include the master) */
    int mid = master_pin_id();
    bool master_done = false;
    cJSON *keys = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *pin = find_pin(ids[i]);
        if (pin && wrap_for_pin(keys, pin, ck) && ids[i] == mid) master_done = true;
    }
    if (!master_done && mid >= 0) { cJSON *mp = find_pin(mid); if (mp) wrap_for_pin(keys, mp, ck); }
    cJSON_ReplaceItemInObject(file, "keys", keys);
    if (seqo) cJSON_SetNumberValue(seqo, seq + 1);

    if (!save_encrypted(DMS_MANIFEST, s_manifest)) {
        remove(newpath);                  /* on-disk manifest still points at the old blob -> safe */
        return false;
    }
    /* committed: the new blob is live, drop the now-orphaned old one */
    if (strcmp(oldblob, newblob) != 0) {
        char oldpath[DMS_BLOB_MAXLEN + 16];
        snprintf(oldpath, sizeof(oldpath), "/sdcard/dms/%s", oldblob);
        remove(oldpath);
    }
    return true;
}

int dms_vault_list_all_files(char names[][DMS_NAME_MAX], int maxn)
{
    int n = 0;
    cJSON *f;
    cJSON_ArrayForEach(f, files_arr()) {
        if (n >= maxn) break;
        const cJSON *nm = cJSON_GetObjectItem(f, "name");
        if (cJSON_IsString(nm)) {
            strncpy(names[n], nm->valuestring, DMS_NAME_MAX - 1);
            names[n][DMS_NAME_MAX - 1] = '\0';
            n++;
        }
    }
    return n;
}

/* ----------------------------------------------------------------- dead-man state */

static void set_num(cJSON *o, const char *k, double v)
{
    cJSON *it = cJSON_GetObjectItem(o, k);
    if (it) cJSON_SetNumberValue(it, v);
    else    cJSON_AddNumberToObject(o, k, v);
}

/* Independent monotonic witness: seconds since power-on from the ESP32's OWN clock. It is RTC-timer
 * backed (CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER), so it keeps advancing across deep sleep, and it is
 * reset only by a full power loss. Crucially it is NEVER set from the external PCF85063 (we only
 * READ that RTC, never settimeofday), so an attacker who fast-forwards the I2C RTC cannot advance
 * this witness. We use the DIFFERENCE between two readings, so any constant offset cancels. */
static uint32_t mono_now(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (uint32_t)tv.tv_sec;
}

/* Time to credit an armed entry for the gap (anchor -> now), bounded so the countdown cannot be
 * fast-forwarded:
 *  - backward external-RTC time -> 0 (a power loss or backward set PAUSES, never completes early);
 *  - the monotonic witness (+ small slack): credit at most the REAL powered time elapsed since the
 *    last tick, so jumping the I2C RTC forward buys nothing while the device stays powered;
 *  - a per-wake fallback cap, used ONLY when the witness is unavailable (the device was fully
 *    power-cycled since the last tick, so mono reset) -- this is the residual path an attacker must
 *    use, and it costs one full power cycle per capped interval.
 * Pure read (no state writes). If *over_jump is non-NULL it receives how far a forward RTC jump
 * exceeded real elapsed time, so the caller can flag suspected tampering. */
static uint32_t credit_delta(cJSON *e, uint32_t now, uint32_t *over_jump)
{
    if (over_jump) *over_jump = 0;
    cJSON *an    = cJSON_GetObjectItem(e, "anchor");
    uint32_t anc = cJSON_IsNumber(an) ? (uint32_t)an->valuedouble : now;
    uint32_t delta = (now >= anc) ? (now - anc) : 0;   /* backward time (RTC reset) -> 0 = pause */

    cJSON *mc = cJSON_GetObjectItem(e, "manc");
    if (cJSON_IsNumber(mc)) {
        uint32_t manc = (uint32_t)mc->valuedouble;
        uint32_t m = mono_now();
        if (m >= manc) {                               /* witness valid: device stayed powered */
            uint32_t allowed = (m - manc) + DMS_MONO_SLACK_SEC;
            if (delta > allowed) { if (over_jump) *over_jump = delta - allowed; delta = allowed; }
            return delta;
        }
        /* m < manc: the witness reset -> the device was fully power-cycled since the last tick.
         * Fall through to the per-wake fallback cap below. */
    }
    if (s_max_wake_credit && delta > s_max_wake_credit) delta = s_max_wake_credit;
    return delta;
}

/* Total counted (powered) time for an armed entry: accrued + the bounded gap since the last tick. */
static uint32_t entry_elapsed(cJSON *e, uint32_t now)
{
    return (uint32_t)jint(e, "accrued", 0) + credit_delta(e, now, NULL);
}

/* #4: set the per-wake fallback credit cap (0 disables it). Normally left at the built-in constant;
 * exposed mainly for tests. NOT wired to config, so it cannot be loosened by editing config.json. */
void dms_vault_set_max_wake_credit(uint32_t sec) { s_max_wake_credit = sec; }

void dms_vault_tick(uint32_t now_unix)
{
    if (!s_state) return;
    cJSON *armed = cJSON_GetObjectItem(s_state, "armed");
    bool changed = false, tamper = false;
    uint32_t mono = mono_now();
    cJSON *e;
    cJSON_ArrayForEach(e, armed) {
        uint32_t over = 0;
        uint32_t add = credit_delta(e, now_unix, &over);
        set_num(e, "accrued", (double)((uint32_t)jint(e, "accrued", 0) + add));
        set_num(e, "anchor",  (double)now_unix);
        set_num(e, "manc",    (double)mono);            /* re-anchor the monotonic witness too */
        if (over > DMS_TAMPER_JUMP_SEC) tamper = true;  /* RTC jumped far beyond real elapsed time */
        changed = true;
    }
    if (tamper) { set_num(s_state, "tamper", 1); ESP_LOGW(TAG, "suspected clock tampering"); }
    if (changed) save_encrypted(DMS_STATE, s_state);
}

/* #4: true if a gross external-RTC forward jump (beyond real powered time) has been seen since the
 * owner last acknowledged it -- surfaced to the owner on the master menu, never a lockout. */
bool dms_vault_clock_tamper_seen(void)
{
    return s_state && jint(s_state, "tamper", 0) != 0;
}

void dms_vault_clear_clock_tamper(void)
{
    if (!s_state) return;
    if (cJSON_GetObjectItem(s_state, "tamper")) {
        cJSON_DeleteItemFromObject(s_state, "tamper");
        save_encrypted(DMS_STATE, s_state);
    }
}

dms_access_t dms_vault_alt_request(int pin_id, uint32_t countdown_sec,
                                   uint32_t now_unix, uint32_t *remaining_sec)
{
    cJSON *armed = cJSON_GetObjectItem(s_state, "armed");
    cJSON *entry = NULL, *e;
    cJSON_ArrayForEach(e, armed) {
        if (jint(e, "pin", -1) == pin_id) { entry = e; break; }
    }

    if (!entry) {                       /* first request: arm the countdown */
        entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "pin", pin_id);
        cJSON_AddNumberToObject(entry, "accrued", 0);
        cJSON_AddNumberToObject(entry, "anchor", (double)now_unix);
        cJSON_AddNumberToObject(entry, "manc", (double)mono_now());  /* #4 monotonic witness anchor */
        cJSON_AddItemToArray(armed, entry);
        save_encrypted(DMS_STATE, s_state);
        if (remaining_sec) *remaining_sec = countdown_sec;
        return DMS_ACCESS_PENDING;
    }

    uint32_t elapsed = entry_elapsed(entry, now_unix);
    if (elapsed >= countdown_sec) {
        set_num(entry, "revealed", 1);     /* viewed -> stop nagging / stop masking others */
        save_encrypted(DMS_STATE, s_state);
        if (remaining_sec) *remaining_sec = 0;
        return DMS_ACCESS_GRANTED;
    }
    if (remaining_sec) *remaining_sec = countdown_sec - elapsed;
    return DMS_ACCESS_PENDING;
}

void dms_vault_master_checkin(uint32_t now_unix)
{
    (void)now_unix;
    cJSON *armed = cJSON_GetObjectItem(s_state, "armed");
    cJSON *fresh = cJSON_CreateArray();
    cJSON_ReplaceItemInObject(s_state, "armed", fresh);
    (void)armed;
    save_encrypted(DMS_STATE, s_state);
}

/* ----------------------------------------------------------------- #8 recovery code */
/* Opt-in spare key for a FORGOTTEN master PIN. We store ONLY a salted verifier of the code
 * (never the code itself): verifier = SHA256(PBKDF2(code||device_secret, salt)), same scheme as
 * a PIN. A correct code lets the owner reset the master back to the default "123". */

bool dms_vault_set_recovery(const char *code)
{
    if (!s_manifest || !code || !code[0]) return false;
    uint8_t salt[DMS_SALT_LEN], pk[DMS_KEY_LEN], hash[32];
    dms_random(salt, sizeof(salt));
    if (!dms_kdf_pin(code, salt, sizeof(salt), pk, sizeof(pk))) return false;
    if (!dms_sha256(pk, sizeof(pk), hash)) return false;
    cJSON_DeleteItemFromObject(s_manifest, "recovery");
    cJSON *r = cJSON_CreateObject();
    add_b64(r, "salt", salt, sizeof(salt));
    add_b64(r, "verifier", hash, DMS_VERIFIER_LEN);
    cJSON_AddItemToObject(s_manifest, "recovery", r);
    return save_encrypted(DMS_MANIFEST, s_manifest);
}

void dms_vault_clear_recovery(void)
{
    if (!s_manifest) return;
    cJSON_DeleteItemFromObject(s_manifest, "recovery");
    save_encrypted(DMS_MANIFEST, s_manifest);
}

bool dms_vault_has_recovery(void)
{
    return s_manifest && cJSON_GetObjectItem(s_manifest, "recovery") != NULL;
}

bool dms_vault_check_recovery(const char *code)
{
    if (!s_manifest || !code || !code[0]) return false;
    cJSON *r = cJSON_GetObjectItem(s_manifest, "recovery");
    if (!r) return false;
    uint8_t salt[DMS_SALT_LEN], pk[DMS_KEY_LEN], hash[32], verifier[DMS_VERIFIER_LEN];
    if (get_b64(r, "salt", salt, sizeof(salt)) != DMS_SALT_LEN) return false;
    if (get_b64(r, "verifier", verifier, sizeof(verifier)) != DMS_VERIFIER_LEN) return false;
    if (!dms_kdf_pin(code, salt, DMS_SALT_LEN, pk, sizeof(pk))) return false;
    if (!dms_sha256(pk, sizeof(pk), hash)) return false;
    return dms_ct_equal(hash, verifier, DMS_VERIFIER_LEN);
}

/* Reset the master PIN to the default "123" after a verified recovery code. If a NON-master PIN
 * currently uses "123", delete it first -- otherwise change_master_pin's in-use guard would make
 * recovery silently fail, locking out the legitimate code holder. */
bool dms_vault_recover_reset_master(void)
{
    int mid = master_pin_id();
    if (mid < 0) return false;
    dms_pin_t hit;
    if (dms_vault_verify_pin("123", &hit) != DMS_ROLE_NONE && hit.id != mid)
        dms_vault_del_pin(hit.id);
    return dms_vault_change_master_pin("123");
}
