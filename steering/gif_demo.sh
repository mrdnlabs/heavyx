#!/usr/bin/env bash
# Capture a follow sequence on the loaner and assemble a GIF.
# Env: PTZ_PASSWORD. Run inside WSL. Args: [zoom on|off]
set -u
H="http://195.60.68.14:12101"
D=/mnt/c/20260812_ConstructionYoloModel/models/stage1/gif
PY=~/machinery/env/bin/python
AUTH=(--digest -u "VLTuser:${PTZ_PASSWORD}")
FRAMES=26
INTERVAL=1.1

ptz()  { curl -s -m 10 "${AUTH[@]}" "$H/axis-cgi/com/ptz.cgi?$1&camera=1" ; }
snap() { curl -s -m 12 "${AUTH[@]}" "$H/axis-cgi/jpg/image.cgi?resolution=1280x720" -o "$1" ; }

rm -rf "$D"; mkdir -p "$D"
echo "reset to off-center view"
ptz "pan=60&tilt=-9&zoom=1" >/dev/null
sleep 3

cd /mnt/c/20260812_ConstructionYoloModel/steering
$PY -u ptz_tracker.py --config config.loaner.yaml > /tmp/gif.log 2>&1 &
PID=$!
for i in $(seq -w 0 $((FRAMES-1))); do
    snap "$D/f_$i.jpg"
    sleep "$INTERVAL"
done
kill "$PID" 2>/dev/null
echo "captured $FRAMES frames; tracking log:"
grep -E 'tracking' /tmp/gif.log | head -2

# assemble GIF (downscale to 640px, ~7 fps)
$PY - "$D" <<'PYEOF'
import sys, glob, os
from PIL import Image
d = sys.argv[1]
files = sorted(glob.glob(os.path.join(d, "f_*.jpg")))
frames = []
for f in files:
    im = Image.open(f).convert("RGB")
    im = im.resize((640, 360))
    frames.append(im)
out = "/mnt/c/20260812_ConstructionYoloModel/models/stage1/loaner_follow.gif"
frames[0].save(out, save_all=True, append_images=frames[1:],
               duration=140, loop=0, optimize=True)
print("wrote", out, os.path.getsize(out)//1024, "KB,", len(frames), "frames")
PYEOF
