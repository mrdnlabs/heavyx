# steerx (legacy)

**Superseded by [acap/heavyx](../../acap/heavyx/)** — HeavyX's `app/PTZ.c`
carries this steering logic in-process (no cross-app dependency, no
service-account HTTP hop). Kept for MIT lineage and for reproducing the
`poc-v0.4.0` release (the last two-app deployment: DetectX + SteerX).

Do not deploy alongside HeavyX — both would command the same PTZ.
