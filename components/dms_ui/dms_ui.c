#include "dms_ui.h"
#include "dms_vault.h"
#include "dms_config.h"
#include "dms_secret.h"
#include "button_bsp.h"

#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "dms_ui";

/* logos (logo_img.c, 1-bit): full 200x200 for idle, 140x140 for the status face */
extern const lv_img_dsc_t dms_logo;
extern const lv_img_dsc_t dms_logo_small;

/* ---- UI font ----------------------------------------------------------------------------
 * The bundled LVGL fonts only carry basic Latin (ASCII), so accented/non-ASCII characters --
 * Croatian c/c/z/s/d-bar, curly quotes, em-dashes, currency, and any special characters in a
 * stored password -- render as the missing-glyph box. To support them, generate an extended
 * font from a TTF (see components/dms_ui/fonts/README.md), drop the .c into fonts/ (it builds
 * automatically), then set MEMENTO_FONT_EXTENDED to 1 below. Everything stays ASCII-correct
 * either way; this only changes what can be DRAWN. */
#define MEMENTO_FONT_EXTENDED 1
#if MEMENTO_FONT_EXTENDED
extern const lv_font_t font_memento_14;     /* generated; covers Latin-1 + Latin Extended-A + symbols */
#define UI_FONT (&font_memento_14)
#else
#define UI_FONT (&lv_font_montserrat_14)    /* default: ASCII only */
#endif

/* End-user-facing button names (kept short so two fit on one 200px line).
 * BOOT = UPPER button, PWR = LOWER button (confirmed by the user). */
#define BTN_UP   "UPR"   /* BOOT button (upper) */
#define BTN_LOW  "LWR"   /* PWR button (lower)  */

/* ---- config ---- */
#define PIN_MIN_LEN     3
#define PIN_MAX_LEN     12
#define READER_BODY_Y   22    /* y where note body starts */
#define READER_HINT_Y   182   /* y of the bottom control hint */
#define READER_AVAIL_H  140   /* px of body height a page may use (leaves a margin above the hint) */
/* Upper bound on how many bytes reader_fit() will probe for one page. A page only holds a few
 * hundred chars, so capping the binary-search window keeps each measurement cheap and stops LVGL
 * from laying out huge substrings -- without this a large note hangs the UI (had to pull power). */
#define READER_MAX_PAGE_BYTES 1024
#define READER_NAME_MAX 14    /* chars of the filename shown in the reader header (rest -> "...") */
#define MENU_NAME_MAX   22    /* chars of a name shown in the menu list (rest -> "..."); also fits
                               * the longest action row "[WiFi Upload & Config]" without clipping  */
#define MENU_WINDOW     5
#define LIST_MAX        DMS_MAX_FILES

/* E-ink refresh: normal redraws use the fast PARTIAL update (no flashing). A slow FULL
 * (ghost-clearing) refresh is done only when closing a note and just before deep sleep
 * (see s_force_full_next / clear_screen). */

/* The input task loop ticks every ~60 ms. */
#define TICK_MS          60
#define IDLE_FACE_MS     (8 * 1000)         /* timer-wake face refresh, then resleep */
/* Idle timeouts, the countdown-refresh interval, and the battery-critical threshold are
 * now runtime config (dms_config), editable from the WiFi page. Defaults in dms_config.h. */

/* button event bits (see button_bsp.c).
 * Mapping (intuitive: BOOT is the action/confirm button):
 *   BOOT short -> ENTER (select / OK / open / prev page)
 *   PWR  short -> NEXT  (move highlight / next page)
 *   BOOT long  -> BACK
 *   PWR  long  -> SLEEP */
#define BIT_BOOT_SHORT  0x01
#define BIT_BOOT_LONG   0x02
#define BIT_BOOT_DOUBLE 0x08   /* BOOT double-click -> move backward */
#define BIT_PWR_SHORT   0x01
#define BIT_PWR_DOUBLE  0x02   /* PWR double-click -> help/guide */
#define BIT_PWR_LONG    0x04

typedef enum {
    ST_LOGO,           /* idle/boot face: just the logo; any key -> route()          */
    ST_OWNER,          /* optional lost-device/owner message shown before the PIN pad */
    ST_PIN,
    ST_MESSAGE,        /* transient info; any key -> action set in s_msg_next        */
    ST_MENU,           /* master menu or file list                                   */
    ST_READER,
    ST_WIFI,
    ST_HELP,           /* controls guide (double-click lower); any key returns         */
    ST_COUNTDOWN,      /* alt armed, waiting; deep-sleeps and refreshes hourly        */
} ui_state_t;

typedef enum { IN_NEXT, IN_PREV, IN_ENTER, IN_BACK, IN_SLEEP } ui_input_t;

static const dms_ui_host_t *s_host;
static ui_state_t s_state;

/* PIN entry */
static char s_pin[PIN_MAX_LEN + 1];
static int  s_pin_len;
static int  s_pin_sel;          /* 0..11 selected key */
static int  s_wrong_pin_count;  /* #9: consecutive wrong PINs -> escalating delay; persisted in NVS
                                 *     (pinguard) so it survives a reboot/power-cut, see submit_pin */

/* idle -> sleep bookkeeping (driven by input_task) */
static int  s_idle_ticks;
static bool s_face_only;        /* woke just to refresh the face: sleep again quickly */
static uint32_t s_wifi_last_req; /* last web-request count seen on the WiFi screen          */

