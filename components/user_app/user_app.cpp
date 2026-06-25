/*
 * Memento DMS - application orchestrator (ESP-IDF).
 *
 * Brings up the board (manufacturer BSPs: power, I2C, buttons, e-paper, PCF85063 RTC),
 * initialises NVS + crypto + the encrypted vault, then hands control to the LVGL UI flow
 * (dms_ui). Provides the host hooks the UI needs: current time (RTC), deep sleep, and the
 * master-only SoftAP + web-setup server (dms_web).
 *
 * Boot path: deep-sleep wake on BOOT -> app_main (main.cpp) -> user_app_init() ->
 * LVGL port -> user_ui_init() -> dms_ui_start(). The UI shows the PIN pad (or, on first
 * run with no master PIN, the WiFi setup screen).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "user_app.h"
#include "driver/gpio.h"
#include "user_config.h"
#include "board_power_bsp.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_random.h"

#include "button_bsp.h"
#include "i2c_equipment.h"
#include "i2c_bsp.h"
#include "adc_bsp.h"

#include "dms_crypto.h"
#include "dms_vault.h"
#include "dms_config.h"
#include "dms_ui.h"
#include "dms_web.h"

static const char *TAG = "memento_dms";

i2c_equipment        *rtc_dev = NULL;
epaper_driver_display *driver = NULL;
board_power_bsp_t     board_div(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);

static bool s_net_inited = false;
static bool s_ap_up      = false;
static wl_handle_t s_wl  = WL_INVALID_HANDLE;

/* Mount the internal-flash FAT partition ('storage') at the vault mount point (/sdcard, a
 * legacy path name -- it's internal flash, not a card). Formats on first use / size change. */
