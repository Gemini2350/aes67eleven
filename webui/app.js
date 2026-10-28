"use strict";

const $ = id => document.getElementById(id);
let config = {};
let status = {};
/* sender browser state - declared before showPage() may call loadSenders() */
let sendersCache = [];
let lastCurSender = null;
const sndOpen = new Set();      //expanded device groups, kept across refreshes

/* ---------- navigation ---------- */
function showPage(name) {
    if (!$("page-" + name)) return;
    document.querySelectorAll(".tab").forEach(t => t.classList.toggle("active", t.dataset.page === name));
    document.querySelectorAll(".page").forEach(p => p.classList.remove("active"));
    $("page-" + name).classList.add("active");
    history.replaceState(null, "", "#" + name);
    if (name === "settings") loadNetwork();
    if (name === "receiver") loadSenders();
    if (name === "player") loadPlayerFiles();
}
document.querySelectorAll(".tab").forEach(tab => { tab.onclick = () => showPage(tab.dataset.page); });
if (location.hash.length > 1) showPage(location.hash.slice(1));

/* ---------- helpers ---------- */
const STATE_NAMES = ["Inactive", "Healthy", "Partially healthy", "Unhealthy"];
const STATE_CLASS = ["off", "ok", "warn", "bad"];
const LINK_NAMES = {1: "All up", 2: "Some down", 3: "All down"};

function pill(el, text, cls) {
    el.textContent = text;
    el.className = "pill " + cls;
}
function meterWidth(db) {
    return Math.max(0, Math.min(100, (db + 60) / 60 * 100)) + "%";
}
function fmtNs(ns) {
    if (ns === null || ns === undefined) return "–";
    const abs = Math.abs(ns);
    if (abs > 1e9) return (ns / 1e9).toFixed(3) + " s";
    if (abs > 1e6) return (ns / 1e6).toFixed(2) + " ms";
    if (abs > 1e3) return (ns / 1e3).toFixed(1) + " µs";
    return Math.round(ns) + " ns";
}

async function api(path, method, body) {
    const r = await fetch(path, {
        method: method || "GET",
        headers: body ? {"Content-Type": "application/json"} : {},
        body: body ? JSON.stringify(body) : undefined
    });
    return r.ok ? r.json().catch(() => ({})) : Promise.reject(await r.text());
}
const setConfig = updates => api("/api/config", "POST", updates).then(loadConfig);

/* ---------- websocket status feed ---------- */
function connectWs() {
    const ws = new WebSocket((location.protocol === "https:" ? "wss://" : "ws://") + location.host + "/ws");
    ws.onopen = () => $("conn-dot").classList.add("on");
    ws.onclose = () => { $("conn-dot").classList.remove("on"); setTimeout(connectWs, 2000); };
    ws.onmessage = e => {
        const msg = JSON.parse(e.data);
        if (msg.meters && !msg.version) { applyMeters(msg.meters); return; }   //20 Hz fast path
        status = msg;
        render();
    };
}

/* selected stream on the receiver/sender pages (dashboard follows selection) */
let selRx = 0, selTx = 0;
const selRxStatus = () => (status.receivers || [])[selRx] || status.receiver || {};
const selTxStatus = () => (status.senders || [])[selTx] || status.sender || {};
const txKey = k => "senders." + selTx + "." + k;
const txCfgOf = cfg => (((cfg || {}).senders) || [])[selTx] || (cfg || {}).sender || {};

function updateBank(el, vals) {
    if (!el) return;
    if (!el.dataset.built) {
        el.dataset.built = "1";
        el.innerHTML = Array.from({length: 64}, (_, i) =>
            `<div class="bar${i && i % 8 === 0 ? " g8" : ""}" title="Kanal ${i + 1}"><i></i></div>`).join("");
    }
    const bars = el.children;
    for (let i = 0; i < 64 && i < bars.length; i++) {
        const db = vals[i] ?? -120;
        bars[i].firstElementChild.style.height = meterWidth(db);
        bars[i].classList.toggle("hot", db > -3);
    }
}
function applyMeters(m) {
    updateBank($("bank-in"), m.in64 || []);
    updateBank($("bank-out"), m.out64 || []);
    //per-stream meters: one bar per stream channel, sliced out of the 64ch
    //device banks via the stream's channel map
    updateStreamBank($("rx-meters"), selRxStatus(), m.in64 || []);
    updateStreamBank($("tx-meters"), selTxStatus(), m.out64 || []);
}

function updateStreamBank(el, stream, vals64) {
    if (!el) return;
    const map = stream.channel_map || [];
    const fp = JSON.stringify(map);
    if (el.dataset.fp !== fp) {
        el.dataset.fp = fp;
        el.innerHTML = map.map(ch =>
            `<div class="bar" title="Device channel ${ch + 1}"><i></i><b>${ch + 1}</b></div>`).join("")
            || `<span class="sub">no channels</span>`;
    }
    const bars = el.children;
    for (let i = 0; i < map.length && i < bars.length; i++) {
        if (!bars[i].firstElementChild) continue;
        const db = vals64[map[i]] ?? -120;
        bars[i].firstElementChild.style.height = meterWidth(db);
        bars[i].classList.toggle("hot", db > -3);
    }
}

