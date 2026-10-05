# Ladder replay analysis pipeline

Works on the text logs exported from the replay viewer (`<Map>.txt`, first three lines = who started,
result, which ids are ours). Map files come from `maps/` in this project.

```bash
bash tools/replay_analysis/run_all.sh Byte-Force_Match_Replays Jingling_munmin_Replays
```

For every `<Map>.txt` in each folder this writes `<folder>/analysis/raw/`:

| file | what it answers |
|---|---|
| `<Map>_metrics.txt` | timeline (dragons, total length, queen length, longest, pearls eaten), economy, splits, movement, deaths by cause and phase, head-on trades (lengths, who sprinted), queens' last 8 rounds with a board, our status texts by phase, our last deaths |
| `<Map>_deep.txt` | how the enemy queen died and whether any of our dragons could see it; self-inflicted deaths (forced or chosen, re-eaten or not); our non-combat deaths (age, corridor cells); our attack deaths and the fleet ratio at the time; pearls per dragon-turn (opening vs later); pearl races |
| `<Map>_econ.txt` | head distance to spawn tiles / pearls, pearls left next to a head, races lost while we were nearer and what our dragon was doing |
| `<Map>_profile.txt` | length mix of each fleet over time, lifespans, queen length/roaming/pressure, use of paid steps |

Single-purpose tools: `opening.py <log> [rounds]` (round-by-round opening), `adjacent.py <log>`
(one-step pearls eaten vs missed, kelp-aware).

Library: `ladderlog.py` (parser + replay: teams through splits, multi-step moves, eating, paid steps,
splits, deaths; `board()` prints an ASCII board). Edge numbering checked against all 84 wall deaths in
5 maps. Ids are shared by both teams after the first split, so never use id parity for the team.

## Opponent tactics (added for the Stochastic Parrot analysis)

```bash
bash tools/replay_analysis/tactics.sh Stochastic_Parrot_Replays     # -> analysis/raw/<Map>_tactics.txt
```

| script | what it answers |
|---|---|
| `fountain.py` | fountain tiles (respawn period <= 5) and who ate from them, by phase |
| `pocketcycle.py` | every trip into a fountain pocket: entry length, splits inside, dead stub, net length |
| `escort.py LOG us|opp` | enemy heads that stay within 2 of a queen for 3+ rounds (stalkers / pinning escorts) |
| `queenstyle.py` | both queens: length over time, roaming, 2x2 loops, contact with enemy heads |
| `hunters.py` | our hunting effort and distance to their queen per 50 rounds |
| `approach.py` | do heads move toward the other side's dragons / queen more than chance (lift) |
| `infant.py` | split children dying within 2 rounds, by the parent's status |
| `trades.py` | our attack deaths: 1-for-1 or alone, which enemy lengths we took |
| `snap.py LOG R [X Y RAD]` | the board at the start of round R (whole map or a window) + our dragons |
| `track.py LOG ID [FROM TO]` | one dragon round by round (head, length, ask, status, split, death) |
| `local2ladder.py IN OUT MAP` | convert a local `tools/replay_to_txt.mjs` log into this format (put it in a folder as `<Map>.txt`) |
