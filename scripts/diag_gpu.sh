#!/usr/bin/env bash
echo "--- /usr/lib/wsl/lib ---"
ls -la /usr/lib/wsl/lib/ 2>&1 | head -20
echo "--- /dev/dxg ---"
ls -la /dev/dxg 2>&1
echo "--- nvidia-smi ---"
command -v nvidia-smi && nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
echo "--- kernel ---"
uname -r
echo "--- wsl.conf ---"
cat /etc/wsl.conf 2>/dev/null
