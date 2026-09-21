#!/bin/bash
# Beam-search weight sweep. Baseline (beam as no-op) is 30-2, self-deaths 8.
set -u
cd "$(dirname "$0")/.."
export PATH="$HOME/.local/bin:$PATH"

CONFIGS=(
  "room100|W_DEEP_ROOM=100"
  "room250|W_DEEP_ROOM=250"
  "room600|W_DEEP_ROOM=600"
  "deep6_room200|BEAM_DEPTH=6 W_DEEP_ROOM=200"
)
for cfg in "${CONFIGS[@]}"; do
  name="${cfg%%|*}"; defs="${cfg#*|}"
  dir="tb_${name}"
  rm -rf "$dir"; cp -r seadragon "$dir"
  hdr=""; for d in $defs; do hdr+="#define ${d/=/ }"$'\n'; done
  printf '%s' "$hdr" | cat - "$dir/main.cpp" > "$dir/m.cpp" && mv "$dir/m.cpp" "$dir/main.cpp"
  echo "######## $name   ($defs) ########"
  python3 tools/bench.py --bot "$dir" --opps v1bot,starter 2>&1 | grep -E "record|our deaths|avg reach"
  rm -rf "$dir"
done
echo "SWEEP_DONE"
