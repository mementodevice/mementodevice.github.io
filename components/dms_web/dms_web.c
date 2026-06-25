#include "dms_web.h"
#include "dms_vault.h"
#include "dms_config.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "dms_web";
static httpd_handle_t s_server = NULL;

/* Bumped on every request so the UI can tell the AP is in use and hold off the idle
 * timeout while the master is mid-upload (button-idle alone can't see web traffic). */
static volatile uint32_t s_req_count = 0;
uint32_t dms_web_req_count(void) { return s_req_count; }

/* Big enough to hold a max-size note even when URL-encoding inflates it (~3x worst case), so a
 * within-limit note is never silently truncated by the body cap; oversize is detected + rejected. */
#define BODY_MAX (DMS_FILE_MAX_BYTES * 3 + 1024)

/* #3 PIN policy for PINs SET through this web UI: digits only, within [min, DMS_PIN_MAX]. The master
 * minimum is higher because it is the brute-force-sensitive credential (every note is readable by
 * it); alt PINs are owner-recoverable (the master can re-issue one), so they keep a lower floor for
 * the non-technical heir entering it on the 2-button pad. The default master "123" is seeded inside
 * the vault, not here, so this minimum does not block first boot -- it forces the owner to pick a
 * stronger one when they change it. The on-device keypad is digits-only and caps at 12. */
#define DMS_MASTER_PIN_MIN 6
#define DMS_ALT_PIN_MIN    4
#define DMS_PIN_MAX        12
static bool pin_well_formed(const char *pin, int min)
{
    int n = (int)strlen(pin);
    if (n < min || n > DMS_PIN_MAX) return false;
    for (int i = 0; i < n; i++) if (pin[i] < '0' || pin[i] > '9') return false;
    return true;
}

/* ----------------------------------------------------------------- form parsing */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Find application/x-www-form-urlencoded field `key`, url-decode its value into out. */
static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if ((p == body || p[-1] == '&') && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t o = 0;
            while (*v && *v != '&' && o < cap - 1) {
                char c = *v;
                if (c == '+') { c = ' '; }
                else if (c == '%' && v[1] && v[2]) {
                    int hi = hexval(v[1]), lo = hexval(v[2]);
                    if (hi >= 0 && lo >= 0) { c = (char)((hi << 4) | lo); v += 2; }
                }
                out[o++] = c;
                v++;
            }
            out[o] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static bool form_present(const char *body, const char *key)
{
    char tmp[8];
    return form_field(body, key, tmp, sizeof(tmp));
}

/* Minimal HTML-escape into out (for showing user text back inside a page, e.g. the owner
 * message in its textarea). Stops cleanly if out fills up. */
static void html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 6 < cap; p++) {
        switch (*p) {
            case '<': memcpy(out + o, "&lt;",  4); o += 4; break;
            case '>': memcpy(out + o, "&gt;",  4); o += 4; break;
            case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
            default:  out[o++] = *p;
        }
    }
    out[o] = '\0';
}

static int recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    s_req_count++;
    size_t total = 0;
    int remaining = req->content_len;
    while (remaining > 0 && total < cap - 1) {
        int chunk = (remaining < (int)(cap - 1 - total)) ? remaining : (int)(cap - 1 - total);
        int r = httpd_req_recv(req, buf + total, chunk);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return -1;
        }
        total += r;
        remaining -= r;
    }
    buf[total] = '\0';
    return (int)total;
}

/* Redirect back to "/" carrying a short result code in ?m=, which get_root turns
   into a green (or red) confirmation banner so the user gets feedback on the action. */
