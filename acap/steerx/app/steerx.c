/**
 * steerx — on-camera PTZ follow controller (step mode). v0.4.0
 *
 * Loop at 1 Hz:
 *   1. GET DetectX's local status endpoint -> latest detections
 *   2. pick the highest-priority / largest target, EMA-smooth its box
 *   3. ZOOM POLICY (margin-based): keep the whole object inside the center
 *      MarginPercent (default 33%) of the frame on BOTH axes. If the raw box
 *      touches a frame edge, the object is cut off -> zoom out immediately
 *      (a clipped box lies about the object's center — the "half a bulldozer"
 *      problem). Otherwise zoom in/out toward the margin with hysteresis.
 *   4. command VAPIX `center` a fraction (APPROACH_FRAC) of the way toward
 *      the smoothed target center (exponential convergence, no overshoot)
 *   5. burn live stats into the video via dynamic-text overlay #D<slot>
 *      (OverlaySlot param, 0=off; operator must add #D<slot> under
 *      Video > Overlays)
 *   6. append a JSONL line per tick to html/tracking.log (size-capped by
 *      LogMaxKB, one rotation to tracking.log.1) — downloadable at
 *      /local/steerx/tracking.log
 *
 * VAPIX on 127.0.0.12 (service-account virtual host) with HTTP Basic auth —
 * credentials from the D-Bus VAPIXServiceAccounts1 API. Runtime parameters
 * via axparameter (Apps page settings dialog / param.cgi group root.Steerx).
 * Design notes: docs/STEERING.md in the project repo.
 */
#include <axsdk/axparameter.h>
#include <curl/curl.h>
#include <gio/gio.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- fixed tuning (validated on loaner Q6355; adjustables via params) ---- */
#define PERIOD_S        1
#define APPROACH_FRAC   0.33    /* fraction of centering error closed per step */
#define DEADBAND        0.07    /* quiet zone: no move when ~centered */
#define MIN_CONF        40      /* DetectX confidence is 0-100 */
#define EMA_ALPHA       0.35    /* smoothing of box center/size */
#define STICKY_S        5
#define LOSS_ZOOMOUT_S  6
#define EDGE_PX         3       /* raw box within this many px of frame edge
                                   counts as clipped */
#define ZOOM_HI         1.15    /* act when maxdim > margin*ZOOM_HI  (too big) */
#define ZOOM_LO         0.55    /* act when maxdim < margin*ZOOM_LO (too small) */
#define ZOOM_STEP_MAX   1.6
#define ZOOM_STEP_MIN   0.6
#define EDGE_RZOOM      -2500   /* immediate zoom-out step on clipped box */
#define LOG_PATH        "html/tracking.log"
#define LOG_PATH_OLD    "html/tracking.log.1"

/* Follow priority: first match wins. Strings MUST match the deployed model's
 * labels.txt (Stage 3 12-class machinery taxonomy). person LAST so equipment
 * beats workers; drop it to ignore workers entirely. */
static const char *PRIORITY[] = {
    "excavator", "bulldozer", "wheel loader", "dump truck", "truck",
    "mobile crane", "tower crane", "pump truck", "concrete mixer",
    "pile driver", "roller", "person", NULL
};

typedef struct {
    char label[64];
    int conf, x, y, w, h;
} det_t;

typedef struct {
    char *buf;
    size_t len;
} membuf_t;

static char g_auth[256];          /* "user:pass" for 127.0.0.12 VAPIX */
static int g_vid_w = 856, g_vid_h = 640;
static AXParameter *g_params = NULL;

/* ------------------------------------------------------------------ http */
static size_t write_cb(void *data, size_t sz, size_t nm, void *userp) {
    membuf_t *m = userp;
    size_t n = sz * nm;
    char *p = realloc(m->buf, m->len + n + 1);
    if (!p) return 0;
    m->buf = p;
    memcpy(m->buf + m->len, data, n);
    m->len += n;
    m->buf[m->len] = 0;
    return n;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    membuf_t m = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERPWD, g_auth);
    /* service-account creds on 127.0.0.12 are validated with BASIC (per the
     * official Axis vapix example) — DIGEST is rejected (401) */
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 3L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || code != 200) {
        static int logged = 0;
        if (logged++ < 8)
            syslog(LOG_WARNING, "GET %s failed: curl=%s http=%ld",
                   url, curl_easy_strerror(rc), code);
        free(m.buf);
        return NULL;
    }
    return m.buf;
}

