/*
 * Project : Escape room corridor audio device
 * File    : WebPage.h - the single-page webapp served by the Master (Phase 5)
 *
 * One self-contained HTML document (inline CSS and JS, no external resources: the AP has no
 * internet). Kept in flash (PROGMEM) and sent with WebServer::send_P().
 * UI text is in Portuguese (pt-PT); code comments are in English.
 *
 * API used (all JSON): GET /api/status, GET /api/calib, GET /api/log?since=N,
 *                      POST /api/params, POST /api/audio
 */
#pragma once

#include <Arduino.h>

const char WEB_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="pt-PT">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Disparador Áudio</title>
<style>
:root{--bg:#10151c;--card:#1a222d;--line:#2b3644;--txt:#e6edf3;--dim:#8b98a8;--ok:#3fb950;--warn:#d29922;--err:#f85149;--acc:#58a6ff}
*{box-sizing:border-box}
body{margin:0;font:16px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--txt)}
header{display:flex;align-items:center;justify-content:space-between;padding:10px 14px;background:var(--card);border-bottom:1px solid var(--line)}
header h1{font-size:1rem;margin:0}
#conn{font-size:.85rem;color:var(--dim)}
#conn::before{content:"";display:inline-block;width:.7em;height:.7em;border-radius:50%;margin-right:.4em;background:var(--err)}
#conn.on::before{background:var(--ok)}
nav{display:flex;border-bottom:1px solid var(--line)}
nav button{flex:1;padding:12px 4px;background:none;border:0;border-bottom:3px solid transparent;color:var(--dim);font:inherit;cursor:pointer}
nav button.sel{color:var(--txt);border-bottom-color:var(--acc)}
main{padding:12px;max-width:760px;margin:0 auto}
section{display:none}
section.sel{display:block}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px;margin-bottom:12px}
.card h2{font-size:.95rem;margin:0 0 8px;color:var(--dim);font-weight:600}
.row{display:flex;justify-content:space-between;gap:8px;padding:3px 0;border-bottom:1px dashed var(--line)}
.row:last-child{border-bottom:0}
.row span:first-child{color:var(--dim)}
label{display:block;margin:10px 0 2px}
label small{color:var(--dim)}
input[type=number]{width:100%;padding:10px;background:var(--bg);color:var(--txt);border:1px solid var(--line);border-radius:6px;font:inherit}
button.act{padding:10px 16px;border:0;border-radius:6px;background:var(--acc);color:#04111f;font:inherit;font-weight:600;cursor:pointer;margin:8px 8px 0 0}
button.act.red{background:var(--err);color:#fff}
button.act:disabled{opacity:.4}
.msg{margin-top:8px;min-height:1.2em}
.msg.err{color:var(--err)}.msg.ok{color:var(--ok)}
.err-item{padding:6px 8px;margin-bottom:6px;border-radius:6px;background:#3a1618;border-left:4px solid var(--err)}
.err-item.warn{background:#35290b;border-left-color:var(--warn)}
.err-item.info{background:#102a43;border-left-color:var(--acc)}
.none{color:var(--ok)}
.big{font-size:2.6rem;font-weight:700;text-align:center}
.big small{font-size:1rem;color:var(--dim);font-weight:400}
.bar{position:relative;height:22px;background:var(--bg);border:1px solid var(--line);border-radius:6px;margin:10px 0;overflow:hidden}
.fill{position:absolute;left:0;top:0;bottom:0;background:var(--acc);width:0}
.fill.occ{background:var(--err)}
.thr{position:absolute;top:0;bottom:0;width:2px;background:var(--warn)}
pre{margin:0;max-height:320px;overflow:auto;font:12px/1.35 ui-monospace,monospace;white-space:pre-wrap;word-break:break-word}
</style>
</head>
<body>
<header><h1>Disparador Áudio</h1><span id="conn">sem ligação</span></header>
<nav>
<button data-tab="set" class="sel">Ajustes</button>
<button data-tab="cal">Calibração</button>
<button data-tab="dbg">Debugging</button>
</nav>
<main>

<section id="set" class="sel">
<div class="card">
<h2>Parâmetros</h2>
<form id="form">
<label>Volume <small id="l_volume"></small><input type="number" id="volume" step="1"></label>
<label>Duração do fade-out (s) <small id="l_fade"></small><input type="number" id="fadeOut" step="0.1"></label>
<label>Janela de silêncio (s) <small id="l_sil"></small><input type="number" id="silence" step="1"></label>
<label>Espera máxima (s) <small id="l_max"></small><input type="number" id="maxWait" step="1"></label>
<label>Largura do corredor na entrada, Mestre (cm) <small id="l_wm"></small><input type="number" id="widthM" step="1"></label>
<label>Largura do corredor na saída, Remoto (cm) <small id="l_wr"></small><input type="number" id="widthR" step="1"></label>
<button class="act" type="submit">Guardar</button>
<button class="act" type="button" id="reload" style="background:var(--line);color:var(--txt)">Repor valores guardados</button>
<div class="msg" id="setMsg"></div>
<div class="msg" id="remCfg"></div>
</form>
</div>
</section>

<section id="cal">
<div class="card"><h2>Entrada (Mestre)</h2>
<div class="big" id="mVal">-<small> cm</small></div>
<div class="bar"><div class="fill" id="mFill"></div><div class="thr" id="mThr"></div></div>
<div class="row"><span>Mínimo / máximo desde a última leitura</span><span id="mMM">-</span></div>
<div class="row"><span>Limiar de deteção</span><span id="mTh">-</span></div>
<div class="row"><span>Barreira</span><span id="mOcc">-</span></div>
<div class="row"><span>Passagens contadas</span><span id="mCnt">-</span></div>
</div>
<div class="card"><h2>Saída (Remoto)</h2>
<div class="big" id="rVal">-<small> cm</small></div>
<div class="bar"><div class="fill" id="rFill"></div><div class="thr" id="rThr"></div></div>
<div class="row"><span>Mínimo / máximo desde a última leitura</span><span id="rMM">-</span></div>
<div class="row"><span>Limiar de deteção</span><span id="rTh">-</span></div>
<div class="row"><span>Barreira</span><span id="rOcc">-</span></div>
<div class="row"><span>Passagens recebidas</span><span id="rCnt">-</span></div>
<div class="row"><span>Idade da leitura</span><span id="rAge">-</span></div>
</div>
<div class="card"><div class="msg" style="color:var(--dim)">Sem alvo = "-". A escala vai de 0 a 130 cm; a marca laranja é o limiar. O jogo continua ativo durante a calibração: uma passagem ainda dispara o áudio.</div></div>
</section>

<section id="dbg">
<div class="card"><h2>Erros e avisos</h2><div id="errors"></div></div>
<div class="card"><h2>Áudio</h2>
<button class="act" id="aStart">Iniciar áudio</button>
<button class="act red" id="aStop">Parar áudio</button>
<div class="msg" id="audMsg"></div>
<div class="row"><span>Cartão</span><span id="dCard">-</span></div>
<div class="row"><span>A tocar</span><span id="dPlay">-</span></div>
<div class="row"><span>Volume atual</span><span id="dVol">-</span></div>
</div>
<div class="card"><h2>Estado do jogo</h2>
<div class="row"><span>Estado</span><span id="dState">-</span></div>
<div class="row"><span>Janela de silêncio restante</span><span id="dWin">-</span></div>
<div class="row"><span>Espera máxima restante</span><span id="dMax">-</span></div>
<div class="row"><span>Failsafe (Remoto offline)</span><span id="dFail">-</span></div>
</div>
<div class="card"><h2>Ligação ao Remoto</h2>
<div class="row"><span>Ligação</span><span id="rLink">-</span></div>
<div class="row"><span>Último pacote há</span><span id="rLast">-</span></div>
<div class="row"><span>Sessão (bootId)</span><span id="rBoot">-</span></div>
<div class="row"><span>ToF do Remoto</span><span id="rTof">-</span></div>
<div class="row"><span>Eventos / duplicados / lacunas</span><span id="rEv">-</span></div>
<div class="row"><span>Heartbeats / fila cheia</span><span id="rHb">-</span></div>
<div class="row"><span>Largura no Remoto</span><span id="rCfg">-</span></div>
</div>
<div class="card"><h2>Registo de eventos</h2><pre id="log"></pre></div>
<div class="card"><h2>Ponto de acesso</h2>
<div class="row"><span>Clientes ligados</span><span id="apCl">-</span></div>
<div class="row"><span>Desliga sozinho em</span><span id="apLeft">-</span></div>
<div class="row"><span>Tempo desde o arranque</span><span id="up">-</span></div>
</div>
</section>

</main>
<script>
"use strict";
const $ = id => document.getElementById(id);
const STATE_PT = {"READY":"Pronto","PLAYING":"A tocar","WAITING":"A aguardar","FADE-OUT":"Fade-out","REARM":"Rearme"};
const ERR_PT = {
  card_missing:"Cartão de áudio em falta ou unidade sem resposta",
  card_removed:"Cartão de áudio removido",
  tof_master:"ToF da entrada (Mestre) sem dados",
  remote_offline:"Remoto sem ligação",
  tof_remote:"ToF da saída (Remoto) com falha",
  net_off:"ESP-NOW não iniciado",
  cfg_pending:"Largura do Remoto ainda por confirmar"
};
const SCALE_CM = 130;
let tab = "set", lastStatus = null, formDirty = false, formLoaded = false, logCursor = 0;

function http(method, url, body) {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 2500);
  const opt = {method, signal: ctl.signal, cache: "no-store"};
  if (body !== undefined) { opt.body = JSON.stringify(body); opt.headers = {"Content-Type": "application/json"}; }
  return fetch(url, opt).then(r => r.json().then(j => ({ok: r.ok, data: j}))).finally(() => clearTimeout(timer));
}
function setConn(on) { const c = $("conn"); c.className = on ? "on" : ""; c.textContent = on ? "ligado" : "sem ligação"; }
function fmtMs(ms) { return ms == null ? "-" : (ms / 1000).toFixed(1) + " s"; }
function fmtUp(ms) { const s = Math.floor(ms / 1000); return Math.floor(s / 3600) + " h " + Math.floor(s % 3600 / 60) + " min " + s % 60 + " s"; }
function cm(mm) { return mm == null || mm < 0 ? "-" : (mm / 10).toFixed(1); }

// ---- tabs
document.querySelectorAll("nav button").forEach(b => b.onclick = () => {
  tab = b.dataset.tab;
  document.querySelectorAll("nav button").forEach(x => x.classList.toggle("sel", x === b));
  document.querySelectorAll("section").forEach(s => s.classList.toggle("sel", s.id === tab));
});

// ---- settings
function fillForm(p, lim) {
  $("volume").value = p.volume;
  $("fadeOut").value = (p.fadeOutMs / 1000).toFixed(1);
  $("silence").value = p.silenceS;
  $("maxWait").value = p.maxWaitS;
  $("widthM").value = p.widthCm;
  $("widthR").value = p.widthRemoteCm;
  $("l_volume").textContent = "(" + lim.volume[0] + " a " + lim.volume[1] + ")";
  $("l_fade").textContent = "(" + lim.fadeOutMs[0] / 1000 + " a " + lim.fadeOutMs[1] / 1000 + ")";
  $("l_sil").textContent = "(" + lim.silenceS[0] + " a " + lim.silenceS[1] + ")";
  $("l_max").textContent = "(janela a " + lim.maxWaitS[1] + ")";
  $("l_wm").textContent = "(" + lim.widthCm[0] + " a " + lim.widthCm[1] + ")";
  $("l_wr").textContent = "(" + lim.widthCm[0] + " a " + lim.widthCm[1] + ")";
  formDirty = false;
}
document.querySelectorAll("#form input").forEach(i => i.oninput = () => { formDirty = true; });
$("reload").onclick = () => { if (lastStatus) { fillForm(lastStatus.params, lastStatus.limits); $("setMsg").textContent = ""; } };
$("form").onsubmit = e => {
  e.preventDefault();
  const msg = $("setMsg");
  const body = {
    volume: Math.round($("volume").valueAsNumber),
    fadeOutMs: Math.round($("fadeOut").valueAsNumber * 1000),
    silenceS: Math.round($("silence").valueAsNumber),
    maxWaitS: Math.round($("maxWait").valueAsNumber),
    widthCm: Math.round($("widthM").valueAsNumber),
    widthRemoteCm: Math.round($("widthR").valueAsNumber)
  };
  if (Object.values(body).some(v => !Number.isFinite(v))) { msg.className = "msg err"; msg.textContent = "Preencha todos os campos."; return; }
  http("POST", "/api/params", body).then(r => {
    if (r.ok && r.data.ok) { msg.className = "msg ok"; msg.textContent = "Guardado."; formDirty = false; pollStatus(); }
    else { msg.className = "msg err"; msg.textContent = r.data.error || "Recusado."; }
  }).catch(() => { msg.className = "msg err"; msg.textContent = "Sem ligação ao dispositivo."; });
};

// ---- status (always polled: it also tells the Master the webapp is in use)
function setRows(map) { for (const k in map) $(k).textContent = map[k]; }
function renderStatus(s) {
  lastStatus = s;
  if (!formLoaded || !formDirty) { if (!formLoaded || tab !== "set") { fillForm(s.params, s.limits); formLoaded = true; } }
  const g = s.game, a = s.audio, r = s.remote;
  setRows({
    dState: STATE_PT[g.state] || g.state,
    dWin: fmtMs(g.windowLeftMs), dMax: fmtMs(g.maxWaitLeftMs), dFail: fmtMs(g.failsafeLeftMs),
    dCard: a.card, dPlay: a.playing ? "sim" : "não", dVol: a.volume + " / 30",
    rLink: r.online ? "online" : "offline",
    rLast: r.lastRxAgeMs == null ? "nunca" : fmtMs(r.lastRxAgeMs),
    rBoot: r.bootId, rTof: r.tofFault ? "falha" : "ok",
    rEv: r.events + " / " + r.duplicates + " / " + r.gaps,
    rHb: r.heartbeats + " / " + r.queueFull,
    rCfg: r.widthWanted + " cm (" + (r.cfgConfirmed ? "confirmada" : "por confirmar") + ")",
    apCl: s.ap.clients, apLeft: fmtMs(s.ap.idleLeftMs), up: fmtUp(s.uptimeMs)
  });
  $("remCfg").textContent = r.cfgConfirmed ? "Remoto: largura confirmada." : "Remoto: largura por confirmar (offline ou a aguardar resposta).";
  $("aStart").disabled = !(g.state === "READY" && a.cardOk);
  $("aStop").disabled = !(g.state === "PLAYING" || g.state === "WAITING" || g.state === "FADE-OUT");
  const box = $("errors");
  box.textContent = "";
  if (!s.errors.length) { const d = document.createElement("div"); d.className = "none"; d.textContent = "Sem erros."; box.appendChild(d); }
  s.errors.forEach(e => {
    const d = document.createElement("div");
    d.className = "err-item " + (e.level === "error" ? "" : e.level);
    d.textContent = ERR_PT[e.code] || e.code;
    box.appendChild(d);
  });
}
function pollStatus() {
  return http("GET", "/api/status").then(r => { setConn(true); renderStatus(r.data); }).catch(() => setConn(false));
}

// ---- audio buttons
function audio(action) {
  const m = $("audMsg");
  http("POST", "/api/audio", {action}).then(r => {
    m.className = "msg " + (r.ok && r.data.ok ? "ok" : "err");
    m.textContent = r.ok && r.data.ok ? "Pedido enviado." : (r.data.error || "Recusado.");
    pollStatus();
  }).catch(() => { m.className = "msg err"; m.textContent = "Sem ligação ao dispositivo."; });
}
$("aStart").onclick = () => audio("start");
$("aStop").onclick = () => audio("stop");

// ---- calibration (fast poll, only while its tab is visible)
function renderBarrier(p, v) {
  const toPct = mm => Math.max(0, Math.min(100, mm / 10 / SCALE_CM * 100));
  $(p + "Val").firstChild.textContent = v.last >= 0 ? (v.last / 10).toFixed(1) : "-";
  $(p + "Fill").style.width = v.last >= 0 ? toPct(v.last) + "%" : "0";
  $(p + "Fill").className = "fill" + (v.occupied ? " occ" : "");
  $(p + "Thr").style.left = toPct(v.thresholdCm * 10) + "%";
  $(p + "MM").textContent = cm(v.min) + " / " + cm(v.max) + " cm";
  $(p + "Th").textContent = v.thresholdCm + " cm";
  $(p + "Occ").textContent = v.occupied ? "ocupada" : "livre";
  $(p + "Cnt").textContent = v.count;
}
let calBusy = false;
function pollCalib() {
  if (tab !== "cal" || document.hidden || calBusy) return;
  calBusy = true;
  http("GET", "/api/calib").then(r => {
    renderBarrier("m", r.data.master);
    renderBarrier("r", r.data.remote);
    $("rAge").textContent = r.data.remote.ageMs == null ? "sem leituras (Remoto offline?)" : fmtMs(r.data.remote.ageMs);
  }).catch(() => {}).finally(() => { calBusy = false; });
}

// ---- event log (incremental)
function pollLog() {
  if (tab !== "dbg" || document.hidden) return;
  http("GET", "/api/log?since=" + logCursor).then(r => {
    const d = r.data, pre = $("log");
    if (d.entries.length) {
      const atEnd = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 20;
      pre.textContent += d.entries.map(e => "[" + (e.t / 1000).toFixed(1).padStart(8) + "] " + e.m).join("\n") + "\n";
      if (atEnd) pre.scrollTop = pre.scrollHeight;
    }
    logCursor = d.last;
  }).catch(() => {});
}

pollStatus();
setInterval(pollStatus, 1000);
setInterval(pollCalib, 250);
setInterval(pollLog, 2000);
</script>
</body>
</html>
)HTML";