/* ---------- render ---------- */
let buildSeen = null;
function render() {
    /* the kiosk browser never reloads on its own - pick up new ui builds */
    if (status.build) {
        if (buildSeen && status.build !== buildSeen) { location.reload(); return; }
        buildSeen = status.build;
    }
    const ptp = status.ptp || {};
    const rx = selRxStatus();
    const tx = selTxStatus();
    const mon = status.monitors || {};
    renderSelectors();
    renderMatrixPage();
    renderPlayers();

    /* dashboard */
    pill($("d-ptp-state"), ptp.synced ? "Locked" : (ptp.masters && ptp.masters.length ? "Syncing…" : "No master"),
         ptp.synced ? "ok" : "warn");
    const gm = (ptp.masters || []).find(m => m.selected);
    $("d-ptp-gm").textContent = gm ? gm.grandmaster : "–";
    $("d-ptp-offset").textContent = ptp.synced ? fmtNs(ptp.correction_ns) : "–";

    const nmos = status.nmos || {};
    $("d-nmos-registry").textContent = nmos.registry || "no registry";
    $("d-nmos-status").textContent = (nmos.status || "–") + " · Node " + (nmos.node_id || "").slice(0, 8) + "…";
    if ($("set-nmos-pill")) {
        if (nmos.server_up === false) {
            pill($("set-nmos-pill"), "Port in use", "bad");
            $("set-nmos-status").textContent =
                `Node API cannot bind port ${(status.config?.nmos || {}).http_port || 8080} — ` +
                `another service is using it. Pick a different node API port.`;
        } else if (nmos.status === "registered") {
            pill($("set-nmos-pill"), "Registered", "ok");
            $("set-nmos-status").textContent = "registered with " + (nmos.registry || "");
        } else if ((nmos.status || "").includes("failed")) {
            pill($("set-nmos-pill"), "Failed", "bad");
            $("set-nmos-status").textContent =
                "registration failed — registry not reachable at " +
                ((status.config?.nmos || {}).registry_override || "(discovered address)");
        } else {
            pill($("set-nmos-pill"), nmos.status || "searching…", "warn");
            $("set-nmos-status").textContent = nmos.status === "no registry found"
                ? "no registry found via DNS-SD — enter one manually" : (nmos.status || "starting…");
        }
    }
    renderDashStreams(mon);

    /* receiver page */
    const rmon = (mon.receivers || [])[selRx] || mon.receiver || {};
    pill($("rx-overall"), STATE_NAMES[rmon.overall || 0], STATE_CLASS[rmon.overall || 0]);
    const rxLegs = (rx.legs || []).map((leg, i) =>
        `${i ? "SEC" : "PRI"} ${leg.multicast} (${leg.received} rx · ${leg.lost} lost)`).join(" · ");
    $("rx-mini").textContent = rx.running
        ? `${rx.receiving ? "receiving" : "no signal"} · ${rx.session_name || ""} · ` +
          `${rx.played || 0} pkts · ${rx.concealed || 0} concealed` + (rxLegs ? " · " + rxLegs : "") +
          (rmon.overall_message ? " · " + rmon.overall_message : "")
        : "not connected — pick an NMOS sender or paste an SDP";

    /* keep the gain slider in sync with changes made from other browsers,
       unless the user is moving it right now */
    if (rx.gain_db !== undefined && Date.now() - gainTouchedAt > 1500
        && parseFloat($("rx-gain").value) !== rx.gain_db) {
        $("rx-gain").value = rx.gain_db;
        $("rx-gain-val").textContent = rx.gain_db + " dB";
    }

    drawBufferChart(rx.buffer_history || [], rx.critical_ms || 2, rx.running && rx.receiving);
    renderPatchRows();

    /* local monitor: bound to ONE receiver (monitor.stream) */
    const lmon = status.monitor || {};
    const monCfg = (status.config || {}).monitor || {};
    monitorMine = !!monCfg.enabled && (monCfg.stream || 0) === selRx;
    const monElsewhere = !!monCfg.enabled && !monitorMine;
    $("rx-monitor").textContent = "Monitor: " + (monitorMine ? (lmon.running ? "on" : "starting…")
                                  : (monElsewhere ? `RX ${(monCfg.stream || 0) + 1}` : "off"));
    $("rx-monitor").classList.toggle("primary", monitorMine && !!lmon.running);
    $("rx-monitor").title = `Listen to RX ${selRx + 1} locally · device: ` + (lmon.device || monCfg.device || "default");

    /* the connected badge in the sender list follows is-05/registry state
       live, e.g. when an external controller patches this receiver */
    const curSender = (status.nmos || {}).connected_sender_id;
    if (curSender !== lastCurSender) {
        lastCurSender = curSender;
        if (sendersCache.length) renderSenders();
    }


    /* sender page */
    pill($("tx-state"), tx.running ? (tx.waiting_for_ptp ? "Waiting for PTP" : "Sending") : "Off",
         tx.running ? (tx.waiting_for_ptp ? "warn" : "ok") : "off");
    const txLegs = (tx.legs || []).map((leg, i) =>
        `${i ? "SEC" : "PRI"} ${leg.multicast}:${leg.port}` + (leg.enabled === false ? " (RTP off)" : "")).join(" · ");
    $("tx-mini").textContent = tx.running
        ? `${tx.packets_sent || 0} pkts · avg ${(tx.late_avg_us || 0).toFixed(0)} µs` + (txLegs ? " · " + txLegs : "")
        : "stopped";

    /* transport fields follow config changes made via nmos/is-05,
       unless the user is editing them right now */
    const cfg = status.config;
    if (cfg && Date.now() - cfgTouchedAt > 1500) {
        config = cfg;
        const sync = (el, v) => { if (document.activeElement !== el && el.value != v) el.value = v; };
        const txc = txCfgOf(cfg);
        sync($("tx-mc1"), txc.multicast_primary);
        sync($("tx-mc2"), txc.multicast_secondary);
        sync($("tx-port"), txc.port);
        if (document.activeElement !== $("tx-leg1")) $("tx-leg1").checked = txc.leg1_enabled !== false;
        if (document.activeElement !== $("tx-leg2")) $("tx-leg2").checked = txc.leg2_enabled !== false;
    }

    /* ptp page */
    pill($("ptp-state"), ptp.synced ? "Locked" : "Not synced", ptp.synced ? "ok" : "warn");
    ptpClock = ptp.synced && ptp.time_ms ? {ms: ptp.time_ms, at: performance.now(), utc: ptp.utc_offset ?? 37} : null;
    $("ptp-identity").textContent = ptp.identity || "–";
    $("ptp-active-domain").textContent = (ptp.domain ?? "–") + (ptp.domain_auto ? " (auto)" : "") +
        ((ptp.domains_seen || []).length > 1 ? " · seen: " + ptp.domains_seen.join(", ") : "");
    $("ptp-offset").textContent = ptp.synced ? fmtNs(ptp.correction_ns) : "–";
    $("ptp-delay").textContent = fmtNs(ptp.mean_path_delay_ns);
    /* meta bmca: do amber and blue agree on the grandmaster? */
    const meta = ptp.meta;
    $("ptp-meta").style.display = meta ? "" : "none";
    if (meta) {
        const ok = meta.match === true;
        $("ptp-meta").innerHTML = `Meta BMCA: <span class="pill amber">A</span> ${esc(meta.amber_gm) || "–"}
            · <span class="pill blue">B</span> ${esc(meta.blue_gm) || "–"}
            <span class="pill ${meta.match === null ? "warn" : ok ? "ok" : "bad"}">${
                meta.match === null ? esc(meta.detail) : ok ? (meta.detail ? "same GM · " + esc(meta.detail) : "match") : "GM MISMATCH"}</span>`;
    }

    const tbody = $("ptp-masters").querySelector("tbody");
    tbody.innerHTML = (ptp.masters || []).map(m =>
        `<tr class="${m.selected ? "selected" : ""}">
         <td>${m.selected ? '<span class="crown">★ Master</span>' : ""}</td>
         <td>${m.port_identity}<br><span class="sub">${m.address}</span></td>
         <td>${m.grandmaster}</td>
         <td>${m.priority1}</td><td>${m.clock_class}</td><td>0x${m.clock_accuracy.toString(16)}</td>
         <td>0x${m.variance.toString(16)}</td><td>${m.priority2}</td><td>${m.steps_removed}</td>
         <td>${m.announces}</td>
         <td>${m.nets ? (m.nets.amber ? '<span class="pill amber">A</span>' : "") +
                (m.nets.blue ? ' <span class="pill blue">B</span>' : "") +
                (m.net_match === false ? ' <span class="pill bad">≠</span>' : "") : ""}</td>
         <td class="sub">${m.bmca}</td></tr>`).join("")
        || `<tr><td colspan="12" class="sub">no announce messages on domain ${ptp.domain}</td></tr>`;
    drawChart(ptp.offset_history || []);
}

