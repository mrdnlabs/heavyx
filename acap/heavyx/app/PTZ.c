/**
 * PTZ.c — see PTZ.h. Control law ported from steerx v0.4.0 (live-validated on
 * a Q6355 loaner; constants deliberately identical — do not retune casually):
 *
 *   per tick (1 Hz), at most ONE command, precedence:
 *     1. edge-touch  : RAW box within EDGE_PX of a video-frame edge means the
 *                      object is clipped and its box center is a lie ("half a
 *                      bulldozer") -> immediate rzoom -2500
 *     2. centering   : EMA error beyond deadband -> VAPIX `center` at
 *                      0.5 + APPROACH_FRAC * err (camera does pixel->angle)
 *     3. margin zoom : keep max(box w,h) inside the center marginPercent of
 *                      the frame, hysteresis band, sqrt step in log-zoom
 *   loss for lossZoomoutSeconds -> single rzoom -2500.
 *
 * Fork changes vs steerx: detections arrive in-process (PTZ_Feed) in MODEL
 * pixel space and are mapped to VIDEO space via MAP.h (letterbox-correct);
 * priority comes from the live settings array ptz.priority; VAPIX goes
 * through ACAP_VAPIX_Get; stats go to the burned-in overlay footer.
 */
#include <glib.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>

#include "ACAP.h"
#include "MAP.h"
#include "OVERLAY.h"
#include "PTZ.h"

#define MAX_DETS        32
#define EMA_ALPHA_DEFAULT 0.35   /* overridable via settings ptz.emaAlpha */
#define EDGE_RZOOM      "-2500"
#define ZOOM_HI         1.15
#define ZOOM_LO         0.55
#define ZOOM_STEP_MAX   1.6
#define ZOOM_STEP_MIN   0.6
#define SNAPSHOT_MAX_AGE 2.0    /* s — stale detections count as none */
/* localdata/ is the app-writable dir (html/ is root-owned package content);
 * served via the "tracking" FastCGI endpoint, not as a static file */
#define LOG_PATH        "localdata/tracking.log"
#define LOG_PATH_OLD    "localdata/tracking.log.1"

typedef struct {
    char label[64];
    int conf;
    double x, y, w, h;        /* MODEL pixel space (raw) */
} det_t;

static det_t latest[MAX_DETS];
static int latest_n = 0;
static gint64 latest_ts = 0;
static int feed_model_w = 640, feed_model_h = 640;

static int ptz_supported = 0;
static guint tick_source = 0;
static cJSON *g_settings = NULL;  /* main.c's settings object (not owned) */

static char sticky[64] = "";
static time_t sticky_since = 0, last_seen = 0;
static int zoomed_out = 0, tracking_state = 0;
static double ema_cx, ema_cy, ema_w, ema_h;
static int ema_init = 0;
static long tick_count = 0;

/* ---------------------------------------------------------------- helpers */
static cJSON *ptz_cfg(void) {
    return g_settings ? cJSON_GetObjectItem(g_settings, "ptz") : NULL;
}

static double cfg_num(const char *name, double fallback) {
    cJSON *p = ptz_cfg();
    cJSON *j = p ? cJSON_GetObjectItem(p, name) : NULL;
    return j && cJSON_IsNumber(j) ? j->valuedouble : fallback;
}

static int cfg_bool(const char *name, int fallback) {
    cJSON *p = ptz_cfg();
    cJSON *j = p ? cJSON_GetObjectItem(p, name) : NULL;
    return j ? cJSON_IsTrue(j) : fallback;
}

/* priority rank from the live settings array: index = precedence,
 * absence = never follow. Returns -1 when not followable. */
static int priority_rank(const char *label) {
    cJSON *p = ptz_cfg();
    cJSON *arr = p ? cJSON_GetObjectItem(p, "priority") : NULL;
    if (!arr || !cJSON_IsArray(arr)) return -1;
    int i = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item) && item->valuestring &&
            strcmp(item->valuestring, label) == 0)
            return i;
        i++;
    }
    return -1;
}

static void vapix_ptz(const char *params) {
    char url[256];
    snprintf(url, sizeof(url), "com/ptz.cgi?%s&camera=1", params);
    char *r = ACAP_VAPIX_Get(url);
    free(r);
}

static void log_tick(const char *fmt, ...) {
    int max_kb = (int)cfg_num("logMaxKB", 1024);
    if (max_kb <= 0) return;
    char path[512], old[512];
    snprintf(path, sizeof(path), "%s%s", ACAP_FILE_AppPath(), LOG_PATH);
    snprintf(old, sizeof(old), "%s%s", ACAP_FILE_AppPath(), LOG_PATH_OLD);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > (off_t)max_kb * 1024)
        rename(path, old);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "{\"t\":%ld,", (long)time(NULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputs("}\n", f);
    fclose(f);
}

