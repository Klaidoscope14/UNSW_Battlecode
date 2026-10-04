#!/bin/bash
# run_all.sh FOLDER... : analyse every <Map>.txt log in each folder into FOLDER/analysis/raw/
HERE=$(cd "$(dirname "$0")" && pwd)
for d in "$@"; do
  mkdir -p "$d/analysis/raw"
  for f in "$d"/*.txt; do
    n=$(basename "$f" .txt)
    for s in analyze deep econ profile; do
      out=$([ $s = analyze ] && echo metrics || echo $s)
      (cd "$HERE" && python3 $s.py "$OLDPWD/$f" > "$OLDPWD/$d/analysis/raw/${n}_${out}.txt" 2>&1) &
    done
  done
done
wait
echo "done: $(ls "$@" 2>/dev/null | wc -l) entries; see <folder>/analysis/raw/"
