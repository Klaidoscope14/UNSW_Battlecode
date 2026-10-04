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
