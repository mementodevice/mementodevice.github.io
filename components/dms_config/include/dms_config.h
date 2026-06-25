#ifndef DMS_CONFIG_H
#define DMS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* Max length (characters) of the optional lost-device / owner message. Kept short so it always
 * fits the 200x200 screen above the PIN pad (~6 wrapped lines at the UI font). */
#define DMS_OWNER_MSG_MAX 160

/*
 * Runtime-tunable settings, persisted as plain JSON at /sdcard/dms/config.json and editable
 * from the master-only WiFi setup page. Missing/invalid values fall back to the defaults
 * below; every value is clamped to a safe range on load and on set.
 *
 * These are NOT secret (just timeouts + a lost-device contact note), so the file is plain text.
 * Per-PIN countdowns and the vault itself stay in the encrypted manifest -- not here.
 */
typedef struct {
    uint32_t idle_sleep_sec;        /* PIN/menu idle -> deep sleep        (default 60)        */
    uint32_t reader_idle_sec;       /* reading a file idle -> deep sleep  (default 300)       */
    uint32_t wifi_idle_sec;         /* WiFi setup idle -> deep sleep      (default 900 = 15m) */
    uint32_t countdown_refresh_sec; /* wake interval to refresh countdown (default 3600)      */
    uint32_t default_countdown_sec; /* prefill for a new alt PIN's timer  (default 72h)       */
    uint32_t batt_critical_pct;     /* below this -> "battery low" warning (default 15)       */
    uint32_t owner_msg_on;          /* show owner_msg before the PIN pad   (0/1, default 0)    */
    char owner_msg[DMS_OWNER_MSG_MAX + 1]; /* lost-device contact note; "" when unset          */
} dms_config_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Load config from disk (creating it with defaults if absent). Call once after the vault
 * storage is mounted. */
void dms_config_load(void);

/* Current settings (always valid; defaults until loaded). */
const dms_config_t *dms_config_get(void);

/* Set one numeric field by JSON key (clamped) and persist. Returns false on unknown key. */
bool dms_config_set_u32(const char *key, uint32_t value);

/* Set one string field by JSON key (truncated to its max) and persist. Currently only
 * "owner_msg" is accepted. Returns false on unknown key. */
bool dms_config_set_str(const char *key, const char *value);

#ifdef __cplusplus
}
#endif

#endif /* DMS_CONFIG_H */