/* ---------- receiver buffer chart ---------- */
function drawBufferChart(history, criticalMs, live) {
    const canvas = $("rx-buffer-chart");
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    const w = canvas.width = canvas.clientWidth;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    ctx.font = "11px sans-serif";
    if (history.length < 2) {
        ctx.fillStyle = "#8b93a7";
        ctx.fillText(live ? "collecting…" : "no active reception", 6, 14);
        return;
    }

    /* quantized scale so the chart does not rescale on every sample */
    const need = Math.max(criticalMs * 2, 10, ...history.map(s => s[1])) * 1.15;
    const STEPS = [10, 20, 30, 50, 75, 100, 150, 200, 300, 500];
    const top = STEPS.find(v => v >= need) || Math.ceil(need / 500) * 500;
    const y = ms => h - 4 - (ms / top) * (h - 20);
    const x = i => i / (history.length - 1) * w;

    /* min/max band: network jitter shows as band width and dips */
    ctx.beginPath();
    history.forEach((s, i) => { i ? ctx.lineTo(x(i), y(s[1])) : ctx.moveTo(x(i), y(s[1])); });
    for (let i = history.length - 1; i >= 0; i--) ctx.lineTo(x(i), y(history[i][0]));
    ctx.closePath();
    ctx.fillStyle = "rgba(77,163,255,.22)";
    ctx.fill();

    /* last-value line */
    ctx.strokeStyle = "#4da3ff";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    history.forEach((s, i) => { i ? ctx.lineTo(x(i), y(s[2])) : ctx.moveTo(x(i), y(s[2])); });
    ctx.stroke();

    /* critical line: below this the playout starts concealing */
    ctx.fillStyle = "rgba(231,76,60,.12)";
    ctx.fillRect(0, y(criticalMs), w, h - 4 - y(criticalMs));
    ctx.strokeStyle = "#e74c3c";
    ctx.setLineDash([5, 4]);
    ctx.beginPath(); ctx.moveTo(0, y(criticalMs)); ctx.lineTo(w, y(criticalMs)); ctx.stroke();
    ctx.setLineDash([]);

    const last = history[history.length - 1];
    ctx.fillStyle = "#e74c3c";
    ctx.fillText("critical < " + criticalMs.toFixed(0) + " ms", 6, y(criticalMs) - 5);
    ctx.fillStyle = "#8b93a7";
    ctx.fillText("now " + last[2].toFixed(1) + " ms · min " + last[0].toFixed(1) + " · max " + last[1].toFixed(1)
        + " · 30 s window · scale 0–" + top + " ms", 6, 14);
}

/* ---------- monitor matrix ---------- */
/* ---------- ptp clock ---------- */
let ptpClock = null;    //snapshot of the last status push, extrapolated locally
setInterval(() => {
    const el = $("ptp-time");
    if (!ptpClock) { el.textContent = "PTP time: – (not synced)"; return; }
    const p = n => String(n).padStart(2, "0");
    const tai = new Date(ptpClock.ms + (performance.now() - ptpClock.at));
    const utc = new Date(tai.getTime() - ptpClock.utc * 1000);
    el.textContent = `${tai.getUTCFullYear()}-${p(tai.getUTCMonth() + 1)}-${p(tai.getUTCDate())} `
        + `${p(tai.getUTCHours())}:${p(tai.getUTCMinutes())}:${p(tai.getUTCSeconds())} TAI`
        + ` · ${p(utc.getUTCHours())}:${p(utc.getUTCMinutes())}:${p(utc.getUTCSeconds())} UTC (TAI−${ptpClock.utc}s)`;
}, 250);