static esp_err_t redirect_code(httpd_req_t *req, const char *code)
{
    char loc[16];
    snprintf(loc, sizeof(loc), "/?m=%s", code);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ----------------------------------------------------------------- GET / (page) */

static esp_err_t get_root(httpd_req_t *req)
{
    s_req_count++;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Memento Setup</title><style>"
        ":root{--bg:#f4f5f7;--card:#fff;--ink:#1c2330;--muted:#6b7280;"
        "--accent:#2f6df0;--danger:#e5484d;--ok:#1a7f4b;--line:#e5e7eb}"
        "*{box-sizing:border-box}body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;"
        "margin:0;background:var(--bg);color:var(--ink);line-height:1.45}"
        ".wrap{max-width:560px;margin:0 auto;padding:16px}"
        "h1{font-size:21px;margin:0 0 2px}.sub{color:var(--muted);font-size:13px;margin:0 0 16px}"
        ".card{background:var(--card);border:1px solid var(--line);border-radius:12px;"
        "padding:16px;margin:0 0 14px;box-shadow:0 1px 2px rgba(0,0,0,.04)}"
        ".card h2{font-size:15px;margin:0 0 10px}"
        "label{display:block;font-size:13px;font-weight:600;margin:10px 0 4px}"
        "input,textarea,select{width:100%;padding:9px 10px;border:1px solid var(--line);"
        "border-radius:8px;font-size:15px;font-family:inherit;background:#fff}textarea{resize:vertical}"
        "button{border:0;border-radius:8px;padding:9px 14px;font-size:14px;font-weight:600;"
        "color:#fff;background:var(--accent);cursor:pointer;margin-top:8px}"
        "button.del{background:var(--danger)}"   /* same shape/size/white text as the primary button, red */
        ".list{list-style:none;padding:0;margin:0}.list li{display:flex;align-items:center;"
        "justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--line)}"
        ".list li:last-child{border-bottom:0}"
        ".tag{font-size:11px;font-weight:700;padding:2px 8px;border-radius:20px;"
        "background:#eef2ff;color:var(--accent);margin-left:6px}.tag.m{background:#fdecec;color:var(--danger)}"
        ".empty{color:var(--muted);font-size:13px;font-style:italic}"
        ".banner{background:#e7f6ee;border:1px solid #b6e2c8;color:var(--ok);padding:10px 12px;"
        "border-radius:8px;font-size:14px;font-weight:600;margin:0 0 14px}"
        ".banner.err{background:#fdecec;border-color:#f3c0c2;color:var(--danger)}"
        ".chk{display:flex;align-items:center;gap:8px;font-weight:400;margin:6px 0;font-size:14px}"
        ".chk input{width:auto}.hint{color:var(--muted);font-size:12px;margin:8px 0 0}"
        "</style></head><body><div class=wrap>"
        "<h1>Memento Setup</h1><p class=sub>Manage PINs, files and device settings.</p>");

    /* success / error banner from the ?m= code set by the POST redirect */
    {
        char banner[160] = "";
        size_t qlen = httpd_req_get_url_query_len(req);
        if (qlen > 0 && qlen < 60) {
            char q[64];
            if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
                char m[8] = "";
                if (httpd_query_key_value(q, "m", m, sizeof(m)) == ESP_OK) {
                    const char *cls = "banner", *txt = NULL;
                    if      (!strcmp(m, "pa")) txt = "PIN added.";
                    else if (!strcmp(m, "pd")) txt = "PIN deleted.";
                    else if (!strcmp(m, "pe")) { txt = "Could not add that PIN (it may already exist)."; cls = "banner err"; }
                    else if (!strcmp(m, "pt")) { txt = "Alternative PIN must be 4 to 12 digits."; cls = "banner err"; }
                    else if (!strcmp(m, "mt")) { txt = "Master PIN must be 6 to 12 digits."; cls = "banner err"; }
                    else if (!strcmp(m, "fa")) txt = "File saved.";
                    else if (!strcmp(m, "fd")) txt = "File deleted.";
                    else if (!strcmp(m, "fu")) txt = "Note updated.";
                    else if (!strcmp(m, "fe")) { txt = "Could not save that file (needs a unique name and some content)."; cls = "banner err"; }
                    else if (!strcmp(m, "ft")) { txt = "That note is too large (max 20 KB of text)."; cls = "banner err"; }
                    else if (!strcmp(m, "cs")) txt = "Settings saved.";
                    else if (!strcmp(m, "ma")) txt = "Master PIN changed.";
                    else if (!strcmp(m, "me")) { txt = "Could not change the master PIN (in use by another PIN, or empty)."; cls = "banner err"; }
                    else if (!strcmp(m, "ra")) txt = "File readers updated.";
                    else if (!strcmp(m, "re")) { txt = "Could not update that file's readers."; cls = "banner err"; }
                    else if (!strcmp(m, "rn")) txt = "PIN name updated.";
                    else if (!strcmp(m, "rd")) txt = "Recovery code disabled.";
                    else if (!strcmp(m, "oa")) txt = "Lost-device message saved.";
                    if (txt) snprintf(banner, sizeof(banner), "<div class='%s'>%s</div>", cls, txt);
                }
            }
        }
        if (banner[0]) httpd_resp_sendstr_chunk(req, banner);
    }

    /* ===== Card: existing PINs (with names + rename; alts also deletable) ===== */
    /* 'static' on purpose: httpd serves one request at a time, and these big page buffers were
       overflowing the httpd task stack (crash: "stack overflow in task httpd") when the browser
       loaded the setup page. Keeping them off the request stack fixes that. */
    static dms_pin_t pins[DMS_MAX_PINS];
    int np = dms_vault_list_pins(pins, DMS_MAX_PINS);
    httpd_resp_sendstr_chunk(req, "<div class=card><h2>PINs</h2><ul class=list>");
    if (np == 0) httpd_resp_sendstr_chunk(req, "<li class=empty>No PINs yet</li>");
    for (int i = 0; i < np; i++) {
        bool master = (pins[i].role == DMS_ROLE_MASTER);
        char wait[40] = "";
        if (!master)
            snprintf(wait, sizeof(wait), " &middot; %luh %lum wait",
                     (unsigned long)(pins[i].countdown_sec / 3600),
                     (unsigned long)((pins[i].countdown_sec % 3600) / 60));
        char disp[DMS_NAME_MAX];                       /* bounded copy for the format string */
        strncpy(disp, pins[i].name[0] ? pins[i].name : "(unnamed)", sizeof(disp) - 1);
        disp[sizeof(disp) - 1] = '\0';
        char delbtn[200];
        strcpy(delbtn, master ? "" : "<button class=del formaction=/delpin onclick=\"return confirm('Delete this PIN? Files attached to this PIN will be unattached from it.')\">Delete</button>");
        char row[DMS_NAME_MAX * 2 + 720];
        snprintf(row, sizeof(row),
                 "<li><div style='width:100%%'>"
                 "<div>#%d <span class='tag%s'>%s</span> <b>%s</b>%s</div>"
                 "<form method=post action=/renamepin style='display:flex;gap:6px;margin-top:6px'>"
                 "<input type=hidden name=id value=%d>"
                 "<input name=label value=\"%s\" placeholder='name / label' style='flex:1'>"
                 "<button>Rename</button>%s"
                 "</form></div></li>",
                 pins[i].id, master ? " m" : "", master ? "MASTER" : "alt",
                 disp, wait, pins[i].id, pins[i].name, delbtn);
        httpd_resp_sendstr_chunk(req, row);
    }
    httpd_resp_sendstr_chunk(req, "</ul></div>");

    /* ===== Card: change the master PIN (separate group; master never added/deleted) ===== */
    httpd_resp_sendstr_chunk(req,
        "<div class=card><h2>Change master PIN</h2>"
        "<form method=post action=/setmaster>"
        "<label>New master PIN (6-12 digits)</label>"
        "<input name=pin inputmode=numeric minlength=6 maxlength=12 pattern=\"[0-9]*\">"
        "<p class=hint>Replaces the master PIN; all files stay readable by the master. Use at least "
        "6 digits &mdash; longer is stronger. (The default 123 should be changed immediately.)</p>"
        "<button>Change master PIN</button></form></div>");

    /* ===== Card: recovery code (optional, opt-in). Sits under Change master PIN since it is the
     * spare key for a forgotten master PIN. ===== */
    httpd_resp_sendstr_chunk(req,
        "<div class=card><h2>Recovery code (optional)</h2>"
        "<p class=hint>A long code you write down and keep somewhere safe. If the master PIN is "
        "ever forgotten, typing this code on the device resets the master PIN to 123 so you can "
        "get back in. Off by default &mdash; treat it like a spare key (anyone who has it can "
        "reset the master).</p>");
    httpd_resp_sendstr_chunk(req, dms_vault_has_recovery()
        ? "<p><b>Status: enabled.</b></p>"
          "<div style='display:flex;gap:8px'>"
          "<form method=post action=/genrecovery><button>Regenerate</button></form>"
          "<form method=post action=/norecovery onsubmit=\"return confirm('Disable the recovery code? The current code will stop working.')\"><button class=del>Disable</button></form></div></div>"
        : "<p>Status: disabled.</p>"
          "<form method=post action=/genrecovery><button>Generate recovery code</button></form></div>");

    /* ===== Card: add an alternative PIN (separate group; with a name/label) ===== */
    httpd_resp_sendstr_chunk(req,
        "<div class=card><h2>Add alternative PIN</h2>"
        "<form method=post action=/addpin>"
        "<label>New PIN (4-12 digits)</label>"
        "<input name=pin inputmode=numeric minlength=4 maxlength=12 pattern=\"[0-9]*\">"
        "<label>Name / label</label><input name=label placeholder='e.g. Spouse, Lawyer'>"
        "<label>Countdown before files unlock</label>"
        "<div style='display:flex;gap:8px'>"
        "<input name=hours type=number min=0 value=72 style='flex:1' aria-label=hours>"
        "<input name=mins type=number min=0 max=59 value=0 style='flex:1' aria-label=minutes>"
        "</div><p class=hint>Hours and minutes after this PIN is used before its files unlock.</p>"
        "<button>Add alternative PIN</button></form></div>");

    /* ===== Card: existing files (edit readers / delete). Master always reads (grayed). ===== */
    static char files[DMS_MAX_FILES][DMS_NAME_MAX];   /* static: keep off the httpd task stack */
    int nf = dms_vault_list_all_files(files, DMS_MAX_FILES);
    httpd_resp_sendstr_chunk(req, "<div class=card><h2>Files</h2><ul class=list>");
    if (nf == 0) httpd_resp_sendstr_chunk(req, "<li class=empty>No files yet</li>");
    for (int i = 0; i < nf; i++) {
        char hdr[DMS_NAME_MAX * 2 + 280];
        snprintf(hdr, sizeof(hdr),
                 "<li><form method=post action=/setreaders style='width:100%%'>"
                 "<input type=hidden name=name value='%s'>"
                 "<div><b>%s</b></div>"
                 "<p class=hint>Readable by:</p>"
                 "<label class=chk><input type=checkbox checked disabled> Master (always)</label>",
                 files[i], files[i]);
        httpd_resp_sendstr_chunk(req, hdr);
        for (int j = 0; j < np; j++) {
            if (pins[j].role == DMS_ROLE_MASTER) continue;   /* master shown grayed above */
            bool can = dms_vault_file_has_reader(files[i], pins[j].id);
            char lbl[DMS_NAME_MAX];
            strncpy(lbl, pins[j].name[0] ? pins[j].name : "alt", sizeof(lbl) - 1);
            lbl[sizeof(lbl) - 1] = '\0';
            char cb[DMS_NAME_MAX + 96];
            snprintf(cb, sizeof(cb),
                     "<label class=chk><input type=checkbox name=p%d%s> #%d %s</label>",
                     pins[j].id, can ? " checked" : "", pins[j].id, lbl);
            httpd_resp_sendstr_chunk(req, cb);
        }
        httpd_resp_sendstr_chunk(req,
            "<div style='display:flex;gap:8px;margin-top:8px'>"
            "<button formaction=/editfile>Edit</button>"
            "<button>Update readers</button>"
            "<button class=del formaction=/delfile onclick=\"return confirm('Delete this file? This cannot be undone.')\">Delete</button>"
            "</div></form></li>");
    }
    httpd_resp_sendstr_chunk(req, "</ul></div>");

    /* ===== Card: add a note (separate group). Master always reads (grayed). ===== */
    httpd_resp_sendstr_chunk(req,
        "<div class=card><h2>Add note</h2>"
        "<form method=post action=/addfile>"
        "<label>File name</label><input name=name value='Note' maxlength=47>"
        "<label>Content</label><textarea name=content rows=8 maxlength=20480></textarea>"
        "<p class=hint>Up to 20 KB of text per note.</p>"
        "<label>Readable by</label>"
        "<label class=chk><input type=checkbox checked disabled> Master (always)</label>");
    for (int i = 0; i < np; i++) {
        if (pins[i].role == DMS_ROLE_MASTER) continue;       /* master is grayed above */
        char lbl[DMS_NAME_MAX];
        strncpy(lbl, pins[i].name[0] ? pins[i].name : "alt", sizeof(lbl) - 1);
        lbl[sizeof(lbl) - 1] = '\0';
        char cb[DMS_NAME_MAX + 96];
        snprintf(cb, sizeof(cb),
                 "<label class=chk><input type=checkbox name=p%d> #%d %s</label>",
                 pins[i].id, pins[i].id, lbl);
        httpd_resp_sendstr_chunk(req, cb);
    }
    httpd_resp_sendstr_chunk(req,
        "<button>Save note</button></form></div>");

    /* ===== Card: lost-device / owner message (optional, shown before the PIN pad) ===== */
    const dms_config_t *cf = dms_config_get();
    {
        char head[700];
        snprintf(head, sizeof(head),
            "<div class=card><h2>Lost-device message</h2>"
            "<p class=hint>If on, this shows on the device screen <b>before</b> the PIN pad when a "
            "button is pressed, so a finder can return a lost device. It does not reveal any files.</p>"
            "<form method=post action=/ownermsg>"
            "<label class=chk><input type=checkbox name=on%s> Show this message before the PIN screen</label>"
            "<label>Message (max %d characters)</label>"
            "<textarea name=msg rows=4 maxlength=%d>",
            cf->owner_msg_on ? " checked" : "", DMS_OWNER_MSG_MAX, DMS_OWNER_MSG_MAX);
        httpd_resp_sendstr_chunk(req, head);

        static char esc[DMS_OWNER_MSG_MAX * 5 + 1];   /* static: escaped copy, off the stack */
        html_escape(cf->owner_msg, esc, sizeof(esc));
        /* IMPORTANT: never send an empty chunk -- a zero-length chunk terminates the chunked
         * HTTP response, which would cut off the Save button and every card after this one. */
        if (esc[0]) httpd_resp_sendstr_chunk(req, esc);

        httpd_resp_sendstr_chunk(req,
            "</textarea>"
            "<p class=hint>e.g. \"Owner: Jane Doe, +1 555 0100, jane@example.com\".</p>"
            "<button>Save message</button></form></div>");
    }

    /* settings (timeouts etc.) */
    static char st[1200];   /* static: keep off the httpd task stack */
    snprintf(st, sizeof(st),
        "<div class=card><h2>Settings (seconds)</h2>"
        "<form method=post action=/config>"
        "<label>Idle &rarr; sleep</label><input name=idle_sleep_sec value=%lu>"
        "<p class=hint>Inactivity on the PIN pad / menu / file list before the screen sleeps.</p>"
        "<label>Reader idle</label><input name=reader_idle_sec value=%lu>"
        "<p class=hint>Inactivity while reading a note before the screen sleeps.</p>"
        "<label>WiFi page idle</label><input name=wifi_idle_sec value=%lu>"
        "<p class=hint>Inactivity on this page before WiFi turns off and the screen sleeps.</p>"
        "<label>Auto wake-up interval</label><input name=countdown_refresh_sec value=%lu>"
        "<p class=hint>How often the device wakes itself (only while a countdown is running) "
        "to refresh the screen. Lower = more accurate display, more battery use.</p>"
        "<label>Battery-low %%</label><input name=batt_critical_pct value=%lu>"
        "<p class=hint>Below this charge, the idle screen shows a 'Battery low - charge' warning.</p>"
        "<button>Save settings</button></form></div>",
        (unsigned long)cf->idle_sleep_sec, (unsigned long)cf->reader_idle_sec,
        (unsigned long)cf->wifi_idle_sec, (unsigned long)cf->countdown_refresh_sec,
        (unsigned long)cf->batt_critical_pct);
    httpd_resp_sendstr_chunk(req, st);

    httpd_resp_sendstr_chunk(req, "</div></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);   /* end */
    return ESP_OK;
}