/* GET /local/heavyx/tracking[?old=1] — stream the JSONL tick log.
 * Read-only (GET side-effect-free per the ACAP standards). */
static void HTTP_ENDPOINT_tracking(ACAP_HTTP_Response response,
                                   const ACAP_HTTP_Request request) {
    const char *method = ACAP_HTTP_Get_Method(request);
    if (method && strcmp(method, "GET") != 0) {
        ACAP_HTTP_Respond_Error(response, 405, "GET only");
        return;
    }
    const char *old = ACAP_HTTP_Request_Param(request, "old");
    char path[512];
    snprintf(path, sizeof(path), "%s%s", ACAP_FILE_AppPath(),
             (old && old[0] == '1') ? LOG_PATH_OLD : LOG_PATH);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ACAP_HTTP_Respond_Error(response, 404, "no tracking log yet");
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); ACAP_HTTP_Respond_Text(response, ""); return; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); ACAP_HTTP_Respond_Error(response, 500, "oom"); return; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    ACAP_HTTP_Respond_Text(response, buf);
    free(buf);
}

static void set_tracking(int on, const char *label) {
    if (on != tracking_state) {
        tracking_state = on;
        ACAP_EVENTS_Fire_State("tracking", on);
    }
    ACAP_STATUS_SetString("ptz", "state",
                          !ptz_supported ? "unsupported" :
                          !cfg_bool("enabled", 0) ? "disabled" :
                          on ? "tracking" : "searching");
    ACAP_STATUS_SetString("ptz", "target", on && label ? label : "");
}

