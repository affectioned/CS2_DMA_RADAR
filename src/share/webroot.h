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
//
// Visual language borrows from clauadv/cs2_webradar: teal/blue palette,
// radial gradient bg, teardrop view-pointer dots, soft typography.
inline constexpr const char* kWebRootHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>CS2 Shared Radar</title>
<style>
  :root {
    color-scheme: dark;
    --bg-core:   #07141e;
    --bg-mid:    #0d2434;
    --bg-high:   #142837;
    --txt:       #b1d0e7;
    --txt-dim:   #6492b4;
    --line:      rgba(177,208,231,0.10);
    --line-soft: rgba(177,208,231,0.05);
    --t-color:   #ffbb3d;
    --ct-color: #59a6ff;
    --red:       #c90b0b;
    --green:     #50904c;
  }
  * { box-sizing: border-box; }
  html, body {
    margin: 0; padding: 0; height: 100%;
    background:
      radial-gradient(50% 50% at 50% 50%, rgba(20,40,55,0.95) 0%, rgba(7,20,30,0.98) 100%);
    color: var(--txt);
    font: 13px/1.4 "TASAOrbiter", "Inter", "SF Pro Display", -apple-system, "Segoe UI", system-ui, sans-serif;
    font-feature-settings: "tnum", "ss01";
    letter-spacing: 0.2px;
    overflow: hidden; touch-action: none;
    -webkit-font-smoothing: antialiased;
    backdrop-filter: blur(7.5px);
  }
  #root {
    position: fixed; inset: 0;
    display: flex; align-items: center; justify-content: center;
    padding: 28px;
  }
  #frame {
    position: relative;
    border-radius: 2px;
  }
  canvas {
    display: block;
    background: transparent;
    image-rendering: -webkit-optimize-contrast;
    position: relative; z-index: 0;
  }

  /* HUD chrome — no boxes, just light text + a subtle rule */
  .hud {
    position: fixed; z-index: 20;
    color: var(--txt-dim);
    font-size: 11px;
    letter-spacing: 1.4px;
    text-transform: uppercase;
    font-weight: 500;
    user-select: none;
  }
  #status  { top: 20px; left: 24px; display: flex; align-items: center; gap: 9px; }
  #map-pill{ top: 20px; right: 24px; display: none; align-items: center; gap: 10px; }
  #map-pill.visible { display: inline-flex; }
  #banner  { bottom: 20px; left: 50%; transform: translateX(-50%); font-size: 10px; letter-spacing: 2px; }

  #status .dot {
    width: 6px; height: 6px; border-radius: 50%;
    background: var(--red);
    box-shadow: 0 0 10px rgba(201,11,11,0.55);
    animation: pulse-bad 1.6s ease-out infinite;
  }
  #status.ok .dot {
    background: var(--green);
    box-shadow: 0 0 10px rgba(80,144,76,0.55);
    animation: pulse-good 2.2s ease-out infinite;
  }
  #status.ok #st-text { color: var(--txt); }
  @keyframes pulse-bad {
    0%   { box-shadow: 0 0 0 0 rgba(201,11,11,0.5); }
    70%  { box-shadow: 0 0 0 7px rgba(201,11,11,0); }
    100% { box-shadow: 0 0 0 0 rgba(201,11,11,0); }
  }
  @keyframes pulse-good {
    0%   { box-shadow: 0 0 0 0 rgba(80,144,76,0.45); }
    70%  { box-shadow: 0 0 0 6px rgba(80,144,76,0); }
    100% { box-shadow: 0 0 0 0 rgba(80,144,76,0); }
  }

  #map-pill .sep { width: 1px; height: 10px; background: var(--line); display: inline-block; }
  #map-pill .name { color: var(--txt); font-weight: 600; letter-spacing: 1.6px; }

  /* Team counters — stacked on each side of the radar */
  #teams-t, #teams-ct {
    position: fixed; z-index: 20;
    top: 50%; transform: translateY(-50%);
    display: flex; flex-direction: column; align-items: center; gap: 4px;
  }
  #teams-t  { left: 24px; }
  #teams-ct { right: 24px; }
  .team-label {
    font-size: 9.5px; letter-spacing: 2.4px; color: var(--txt-dim);
    text-transform: uppercase; font-weight: 500;
  }
  .team-count {
    font-size: 34px; font-weight: 300; letter-spacing: 0;
    line-height: 1;
    font-variant-numeric: tabular-nums;
  }
  #teams-t  .team-count { color: var(--t-color); text-shadow: 0 0 20px rgba(255,187,61,0.25); }
  #teams-ct .team-count { color: var(--ct-color); text-shadow: 0 0 20px rgba(89,166,255,0.25); }
