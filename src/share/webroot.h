#pragma once

// Self-contained web radar page served by ShareServer at GET /.
// Loads map textures from /textures/maps/<name>_radar.png and overview
// bounds from /textures/maps/<name>_radar.txt (same files the overlay
// already deploys), then connects to ws://<host>:<port>/ws and renders
// each incoming JSON frame on a canvas.
//
// Anonymized — the server sends an unlabeled roster (just team T/CT), no
// "this is the sharer" hint. The web radar renders standard spectator
// colors (yellow T, blue CT) and never rotates.
inline constexpr const char* kWebRootHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>CS2 Shared Radar</title>
<style>
  :root { color-scheme: dark; }
  html, body {
    margin: 0; padding: 0; height: 100%; background: #0b0b0f;
    color: #e6e6ec; font: 14px -apple-system, "Segoe UI", system-ui, sans-serif;
    overflow: hidden; touch-action: none;
  }
  #root { position: fixed; inset: 0; display: flex; align-items: center; justify-content: center; }
  canvas { background: #0b0b0f; image-rendering: -webkit-optimize-contrast; }
  #status {
    position: fixed; top: 10px; left: 10px; z-index: 10;
    background: rgba(12,12,16,0.78); padding: 6px 10px; border-radius: 6px;
    font-size: 12px; letter-spacing: 0.3px;
  }
  #status .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: #e85c5c; margin-right: 6px; vertical-align: middle; }
  #status.ok .dot { background: #4caf50; }
  #banner {
    position: fixed; bottom: 10px; left: 50%; transform: translateX(-50%);
    background: rgba(12,12,16,0.78); padding: 6px 12px; border-radius: 6px;
    font-size: 11px; color: #a9a9b3; letter-spacing: 0.3px;
  }
</style>
</head>
<body>
<div id="status"><span class="dot"></span><span id="st-text">connecting…</span></div>
<div id="root"><canvas id="c"></canvas></div>
<div id="banner">spectator view</div>
<script>
'use strict';

const stText = document.getElementById('st-text');
const statusEl = document.getElementById('status');
const canvas = document.getElementById('c');
const ctx = canvas.getContext('2d');

let state = null;              // last JSON frame from server
const mapImages = Object.create(null);
const mapBounds = Object.create(null);
let currentMap = null;

// Map atlases are 1024-wide square textures; cs2-dumper overview .txt files
// carry pos_x / pos_y / scale which together with that 1024 yield the world
// span represented by one texture. Same math the C++ radar does.
async function loadMap(name) {
  if (name in mapImages) return;
  mapImages[name] = null;
  try {
    const [img, txt] = await Promise.all([
      new Promise((res, rej) => {
        const i = new Image();
        i.onload = () => res(i); i.onerror = rej;
        i.src = `/textures/maps/${name}_radar.png`;
      }),
      fetch(`/textures/maps/${name}_radar.txt`).then(r => r.ok ? r.text() : '')
    ]);
    mapImages[name] = img;
    const kv = {};
    const re = /"(\w+)"\s+"([-\d.]+)"/g;
    let m;
    while ((m = re.exec(txt)) !== null) kv[m[1]] = parseFloat(m[2]);
    if (kv.scale) mapBounds[name] = { x: kv.pos_x, y: kv.pos_y, scale: kv.scale };
  } catch (e) {
    mapImages[name] = null;
  }
}

function resize() {
  const d = Math.min(window.innerWidth, window.innerHeight);
  canvas.width  = d * devicePixelRatio;
  canvas.height = d * devicePixelRatio;
  canvas.style.width  = d + 'px';
  canvas.style.height = d + 'px';
}
window.addEventListener('resize', resize);
resize();

function setStatus(text, ok) {
  stText.textContent = text;
  statusEl.classList.toggle('ok', !!ok);
}

function worldToRadar(x, y, bounds, size) {
  const span = 1024 * bounds.scale;
  const rx = (x - bounds.x) / span * size;
  const ry = (bounds.y - y) / span * size;
  return [rx, ry];
}

function teamColor(team) {
  if (team === 'T')  return '#ffbb3d';    // yellow
  if (team === 'CT') return '#59a6ff';    // blue
  return '#888';
}

