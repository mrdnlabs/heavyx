/**
 * steerx — on-camera PTZ follow controller (step mode).
 *
 * Loop at 1 Hz:
 *   1. GET DetectX's local status endpoint -> latest detections
 *   2. pick the highest-priority / largest target
 *   3. command VAPIX `center` on a point a fraction (APPROACH_FRAC, default
 *      1/3) of the way from frame center to the target => closes that fraction
 *      of the angular error per tick (exponential convergence, no overshoot;
 *      camera does all pixel->angle math)
 *   4. when roughly centered, nudge zoom so the target is ~half frame height
 *      (hysteresis band avoids zoom hunting)
 *   5. on target loss: hold, then one zoom-out step to reacquire
 *
 * VAPIX auth on 127.0.0.12 (service-account virtual host) comes from the D-Bus VAPIX service-account API.
 * Design notes: docs/STEERING.md in the project repo.
 */
#include <curl/curl.h>
#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* ---- tuning (values validated off-camera on the loaner Q6355) ---- */
#define PERIOD_S        1
#define APPROACH_FRAC   0.33    /* fraction of error closed per step */
#define DEADBAND        0.07    /* quiet zone: no move when ~centered */
#define MIN_CONF        40      /* DetectX confidence is 0-100 */
#define ZOOM_ENABLED    1
#define ZOOM_TARGET_H   0.50
#define ZOOM_BAND_LO    0.35
#define ZOOM_BAND_HI    0.65
#define ZOOM_STEP_MAX   1.6
#define ZOOM_STEP_MIN   0.7
#define LOSS_ZOOMOUT_S  6
#define STICKY_S        5

/* Follow priority: first match wins when several targets are visible. Strings
 * MUST match the deployed model's labels.txt exactly (Stage 3 12-class
 * machinery taxonomy). Order = active earthmovers first, static/support plant
 * next, and person LAST so the PTZ prefers equipment over workers (drop
 * "person" from this list to ignore workers entirely). */
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

static char g_auth[256];         /* "user:pass" for 127.0.0.12 VAPIX */
static int g_vid_w = 856, g_vid_h = 640;

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

/* HTTP GET on localhost with the service-account credentials */
static char *http_get(const char *url) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    membuf_t m = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERPWD, g_auth);
    /* service-account creds on the 127.0.0.12 virtual host are validated with
     * BASIC (per the official Axis vapix example) — DIGEST is rejected (401) */
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

