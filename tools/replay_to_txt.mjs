#!/usr/bin/env node
// Convert UNSW Battlecode .replay files into readable game-log .txt files.
//
// Usage:
//   node tools/replay_to_txt.mjs <replay files or folders...> [--out DIR] [--sonar] [--countdowns]
//
//   node tools/replay_to_txt.mjs battle-M977648-replays
//     -> battle-M977648-replays/txt/M977648.txt ... plus battle-M977648-replays/txt/summary.txt
//
// The replay format is decoded by the official replay viewer's own code (the
// VS Code extension that `unswbc vscode` installs), loaded here without VS Code.
// Sonar pings and pearl countdowns are left out unless asked for (they are bulky).

import fs from 'fs';
import os from 'os';
import path from 'path';
import { execFileSync } from 'child_process';

// ---------------------------------------------------------------- arguments
const args = process.argv.slice(2);
const opts = { out: null, sonar: false, countdowns: false };
const inputs = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--out') opts.out = args[++i];
  else if (args[i] === '--sonar') opts.sonar = true;
  else if (args[i] === '--countdowns') opts.countdowns = true;
  else if (args[i] === '-h' || args[i] === '--help') {
    console.log(fs.readFileSync(new URL(import.meta.url)).toString().split('\n').slice(1, 12).join('\n'));
    process.exit(0);
  } else inputs.push(args[i]);
}
if (!inputs.length) {
  console.error('usage: node tools/replay_to_txt.mjs <replay files or folders...> [--out DIR] [--sonar] [--countdowns]');
  process.exit(1);
}

// ---------------------------------------------------------------- viewer code
function findViewerJs() {
  if (process.env.UNSWBC_VIEWER_JS) return fs.readFileSync(process.env.UNSWBC_VIEWER_JS, 'utf8');
  // 1. the extension installed into VS Code
  const extDir = path.join(os.homedir(), '.vscode', 'extensions');
  if (fs.existsSync(extDir)) {
    const hits = fs.readdirSync(extDir).filter((d) => d.includes('battledragon-replay-viewer')).sort();
    for (const d of hits.reverse()) {
      const f = path.join(extDir, d, 'dist', 'webview', 'webview.js');
      if (fs.existsSync(f)) return fs.readFileSync(f, 'utf8');
    }
  }
  // 2. the .vsix shipped inside the unswbc Python package
  for (const py of ['/opt/anaconda3/bin/python3', 'python3']) {
    try {
      const dir = execFileSync(py, ['-c', 'import unswbc,os;print(os.path.dirname(unswbc.__file__))']).toString().trim();
      const vsix = path.join(dir, 'replay-viewer.vsix');
      if (fs.existsSync(vsix)) return execFileSync('unzip', ['-p', vsix, 'extension/dist/webview/webview.js'], { maxBuffer: 64 << 20 }).toString();
    } catch (_) { /* try the next one */ }
  }
  throw new Error('replay viewer not found: run `unswbc vscode`, or set UNSWBC_VIEWER_JS to its webview.js');
}

function loadDecoder() {
  const src = findViewerJs();
  // The bundle ends by mounting its UI; swap that for exposing the decoder.
  const tail = "var __=document.getElementById(`app`);ni(__.dataset.view===`map`?g_:d_,{target:__})})();";
  if (!src.endsWith(tail) || !src.includes('function Xf(')) throw new Error('unexpected replay viewer version: this script needs updating');
  const patched = src.slice(0, -tail.length) + 'globalThis.__replayDecode=Xf;})();';
  const noop = () => {};
  const stub = () => new Proxy(function () {}, { get: (t, k) => (k === Symbol.toPrimitive ? () => '' : stub()), apply: () => stub(), construct: () => stub() });
  Object.assign(globalThis, {
    window: globalThis,
    document: stub(),
    localStorage: { getItem: () => null, setItem: noop, removeItem: noop },
    matchMedia: () => ({ matches: false, addEventListener: noop, removeEventListener: noop }),
    requestAnimationFrame: noop,
  });
  (0, eval)(patched);
  return globalThis.__replayDecode;
}