/* set true to make the next clear_screen() do a full (ghost-clearing) refresh */
static bool s_force_full_next;

/* identified principal after a correct PIN */
static dms_pin_t s_who;

/* menu/list */
static char s_items[LIST_MAX + 3][DMS_NAME_MAX];   /* +3 for master action rows */
static int  s_item_count;
static int  s_menu_sel;
static bool s_menu_is_master;    /* master menu has action rows at the top */

/* reader */
static char s_filebuf[DMS_FILE_MAX_BYTES + 1];
static size_t s_file_len;
static size_t s_reader_start;   /* byte offset where the current page begins */
static int  s_reader_page;
static int  s_reader_pages;
static char s_reader_name[DMS_NAME_MAX];

/* message screen follow-up */
typedef enum { MSG_TO_PIN, MSG_TO_SLEEP, MSG_TO_MENU } msg_next_t;
static msg_next_t s_msg_next;

static const char *KEYS[12] = {"1","2","3","4","5","6","7","8","9","DEL","0","OK"};

/* ----------------------------------------------------------------- render helpers */

/* Copy src into dst (cap bytes), clipped to `maxchars` with a trailing "..." if it was longer, so
 * long file names render on a single line instead of wrapping over other UI elements. */
static void clip_name(char *dst, size_t cap, const char *src, int maxchars)
{
    int n = (int)strlen(src);
    if (n <= maxchars || (int)cap <= maxchars + 4) {
        strncpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
        return;
    }
    memcpy(dst, src, maxchars);
    strcpy(dst + maxchars, "...");
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, bool inverted)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, UI_FONT, 0);   /* one chokepoint for the whole UI's font */
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    /* monochrome e-ink: "highlight" = black box, white text */
    if (inverted) {
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(l, lv_color_black(), 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_pad_all(l, 2, 0);
    } else {
        lv_obj_set_style_text_color(l, lv_color_black(), 0);
    }
    return l;
}

static void clear_screen(void)
{
    /* Full (ghost-clearing) refresh ONLY when something explicitly asked for it -- i.e. when
     * closing a note or just before deep sleep. All normal redraws (PIN pad, menus, paging a
     * note) use the fast partial refresh and never flash. */
    if (s_force_full_next && s_host && s_host->display_full_refresh)
        s_host->display_full_refresh();
    s_force_full_next = false;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
}

static int batt_pct(void)
{
    return (s_host && s_host->battery_pct) ? s_host->battery_pct() : -1;
}

/* Battery % in the top-right corner. Used on every interactive screen (not the logo). */
static void draw_battery(lv_obj_t *scr)
{
    int p = batt_pct();
    if (p < 0) return;
    char t[16];
    snprintf(t, sizeof(t), "%d%%", p);
    lv_obj_t *l = mk_label(scr, t, 150, 2, 48, false);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
}

