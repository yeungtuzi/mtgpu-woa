const $ = (id) => document.getElementById(id);

const state = {
  status: null,
  settings: { width: 1920, height: 1080, fps: 60, bitrate: 20000, app: "Desktop" },
  apps: [],
  encoderHistory: new Array(60).fill(0),
  busy: false,
};

const els = {
  pillVm: $("pill-vm"),
  pillSun: $("pill-sun"),
  pillClip: $("pill-clip"),
  pillEnc: $("pill-enc"),
  ring: $("ring-fg"),
  vpuVal: $("vpu-val"),
  spark: $("spark"),
  statRes: $("stat-res"),
  statFps: $("stat-fps"),
  statBitrate: $("stat-bitrate"),
  appGrid: $("app-grid"),
  appsCount: $("apps-count"),
  btnStream: $("btn-stream"),
  btnStreamLabel: $("btn-stream-label"),
  btnStop: $("btn-stop"),
  btnVm: $("btn-vm"),
  drawer: $("drawer"),
  toast: $("toast"),
};

const RING_LEN = 2 * Math.PI * 52;
// file:// mode (used by the desktop launcher on this host) renders the UI from
// disk and talks to the FastAPI backend over HTTP; normal http:// serving uses
// same-origin relative requests.
const API_BASE = location.protocol === "file:" ? "http://127.0.0.1:8760" : "";

async function api(path, opts = {}) {
  const res = await fetch(API_BASE + path, {
    headers: { "Content-Type": "application/json" },
    ...opts,
  });
  if (!res.ok) {
    let detail = "";
    try { detail = (await res.json()).error || ""; } catch (_) {}
    throw new Error(detail || `${res.status} ${res.statusText}`);
  }
  return res.json();
}

function toast(msg, isError = false) {
  els.toast.textContent = msg;
  els.toast.classList.toggle("err", isError);
  els.toast.classList.add("show");
  clearTimeout(toast._t);
  toast._t = setTimeout(() => els.toast.classList.remove("show"), 2600);
}

function setPill(el, kind, text) {
  el.classList.remove("ok", "warn", "bad");
  if (kind) el.classList.add(kind);
  el.querySelector("b").textContent = text;
}

function fmtRes(settings) {
  return `${settings.width}×${settings.height}`;
}

function renderStatus() {
  const s = state.status;
  if (!s) return;
  setPill(els.pillVm, s.vm.running ? "ok" : "bad", s.vm.running ? "运行中" : "已停止");
  setPill(els.pillSun, s.sunshine.reachable ? "ok" : "bad", s.sunshine.reachable ? "在线" : "离线");
  setPill(els.pillClip, s.clipboard.ok ? "ok" : "warn", s.clipboard.ok ? "同步" : "关闭");
  let encKind = "", encText = "待机";
  if (s.stream.running) {
    if (s.vpu.encoder > 0.5) { encKind = "ok"; encText = "MT VPU"; }
    else { encKind = "warn"; encText = "软件"; }
  }
  setPill(els.pillEnc, encKind, encText);

  const pct = Math.max(0, Math.min(100, s.vpu.encoder || 0));
  els.vpuVal.textContent = `${pct.toFixed(1)}%`;
  els.ring.style.strokeDashoffset = String(RING_LEN * (1 - pct / 100));

  state.encoderHistory.push(pct);
  if (state.encoderHistory.length > 60) state.encoderHistory.shift();
  drawSpark();

  const st = s.settings || state.settings;
  state.settings = st;
  els.statRes.textContent = fmtRes(st);
  els.statFps.textContent = String(st.fps);
  els.statBitrate.textContent = `${Math.round(st.bitrate / 1000)} Mbps`;
  const encStat = $("stat-enc");
  if (encStat) encStat.textContent = (!s.stream.running) ? "待机" : (s.vpu.encoder > 0.5 ? "MT VPU" : "软件");
  const sess = $("session-line");
  if (sess) sess.textContent = s.stream.running
    ? `会话：${s.stream.app || "Desktop"} · Moonlight V+ · 已连接 · ${s.vpu.encoder.toFixed(1)}% VPU`
    : "会话：未连接 · 选择应用或点击「开始串流」";

  const running = s.stream.running;
  els.btnStreamLabel.textContent = running ? "停止串流" : "开始串流";
  els.btnStream.disabled = false;
  els.btnVm.textContent = s.vm.running ? "停止虚拟机" : "启动虚拟机";
}

