#!/usr/bin/env bash
# nplus-nx: tools/run_ryujinx.ps1 for macOS. Runs the Switch NRO in Ryujinx with N+'s own data folder
# (~/nplus/ryu-data/nplus: copies of the keys, firmware and Config.json, its own SD card), never the user's own.
#   tools/nplus/run_ryujinx.sh [seconds=180] [name=emu] [toml] [build=switch]
# The toml (optional) goes next to the NRO as nplus.toml. Logs and screenshots go to out/emu_runs/<name>/.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SECONDS_TO_RUN="${1:-180}"
NAME="${2:-emu}"
TOML="${3:-}"
BUILD="${4:-switch}"
RYU="${RYUJINX:-$HOME/Downloads/Ryujinx.app/Contents/MacOS/Ryujinx}"
# N+'s patched Ryujinx (tools/nplus/ryujinx-1.3.3-nplus.patch) is a framework-dependent build: it needs .NET 9's root.
if [ -z "${DOTNET_ROOT:-}" ] && [ -d /opt/homebrew/opt/dotnet@9/libexec ]; then
    export DOTNET_ROOT=/opt/homebrew/opt/dotnet@9/libexec
fi
DATA="$HOME/nplus/ryu-data/nplus"
SD="$DATA/sdcard/switch/nplus-nx"
NRO="$SD/nplus-nx.nro"
OUT="$ROOT/out/emu_runs/$NAME"
if pgrep -x Ryujinx >/dev/null; then echo "a Ryujinx is running: not starting"; exit 2; fi
BUILT="$ROOT/app/out/$BUILD/renut.nro"
[ -f "$BUILT" ] || { echo "no NRO at $BUILT"; exit 4; }
cp "$BUILT" "$NRO"
if [ -n "$TOML" ]; then cp "$TOML" "$SD/nplus.toml"; else rm -f "$SD/nplus.toml"; fi
mkdir -p "$OUT"
rm -rf "$SD/logs"
touch "$OUT/.start"
"$RYU" -r "$DATA" "$NRO" > "$OUT/ryujinx_stdout.txt" 2> "$OUT/ryujinx_stderr.txt" &
PID=$!
echo "started Ryujinx pid $PID"
START=$(date +%s)
while kill -0 "$PID" 2>/dev/null && [ $(( $(date +%s) - START )) -lt "$SECONDS_TO_RUN" ]; do sleep 3; done
if kill -0 "$PID" 2>/dev/null; then
    kill "$PID"; sleep 3; kill -9 "$PID" 2>/dev/null
    echo "stopped pid $PID after $SECONDS_TO_RUN s"
else
    wait "$PID"; echo "Ryujinx exited by itself (code $?)"
fi
sleep 2
[ -d "$SD/logs" ] && cp -R "$SD/logs" "$OUT/logs"
find "$SD" -name '*.png' -newer "$OUT/.start" -exec mv {} "$OUT/" \; 2>/dev/null
[ -d "$DATA/Logs" ] && find "$DATA/Logs" -name '*.log' -newer "$OUT/.start" -exec cp {} "$OUT/" \; 2>/dev/null
ls -la "$OUT"