/* ---------- offset chart ---------- */
function drawChart(history) {
    const canvas = $("ptp-chart");
    const ctx = canvas.getContext("2d");
    const w = canvas.width = canvas.clientWidth;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);
    if (history.length < 2) return;

    const values = history.map(p => p[1]);
    const max = Math.max(1000, ...values.map(Math.abs));
    const Y = v => h / 2 - (v / max) * (h / 2 - 6);
    ctx.strokeStyle = "#2a3245";
    ctx.beginPath(); ctx.moveTo(0, h / 2); ctx.lineTo(w, h / 2); ctx.stroke();

    /* raw measurements: dim scatter - this is timestamp noise, not the clock */
    ctx.fillStyle = "rgba(139,147,167,0.45)";
    values.forEach((v, i) => {
        ctx.fillRect(i / (values.length - 1) * w - 1, Y(v) - 1, 2, 2);
    });

    /* what the servo actually follows: smoothed line */
    let ema = values[0];
    ctx.strokeStyle = "#4da3ff";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    values.forEach((v, i) => {
        ema += (v - ema) * 0.05;
        const x = i / (values.length - 1) * w;
        i ? ctx.lineTo(x, Y(ema)) : ctx.moveTo(x, Y(ema));
    });
    ctx.stroke();

    const mean = values.reduce((a, b) => a + b, 0) / values.length;
    const std = Math.sqrt(values.reduce((a, b) => a + (b - mean) * (b - mean), 0) / values.length);
    ctx.fillStyle = "#8b93a7";
    ctx.font = "11px sans-serif";
    ctx.fillText(`±${fmtNs(max)} · raw σ ${fmtNs(std)} (dots = measurement noise, line = servo)`, 6, 14);
}

/* ---------- nmos sender browser ---------- */
const esc = s => String(s ?? "").replace(/[&<>"]/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;"}[c]));

async function loadSenders() {
    /* refresh silently once a list is shown - collapsing to "loading…"
       makes the whole page jump */
    if (!sendersCache.length) $("rx-senders").textContent = "loading…";
    let r;
    try { r = await api("/api/action/nmos-senders", "POST", {}); }
    catch (e) { if (!sendersCache.length) $("rx-senders").textContent = "query failed"; return; }
    if (r.error) { if (!sendersCache.length) $("rx-senders").textContent = r.error; return; }
    sendersCache = r.senders || [];

    /* the group with the active connection starts expanded */
    const cur = (status.nmos || {}).connected_sender_id;
    const conn = sendersCache.find(s => s.id === cur);
    if (conn) sndOpen.add(conn.device || "(no device)");
    renderSenders();
}

function renderSenders() {
    if (!sendersCache.length) { $("rx-senders").textContent = "no audio senders in the registry"; return; }
    const filter = ($("rx-filter").value || "").trim().toLowerCase();
    const cur = (status.nmos || {}).connected_sender_id;

    const groups = new Map();
    sendersCache.forEach((s, i) => {
        if (filter && !(s.label + " " + s.device).toLowerCase().includes(filter)) return;
        const dev = s.device || "(no device)";
        if (!groups.has(dev)) groups.set(dev, []);
        groups.get(dev).push(i);
    });
    if (!groups.size) { $("rx-senders").textContent = "no sender matches the filter"; return; }

    $("rx-senders").innerHTML = [...groups.entries()].sort((a, b) => a[0].localeCompare(b[0])).map(([dev, items]) => {
        const connected = items.some(i => sendersCache[i].id === cur);
        const open = !!filter || sndOpen.has(dev);      //an active filter opens everything
        return `<div class="sndgrp">
            <div class="sndhead" data-dev="${esc(dev)}"><span class="arrow">${open ? "▾" : "▸"}</span>
                ${esc(dev)} <span class="sub">${items.length} sender${connected ? ' · <span class="pill ok">connected</span>' : ""}</span>
            </div>` +
            (open ? `<table class="kv">` + items.map(i => {
                const s = sendersCache[i];
                return `<tr${s.active === false ? ' class="snd-inactive"' : ""}>
                <td>${esc(s.label) || "(unnamed)"}${s.is_self ? ' <span class="pill off">this device</span>' : ""}
                    ${s.active === false ? ' <span class="pill off">inactive</span>' : ""}
                    ${s.media_type ? `<span class="sub"> · ${esc(s.media_type)}</span>` : ""}</td>
                <td style="text-align:right">${s.id === cur
                    ? '<span class="pill ok">connected</span> <button class="btn" data-off="' + i + '">Disconnect</button>'
                    : '<button class="btn" data-on="' + i + '"' + (s.manifest_href ? "" : " disabled") + '>Connect</button>'}
                </td></tr>`;
            }).join("") + `</table>` : "") + `</div>`;
    }).join("");

    document.querySelectorAll("#rx-senders .sndhead").forEach(el => {
        el.onclick = () => {
            const dev = el.dataset.dev;
            sndOpen.has(dev) ? sndOpen.delete(dev) : sndOpen.add(dev);
            renderSenders();
        };
    });
    document.querySelectorAll("#rx-senders [data-on]").forEach(btn => {
        btn.onclick = async () => {
            const s = sendersCache[+btn.dataset.on];
            btn.textContent = "Connecting…";
            const result = await api("/api/action/nmos-connect", "POST",
                {sender_id: s.id, manifest_href: s.manifest_href, stream: selRx}).catch(() => ({error: "request failed"}));
            if (result.error) alert(result.error);
            setTimeout(loadSenders, 800);
        };
    });
    document.querySelectorAll("#rx-senders [data-off]").forEach(btn => {
        btn.onclick = async () => {
            await api("/api/action/receiver-disconnect", "POST", {stream: selRx}).catch(() => {});
            setTimeout(loadSenders, 800);
        };
    });
}
$("rx-browse").onclick = loadSenders;
$("rx-filter").oninput = renderSenders;
/* keep the registry view current while the page is open */
setInterval(() => {
    if ($("page-receiver").classList.contains("active")) loadSenders();
}, 30000);

