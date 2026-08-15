# HeavyX — Camera App Redesign (design package)

Interactive design mockups for the HeavyX on-camera web app (Axis ARTPEC-9, `/local/heavyx/`).
Open any `.dc.html` file in a browser — data is simulated (moving detections, PTZ commands, health states).

## Files
- `HeavyX.dc.html` — the app: full-bleed live HUD, Detect/Track/Output drawer, drag-the-corners follow zone, AOI polygon editor (drag vertices), steering log, priority list (person never followed), phone-responsive. Simulate toggles (target lost / model fault / non-PTZ camera) are exposed as tweakable props.
- `Live A - Console.dc.html`, `Live B - Signal HUD.dc.html`, `Live C - Field.dc.html` — the three explored directions (B was chosen).
- (design-tool runtime shims removed from repo; mockups need them only to open standalone)
- `github.md` — source-repo association (mrdnlabs/heavyx) + screen map.

## Notes for implementation
- Fonts load from Google Fonts in these mockups; the camera app must self-host Archivo + JetBrains Mono (offline constraint).
- Colors: bg #070707, panel rgba(7,7,7,.85), line #2a2822, ink #f2f0ea, muted #9a958a, accent #ffc400, ok #5ad07e, warn #e07840, fail #e85d4d.
- Data shapes mirror the real endpoints (HeavyX `/local/heavyx/status` detections, HeavyX settings ptz.marginPercent/ptz.logMaxKB, VAPIX `center`/`areazoom` commands).
