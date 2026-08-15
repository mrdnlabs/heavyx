/**
 * OVERLAY.c — see OVERLAY.h. Follows the official axoverlay example:
 * Cairo backend, palette overlay for boxes + ARGB32 overlay for text,
 * adjustment callback for resolution/rotation, redraw failures non-fatal.
 *
 * Threading: axoverlay dispatches callbacks on the default GMainContext —
 * the same main loop that runs ImageProcess and the PTZ tick — so the
 * detection snapshot needs no lock. A one-shot runtime check logs if that
 * assumption is ever violated on some future firmware.
 */
#include <axoverlay.h>
#include <cairo/cairo.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "ACAP.h"
#include "MAP.h"
#include "OVERLAY.h"

#define MAX_DETS 32

typedef struct {
    char label[64];
    int conf;                 /* 0-100 */
    double x, y, w, h;        /* MODEL pixel space */
} ovl_det_t;

static ovl_det_t snapshot[MAX_DETS];
static int snapshot_n = 0;
static int model_w = 640, model_h = 640;
static char stats_line[192] = "";

static int overlay_enabled = 0;
static int labels_enabled = 1;
static int stats_enabled = 1;
static int boxes_id = -1;
static int text_id = -1;
static int overlay_ok = 0;
static GThread *main_thread = NULL;
static int thread_warned = 0;

/* palette indices (0 = transparent) */
enum { COL_NONE = 0, COL_MACHINE = 1, COL_TRUCK = 2, COL_CRANE = 3, COL_PERSON = 4 };

static int class_color(const char *label) {
    if (!strcmp(label, "person")) return COL_PERSON;
    if (!strcmp(label, "mobile crane") || !strcmp(label, "tower crane"))
        return COL_CRANE;
    if (!strcmp(label, "dump truck") || !strcmp(label, "truck") ||
        !strcmp(label, "pump truck") || !strcmp(label, "concrete mixer"))
        return COL_TRUCK;
    return COL_MACHINE;   /* excavator, bulldozer, wheel loader, roller, ... */
}

/* palette index -> cairo channel value (official example's index2cairo) */
static double index2cairo(int index) {
    return ((index << 4) + index) / 255.0;
}

static void set_palette(void) {
    struct axoverlay_palette_color c = {0};
    GError *err = NULL;
    /* index, r, g, b, alpha, pixelate */
    struct { int i; guint8 r, g, b, a; } colors[] = {
        { COL_NONE,    0,   0,   0,   0 },
        { COL_MACHINE, 255, 214, 0,  255 },   /* yellow */
        { COL_TRUCK,   255, 132, 0,  255 },   /* orange */
        { COL_CRANE,   0,   200, 215, 255 },  /* cyan */
        { COL_PERSON,  230, 60,  50, 255 },   /* red */
    };
    for (unsigned i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        c.red = colors[i].r;
        c.green = colors[i].g;
        c.blue = colors[i].b;
        c.alpha = colors[i].a;
        c.pixelate = FALSE;
        axoverlay_set_palette_color(colors[i].i, &c, &err);
        if (err) {
            syslog(LOG_WARNING, "overlay palette %d: %s", colors[i].i, err->message);
            g_clear_error(&err);
        }
    }
}

static void check_thread(void) {
    if (!thread_warned && main_thread && g_thread_self() != main_thread) {
        thread_warned = 1;
        syslog(LOG_WARNING,
               "overlay render callback on non-main thread — snapshot access "
               "is unlocked by design; report this firmware combination");
    }
}

static void adjustment_cb(gint id, struct axoverlay_stream_data *stream,
                          enum axoverlay_position_type *postype,
                          gfloat *overlay_x, gfloat *overlay_y,
                          gint *overlay_width, gint *overlay_height,
                          gpointer user_data) {
    (void)id; (void)postype; (void)overlay_x; (void)overlay_y; (void)user_data;
    *overlay_width = stream->width;
    *overlay_height = stream->height;
    if (stream->rotation == 90 || stream->rotation == 270) {
        *overlay_width = stream->height;
        *overlay_height = stream->width;
    }
}

