#!/bin/bash
# run_all.sh FOLDER... : analyse every <Map>.txt log in each folder into FOLDER/analysis/raw/
HERE=$(cd "$(dirname "$0")" && pwd)
for d in "$@"; do
  mkdir -p "$d/analysis/raw"
  for f in "$d"/*.txt; do
    [ "$(wc -l < "$f")" -lt 5 ] && continue   # header only, no game
    n=$(basename "$f" .txt)
    for s in analyze deep econ profile; do
      out=$([ $s = analyze ] && echo metrics || echo $s)
      (cd "$HERE" && python3 $s.py "$OLDPWD/$f" > "$OLDPWD/$d/analysis/raw/${n}_${out}.txt" 2>&1) &
    done
  done
done
wait
# one-page scorecards: per game, and a summary for the whole folder
for d in "$@"; do
  for f in "$d"/*.txt; do
    [ "$(wc -l < "$f")" -lt 5 ] && continue   # header only, no game
    n=$(basename "$f" .txt)
    (cd "$HERE" && python3 scorecard.py "$OLDPWD/$f" > "$OLDPWD/$d/analysis/raw/${n}_scorecard.txt" 2>&1)
  done
  (cd "$HERE" && python3 scorecard.py --summary-only $(for f in "$OLDPWD/$d"/*.txt; do [ "$(wc -l < "$f")" -ge 5 ] && printf '%q ' "$f"; done) > "$OLDPWD/$d/analysis/SCORECARD.txt" 2>&1)
done
echo "done: $(ls "$@" 2>/dev/null | wc -l) entries; see <folder>/analysis/raw/"