/* ----------------------------------------------------------------- POST handlers */

static esp_err_t post_addpin(httpd_req_t *req)
{
    char *body = malloc(BODY_MAX);
    if (!body) return ESP_FAIL;
    const char *code = "pe";
    if (recv_body(req, body, BODY_MAX) >= 0) {
        char pin[24] = "", hours[12] = "", mins[12] = "", label[DMS_NAME_MAX] = "";
        form_field(body, "pin", pin, sizeof(pin));
        form_field(body, "hours", hours, sizeof(hours));
        form_field(body, "mins", mins, sizeof(mins));
        form_field(body, "label", label, sizeof(label));
        if (pin[0]) {
            /* Web UI only ever creates ALTERNATIVE PINs; the master is seeded and changed
               via /setmaster, never added here. */
            if (!pin_well_formed(pin, DMS_ALT_PIN_MIN)) {
                code = "pt";                          /* #3: reject too-short / non-digit alt PINs */
            } else {
                uint32_t cd = (uint32_t)atoi(hours[0] ? hours : "0") * 3600U
                            + (uint32_t)atoi(mins[0] ? mins : "0") * 60U;
                int id = -1;
                if (dms_vault_add_pin(pin, DMS_ROLE_ALT, cd, &id)) {
                    if (label[0]) dms_vault_set_pin_name(id, label);
                    code = "pa";
                } else ESP_LOGW(TAG, "add_pin failed");
            }
        }
    }
    free(body);
    return redirect_code(req, code);
}