static void render_cb(gpointer rendering_context, gint id,
                      struct axoverlay_stream_data *stream,
                      enum axoverlay_position_type postype,
                      gfloat overlay_x, gfloat overlay_y,
                      gint overlay_width, gint overlay_height,
                      gpointer user_data) {
    (void)stream; (void)postype; (void)overlay_x; (void)overlay_y;
    (void)user_data;
    check_thread();
    cairo_t *ctx = rendering_context;

    /* surface has the video aspect: map model→video, then video→surface */
    cJSON *modelCfg = ACAP_Get_Config("model");
    int vw = model_w, vh = model_h;
    const char *smode = "stretch";
    if (modelCfg) {
        cJSON *j;
        if ((j = cJSON_GetObjectItem(modelCfg, "videoWidth"))) vw = j->valueint;
        if ((j = cJSON_GetObjectItem(modelCfg, "videoHeight"))) vh = j->valueint;
        if ((j = cJSON_GetObjectItem(modelCfg, "scaleModeName")) && j->valuestring)
            smode = j->valuestring;
    }
    MAP_Transform map = MAP_Make(model_w, model_h, vw, vh, smode);
    double fx = (double)overlay_width / (vw > 0 ? vw : 1);
    double fy = (double)overlay_height / (vh > 0 ? vh : 1);

    if (id == boxes_id) {
        /* clear with transparent palette index */
        cairo_set_operator(ctx, CAIRO_OPERATOR_SOURCE);
        double t = index2cairo(COL_NONE);
        cairo_set_source_rgba(ctx, t, t, t, t);
        cairo_rectangle(ctx, 0, 0, overlay_width, overlay_height);
        cairo_fill(ctx);

        double lw = overlay_height / 270.0;
        if (lw < 2) lw = 2;
        cairo_set_line_width(ctx, lw);
        for (int i = 0; i < snapshot_n; i++) {
            ovl_det_t *d = &snapshot[i];
            double col = index2cairo(class_color(d->label));
            cairo_set_source_rgba(ctx, col, col, col, col);
            cairo_rectangle(ctx,
                            MAP_X(&map, d->x) * fx, MAP_Y(&map, d->y) * fy,
                            MAP_W(&map, d->w) * fx, MAP_H(&map, d->h) * fy);
            cairo_stroke(ctx);
        }
        return;
    }

    if (id == text_id) {
        cairo_set_operator(ctx, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(ctx, 0, 0, 0, 0);
        cairo_rectangle(ctx, 0, 0, overlay_width, overlay_height);
        cairo_fill(ctx);
        cairo_set_operator(ctx, CAIRO_OPERATOR_OVER);

        double fs = overlay_height / 30.0;
        if (fs < 14) fs = 14;
        cairo_select_font_face(ctx, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(ctx, fs);

        if (labels_enabled) {
            for (int i = 0; i < snapshot_n; i++) {
                ovl_det_t *d = &snapshot[i];
                char txt[96];
                snprintf(txt, sizeof(txt), "%s %d%%", d->label, d->conf);
                double tx = MAP_X(&map, d->x) * fx;
                double ty = MAP_Y(&map, d->y) * fy - fs * 0.35;
                if (ty < fs) ty = fs;
                /* shadow then text for legibility on any background */
                cairo_set_source_rgba(ctx, 0, 0, 0, 0.85);
                cairo_move_to(ctx, tx + 1.5, ty + 1.5);
                cairo_show_text(ctx, txt);
                cairo_set_source_rgba(ctx, 1, 1, 1, 1);
                cairo_move_to(ctx, tx, ty);
                cairo_show_text(ctx, txt);
            }
        }

        if (stats_enabled && stats_line[0]) {
            double ty = overlay_height - fs * 0.6;
            cairo_set_source_rgba(ctx, 0, 0, 0, 0.85);
            cairo_move_to(ctx, fs * 0.5 + 1.5, ty + 1.5);
            cairo_show_text(ctx, stats_line);
            cairo_set_source_rgba(ctx, 1, 1, 1, 1);
            cairo_move_to(ctx, fs * 0.5, ty);
            cairo_show_text(ctx, stats_line);
        }
    }
}

int OVERLAY_Init(cJSON *settings) {
    main_thread = g_thread_self();

    cJSON *ovl = settings ? cJSON_GetObjectItem(settings, "overlay") : NULL;
    overlay_enabled = 1;
    if (ovl) {
        cJSON *j;
        if ((j = cJSON_GetObjectItem(ovl, "enabled")))
            overlay_enabled = cJSON_IsTrue(j);
        if ((j = cJSON_GetObjectItem(ovl, "labels")))
            labels_enabled = cJSON_IsTrue(j);
        if ((j = cJSON_GetObjectItem(ovl, "stats")))
            stats_enabled = cJSON_IsTrue(j);
    }
    ACAP_STATUS_SetBool("overlay", "enabled", overlay_enabled);
    ACAP_STATUS_SetBool("overlay", "created", 0);
    if (!overlay_enabled) {
        syslog(LOG_INFO, "overlay disabled in settings — skipping creation");
        return 0;
    }

    /* fontconfig cache location must exist before axoverlay/cairo text */
    char cache[256];
    snprintf(cache, sizeof(cache), "%slocaldata", ACAP_FILE_AppPath());
    setenv("XDG_CACHE_HOME", cache, 1);

    if (!axoverlay_is_backend_supported(AXOVERLAY_CAIRO_IMAGE_BACKEND)) {
        syslog(LOG_WARNING, "axoverlay cairo backend unsupported — no overlay");
        return -1;
    }

    GError *err = NULL;
    struct axoverlay_settings ax = {0};
    axoverlay_init_axoverlay_settings(&ax);
    ax.render_callback = render_cb;
    ax.adjustment_callback = adjustment_cb;
    ax.select_callback = NULL;
    ax.backend = AXOVERLAY_CAIRO_IMAGE_BACKEND;
    axoverlay_init(&ax, &err);
    if (err) {
        syslog(LOG_WARNING, "axoverlay_init: %s", err->message);
        g_clear_error(&err);
        return -1;
    }
    set_palette();

    gint maxw = axoverlay_get_max_resolution_width(1, &err);
    g_clear_error(&err);
    gint maxh = axoverlay_get_max_resolution_height(1, &err);
    g_clear_error(&err);
    if (maxw <= 0) maxw = 1920;
    if (maxh <= 0) maxh = 1080;

    struct axoverlay_overlay_data data;
    axoverlay_init_overlay_data(&data);
    data.postype = AXOVERLAY_CUSTOM_NORMALIZED;
    data.anchor_point = AXOVERLAY_ANCHOR_CENTER;
    data.x = 0.0;
    data.y = 0.0;
    data.width = maxw;
    data.height = maxh;
    data.scale_to_stream = FALSE;

    data.colorspace = AXOVERLAY_COLORSPACE_4BIT_PALETTE;
    boxes_id = axoverlay_create_overlay(&data, NULL, &err);
    if (err) {
        syslog(LOG_WARNING, "overlay boxes create: %s", err->message);
        g_clear_error(&err);
        boxes_id = -1;
    }

    data.colorspace = AXOVERLAY_COLORSPACE_ARGB32;
    text_id = axoverlay_create_overlay(&data, NULL, &err);
    if (err) {
        syslog(LOG_WARNING, "overlay text create: %s", err->message);
        g_clear_error(&err);
        text_id = -1;
    }

    overlay_ok = (boxes_id > 0 || text_id > 0);
    ACAP_STATUS_SetBool("overlay", "created", overlay_ok);
    syslog(LOG_INFO, "overlay init: boxes_id=%d text_id=%d max=%dx%d",
           boxes_id, text_id, maxw, maxh);

    axoverlay_redraw(&err);
    g_clear_error(&err);
    return overlay_ok ? 0 : -1;
}

void OVERLAY_Update(cJSON *detections, int modelWidth, int modelHeight) {
    if (!overlay_ok) return;
    model_w = modelWidth;
    model_h = modelHeight;
    int n = 0;
    cJSON *d = detections ? detections->child : NULL;
    for (; d && n < MAX_DETS; d = d->next) {
        cJSON *label = cJSON_GetObjectItem(d, "label");
        cJSON *c = cJSON_GetObjectItem(d, "c");
        cJSON *x = cJSON_GetObjectItem(d, "x");
        cJSON *y = cJSON_GetObjectItem(d, "y");
        cJSON *w = cJSON_GetObjectItem(d, "w");
        cJSON *h = cJSON_GetObjectItem(d, "h");
        if (!label || !label->valuestring || !x || !y || !w || !h) continue;
        ovl_det_t *o = &snapshot[n++];
        snprintf(o->label, sizeof(o->label), "%s", label->valuestring);
        o->conf = c ? c->valueint : 0;
        o->x = x->valuedouble;
        o->y = y->valuedouble;
        o->w = w->valuedouble;
        o->h = h->valuedouble;
    }
    snapshot_n = n;

    GError *err = NULL;
    axoverlay_redraw(&err);
    if (err) {
        /* overlayd may be restarting — transient, never fatal */
        static int logged = 0;
        if (logged++ < 5)
            syslog(LOG_WARNING, "axoverlay_redraw: %s", err->message);
        g_clear_error(&err);
    }
}

void OVERLAY_SetStats(const char *line) {
    if (!line) line = "";
    if (strncmp(stats_line, line, sizeof(stats_line) - 1) == 0) return;
    snprintf(stats_line, sizeof(stats_line), "%s", line);
    /* text refresh rides the next detection redraw at inference rate */
}

void OVERLAY_Cleanup(void) {
    GError *err = NULL;
    if (boxes_id > 0) { axoverlay_destroy_overlay(boxes_id, &err); g_clear_error(&err); }
    if (text_id > 0) { axoverlay_destroy_overlay(text_id, &err); g_clear_error(&err); }
    if (overlay_ok) axoverlay_cleanup();
    overlay_ok = 0;
}