static void render_pin(void)
{
    clear_screen();
    lv_obj_t *scr = lv_scr_act();
    mk_label(scr, "Enter PIN", 4, 2, 0, false);

    char masked[PIN_MAX_LEN + 1];
    int i;
    for (i = 0; i < s_pin_len; i++) masked[i] = '*';
    masked[i] = '\0';
    mk_label(scr, masked[0] ? masked : "____", 4, 24, 0, false);

    const lv_coord_t cx[3] = {8, 74, 140};
    const lv_coord_t cw = 54;
    const lv_coord_t y0 = 44, dy = 30;
    for (int k = 0; k < 12; k++) {
        int row = k / 3, col = k % 3;
        lv_obj_t *cell = mk_label(scr, KEYS[k], cx[col], y0 + row * dy, cw, k == s_pin_sel);
        lv_obj_set_style_text_align(cell, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
    }
    /* button hints (short so both buttons fit one line) */
    mk_label(scr, BTN_UP"=move   "BTN_LOW"=enter", 4, 168, 192, false);
    mk_label(scr, "2x "BTN_LOW" = help", 4, 184, 192, false);
    draw_battery(scr);
}

static void render_message(const char *title, const char *body, msg_next_t next)
{
    s_state = ST_MESSAGE;
    s_msg_next = next;
    clear_screen();
    lv_obj_t *scr = lv_scr_act();
    mk_label(scr, title, 4, 6, 144, false);
    mk_label(scr, body, 4, 40, 192, false);
    mk_label(scr, "(press a button)", 4, 174, 192, false);
    draw_battery(scr);
}

/* Optional lost-device / owner-contact screen, shown before the PIN pad when the master has
 * enabled it. Helps a finder return a lost device. Any key continues to the normal flow. */
static void render_owner(void)
{
    s_state = ST_OWNER;
    clear_screen();
    lv_obj_t *scr = lv_scr_act();
    /* Show ONLY the owner's custom text (no hard-coded heading) -- the owner writes whatever they
     * want a finder to see. */
    mk_label(scr, dms_config_get()->owner_msg, 4, 6, 192, false);
    mk_label(scr, "(press a button)", 4, 178, 192, false);
}

/* Controls guide, reachable from any screen via a double-click of the lower button. */
static void render_help(void)
{
    s_state = ST_HELP;
    clear_screen();
    lv_obj_t *scr = lv_scr_act();
    mk_label(scr, "How to use", 4, 2, 192, false);
    mk_label(scr,
        BTN_UP"=upper  "BTN_LOW"=lower\n"
        BTN_UP" tap: move\n"
        "2x "BTN_UP": move back\n"
        BTN_LOW" tap: enter / open\n"
        "hold "BTN_UP": back / OK\n"
        "hold "BTN_LOW": sleep\n"
        "2x "BTN_LOW": this help",
        4, 22, 192, false);
    /* keep-powered note */
    mk_label(scr,
        "Keep on power/charge\nso the timer can finish.",
        4, 158, 192, false);
    draw_battery(scr);
}

static void render_menu(void)
{
    clear_screen();
    lv_obj_t *scr = lv_scr_act();
    mk_label(scr, s_menu_is_master ? "Master" : "Your files", 4, 2, 192, false);

    if (s_item_count == 0) {
        mk_label(scr, "(no files)", 4, 40, 192, false);
    } else {
        int first = s_menu_sel - MENU_WINDOW / 2;
        if (first < 0) first = 0;
        if (first > s_item_count - MENU_WINDOW) first = s_item_count - MENU_WINDOW;
        if (first < 0) first = 0;

        lv_coord_t y = 24;
        for (int i = first; i < s_item_count && i < first + MENU_WINDOW; i++) {
            /* Clip to one line so a long file name can't wrap over the next list item.
             * (Opening still uses the full s_items[i] name, not this display copy.) */
            char disp[MENU_NAME_MAX + 4];
            clip_name(disp, sizeof(disp), s_items[i], MENU_NAME_MAX);
            mk_label(scr, disp, 4, y, 192, i == s_menu_sel);
            y += 28;
        }
    }
    /* control hint for a first-time user (back/help live on the help screen: 2x LWR) */
    mk_label(scr, BTN_UP"=move  "BTN_LOW"=open", 4, 176, 192, false);
    draw_battery(scr);
}

/* How many characters from `start` fit in the body height. Measures a REAL wrapped label with
 * the same font/spacing/width the reader uses, so the fit matches the on-screen render exactly
 * (no overflow into the hint, nothing hidden). Always returns >= 1 so paging makes progress. */
static size_t reader_fit(size_t start)
{
    if (start >= s_file_len) return 0;

    lv_obj_t *m = lv_label_create(lv_scr_act());      /* transient measuring label */
    lv_obj_set_style_text_font(m, UI_FONT, 0);
    lv_obj_set_style_text_line_space(m, 0, 0);
    lv_obj_set_width(m, 192);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);

    size_t remaining = s_file_len - start;
    size_t lo = 1, hi = remaining < READER_MAX_PAGE_BYTES ? remaining : READER_MAX_PAGE_BYTES, best = 1;
    while (lo <= hi) {
        size_t mid = (lo + hi) / 2;
        char saved = s_filebuf[start + mid];
        s_filebuf[start + mid] = '\0';
        lv_label_set_text(m, &s_filebuf[start]);      /* label copies the text */
        s_filebuf[start + mid] = saved;
        lv_obj_update_layout(m);
        if (lv_obj_get_height(m) <= READER_AVAIL_H) { best = mid; lo = mid + 1; }
        else if (mid > 0)                           { hi = mid - 1; }
        else                                        break;
    }
    lv_obj_del(m);
    return best;
}

/* Byte offset where reader page `page` begins (walk measured pages from the top). */
static size_t reader_page_start(int page)
{
    size_t off = 0;
    for (int p = 0; p < page && off < s_file_len; p++) off += reader_fit(off);
    return off;
}

static void render_reader(void)
{
    /* Paging a note uses partial refreshes (no flashing). The full refresh that clears the
     * note's text happens only when the note is CLOSED (see on_reader_input) or before sleep. */
    clear_screen();
    lv_obj_t *scr = lv_scr_act();
    /* Header: clipped filename + page count, forced to a SINGLE clipped line (LONG_CLIP) so a long
     * name can never wrap down over the body or push the "x/N" into the text. */
    char dispname[READER_NAME_MAX + 4];
    clip_name(dispname, sizeof(dispname), s_reader_name, READER_NAME_MAX);
    char hdr[READER_NAME_MAX + 32];
    snprintf(hdr, sizeof(hdr), "%s  %d/%d", dispname, s_reader_page + 1, s_reader_pages);
    lv_obj_t *hl = lv_label_create(scr);
    lv_obj_set_style_text_font(hl, UI_FONT, 0);
    lv_obj_set_style_text_color(hl, lv_color_black(), 0);
    lv_label_set_long_mode(hl, LV_LABEL_LONG_CLIP);   /* never wrap; one line, clipped at width */
    lv_obj_set_width(hl, 144);                        /* narrower so it clears the battery % */
    lv_label_set_text(hl, hdr);
    lv_obj_set_pos(hl, 4, 2);

    /* Show exactly the run of characters that fits the body height, starting at s_reader_start.
       The next page resumes right where this one ends -- nothing is hidden behind the hint and
       nothing overflows it. (lv_label copies the text, so we can restore the byte after.) */
    size_t n = reader_fit(s_reader_start);
    char saved = s_filebuf[s_reader_start + n];
    s_filebuf[s_reader_start + n] = '\0';
    lv_obj_t *body = mk_label(scr, &s_filebuf[s_reader_start], 4, READER_BODY_Y, 192, false);
    lv_obj_set_style_text_line_space(body, 0, 0);  /* match reader_fit's measurement exactly */
    s_filebuf[s_reader_start + n] = saved;

    /* Only advertise paging when there's more than one page. */
    if (s_reader_pages > 1)
        mk_label(scr, BTN_UP"=page  "BTN_LOW"=back", 4, READER_HINT_Y, 192, false);
    else
        mk_label(scr, BTN_LOW"=back  (1 page)", 4, READER_HINT_Y, 192, false);
    draw_battery(scr);
}

