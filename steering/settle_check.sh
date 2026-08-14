#!/usr/bin/env bash
# Quantify step-mode settling: reset off-center, run controller, sample tilt.
set -u
H="http://195.60.68.14:12101"
AUTH=(--digest -u "VLTuser:${PTZ_PASSWORD}")
PY=~/machinery/env/bin/python
ptz() { curl -s -m 10 "${AUTH[@]}" "$H/axis-cgi/com/ptz.cgi?$1&camera=1"; }

ptz "pan=60&tilt=-9&zoom=1" >/dev/null
sleep 2
cd /mnt/c/20260812_ConstructionYoloModel/steering
$PY -u ptz_tracker.py --config config.loaner.yaml >/tmp/t.log 2>&1 &
P=$!
for i in $(seq 1 9); do
    sleep 2
    t=$(ptz "query=position" | grep '^tilt=')
    echo "t=$((i * 2))s  $t"
done
kill "$P" 2>/dev/null
ptz "move=home" >/dev/null
echo "camera returned home"