</style>
</head>
<body>
<div id="status" class="hud"><span class="dot"></span><span id="st-text">connecting</span></div>
<div id="map-pill" class="hud"><span>map</span><span class="sep"></span><span class="name" id="map-name">—</span></div>
<div id="root"><div id="frame"><canvas id="c"></canvas></div></div>

<div id="teams-t">
  <div class="team-label">T</div>
  <div class="team-count" id="t-count">0</div>
</div>
<div id="teams-ct">
  <div class="team-label">CT</div>
  <div class="team-count" id="ct-count">0</div>
</div>

<div id="banner" class="hud">spectator view</div>

<script>
'use strict';

const stText    = document.getElementById('st-text');
const statusEl  = document.getElementById('status');
const canvas    = document.getElementById('c');
const ctx       = canvas.getContext('2d');
const mapPill   = document.getElementById('map-pill');
const mapNameEl = document.getElementById('map-name');
const tCountEl  = document.getElementById('t-count');
const ctCountEl = document.getElementById('ct-count');

let state = null;
const mapImages = Object.create(null);
const mapBounds = Object.create(null);
let currentMap = null;

// Smoothed rotation per dot slot — same shortest-arc interpolation the
// reference project uses, so dots never "flip" through 360°.
const smoothedYaw = Object.create(null);
function smoothYaw(idx, target) {
  if (smoothedYaw[idx] === undefined) { smoothedYaw[idx] = target; return target; }
  let cur = smoothedYaw[idx];
  let diff = ((target - cur + 540) % 360) - 180;
  cur += diff * 0.35;
  smoothedYaw[idx] = cur;
  return cur;
}

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
  const pad = 80;
  const d = Math.max(240, Math.min(window.innerWidth, window.innerHeight) - pad);
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

function prettyMap(name) {
  if (!name) return '—';
  return name.replace(/^de_|^cs_|^ar_/,'').replace(/_/g,' ');
}

function worldToRadar(x, y, bounds, size) {
  const span = 1024 * bounds.scale;
  const rx = (x - bounds.x) / span * size;
  const ry = (bounds.y - y) / span * size;
  return [rx, ry];
}

function teamColor(team) {
  if (team === 'T')  return '#ffbb3d';
  if (team === 'CT') return '#59a6ff';
  return '#888';
}