/* fetch "user:pass" via com.axis.HTTPConf1.VAPIXServiceAccounts1 */
static int fetch_credentials(void) {
    GError *err = NULL;
    GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
    if (!conn) {
        syslog(LOG_ERR, "dbus connect failed: %s", err ? err->message : "?");
        return -1;
    }
    GVariant *res = g_dbus_connection_call_sync(
        conn, "com.axis.HTTPConf1", "/com/axis/HTTPConf1/VAPIXServiceAccounts1",
        "com.axis.HTTPConf1.VAPIXServiceAccounts1", "GetCredentials",
        g_variant_new("(s)", "default"), NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &err);
    if (!res) {
        syslog(LOG_ERR, "GetCredentials failed: %s", err ? err->message : "?");
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

/* pull an integer field like "videoWidth":856 out of a JSON blob */
static int json_int(const char *json, const char *key, int fallback) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    return p ? atoi(p + strlen(pat)) : fallback;
}

/* parse DetectX status JSON detections into dets[]; returns count */
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

static void vapix_ptz(const char *params) {
    char url[512];
    snprintf(url, sizeof(url),
             "http://127.0.0.12/axis-cgi/com/ptz.cgi?%s&camera=1", params);
    char *r = http_get(url);
    free(r);
}

int main(void) {
    openlog("steerx", LOG_PID, LOG_USER);
    syslog(LOG_INFO, "steerx starting");
    curl_global_init(CURL_GLOBAL_DEFAULT);

    while (fetch_credentials() != 0) sleep(5);

    /* probe: can the service account read a plain VAPIX CGI vs DetectX's
     * cross-app admin endpoint? disambiguates auth-scope from auth-scheme */
    char *probe = http_get("http://127.0.0.12/axis-cgi/param.cgi"
                           "?action=list&group=Brand.ProdNbr");
    syslog(LOG_INFO, "probe /axis-cgi/param.cgi: %s",
           probe ? "OK (200)" : "FAILED");
    free(probe);

    /* learn DetectX's video geometry (retry until DetectX is up) */
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

    for (;;) {
        sleep(PERIOD_S);
        char *st = http_get("http://127.0.0.12/local/detectx/status");
        if (!st) continue;
        det_t dets[32];
        int n = parse_detections(st, dets, 32);
        free(st);

        /* choose target: sticky class first, else best priority, largest box */
        det_t *best = NULL;
        int best_rank = 999;
        time_t now = time(NULL);
        int keep_sticky = sticky[0] && (now - sticky_since) < STICKY_S;
        for (int i = 0; i < n; i++) {
            if (dets[i].conf < MIN_CONF) continue;
            int r = priority_rank(dets[i].label);
            if (r < 0) continue;
            if (keep_sticky && strcmp(dets[i].label, sticky) != 0) continue;
            long area = (long)dets[i].w * dets[i].h;
            if (!best || r < best_rank ||
                (r == best_rank && area > (long)best->w * best->h)) {
                best = &dets[i];
                best_rank = r;
            }
        }
        if (keep_sticky && !best) {
            /* sticky class vanished this tick — fall through to any target */
            for (int i = 0; i < n; i++) {
                if (dets[i].conf < MIN_CONF) continue;
                int r = priority_rank(dets[i].label);
                if (r < 0) continue;
                if (!best || r < best_rank) { best = &dets[i]; best_rank = r; }
            }
        }

        if (!best) {
            if (last_seen && !zoomed_out &&
                now - last_seen > LOSS_ZOOMOUT_S && ZOOM_ENABLED) {
                syslog(LOG_INFO, "target lost %lds — zooming out",
                       (long)(now - last_seen));
                vapix_ptz("rzoom=-2000");
                zoomed_out = 1;
            }
            continue;
        }

        if (strcmp(best->label, sticky) != 0) {
            snprintf(sticky, sizeof(sticky), "%s", best->label);
            sticky_since = now;
            syslog(LOG_INFO, "tracking %s (%d%%)", best->label, best->conf);
        }
        last_seen = now;
        zoomed_out = 0;

        double cx = (best->x + best->w / 2.0) / g_vid_w;
        double cy = (best->y + best->h / 2.0) / g_vid_h;
        double ex = cx - 0.5, ey = cy - 0.5;
        double hfrac = (double)best->h / g_vid_h;

        if (hypot(ex, ey) > DEADBAND) {
            int px = (int)((0.5 + APPROACH_FRAC * ex) * g_vid_w);
            int py = (int)((0.5 + APPROACH_FRAC * ey) * g_vid_h);
            char params[128];
            snprintf(params, sizeof(params),
                     "center=%d,%d&imagewidth=%d&imageheight=%d",
                     px, py, g_vid_w, g_vid_h);
            vapix_ptz(params);
            syslog(LOG_INFO, "step: %s err=(%.2f,%.2f) -> center %d,%d",
                   best->label, ex, ey, px, py);
        } else if (ZOOM_ENABLED && (hfrac < ZOOM_BAND_LO || hfrac > ZOOM_BAND_HI)) {
            double want = ZOOM_TARGET_H / (hfrac > 0.001 ? hfrac : 0.001);
            double factor = sqrt(want);
            if (factor > ZOOM_STEP_MAX) factor = ZOOM_STEP_MAX;
            if (factor < ZOOM_STEP_MIN) factor = ZOOM_STEP_MIN;
            char params[128];
            snprintf(params, sizeof(params),
                     "areazoom=%d,%d,%d&imagewidth=%d&imageheight=%d",
                     (int)(cx * g_vid_w), (int)(cy * g_vid_h),
                     (int)(factor * 100), g_vid_w, g_vid_h);
            vapix_ptz(params);
            syslog(LOG_INFO, "zoom: hfrac=%.2f factor=%.2f", hfrac, factor);
        }
    }
    return 0;
}
