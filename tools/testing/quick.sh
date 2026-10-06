#!/bin/bash
# quick.sh [BOT_DIR] : ~2 min smoke test. 5 maps x vib2 x both sides (10 games) + 1 sandbox game on the
# biggest map for CPU ("team A points per turn": p50 / p99 / max). Default BOT_DIR = fleet.
T=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$T/../.." && pwd); BOT=${1:-fleet}
export PATH=~/.local/bin:$PATH UNSWBC_NO_UPDATE=1
cd "$ROOT"
unswbc run maps/default_small.map "$BOT" "$T/sparring/vib2" --seed 1 --no-replay >/dev/null 2>&1   # build once: parallel rebuilds clash
python3 "$T/bench.py" --bot "$BOT" --opps "$T/sparring/vib2" --maps big_empty,maze,portals,devil,unsw -j 10 | grep -E "^vs|queen deaths|: \?"
unswbc run maps/big_empty.map "$BOT" "$T/sparring/vib2" --seed 1 --no-replay --sandbox 2>&1 | grep -E "team (A|B) points|error|panic|invalid" | head -3