/* ---------- receiver controls ---------- */
$("rx-connect").onclick = () => {
    const sdp = $("rx-sdp").value.trim();
    if (sdp) api("/api/action/receiver-connect", "POST", {sdp, stream: selRx});
};
$("rx-disconnect").onclick = () => api("/api/action/receiver-disconnect", "POST", {stream: selRx});
let gainTouchedAt = 0;
let gainSendTimer = null;
$("rx-gain").oninput = () => {
    gainTouchedAt = Date.now();
    $("rx-gain-val").textContent = $("rx-gain").value + " dB";
    /* apply while dragging, throttled - the final value goes out in onchange */
    if (!gainSendTimer) {
        gainSendTimer = setTimeout(() => {
            gainSendTimer = null;
            api("/api/config", "POST", {"receiver.gain_db": parseFloat($("rx-gain").value)}).catch(() => {});
        }, 120);
    }
};
$("rx-gain").onchange = () => setConfig({"receiver.gain_db": parseFloat($("rx-gain").value)});

let monitorMine = false;
$("rx-monitor").onclick = () => setConfig(monitorMine
    ? {"monitor.enabled": false}
    : {"monitor.enabled": true, "monitor.stream": selRx});

/* dashboard: one chip per stream - green when healthy, shows what flows */
function renderDashStreams(mon) {
    const el = $("d-streams");
    if (!el) return;
    const cfgSenders = ((status.config || {}).senders) || [];
    const Chip = (s, type) => {
        const m = ((type === "rx" ? mon.receivers : mon.senders) || [])[s.index] || {};
        let cls = "off", what = "off";
        if (s.running) {
            const health = m.overall === 3 ? "bad" : (m.overall === 2 ? "warn" : "ok");
            if (type === "rx") {
                cls = s.receiving ? health : "bad";
                what = s.receiving ? (s.session_name || "connected") : "no signal";
            } else {
                cls = s.waiting_for_ptp ? "warn" : health;
                const label = (cfgSenders[s.index] || {}).label;
                const mc = (s.legs || [])[0] ? s.legs[0].multicast : "";
                what = (label ? label + " · " : "") + "→ " + mc;
            }
        }
        return `<span class="chip ${cls}" data-go="${type}:${s.index}"><span class="led"></span>` +
               `${type === "rx" ? "RX" : "TX"} ${s.index + 1} <span class="what">${what}</span></span>`;
    };
    el.innerHTML = `<div class="chips">${(status.receivers || []).map(r => Chip(r, "rx")).join("")}</div>` +
                   `<div class="chips">${(status.senders || []).map(t => Chip(t, "tx")).join("")}</div>`;
    el.querySelectorAll(".chip").forEach(chip => chip.onclick = () => {
        const [type, index] = chip.dataset.go.split(":");
        if (type === "rx") { selRx = +index; showPage("receiver"); } else { selTx = +index; showPage("sender"); }
        loadConfig();
        render();
    });
}

/* ---------- stream selectors (receiver/sender pages) ---------- */
function renderSelectors() {
    const Build = (el, list, sel, prefix, type) => {
        if (!el) return;
        const fp = JSON.stringify(list.map(s => [s.index, s.running, s.receiving]));
        if (el.dataset.fp === fp && el.dataset.sel == sel) return;
        el.dataset.fp = fp;
        el.dataset.sel = sel;
        el.innerHTML = list.map(s =>
            `<button class="btn ${s.index === sel ? "primary" : ""}" data-sel="${s.index}">
             ${prefix} ${s.index + 1}${s.running ? " ●" : ""}</button>`).join("")
            + `<button class="btn" data-addstream title="Add a ${prefix} stream">+</button>`
            + (list.length ? `<button class="btn" data-delstream title="Remove ${prefix} ${sel + 1}">✕</button>` : "");
        el.querySelectorAll("[data-sel]").forEach(btn => btn.onclick = () => {
            if (prefix === "RX") { selRx = +btn.dataset.sel; } else { selTx = +btn.dataset.sel; }
            loadConfig();
            render();
        });
        //inline add form - prompt()/confirm() dialogs are blocked in some
        //browsers/webviews and then the buttons appear dead
        el.querySelector("[data-addstream]").onclick = () => {
            const old = el.querySelector(".addform");
            if (old) { old.remove(); return; }
            const form = document.createElement("span");
            form.className = "addform";
            form.innerHTML = `
                <label class="inline">Channels <select data-ch>${[1,2,3,4,5,6,7,8].map(n =>
                    `<option ${n === 2 ? "selected" : ""}>${n}</option>`).join("")}</select></label>
                <label class="inline">from device ch <input data-start size="3" value="1"></label>
                <button class="btn primary" data-ok>Add</button>`;
            el.appendChild(form);
            form.querySelector("[data-ok]").onclick = async () => {
                const channels = parseInt(form.querySelector("[data-ch]").value, 10);
                const start = parseInt(form.querySelector("[data-start]").value, 10);
                if (!start || start < 1 || start > 64) {
                    form.querySelector("[data-start]").style.borderColor = "#e74c3c";
                    return;
                }
                const r = await api("/api/action/stream-add", "POST",
                    {type, channels, channel_offset: start - 1}).catch(alert);
                form.remove();
                if (r && r.ok !== undefined) {
                    if (prefix === "RX") { selRx = r.index; } else { selTx = r.index; }
                }
            };
        };
        const del = el.querySelector("[data-delstream]");
        if (del) del.onclick = async () => {
            if (del.dataset.armed !== "1") {
                del.dataset.armed = "1";
                del.textContent = `Remove ${prefix} ${sel + 1}?`;
                del.classList.add("danger");
                setTimeout(() => {
                    del.dataset.armed = "0";
                    del.textContent = "✕";
                    del.classList.remove("danger");
                }, 4000);
                return;
            }
            await api("/api/action/stream-remove", "POST", {type, index: sel}).catch(alert);
            if (prefix === "RX") { selRx = 0; } else { selTx = 0; }
        };
    };
    Build($("rx-select"), status.receivers || [], selRx, "RX", "receiver");
    Build($("tx-select"), status.senders || [], selTx, "TX", "sender");
}