/* Full-screen logo, with an optional one-line overlay (inverted strip at the bottom).
 * This is the face shown at boot, while idle, and -- because e-ink retains it -- the
 * whole time the device is in deep sleep. */
static void render_logo_face(const char *overlay)
{
    clear_screen();
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *img = lv_img_create(scr);
    if (overlay && overlay[0]) {
        /* Smaller logo at the top so the status line sits in a clean band below it,
         * instead of covering the "memento" wordmark. */
        lv_img_set_src(img, &dms_logo_small);   /* 140x140 */
        lv_obj_set_pos(img, 30, 4);             /* centered: (200-140)/2 */
        lv_obj_t *l = mk_label(scr, overlay, 0, 154, 200, false);   /* plain black text */
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        lv_img_set_src(img, &dms_logo);         /* idle/sleep: full-size logo */
        lv_obj_set_pos(img, 0, 0);
        int p = batt_pct();
        if (p >= 0 && p < (int)dms_config_get()->batt_critical_pct) {  /* critical -> warn */
            lv_obj_t *w = mk_label(scr, "Battery low - charge", 0, 0, 200, true);
            lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
        }
    }
}

/* Countdown face: logo + coarse remaining time. Granularity is ~1 hour because the
 * device sleeps in between and only wakes hourly to refresh this. */
static void render_countdown(uint32_t remaining)
{
    char dur[24];
    if (remaining >= 3600) {
        unsigned long h = remaining / 3600;
        unsigned long m = (remaining % 3600 + 59) / 60;   /* round the partial minute up */
        if (m >= 60) { h++; m = 0; }
        snprintf(dur, sizeof(dur), "~%luh %lum", h, m);
    } else if (remaining >= 60) {
        snprintf(dur, sizeof(dur), "~%lum", (unsigned long)((remaining + 59) / 60));
    } else {
        snprintf(dur, sizeof(dur), "<1m");
    }
    char ov[64];
    snprintf(ov, sizeof(ov), "Locked  %s\nremaining until unlock", dur);
    render_logo_face(ov);
}

/* ----------------------------------------------------------------- flow helpers */

/* Render the retained sleep face, give the LVGL task time to flush it to the e-ink,
 * then enter deep sleep (with an hourly wake while a countdown is pending).
 * NOTE: must be called from input_task (not under the LVGL mutex), since the actual
 * flush happens in the LVGL task. */
static void go_sleep(void)
{
    /* Persist any countdown progress before sleeping (survives a later power loss). */
    uint32_t now = s_host && s_host->now_unix ? s_host->now_unix() : 0;
    dms_vault_tick(now);

    /* If ANY alt countdown is still running, the sleep face is the small-logo countdown (with
     * the timer wake to refresh it) -- no matter which screen we slept from -- so the user
     * always sees how long remains until unlock. Otherwise show the plain idle logo. */
    int pid = -1; uint32_t rem = 0;
    bool in_countdown = (dms_vault_pending_check(now, &pid, &rem) == 1);

    /* Full (ghost-clearing) refresh for the image left on screen during deep sleep, so no
       text/menu shadows persist on the retained logo/countdown face. */
    s_force_full_next = true;
    if (in_countdown) render_countdown(rem);
    else              render_logo_face(NULL);

    vTaskDelay(pdMS_TO_TICKS(4000));   /* let LVGL flush + e-ink full refresh finish */

    if (in_countdown && s_host && s_host->enter_sleep_timer) {
        uint32_t refresh = dms_config_get()->countdown_refresh_sec;
        uint32_t wake = (rem < refresh) ? rem + 10 : refresh;
        s_host->enter_sleep_timer(wake);
    } else if (s_host && s_host->enter_sleep) {
        s_host->enter_sleep();
    }
}

static void show_pin(void)
{
    s_state = ST_PIN;
    s_pin_len = 0;
    s_pin_sel = 0;
    s_pin[0] = '\0';
    render_pin();
}

/* Build the menu for the principal in s_who (master: actions + all files; alt: its files). */
static void open_menu_for_principal(void)
{
    s_item_count = 0;
    s_menu_sel = 0;
    s_menu_is_master = (s_who.role == DMS_ROLE_MASTER);

    if (s_menu_is_master) {
        strcpy(s_items[s_item_count++], "[WiFi Upload & Config]");
        strcpy(s_items[s_item_count++], "[Deep Sleep]");
        char files[DMS_MAX_FILES][DMS_NAME_MAX];
        int nf = dms_vault_list_all_files(files, DMS_MAX_FILES);
        for (int i = 0; i < nf && s_item_count < LIST_MAX + 3; i++)
            strncpy(s_items[s_item_count++], files[i], DMS_NAME_MAX - 1);
    } else {
        char files[DMS_MAX_FILES][DMS_NAME_MAX];
        int nf = dms_vault_files_for_pin(s_who.id, files, DMS_MAX_FILES);
        for (int i = 0; i < nf && s_item_count < LIST_MAX; i++)
            strncpy(s_items[s_item_count++], files[i], DMS_NAME_MAX - 1);
    }
    s_state = ST_MENU;
    render_menu();
}