/* ------------------------------------------------------------------ tick */
static gboolean ptz_tick(gpointer data) {
    (void)data;
    tick_count++;
    ACAP_STATUS_SetNumber("ptz", "ticks", tick_count);

    /* keep the published priority list in sync with live settings (the UI
     * edits it at runtime; a stale init-time snapshot misleads monitoring) */
    cJSON *pcfg = ptz_cfg();
    cJSON *prio_now = pcfg ? cJSON_GetObjectItem(pcfg, "priority") : NULL;
    ACAP_STATUS_SetObject("ptz", "priority",
                          prio_now ? cJSON_Duplicate(prio_now, 1)
                                   : cJSON_CreateArray());

    if (!ptz_supported || !cfg_bool("enabled", 0)) {
        set_tracking(0, NULL);
        return G_SOURCE_CONTINUE;
    }

    /* snapshot freshness */
    double age = (g_get_monotonic_time() - latest_ts) / 1e6;
    int n = age <= SNAPSHOT_MAX_AGE ? latest_n : 0;

    int min_conf = (int)cfg_num("minConfidence", 40);
    time_t now = time(NULL);
    double sticky_s = cfg_num("stickySeconds", 5);

    /* selection: sticky pass, then priority rank, tie-break largest area */
    det_t *best = NULL;
    int best_rank = 9999;
    int keep_sticky = sticky[0] && (now - sticky_since) < sticky_s;
    for (int pass = 0; pass < 2 && !best; pass++) {
        for (int i = 0; i < n; i++) {
            if (latest[i].conf < min_conf) continue;
            int r = priority_rank(latest[i].label);
            if (r < 0) continue;
            if (pass == 0 && keep_sticky &&
                strcmp(latest[i].label, sticky) != 0) continue;
            double area = latest[i].w * latest[i].h;
            if (!best || r < best_rank ||
                (r == best_rank && area > best->w * best->h)) {
                best = &latest[i];
                best_rank = r;
            }
        }
        if (pass == 0 && !keep_sticky) break;
    }

    if (!best) {
        double loss_s = cfg_num("lossZoomoutSeconds", 6);
        if (last_seen && !zoomed_out && now - last_seen > loss_s) {
            syslog(LOG_INFO, "ptz: target lost %lds — zooming out",
                   (long)(now - last_seen));
            vapix_ptz("rzoom=" EDGE_RZOOM);
            zoomed_out = 1;
            log_tick("\"event\":\"lost_zoomout\",\"dets\":%d", n);
        } else {
            log_tick("\"event\":\"no_target\",\"dets\":%d", n);
        }
        set_tracking(0, NULL);
        OVERLAY_SetStats("HeavyX: searching");
        ema_init = 0;
        return G_SOURCE_CONTINUE;
    }

    if (strcmp(best->label, sticky) != 0) {
        snprintf(sticky, sizeof(sticky), "%s", best->label);
        sticky_since = now;
        ema_init = 0;
        syslog(LOG_INFO, "ptz: tracking %s (%d%%)", best->label, best->conf);
    }
    last_seen = now;
    zoomed_out = 0;
    set_tracking(1, best->label);

    /* map RAW model-space box to video space */
    cJSON *modelCfg = ACAP_Get_Config("model");
    int vw = feed_model_w, vh = feed_model_h;
    const char *smode = "stretch";
    if (modelCfg) {
        cJSON *j;
        if ((j = cJSON_GetObjectItem(modelCfg, "videoWidth"))) vw = j->valueint;
        if ((j = cJSON_GetObjectItem(modelCfg, "videoHeight"))) vh = j->valueint;
        if ((j = cJSON_GetObjectItem(modelCfg, "scaleModeName")) && j->valuestring)
            smode = j->valuestring;
    }
    MAP_Transform map = MAP_Make(feed_model_w, feed_model_h, vw, vh, smode);
    double bx = MAP_X(&map, best->x), by = MAP_Y(&map, best->y);
    double bw = MAP_W(&map, best->w), bh = MAP_H(&map, best->h);

    /* EMA on normalized (video-space) center + size */
    double cx = (bx + bw / 2.0) / vw, cy = (by + bh / 2.0) / vh;
    double nw = bw / vw, nh = bh / vh;
    double alpha = cfg_num("emaAlpha", EMA_ALPHA_DEFAULT);
    if (alpha < 0.05) alpha = 0.05;
    if (alpha > 1.0) alpha = 1.0;
    if (!ema_init) {
        ema_cx = cx; ema_cy = cy; ema_w = nw; ema_h = nh;
        ema_init = 1;
    } else {
        ema_cx = alpha * cx + (1 - alpha) * ema_cx;
        ema_cy = alpha * cy + (1 - alpha) * ema_cy;
        ema_w  = alpha * nw + (1 - alpha) * ema_w;
        ema_h  = alpha * nh + (1 - alpha) * ema_h;
    }

    double margin = cfg_num("marginPercent", 33) / 100.0;
    if (margin < 0.10) margin = 0.10;
    if (margin > 0.90) margin = 0.90;
    double deadband = cfg_num("deadband", 0.07);
    double approach = cfg_num("approach", 0.33);

    double edge_px = 3 > 0.005 * vw ? 3 : 0.005 * vw;
    int edge = bx <= edge_px || by <= edge_px ||
               bx + bw >= vw - edge_px || by + bh >= vh - edge_px;

    double ex = ema_cx - 0.5, ey = ema_cy - 0.5;
    double maxdim = ema_w > ema_h ? ema_w : ema_h;
    char cmd[64] = "none";

    if (edge) {
        vapix_ptz("rzoom=" EDGE_RZOOM);
        snprintf(cmd, sizeof(cmd), "rzoom %s (edge)", EDGE_RZOOM);
    } else if (hypot(ex, ey) > deadband) {
        int px = (int)((0.5 + approach * ex) * vw);
        int py = (int)((0.5 + approach * ey) * vh);
        char params[128];
        snprintf(params, sizeof(params),
                 "center=%d,%d&imagewidth=%d&imageheight=%d", px, py, vw, vh);
        vapix_ptz(params);
        snprintf(cmd, sizeof(cmd), "center %d,%d", px, py);
    } else if (maxdim > margin * ZOOM_HI || maxdim < margin * ZOOM_LO) {
        double factor = sqrt(margin / (maxdim > 0.001 ? maxdim : 0.001));
        if (factor > ZOOM_STEP_MAX) factor = ZOOM_STEP_MAX;
        if (factor < ZOOM_STEP_MIN) factor = ZOOM_STEP_MIN;
        char params[128];
        snprintf(params, sizeof(params),
                 "areazoom=%d,%d,%d&imagewidth=%d&imageheight=%d",
                 (int)(ema_cx * vw), (int)(ema_cy * vh),
                 (int)(factor * 100), vw, vh);
        vapix_ptz(params);
        snprintf(cmd, sizeof(cmd), "areazoom %.2f", factor);
    }

    char stats[192];
    snprintf(stats, sizeof(stats), "HeavyX: %s %d%% size %.0f%%%s",
             best->label, best->conf, maxdim * 100, edge ? " CLIPPED" : "");
    OVERLAY_SetStats(stats);
    ACAP_STATUS_SetNumber("ptz", "conf", best->conf);
    ACAP_STATUS_SetNumber("ptz", "sizePercent", maxdim * 100);
    ACAP_STATUS_SetString("ptz", "lastCmd", cmd);

    log_tick("\"target\":\"%s\",\"conf\":%d,\"dets\":%d,"
             "\"raw\":[%.0f,%.0f,%.0f,%.0f],\"ema\":[%.3f,%.3f,%.3f,%.3f],"
             "\"err\":[%.3f,%.3f],\"maxdim\":%.3f,\"margin\":%.2f,"
             "\"edge\":%d,\"cmd\":\"%s\"",
             best->label, best->conf, n, bx, by, bw, bh,
             ema_cx, ema_cy, ema_w, ema_h, ex, ey, maxdim, margin, edge, cmd);
    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ api */
void PTZ_Feed(cJSON *detections, int modelWidth, int modelHeight) {
    feed_model_w = modelWidth;
    feed_model_h = modelHeight;
    int n = 0;
    /* B4: on fixed cameras (or steering disabled) the tick never runs the
     * stats path — publish a detection summary here so the burned-in footer
     * is alive everywhere. OVERLAY_SetStats dedups, so per-frame is cheap. */
    if (!ptz_supported || !cfg_bool("enabled", 0)) {
        int cnt = detections ? cJSON_GetArraySize(detections) : 0;
        char line[96];
        if (cnt == 0) {
            line[0] = 0;   /* clean frame: no footer */
        } else {
            cJSON *first = cJSON_GetArrayItem(detections, 0);
            cJSON *lbl = first ? cJSON_GetObjectItem(first, "label") : NULL;
            snprintf(line, sizeof(line), "HeavyX: %d object%s%s%s", cnt,
                     cnt == 1 ? "" : "s",
                     lbl && lbl->valuestring ? " · " : "",
                     lbl && lbl->valuestring ? lbl->valuestring : "");
        }
        OVERLAY_SetStats(line);
    }
    cJSON *d = detections ? detections->child : NULL;
    for (; d && n < MAX_DETS; d = d->next) {
        cJSON *label = cJSON_GetObjectItem(d, "label");
        cJSON *c = cJSON_GetObjectItem(d, "c");
        cJSON *x = cJSON_GetObjectItem(d, "x");
        cJSON *y = cJSON_GetObjectItem(d, "y");
        cJSON *w = cJSON_GetObjectItem(d, "w");
        cJSON *h = cJSON_GetObjectItem(d, "h");
        if (!label || !label->valuestring || !x || !y || !w || !h) continue;
        det_t *o = &latest[n++];
        snprintf(o->label, sizeof(o->label), "%s", label->valuestring);
        o->conf = c ? c->valueint : 0;
        o->x = x->valuedouble;
        o->y = y->valuedouble;
        o->w = w->valuedouble;
        o->h = h->valuedouble;
    }
    latest_n = n;
    latest_ts = g_get_monotonic_time();
}

int PTZ_Init(cJSON *settings) {
    g_settings = settings;

    /* mechanical-PTZ probe, verified against real devices:
     *   P3408-VE fixed dome:  PTZ=yes, DigitalPTZ=yes  -> NOT supported
     *   Q6355-LE real PTZ:    PTZ=yes, DigitalPTZ=no   -> supported
     * DigitalPTZ=yes means the "PTZ" is digital crop emulation. */
    ptz_supported = 0;
    char *r = ACAP_VAPIX_Get("param.cgi?action=list&group=Properties.PTZ");
    if (r) {
        int has_ptz = strstr(r, ".PTZ.PTZ=yes") != NULL;
        int digital = strstr(r, ".DigitalPTZ=yes") != NULL;
        ptz_supported = has_ptz && !digital;
        free(r);
    }
    ACAP_STATUS_SetBool("ptz", "supported", ptz_supported);
    ACAP_STATUS_SetString("ptz", "state",
                          ptz_supported ? "disabled" : "unsupported");
    ACAP_STATUS_SetString("ptz", "target", "");
    ACAP_STATUS_SetString("ptz", "lastCmd", "none");

    /* expose the effective priority list */
    cJSON *p = ptz_cfg();
    cJSON *prio = p ? cJSON_GetObjectItem(p, "priority") : NULL;
    ACAP_STATUS_SetObject("ptz", "priority",
                          prio ? cJSON_Duplicate(prio, 1) : cJSON_CreateArray());

    ACAP_EVENTS_Add_Event("tracking", "HeavyX: Tracking", 1);
    ACAP_HTTP_Node("tracking", HTTP_ENDPOINT_tracking);

    tick_source = g_timeout_add_seconds(1, ptz_tick, NULL);
    syslog(LOG_INFO, "ptz init: supported=%d enabled=%d",
           ptz_supported, cfg_bool("enabled", 0));
    return 0;
}

void PTZ_Cleanup(void) {
    if (tick_source) {
        g_source_remove(tick_source);
        tick_source = 0;
    }
    if (tracking_state) ACAP_EVENTS_Fire_State("tracking", 0);
}
