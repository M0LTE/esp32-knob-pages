// Settings web page: plain server-side HTML, no scripts from elsewhere.
// On the set-up AP it is open (the AP itself has a password) and every
// unknown URL redirects to it, so phones pop it up as a sign-in page. On the
// LAN it needs HTTP basic auth with the admin password.
#include "web.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include "esp_http_server.h"
#include "ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "model.h"
#include "net.h"
#include "platform.h"
#include "settings.h"

static const char *TAG = "web";

// ---- small output buffer ----

typedef struct {
    char *p;
    size_t n, cap;
} sbuf_t;

static void sb_grow(sbuf_t *b, size_t need)
{
    if (b->n + need + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + need + 1) cap *= 2;
    char *np = heap_caps_realloc(b->p, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!np) return;
    b->p = np;
    b->cap = cap;
}

static void sb_printf(sbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    sb_grow(b, n);
    if (b->n + n + 1 > b->cap) return;
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    b->n += n;
}

static void sb_esc(sbuf_t *b, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_printf(b, "&amp;"); break;
        case '<': sb_printf(b, "&lt;"); break;
        case '>': sb_printf(b, "&gt;"); break;
        case '"': sb_printf(b, "&quot;"); break;
        default: sb_printf(b, "%c", *s);
        }
    }
}

// ---- auth ----