/* single-stream patch row on the receiver/sender pages */
function renderPatchRows() {
    const BuildRow = (el, stream, type) => {
        if (!el) return;
        if (!stream) { el.innerHTML = "<span class='sub'>no stream selected</span>"; delete el.dataset.fp; return; }
        const fp = JSON.stringify([stream.index, stream.channel_map, stream.running]);
        if (el.dataset.fp === fp) return;
        el.dataset.fp = fp;
        const map = stream.channel_map || [];
        let html = "<table><tr><th></th>";
        for (let ch = 0; ch < 64; ch++) {
            html += `<th>${ch % 8 === 0 ? ch + 1 : (ch % 4 === 0 ? "·" : "")}</th>`;
        }
        html += `</tr><tr><td class="name"><span class="pill ${stream.running ? "ok" : "off"}">` +
                `${type === "sender" ? "TX" : "RX"} ${stream.index + 1}</span></td>`;
        for (let ch = 0; ch < 64; ch++) {
            const pos = map.indexOf(ch);
            html += `<td class="mx-cell ${pos >= 0 ? "set" : ""} ${pos >= 0 && stream.running ? "live" : ""}"` +
                    ` data-ch="${ch}" title="device channel ${ch + 1}">${pos >= 0 ? pos + 1 : ""}</td>`;
        }
        html += "</tr></table>";
        el.innerHTML = html;
        el.querySelectorAll(".mx-cell").forEach(cell => cell.onclick = async () => {
            const ch = +cell.dataset.ch;
            let m = (stream.channel_map || []).slice();
            const pos = m.indexOf(ch);
            if (pos >= 0) { m.splice(pos, 1); }
            else if (m.length >= 8) { alert("Max 8 channels per stream (AES67)"); return; }
            else { m.push(ch); }
            if (!m.length) { alert("At least 1 channel required"); return; }
            await api("/api/action/stream-map", "POST",
                      {type, index: stream.index, channel_map: m}).catch(alert);
        });
    };
    BuildRow($("rx-patch"), (status.receivers || [])[selRx], "receiver");
    BuildRow($("tx-patch"), (status.senders || [])[selTx], "sender");
}

/* ---------- graphical 64ch matrix ---------- */
function renderMatrixPage() {
    const BuildGrid = (el, list, type) => {
        if (!el) return;
        const fp = JSON.stringify(list.map(s => [s.index, s.channel_map, s.running]));
        if (el.dataset.fp === fp) {
            //update live state only
            list.forEach(s => (s.channel_map || []).forEach(ch => {
                const cell = el.querySelector(`[data-cell="${s.index}:${ch}"]`);
                if (cell) cell.classList.toggle("live", !!s.running);
            }));
            return;
        }
        el.dataset.fp = fp;
        let html = "<table><tr><th></th>";
        for (let ch = 0; ch < 64; ch++) {
            html += `<th>${ch % 8 === 0 ? ch + 1 : (ch % 4 === 0 ? "·" : "")}</th>`;
        }
        html += "</tr>";
        for (const s of list) {
            const map = s.channel_map || [];
            const name = s.session_name || s.source || "";
            html += `<tr><td class="name"><span class="pill ${s.running ? "ok" : "off"}">` +
                    `${type === "sender" ? "TX" : "RX"} ${s.index + 1}</span>${name}</td>`;
            for (let ch = 0; ch < 64; ch++) {
                const pos = map.indexOf(ch);
                html += `<td class="mx-cell ${pos >= 0 ? "set" : ""} ${pos >= 0 && s.running ? "live" : ""}"` +
                        ` data-cell="${s.index}:${ch}" data-stream="${s.index}" data-ch="${ch}"` +
                        ` title="${type} ${s.index + 1} · device channel ${ch + 1}">` +
                        `${pos >= 0 ? pos + 1 : ""}</td>`;
            }
            html += "</tr>";
        }
        html += "</table>";
        el.innerHTML = html;
        el.querySelectorAll(".mx-cell").forEach(cell => cell.onclick = async () => {
            const idx = +cell.dataset.stream;
            const ch = +cell.dataset.ch;
            const stream = list.find(x => x.index === idx);
            if (!stream) return;
            let map = (stream.channel_map || []).slice();
            const pos = map.indexOf(ch);
            if (pos >= 0) { map.splice(pos, 1); }
            else if (map.length >= 8) { alert("Max 8 channels per stream (AES67)"); return; }
            else { map.push(ch); }
            if (!map.length) { alert("Mindestens 1 Kanal - zum Löschen des Streams ✕ auf dem Dashboard"); return; }
            await api("/api/action/stream-map", "POST", {type, index: idx, channel_map: map}).catch(alert);
        });
    };
    BuildGrid($("mx-tx"), status.senders || [], "sender");
    BuildGrid($("mx-rx"), status.receivers || [], "receiver");
}

