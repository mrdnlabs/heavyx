/**
 * OVERLAY.h — burned-in detection boxes/labels + stats footer via axoverlay.
 *
 * Draws at inference rate on the camera's own overlay engine, so boxes appear
 * in every stream/recording (unlike the web UI's browser canvas).
 * Two overlays: 4-bit palette layer for boxes (low memory) and an ARGB32
 * layer for label/confidence text plus a one-line stats footer.
 */
#ifndef HEAVYX_OVERLAY_H
#define HEAVYX_OVERLAY_H

#include "cJSON.h"

/* Non-fatal on failure (status "overlay.created" reflects the truth):
 * detection must run even when the overlay engine is unavailable. */
int  OVERLAY_Init(cJSON *settings);

/* Called from the detection hook with the final per-frame detections
 * (MODEL pixel space). Deep-copies; does not retain the pointer. */
void OVERLAY_Update(cJSON *detections, int modelWidth, int modelHeight);

/* One-line footer text (set by PTZ tick; replaces steerx's #D overlay). */
void OVERLAY_SetStats(const char *line);

void OVERLAY_Cleanup(void);

#endif /* HEAVYX_OVERLAY_H */
