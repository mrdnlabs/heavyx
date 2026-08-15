/**
 * PTZ.h — on-camera follow steering for HeavyX (steerx v0.4.0 lineage).
 *
 * Consumes the per-frame detections in-process and drives the camera's own
 * PTZ over local VAPIX (ACAP_VAPIX_Get). Steering runs on a 1 Hz GLib timer;
 * detections are snapshotted by PTZ_Feed at inference rate.
 *
 * The follow policy is the settings array ptz.priority: order = precedence,
 * inclusion = followability (classes absent are never followed). Detection
 * and overlay always cover all classes regardless.
 */
#ifndef HEAVYX_PTZ_H
#define HEAVYX_PTZ_H

#include "cJSON.h"

int  PTZ_Init(cJSON *settings);   /* probes PTZ support; registers 1 Hz tick */
void PTZ_Feed(cJSON *detections, int modelWidth, int modelHeight);
void PTZ_Cleanup(void);

#endif /* HEAVYX_PTZ_H */