static esp_err_t post_renamepin(httpd_req_t *req)
{
    char body[128];
    const char *code = "rn";
    if (recv_body(req, body, sizeof(body)) >= 0) {
        char id[12] = "", label[DMS_NAME_MAX] = "";
        form_field(body, "id", id, sizeof(id));
        form_field(body, "label", label, sizeof(label));   /* empty clears the name */
        if (id[0]) dms_vault_set_pin_name(atoi(id), label);
    }
    return redirect_code(req, code);
}

static esp_err_t post_setmaster(httpd_req_t *req)
{
    char body[64];
    const char *code = "me";
    if (recv_body(req, body, sizeof(body)) >= 0) {
        char pin[24] = "";
        if (form_field(body, "pin", pin, sizeof(pin)) && pin[0]) {
            if (!pin_well_formed(pin, DMS_MASTER_PIN_MIN)) code = "mt";  /* #3: enforce >= 6 digits */
            else if (dms_vault_change_master_pin(pin))     code = "ma";
            /* else stays "me": rejected by the vault (e.g. in use by another PIN) */
        }
    }
    return redirect_code(req, code);
}

static esp_err_t post_delpin(httpd_req_t *req)
{
    char body[128];   /* shared rename/delete form also carries a label field */
    if (recv_body(req, body, sizeof(body)) >= 0) {
        char id[12] = "";
        if (form_field(body, "id", id, sizeof(id))) dms_vault_del_pin(atoi(id));
    }
    return redirect_code(req, "pd");
}