/* ------------------------------------------------------- credentials/dbus */
static int fetch_credentials(void) {
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!conn) {
        syslog(LOG_ERR, "dbus connect failed: %s", err ? err->message : "?");
        g_clear_error(&err);
        return -1;
    }
    GVariant *res = g_dbus_connection_call_sync(
        conn, "com.axis.HTTPConf1", "/com/axis/HTTPConf1/VAPIXServiceAccounts1",
        "com.axis.HTTPConf1.VAPIXServiceAccounts1", "GetCredentials",
        g_variant_new("(s)", "default"), NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    if (!res) {
        syslog(LOG_ERR, "GetCredentials failed: %s", err ? err->message : "?");
        g_clear_error(&err);
        g_object_unref(conn);
        return -1;
    }
    const char *cred = NULL;
    g_variant_get(res, "(&s)", &cred);   /* "username:password" */
    snprintf(g_auth, sizeof(g_auth), "%s", cred);
    const char *colon = strchr(cred, ':');
    syslog(LOG_INFO, "got service-account credentials (user='%.*s')",
           colon ? (int)(colon - cred) : 0, cred);
    g_variant_unref(res);
    g_object_unref(conn);
    return 0;
}

/* ------------------------------------------------------------ parameters */
static int param_int(const char *name, int fallback) {
    if (!g_params) return fallback;
    gchar *value = NULL;
    GError *err = NULL;
    /* read live each call — settings dialog changes apply without restart */
    if (!ax_parameter_get(g_params, name, &value, &err)) {
        g_clear_error(&err);
        return fallback;
    }
    int v = atoi(value);
    g_free(value);
    return v;
}

/* ------------------------------------------------------------------ json */
static int json_int(const char *json, const char *key, int fallback) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    return p ? atoi(p + strlen(pat)) : fallback;
}

static int parse_detections(const char *json, det_t *dets, int max) {
    const char *arr = strstr(json, "\"detections\":[");
    if (!arr) return 0;
    const char *end = strchr(arr, ']');
    if (!end) return 0;
    int n = 0;
    const char *p = arr;
    while (n < max && (p = strstr(p, "\"label\":\"")) && p < end) {
        det_t *d = &dets[n];
        p += 9;
        size_t i = 0;
        while (*p && *p != '"' && i < sizeof(d->label) - 1) d->label[i++] = *p++;
        d->label[i] = 0;
        const char *obj_end = strchr(p, '}');
        if (!obj_end || obj_end > end) obj_end = end;
        char seg[512];
        size_t seglen = (size_t)(obj_end - p) < sizeof(seg) - 1
                        ? (size_t)(obj_end - p) : sizeof(seg) - 1;
        memcpy(seg, p, seglen);
        seg[seglen] = 0;
        d->conf = json_int(seg, "c", 0);
        d->x = json_int(seg, "x", 0);
        d->y = json_int(seg, "y", 0);
        d->w = json_int(seg, "w", 0);
        d->h = json_int(seg, "h", 0);
        n++;
    }
    return n;
}

static int priority_rank(const char *label) {
    for (int i = 0; PRIORITY[i]; i++)
        if (strcmp(PRIORITY[i], label) == 0) return i;
    return -1;
}

/* ------------------------------------------------------------------ vapix */
static void vapix_ptz(const char *params) {
    char url[512];
    snprintf(url, sizeof(url),
             "http://127.0.0.12/axis-cgi/com/ptz.cgi?%s&camera=1", params);
    char *r = http_get(url);
    free(r);
}

/* minimal URL-encode for overlay text (alnum kept, rest %XX) */
static void url_encode(const char *in, char *out, size_t outsz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '_') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

/* burn stats into the video via dynamic text slot #D<slot> (0 = disabled).
 * The operator must add the #D<slot> modifier under Video > Overlays. */
static void overlay_stats(const char *text) {
    static char last[160];
    int slot = param_int("OverlaySlot", 0);
    if (slot < 1 || slot > 16) return;
    if (strncmp(text, last, sizeof(last) - 1) == 0) return;
    snprintf(last, sizeof(last), "%s", text);
    char enc[480], url[640];
    url_encode(text, enc, sizeof(enc));
    snprintf(url, sizeof(url),
             "http://127.0.0.12/axis-cgi/dynamicoverlay.cgi"
             "?action=settext&text_index=%d&text=%s", slot, enc);
    char *r = http_get(url);
    free(r);
}