function roundRect(x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

// Teardrop/arrow dot: round body with a forward point in view direction.
// Pure canvas analogue of the reference's rounded-[50%_50%_50%_0%] shape
// rotated to the player's yaw.
function drawPointer(x, y, yaw, r, fill, stroke) {
  ctx.save();
  ctx.translate(x, y);
  ctx.rotate(-yaw);  // world +cos(yaw)/-sin(yaw) → canvas pointing +x locally
  ctx.beginPath();
  // Start at top of circle (local -y), curve out to the forward tip (local +x)
  ctx.moveTo(0, -r);
  ctx.quadraticCurveTo(r * 0.55, -r * 0.55, r * 1.55, 0);
  ctx.quadraticCurveTo(r * 0.55,  r * 0.55, 0,  r);
  // Back half = clean circular arc
  ctx.arc(0, 0, r, Math.PI / 2, -Math.PI / 2, false);
  ctx.closePath();
  if (stroke) {
    ctx.lineWidth = 1.4 * devicePixelRatio;
    ctx.strokeStyle = stroke;
    ctx.stroke();
  }
  ctx.fillStyle = fill;
  ctx.fill();
  ctx.restore();
}

function drawDeadMarker(x, y, r, color) {
  ctx.save();
  ctx.globalAlpha = 0.45;
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.6 * devicePixelRatio;
  ctx.lineCap = 'round';
  const d = r * 0.9;
  ctx.beginPath();
  ctx.moveTo(x - d, y - d); ctx.lineTo(x + d, y + d);
  ctx.moveTo(x + d, y - d); ctx.lineTo(x - d, y + d);
  ctx.stroke();
  ctx.restore();
}

function render() {
  const size = canvas.width;
  ctx.clearRect(0, 0, size, size);

  if (!state || !state.map) {
    ctx.fillStyle = '#4a6a82';
    ctx.font = `500 ${12 * devicePixelRatio}px "TASAOrbiter", "Inter", system-ui`;
    ctx.textAlign = 'center';
    ctx.fillText('WAITING FOR MAP', size/2, size/2);
    return;
  }
  const name = state.map;
  const img  = mapImages[name];
  const b    = state.bounds || mapBounds[name];
  if (!img || !b) {
    if (!(name in mapImages)) loadMap(name);
    ctx.fillStyle = '#4a6a82';
    ctx.font = `500 ${12 * devicePixelRatio}px "TASAOrbiter", "Inter", system-ui`;
    ctx.textAlign = 'center';
    ctx.fillText('LOADING ' + prettyMap(name).toUpperCase(), size/2, size/2);
    return;
  }

  // Map atlas with a slight blue wash — ties the texture into the chrome
  ctx.save();
  ctx.filter = 'brightness(0.88) contrast(1.05) saturate(0.85)';
  ctx.drawImage(img, 0, 0, size, size);
  ctx.restore();
  ctx.fillStyle = 'rgba(100,146,180,0.08)';
  ctx.fillRect(0, 0, size, size);

  // Soft vignette for depth
  const vg = ctx.createRadialGradient(size/2, size/2, size*0.35, size/2, size/2, size*0.75);
  vg.addColorStop(0, 'rgba(0,0,0,0)');
  vg.addColorStop(1, 'rgba(0,0,0,0.40)');
  ctx.fillStyle = vg;
  ctx.fillRect(0, 0, size, size);

  const dotR = 7 * devicePixelRatio;
  let tAlive = 0, ctAlive = 0;
  const players = state.players || [];

  // Dead markers first (behind alive dots)
  players.forEach((p, i) => {
    if (p.hp > 0) return;
    if (!p.pos) return;
    const [sx, sy] = worldToRadar(p.pos[0], p.pos[1], b, size);
    drawDeadMarker(sx, sy, dotR, teamColor(p.team));
  });

  // Alive players
  players.forEach((p, i) => {
    if (p.hp <= 0) return;
    if (p.team === 'T')  tAlive++;
    if (p.team === 'CT') ctAlive++;

    const [sx, sy] = worldToRadar(p.pos[0], p.pos[1], b, size);
    const col = teamColor(p.team);
    const yawDeg = p.yaw || 0;
    const yaw = smoothYaw(i, yawDeg) * Math.PI / 180;

    // Soft glow halo under the pointer
    ctx.save();
    ctx.shadowColor = col;
    ctx.shadowBlur = 16 * devicePixelRatio;
    drawPointer(sx, sy, yaw, dotR, col, null);
    ctx.restore();

    // Crisp outline on top, slight dark rim for contrast on bright maps
    drawPointer(sx, sy, yaw, dotR, col, 'rgba(7,20,30,0.75)');

    // Inner highlight bead at the back of the drop
    ctx.save();
    ctx.translate(sx, sy); ctx.rotate(-yaw);
    ctx.fillStyle = 'rgba(255,255,255,0.35)';
    ctx.beginPath(); ctx.arc(-dotR*0.25, -dotR*0.25, dotR*0.28, 0, Math.PI*2); ctx.fill();
    ctx.restore();

    if (p.defusing) {
      const t = performance.now() / 1000;
      const pulse = 0.65 + 0.35 * Math.sin(t * 4);
      ctx.strokeStyle = `rgba(80,144,76,${pulse})`;
      ctx.lineWidth = 2.2 * devicePixelRatio;
      ctx.beginPath(); ctx.arc(sx, sy, dotR + 6*devicePixelRatio, 0, Math.PI*2); ctx.stroke();
    }

    // Health bar — thin rounded pill under the dot
    if (p.hp < 100) {
      const w = 24 * devicePixelRatio, h = 3 * devicePixelRatio;
      const bx = sx - w/2, by = sy + dotR + 5*devicePixelRatio;
      ctx.fillStyle = 'rgba(7,20,30,0.75)';
      roundRect(bx - 1, by - 1, w + 2, h + 2, (h + 2)/2); ctx.fill();
      const hp = Math.max(0, Math.min(100, p.hp));
      ctx.fillStyle = `hsl(${hp * 1.2}, 85%, 55%)`;
      roundRect(bx, by, Math.max(h, w * hp/100), h, h/2); ctx.fill();
    }
  });

  tCountEl.textContent  = tAlive;
  ctCountEl.textContent = ctAlive;

  // Bomb — dropped
  const bomb = state.bomb;
  if (bomb && bomb.state === 'dropped' && bomb.pos) {
    const [bx, by] = worldToRadar(bomb.pos[0], bomb.pos[1], b, size);
    const t = performance.now() / 1000;
    const pulse = 0.55 + 0.45 * Math.sin(t * 2.5);
    ctx.save();
    ctx.shadowColor = '#ffd200';
    ctx.shadowBlur = 12 * devicePixelRatio;
    ctx.fillStyle = '#d4d4d4';
    ctx.beginPath(); ctx.arc(bx, by, dotR * 0.8, 0, Math.PI*2); ctx.fill();
    ctx.restore();
    ctx.strokeStyle = `rgba(255,210,0,${pulse})`;
    ctx.lineWidth = 1.8 * devicePixelRatio;
    ctx.beginPath(); ctx.arc(bx, by, dotR + 4*devicePixelRatio, 0, Math.PI*2); ctx.stroke();
  }

  // Bomb — planted
  if (bomb && bomb.state === 'planted' && bomb.pos) {
    const [bx, by] = worldToRadar(bomb.pos[0], bomb.pos[1], b, size);
    const t = performance.now() / 1000;
    const pulse = 0.5 + 0.5 * Math.sin(t * 6);
    const secs = Math.max(0, bomb.time || 0);
    const danger = secs <= 10;
    const mark = danger ? '#ff3b3b' : '#ffc23d';

    ctx.save();
    ctx.shadowColor = mark;
    ctx.shadowBlur = 20 * devicePixelRatio;
    ctx.fillStyle = mark;
    ctx.beginPath(); ctx.arc(bx, by, dotR * 0.9, 0, Math.PI*2); ctx.fill();
    ctx.restore();
    ctx.strokeStyle = `${danger ? 'rgba(255,59,59,' : 'rgba(255,194,61,'}${pulse})`;
    ctx.lineWidth = 2 * devicePixelRatio;
    ctx.beginPath(); ctx.arc(bx, by, dotR + 6*devicePixelRatio, 0, Math.PI*2); ctx.stroke();

    // Timer — large tabular numerals centred at top, no card
    ctx.textAlign = 'center';
    ctx.fillStyle = danger ? '#ff6b6b' : '#ffd97a';
    ctx.font = `300 ${34*devicePixelRatio}px "TASAOrbiter", "Inter", system-ui`;
    ctx.shadowColor = 'rgba(0,0,0,0.6)';
    ctx.shadowBlur = 10 * devicePixelRatio;
    ctx.fillText(`${secs.toFixed(1)}s`, size/2, 38*devicePixelRatio);
    ctx.shadowBlur = 0;
    ctx.fillStyle = 'rgba(177,208,231,0.6)';
    ctx.font = `500 ${9.5*devicePixelRatio}px "TASAOrbiter", "Inter", system-ui`;
    ctx.fillText(bomb.site ? `C4 PLANTED  ·  SITE ${bomb.site}` : 'C4 PLANTED', size/2, 54*devicePixelRatio);
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
  ws.onopen = () => { setStatus('live', true); backoff = 500; };
  ws.onmessage = (ev) => {
    try {
      state = JSON.parse(ev.data);
      if (state && state.map && state.map !== currentMap) {
        currentMap = state.map;
        mapNameEl.textContent = prettyMap(currentMap).toUpperCase();
        mapPill.classList.add('visible');
        loadMap(currentMap);
      }
    } catch (_) {}
  };
  ws.onclose = () => {
    setStatus('offline', false);
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
