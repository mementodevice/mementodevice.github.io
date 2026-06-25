#include "dms_secret.h"

#include "nvs.h"
#include "esp_random.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "dms_secret";

bool dms_secret_get(uint8_t *out, bool *created)
{
    if (out == NULL) return false;
    if (created) *created = false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(DMS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Try to load an existing secret. */
    size_t len = DMS_DEVICE_SECRET_LEN;
    err = nvs_get_blob(h, DMS_NVS_KEY_SECRET, out, &len);
    if (err == ESP_OK && len == DMS_DEVICE_SECRET_LEN) {
        nvs_close(h);
        return true;
    }

    /* None (or wrong size) -> generate. esp_fill_random() is the hardware RNG; it is
       suitable for key material on the ESP32-S3. */
    esp_fill_random(out, DMS_DEVICE_SECRET_LEN);

    err = nvs_set_blob(h, DMS_NVS_KEY_SECRET, out, DMS_DEVICE_SECRET_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to persist device_secret: %s", esp_err_to_name(err));
        return false;
    }

    if (created) *created = true;
    return true;
}

uint32_t dms_secret_counter_get(const char *key, uint32_t def)
{
    if (!key) return def;
    nvs_handle_t h;
    if (nvs_open(DMS_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return def;
    uint32_t v = def;
    esp_err_t err = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return (err == ESP_OK) ? v : def;
}

bool dms_secret_counter_set(const char *key, uint32_t value)
{
    if (!key) return false;
    nvs_handle_t h;
    if (nvs_open(DMS_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u32(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}
