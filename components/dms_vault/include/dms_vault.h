#ifndef DMS_VAULT_H
#define DMS_VAULT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * The vault: an encrypted manifest on the SD card plus per-file ciphertext blobs.
 *
 *   manifest.enc : AES-GCM([iv|tag|json]) under the manifest key (device_secret-derived).
 *                  Holds PINs (salt + verifier + countdown + PK wrapped under manifest key)
 *                  and files (content iv/tag/len + per-PIN wrapped content key).
 *   files/NNNN.enc : raw AES-GCM ciphertext of one text file (key = that file's content key).
 *   state.enc    : armed-state for the dead-man countdown.
 *
 * Each file's random content key is wrapped once per authorising PIN, under that PIN's
 * PK = PBKDF2(pin||device_secret, salt). The master PIN is wrapped into every file, so the
 * master can always read everything.
 */

#define DMS_MAX_PINS       16
#define DMS_MAX_FILES      64
#define DMS_NAME_MAX       48
#define DMS_FILE_MAX_BYTES 20480  /* 20 KB: largest note handled in RAM (bigger ones are refused
                                   * at upload). Kept modest so the on-device reader stays snappy. */

typedef enum { DMS_ROLE_NONE = 0, DMS_ROLE_MASTER = 1, DMS_ROLE_ALT = 2 } dms_role_t;

typedef struct {
    int        id;
    dms_role_t role;
    uint32_t   countdown_sec;     /* alt only */
    char       name[DMS_NAME_MAX]; /* owner-assigned label, "" if unset */
} dms_pin_t;

typedef enum { DMS_ACCESS_GRANTED = 0, DMS_ACCESS_PENDING = 1 } dms_access_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Load (or create) manifest + state. Requires dms_crypto_init() first. */
bool dms_vault_init(void);

bool dms_vault_has_master(void);

/* Identify an entered PIN. Returns its role (NONE if no match), fills *out on match. */
dms_role_t dms_vault_verify_pin(const char *pin, dms_pin_t *out);

/* Names of files readable by pin_id -> names[] (each DMS_NAME_MAX). Returns count. */
int dms_vault_files_for_pin(int pin_id, char names[][DMS_NAME_MAX], int maxn);

/* Decrypt file `name` for the entered `pin` (whose id is pin_id). Returns bytes in *outlen. */
bool dms_vault_read_file(const char *pin, int pin_id, const char *name,
                         char *buf, size_t cap, size_t *outlen);

/* ---- management (master / web UI) ---- */
bool dms_vault_add_pin(const char *pin, dms_role_t role, uint32_t countdown_sec, int *new_id);
bool dms_vault_del_pin(int pin_id);
/* Set/replace a PIN's display label (any role). Empty string clears it. */
bool dms_vault_set_pin_name(int pin_id, const char *name);
/* Re-key the existing master to a new PIN (keeps file access). Web UI cannot add/remove the
 * master; it can only change its PIN through this. False if new_pin is taken by another PIN. */
bool dms_vault_change_master_pin(const char *new_pin);
int  dms_vault_list_pins(dms_pin_t *out, int maxn);
bool dms_vault_add_file(const char *name, const char *content, size_t len,
                        const int *pin_ids, int npins);
bool dms_vault_del_file(const char *name);
int  dms_vault_list_all_files(char names[][DMS_NAME_MAX], int maxn);
/* True if pin_id may currently read file `name`. */
bool dms_vault_file_has_reader(const char *name, int pin_id);
/* Replace the set of PINs that may read `name` (master is always included). Re-wraps the
 * file's content key for the new set -- lets the owner fix readership without re-uploading. */
bool dms_vault_set_file_readers(const char *name, const int *pin_ids, int npins);
/* Read a note's plaintext for master management (no PIN; via the manifest key). For the edit UI. */
bool dms_vault_read_file_mgmt(const char *name, char *buf, size_t cap, size_t *outlen);
/* Replace a note's content, keeping its current readers (re-encrypts + re-wraps). */
bool dms_vault_set_file_content(const char *name, const char *content, size_t len);

/* ---- dead-man state ---- */
/* An alt entered their PIN: arm if not already; GRANTED once the countdown has elapsed,
 * otherwise PENDING with *remaining_sec set. */
dms_access_t dms_vault_alt_request(int pin_id, uint32_t countdown_sec,
                                    uint32_t now_unix, uint32_t *remaining_sec);

/* The master checked in: disarm every pending countdown. */
void dms_vault_master_checkin(uint32_t now_unix);

/* Fold elapsed time into each armed entry's accrued total and re-anchor to `now`. Call on
 * every wake and before sleep. Countdowns accrue only *powered* time and never run
 * backwards: if the RTC was reset (battery died), the lost gap is skipped but progress is
 * preserved -- so a power loss pauses the countdown instead of silently resetting it. */
void dms_vault_tick(uint32_t now_unix);

/* Boot-time dead-man check (no PIN needed). Returns:
 *   2 = an armed alt's countdown has ELAPSED -> reveal its files (*pin_id set)
 *   1 = an alt is armed but still pending    -> *pin_id + *remaining_sec set (soonest)
 *   0 = nothing armed */
int dms_vault_pending_check(uint32_t now_unix, int *pin_id, uint32_t *remaining_sec);

/* #4 anti forward-clock-set: per-wake FALLBACK credit cap, applied only when the independent
 * monotonic witness is unavailable (the device was fully power-cycled since the last tick). Honest
 * operation is bounded by the witness, which credits at most the real powered time elapsed -- so an
 * attacker who fast-forwards the external RTC while the device stays powered gains nothing. Defaults
 * to a built-in constant (NOT derived from the editable config); 0 disables the fallback cap.
 * Exposed mainly for tests. */
void dms_vault_set_max_wake_credit(uint32_t sec);

/* #4: true if a gross external-RTC forward jump (well beyond real powered time) has been observed
 * since the owner last acknowledged it. Advisory only -- shown to the owner on the master menu,
 * never a lockout. Cleared by dms_vault_clear_clock_tamper() once the owner has seen it. */
bool dms_vault_clock_tamper_seen(void);
void dms_vault_clear_clock_tamper(void);

/* ---- #8 recovery code (opt-in spare key for a forgotten master PIN) ---- */
/* Store only a salted verifier of `code` (enables recovery). Returns false on error. */
bool dms_vault_set_recovery(const char *code);
/* Forget the recovery code (disable recovery). */
void dms_vault_clear_recovery(void);
/* True if a recovery code is currently set. */
bool dms_vault_has_recovery(void);
/* Constant-time check of an entered code against the stored verifier. */
bool dms_vault_check_recovery(const char *code);
/* Reset the master PIN to the default "123" (after check_recovery succeeded). */
bool dms_vault_recover_reset_master(void);

#ifdef __cplusplus
}
#endif

#endif /* DMS_VAULT_H */
