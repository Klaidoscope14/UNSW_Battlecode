#!/bin/bash
# Mobility/topology weight sweep. Each config is a seadragon copy with the weight
# macros pre-#defined; bench.py runs it 32 games (all maps x both sides x 2 opps)
# and we print record + death breakdown. Defaults reproduce the shipped bot.
set -u
cd "$(dirname "$0")/.."
export PATH="$HOME/.local/bin:$PATH"

# name | prepended #defines (space separated NAME=VALUE)
CONFIGS=(
  "default|"
  "moremob|W_AREA=40 W_MOB=50"
  "morearea|W_AREA=100 W_MOB=20"
  "strongbottle|W_BOTTLE=60000"
  "deepmob|W_EXIT1=60 W_EXIT2=40 W_MOB=40 W_BOTTLE=40000"
)

for cfg in "${CONFIGS[@]}"; do
  name="${cfg%%|}"; name="${cfg%%|*}"; defs="${cfg#*|}"
  dir="tv_${name}"
  rm -rf "$dir"; cp -r seadragon "$dir"
  hdr=""
  for d in $defs; do hdr+="#define ${d/=/ }"$'\n'; done
  if [ -n "$hdr" ]; then printf '%s' "$hdr" | cat - "$dir/main.cpp" > "$dir/m.cpp" && mv "$dir/m.cpp" "$dir/main.cpp"; fi
  echo "######## config: $name   ($defs) ########"
  out=$(python3 tools/bench.py --bot "$dir" --opps v1bot,starter 2>&1)
  echo "$out" | grep -E "record|our deaths|length|avg reach"
  rm -rf "$dir"
done
echo "SWEEP_DONE"
