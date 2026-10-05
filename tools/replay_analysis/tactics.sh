#!/bin/bash
# tactics.sh FOLDER... : opponent-tactic analyses for every <Map>.txt into FOLDER/analysis/raw/<Map>_tactics.txt
# (fountain pockets, pocket-trip economics, queen escorts/stalkers, queen styles, hunting effort,
#  approach tendencies, infant deaths, attack trades).  Run after run_all.sh.
HERE=$(cd "$(dirname "$0")" && pwd)
for d in "$@"; do
  mkdir -p "$d/analysis/raw"
  for f in "$d"/*.txt; do
    n=$(basename "$f" .txt); F=$(cd "$(dirname "$f")" && pwd)/$(basename "$f")
    out="$(cd "$d" && pwd)/analysis/raw/${n}_tactics.txt"
    (cd "$HERE" && {
      for s in fountain pocketcycle queenstyle hunters approach infant trades; do echo "### $s"; python3 $s.py "$F"; echo; done
      echo "### escort (our queen)"; python3 escort.py "$F" us; echo
      echo "### escort (their queen)"; python3 escort.py "$F" opp
    } > "$out" 2>&1) &
  done
done
wait
echo "done: see <folder>/analysis/raw/*_tactics.txt"
