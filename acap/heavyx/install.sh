#!/usr/bin/env bash
# Install/upgrade HeavyX on an Axis ARTPEC-9 camera.
# Usage:  AXIS_USER=root AXIS_PASS=... ./install.sh <camera-host> [eap-file]
# Temporarily enables unsigned-app installs and re-disables afterwards.
set -euo pipefail
HOST=${1:?usage: AXIS_USER=u AXIS_PASS=p ./install.sh <camera-host> [eap]}
EAP=${2:-$(ls -t build/HeavyX_*_aarch64.eap 2>/dev/null | head -1)}
: "${AXIS_USER:?set AXIS_USER}" ; : "${AXIS_PASS:?set AXIS_PASS}"
[ -f "$EAP" ] || { echo "no .eap found (build first or pass a path)"; exit 1; }
A=(--digest -u "${AXIS_USER}:${AXIS_PASS}")
B="http://${HOST}/axis-cgi"
echo "== installing $(basename "$EAP") on $HOST =="
curl -sf -m 10 "${A[@]}" "$B/applications/config.cgi?action=set&name=AllowUnsigned&value=true" >/dev/null
trap 'curl -s -m 10 "${A[@]}" "$B/applications/config.cgi?action=set&name=AllowUnsigned&value=false" >/dev/null' EXIT
curl -sf -m 180 "${A[@]}" -F "packfil=@${EAP}" "$B/applications/upload.cgi"
curl -sf -m 20 "${A[@]}" "$B/applications/control.cgi?action=start&package=heavyx"
echo
echo "== done — open http://${HOST}/local/heavyx/ =="