static void open_reader(const char *name)
{
    s_file_len = 0;
    if (!dms_vault_read_file(s_pin, s_who.id, name, s_filebuf, DMS_FILE_MAX_BYTES, &s_file_len)) {
        render_message("Error", "Could not open file.", MSG_TO_PIN);
        return;
    }
    s_filebuf[s_file_len] = '\0';
    strncpy(s_reader_name, name, sizeof(s_reader_name) - 1);
    s_reader_name[sizeof(s_reader_name) - 1] = '\0';
    s_reader_start = 0;
    s_reader_page = 0;
    /* Count pages by walking measured fits (so the "x/N" header is accurate). */
    s_reader_pages = 0;
    for (size_t off = 0; off < s_file_len; s_reader_pages++) off += reader_fit(off);
    if (s_reader_pages == 0) s_reader_pages = 1;
    s_state = ST_READER;
    render_reader();
}

static void show_wifi(void)
{
    char ssid[33] = "", pass[33] = "", url[48] = "";
    if (s_host && s_host->start_setup_wifi &&
        s_host->start_setup_wifi(ssid, sizeof(ssid), pass, sizeof(pass), url, sizeof(url))) {
        clear_screen();
        lv_obj_t *scr = lv_scr_act();
        char body[220];
        snprintf(body, sizeof(body),
                 "On phone/PC:\n"
                 "1) Join Wi-Fi\n   %s\n"
                 "2) Password\n   %s\n"
                 "3) Open in browser\n   %s\n"
                 "Add your files.",
                 ssid, pass, url);
        mk_label(scr, "WiFi Upload & Config", 4, 2, 192, false);
        mk_label(scr, body, 4, 22, 192, false);
        char idle[28];
        snprintf(idle, sizeof(idle), "%lu min, then sleeps",
                 (unsigned long)((dms_config_get()->wifi_idle_sec + 59) / 60));
        mk_label(scr, idle, 4, 168, 192, false);
        mk_label(scr, "hold "BTN_UP" = close", 4, 184, 192, false);
        draw_battery(scr);
        s_state = ST_WIFI;
        s_wifi_last_req = (s_host && s_host->wifi_req_count) ? s_host->wifi_req_count() : 0;
    } else {
        render_message("Wi-Fi", "Could not start setup AP.", MSG_TO_PIN);
    }
}

/* Show the countdown face; the idle timer deep-sleeps it (hourly timer wake refreshes). */
static void enter_countdown(uint32_t remaining)
{
    s_state = ST_COUNTDOWN;
    render_countdown(remaining);
}

/* alt principal just authenticated: arm + show countdown, or reveal if already elapsed */
static void handle_alt_access(void)
{
    uint32_t now = s_host && s_host->now_unix ? s_host->now_unix() : 0;
    uint32_t remaining = 0;
    dms_access_t a = dms_vault_alt_request(s_who.id, s_who.countdown_sec, now, &remaining);
    if (a == DMS_ACCESS_GRANTED)
        open_menu_for_principal();      /* countdown already done -> show files (have PIN) */
    else
        enter_countdown(remaining);     /* armed -> countdown screen, then deep sleep */
}

/* #9: escalating soft delay (seconds) by consecutive-wrong count. Capped at 30 s, so it is a
 * throttle, never a permanent (brick-risk) lockout. The count is PERSISTED in NVS (see pinguard_*)
 * so the delay survives a reboot -- a RAM-only counter would be reset by exactly the power-cycle an
 * attacker performs between guesses. */
static uint32_t wrong_pin_delay_sec(int count)
{
    static const uint32_t tbl[] = {0, 0, 1, 2, 5, 10, 20, 30};
    int n = (int)(sizeof(tbl) / sizeof(tbl[0]));
    if (count < 0) count = 0;
    return tbl[count < n ? count : n - 1];
}

/* #9 pinguard: persist the wrong-PIN throttle so it survives a reboot AND a power cut.
 *   PINGUARD_KEY_FAIL = consecutive wrong count (drives wrong_pin_delay_sec).
 *   PINGUARD_KEY_PEND = "a delay is owed but may not have been served" (set the instant a wrong
 *                       guess is registered, cleared only after the delay is actually served).
 * If power is cut mid-delay, PEND stays set and the NEXT attempt serves the delay BEFORE the guess
 * is evaluated -- so the cost cannot be skipped by power-cycling. A correct PIN or the recovery code
 * clears both, and the delay ceiling is low and absolute, so the owner is never locked out. */
#define PINGUARD_KEY_FAIL "pin_fail"
#define PINGUARD_KEY_PEND "pin_pend"

static void pinguard_reset(void)            /* correct PIN / recovery: clear the throttle */
{
    s_wrong_pin_count = 0;
    dms_secret_counter_set(PINGUARD_KEY_FAIL, 0);
    dms_secret_counter_set(PINGUARD_KEY_PEND, 0);
}

