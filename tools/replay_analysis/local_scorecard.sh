#!/bin/bash
# local_scorecard.sh REPLAY... : scorecards for LOCAL games (our bot = team A).
# Converts each .replay with tools/replay_to_txt.mjs, adapts it with local2ladder.py into
# $OUT/<n>/<map>.txt (default OUT=/tmp/local_scorecards) and prints the scorecards + a summary.
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
OUT=${OUT:-/tmp/local_scorecards}; mkdir -p "$OUT/txt"
node "$ROOT/tools/replay_to_txt.mjs" "$@" --out "$OUT/txt" >/dev/null || exit 1
logs=()
for r in "$@"; do
  n=$(basename "$r" .replay); t="$OUT/txt/$n.txt"
  map=$(sed -n 's/^map: \([^ ]*\).*/\1/p' "$t" | head -1)
  mkdir -p "$OUT/$n"; python3 "$HERE/local2ladder.py" "$t" "$OUT/$n/$map.txt" "$map"
  logs+=("$OUT/$n/$map.txt")
done
cd "$HERE" && python3 scorecard.py "${logs[@]}"