/* ---------- sender controls (source is always the AES67 device) ---------- */
$("tx-start").onclick = () => setConfig({
    [txKey("enabled")]: true,
    [txKey("label")]: $("tx-label").value.trim(),
    [txKey("source")]: "device",
    [txKey("multicast_primary")]: $("tx-mc1").value,
    [txKey("multicast_secondary")]: $("tx-mc2").value,
    [txKey("port")]: parseInt($("tx-port").value, 10)
});
$("tx-stop").onclick = () => setConfig({[txKey("enabled")]: false});
$("tx-apply").onclick = async () => {
    cfgTouchedAt = Date.now();
    await setConfig({
        [txKey("multicast_primary")]: $("tx-mc1").value.trim(),
        [txKey("multicast_secondary")]: $("tx-mc2").value.trim(),
        [txKey("port")]: parseInt($("tx-port").value, 10) || 5004
    });
    $("tx-apply-info").textContent = "applied";
    setTimeout(() => { $("tx-apply-info").textContent =
        "restarts this transmitter with the new destination"; }, 2500);
};
let cfgTouchedAt = 0;
["tx-mc1", "tx-mc2", "tx-port"].forEach(id => { $(id).oninput = () => { cfgTouchedAt = Date.now(); }; });
$("tx-leg1").onchange = () => { cfgTouchedAt = Date.now(); setConfig({[txKey("leg1_enabled")]: $("tx-leg1").checked}); };
$("tx-leg2").onchange = () => { cfgTouchedAt = Date.now(); setConfig({[txKey("leg2_enabled")]: $("tx-leg2").checked}); };
$("tx-show-sdp").onclick = async () => {
    const result = await api("/api/action/sender-sdp", "POST", {stream: selTx});
    $("tx-sdp").textContent = result.sdp || result.error || "sender is not running";
    $("tx-sdp").style.display = "";
    $("tx-copy-sdp").style.display = result.sdp ? "" : "none";
};
$("tx-copy-sdp").onclick = () => navigator.clipboard.writeText($("tx-sdp").textContent);

/* ---------- interface setup ---------- */
async function loadNetwork() {
    let ifs;
    try { ifs = (await api("/api/action/network-status", "POST", {})).interfaces || []; }
    catch (e) { $("net-ifs").textContent = "network state unavailable"; return; }

    //status only - interface addressing is managed in macos system settings
    $("net-ifs").innerHTML = ifs.map(f => `
        <div class="netif">
            <div class="row">
                <span class="pill ${f.role}">${f.role.toUpperCase()}</span>
                <span class="ifname">${f.interface}</span>
                <span class="pill ${f.link ? "ok" : "bad"}">${f.link ? "Link" : "No link"}</span>
                <span class="cur">${f.address || "no address"}</span>
            </div>
        </div>`).join("") || `<div class="sub">no interfaces configured</div>`;
}

/* ---------- settings ---------- */
$("set-save").onclick = async () => {
    const regHost = $("set-reg-host").value.trim();
    const regPort = parseInt($("set-reg-port").value, 10);
    const override = regHost
        ? `${$("set-reg-scheme").value}://${regHost}${regPort ? ":" + regPort : ""}` : "";
    const updates = {
        "device.label": $("set-label").value,
        "network.interface_primary": $("set-if1").value,
        "network.interface_secondary": $("set-if2").value,
        "receiver.playout_delay_ms": parseInt($("set-delay").value, 10),
        "nmos.registry_override": override,
        "nmos.http_port": parseInt($("set-nmos-port").value, 10) || 8080
    };
    //these settings only take full effect after a service restart - do it
    const RESTART_KEYS = ["network.interface_primary", "network.interface_secondary",
                          "nmos.registry_override", "nmos.http_port"];
    const before = config;
    const get = key => key.split(".").reduce((o, k) => (o || {})[k], before);
    const needsRestart = RESTART_KEYS.some(k => String(get(k) ?? "") !== String(updates[k] ?? ""));
    await setConfig(updates);
    if (needsRestart) {
        $("set-save-info").textContent = "restarting service…";
        await api("/api/action/daemon-restart", "POST", {}).catch(() => {});
        setTimeout(() => location.reload(), 4000);
    } else {
        $("set-save-info").textContent = "applied";
        setTimeout(() => { $("set-save-info").textContent =
            "Interface/registry/port changes restart the service automatically"; }, 3000);
    }
};
$("ptp-domain").onchange = () => setConfig({"ptp.domain": parseInt($("ptp-domain").value, 10)});
$("ptp-domain-auto").onchange = () => setConfig({"ptp.domain_auto": $("ptp-domain-auto").checked});

/* interface dropdowns from the live interface list */
async function loadInterfaceOptions() {
    let ifs = [];
    try { ifs = (await api("/api/action/list-interfaces", "POST", {})).interfaces || []; }
    catch (e) { /* keep empty - options are added from config below */ }
    const fill = (el, current, allowOff) => {
        if (!el) return;
        let html = allowOff ? `<option value="">off</option>` : "";
        let seen = false;
        for (const f of ifs) {
            if (f.name === current) seen = true;
            html += `<option value="${f.name}">${f.name}${f.ip ? " · " + f.ip : ""}</option>`;
        }
        if (current && !seen) html += `<option value="${current}">${current} (not present)</option>`;
        el.innerHTML = html;
        el.value = current || "";
    };
    fill($("set-if1"), (config.network || {}).interface_primary || "en0", false);
    fill($("set-if2"), (config.network || {}).interface_secondary || "", true);
}

async function loadConfig() {
    config = await api("/api/config");
    const txc2 = txCfgOf(config);
    $("tx-label").value = txc2.label || "";
    $("tx-leg1").checked = txc2.leg1_enabled !== false;
    $("tx-leg2").checked = txc2.leg2_enabled !== false;
    $("tx-mc1").value = txc2.multicast_primary || "";
    $("tx-mc2").value = txc2.multicast_secondary || "";
    $("tx-port").value = txc2.port || 5004;
    $("rx-gain").value = config.receiver.gain_db;
    $("rx-gain-val").textContent = config.receiver.gain_db + " dB";
    $("set-label").value = config.device.label;
    loadInterfaceOptions();
    $("set-delay").value = config.receiver.playout_delay_ms;
    {
        const m = /^(https?):\/\/([^:\/]+)(?::(\d+))?/.exec(config.nmos.registry_override || "");
        $("set-reg-scheme").value = m ? m[1] : "http";
        $("set-reg-host").value = m ? m[2] : (config.nmos.registry_override || "");
        $("set-reg-port").value = m && m[3] ? m[3] : "";
        $("set-nmos-port").value = config.nmos.http_port || 8080;
    }
    $("ptp-domain").value = config.ptp.domain;
    $("ptp-domain-auto").checked = config.ptp.domain_auto !== false;
    $("set-version").textContent = "AES67eleven " + (status.version || "");
}