static bool mount_internal_vault(void)
{
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = true;
    cfg.max_files = 5;
    cfg.allocation_unit_size = 4096;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(DMS_SD_MOUNT, "storage", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "internal vault mount failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "internal-flash vault mounted at %s", DMS_SD_MOUNT);
    return true;
}

/* ----------------------------------------------------------------- helpers */

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* RtcDateTime -> Unix seconds (UTC, TZ-independent: days-from-civil algorithm). */
static uint32_t rtc_to_unix(const RtcDateTime_t *t)
{
    int y = t->year, m = t->month, d = t->day;
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long days = (long)era * 146097L + (long)doe - 719468L;
    return (uint32_t)(days * 86400L + t->hour * 3600 + t->minute * 60 + t->second);
}

/* ----------------------------------------------------------------- UI host hooks (C) */

extern "C" uint32_t host_now_unix(void)
{
    if (!rtc_dev) return 0;
    RtcDateTime_t t = rtc_dev->get_rtcTime();
    return rtc_to_unix(&t);
}

extern "C" void host_enter_sleep(void)
{
    ESP_LOGI(TAG, "entering deep sleep (wake on BOOT)");
    vTaskDelay(pdMS_TO_TICKS(300));
    board_div.EnableDeepLowPowerMode();   /* configures EXT1 wake on GPIO0 + sleeps */
}

/* Deep sleep that also wakes after `sec` (hourly countdown-face refresh). The timer wake
 * source simply adds to the EXT1 button wake configured by EnableDeepLowPowerMode(). */
extern "C" void host_enter_sleep_timer(uint32_t sec)
{
    ESP_LOGI(TAG, "entering deep sleep (wake on button or in %lu s)", (unsigned long)sec);
    vTaskDelay(pdMS_TO_TICKS(300));
    board_div.EnableDeepLowPowerMode(sec);   /* timer enabled inside, after wakeup-source reset */
}

extern "C" bool host_start_setup_wifi(char *ssid, size_t scap, char *pass, size_t pcap,
                                      char *url, size_t ucap)
{
    if (!s_net_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_ap();
        wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&ic));
        s_net_inited = true;
    }

    /* Fresh random SSID each session: the password is also random each time, and phones
       remember a network by SSID -> a fixed SSID would auto-reconnect with the stale saved
       password and fail. A new SSID each time avoids that. */
    char ssid_buf[24];
    snprintf(ssid_buf, sizeof(ssid_buf), "Memento-%04u", (unsigned)(esp_random() % 10000));

    /* Random alphanumeric AP password: 12 chars from a 56-char set (~70 bits), so a captured WPA2
     * handshake cannot be brute-forced within a setup session (an 8-digit numeric password, ~27
     * bits, could be). Look-alike characters (0/O/o, 1/l/I) are excluded so it is easy to read off
     * the e-ink screen and type. Still do Wi-Fi setup in private (see docs). */
    static const char pw_cs[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
    char pw[13];
    for (int i = 0; i < 12; i++) pw[i] = pw_cs[esp_random() % (sizeof(pw_cs) - 1)];
    pw[12] = '\0';

    wifi_config_t wc = {};
    strncpy((char *)wc.ap.ssid, ssid_buf, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len = strlen(ssid_buf);
    strncpy((char *)wc.ap.password, pw, sizeof(wc.ap.password) - 1);
    wc.ap.max_connection = 2;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.channel = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Lower TX power. A SoftAP at full power draws a large current spike when a station
     * associates, which can brown-out the device (small battery / weak USB) and RESET it --
     * the reset lands back on the logo and looks like the device "went to sleep". The phone
     * is right next to the device, so ~10 dBm is plenty. Units are 0.25 dBm; default ~20 dBm. */
    esp_wifi_set_max_tx_power(40);

    s_ap_up = true;

    if (!dms_web_start()) return false;

    snprintf(ssid, scap, "%s", ssid_buf);
    snprintf(pass, pcap, "%s", pw);
    snprintf(url, ucap, "http://192.168.4.1");
    ESP_LOGI(TAG, "setup AP up: ssid=%s pass=%s", ssid_buf, pw);
    return true;
}

extern "C" void host_stop_setup_wifi(void)
{
    dms_web_stop();
    if (s_ap_up) { esp_wifi_stop(); s_ap_up = false; }
}

/* Battery charge left, 0..100 (-1 if unreadable). LiPo voltage mapped roughly:
 * 3.30 V ~ empty, 4.05 V ~ full. The on-board charger terminates around 4.05 V (not the LiPo
 * nominal 4.20 V), so a fully-charged pack reads ~4.05 V -- mapping "full" to 4.20 V capped the
 * display near ~80%. adc_get_value() already applies the /2 divider. */
#define BATT_EMPTY_V 3.30f
#define BATT_FULL_V  4.05f
extern "C" int host_battery_pct(void)
{
    /* (1) Multisampling: average several quick reads to reject per-read ADC noise. */
    float sum = 0.0f; int got = 0;
    for (int i = 0; i < 16; i++) {
        float v = 0.0f;
        adc_get_value(&v, NULL);
        if (v > 0.5f) { sum += v; got++; }
    }
    if (got == 0) return -1;                  /* read failed / USB-only, no cell */
    float v = sum / got;

    /* (2) Low-pass (exponential moving average): ease toward the new value across calls
       instead of snapping, so load-induced wobble doesn't jump the reading. */
    static float s_v = 0.0f;
    s_v = (s_v <= 0.5f) ? v : (s_v * 0.8f + v * 0.2f);

    int pct = (int)((s_v - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V) * 100.0f + 0.5f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    /* (3) Quantize to 5% steps so the displayed value stops flickering by 1%. */
    pct = (pct + 2) / 5 * 5;
    if (pct > 100) pct = 100;
    return pct;
}

/* Web-request counter, so the UI can keep the setup AP awake while the browser is busy. */
extern "C" uint32_t host_wifi_req_count(void) { return dms_web_req_count(); }

/* Ask the LVGL flush (main.cpp) to do a full-waveform refresh on the next draw, clearing
 * accumulated e-ink ghosting. */
extern volatile bool g_epd_full_refresh;
extern "C" void host_display_full_refresh(void) { g_epd_full_refresh = true; }

/* ----------------------------------------------------------------- entry points */

void user_app_init(void)
{
    ESP_LOGI(TAG, "=== Memento DMS init ===");

    init_nvs();
    i2c_master_Init();
    user_button_init();
    board_div.VBAT_POWER_ON();
    board_div.POWEER_EPD_ON();
    board_div.POWEER_Audio_ON();

    /* e-paper */
    custom_lcd_spi_t cfg = {};
    cfg.cs = EPD_CS_PIN;  cfg.dc = EPD_DC_PIN;   cfg.rst = EPD_RST_PIN;
    cfg.busy = EPD_BUSY_PIN; cfg.mosi = EPD_MOSI_PIN; cfg.scl = EPD_SCK_PIN;
    cfg.spi_host = EPD_SPI_NUM; cfg.buffer_len = 5000;
    driver = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT, cfg);
    driver->EPD_Init();
    driver->EPD_Clear();
    driver->EPD_DisplayPartBaseImage();
    driver->EPD_Init_Partial();

    /* RTC: seed if it lost power so countdown timestamps stay monotonic */
    rtc_dev = new i2c_equipment();
    if (rtc_dev->get_rtcTime().year < 2025) {
        rtc_dev->set_rtcTime(2025, 1, 1, 0, 0, 0);
        ESP_LOGW(TAG, "RTC had lost power; seeded to 2025-01-01");
    }

    /* storage: internal flash ONLY. The SD card is removable and cannot be covered by ESP
     * flash encryption, so all files live in the internal 'storage' FAT partition. */
    adc_bsp_init();                          /* battery voltage ADC */
    if (!mount_internal_vault())
        ESP_LOGE(TAG, "internal vault mount failed -- vault unavailable");
    if (!dms_crypto_init())  ESP_LOGE(TAG, "crypto init failed (device_secret)");
    if (!dms_vault_init())   ESP_LOGE(TAG, "vault init failed");
    dms_config_load();       /* runtime settings (timeouts etc.) from /sdcard/dms/config.json */

    /* #4 anti forward-clock-set: the countdown is now bounded by an independent monotonic witness
     * inside dms_vault (it credits at most the real powered time elapsed, so fast-forwarding the
     * external PCF85063 over I2C buys nothing while the device stays powered). The per-wake credit
     * cap is only a fallback for the fully-power-cycled case and is a built-in constant in the
     * vault -- deliberately NOT derived from config.json, so tampering with that plaintext file
     * cannot loosen the bound. No setup call is needed here. */

    ESP_LOGI(TAG, "=== init done (master set: %d) ===", (int)dms_vault_has_master());
}

void user_ui_init(void)
{
    static const dms_ui_host_t host = {
        .now_unix          = host_now_unix,
        .enter_sleep       = host_enter_sleep,
        .start_setup_wifi  = host_start_setup_wifi,
        .stop_setup_wifi   = host_stop_setup_wifi,
        .enter_sleep_timer = host_enter_sleep_timer,
        .battery_pct       = host_battery_pct,
        .wifi_req_count    = host_wifi_req_count,
        .display_full_refresh = host_display_full_refresh,
    };

    dms_wake_t wake = DMS_WAKE_COLD;
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1:  wake = DMS_WAKE_BUTTON; break;
    case ESP_SLEEP_WAKEUP_TIMER: wake = DMS_WAKE_TIMER;  break;
    default:                     wake = DMS_WAKE_COLD;   break;
    }
    ESP_LOGI(TAG, "wake cause: %d", (int)wake);
    dms_ui_start(&host, wake);
}
