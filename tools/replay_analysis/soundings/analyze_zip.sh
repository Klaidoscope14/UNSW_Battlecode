#!/bin/zsh
# analyze_zip.sh REPLAYS.zip NAME : unpack ladder replays into Ladder_NAME_Replays/, decode them, write Ladder_Reports/NAME.md
set -e
ZIP=$1; NAME=$2
ROOT=${0:A:h}/../../..
DIR=$ROOT/Ladder_${NAME}_Replays
mkdir -p "$DIR" "$DIR/out" "$ROOT/Ladder_Reports"
unzip -q -n "$ZIP" -d "$DIR"
for f in "$DIR"/*.replay; do node ${0:A:h}/dump.mjs "$f" "$DIR/out/${f:t:r}.tsv" > /dev/null; done
python3 ${0:A:h}/report.py "$DIR/out" > "$ROOT/Ladder_Reports/$NAME.md"
echo "$ROOT/Ladder_Reports/$NAME.md"