// ---------------------------------------------------------------- formatting
const REASON = { H: 'lost a head-to-head', W: 'hit a wall', S: 'hit itself', O: 'hit another dragon', A: 'no valid action' };
const isQueen = (id) => id < 2;

function convert(decode, file) {
  const r = decode(new Uint8Array(fs.readFileSync(file)));
  const m = r.match;
  const mapName = (r.map.match(/^MAP_NAME (.*)$/m) || [, '?'])[1];
  const size = (r.map.match(/^MAP (\d+) (\d+)/m) || [, '?', '?']).slice(1).join('x');
  // Teams: from the round-0 bodies, then from each split (children are born mid-round).
  const teamById = new Map();
  for (const d of m.roundAt(0).bodies.dragons.values()) teamById.set(d.id, d.team);
  for (const e of r.events) if (e.type === 'dragonSplit') teamById.set(e.childId, e.team);
  const teamOf = (round, id) => teamById.get(id) ?? m.roundAt(round).bodies.getById(id)?.team ?? '?';
  const ours = Object.values(r.teams).filter((t) => t.botId);
  const ourTeam = ours.length === 1 ? ours[0].id : '?';
  const res = r.result || {};

  const out = [];
  out.push(`replay: ${path.basename(file)}`);
  out.push(`map: ${mapName} (${size})   seed: ${r.seed}   rounds: ${m.maxRound}`);
  for (const t of Object.values(r.teams)) out.push(`team ${t.id}: ${t.name}  botId=${t.botId || '(hidden)'}${t.id === ourTeam ? '   <- probably us (our bot id is shown)' : ''}`);
  out.push(`result: winner ${res.winner ?? '?'} by ${res.endReason ?? '?'}`);
  for (const k of ['teamA', 'teamB'])
    if (res[k]) out.push(`  ${k}: dragons ${res[k].dragonCount}, longest ${res[k].longestDragon}, total length ${res[k].totalLength}, queen length ${res[k].queenLength}`);
  out.push('');

  // per-round summary + events
  let round = -1, prev = null;
  const deaths = [];
  const summary = (rd) => {
    const per = { A: { n: 0, len: 0, q: 0 }, B: { n: 0, len: 0, q: 0 } };
    for (const d of m.roundAt(rd).bodies.dragons.values()) {
      const p = per[d.team] || (per[d.team] = { n: 0, len: 0, q: 0 });
      p.n++; p.len += d.body.length; if (isQueen(d.id)) p.q = d.body.length;
    }
    return Object.entries(per).map(([t, p]) => `${t}: ${p.n} dragons, total length ${p.len}, queen ${p.q || 'dead'}`).join(' | ');
  };
  for (const e of r.events) {
    let line = null;
    const tm = (id) => teamOf(Math.max(0, round), id);
    switch (e.type) {
      case 'roundStart':
        round = e.round;
        out.push(`=== round ${round} === ${summary(round)}`);
        break;
      case 'tileChange':
        line = `(${e.tile.x},${e.tile.y}) ${e.hasPearl ? 'pearl appears' : 'pearl taken'}`;
        break;
      case 'pearlCountdown':
        if (opts.countdowns) line = `(${e.tile.x},${e.tile.y}) next pearl due in ${e.countdown}`;
        break;
      case 'dragonIndicator':
        line = `[${tm(e.id)}] dragon ${e.id} indicator: ${e.text}`;
        break;
      case 'dragonLog':
        line = `[${tm(e.id)}] dragon ${e.id}: ${e.text}`;
        break;
      case 'engineLog':
        line = `[${tm(e.id)}] engine on dragon ${e.id}: ${e.text}`;
        break;
      case 'dragonAction': {
        const a = e.action || {};
        const cpu = e.instructions ? `  (cpu ${e.instructions.count}${e.instructions.exceeded ? ' EXCEEDED' : ''})` : '';
        if (a.kind === 'move') line = `[${tm(e.id)}] dragon ${e.id} asks to move ${a.steps.join('')}${cpu}`;
        else if (a.kind === 'split') line = `[${tm(e.id)}] dragon ${e.id} asks to split off ${a.childSegmentCount}${cpu}`;
        else line = `[${tm(e.id)}] dragon ${e.id} action ${JSON.stringify(a)}${cpu}`;
        break;
      }
      case 'dragonUpdate': {
        if (round < 0) break; // initial placement
        const ate = prev && prev.type === 'tileChange' && !prev.hasPearl && prev.tile.x === e.head.x && prev.tile.y === e.head.y;
        line = `[${tm(e.id)}] dragon ${e.id} moves ${e.facing} to (${e.head.x},${e.head.y})${ate ? ', eats a pearl' : ''}`;
        break;
      }
      case 'dragonSplit':
        line = `[${e.team}] dragon ${e.parentId} splits off dragon ${e.childId}, ${e.childBody.length} cells (parent keeps ${e.parentBody.length})`;
        break;
      case 'dragonDeath': {
        const t = tm(e.id);
        const why = REASON[e.reason] ?? e.reason;
        deaths.push({ round, id: e.id, team: t, why });
        line = `[${t}] dragon ${e.id}${isQueen(e.id) ? ' (QUEEN)' : ''} dies: ${why}`;
        break;
      }
      case 'sonarPing':
        if (opts.sonar) line = `[${tm(e.senderId)}] dragon ${e.senderId} pings ${e.direction} ${e.value} -> ${e.hitId === undefined ? (e.hitKind ?? 'nothing') : `dragon ${e.hitId} (${e.hitKind})`}`;
        break;
      default:
        break;
    }
    if (line) out.push(`${round} ${line}`);
    prev = e;
  }

  // death totals
  out.push('');
  out.push('=== deaths ===');
  const tally = {};
  for (const d of deaths) tally[`${d.team} ${d.why}`] = (tally[`${d.team} ${d.why}`] || 0) + 1;
  for (const [k, v] of Object.entries(tally).sort()) out.push(`${k}: ${v}`);
  for (const d of deaths.filter((x) => isQueen(x.id))) out.push(`queen of team ${d.team} died in round ${d.round}: ${d.why}`);

  const queenDeaths = deaths.filter((x) => isQueen(x.id)).map((d) => `${d.team}-queen r${d.round} ${d.why}`).join('; ');
  const brief = `${path.basename(file)}  ${mapName}  winner ${res.winner ?? '?'} (${res.endReason ?? '?'})  us=${ourTeam}  rounds ${m.maxRound}  ${queenDeaths || 'no queen deaths'}`;
  return { text: out.join('\n') + '\n', brief };
}

// ---------------------------------------------------------------- main
const decode = loadDecoder();
const files = [];
for (const p of inputs) {
  if (fs.statSync(p).isDirectory())
    for (const f of fs.readdirSync(p).sort()) { if (f.endsWith('.replay')) files.push(path.join(p, f)); }
  else files.push(p);
}
if (!files.length) { console.error('no .replay files found'); process.exit(1); }

const briefs = [];
for (const f of files) {
  const outDir = opts.out ?? path.join(path.dirname(f), 'txt');
  fs.mkdirSync(outDir, { recursive: true });
  try {
    const { text, brief } = convert(decode, f);
    const dest = path.join(outDir, path.basename(f).replace(/\.replay$/, '.txt'));
    fs.writeFileSync(dest, text);
    briefs.push(brief);
    console.log(`wrote ${dest}`);
  } catch (err) {
    console.error(`failed on ${f}: ${err.message}`);
  }
}
const sumDir = opts.out ?? path.join(path.dirname(files[0]), 'txt');
fs.writeFileSync(path.join(sumDir, 'summary.txt'), briefs.join('\n') + '\n');
console.log(`wrote ${path.join(sumDir, 'summary.txt')}`);
