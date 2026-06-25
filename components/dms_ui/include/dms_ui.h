#ifndef DMS_UI_H
#define DMS_UI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * The interactive front-end: numeric PIN pad + file list + paged reader, driven by the
 * two physical buttons. The state machine ties PIN verification (dms_vault) to the
 * master / alternative flows and the dead-man countdown.
 *
 * Button mapping (from button_bsp event groups):
 *    BOOT short  -> NEXT  (move forward)
 *    BOOT double -> PREV  (move backward)
 *    PWR  short  -> ENTER (select / OK)
 *    BOOT long   -> submit PIN / back
 *    PWR  long   -> SLEEP (power down)
 *
 * The host (user_app) supplies time, sleep, and WiFi-setup hooks so this component stays
 * free of board/RTC/WiFi specifics.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Why did we boot? Determines the first screen (logo vs PIN vs countdown refresh). */
typedef enum {
    DMS_WAKE_COLD = 0,   /* power-on / reflash            */
    DMS_WAKE_BUTTON,     /* user pressed the wake button  */
    DMS_WAKE_TIMER,      /* hourly countdown-refresh wake */
} dms_wake_t;

typedef struct {
    uint32_t (*now_unix)(void);                 /* current time from the RTC               */
    void     (*enter_sleep)(void);              /* deep sleep (wakes on button)            */
    /* Start the master-only SoftAP + web setup; fill ssid/pass/url. Returns true on ok.   */
    bool     (*start_setup_wifi)(char *ssid, size_t scap, char *pass, size_t pcap,
                                 char *url, size_t ucap);
    void     (*stop_setup_wifi)(void);
    /* Deep sleep that ALSO wakes after wake_after_sec (for the hourly countdown refresh). */
    void     (*enter_sleep_timer)(uint32_t wake_after_sec);
    /* Battery charge left, 0..100, or -1 if unknown. */
    int      (*battery_pct)(void);
    /* Monotonic web-request counter; lets the UI keep the WiFi-setup AP awake while the
     * browser is active. May be NULL (then only button presses defer the idle sleep). */
    uint32_t (*wifi_req_count)(void);
    /* Request a full-waveform (ghost-clearing) refresh for the NEXT screen draw. May be NULL.
     * Used before sleeping and when leaving the reader so stale text shadows don't linger. */
    void     (*display_full_refresh)(void);
} dms_ui_host_t;

/* Build the first screen and start the input task. Call under the LVGL lock. */
void dms_ui_start(const dms_ui_host_t *host, dms_wake_t wake);

#ifdef __cplusplus
}
#endif

#endif /* DMS_UI_H */
