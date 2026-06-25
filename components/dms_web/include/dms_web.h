#ifndef DMS_WEB_H
#define DMS_WEB_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Master-only setup web server (runs on the device's SoftAP). Lets the owner:
 *   - create/delete PINs (master + alternatives, with per-alt countdown),
 *   - add/delete text files and choose which PINs may read each file.
 *
 * The caller is responsible for bringing the SoftAP up first (see user_app). The server
 * talks only to dms_vault, so every change is encrypted on the SD card.
 */

#ifdef __cplusplus
extern "C" {
#endif

bool dms_web_start(void);
void dms_web_stop(void);

/* Monotonic count of HTTP requests served. The UI polls this to keep the SoftAP awake
 * while the browser is active (so an in-progress upload isn't cut off by the idle timer). */
uint32_t dms_web_req_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DMS_WEB_H */