/* ---------- player page ---------- */
function fmtTime(s) {
    if (!s && s !== 0) return "–";
    s = Math.max(0, Math.round(s));
    return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
}

function loadPlayerFiles() {
    api("/api/files").then(files => {
        document.querySelectorAll(".pl-filesel").forEach(sel => {
            const keep = sel.value;
            sel.innerHTML = `<option value="">– file –</option>` +
                files.map(f => `<option>${f}</option>`).join("");
            if (files.includes(keep)) sel.value = keep;
        });
    }).catch(() => {});
}

function renderPlayers() {
    const decks = status.players || [];
    const host = $("pl-decks");
    if (!host || !decks.length) return;

    if (!host.dataset.built) {
        host.dataset.built = "1";
        const chOpts = Array.from({length: 64}, (_, i) => `<option value="${i}">${i + 1}</option>`).join("");
        host.innerHTML = decks.map((d, n) => `
            <div class="card wide pl-deck" id="pl-${n}">
                <h2>Player ${n + 1} <span class="sub pl-file"></span></h2>
                <div class="row">
                    <select class="pl-filesel" title="Selecting a file loads it right away"></select>
                    <button class="btn primary pl-play">Play</button>
                    <button class="btn pl-stop">Stop</button>
                    <label class="inline"><input type="checkbox" class="pl-loop"> Loop</label>
                    <label class="inline" title="Load and play this file automatically when the service starts"><input type="checkbox" class="pl-auto"> Auto-play</label>
                    <label class="inline">Gain <input class="pl-gain" size="4"> dB</label>
                    <label class="inline">L → ch <select class="pl-chl">${chOpts}</select></label>
                    <label class="inline">R → ch <select class="pl-chr">${chOpts}</select></label>
                </div>
                <div class="pl-progress"><div class="pl-fill"></div></div>
                <div class="sub pl-time">–</div>
            </div>`).join("");

        decks.forEach((d, n) => {
            const card = $("pl-" + n);
            const deckStatus = () => (status.players || [])[n] || {};
            card.querySelector(".pl-filesel").onchange = async e => {
                const file = e.target.value;
                if (!file) return;
                await api("/api/action/player-load", "POST", {player: n, file}).catch(alert);
            };
            card.querySelector(".pl-play").onclick = () =>
                api("/api/action/player-ctrl", "POST",
                    {player: n, command: deckStatus().playing ? "pause" : "play"}).catch(alert);
            card.querySelector(".pl-stop").onclick = () =>
                api("/api/action/player-ctrl", "POST", {player: n, command: "stop"}).catch(alert);
            card.querySelector(".pl-loop").onchange = e =>
                api("/api/action/player-set", "POST", {player: n, loop: e.target.checked});
            card.querySelector(".pl-auto").onchange = e =>
                api("/api/action/player-set", "POST", {player: n, autostart: e.target.checked});
            card.querySelector(".pl-gain").onchange = e =>
                api("/api/action/player-set", "POST", {player: n, gain_db: parseFloat(e.target.value) || 0});
            card.querySelector(".pl-chl").onchange = e =>
                api("/api/action/player-set", "POST", {player: n, ch_left: parseInt(e.target.value, 10)});
            card.querySelector(".pl-chr").onchange = e =>
                api("/api/action/player-set", "POST", {player: n, ch_right: parseInt(e.target.value, 10)});
            card.querySelector(".pl-progress").onclick = e => {
                const d = deckStatus();
                if (!d.duration_s) return;
                const rect = e.currentTarget.getBoundingClientRect();
                const frac = (e.clientX - rect.left) / rect.width;
                api("/api/action/player-seek", "POST", {player: n, seconds: frac * d.duration_s});
            };
        });
        loadPlayerFiles();
    }

    decks.forEach((d, n) => {
        const card = $("pl-" + n);
        if (!card) return;
        card.querySelector(".pl-file").textContent = d.file || "no file loaded";
        card.querySelector(".pl-play").textContent = d.playing ? "Pause" : "Play";
        const frac = d.duration_s ? d.position_s / d.duration_s : 0;
        card.querySelector(".pl-fill").style.width = (frac * 100).toFixed(1) + "%";
        card.querySelector(".pl-time").textContent =
            d.file ? fmtTime(d.position_s) + " / " + fmtTime(d.duration_s) : "–";
        const SYNC = [[".pl-filesel", el => { if (d.file && [...el.options].some(o => o.value === d.file)) el.value = d.file; }],
                      [".pl-loop", el => { el.checked = !!d.loop; }],
                      [".pl-auto", el => { el.checked = !!d.autostart; }],
                      [".pl-gain", el => { el.value = d.gain_db; }],
                      [".pl-chl",  el => { el.value = d.ch_left; }],
                      [".pl-chr",  el => { el.value = d.ch_right; }]];
        for (const [cls, apply] of SYNC) {
            const el = card.querySelector(cls);
            if (document.activeElement !== el) apply(el);
        }
    });
}

$("pl-upload").onchange = async e => {
    const file = e.target.files[0];
    if (!file) return;
    $("pl-upload-info").textContent = "uploading…";
    try {
        const r = await fetch("/api/files/" + encodeURIComponent(file.name),
                              {method: "PUT", body: file});
        $("pl-upload-info").textContent = r.ok ? "uploaded " + file.name : "upload failed";
    } catch (err) {
        $("pl-upload-info").textContent = "upload failed";
    }
    e.target.value = "";
    loadPlayerFiles();
};

loadConfig();
loadNetwork();
connectWs();
