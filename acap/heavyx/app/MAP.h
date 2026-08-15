/**
 * MAP.h — model-space ⇄ video-frame coordinate mapping for HeavyX.
 *
 * Detections leave Model_Inference() in MODEL pixel space (modelW×modelH,
 * top-left origin). The video frame is videoW×videoH whose relationship to
 * model space depends on the scale mode (see Model.c video-geometry setup and
 * preprocess.c transform factors, which this mirrors):
 *
 *   stretch   : model input is the full frame anisotropically stretched —
 *               plain per-axis scaling.
 *   crop      : video == model dimensions (1:1 center crop) — identity.
 *   letterbox : frame scaled uniformly by s = modelW/videoW to fit width,
 *               padded top+bottom; content height = videoH*s.
 *
 * Used by OVERLAY.c (drawing onto a video-aspect surface) and PTZ.c
 * (centering math against real frame edges). Upstream ships an equivalent
 * preprocess_transform_detection() but never wires the context out of
 * Model.c, so we map from the public geometry instead.
 */
#ifndef HEAVYX_MAP_H
#define HEAVYX_MAP_H

typedef struct {
    int model_w, model_h;
    int video_w, video_h;
    int letterbox;        /* 1 when scaleMode is letterbox */
    double pad_y;         /* model-space vertical padding (letterbox only) */
    double sx, sy;        /* model→video scale factors */
} MAP_Transform;

/* scaleMode string as reported in the model config: "stretch" | "crop" |
 * "letterbox" (Model.c scaleModeName). Unknown strings fall back to stretch. */
static inline MAP_Transform MAP_Make(int model_w, int model_h,
                                     int video_w, int video_h,
                                     const char *scale_mode_name) {
    MAP_Transform t;
    t.model_w = model_w > 0 ? model_w : 1;
    t.model_h = model_h > 0 ? model_h : 1;
    t.video_w = video_w > 0 ? video_w : t.model_w;
    t.video_h = video_h > 0 ? video_h : t.model_h;
    t.letterbox = 0;
    t.pad_y = 0.0;
    t.sx = (double)t.video_w / t.model_w;
    t.sy = (double)t.video_h / t.model_h;
    if (scale_mode_name && scale_mode_name[0] == 'l') {   /* letterbox */
        double s = (double)t.model_w / t.video_w;         /* width-bound fit */
        double content_h = t.video_h * s;                 /* in model px */
        t.letterbox = 1;
        t.pad_y = (t.model_h - content_h) / 2.0;
        t.sx = 1.0 / s;
        t.sy = 1.0 / s;
    }
    return t;
}

/* model px → video px (x is unaffected by vertical letterbox padding) */
static inline double MAP_X(const MAP_Transform *t, double mx) {
    return mx * t->sx;
}
static inline double MAP_Y(const MAP_Transform *t, double my) {
    return (my - (t->letterbox ? t->pad_y : 0.0)) * t->sy;
}
/* widths/heights scale without offset */
static inline double MAP_W(const MAP_Transform *t, double mw) {
    return mw * t->sx;
}
static inline double MAP_H(const MAP_Transform *t, double mh) {
    return mh * t->sy;
}

#endif /* HEAVYX_MAP_H */
