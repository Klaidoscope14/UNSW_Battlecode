# Ladder .replay decoding

`replay.js` is the decoder from [battlecode-soundings](https://github.com/Lachy-Dauth/battlecode-soundings) (MIT licence), copied unchanged apart from a header line.

```
node dump.mjs GAME.replay out/GAME.tsv            # writes out/GAME.tsv (events) and out/GAME.map
python3 econ5116.py out                           # per map: pearls eaten by r50/100/150, dragons, splits, our deaths, our intents
python3 eatmap.py out/GAME.tsv out/GAME.map 100   # where pearls appear / who eats them, by column band
```

Team 0 in the map's DRAGON lines is team A. Our side is the team whose bot writes indicator strings.
Bed detection in econ5116.py does not work (map headers carry no bed marks); use eatmap.py for locations.

## One command

```
tools/replay_analysis/soundings/analyze_zip.sh ~/Downloads/battle-XXXX-replays.zip v5118
```
unpacks into `Ladder_v5118_Replays/`, decodes, and writes `Ladder_Reports/v5118.md` (results, by map, where length goes,
pearl sources, length/dragons over the game, when games turn, tactics, per-map opening with who-eats-where bands and flags).
Checked against the Soundings site on the v5.1.16 games: pearls eaten, dragons lost and sonar pings match exactly.