function render() {
  const size = canvas.width;
  ctx.clearRect(0, 0, size, size);

  if (!state || !state.map) {
    ctx.fillStyle = '#5a5a63';
    ctx.font = (18 * devicePixelRatio) + 'px system-ui';
    ctx.textAlign = 'center';
    ctx.fillText('waiting for map…', size/2, size/2);
    return;
  }
  const name = state.map;
  const img  = mapImages[name];
  const b    = state.bounds || mapBounds[name];
  if (!img || !b) {
    if (!(name in mapImages)) loadMap(name);
    ctx.fillStyle = '#5a5a63';
    ctx.font = (18 * devicePixelRatio) + 'px system-ui';
    ctx.textAlign = 'center';
    ctx.fillText(name, size/2, size/2);
    return;
  }

  ctx.drawImage(img, 0, 0, size, size);

  const dotR = 6 * devicePixelRatio;
  const aimLen = 24 * devicePixelRatio;

  // Players — pure spectator: no "local" indicator, no friendly/enemy. Dead
  // players (lifeState !== 0 or hp <= 0) are skipped. The dot order is the
  // server's roster order, never reshuffled so you can't infer the sharer
  // from "which dot is always alive first".
  for (const p of state.players || []) {
    if (p.hp <= 0) continue;
    const [sx, sy] = worldToRadar(p.pos[0], p.pos[1], b, size);
    const col = teamColor(p.team);

    // Aim line
    const yaw = (p.yaw || 0) * Math.PI / 180;
    ctx.strokeStyle = 'rgba(255,255,255,0.9)';
    ctx.lineWidth = 2.2 * devicePixelRatio;
    ctx.beginPath();
    ctx.moveTo(sx, sy);
    ctx.lineTo(sx + Math.cos(yaw) * aimLen, sy - Math.sin(yaw) * aimLen);
    ctx.stroke();

    // Dot
    ctx.fillStyle = '#000';
    ctx.beginPath(); ctx.arc(sx, sy, dotR + 1.5*devicePixelRatio, 0, 7); ctx.fill();
    ctx.fillStyle = col;
    ctx.beginPath(); ctx.arc(sx, sy, dotR, 0, 7); ctx.fill();

    if (p.defusing) {
      ctx.strokeStyle = 'rgba(255,150,20,1)';
      ctx.lineWidth = 2.2 * devicePixelRatio;
      ctx.beginPath(); ctx.arc(sx, sy, dotR + 4*devicePixelRatio, 0, 7); ctx.stroke();
    }

    // Health bar
    if (p.hp < 100) {
      const w = 22 * devicePixelRatio, h = 3 * devicePixelRatio;
      ctx.fillStyle = '#000';
      ctx.fillRect(sx - w/2, sy + dotR + 4*devicePixelRatio, w, h);
      ctx.fillStyle = `rgb(${255-Math.round(p.hp*2.55)},${Math.round(p.hp*2.55)},0)`;
      ctx.fillRect(sx - w/2, sy + dotR + 4*devicePixelRatio, w * p.hp/100, h);
    }
  }

  // Bomb
  const bomb = state.bomb;
  if (bomb && bomb.state === 'planted' && bomb.pos) {
    const [bx, by] = worldToRadar(bomb.pos[0], bomb.pos[1], b, size);
    ctx.fillStyle = '#000';
    ctx.beginPath(); ctx.arc(bx, by, dotR + 1.5*devicePixelRatio, 0, 7); ctx.fill();
    ctx.fillStyle = '#ffc800';
    ctx.beginPath(); ctx.arc(bx, by, dotR, 0, 7); ctx.fill();

    const t = Math.max(0, bomb.time || 0);
    ctx.fillStyle = t > 10 ? '#ffe566' : '#ff5555';
    ctx.font = `bold ${16*devicePixelRatio}px system-ui`;
    ctx.textAlign = 'center';
    ctx.fillText(`${t.toFixed(1)}s`, size/2, 24*devicePixelRatio);
    ctx.fillStyle = '#e6e6ec';
    ctx.font = `${11*devicePixelRatio}px system-ui`;
    ctx.fillText(bomb.site ? `PLANTED ${bomb.site}` : 'PLANTED', size/2, 42*devicePixelRatio);
  }
}

function tick() {
  render();
  requestAnimationFrame(tick);
}
requestAnimationFrame(tick);

// ── WebSocket ────────────────────────────────────────────────────────────
let ws = null;
let backoff = 500;
function connect() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${proto}//${location.host}/ws`);
  ws.onopen = () => { setStatus('connected', true); backoff = 500; };
  ws.onmessage = (ev) => {
    try {
      state = JSON.parse(ev.data);
      if (state && state.map && state.map !== currentMap) {
        currentMap = state.map;
        loadMap(currentMap);
      }
    } catch (_) {}
  };
  ws.onclose = () => {
    setStatus('disconnected', false);
    setTimeout(connect, backoff);
    backoff = Math.min(backoff * 2, 5000);
  };
  ws.onerror = () => { try { ws.close(); } catch (_) {} };
}
connect();
</script>
</body>
</html>
)HTML";