/* --------------------------------------------------------------- logging */
/* JSONL tick log, size-capped, one rotation. Served at
 * /local/steerx/tracking.log because it lives in html/. */
static void log_tick(const char *fmt, ...) {
    int max_kb = param_int("LogMaxKB", 1024);
    if (max_kb <= 0) return;
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > (off_t)max_kb * 1024) {
        rename(LOG_PATH, LOG_PATH_OLD);   /* overwrites previous rotation */
    }
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    fprintf(f, "{\"t\":%ld,", (long)time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputs("}\n", f);
    fclose(f);
}

/* ------------------------------------------------------------------ main */
int main(void) {
    openlog("steerx", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "steerx 0.4.0 starting");
    curl_global_init(CURL_GLOBAL_DEFAULT);

    GError *perr = NULL;
    g_params = ax_parameter_new("steerx", &perr);
    if (!g_params) {
        syslog(LOG_WARNING, "axparameter unavailable (%s) — using defaults",
               perr ? perr->message : "?");
        g_clear_error(&perr);
    }

    while (fetch_credentials() != 0) sleep(5);

    char *probe = http_get("http://127.0.0.12/axis-cgi/param.cgi"
                           "?action=list&group=Brand.ProdNbr");
    syslog(LOG_INFO, "probe /axis-cgi/param.cgi: %s",
           probe ? "OK (200)" : "FAILED");
    free(probe);

    for (;;) {
        char *app = http_get("http://127.0.0.12/local/detectx/app");
        if (app) {
            g_vid_w = json_int(app, "videoWidth", 856);
            g_vid_h = json_int(app, "videoHeight", 640);
            free(app);
            break;
        }
        syslog(LOG_INFO, "waiting for DetectX...");
        sleep(5);
    }
    syslog(LOG_INFO, "video geometry %dx%d", g_vid_w, g_vid_h);

    char sticky[64] = "";
    time_t sticky_since = 0, last_seen = 0;
    int zoomed_out = 0;
    double ema_cx = 0, ema_cy = 0, ema_w = 0, ema_h = 0;
    int ema_init = 0;

    for (;;) {
        sleep(PERIOD_S);
        char *st = http_get("http://127.0.0.12/local/detectx/status");
        if (!st) {
            log_tick("\"event\":\"detectx_unreachable\"");
            continue;
        }
        det_t dets[32];
        int n = parse_detections(st, dets, 32);
        free(st);

        /* -------- target selection: sticky class, then priority, largest */
        det_t *best = NULL;
        int best_rank = 999;
        time_t now = time(NULL);
        int keep_sticky = sticky[0] && (now - sticky_since) < STICKY_S;
        for (int pass = 0; pass < 2 && !best; pass++) {
            for (int i = 0; i < n; i++) {
                if (dets[i].conf < MIN_CONF) continue;
                int r = priority_rank(dets[i].label);
                if (r < 0) continue;
                if (pass == 0 && keep_sticky &&
                    strcmp(dets[i].label, sticky) != 0) continue;
                long area = (long)dets[i].w * dets[i].h;
                if (!best || r < best_rank ||
                    (r == best_rank && area > (long)best->w * best->h)) {
                    best = &dets[i];
                    best_rank = r;
                }
            }
            if (pass == 0 && !keep_sticky) break;
        }

        if (!best) {
            if (last_seen && !zoomed_out && now - last_seen > LOSS_ZOOMOUT_S) {
                syslog(LOG_INFO, "target lost %lds — zooming out",
                       (long)(now - last_seen));
                vapix_ptz("rzoom=" G_STRINGIFY(EDGE_RZOOM));
                zoomed_out = 1;
                log_tick("\"event\":\"lost_zoomout\",\"dets\":%d", n);
            } else {
                log_tick("\"event\":\"no_target\",\"dets\":%d", n);
            }
            overlay_stats("SteerX: searching");
            ema_init = 0;
            continue;
        }

        if (strcmp(best->label, sticky) != 0) {
            snprintf(sticky, sizeof(sticky), "%s", best->label);
            sticky_since = now;
            ema_init = 0;
            syslog(LOG_INFO, "tracking %s (%d%%)", best->label, best->conf);
        }
        last_seen = now;
        zoomed_out = 0;

        /* -------- EMA smoothing of the box (center AND size) */
        double cx = (best->x + best->w / 2.0) / g_vid_w;
        double cy = (best->y + best->h / 2.0) / g_vid_h;
        double bw = (double)best->w / g_vid_w;
        double bh = (double)best->h / g_vid_h;
        if (!ema_init) {
            ema_cx = cx; ema_cy = cy; ema_w = bw; ema_h = bh;
            ema_init = 1;
        } else {
            ema_cx = EMA_ALPHA * cx + (1 - EMA_ALPHA) * ema_cx;
            ema_cy = EMA_ALPHA * cy + (1 - EMA_ALPHA) * ema_cy;
            ema_w  = EMA_ALPHA * bw + (1 - EMA_ALPHA) * ema_w;
            ema_h  = EMA_ALPHA * bh + (1 - EMA_ALPHA) * ema_h;
        }

        double margin = param_int("MarginPercent", 33) / 100.0;
        if (margin < 0.10) margin = 0.10;
        if (margin > 0.90) margin = 0.90;

        /* -------- edge-touch: RAW box clipped by frame => object cut off.
         * The box center is a lie (half a bulldozer) — zoom out first. */
        int edge = best->x <= EDGE_PX || best->y <= EDGE_PX ||
                   best->x + best->w >= g_vid_w - EDGE_PX ||
                   best->y + best->h >= g_vid_h - EDGE_PX;

        double ex = ema_cx - 0.5, ey = ema_cy - 0.5;
        double maxdim = ema_w > ema_h ? ema_w : ema_h;
        char cmd[64] = "none";

        if (edge) {
            vapix_ptz("rzoom=" G_STRINGIFY(EDGE_RZOOM));
            snprintf(cmd, sizeof(cmd), "rzoom %d (edge)", EDGE_RZOOM);
        } else if (hypot(ex, ey) > DEADBAND) {
            int px = (int)((0.5 + APPROACH_FRAC * ex) * g_vid_w);
            int py = (int)((0.5 + APPROACH_FRAC * ey) * g_vid_h);
            char params[128];
            snprintf(params, sizeof(params),
                     "center=%d,%d&imagewidth=%d&imageheight=%d",
                     px, py, g_vid_w, g_vid_h);
            vapix_ptz(params);
            snprintf(cmd, sizeof(cmd), "center %d,%d", px, py);
        } else if (maxdim > margin * ZOOM_HI || maxdim < margin * ZOOM_LO) {
            /* zoom toward the margin, half the error in log-zoom space */
            double factor = sqrt(margin / (maxdim > 0.001 ? maxdim : 0.001));
            if (factor > ZOOM_STEP_MAX) factor = ZOOM_STEP_MAX;
            if (factor < ZOOM_STEP_MIN) factor = ZOOM_STEP_MIN;
            char params[128];
            snprintf(params, sizeof(params),
                     "areazoom=%d,%d,%d&imagewidth=%d&imageheight=%d",
                     (int)(ema_cx * g_vid_w), (int)(ema_cy * g_vid_h),
                     (int)(factor * 100), g_vid_w, g_vid_h);
            vapix_ptz(params);
            snprintf(cmd, sizeof(cmd), "areazoom %.2f", factor);
        }

        char stats[160];
        snprintf(stats, sizeof(stats), "SteerX: %s %d%% size %.0f%%%s",
                 best->label, best->conf, maxdim * 100, edge ? " CLIPPED" : "");
        overlay_stats(stats);

        log_tick("\"target\":\"%s\",\"conf\":%d,\"dets\":%d,"
                 "\"raw\":[%d,%d,%d,%d],\"ema\":[%.3f,%.3f,%.3f,%.3f],"
                 "\"err\":[%.3f,%.3f],\"maxdim\":%.3f,\"margin\":%.2f,"
                 "\"edge\":%d,\"cmd\":\"%s\"",
                 best->label, best->conf, n,
                 best->x, best->y, best->w, best->h,
                 ema_cx, ema_cy, ema_w, ema_h,
                 ex, ey, maxdim, margin, edge, cmd);
    }
    return 0;
}
