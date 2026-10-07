#!/bin/bash
# opening.sh NAME "DEF=VAL ..." ["map1 map2 ..."] : opening economy (pearls / dragons by r50/100/150, ours vs theirs,
# medians) on the given maps x 3 sparring bots x 2 sides, seed 1, with replays (deleted afterwards).
# Noisy: one Around UNSW game can swing +-150 pearls - use it to spot big effects, decide with suite.sh.
T=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$T/../.." && pwd)
export PATH=~/.local/bin:$PATH UNSWBC_NO_UPDATE=1
NAME=$1; DEFS=$2; MAPS=${3:-"islands stripes tower_defense trauma trophy weakhold unsw australia"}
B="$T/work/$NAME"; W="$T/work/op_$NAME"; rm -rf "$B" "$W"; mkdir -p "$B" "$W/rep" "$W/L"
cp "$ROOT/fleet/bot.toml" "$ROOT"/fleet/*.hpp "$B/"
{ for d in $DEFS; do echo "#define ${d%%=*} ${d#*=}"; done; cat "$ROOT/fleet/main.cpp"; } > "$B/main.cpp"
cd "$ROOT"; unswbc run maps/default_small.map "$B" "$T/sparring/vib2" --seed 1 --no-replay >/dev/null 2>&1
for m in $MAPS; do for o in vib2 swarm2 fleet11; do for s in A B; do echo "$m $o $s"; done; done; done | \
  xargs -P 8 -L 1 bash -c 'if [ "$2" = A ]; then a="'"$B"'"; b="'"$T"'/sparring/$1"; else a="'"$T"'/sparring/$1"; b="'"$B"'"; fi; unswbc run maps/$0.map "$a" "$b" --seed 1 -o "'"$W"'/rep/$0-$1-$2.replay" >/dev/null 2>&1'
node tools/replay_to_txt.mjs "$W/rep" --out "$W/txt" >/dev/null
for f in "$W"/txt/*-*.txt; do n=$(basename "$f" .txt); map=${n%%-*}; case $map in tower_defense) M=Tower_Defense;; queen_of_spades) M=Queen_of_spades;; slithery_fight) M=Slithery_fight;; *) M=$(python3 -c "print('$map'.capitalize())");; esac
  mkdir -p "$W/L/$M"; python3 tools/replay_analysis/local2ladder.py "$f" "$W/L/$M/Elo-0-$n.txt" "$map"; done
(cd tools/replay_analysis && python3 opening_econ.py "$W/L") | grep -v "our queen"
rm -rf "$W/rep" "$W/txt"   # replays are big - keep only the converted logs in work/op_NAME/L
