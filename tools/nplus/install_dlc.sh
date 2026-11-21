#!/usr/bin/env bash
# nplus-nx: installs N+'s extracted DLC packs (the PC port download's data/DLC/<package>/{Content.header,Data0.zip}:
# Levelpacks 1-3) into a user-data root in the SDK's content layout (sdk/src/system/xam/content_manager.cpp):
#   <root>/0000000000000000/5841085C/00000002/<package>/Data0.zip          the content (marketplace: xuid 0)
#   <root>/0000000000000000/5841085C/Headers/00000002/<package>.header     its XCONTENT_AGGREGATE_DATA (328 bytes)
#   tools/nplus/install_dlc.sh <DLC folder> <user-data root>
# PC: the root is --user_data_root (out/pc_user in tools/nplus/run_pc.sh). Copies; the source is left as it is.
set -euo pipefail
SRC="${1:?DLC folder (data/DLC of the download)}"
ROOT="${2:?user-data root}"
TITLE=5841085C
BASE="$ROOT/0000000000000000/$TITLE"
mkdir -p "$BASE/00000002" "$BASE/Headers/00000002"
for d in "$SRC"/*/; do
    name="$(basename "$d")"
    [ -f "$d/Content.header" ] || { echo "skip $name: no Content.header"; continue; }
    mkdir -p "$BASE/00000002/$name"
    find "$d" -mindepth 1 -maxdepth 1 ! -name Content.header -exec cp -R {} "$BASE/00000002/$name/" \;
    cp "$d/Content.header" "$BASE/Headers/00000002/$name.header"
    label="$(LC_ALL=C tr -d '\000' < "$d/Content.header" | LC_ALL=C grep -ao 'N+ [A-Za-z ]*[0-9]' | head -1 || true)"
    echo "installed $name ($label)"
done