function drawSpark() {
  const canvas = els.spark;
  const ctx = canvas.getContext("2d");
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  ctx.strokeStyle = "rgba(255,255,255,0.06)";
  ctx.lineWidth = 1;
  for (let i = 1; i < 4; i++) {
    const y = (h / 4) * i;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  const data = state.encoderHistory;
  const step = w / (data.length - 1);
  const max = Math.max(30, ...data) * 1.25;
  const pts = data.map((v, i) => [i * step, h - (v / max) * (h - 10) - 5]);

  const grad = ctx.createLinearGradient(0, 0, 0, h);
  grad.addColorStop(0, "rgba(94,234,212,0.35)");
  grad.addColorStop(1, "rgba(94,234,212,0.0)");

  ctx.beginPath();
  ctx.moveTo(pts[0][0], h);
  for (const [x, y] of pts) ctx.lineTo(x, y);
  ctx.lineTo(pts[pts.length - 1][0], h);
  ctx.closePath();
  ctx.fillStyle = grad;
  ctx.fill();

  ctx.beginPath();
  pts.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
  ctx.strokeStyle = "#5eead4";
  ctx.lineWidth = 2;
  ctx.lineJoin = "round";
  ctx.shadowColor = "rgba(94,234,212,0.7)";
  ctx.shadowBlur = 10;
  ctx.stroke();
  ctx.shadowBlur = 0;

  const [lx, ly] = pts[pts.length - 1];
  ctx.beginPath();
  ctx.arc(lx, ly, 3.5, 0, Math.PI * 2);
  ctx.fillStyle = "#a7f3d0";
  ctx.fill();
}

function iconFor(name) {
  const n = name.toLowerCase();
  if (n.includes("steam")) return { icon: "🎮", glow: "rgba(96,165,250,0.30)" };
  if (n.includes("desktop") || n.includes("桌面")) return { icon: "🖥️", glow: "rgba(94,234,212,0.30)" };
  return { icon: "🪟", glow: "rgba(167,139,250,0.28)" };
}

function renderApps() {
  els.appGrid.innerHTML = "";
  els.appsCount.textContent = `${state.apps.length} 个`;
  state.apps.forEach((app, i) => {
    const { icon, glow } = iconFor(app.name);
    const card = document.createElement("div");
    card.className = "app-card";
    card.tabIndex = 0;
    card.style.setProperty("--glow", glow);
    card.style.animationDelay = `${i * 60}ms`;
    card.innerHTML = `
      <div class="app-play">▶</div>
      <div class="app-icon">${icon}</div>
      <div>
        <div class="app-name">${app.name}</div>
        <div class="app-meta">Moonlight 串流 · ${fmtRes(state.settings)}</div>
      </div>`;
    card.addEventListener("click", () => startStream(app.name));
    card.addEventListener("keydown", (e) => {
      if (e.key === "Enter") startStream(app.name);
      if (e.key === "ArrowRight") focusCard(i + 1);
      if (e.key === "ArrowLeft") focusCard(i - 1);
    });
    els.appGrid.appendChild(card);
  });
}

function focusCard(i) {
  const cards = [...els.appGrid.querySelectorAll(".app-card")];
  if (!cards.length) return;
  const idx = (i + cards.length) % cards.length;
  cards[idx].focus();
}

async function loadApps() {
  try {
    state.apps = await api("/api/apps");
  } catch (e) {
    state.apps = [{ name: "Desktop" }, { name: "Steam Big Picture" }];
  }
  renderApps();
}

async function refresh() {
  try {
    const s = await api("/api/status");
    state.status = s;
    state.settings = s.settings;
    renderStatus();
  } catch (e) {
    console.error(e);
  }
}

async function startStream(appName) {
  if (state.busy) return;
  if (state.status?.stream.running) {
    toast("已有串流在运行");
    return;
  }
  state.busy = true;
  try {
    const st = state.settings;
    await api("/api/stream", {
      method: "POST",
      body: JSON.stringify({ app: appName || st.app, ...pick(st) }),
    });
    toast(`正在串流：${appName || st.app}`);
    setTimeout(refresh, 1200);
  } catch (e) {
    toast(e.message || "启动失败", true);
  } finally {
    state.busy = false;
  }
}

async function stopStream() {
  try {
    await api("/api/stream/stop", { method: "POST", body: "{}" });
    toast("已停止串流");
    setTimeout(refresh, 800);
  } catch (e) {
    toast(e.message || "停止失败", true);
  }
}

function pick(s) {
  return { width: s.width, height: s.height, fps: s.fps, bitrate: s.bitrate };
}

async function toggleVm() {
  const running = state.status?.vm.running;
  try {
    toast(running ? "正在停止虚拟机…" : "正在启动虚拟机…");
    const r = await api("/api/vm", {
      method: "POST",
      body: JSON.stringify({ action: running ? "stop" : "start" }),
    });
    toast(r.ok ? "操作成功" : (r.error || "操作失败"), !r.ok);
    setTimeout(refresh, 1500);
  } catch (e) {
    toast(e.message || "操作失败", true);
  }
}

function openDrawer() {
  els.drawer.classList.add("open");
  const s = state.settings;
  $("sel-res").value = `${s.width}x${s.height}`;
  $("sel-fps").value = String(s.fps);
  $("rng-bitrate").value = String(Math.round(s.bitrate / 1000));
  $("bitrate-val").textContent = String(Math.round(s.bitrate / 1000));
}

function closeDrawer() {
  els.drawer.classList.remove("open");
}

async function saveSettings() {
  const [w, h] = $("sel-res").value.split("x").map(Number);
  const fps = Number($("sel-fps").value);
  const bitrate = Number($("rng-bitrate").value) * 1000;
  try {
    state.settings = await api("/api/settings", {
      method: "PUT",
      body: JSON.stringify({ width: w, height: h, fps, bitrate }),
    });
    toast("设置已保存");
    closeDrawer();
    refresh();
  } catch (e) {
    toast(e.message || "保存失败", true);
  }
}

function bind() {
  els.btnStream.addEventListener("click", () => {
    if (state.status?.stream.running) stopStream();
    else startStream(state.settings.app);
  });
  els.btnStop.addEventListener("click", stopStream);
  els.btnVm.addEventListener("click", toggleVm);
  $("btn-settings").addEventListener("click", openDrawer);
  $("btn-close").addEventListener("click", closeDrawer);
  $("btn-save").addEventListener("click", saveSettings);
  $("rng-bitrate").addEventListener("input", (e) => {
    $("bitrate-val").textContent = e.target.value;
  });
  $("link-refresh").addEventListener("click", (e) => {
    e.preventDefault();
    refresh();
    loadApps();
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") closeDrawer();
  });
}

async function main() {
  bind();
  await refresh();
  await loadApps();
  setInterval(refresh, 2000);
}

main();