static esp_err_t post_addfile(httpd_req_t *req)
{
    char *body = malloc(BODY_MAX);
    /* +2 so a too-large paste fills past DMS_FILE_MAX_BYTES and we can DETECT it (and reject)
     * instead of silently truncating it -- an oversize note used to freeze the on-device reader. */
    char *content = malloc(DMS_FILE_MAX_BYTES + 2);
    if (!body || !content) { free(body); free(content); return ESP_FAIL; }

    const char *code = "fe";
    if (recv_body(req, body, BODY_MAX) >= 0) {
        char name[DMS_NAME_MAX] = "";
        form_field(body, "name", name, sizeof(name));
        if (form_field(body, "content", content, DMS_FILE_MAX_BYTES + 2) && name[0]) {
            size_t len = strlen(content);
            if (len > DMS_FILE_MAX_BYTES) {
                code = "ft";                 /* too large -> reject, don't truncate */
                ESP_LOGW(TAG, "add_file: note exceeds %d bytes, refused", DMS_FILE_MAX_BYTES);
            } else {
                /* which pins may read it */
                dms_pin_t pins[DMS_MAX_PINS];
                int np = dms_vault_list_pins(pins, DMS_MAX_PINS);
                int ids[DMS_MAX_PINS];
                int n = 0;
                for (int i = 0; i < np; i++) {
                    char key[8];
                    snprintf(key, sizeof(key), "p%d", pins[i].id);
                    if (form_present(body, key)) ids[n++] = pins[i].id;
                }
                if (len > 0 && dms_vault_add_file(name, content, len, ids, n)) code = "fa";
                else ESP_LOGW(TAG, "add_file failed");
            }
        }
    }
    free(body);
    free(content);
    return redirect_code(req, code);
}

