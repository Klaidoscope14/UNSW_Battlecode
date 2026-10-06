#!/bin/bash
# suite.sh NAME [DEF=VAL ...] : the 204-game check. Builds fleet/main.cpp with the given #define overrides into
# work/NAME, plays 17 maps x 3 sparring bots (vib2, swarm2, fleet11) x 2 sides x 2 seeds (~6 min, 8 parallel),
# writes results/NAME.txt and prints a summary. SEED0=3 env var -> seeds 3-4 (results/NAME_s3.txt).
# Compare: python3 compare.py v2.16.0 NAME      Identical-game check (pure refactors): python3 same.py v2.16.0 NAME
T=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$T/../.." && pwd)
export PATH=~/.local/bin:$PATH UNSWBC_NO_UPDATE=1
NAME=$1; shift
OUT="$T/results/${NAME}${SEED0:+_s$SEED0}.txt"
B="$T/work/$NAME"; rm -rf "$B"; mkdir -p "$B"; cp "$ROOT/fleet/bot.toml" "$ROOT/fleet/helper.hpp" "$B/"
{ for d in "$@"; do echo "#define ${d%%=*} ${d#*=}"; done; cat "$ROOT/fleet/main.cpp"; } > "$B/main.cpp"
cd "$ROOT"
if ! unswbc run maps/default_small.map "$B" "$T/sparring/vib2" --seed 1 --no-replay > "$B/build.log" 2>&1; then echo "BUILD/RUN FAILED - see $B/build.log"; exit 1; fi
grep -iE "error:" "$B/build.log" && { echo "BUILD FAILED"; exit 1; }
caffeinate -i python3 "$T/bench.py" --seed0 "${SEED0:-1}" --seeds 2 -j 8 --bot "$B" \
  --opps "$T/sparring/vib2,$T/sparring/swarm2,$T/sparring/fleet11" \
  --maps australia,autarky,default,devil,dilemma,islands,maze,portals,queen_of_spades,schooltime,slithery_fight,stripes,tower_defense,trauma,trophy,unsw,weakhold \
  > "$OUT" 2>&1
python3 "$T/compare.py" "$(basename "$OUT" .txt)"
