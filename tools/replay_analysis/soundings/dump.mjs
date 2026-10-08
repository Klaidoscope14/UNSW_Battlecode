// dump.mjs IN.replay OUT.tsv : one line per event (round, kind, fields) via the Soundings decoder
import { decodeReplay, EV } from './replay.js';
import { readFileSync, writeFileSync } from 'fs';
const [,, inp, out] = process.argv;
const rep = await decodeReplay(readFileSync(inp), { logs: true });
const L = [];
L.push(`#map\t${rep.map.split('\n').find(l => l.startsWith('MAP_NAME')) || ''}`);
L.push(`#bots\t${rep.botA}\t${rep.botB}`);
L.push(`#result\t${JSON.stringify(rep.result)}`);
let R = 0, turn = -1;
for (const e of rep.events) {
  switch (e.k) {
    case EV.ROUND: R = e.round; break;
    case EV.TURN: turn = e.id; break;
    case EV.TILE: L.push(`${R}\ttile\t${e.x}\t${e.y}\t${e.pearl ? 1 : 0}\t${turn}`); break;
    case EV.UPDATE: L.push(`${R}\tupd\t${e.id}\t${e.hx}\t${e.hy}\t${e.tx}\t${e.ty}`); break;
    case EV.SPLIT: L.push(`${R}\tsplit\t${e.parent}\t${e.child}\t${e.team}\t${e.childBody.length}\t${e.parentBody.length}`); break;
    case EV.SONAR: L.push(`${R}\tsonar\t${e.id}`); break;
    case EV.DEATH: L.push(`${R}\tdeath\t${e.id}\t${e.reason}`); break;
    case EV.ACTION: if (e.kind) L.push(`${R}\tact\t${e.id}\t${e.kind}\t${e.steps ? e.steps.length : 0}\t${e.split}`); break;
    case EV.INDICATOR: L.push(`${R}\tind\t${e.id}\t${(e.text || '').replace(/\s+/g, ' ')}`); break;
    default: break;
  }
}
writeFileSync(out, L.join('\n') + '\n');
// the map text too, for kelp / beds
writeFileSync(out.replace(/\.tsv$/, '.map'), rep.map);
console.log(inp.split('/').pop(), rep.map.split('\n').find(l => l.startsWith('MAP_NAME')), '|', rep.botA, 'vs', rep.botB, '|', JSON.stringify(rep.result));