static bool authorised(httpd_req_t *req, const settings_t *s)
{
    if (net_mode() == NET_AP || !s->admin_pass[0]) return true;
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK &&
        !strncmp(hdr, "Basic ", 6)) {
        unsigned char dec[120];
        size_t n = 0;
        if (mbedtls_base64_decode(dec, sizeof dec - 1, &n, (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[n] = 0;
            char *colon = strchr((char *)dec, ':');
            if (colon && !strcmp(colon + 1, s->admin_pass)) return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"desk-knob\"");
    httpd_resp_sendstr(req, "Password needed (user name: admin)\n");
    return false;
}

// ---- form decoding ----

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Finds name=value in an urlencoded body and decodes it. False if absent.
static bool form_get(const char *body, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    const char *p = body;
    while (p && *p) {
        if (!strncmp(p, name, nl) && p[nl] == '=') {
            p += nl + 1;
            size_t o = 0;
            while (*p && *p != '&') {
                char c = *p++;
                if (c == '+') c = ' ';
                else if (c == '%' && hexv(p[0]) >= 0 && hexv(p[1]) >= 0) {
                    c = (char)(hexv(p[0]) * 16 + hexv(p[1]));
                    p += 2;
                }
                if (o + 1 < n) out[o++] = c;
            }
            out[o] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

// ---- page ----

static const char *CSS =
    "body{font-family:sans-serif;max-width:40em;margin:1em auto;padding:0 1em;background:#111;color:#eee}"
    "h1{font-size:1.3em;color:#ff9f1c}h2{font-size:1em;margin-top:1.6em;border-bottom:1px solid #333}"
    "label{display:block;margin:.6em 0 .2em;color:#aaa;font-size:.9em}"
    "input,textarea{width:100%;box-sizing:border-box;padding:.5em;background:#222;color:#eee;border:1px solid #444;"
    "border-radius:4px;font-size:1em}input[type=checkbox]{width:auto}textarea{font-family:monospace;height:9em}"
    "button{margin-top:1.2em;padding:.7em 1.4em;font-size:1em;background:#ff9f1c;border:0;border-radius:4px}"
    ".note{color:#888;font-size:.85em}.warn{color:#f5d547}table{font-size:.85em;color:#aaa}td{padding-right:1em}";

static void field(sbuf_t *b, const char *label, const char *name, const char *type, const char *val, const char *note)
{
    sb_printf(b, "<label for=%s>%s</label><input id=%s name=%s type=%s value=\"", name, label, name, name, type);
    sb_esc(b, val);
    sb_printf(b, "\" autocapitalize=off autocorrect=off spellcheck=false>");
    if (note) sb_printf(b, "<div class=note>%s</div>", note);
}

static void head(sbuf_t *b, const char *title)
{
    sb_printf(b, "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,"
                 "initial-scale=1\"><title>%s</title><style>%s</style></head><body>", title, CSS);
}

static esp_err_t send_buf(httpd_req_t *req, sbuf_t *b)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, b->p ? b->p : "", b->n);
    free(b->p);
    return e;
}

static esp_err_t page_root(httpd_req_t *req)
{
    settings_t s;
    settings_get(&s);
    if (!authorised(req, &s)) return ESP_OK;
    bool setup = net_mode() == NET_AP;
    sbuf_t b = {0};
    head(&b, "Desk knob");
    sb_printf(&b, "<h1>Desk knob%s</h1>", setup ? " set-up" : "");
    if (setup) {
        sb_printf(&b, "<p>Enter your Wi-Fi network and choose an admin password for this page. "
                      "After Apply the knob restarts and joins your network; the settings page is then at "
                      "<b>http://%s/</b> (user name <b>admin</b>).</p>", s.hostname);
    } else {
        model_lock();
        bool ku = g_model.kiss_up, mu = g_model.modem_up;
        model_unlock();
        sb_printf(&b, "<table><tr><td>Firmware</td><td>%s</td></tr><tr><td>Address</td><td>%s, signal %d dBm</td></tr>"
                      "<tr><td>Up</td><td>%lu min</td></tr><tr><td>Free memory</td><td>%u KB internal, %u KB PSRAM</td></tr>"
                      "<tr><td>KISS</td><td>%s</td></tr><tr><td>Metrics</td><td>%s</td></tr></table>",
                  esp_app_get_description()->version, net_ip(), net_rssi(),
                  (unsigned long)(esp_timer_get_time() / 60000000),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), ku ? "connected" : "down",
                  mu ? "ok" : "failing");
        if (!s.admin_pass[0]) sb_printf(&b, "<p class=warn>No admin password is set, so this page is open to the LAN.</p>");
    }
    sb_printf(&b, "<form method=post action=/save>");
    sb_printf(&b, "<h2>Wi-Fi</h2>");
    field(&b, "Network name (SSID)", "ssid", "text", s.ssid, NULL);
    field(&b, "Password", "pass", "password", "", s.pass[0] ? "Leave blank to keep the current password." : NULL);
    field(&b, "Host name", "hostname", "text", s.hostname, "Given to the router by DHCP.");
    sb_printf(&b, "<h2>Admin password</h2>");
    field(&b, "Admin password for this page", "admin", "password", "",
          s.admin_pass[0] ? "Leave blank to keep the current one." : "Needed: at least 6 characters.");

    if (!setup) {
        sb_printf(&b, "<h2>Pages</h2><label for=pages>One per line: type, then title. Types: hosts, station, modem, energy, rooms, sky, waterfall, gpsdo.</label>"
                      "<textarea id=pages name=pages>");
        for (int i = 0; i < s.n_pages; i++) {
            sb_esc(&b, s.pages[i].type);
            sb_printf(&b, " ");
            sb_esc(&b, s.pages[i].title);
            sb_printf(&b, "\n");
        }
        sb_printf(&b, "</textarea><h2>Hosts to ping</h2><label for=hosts>One per line: name, then IP address or "
                      "DNS name. Up to %d.</label><textarea id=hosts name=hosts>", SET_MAX_HOSTS);
        for (int i = 0; i < s.n_hosts; i++) {
            sb_esc(&b, s.hosts[i].name);
            sb_printf(&b, " ");
            sb_esc(&b, s.hosts[i].addr);
            sb_printf(&b, "\n");
        }
        char port[8];
        snprintf(port, sizeof port, "%d", s.kiss_port);
        sb_printf(&b, "</textarea><h2>KISS feed (listen only)</h2>");
        field(&b, "Host", "kiss_host", "text", s.kiss_host, NULL);
        field(&b, "Port", "kiss_port", "number", port, NULL);
        field(&b, "Station call (frames from it count as sent)", "kiss_name", "text", s.kiss_name, NULL);
        sb_printf(&b, "<h2>Modem metrics</h2>");
        field(&b, "Prometheus URL", "metrics_url", "text", s.metrics_url, NULL);
        field(&b, "Name", "metrics_name", "text", s.metrics_name, NULL);
        char poll[8];
        snprintf(poll, sizeof poll, "%d", s.energy_poll_s);
        sb_printf(&b, "<h2>Home energy (Home Assistant)</h2>");
        field(&b, "Home Assistant URL", "energy_url", "text", s.energy_url, NULL);
        field(&b, "Long-lived access token", "energy_token", "password", "",
              s.energy_token[0] ? "Set. Leave blank to keep it." : "Not set: the Energy page stays empty until it is.");
        field(&b, "Poll every (seconds)", "energy_poll", "number", poll, NULL);
        sb_printf(&b, "<label for=energy_ent>Entities, one per line: name, then entity id (name alone: not used).</label>"
                      "<textarea id=energy_ent name=energy_ent style=height:16em>");
        for (int i = 0; i < EN_COUNT; i++) {
            sb_printf(&b, "%s ", energy_keys[i]);
            sb_esc(&b, s.energy_ent[i]);
            sb_printf(&b, "\n");
        }
        sb_printf(&b, "</textarea>");
        sb_printf(&b, "<h2>Rooms (climate controller)</h2>");
        field(&b, "Climate controller URL", "brain_url", "text", s.brain_url, NULL);
        sb_printf(&b, "<label for=rooms>One room per line: zone id, name (use _ for a space), target helper, then "
                      "optionally a night helper and its hours, e.g. HH:MM-HH:MM. The knob writes the helper in "
                      "force, through the Home Assistant URL and token above.</label><textarea id=rooms name=rooms "
                      "style=height:10em>");
        for (int i = 0; i < s.n_rooms; i++) {
            char line[200];
            rooms_cfg_line(&s.rooms[i], line, sizeof line);
            sb_esc(&b, line);
            sb_printf(&b, "\n");
        }
        sb_printf(&b, "</textarea>");
        {
            char lat[16] = "", lon[16] = "", rad[12];
            if (!isnan(s.home_lat) && !isnan(s.home_lon)) {
                snprintf(lat, sizeof lat, "%.5f", s.home_lat);
                snprintf(lon, sizeof lon, "%.5f", s.home_lon);
            }
            snprintf(rad, sizeof rad, "%.0f", s.adsb_radius_km);
            sb_printf(&b, "<h2>Aircraft (ADS-B)</h2><label><input type=checkbox name=adsb_on value=1%s> Show aircraft "
                          "near home</label>", s.adsb_enabled ? " checked" : "");
            field(&b, "Home latitude (decimal degrees, north positive)", "home_lat", "text", lat, NULL);
            field(&b, "Home longitude (decimal degrees, east positive, so west is negative)", "home_lon", "text", lon,
                  "Blank: no aircraft are fetched. Your location stays on the knob; the aircraft feed is asked for a "
                  "box round it.");
            field(&b, "Radius (km)", "adsb_radius", "number", rad, NULL);
        }
        sb_printf(&b, "<h2>MQTT and GPSDO</h2>");
        field(&b, "MQTT broker URL", "mqtt_url", "text", s.mqtt_url, "e.g. mqtt://broker (subscribe only: the knob never publishes)");
        field(&b, "GPSDO status topic", "gpsdo_topic", "text", s.gpsdo_topic, NULL);
        field(&b, "GPSDO name", "gpsdo_name", "text", s.gpsdo_name, NULL);
        sb_printf(&b, "<h2>Waterfall (pdn-soundmodem station page)</h2>");
        field(&b, "WebSocket URL", "wf_url", "text", s.wf_url, "e.g. ws://host:8099/ (listen only: the knob never sends on it)");
        field(&b, "Name", "wf_name", "text", s.wf_name, NULL);
        char vol[8];
        snprintf(vol, sizeof vol, "%d", s.volume);
        sb_printf(&b, "<h2>Alerts</h2><label><input type=checkbox name=sound value=1%s> Sound on alerts</label>",
                  s.sound ? " checked" : "");
        field(&b, "Volume (0 to 100)", "volume", "number", vol, NULL);
        sb_printf(&b, "<label><input type=checkbox name=quiet value=1%s> Quiet hours (no sound), UK time</label>",
                  s.quiet ? " checked" : "");
        field(&b, "From", "quiet_from", "time", s.quiet_from, NULL);
        field(&b, "To", "quiet_to", "time", s.quiet_to, NULL);
    }
    sb_printf(&b, "<input type=hidden name=form value=%s><button type=submit>Apply</button></form>",
              setup ? "setup" : "full");
    if (!setup)
        sb_printf(&b, "<form method=post action=/test><button type=submit>Play the alert sound</button></form>"
                      "<p class=note><a style=color:#888 href=/settings.json>settings.json</a> (without passwords)</p>");
    sb_printf(&b, "</body></html>");
    return send_buf(req, &b);
}

static void restart_cb(void *arg) { esp_restart(); }

static void restart_soon(void)
{
    const esp_timer_create_args_t t = {.callback = restart_cb, .name = "restart"};
    esp_timer_handle_t h;
    if (esp_timer_create(&t, &h) == ESP_OK) esp_timer_start_once(h, 1500 * 1000);
}

static esp_err_t message(httpd_req_t *req, const char *title, const char *msg)
{
    sbuf_t b = {0};
    head(&b, title);
    sb_printf(&b, "<h1>%s</h1><p>%s</p><p><a style=color:#ff9f1c href=/>Back</a></p></body></html>", title, msg);
    return send_buf(req, &b);
}

static esp_err_t page_save(httpd_req_t *req)
{
    // Two copies of the settings are too big for the server's stack; keep them
    // in PSRAM (the server handles one request at a time).
    static settings_t *save_old, *save_new;
    if (!save_old) save_old = heap_caps_malloc(sizeof *save_old, MALLOC_CAP_SPIRAM);
    if (!save_new) save_new = heap_caps_malloc(sizeof *save_new, MALLOC_CAP_SPIRAM);
    if (!save_old || !save_new) return ESP_FAIL;
#define old (*save_old)
#define s (*save_new)
    settings_get(&old);
    if (!authorised(req, &old)) return ESP_OK;
    if (req->content_len > 8192) return message(req, "Too big", "The form was too large.");
    char *body = calloc(1, req->content_len + 1);
    if (!body) return ESP_FAIL;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    s = old;
    char *v = malloc(2048);
    bool full = form_get(body, "form", v, 2048) && !strcmp(v, "full");

    if (form_get(body, "ssid", v, 64)) { trim(v); snprintf(s.ssid, sizeof s.ssid, "%s", v); }
    if (form_get(body, "pass", v, 80) && v[0]) snprintf(s.pass, sizeof s.pass, "%s", v);
    if (form_get(body, "hostname", v, 64)) {
        // DHCP host names: letters, digits and hyphens only.
        trim(v);
        char *o = v;
        for (char *p = v; *p; p++)
            if (isalnum((unsigned char)*p) || *p == '-') *o++ = (char)tolower((unsigned char)*p);
        *o = 0;
        if (v[0]) snprintf(s.hostname, sizeof s.hostname, "%s", v);
    }
    if (form_get(body, "admin", v, 64) && v[0]) {
        if (strlen(v) < 6) {
            free(v); free(body);
            return message(req, "Not saved", "The admin password needs at least 6 characters.");
        }
        snprintf(s.admin_pass, sizeof s.admin_pass, "%s", v);
    }
    if (!s.admin_pass[0]) {
        free(v); free(body);
        return message(req, "Not saved", "Please choose an admin password (at least 6 characters).");
    }
    if (!s.ssid[0]) {
        free(v); free(body);
        return message(req, "Not saved", "Please enter the Wi-Fi network name.");
    }

    if (full) {
        if (form_get(body, "pages", v, 2048)) {
            s.n_pages = 0;
            for (char *line = strtok(v, "\r\n"); line && s.n_pages < SET_MAX_PAGES; line = strtok(NULL, "\r\n")) {
                trim(line);
                if (!line[0]) continue;
                set_page_t *p = &s.pages[s.n_pages];
                memset(p, 0, sizeof *p);
                char *sp = strchr(line, ' ');
                if (sp) *sp++ = 0;
                snprintf(p->type, sizeof p->type, "%s", line);
                if (sp) { trim(sp); snprintf(p->title, sizeof p->title, "%s", sp); }
                s.n_pages++;
            }
        }
        if (form_get(body, "hosts", v, 2048)) {
            s.n_hosts = 0;
            for (char *line = strtok(v, "\r\n"); line && s.n_hosts < SET_MAX_HOSTS; line = strtok(NULL, "\r\n")) {
                trim(line);
                if (!line[0]) continue;
                set_host_t *h = &s.hosts[s.n_hosts];
                memset(h, 0, sizeof *h);
                char *sp = strpbrk(line, " \t");
                if (sp) {
                    *sp++ = 0;
                    trim(sp);
                    snprintf(h->name, sizeof h->name, "%s", line);
                    snprintf(h->addr, sizeof h->addr, "%s", sp);
                } else {
                    snprintf(h->name, sizeof h->name, "%s", line);
                    snprintf(h->addr, sizeof h->addr, "%s", line);
                }
                s.n_hosts++;
            }
        }
        if (form_get(body, "kiss_host", v, 64)) { trim(v); snprintf(s.kiss_host, sizeof s.kiss_host, "%s", v); }
        if (form_get(body, "kiss_port", v, 16)) s.kiss_port = atoi(v);
        if (form_get(body, "kiss_name", v, 64)) { trim(v); snprintf(s.kiss_name, sizeof s.kiss_name, "%s", v); }
        if (form_get(body, "metrics_url", v, 200)) { trim(v); snprintf(s.metrics_url, sizeof s.metrics_url, "%s", v); }
        if (form_get(body, "metrics_name", v, 64)) { trim(v); snprintf(s.metrics_name, sizeof s.metrics_name, "%s", v); }
        if (form_get(body, "energy_url", v, 200)) { trim(v); snprintf(s.energy_url, sizeof s.energy_url, "%s", v); }
        if (form_get(body, "energy_token", v, 400)) {
            trim(v);
            if (v[0]) snprintf(s.energy_token, sizeof s.energy_token, "%s", v);
        }
        if (form_get(body, "energy_poll", v, 16)) {
            int p = atoi(v);
            s.energy_poll_s = p < 5 ? 5 : p > 3600 ? 3600 : p;
        }
        if (form_get(body, "energy_ent", v, 2048)) {
            for (char *line = strtok(v, "\r\n"); line; line = strtok(NULL, "\r\n")) {
                trim(line);
                // "name entity_id", or just "name" to leave that reading out.
                char *sp = strpbrk(line, " \t");
                if (sp) {
                    *sp++ = 0;
                    trim(sp);
                } else {
                    sp = line + strlen(line);
                }
                for (int i = 0; i < EN_COUNT; i++)
                    if (!strcmp(line, energy_keys[i])) snprintf(s.energy_ent[i], sizeof s.energy_ent[i], "%s", sp);
            }
        }
        if (form_get(body, "brain_url", v, 200)) { trim(v); snprintf(s.brain_url, sizeof s.brain_url, "%s", v); }
        if (form_get(body, "rooms", v, 2048)) s.n_rooms = rooms_cfg_parse(v, s.rooms, MODEL_MAX_ROOMS);
        s.adsb_enabled = form_get(body, "adsb_on", v, 8);
        if (form_get(body, "home_lat", v, 32) && form_get(body, "home_lon", v + 64, 32)) {
            trim(v);
            trim(v + 64);
            char *e1, *e2;
            float la = strtof(v, &e1), lo = strtof(v + 64, &e2);
            if (!v[0] || !v[64]) {
                s.home_lat = s.home_lon = NAN;
            } else if (*e1 || *e2 || e1 == v || e2 == v + 64 || la < -90 || la > 90 || lo < -180 || lo > 180) {
                free(v);
                free(body);
                return message(req, "Not saved", "Home latitude and longitude should be decimal degrees, e.g. 51.5 "
                                                  "and -0.12, or both blank.");
            } else {
                s.home_lat = la;
                s.home_lon = lo;
            }
        }
        if (form_get(body, "adsb_radius", v, 16)) {
            float r = strtof(v, NULL);
            s.adsb_radius_km = r >= 2 && r <= 100 ? r : 25;
        }
        if (form_get(body, "mqtt_url", v, 200)) { trim(v); snprintf(s.mqtt_url, sizeof s.mqtt_url, "%s", v); }
        if (form_get(body, "gpsdo_topic", v, 128)) { trim(v); snprintf(s.gpsdo_topic, sizeof s.gpsdo_topic, "%s", v); }
        if (form_get(body, "gpsdo_name", v, 64)) { trim(v); snprintf(s.gpsdo_name, sizeof s.gpsdo_name, "%s", v); }
        if (form_get(body, "wf_url", v, 200)) { trim(v); snprintf(s.wf_url, sizeof s.wf_url, "%s", v); }
        if (form_get(body, "wf_name", v, 64)) { trim(v); snprintf(s.wf_name, sizeof s.wf_name, "%s", v); }
        s.sound = form_get(body, "sound", v, 8);
        s.quiet = form_get(body, "quiet", v, 8);
        if (form_get(body, "volume", v, 16)) {
            s.volume = atoi(v);
            if (s.volume < 0) s.volume = 0;
            if (s.volume > 100) s.volume = 100;
        }
        if (form_get(body, "quiet_from", v, 16)) snprintf(s.quiet_from, sizeof s.quiet_from, "%.5s", v);
        if (form_get(body, "quiet_to", v, 16)) snprintf(s.quiet_to, sizeof s.quiet_to, "%.5s", v);
    }
    free(v);
    free(body);

    bool reboot = net_mode() == NET_AP || strcmp(s.ssid, old.ssid) || strcmp(s.pass, old.pass) ||
                  strcmp(s.hostname, old.hostname) || s.n_pages != old.n_pages ||
                  memcmp(s.pages, old.pages, sizeof s.pages);
    if (!settings_save(&s)) return message(req, "Not saved", "Writing the settings to flash failed.");
    if (reboot) {
        char msg[200];
        snprintf(msg, sizeof msg, "The knob is restarting to apply them. Its page will then be at http://%s/", s.hostname);
        message(req, "Saved", msg);
        restart_soon();
        return ESP_OK;
    }
    return message(req, "Saved", "Applied. The data sources pick up the change within a few seconds.");
}

#undef old
#undef s

static esp_err_t page_test(httpd_req_t *req)
{
    settings_t s;
    settings_get(&s);
    if (!authorised(req, &s)) return ESP_OK;
    platform_test_sound();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t page_json(httpd_req_t *req)
{
    settings_t s;
    settings_get(&s);
    if (!authorised(req, &s)) return ESP_OK;
    char *j = settings_to_json(&s, false);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, j ? j : "{}");
    free(j);
    return e;
}

// ---- updates, version, crash ----

static esp_err_t json_reply(httpd_req_t *req, const char *status, const char *json)
{
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t ota_refuse(httpd_req_t *req, const char *status, const char *why)
{
    char b[160];
    snprintf(b, sizeof b, "{\"ok\":false,\"error\":\"%s\"}", why);
    ESP_LOGW(TAG, "update refused: %s", why);
    return json_reply(req, status, b);
}

static void restart_cb(void *arg);

// POST /api/ota: the raw app image as the body, written straight into the
// other app slot as it arrives.
static esp_err_t api_ota(httpd_req_t *req)
{
    settings_t *s = malloc(sizeof *s);
    if (!s) return ESP_FAIL;
    settings_get(s);
    bool ap = net_mode() == NET_AP, nopass = !s->admin_pass[0];
    bool ok = !ap && !nopass && authorised(req, s);
    free(s);
    if (ap) return ota_refuse(req, "403 Forbidden", "updates are not taken on the set-up access point");
    if (nopass) return ota_refuse(req, "403 Forbidden", "set an admin password first");
    if (!ok) return ESP_OK;  // authorised() has replied 401
    if (g_ota_active) return ota_refuse(req, "409 Conflict", "an update is already in progress");
    size_t total = req->content_len;
    if (total < 1024) return ota_refuse(req, "400 Bad Request", "no image in the request body");

    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) return ota_refuse(req, "500 Internal Server Error", "out of memory");
    size_t got = 0;
    const char *err = NULL;
    bool begun = false;
    int timeouts = 0;
    while (got < total && !err) {
        size_t want = total - got < 4096 ? total - got : 4096;
        size_t have = 0;
        // The first chunk must hold the image header and app description.
        while (have < want) {
            int r = httpd_req_recv(req, (char *)buf + have, want - have);
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
            if (r <= 0) {
                err = "upload interrupted";
                break;
            }
            have += r;
        }
        if (err) break;
        err = begun ? ota_write(buf, have) : ota_begin(buf, have, total);
        begun = true;
        got += have;
    }
    free(buf);
    if (err) {
        ota_abort();
        if (!strcmp(err, "an update is already in progress")) return ota_refuse(req, "409 Conflict", err);
        return ota_refuse(req, "400 Bad Request", err);
    }
    char ver[40] = "";
    err = ota_finish(ver, sizeof ver);
    if (err) return ota_refuse(req, "400 Bad Request", err);
    char b[120];
    snprintf(b, sizeof b, "{\"ok\":true,\"version\":\"%s\"}", ver);
    json_reply(req, NULL, b);
    // Reply first, then restart into the new image.
    const esp_timer_create_args_t t = {.callback = restart_cb, .name = "ota_restart"};
    esp_timer_handle_t h;
    if (esp_timer_create(&t, &h) == ESP_OK) esp_timer_start_once(h, 1000 * 1000);
    return ESP_OK;
}

// GET /api/version, no auth.
static esp_err_t api_version(httpd_req_t *req)
{
    char b[200];
    snprintf(b, sizeof b, "{\"version\":\"%s\",\"partition\":\"%s\",\"idf\":\"%s\"}",
             esp_app_get_description()->version, ota_running_slot(), esp_app_get_description()->idf_ver);
    return json_reply(req, NULL, b);
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "USB";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
    }
}

// GET /api/crash: the reset reason and the last crash kept in flash.
// DELETE /api/crash clears it.
static esp_err_t api_crash(httpd_req_t *req)
{
    settings_t *s = malloc(sizeof *s);
    if (!s) return ESP_FAIL;
    settings_get(s);
    bool ok = net_mode() != NET_AP && authorised(req, s);
    free(s);
    if (net_mode() == NET_AP) return json_reply(req, "403 Forbidden", "{\"error\":\"not on the set-up access point\"}");
    if (!ok) return ESP_OK;
    if (req->method == HTTP_DELETE) {
        esp_err_t e = esp_core_dump_image_erase();
        return json_reply(req, NULL, e == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
    }
    sbuf_t b = {0};
    esp_reset_reason_t rr = esp_reset_reason();
    sb_printf(&b, "{\"reset_reason\":\"%s\",\"reset_code\":%d,\"version\":\"%s\",\"coredump\":", reset_name(rr),
              (int)rr, esp_app_get_description()->version);
    esp_core_dump_summary_t *sum = malloc(sizeof *sum);
    if (sum && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(sum) == ESP_OK) {
        sb_printf(&b, "{\"task\":\"%s\",\"pc\":\"0x%08lx\",\"backtrace\":[", sum->exc_task, (unsigned long)sum->exc_pc);
        for (uint32_t i = 0; i < sum->exc_bt_info.depth; i++)
            sb_printf(&b, "%s\"0x%08lx\"", i ? "," : "", (unsigned long)sum->exc_bt_info.bt[i]);
        sb_printf(&b, "],\"backtrace_corrupted\":%s,\"app_sha\":\"%.16s\"}", sum->exc_bt_info.corrupted ? "true" : "false",
                  (const char *)sum->app_elf_sha256);
    } else {
        sb_printf(&b, "null");
    }
    free(sum);
    sb_printf(&b, "}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(req, b.p ? b.p : "{}", b.n);
    free(b.p);
    return e;
}

// Anything else: on the set-up AP, send phones to the set-up page.
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (net_mode() == NET_AP) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" SETUP_AP_IP "/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
}

void web_start(void)
{
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 12288;
    c.max_uri_handlers = 12;
    c.core_id = 0;
    c.lru_purge_enable = true;
    c.max_open_sockets = 4;  // plus 3 the server keeps for itself; see CONFIG_LWIP_MAX_SOCKETS
    httpd_handle_t h;
    if (httpd_start(&h, &c) != ESP_OK) {
        ESP_LOGE(TAG, "http server failed to start");
        return;
    }
    httpd_uri_t u[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_root},
        {.uri = "/save", .method = HTTP_POST, .handler = page_save},
        {.uri = "/test", .method = HTTP_POST, .handler = page_test},
        {.uri = "/settings.json", .method = HTTP_GET, .handler = page_json},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = api_ota},
        {.uri = "/api/version", .method = HTTP_GET, .handler = api_version},
        {.uri = "/api/crash", .method = HTTP_GET, .handler = api_crash},
        {.uri = "/api/crash", .method = HTTP_DELETE, .handler = api_crash},
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(h, &u[i]);
    httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "settings page up on port 80");
    ota_note_web_up();
}