static esp_err_t post_delfile(httpd_req_t *req)
{
    char body[DMS_NAME_MAX + 32];
    if (recv_body(req, body, sizeof(body)) >= 0) {
        char name[DMS_NAME_MAX] = "";
        if (form_field(body, "name", name, sizeof(name))) dms_vault_del_file(name);
    }
    return redirect_code(req, "fd");
}

/* Change which PINs may read an existing file (master always included). */
static esp_err_t post_setreaders(httpd_req_t *req)
{
    char *body = malloc(BODY_MAX);
    if (!body) return ESP_FAIL;
    const char *code = "re";
    if (recv_body(req, body, BODY_MAX) >= 0) {
        char name[DMS_NAME_MAX] = "";
        if (form_field(body, "name", name, sizeof(name)) && name[0]) {
            dms_pin_t pins[DMS_MAX_PINS];
            int np = dms_vault_list_pins(pins, DMS_MAX_PINS);
            int ids[DMS_MAX_PINS];
            int n = 0;
            for (int i = 0; i < np; i++) {
                char key[8];
                snprintf(key, sizeof(key), "p%d", pins[i].id);
                if (form_present(body, key)) ids[n++] = pins[i].id;
            }
            if (dms_vault_set_file_readers(name, ids, n)) code = "ra";
        }
    }
    free(body);
    return redirect_code(req, code);
}

