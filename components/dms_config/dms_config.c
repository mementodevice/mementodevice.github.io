#include "dms_config.h"

#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "dms_config";
#define CFG_PATH "/sdcard/dms/config.json"

static dms_config_t g = {
    .idle_sleep_sec        = 60,
    .reader_idle_sec       = 300,
    .wifi_idle_sec         = 900,
    .countdown_refresh_sec = 3600,
    .default_countdown_sec = 72u * 3600u,
    .batt_critical_pct     = 15,
    .owner_msg_on          = 0,
    .owner_msg             = "",
};

static const char *KEYS[] = {
    "idle_sleep_sec", "reader_idle_sec", "wifi_idle_sec",
    "countdown_refresh_sec", "default_countdown_sec", "batt_critical_pct",
    "owner_msg_on",
};
#define NKEYS (sizeof(KEYS) / sizeof(KEYS[0]))

static uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Resolve a key to its field pointer and allowed range. */
static bool field_ref(const char *key, uint32_t **p, uint32_t *lo, uint32_t *hi)
{
    if (!strcmp(key, "idle_sleep_sec"))        { *p = &g.idle_sleep_sec;        *lo = 10; *hi = 3600;        return true; }
    if (!strcmp(key, "reader_idle_sec"))       { *p = &g.reader_idle_sec;       *lo = 10; *hi = 3600;        return true; }
    if (!strcmp(key, "wifi_idle_sec"))         { *p = &g.wifi_idle_sec;         *lo = 60; *hi = 7200;        return true; }
    if (!strcmp(key, "countdown_refresh_sec")) { *p = &g.countdown_refresh_sec; *lo = 60; *hi = 86400;       return true; }
    if (!strcmp(key, "default_countdown_sec")) { *p = &g.default_countdown_sec; *lo = 60; *hi = 365u*86400u; return true; }
    if (!strcmp(key, "batt_critical_pct"))     { *p = &g.batt_critical_pct;     *lo = 0;  *hi = 50;          return true; }
    if (!strcmp(key, "owner_msg_on"))          { *p = &g.owner_msg_on;          *lo = 0;  *hi = 1;           return true; }
    return false;
}

static void save(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    for (size_t i = 0; i < NKEYS; i++) {
        uint32_t *p, lo, hi;
        if (field_ref(KEYS[i], &p, &lo, &hi)) cJSON_AddNumberToObject(o, KEYS[i], (double)*p);
    }
    cJSON_AddStringToObject(o, "owner_msg", g.owner_msg);   /* string field (not in KEYS) */
    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        FILE *f = fopen(CFG_PATH, "w");
        if (f) { fputs(s, f); fclose(f); }
        else   ESP_LOGE(TAG, "could not write %s", CFG_PATH);
        cJSON_free(s);
    }
    cJSON_Delete(o);
}

void dms_config_load(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "no config file; writing defaults");
        save();
        return;
    }
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *o = cJSON_Parse(buf);
    if (!o) { ESP_LOGW(TAG, "config parse failed; using defaults"); return; }
    for (size_t i = 0; i < NKEYS; i++) {
        cJSON *it = cJSON_GetObjectItem(o, KEYS[i]);
        if (cJSON_IsNumber(it)) {
            uint32_t *p, lo, hi;
            field_ref(KEYS[i], &p, &lo, &hi);
            *p = clampu((uint32_t)it->valuedouble, lo, hi);
        }
    }
    cJSON *msg = cJSON_GetObjectItem(o, "owner_msg");
    if (cJSON_IsString(msg) && msg->valuestring) {
        strncpy(g.owner_msg, msg->valuestring, DMS_OWNER_MSG_MAX);
        g.owner_msg[DMS_OWNER_MSG_MAX] = '\0';
    }
    cJSON_Delete(o);
    ESP_LOGI(TAG, "config loaded (idle=%lus reader=%lus wifi=%lus refresh=%lus)",
             (unsigned long)g.idle_sleep_sec, (unsigned long)g.reader_idle_sec,
             (unsigned long)g.wifi_idle_sec, (unsigned long)g.countdown_refresh_sec);
}

const dms_config_t *dms_config_get(void)
{
    return &g;
}

bool dms_config_set_u32(const char *key, uint32_t value)
{
    uint32_t *p, lo, hi;
    if (!field_ref(key, &p, &lo, &hi)) return false;
    *p = clampu(value, lo, hi);
    save();
    return true;
}

bool dms_config_set_str(const char *key, const char *value)
{
    if (strcmp(key, "owner_msg") != 0) return false;
    if (!value) value = "";
    strncpy(g.owner_msg, value, DMS_OWNER_MSG_MAX);
    g.owner_msg[DMS_OWNER_MSG_MAX] = '\0';
    save();
    return true;
}