static void pinguard_serve(uint32_t secs)   /* block the input task for `secs` (watchdog still fed) */
{
    if (!secs) return;
    char body[48];
    snprintf(body, sizeof(body), "Too many wrong\ntries. Wait %lus.", (unsigned long)secs);
    render_message("Locked", body, MSG_TO_PIN);   /* transient; replaced once we continue */
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));
}

static void submit_pin(void)
{
    ESP_LOGI(TAG, "submit PIN (len %d)", s_pin_len);
    if (s_pin_len < PIN_MIN_LEN) {
        char body[64];
        snprintf(body, sizeof(body), "PIN too short\n(min %d digits).", PIN_MIN_LEN);
        render_message("PIN", body, MSG_TO_PIN);
        return;
    }

    /* Power-cut recovery: if a wrong-PIN penalty was owed but never fully served (power lost
     * mid-delay), serve it now, BEFORE evaluating this guess -- otherwise the delay is skippable
     * by cutting power. A correct PIN / recovery code below still clears the throttle afterwards. */
    if (dms_secret_counter_get(PINGUARD_KEY_PEND, 0)) {
        pinguard_serve(wrong_pin_delay_sec(s_wrong_pin_count));
        dms_secret_counter_set(PINGUARD_KEY_PEND, 0);
    }

    dms_pin_t info;
    dms_role_t role = dms_vault_verify_pin(s_pin, &info);
    ESP_LOGI(TAG, "verify -> role %d", (int)role);
    if (role != DMS_ROLE_NONE) {
        pinguard_reset();                          /* #9: a correct PIN clears the delay */
        s_who = info;
        if (role == DMS_ROLE_MASTER) {
            uint32_t now = s_host && s_host->now_unix ? s_host->now_unix() : 0;
            dms_vault_master_checkin(now);         /* the owner checked in: disarm everything */
            /* #4: if the device clock was fast-forwarded while counting down, tell the owner once
             * (advisory, never a lockout). master_checkin has already disarmed any countdown. */
            if (dms_vault_clock_tamper_seen()) {
                dms_vault_clear_clock_tamper();
                render_message("Clock warning",
                    "Device clock was\nmoved forward.\nA timer may have\nadvanced. Review\nyour settings.",
                    MSG_TO_MENU);
            } else {
                open_menu_for_principal();
            }
        } else {
            handle_alt_access();
        }
        return;
    }

    /* #8: not a PIN -- it may be the recovery code (checked BEFORE counting a wrong attempt, so a
     * legitimate recovery isn't throttled). A match resets the master PIN to the default "123". */
    if (dms_vault_has_recovery() && dms_vault_check_recovery(s_pin)) {
        pinguard_reset();
        if (dms_vault_recover_reset_master())
            render_message("Recovery", "Master PIN reset to 123.\nLog in with 123 and\nset a new PIN.", MSG_TO_PIN);
        else
            render_message("Recovery", "Could not reset the\nmaster PIN.", MSG_TO_PIN);
        return;
    }

    /* #9: genuinely wrong. Register the penalty in NVS (count + "owed" flag) BEFORE serving the
     * delay, so a power cut during the wait cannot erase it -- the next attempt's pre-gate serves
     * it. vTaskDelay yields, so the idle task still feeds the watchdog. */
    if (s_wrong_pin_count < 1000000) s_wrong_pin_count++;
    dms_secret_counter_set(PINGUARD_KEY_FAIL, (uint32_t)s_wrong_pin_count);
    dms_secret_counter_set(PINGUARD_KEY_PEND, 1);
    uint32_t d = wrong_pin_delay_sec(s_wrong_pin_count);
    render_message("Wrong PIN", "Incorrect PIN.\nPress to retry.", MSG_TO_PIN);
    if (d) vTaskDelay(pdMS_TO_TICKS(d * 1000));
    dms_secret_counter_set(PINGUARD_KEY_PEND, 0);   /* delay served -> clear the owed flag */
}

/* ----------------------------------------------------------------- input dispatch */

static void on_pin_input(ui_input_t in)
{
    switch (in) {
    case IN_NEXT:  s_pin_sel = (s_pin_sel + 1) % 12; render_pin(); break;
    case IN_PREV:  s_pin_sel = (s_pin_sel + 11) % 12; render_pin(); break;
    case IN_ENTER:
        if (s_pin_sel <= 8) {                     /* digits 1-9 */
            if (s_pin_len < PIN_MAX_LEN) { s_pin[s_pin_len++] = '1' + s_pin_sel; s_pin[s_pin_len] = '\0'; }
            render_pin();
        } else if (s_pin_sel == 9) {              /* DEL */
            if (s_pin_len > 0) { s_pin[--s_pin_len] = '\0'; }
            render_pin();
        } else if (s_pin_sel == 10) {             /* 0 */
            if (s_pin_len < PIN_MAX_LEN) { s_pin[s_pin_len++] = '0'; s_pin[s_pin_len] = '\0'; }
            render_pin();
        } else {                                  /* OK */
            submit_pin();
        }
        break;
    case IN_BACK:  submit_pin(); break;   /* hold BOOT = OK (submit the entered PIN) */
    case IN_SLEEP: go_sleep(); break;
    }
}