/* Open the editor for a note: decrypt its content (master management path, no PIN) and return a
 * page with the text pre-filled in a textarea that saves to /savefile. */
static esp_err_t post_editfile(httpd_req_t *req)
{
    char *body    = malloc(BODY_MAX);
    char *content = malloc(DMS_FILE_MAX_BYTES + 1);
    if (!body || !content) { free(body); free(content); return ESP_FAIL; }

    char name[DMS_NAME_MAX] = "";
    size_t clen = 0;
    bool ok = false;
    if (recv_body(req, body, BODY_MAX) >= 0) {
        form_field(body, "name", name, sizeof(name));
        if (name[0]) ok = dms_vault_read_file_mgmt(name, content, DMS_FILE_MAX_BYTES, &clen);
    }

    httpd_resp_set_type(req, "text/html");
    if (!ok) {
        httpd_resp_sendstr(req,
            "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<body style='font-family:sans-serif;margin:16px'><p>Could not open that note.</p>"
            "<a href=/>Back</a></body>");
        free(body); free(content);
        return ESP_OK;
    }
    content[clen] = '\0';

    char namesc[DMS_NAME_MAX * 6];
    html_escape(name, namesc, sizeof(namesc));

    char head[1200];   /* fits the static markup + two escaped filenames (namesc up to ~288 each) */
    snprintf(head, sizeof(head),
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:-apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:#f4f5f7'>"
        "<div style='max-width:560px;margin:0 auto;padding:16px'>"
        "<h2 style='font-size:18px'>Edit note: %s</h2>"
        "<form method=post action=/savefile>"
        "<input type=hidden name=name value=\"%s\">"
        "<textarea name=content rows=16 maxlength=%d style='width:100%%;font-size:15px;"
        "padding:10px;border:1px solid #e5e7eb;border-radius:8px'>",
        namesc, namesc, DMS_FILE_MAX_BYTES);
    httpd_resp_sendstr_chunk(req, head);

    /* escaped content can be large (worst case ~6x); stream it from the heap */
    char *esc = malloc((size_t)clen * 6 + 1);
    if (esc) {
        html_escape(content, esc, (size_t)clen * 6 + 1);
        if (esc[0]) httpd_resp_sendstr_chunk(req, esc);   /* never send an empty chunk */
        free(esc);
    }

    httpd_resp_sendstr_chunk(req,
        "</textarea>"
        "<div style='display:flex;gap:8px;margin-top:10px'>"
        "<button style='border:0;border-radius:8px;padding:9px 14px;font-size:14px;font-weight:600;"
        "color:#fff;background:#2f6df0'>Save changes</button>"
        "<a href=/ style='padding:9px 14px;font-size:14px'>Cancel</a>"
        "</div></form><p style='color:#6b7280;font-size:12px'>Readers stay the same. Up to "
        "20 KB.</p></div></body>");
    httpd_resp_sendstr_chunk(req, NULL);   /* end chunked response */

    free(body); free(content);
    return ESP_OK;
}

/* Save edited note content, keeping the file's existing readers. */
static esp_err_t post_savefile(httpd_req_t *req)
{
    char *body    = malloc(BODY_MAX);
    char *content = malloc(DMS_FILE_MAX_BYTES + 2);
    if (!body || !content) { free(body); free(content); return ESP_FAIL; }

    const char *code = "fe";
    if (recv_body(req, body, BODY_MAX) >= 0) {
        char name[DMS_NAME_MAX] = "";
        form_field(body, "name", name, sizeof(name));
        if (form_field(body, "content", content, DMS_FILE_MAX_BYTES + 2) && name[0]) {
            size_t len = strlen(content);
            if (len > DMS_FILE_MAX_BYTES) code = "ft";
            else if (len > 0 && dms_vault_set_file_content(name, content, len)) code = "fu";
        }
    }
    free(body); free(content);
    return redirect_code(req, code);
}

static esp_err_t post_config(httpd_req_t *req)
{
    static const char *KEYS[] = {
        "idle_sleep_sec", "reader_idle_sec", "wifi_idle_sec",
        "countdown_refresh_sec", "batt_critical_pct",
    };
    char body[512];
    if (recv_body(req, body, sizeof(body)) >= 0) {
        for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
            char v[16];
            if (form_field(body, KEYS[i], v, sizeof(v)) && v[0])
                dms_config_set_u32(KEYS[i], (uint32_t)strtoul(v, NULL, 10));
        }
    }
    return redirect_code(req, "cs");
}

