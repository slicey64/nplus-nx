#!/usr/bin/env bash
# nplus-nx: tools/run_pc.ps1 for macOS. Runs the PC build for a while with the run script's settings, then stops it.
#   tools/nplus/run_pc.sh [seconds=90] [name=run] [extra cvars...]
# Logs and screenshots go to out/pc_runs/<name>/.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SECONDS_TO_RUN="${1:-90}"
NAME="${2:-run}"
shift $(( $# >= 2 ? 2 : $# ))
GAME="${NPLUS_GAME:-$HOME/nplus/game}"
DIR="$ROOT/app/out/pc_host"
LOGS="$ROOT/out/pc_runs/$NAME"
mkdir -p "$LOGS" "$ROOT/out/pc_user"
LOG="$LOGS/$NAME.log"
rm -f "$LOG"
cd "$DIR"
# The SDK stages the loader and MoltenVK next to the executable; point the loader at that ICD explicitly.
export VK_ICD_FILENAMES="$DIR/vulkan/share/vulkan/icd.d/MoltenVK_icd.json"
export DYLD_LIBRARY_PATH="$DIR/vulkan/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
START=$(date +%s); touch "$LOGS/.start"
"$DIR/renut" --log_file="$LOG" --audio_mute=true --fullscreen=false \
    --game_data_root="$GAME" --user_data_root="$ROOT/out/pc_user" \
    --nfsmw_captura_cada_s=15 --nfsmw_captura_max=60 --log_file_writer=false "$@" \
    > "$LOGS/stdout.txt" 2> "$LOGS/stderr.txt" &
PID=$!
echo "started pid $PID"
while kill -0 "$PID" 2>/dev/null && [ $(( $(date +%s) - START )) -lt "$SECONDS_TO_RUN" ]; do sleep 2; done
if kill -0 "$PID" 2>/dev/null; then
    kill "$PID"; sleep 2; kill -9 "$PID" 2>/dev/null
    echo "stopped pid $PID after $SECONDS_TO_RUN s"
else
    wait "$PID"; echo "exited by itself with code $?"
fi
find "$DIR" "$DIR/capturas" -maxdepth 1 -name '*.png' -newer "$LOGS/.start" -exec mv {} "$LOGS/" \; 2>/dev/null
[ -f "$LOG" ] && tail -n 30 "$LOG"