static void on_menu_input(ui_input_t in)
{
    switch (in) {
    case IN_NEXT:
        if (s_item_count > 0) { s_menu_sel = (s_menu_sel + 1) % s_item_count; render_menu(); }
        break;
    case IN_PREV:
        if (s_item_count > 0) { s_menu_sel = (s_menu_sel + s_item_count - 1) % s_item_count; render_menu(); }
        break;
    case IN_ENTER: {
        if (s_item_count == 0) break;
        const char *sel = s_items[s_menu_sel];
        if (s_menu_is_master && strcmp(sel, "[WiFi Upload & Config]") == 0) show_wifi();
        else if (s_menu_is_master && strcmp(sel, "[Deep Sleep]") == 0) go_sleep();
        else                                                           open_reader(sel);
        break;
    }
    case IN_BACK:  show_pin(); break;   /* log out to PIN (also lets master reclaim after a reveal) */
    case IN_SLEEP: go_sleep(); break;
    }
}

static void on_reader_input(ui_input_t in)
{
    switch (in) {
    case IN_NEXT:  s_reader_page = (s_reader_page + 1) % s_reader_pages;
                   s_reader_start = reader_page_start(s_reader_page); render_reader(); break;
    case IN_PREV:  s_reader_page = (s_reader_page + s_reader_pages - 1) % s_reader_pages;
                   s_reader_start = reader_page_start(s_reader_page); render_reader(); break;
    /* Leaving the reader -> force a full refresh on the menu draw so page text doesn't ghost. */
    case IN_ENTER: s_force_full_next = true; open_menu_for_principal(); break;   /* PWR click = back to list */
    case IN_BACK:  s_force_full_next = true; open_menu_for_principal(); break;   /* hold UPR  = back to list */
    case IN_SLEEP: go_sleep(); break;
    }
}

static void on_message_input(ui_input_t in)
{
    (void)in;
    if (s_msg_next == MSG_TO_SLEEP)      go_sleep();
    else if (s_msg_next == MSG_TO_MENU)  open_menu_for_principal();  /* continue into the master menu */
    else                                 show_pin();
}

static void on_wifi_input(ui_input_t in)
{
    if (in == IN_BACK || in == IN_SLEEP) {
        if (s_host && s_host->stop_setup_wifi) s_host->stop_setup_wifi();
        /* return to PIN (master may now log in) or sleep */
        show_pin();
    }
}

/* Countdown screen: ANY press (except hold-LWR=sleep) jumps to the PIN pad, so a present
 * master can check in / use the device immediately while an alt's countdown is running.
 * (Double-click LWR still opens Help, handled in input_task before dispatch.) */
static void on_countdown_input(ui_input_t in)
{
    if (in == IN_SLEEP) go_sleep();
    else                show_pin();
}

/* The user is present and wants in: go to the PIN pad. If an alt's countdown is already
 * complete, tell them first. (An armed-but-still-pending alt does NOT see the countdown
 * here -- the countdown face is only shown after the alt re-enters its PIN, or on the
 * hourly timer refresh while the device is otherwise asleep.) */
static void route(void)
{
    uint32_t now = s_host && s_host->now_unix ? s_host->now_unix() : 0;
    int pid = -1; uint32_t rem = 0;
    int pend = dms_vault_pending_check(now, &pid, &rem);
    (void)rem;
    if (pend == 2) {
        ESP_LOGW(TAG, "alt %d countdown elapsed -> prompt for PIN", pid);
        render_message("Access Ready", "Countdown complete.\nEnter your PIN\nto view your files.", MSG_TO_PIN);
    } else {
        show_pin();
    }
}

/* Entry point when a present person first interacts (button wake / first key on the logo):
 * show the optional owner/lost-device message first (if enabled and set), otherwise go straight
 * into the normal flow. The owner screen then continues to route() on any key. */
static void enter_or_route(void)
{
    const dms_config_t *c = dms_config_get();
    if (c->owner_msg_on && c->owner_msg[0]) render_owner();
    else                                    route();
}

/* Owner/lost-device message: any key continues into the normal flow. */
static void on_owner_input(ui_input_t in)
{
    if (in == IN_SLEEP) go_sleep();
    else                route();
}

/* Logo (idle) face: any key wakes it into the real flow. */
static void on_logo_input(ui_input_t in)
{
    if (in == IN_SLEEP) go_sleep();
    else                enter_or_route();
}

/* Help guide: any key returns to the normal flow. */
static void on_help_input(ui_input_t in)
{
    (void)in;
    route();
}

static void dispatch(ui_input_t in)
{
    switch (s_state) {
    case ST_LOGO:      on_logo_input(in); break;
    case ST_OWNER:     on_owner_input(in); break;
    case ST_PIN:       on_pin_input(in); break;
    case ST_MENU:      on_menu_input(in); break;
    case ST_READER:    on_reader_input(in); break;
    case ST_MESSAGE:   on_message_input(in); break;
    case ST_WIFI:      on_wifi_input(in); break;
    case ST_HELP:      on_help_input(in); break;
    case ST_COUNTDOWN: on_countdown_input(in); break;
    }
}

/* ----------------------------------------------------------------- input task */