/* Save the optional lost-device message + its on/off toggle. An unchecked checkbox is simply
 * absent from the POST body, so its presence drives owner_msg_on. */
static esp_err_t post_ownermsg(httpd_req_t *req)
{
    char body[DMS_OWNER_MSG_MAX * 3 + 64];   /* url-encoding can triple each byte (%XX) */
    if (recv_body(req, body, sizeof(body)) >= 0) {
        char msg[DMS_OWNER_MSG_MAX + 1] = "";
        form_field(body, "msg", msg, sizeof(msg));
        dms_config_set_str("owner_msg", msg);
        dms_config_set_u32("owner_msg_on", form_present(body, "on") ? 1 : 0);
    }
    return redirect_code(req, "oa");
}

/* #8: generate a fresh recovery code, store only its verifier, and show the code ONCE directly in
 * the response (never via a redirect, so the secret never lands in a URL or log). */
static esp_err_t post_genrecovery(httpd_req_t *req)
{
    char body[16];
    recv_body(req, body, sizeof(body));        /* drain; also bumps the keep-alive counter */

    char code[11];                             /* 10 digits: enterable on the PIN pad (<=12) */
    for (int i = 0; i < 10; i++) code[i] = '0' + (esp_random() % 10);
    code[10] = '\0';

    httpd_resp_set_type(req, "text/html");
    if (!dms_vault_set_recovery(code)) {
        httpd_resp_sendstr(req,
            "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<body style='font-family:sans-serif;margin:16px'><p>Could not set a recovery code.</p>"
            "<a href=/>Back</a></body>");
        return ESP_OK;
    }
    char page[640];
    snprintf(page, sizeof(page),
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:sans-serif;margin:16px;max-width:560px'>"
        "<h2>Recovery code</h2>"
        "<p><b>Write this down now and keep it safe.</b> It is shown only once. Anyone who has it "
        "can reset the master PIN, so store it like a spare key.</p>"
        "<p style='font-size:2em;letter-spacing:4px'><b>%s</b></p>"
        "<p>To use it: type this code on the device's PIN pad. The master PIN resets to "
        "<b>123</b>, then log in and set a new one.</p>"
        "<p><a href=/>Done</a></p></body>", code);
    httpd_resp_sendstr(req, page);
    return ESP_OK;
}

static esp_err_t post_norecovery(httpd_req_t *req)
{
    char body[16];
    recv_body(req, body, sizeof(body));
    dms_vault_clear_recovery();
    return redirect_code(req, "rd");
}

/* ----------------------------------------------------------------- lifecycle */

static httpd_uri_t URIS[] = {
    { .uri = "/",        .method = HTTP_GET,  .handler = get_root },
    { .uri = "/addpin",  .method = HTTP_POST, .handler = post_addpin },
    { .uri = "/setmaster", .method = HTTP_POST, .handler = post_setmaster },
    { .uri = "/renamepin", .method = HTTP_POST, .handler = post_renamepin },
    { .uri = "/delpin",  .method = HTTP_POST, .handler = post_delpin },
    { .uri = "/addfile", .method = HTTP_POST, .handler = post_addfile },
    { .uri = "/delfile", .method = HTTP_POST, .handler = post_delfile },
    { .uri = "/setreaders", .method = HTTP_POST, .handler = post_setreaders },
    { .uri = "/editfile", .method = HTTP_POST, .handler = post_editfile },
    { .uri = "/savefile", .method = HTTP_POST, .handler = post_savefile },
    { .uri = "/genrecovery", .method = HTTP_POST, .handler = post_genrecovery },
    { .uri = "/norecovery", .method = HTTP_POST, .handler = post_norecovery },
    { .uri = "/ownermsg", .method = HTTP_POST, .handler = post_ownermsg },
    { .uri = "/config",  .method = HTTP_POST, .handler = post_config },
};

bool dms_web_start(void)
{
    if (s_server) return true;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 12288;   /* 8192 overflowed while building the setup page (see static bufs) */
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return false;
    }
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++)
        httpd_register_uri_handler(s_server, &URIS[i]);
    ESP_LOGI(TAG, "setup web server started");
    return true;
}

void dms_web_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "setup web server stopped");
    }
}
