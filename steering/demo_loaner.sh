#!/usr/bin/env bash
# Live PTZ follow demo on the loaner Q6355-LE: reset to an off-center view of
# the target, run the step controller, capture the follow sequence.
# Needs env PTZ_PASSWORD. Run inside WSL.
set -u
H="http://195.60.68.14:12101"
CAM="camera=1"
IMG="imagewidth=1280&imageheight=720"
D=/mnt/c/20260812_ConstructionYoloModel/models/stage1
PY=~/machinery/env/bin/python
AUTH=(--digest -u "VLTuser:${PTZ_PASSWORD}")

ptz() { curl -s -m 10 "${AUTH[@]}" "$H/axis-cgi/com/ptz.cgi?$1&$CAM" ; }
snap() { curl -s -m 12 "${AUTH[@]}" "$H/axis-cgi/jpg/image.cgi?resolution=1280x720" -o "$1" ; }
pos()  { ptz "query=position" | grep -E '^(pan|tilt|zoom)=' | tr '\n' ' ' ; }

echo "reset to off-center target view"
ptz "pan=60&tilt=-10&zoom=1" >/dev/null
sleep 3
snap "$D/follow_00_before.jpg"
echo "before: $(pos)"

cd /mnt/c/20260812_ConstructionYoloModel/steering
$PY -u ptz_tracker.py --config config.loaner.yaml > /tmp/follow.log 2>&1 &
PID=$!
for i in 1 2 3 4 5 6; do
    sleep 4
    snap "$D/follow_0$i.jpg"
    echo "t=$((i*4))s: $(pos)"
done
kill "$PID" 2>/dev/null
echo "=== controller actions ==="
grep -E 'tracking|center|zoom:' /tmp/follow.log | head -10