/* Idle budget for the current state, in ticks. 0 = never auto-sleep. */
static int idle_limit_ticks(void)
{
    const dms_config_t *c = dms_config_get();
    if (s_state == ST_WIFI)   return (int)(c->wifi_idle_sec   * 1000 / TICK_MS); /* time to add files */
    if (s_face_only)          return IDLE_FACE_MS / TICK_MS;  /* woke only to refresh the face */
    if (s_state == ST_READER) return (int)(c->reader_idle_sec * 1000 / TICK_MS);
    return (int)(c->idle_sleep_sec * 1000 / TICK_MS);
}

static void input_task(void *arg)
{
    (void)arg;
    for (;;) {
        EventBits_t b = xEventGroupWaitBits(boot_groups, BIT_BOOT_SHORT | BIT_BOOT_LONG | BIT_BOOT_DOUBLE,
                                            pdTRUE, pdFALSE, pdMS_TO_TICKS(TICK_MS));
        EventBits_t p = xEventGroupWaitBits(pwr_groups, BIT_PWR_SHORT | BIT_PWR_DOUBLE | BIT_PWR_LONG,
                                            pdTRUE, pdFALSE, 0);
        /* Upper(BOOT): click=forward, double=back, hold=OK/back.  Lower(PWR): click=select/OK,
         * double=help (from any screen), hold=sleep. */
        bool acted = false;
        if (p & BIT_PWR_DOUBLE)  { ESP_LOGI(TAG, "PWR double -> HELP (state %d)", s_state);   render_help();      acted = true; }
        else {
            if (b & BIT_BOOT_SHORT)  { ESP_LOGI(TAG, "BOOT short -> NEXT (state %d)", s_state);   dispatch(IN_NEXT);  acted = true; }
            if (b & BIT_BOOT_DOUBLE) { ESP_LOGI(TAG, "BOOT double -> PREV (state %d)", s_state);  dispatch(IN_PREV);  acted = true; }
            if (p & BIT_PWR_SHORT)   { ESP_LOGI(TAG, "PWR short -> ENTER (state %d)", s_state);   dispatch(IN_ENTER); acted = true; }
            if (b & BIT_BOOT_LONG)   { ESP_LOGI(TAG, "BOOT long -> BACK/OK (state %d)", s_state); dispatch(IN_BACK);  acted = true; }
            if (p & BIT_PWR_LONG)    { ESP_LOGI(TAG, "PWR long -> SLEEP (state %d)", s_state);    dispatch(IN_SLEEP); acted = true; }
        }

        /* Idle -> deep sleep (1 min default, 5 min in the reader, off during WiFi setup). */
        if (acted) {
            s_idle_ticks = 0;
            s_face_only  = false;   /* a real person is here: normal timeouts apply */
        } else {
            /* On the WiFi page, browser traffic (not just button presses) defers the idle
               sleep, so an in-progress upload isn't cut off when no button is touched. */
            if (s_state == ST_WIFI && s_host && s_host->wifi_req_count) {
                uint32_t c = s_host->wifi_req_count();
                if (c != s_wifi_last_req) { s_wifi_last_req = c; s_idle_ticks = 0; }
            }
            int limit = idle_limit_ticks();
            if (limit > 0 && ++s_idle_ticks >= limit) {
                ESP_LOGI(TAG, "idle in state %d -> deep sleep", s_state);
                go_sleep();
                s_idle_ticks = 0;   /* not reached (deep sleep), but keep state sane */
            }
        }
    }
}

void dms_ui_start(const dms_ui_host_t *host, dms_wake_t wake)
{
    s_host = host;
    /* #9: restore the persisted wrong-PIN count so the throttle resumes where it left off across a
     * reboot/power-cut (any pending owed delay is served on the next submit_pin, see pinguard_*). */
    s_wrong_pin_count = (int)dms_secret_counter_get(PINGUARD_KEY_FAIL, 0);
    if (!dms_vault_has_master())
        ESP_LOGW(TAG, "no master PIN set (seeding may have failed)");

    /* First screen by wake cause:
     *  - timer wake: nobody is here; refresh the countdown/logo face, then the short
     *    idle budget puts us back to sleep (input_task does the actual sleeping --
     *    we run under the LVGL lock here and must not block).
     *  - button wake: the wake press IS the "click" -> go straight to the flow.
     *  - cold boot: show the logo; first key enters the flow. */
    uint32_t now = host && host->now_unix ? host->now_unix() : 0;
    dms_vault_tick(now);                 /* fold powered time into accrued progress */
    int pid = -1; uint32_t rem = 0;
    int pend = dms_vault_pending_check(now, &pid, &rem);

    if (wake == DMS_WAKE_TIMER) {
        ESP_LOGI(TAG, "timer wake: refreshing face (pend=%d)", pend);
        s_face_only = true;
        if (pend == 1)      enter_countdown(rem);
        else if (pend == 2) { s_state = ST_LOGO; render_logo_face("Ready - enter PIN"); }
        else                { s_state = ST_LOGO; render_logo_face(NULL); }
    } else if (wake == DMS_WAKE_BUTTON) {
        enter_or_route();
    } else {
        /* Cold boot / reflash: just show the logo. A key press -> route() (PIN pad). */
        s_state = ST_LOGO;
        render_logo_face(NULL);
    }

    xTaskCreatePinnedToCore(input_task, "dms_input", 6 * 1024, NULL, 6, NULL, 1);
}
