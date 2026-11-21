#!/usr/bin/env bash
# nplus-nx: tools/switch_shaders.ps1 for macOS / Linux. Translates every dumped shader (out/renut_shader_dump: the
# shaders N+ compiled at run time, captured by the PC build) for deko3d and packages them for the Switch build
# (app/out/renut_switch). Needs the PC build (its XenosRecomp) and tools/deko3d_build_tools.sh's tools under
# out/deko3d_tools (on macOS: Homebrew's spirv-cross and the PC uam, linked under the .exe names the tools expect).
#   tools/nplus/switch_shaders.sh [--no-centroid]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PY="${PYTHON:-$HOME/nplus/.venv/bin/python}"
EXE="$ROOT/app/out/pc_host/xenos_recomp/build/XenosRecomp/XenosRecomp"
COMMON="$ROOT/app/thirdparty/reblue-XenosRecomp/XenosRecomp/shader_common.h"
DUMP="$ROOT/out/renut_shader_dump"
WORK="$ROOT/out/deko3d_shaders"
PKG="app/out/renut_switch"
unset BANJO_CENTROID
if [ "${1:-}" = "--no-centroid" ]; then
    WORK="$ROOT/out/deko3d_shaders_nocentroid"; PKG="app/out/renut_switch_nocentroid"; export BANJO_CENTROID=0
fi
[ -x "$EXE" ] || { echo "no $EXE: build the PC app with Renut first" >&2; exit 1; }
EARLY="$WORK/early_alpha_hlsl"; SPIRV="$WORK/spirv"
rm -rf "$EARLY" "$SPIRV"; mkdir -p "$EARLY" "$SPIRV"
BANJO_HLSL_DUMP_ONLY=1 "$EXE" "$DUMP" "$WORK/early_alpha_pass1.cpp" "$COMMON" "$EARLY"
"$PY" "$ROOT/tools/renut_early_alpha.py" "$EARLY"
BANJO_HLSL_OVERRIDE="$EARLY" BANJO_DEKO3D_SPIRV_DIR="$SPIRV" "$EXE" "$DUMP" "$WORK/shader_cache_deko3d.cpp" "$COMMON"
cd "$ROOT"
"$PY" tools/deko3d_shaders.py --spirv "$SPIRV" --out "$WORK"
"$PY" tools/deko3d_glsl_opt.py --in "$WORK/glsl" --out "$WORK/glsl_opt"
"$PY" tools/deko3d_shader_package.py --glsl "$WORK/glsl_opt" --out "$PKG"
if [ -z "${BANJO_CENTROID:-}" ]; then
    "$PY" tools/renut_switch_shader_cache.py --pc app/out/pc_host
fi
ls -la "$ROOT/$PKG"
