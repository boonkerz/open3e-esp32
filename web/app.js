"use strict";

/* Single-page UI for the open3e gateway.
 *
 * No framework and no build step: the whole thing is three files served from
 * the device's own flash, and a toolchain would have to be installed by anyone
 * who wants to change a label.
 */

const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, ...kids) => {
  const n = Object.assign(document.createElement(tag), props);
  for (const k of kids) n.append(k);
  return n;
};

async function api(path, opts = {}) {
  if (opts.body) opts.headers = { "Content-Type": "application/json", ...opts.headers };
  const res = await fetch(path, opts);
  const text = await res.text();
  let body;
  try { body = text ? JSON.parse(text) : {}; } catch { body = { error: text }; }
  if (!res.ok) throw new Error(body.error || `HTTP ${res.status}`);
  return body;
}

let toastTimer;
function toast(msg, kind = "") {
  document.querySelector(".toast")?.remove();
  const t = el("div", { className: `toast ${kind}`, textContent: msg });
  document.body.append(t);
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.remove(), kind === "err" ? 8000 : 3500);
}

const fmtDuration = (s) => {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  if (d) return `${d} d ${h} h`;
  if (h) return `${h} h ${m} min`;
  return `${m} min`;
};
const fmtKiB = (b) => `${Math.round(b / 1024)} KiB`;

/* ------------------------------------------------------------------ */
/* Setup mode                                                          */

async function setupScan() {
  const sel = $("setup-ssid");
  sel.innerHTML = "<option value=''>Suche läuft…</option>";
  try {
    const nets = await api("/api/wifi/scan");
    sel.innerHTML = "";

    /* The endpoint answers with an object instead of an array when the scan
       itself failed. Saying so beats "no networks found", which sends people
       looking for a router problem that is not there. */
    if (!Array.isArray(nets)) {
      sel.append(el("option", { value: "", textContent: "Suche fehlgeschlagen" }));
      toast(`WLAN-Suche fehlgeschlagen: ${nets.error || "unbekannter Fehler"}`, "err");
      return;
    }
    if (!nets.length) {
      sel.append(el("option", { value: "", textContent: "Kein Netzwerk in Reichweite" }));
      return;
    }
    /* Strongest first: the list is long in a block of flats and the wanted
       network is almost always the closest one. */
    nets.sort((a, b) => b.rssi - a.rssi);
    for (const n of nets) {
      sel.append(el("option", {
        value: n.ssid,
        textContent: `${n.ssid}  (${n.rssi} dBm${n.secure ? "" : ", offen"})`,
      }));
    }
  } catch (e) {
    sel.innerHTML = "";
    sel.append(el("option", { value: "", textContent: "Suche fehlgeschlagen" }));
    toast(e.message, "err");
  }
}

function initSetup() {
  $("setup").hidden = false;
  setupScan();
  $("setup-rescan").onclick = setupScan;
  $("setup-hostname").oninput = (e) => {
    $("setup-host").textContent = `${e.target.value || "open3e"}.local`;
  };
  $("setup-save").onclick = async (e) => {
    /* A hidden network, or one the scan missed, is still reachable by name. */
    const ssid = $("setup-ssid-manual").value.trim() || $("setup-ssid").value;
    if (!ssid) return toast("Bitte ein Netzwerk auswählen oder den Namen eingeben.", "err");
    e.target.disabled = true;
    try {
      await api("/api/wifi", {
        method: "POST",
        body: JSON.stringify({
          ssid,
          pass: $("setup-pass").value,
          hostname: $("setup-hostname").value,
          apPass: $("setup-appass").value,
        }),
      });
      document.querySelector("#setup main").innerHTML =
        `<div class="card"><h2>Gespeichert</h2><p>Das Gerät startet neu und verbindet
         sich mit <b>${ssid}</b>. Danach ist es unter
         <span class="mono">${$("setup-hostname").value || "open3e"}.local</span>
         erreichbar.</p><p class="hint">Verbinde dich wieder mit deinem normalen WLAN.
         Klappt die Verbindung nicht, erscheint dieser Einrichtungs-Hotspot nach
         etwa einer Minute erneut.</p></div>`;
    } catch (err) {
      e.target.disabled = false;
      toast(err.message, "err");
    }
  };
}

/* ------------------------------------------------------------------ */
/* Status                                                              */

function pill(node, ok, label) {
  node.textContent = label;
  node.className = `pill ${ok === true ? "ok" : ok === false ? "err" : "warn"}`;
}

function renderStatus(s) {
  $("s-ssid").textContent = s.net.ssid || "–";
  $("s-ip").textContent = s.net.ip || "–";
  $("s-rssi").textContent = s.net.rssi ? `${s.net.rssi} dBm` : "–";
  $("s-uptime").textContent = fmtDuration(s.uptimeS);
  /* Without a synchronised clock a trace can only be read against itself. Say
     so plainly rather than showing a plausible-looking wrong date. */
  $("s-clock").textContent = s.clockValid ? s.clock : "nicht gesetzt";
  $("s-clock").className = s.clockValid ? "v" : "v muted";

  /* The poll runs every five seconds and a doorbell press lasts one, so the
     state alone would rarely catch anything. The count is what actually
     confirms the wiring: press once, see it climb by one. */
  /* Verworfene Frames sind das Einzige hier, was still schiefgeht: der
     Empfang läuft weiter, nur mit Löchern. Deshalb hervorgehoben, sobald
     der Zähler nicht mehr null ist. */
  if (s.knx) {
    const el = $("knx-state");
    if (el) {
      el.textContent = !s.knx.enabled
        ? "aus"
        : (s.knx.connected ? `verbunden als ${s.knx.address}` : "nicht verbunden") +
          `, ${s.knx.sent} gesendet` +
          (s.knx.failures ? `, ${s.knx.failures} Fehler` : "") +
          (s.knx.error ? ` \u2014 ${s.knx.error}` : "");
      el.style.color = s.knx.enabled && s.knx.error ? "var(--err)" : "";
    }
  }

  if (s.raw) {
    const el = $("sys-rawstate");
    if (el) {
      el.textContent = !s.raw.enabled
        ? "aus"
        : `${s.raw.nIds} ID(s), ${s.raw.frames} Frames, ${s.raw.published} gesendet` +
          (s.raw.dropped ? `, ${s.raw.dropped} verworfen` : "");
      el.style.color = s.raw.dropped ? "var(--err)" : "";
    }
  }

  (s.contacts || []).forEach((c, i) => {
    const el = $(`ct${i}-state`);
    if (!el) return;
    const out = c.mode === "output";
    el.textContent = !c.enabled
      ? "aus"
      : out
        ? `${c.active ? "eingeschaltet" : "ausgeschaltet"} seit ${fmtDuration(c.sinceS)}` +
          ` · ${c.edges} Schaltung${c.edges === 1 ? "" : "en"}`
        : `${c.active ? "geschlossen" : "offen"} seit ${fmtDuration(c.sinceS)}` +
          ` · ${c.edges} Auslösung${c.edges === 1 ? "" : "en"}`;
    /* The button switches the output as it is now, so its label follows the
       reported state rather than the unsaved form. */
    const btn = $(`ct${i}-toggle`);
    if (btn) {
      btn.hidden = !(c.enabled && out);
      btn.textContent = c.active ? "Ausschalten" : "Einschalten";
      btn.dataset.gpio = c.pin;
      btn.dataset.on = c.active ? "0" : "1";
    }
  });

  $("s-canstate").textContent = s.can.state;
  $("s-buserr").textContent = s.can.busErrors;
  /* TEC/REC climb long before the controller actually goes bus-off, so they
     are the earliest visible sign of a wiring or bitrate problem. */
  $("s-errcnt").textContent = `${s.can.txErrCount} / ${s.can.rxErrCount}`;
  /* Frames the receive interrupt could not queue: the one counter on this
     card that means data this firmware itself threw away. */
  const missed = $("s-rxmissed");
  missed.textContent = s.can.rxMissed;
  missed.style.color = s.can.rxMissed ? "var(--err)" : "";
  $("s-recov").textContent = s.can.recoveries;
  /* A single exchange is bounded to a few seconds; anything longer means one
     is stuck, and every other read will fail with "transport error" until it
     lets go. */
  const held = s.can.heldMs || 0;
  const holderEl = $("s-holder");
  holderEl.textContent = s.can.holder ? `${s.can.holder} (${(held / 1000).toFixed(0)} s)` : "frei";
  holderEl.style.color = held > 20000 ? "var(--err)" : "";

  $("s-mqtt").textContent = s.mqtt.connected ? "verbunden" : "getrennt";
  $("s-pub").textContent = s.mqtt.published;
  /* Shown wherever the page is, not only on the card: something that is
     overriding the installation's own regulation should not be easy to
     forget about. */
  const g = s.grid || {};
  /* "Aktiv, aber noch nie geschrieben" ist der Zustand, der wie Erfolg
     aussieht und keiner ist: eine falsche ECU-Adresse lässt den Hold starten,
     jeden Schreibvorgang scheitern und ihn nach fünf Versuchen verschwinden.
     Das hat hier einmal zwanzig Minuten Suche gekostet, weil nur "aktiv"
     dastand. */
  const nowrite = (active, writes) => active && !writes;
  const gs = $("gh-state");
  if (gs) {
    gs.textContent = g.active
      ? `Aktiv: ${g.watts} W, noch ${fmtDuration(g.remainingS)}`
        + ` · ${g.writes} Schreibvorgänge` + (g.failures ? `, ${g.failures} Fehler` : "")
        + (nowrite(g.active, g.writes) ? " — noch kein Schreibvorgang gelungen, ECU-Adresse prüfen" : "")
      : "Nicht aktiv — die Anlage regelt selbst.";
    gs.style.color = nowrite(g.active, g.writes) ? "var(--err)"
                   : g.active ? "var(--warn)" : "var(--muted)";
  }
  const ss = $("sh-state");
  if (ss) {
    const held = g.storage && g.storage !== "normal";
    const bad = nowrite(held, g.storageWrites);
    ss.textContent = held
      ? `Aktiv: ${g.storage}, noch ${fmtDuration(g.storageRemainingS)}`
        + ` · ${g.storageWrites ?? 0} Schreibvorgänge`
        + (g.storageFailures ? `, ${g.storageFailures} Fehler` : "")
        + (bad ? " — noch kein Schreibvorgang gelungen, ECU-Adresse prüfen" : "")
      : "Nicht gehalten — der Manager entscheidet.";
    ss.style.color = bad ? "var(--err)" : held ? "var(--warn)" : "var(--muted)";
  }
  /* Sensors and controls apart: a datapoint that is read-only and one whose
     control never got published look identical from the outside. */
  $("s-ha").textContent = s.mqtt.haSensors + s.mqtt.haControls
    ? `${s.mqtt.haSensors} Sensoren, ${s.mqtt.haControls} Bedienelemente`
    : "nichts veröffentlicht";
  $("s-points").textContent = s.poll.points;
  $("s-pollerr").textContent = s.poll.failures;
  /* An external client's UDS traffic (ioBroker.e3oncan over /api/rawread),
     apart from our own poller: "ohne Antwort" is the ECU missing a deadline,
     not a frame lost on the bus. */
  const rr = s.rawRead;
  const rrEl = $("s-rawread");
  if (rr && (rr.reads || rr.writes)) {
    let txt = `${rr.reads} gelesen`;
    if (rr.noResponse) txt += `, ${rr.noResponse} ohne Antwort`;
    if (rr.errors) txt += `, ${rr.errors} Fehler`;
    if (rr.writes) txt += `, ${rr.writes} geschrieben` + (rr.writeErrors ? ` (${rr.writeErrors} Fehler)` : "");
    rrEl.textContent = txt;
    rrEl.style.color = (rr.errors || rr.writeErrors) ? "var(--err)" : "";
  } else {
    rrEl.textContent = "keine";
    rrEl.style.color = "";
  }

  $("s-fw").textContent = s.firmware;
  /* The raw API has its own version, because external integrations check it
     rather than the firmware release -- the status page is where someone
     looks when an adapter reports a mismatch. */
  $("s-rawapi").textContent = s.rawApiVersion != null ? `Version ${s.rawApiVersion}` : "–";
  $("s-built").textContent = s.buildDate || "–";
  $("s-sha").textContent = s.elfSha || "–";
  $("s-db").textContent = s.dbLoaded ? `${s.dbVersion} (${s.dbCount})` : "nicht geladen";

  /* Without the database there are no names, no units and no decoding -- every
     datapoint reads "nicht in DB". That is worth saying once and clearly
     rather than leaving it to be inferred from the tables. */
  const warn = $("db-warn");
  const noNames = s.dbLoaded && Object.keys(didNames).length === 0;
  warn.hidden = !!s.dbLoaded && !noNames;
  if (noNames) {
    warn.innerHTML =
      "<b>Die Datenbank ist geladen, aber es kamen keine Namen an.</b> "
      + "Meist passen Firmware und Weboberfläche nicht zusammen — beide auf den "
      + "gleichen Stand bringen. Die Build-Kennung unten sollte zu der aus "
      + "<span class='mono'>make fwinfo</span> passen.";
  } else if (!s.dbLoaded) {
    warn.innerHTML =
      "<b>Die Datenpunkt-Datenbank ist nicht geladen.</b> Ohne sie gibt es keine "
      + "Namen, Einheiten oder dekodierten Werte — alles erscheint als "
      + "„nicht in DB“. Meist passt das Datenbankformat nicht zur Firmware: "
      + "unter <i>Einstellungen → Datenbank aktualisieren</i> die aktuelle "
      + "<span class='mono'>storage.bin</span> einspielen. Der Monitor nennt im "
      + "Log den genauen Grund.";
  }

  $("ota-rollback").hidden = !s.pendingVerify;
  $("ota-part").textContent = `Läuft aus Partition ${s.partition}, Version ${s.firmware}.`;

  pill($("hdr-can"), s.can.state === "running" ? true
       : s.can.state === "bus-off" ? false : null, `CAN ${s.can.state}`);
  pill($("hdr-mqtt"), s.mqtt.connected, s.mqtt.connected ? "MQTT" : "MQTT getrennt");
}

async function loadSysinfo() {
  let s;
  try { s = await api("/api/sysinfo"); } catch { return; }

  $("s-ram").textContent = fmtKiB(s.heap.internalFree);
  $("s-rammin").textContent = fmtKiB(s.heap.internalMin);
  $("s-psram").textContent = s.heap.psramFree ? fmtKiB(s.heap.psramFree) : "–";

  const busy = (s.taskList || []).filter((t) => t.name !== "IDLE" && t.name !== "IDLE0"
                                             && t.name !== "IDLE1");
  const load = busy.reduce((a, t) => a + t.cpu, 0);
  /* Two cores, so the idle tasks can account for up to 2000 per mille; the
     figure shown is the share that is not idle. */
  $("s-cpu").textContent = s.cpuAvailable ? `${(load / 10).toFixed(1)} %` : "misst…";
  $("s-window").textContent = s.cpuAvailable
    ? `CPU-Anteile gemessen über die letzten ${(s.windowMs / 1000).toFixed(0)} s · `
      + `${s.tasks} Tasks`
    : `${s.tasks} Tasks · CPU-Anteile ab der nächsten Messung`;

  const tbody = document.querySelector("#task-table tbody");
  tbody.innerHTML = "";
  for (const t of s.taskList || []) {
    /* Under 512 bytes of headroom is where a stack starts being a real risk. */
    const tight = t.stackFree < 512;
    tbody.append(el("tr", {},
      el("td", { className: "mono", textContent: t.name }),
      el("td", { className: "num", textContent: s.cpuAvailable ? `${(t.cpu / 10).toFixed(1)} %` : "–" }),
      el("td", { className: "num", textContent: `${Math.round(t.stackFree / 1024 * 10) / 10} KiB`,
                 style: tight ? "color:var(--err);font-weight:600" : "" }),
      el("td", { className: "num", textContent: t.prio }),
      el("td", { className: "small muted", textContent: t.core < 0 ? "beide" : t.core }),
      el("td", { className: "small muted", textContent: t.state })));
  }
}

/* ------------------------------------------------------------------ */
/* Scan                                                                */

let scanning = false;

function renderScan(scan) {
  const busy = scan.phase === "ecus" || scan.phase === "dids";
  $("scan-progress").hidden = !busy && scan.phase === "idle";
  $("scan-start").disabled = busy;
  $("scan-abort").disabled = !busy;

  const pct = scan.total ? Math.min(100, (100 * scan.probed) / scan.total) : 0;
  $("scan-bar").style.width = `${pct}%`;

  if (scan.ecuLimitHit) {
    $("scan-msg").style.color = "var(--warn)";
  }
  let msg = scan.message || "";
  if (scan.phase === "ecus") {
    msg = `Suche ECUs… ${scan.probed}/${scan.total}`
        + (scan.cobFirst !== undefined
           ? ` (0x${scan.cobFirst.toString(16).toUpperCase()}–`
             + `0x${scan.cobLast.toString(16).toUpperCase()})` : "");
  }
  else if (scan.phase === "dids") {
    msg = `Lese Datenpunkte… ${scan.probed}/${scan.total} (aktuell DID ${scan.curDid}), `
        + `${scan.ecus} Gerät(e) gefunden`;
  }
  if (scan.ecuLimitHit) {
    msg += `  ⚠ Mehr als ${scan.ecuLimit} Geräte haben geantwortet; `
         + "weitere wurden nicht erfasst.";
  }
  /* A probe counter that stops moving while the bus is busy means one exchange
     is stuck; saying so beats leaving a number standing still. */
  if (busy && scan.stalledMs > 15000) {
    msg += `  ⚠ Seit ${(scan.stalledMs / 1000).toFixed(0)} s kein Fortschritt — `
         + "ein Zugriff hängt. Siehe Monitor-Log oder 'Bus belegt von' oben.";
    $("scan-msg").style.color = "var(--err)";
  } else if (!scan.ecuLimitHit) {
    $("scan-msg").style.color = "";
  }
  $("scan-msg").textContent = msg;

  /* Refresh the device and datapoint views once a scan finishes. */
  if (scanning && !busy) loadSystem();
  scanning = busy;
}

/* ------------------------------------------------------------------ */
/* System / devices                                                    */

let system = { devices: [] };
let points = [];
/* did -> name, fetched once. The scan result stores only numbers so it stays
   small enough to fit on the storage partition. `didDe` holds the German
   reading aid generated from a glossary at build time -- the English name
   stays the identifier everything else uses. */
let didNames = {};
let didDe = {};

async function loadNames() {
  if (Object.keys(didNames).length) return;
  const r = await api("/api/names").catch(() => ({}));

  /* Firmware and interface are updated separately -- that is the point of the
     fast web-only path -- so this endpoint has to accept both shapes it has
     had. Newer firmware answers {n:{...},de:{...}}, older answers a flat
     {did:name}. Without this, a half-updated device shows every datapoint as
     "unbekannt" and blames the database. */
  if (r && typeof r === "object" && r.n) {
    didNames = r.n;
    didDe = r.de || {};
  } else {
    didNames = r || {};
    didDe = {};
  }
}

async function loadSystem() {
  system = await api("/api/system").catch(() => ({ devices: [] }));
  const list = $("ecu-list");

  if (!system.devices?.length) {
    list.className = "muted small";
    list.textContent = "Noch kein Scan durchgeführt.";
  } else {
    list.className = "tablewrap";
    list.innerHTML = "";
    const t = el("table");
    t.append(el("thead", {
      innerHTML: "<tr><th>Adresse</th><th>Name ({device})</th><th>Typ</th>"
               + "<th>Funktion</th><th>Ident-Nr.</th><th>Seriennummer</th>"
               + "<th>Software</th><th class='num'>Datenpunkte</th></tr>",
    }));
    const tb = el("tbody");
    for (const d of system.devices) {
      const name = el("input", { type: "text", value: d.name || "",
                                 placeholder: d.addrHex, style: "min-width:130px" });
      /* Renaming rewrites every topic that uses {device}, so it is saved
         explicitly on blur rather than on every keystroke. */
      name.onchange = async () => {
        try {
          await api("/api/devices", {
            method: "PUT",
            body: JSON.stringify([{ addr: d.addr, name: name.value.trim() || d.addrHex }]),
          });
          d.name = name.value.trim() || d.addrHex;
          toast("Name gespeichert. MQTT-Topics mit {device} ändern sich damit.", "ok");
        } catch (e) { toast(e.message, "err"); }
      };
      tb.append(el("tr", {},
        el("td", { className: "mono", textContent: d.addrHex }),
        el("td", {}, name),
        el("td", { textContent: d.prop || "–" }),
        el("td", { className: "small muted", textContent: d.function || "–" }),
        el("td", { className: "mono small", textContent: d.ident || "–" }),
        el("td", { className: "mono small", textContent: d.vin || "–" }),
        el("td", { className: "mono small", textContent: d.sw || "–",
                   title: d.hw ? `Hardware: ${d.hw}` : "" }),
        el("td", { className: "num", textContent: d.dids.length })));
    }
    t.append(tb);
    list.append(t);
  }

  for (const sel of [$("pt-ecu"), $("dbg-ecu"), $("gh-ecu")]) {
    const prev = sel.value;
    sel.innerHTML = "";
    for (const d of system.devices || []) {
      sel.append(el("option", {
        value: d.addr,
        textContent: `${d.name || d.addrHex} – ${d.prop || "unbekannt"}`,
      }));
    }
    if (prev) sel.value = prev;
  }
  showTarget();
  renderPoints();
}

/* ------------------------------------------------------------------ */
/* Datapoint selection                                                 */

const pointKey = (ecu, did) => `${ecu}:${did}`;
let selection = new Map();   /* key -> config object */
const POINT_PAGE = 300;
let pointLimit = POINT_PAGE;

async function loadPoints() {
  points = await api("/api/points").catch(() => []);
  selection = new Map(points.map((p) =>
    [p.type === "em380" ? `em:${p.canId}`
     : p.type === "collect" ? `co:${p.did}`
     : pointKey(p.ecu, p.did), p]));
  /* Die evcc-Konfiguration hängt an genau dieser Auswahl, also mitziehen --
     auch wenn die Karte auf einem anderen Reiter liegt. */
  if (typeof renderEvcc === "function") renderEvcc();
}

function currentEcu() {
  return Number($("pt-ecu").value) || (system.devices?.[0]?.addr ?? 0);
}

/* Labels turn a bare number into a word, in Home Assistant only.
 *
 * The database names the values of the datapoints Viessmann documented as
 * enumerations; the rest are plain numbers, and a bypass reading of "2" means
 * nothing without the manual. Typing "0=zu, 1=offen, 2=automatisch" here makes
 * that datapoint a named choice instead of a spin box accepting 0..255.
 *
 * Stored inside the selection entry, so it travels with an export and needs no
 * second file. "*" applies to every numeric field of the datapoint, which is
 * what one wants when only one of them carries meaning; a name in front
 * ("BypassStatus: 0=zu, ...") restricts them to that field. */
function parseLabels(text) {
  const t = text.trim();
  if (!t) return undefined;
  let field = "*", body = t;
  const colon = t.indexOf(":");
  /* A colon names a field only when what precedes it is not itself a mapping. */
  if (colon > 0 && !t.slice(0, colon).includes("=")) {
    field = t.slice(0, colon).trim();
    body = t.slice(colon + 1);
  }
  const map = {};
  for (const part of body.split(",")) {
    const eq = part.indexOf("=");
    if (eq < 0) continue;
    const k = part.slice(0, eq).trim();
    const v = part.slice(eq + 1).trim();
    if (k !== "" && v !== "" && /^-?\d+$/.test(k)) map[k] = v;
  }
  return Object.keys(map).length ? { [field]: map } : undefined;
}

function formatLabels(labels) {
  if (!labels) return "";
  const field = Object.keys(labels)[0];
  if (!field) return "";
  const body = Object.entries(labels[field]).map(([k, v]) => `${k}=${v}`).join(", ");
  return field === "*" ? body : `${field}: ${body}`;
}

function renderPoints() {
  const tbody = document.querySelector("#pt-table tbody");
  tbody.innerHTML = "";

  const ecu = currentEcu();
  const dev = (system.devices || []).find((d) => d.addr === ecu);
  if (!dev) {
    $("pt-count").textContent = "Erst einen Bus-Scan durchführen.";
    return;
  }

  const needle = $("pt-filter").value.trim().toLowerCase();
  const onlyActive = $("pt-only-active").checked;
  const onlyKnown = $("pt-only-known").checked;

  /* The table only ever lists the selected device, which is not obvious when
     several devices each return thousands of datapoints. */
  $("pt-scope").textContent =
    `Zeigt ausschließlich Datenpunkte von ${dev.name || dev.addrHex}`
    + `${dev.prop ? ` (${dev.prop})` : ""} – ${dev.dids.length} gefunden.`;

  let matched = 0, shown = 0;
  for (const entry of dev.dids) {
    /* [did, responseLength]; the name comes from the database, not the file. */
    const did = entry[0], len = entry[1];
    const name = didNames[did] || "";
    const dp = { did, len, name, known: name !== "" };
    const key = pointKey(ecu, dp.did);
    const cfg = selection.get(key);
    if (onlyActive && !cfg) continue;
    if (onlyKnown && !dp.known && !cfg) continue;
    /* Search the German label as well, so looking for "Vorlauf" works. */
    if (needle && !dp.name.toLowerCase().includes(needle)
        && !(didDe[dp.did] || "").toLowerCase().includes(needle)
        && !String(dp.did).includes(needle)) {
      continue;
    }
    matched++;
    /* Rendering thousands of rows at once makes the page unusable on a phone,
       so they arrive in pages rather than being silently cut off. */
    if (shown >= pointLimit) continue;
    shown++;

    const on = el("input", { type: "checkbox", checked: !!cfg });
    const iv = el("input", { type: "number", value: cfg?.interval ?? 60, min: 1,
                             style: "width:72px", disabled: !cfg });
    const mode = el("select", { disabled: !cfg, style: "width:104px" });
    mode.append(el("option", { value: "json", textContent: "JSON" }),
                el("option", { value: "flat", textContent: "geflacht" }));
    mode.value = cfg?.mode ?? "json";
    const topic = el("input", { type: "text", value: cfg?.topic ?? "",
                                placeholder: dp.name, disabled: !cfg });
    const ha = el("input", { type: "checkbox", checked: cfg?.ha ?? true, disabled: !cfg });
    const labels = el("input", {
      type: "text", value: formatLabels(cfg?.labels), disabled: !cfg,
      style: "width:186px", className: "mono small",
      placeholder: "0=zu, 1=offen",
      title: "Benennt die Zahlen dieses Datenpunkts für Home Assistant und macht "
           + "daraus eine Auswahlliste statt eines Zahlenfelds. Leer lassen, wenn "
           + "die Zahl selbst genügt. Mit vorangestelltem Feldnamen "
           + "(\"BypassStatus: 0=zu, 1=offen\") nur für dieses Feld.",
    });
    const valCell = el("td", { className: "mono small muted" });

    const sync = () => {
      for (const c of [iv, mode, topic, ha, labels]) c.disabled = !on.checked;
      if (on.checked) {
        const lb = parseLabels(labels.value);
        selection.set(key, {
          ecu, did: dp.did, len: dp.len, enabled: true,
          interval: Number(iv.value) || 60,
          mode: mode.value, topic: topic.value, ha: ha.checked,
          ...(lb ? { labels: lb } : {}),
        });
      } else {
        selection.delete(key);
      }
    };
    for (const c of [on, iv, mode, topic, ha, labels]) c.onchange = sync;

    const readBtn = el("button", { className: "btn sec small", textContent: "lesen",
                                   style: "padding:2px 8px" });
    readBtn.onclick = async () => {
      valCell.textContent = "…";
      try {
        const r = await api(`/api/read?ecu=${ecu}&did=${dp.did}`);
        valCell.textContent = JSON.stringify(r.value);
        valCell.className = "mono small";
      } catch (e) {
        valCell.textContent = e.message;
        valCell.className = "mono small";
        valCell.style.color = "var(--err)";
      }
    };
    valCell.append(readBtn);

    tbody.append(el("tr", {},
      el("td", {}, on),
      el("td", { className: "num mono", textContent: dp.did }),
      el("td", {},
         el("span", { textContent: dp.name || "(unbekannt)" }),
         didDe[dp.did] ? el("div", { className: "small muted",
                                     textContent: didDe[dp.did] }) : "",
         /* An undocumented datapoint still answered the scan -- otherwise it
            would not be in this list at all. Showing the response length makes
            that visible instead of leaving "nicht in DB" to look like an error. */
         dp.known ? "" : el("span", {
           className: "pill warn small",
           style: "margin-left:6px",
           textContent: `nicht in DB · ${dp.len} B`,
           title: `Diese ECU hat auf DID ${dp.did} mit ${dp.len} Byte geantwortet, `
                + `die open3e-Datenbank kennt den Datenpunkt aber nicht. `
                + `Wird als Hex-String gelesen und gesendet; Schreiben ist gesperrt.`,
         })),
      el("td", { className: "num" }, iv, el("span", { className: "muted small",
                                                      textContent: " s" })),
      el("td", {}, mode),
      el("td", {}, topic),
      el("td", {}, ha),
      el("td", {}, labels),
      valCell));
  }

  const activeHere = [...selection.values()].filter((p) => p.ecu === ecu).length;
  const hidden = matched - shown;
  $("pt-more").hidden = hidden <= 0;
  $("pt-count").textContent =
    `${shown} von ${matched} passenden Datenpunkten angezeigt`
    + (hidden > 0 ? ` (${hidden} weitere durch Suche oder Filter erreichbar)` : "")
    + ` · ${activeHere} auf diesem Gerät aktiv, ${selection.size} insgesamt`;
}

async function saveSelection(btn) {
  btn.disabled = true;
  try {
    /* One file holds both the polled datapoints and the broadcast frames, so
       either screen saves the whole selection -- otherwise saving one would
       silently discard the other. */
    await api("/api/points", { method: "PUT", body: JSON.stringify([...selection.values()]) });
    toast(`${selection.size} Einträge gespeichert.`, "ok");
    renderEvcc();
  } catch (e) {
    toast(e.message, "err");
  } finally {
    btn.disabled = false;
  }
}

/* ------------------------------------------------------------------ */
/* Energy meter                                                        */

let meter = { frameList: [] };

async function loadMeter() {
  meter = await api("/api/em380").catch(() => ({ frameList: [] }));

  $("em-state").textContent = !meter.enabled ? "aus (in den Einstellungen aktivieren)"
    : meter.seen ? "empfängt" : "wartet auf Frames";
  $("em-frames").textContent = meter.frames ?? "–";
  $("em-drop").textContent = meter.dropped ?? "–";
  $("em-pub").textContent = meter.published ?? "–";

  const tbody = document.querySelector("#em-table tbody");
  tbody.innerHTML = "";
  const onlySeen = $("em-only-seen").checked;
  let hidden = 0;
  for (const f of meter.frameList || []) {
    const key = `em:${f.canId}`;
    const cfg = selection.get(key);
    /* Half the CAN-IDs belong to the meter at the other CAN address, so they
       never arrive. Hiding them leaves only the rows that mean something --
       an active selection always stays visible. */
    if (onlySeen && f.value === undefined && !cfg) {
      hidden++;
      continue;
    }

    const on = el("input", { type: "checkbox", checked: !!cfg });
    const iv = el("input", { type: "number", value: cfg?.interval ?? 10, min: 1,
                             style: "width:72px", disabled: !cfg });
    const mode = el("select", { disabled: !cfg, style: "width:104px" });
    mode.append(el("option", { value: "json", textContent: "JSON" }),
                el("option", { value: "flat", textContent: "geflacht" }));
    mode.value = cfg?.mode ?? "json";
    const topic = el("input", { type: "text", value: cfg?.topic ?? "",
                                placeholder: `E380/${f.name}`, disabled: !cfg });

    const sync = () => {
      for (const c of [iv, mode, topic]) c.disabled = !on.checked;
      if (on.checked) {
        selection.set(key, {
          type: "em380", canId: f.canId, enabled: true,
          interval: Number(iv.value) || 10,
          mode: mode.value, topic: topic.value,
        });
      } else {
        selection.delete(key);
      }
    };
    for (const c of [on, iv, mode, topic]) c.onchange = sync;

    tbody.append(el("tr", {},
      el("td", {}, on),
      el("td", { className: "mono", textContent: f.canIdHex }),
      el("td", { className: "small muted", textContent: String(f.address) }),
      el("td", { textContent: f.name || "–" }),
      el("td", { className: "num" }, iv, el("span", { className: "muted small",
                                                      textContent: " s" })),
      el("td", {}, mode),
      el("td", {}, topic),
      el("td", { className: "mono small",
                 /* No value means this frame has never arrived -- either no
                    meter on the bus, or only one of the two CAN addresses. */
                 textContent: f.value === undefined ? "–" : JSON.stringify(f.value),
                 style: f.value === undefined ? "opacity:.5" : "" })));
  }
  if (hidden) {
    tbody.append(el("tr", {}, el("td", {
      colSpan: 8, className: "small muted",
      textContent: `${hidden} Frame(s) ausgeblendet, weil sie nie empfangen wurden.`,
    })));
  }
}

/* ------------------------------------------------------------------ */
/* CAN trace                                                           */

let traceFrames = [];

const parseId = (v) => {
  const n = parseInt(String(v).trim(), String(v).trim().startsWith("0x") ? 16 : 16);
  return Number.isFinite(n) ? n : 0;
};

async function traceStatus() {
  let st;
  try { st = await api("/api/trace"); } catch { return null; }
  $("tr-state").textContent = st.running ? "läuft" : (st.captured ? "gestoppt" : "bereit");
  $("tr-count").textContent = `${st.stored} / ${st.capacity}`;
  /* How far back the ring reaches, from the rate actually observed -- the
     number that decides whether a triggered event has any usable lead-up. */
  if (st.elapsedMs > 5000 && st.captured) {
    const fps = st.captured / (st.elapsedMs / 1000);
    const span = st.capacity / Math.max(fps, 0.1) / 60;
    $("tr-span").textContent = `${fps.toFixed(0)}/s · Ring reicht ${span.toFixed(0)} Min zurück`;
  } else {
    $("tr-span").textContent = "";
  }
  $("tr-drop").textContent = st.dropped || "0";
  $("tr-own-skipped").textContent = st.excludeOwn ? (st.skippedOwn || "0") : "aus";
  $("tr-time").textContent = `${(st.elapsedMs / 1000).toFixed(0)} s`;
  $("tr-start").disabled = st.running;
  $("tr-stop").disabled = !st.running;

  const ts = $("tr-trig-state");
  if (st.trigger === "none") {
    ts.textContent = "keiner";
    ts.style.color = "";
  } else if (st.learning) {
    ts.textContent = `lernt noch ${st.learnLeftS} s`;
    ts.style.color = "";
  } else if (st.triggered && st.event) {
    /* The buffer is frozen around the event now, so say what it was rather
       than just that something happened. */
    const e = st.event;
    const idHex = `0x${e.canId.toString(16).toUpperCase()}`;
    const at = `nach ${(e.us / 1e6).toFixed(1)} s`;
    let what;
    if (e.kind === "newId") {
      what = `${idHex} ist neu — diese ID kam während der Lernphase nie vor. `
           + `Daten ${e.data}.`;
      ts.textContent = `${idHex} neu`;
    } else if (e.kind === "byteChange") {
      what = `${idHex}, Byte ${e.byte}: ${e.was} → ${e.now}. Dieses Byte war die `
           + `ganze Lernphase über konstant. Daten ${e.data}.`;
      ts.textContent = `${idHex} Byte ${e.byte}`;
    } else {
      const name = didNames[e.did] || "nicht in DB";
      what = `${idHex} schreibt DID ${e.did} (${name}), Daten ${e.data}.`;
      ts.textContent = `${idHex} → DID ${e.did}`;
    }
    ts.style.color = "var(--err)";
    $("tr-summary").textContent = `Ausgelöst ${at}: ${what}`;
  } else {
    ts.textContent = st.running ? "wartet…" : "nicht ausgelöst";
    ts.style.color = "";
  }
  return st;
}

/* Reassemble ISO-TP and name what the exchange was.
 *
 * Done here rather than in firmware on purpose: this is the part that wants
 * changing while hunting for a particular behaviour, and the web interface can
 * be replaced in seconds where firmware cannot. */
function decodeExchanges(frames) {
  const partial = new Map();   // canId -> { need, bytes }
  const collectSeen = new Set();
  const out = [];

  /* 0x77 is open3e's experimental write service. It answers with the same SID
     rather than SID+0x40, so request and response are not distinguishable by
     the service byte alone. On an E3 bus this is what the control traffic
     actually uses -- watching only for 0x2E finds nothing. */
  /* What the backend gateway writes into the storage: the control mode, the
     power setpoint at the grid connection point, and the charge/discharge
     limits. A change in any of these is the event worth catching. */
  const CONTROL_DIDS = new Set([2188, 2226, 2239]);

  const SERVICES = {
    0x22: "Lesen", 0x62: "Lese-Antwort",
    0x2E: "SCHREIBEN", 0x6E: "Schreib-Quittung",
    0x77: "SCHREIBEN (0x77)",
    0x7F: "Abgelehnt",
  };

  for (const [us, id, tx, hex] of frames) {
    const b = [];
    for (let i = 0; i + 1 < hex.length; i += 2) b.push(parseInt(hex.substr(i, 2), 16));
    if (!b.length) continue;

    /* A frame starting 0x21 is ambiguous: it is either a Viessmann collect
       broadcast (type byte 0xB0..0xBF in position 3) or the first consecutive
       frame of an ISO-TP message. On 0x441/0x451 it is the latter, and the
       payload there happens to carry <did><0xBx> at exactly that offset -- so
       the type-byte test alone throws away the control traffic we are after.
       Only treat it as collect when no reassembly is in flight for that ID. */
    if (b[0] === 0x21 && !partial.has(id) &&
        b.length > 4 && b[3] >= 0xb0 && b[3] < 0xc0) {
      collectSeen.add(id);
      continue;
    }

    const pci = b[0] & 0xf0;
    let msg = null;
    if (pci === 0x00) {
      msg = b.slice(1, 1 + (b[0] & 0x0f));
    } else if (pci === 0x10) {
      partial.set(id, { need: ((b[0] & 0x0f) << 8) | b[1], bytes: b.slice(2) });
      continue;
    } else if (pci === 0x20) {
      const p = partial.get(id);
      if (!p) continue;
      p.bytes.push(...b.slice(1));
      if (p.bytes.length < p.need) continue;
      msg = p.bytes.slice(0, p.need);
      partial.delete(id);
    } else {
      continue;   /* flow control carries no payload */
    }
    if (!msg || !msg.length) continue;

    const sid = msg[0];
    const svc = SERVICES[sid];
    if (!svc) continue;

    const toHex = (a) => a.map((x) => x.toString(16).padStart(2, "0")).join("");

    /* Service 0x77 does not use the plain SID+DID+data layout of 0x22/0x2E:
     *
     *   77 <counter lo> <counter hi> <tag> [01 82 <block> <block> ...]
     *   block := <did lo> <did hi> <0xB0+len> <data>
     *
     * The counter is shared by both control channels and rises monotonically;
     * the tag names the channel. When 01 82 follows, blocks come after it,
     * otherwise the message is a four-byte acknowledgement. Reading msg[1..2]
     * as a DID -- as 0x22 and 0x2E allow -- yields nonsense here. Verified
     * against a 21603-frame capture: 2018 of 2018 messages decode, and every
     * one of the 1979 blocks resolves to a known DID. */
    if (sid === 0x77) {
      if (msg.length >= 6 && msg[4] === 0x01 && msg[5] === 0x82) {
        /* Two bytes and nothing more is a request: the DID being asked for,
           with no length byte and no payload. The answer comes back on the
           partner channel as the same DID plus data. */
        if (msg.length === 8) {
          const q = msg[6] | (msg[7] << 8);
          out.push({
            us, ecu: id, tx, svc: "Lesen (0x77)", sid, did: q,
            name: didNames[q] || "", write: false, raw: false, data: "",
          });
          continue;
        }
        let o = 6, any = false;
        while (o + 3 <= msg.length) {
          const bdid = msg[o] | (msg[o + 1] << 8);
          const lc = msg[o + 2];
          if ((lc & 0xf0) !== 0xb0) break;
          /* 0xB0 means the length follows; a 0xC1 there in turn means it
             follows one byte later. Without this, every message carrying a
             block longer than 15 bytes falls back to raw hex. */
          let n = lc & 0x0f, head = 3;
          if (n === 0) {
            if (o + 3 >= msg.length) break;
            if (msg[o + 3] === 0xc1) {
              if (o + 4 >= msg.length) break;
              n = msg[o + 4]; head = 5;
            } else { n = msg[o + 3]; head = 4; }
          }
          if (o + head + n > msg.length) break;
          /* Tag 0x42 answers a 0x41 request, 0x43 is sent unbidden. Only the
             latter carries the setpoints the backend pushes at the storage, so
             flagging every 0x77 as a write would bury them under telemetry. */
          out.push({
            us, ecu: id, tx, sid, did: bdid,
            svc: msg[3] === 0x42 ? "Antwort (0x77)" : "Meldung (0x77)",
            name: didNames[bdid] || "", raw: false,
            write: msg[3] === 0x43 && CONTROL_DIDS.has(bdid),
            data: toHex(msg.slice(o + head, o + head + n)),
          });
          o += head + n;
          any = true;
        }
        if (any) continue;
      }
      /* The 4-byte form carries no DID: bytes 1..2 are a 16-bit counter shared
         by both control channels, byte 3 tags the channel. */
      if (msg.length === 4) {
        out.push({
          us, ecu: id, tx, svc: "0x77 Quittung", sid, did: null, write: false, raw: false,
          name: `Zähler ${msg[1] | (msg[2] << 8)}, Kanal 0x${msg[3].toString(16)}`,
          data: toHex(msg.slice(1)),
        });
        continue;
      }
      out.push({ us, ecu: id, tx, svc, sid, did: null, name: "", write: true,
                 raw: true, data: toHex(msg.slice(1)) });
      continue;
    }

    /* A response arrives on the request address plus 0x10, so both halves of
       an exchange are attributed to the same ECU. */
    const isResponse = sid === 0x62 || sid === 0x6e || sid === 0x7f;
    const ecu = isResponse ? id - 0x10 : id;
    const plainLayout = sid !== 0x7f;
    const did = plainLayout ? ((msg[1] << 8) | msg[2]) : null;

    out.push({
      us, ecu, tx, svc, sid, did,
      name: did !== null ? (didNames[did] || "") : "",
      data: toHex(msg.slice(plainLayout ? 3 : 1)),
      write: sid === 0x2e,
      raw: false,
    });
  }
  decodeExchanges.collectIds = [...collectSeen];
  return out;
}

/* Reduce the trace to the moments where something actually changed.
 *
 * Nearly all of this bus is periodic telemetry repeating identical bytes --
 * in a 255 s capture, 1979 messages carried 43 distinct datapoints. The event
 * worth finding is a single value moving somewhere in that stream, which is
 * invisible when every repetition is listed. Keyed per (ECU, DID) so a value
 * that changes is judged against its own history, not the row above it. */
function onlyChanges(rows) {
  const last = new Map();
  const out = [];
  for (const e of rows) {
    if (e.did === null) continue;      /* quittances carry no value to compare */
    const key = `${e.ecu}.${e.did}`;
    const prev = last.get(key);
    if (prev === e.data) continue;
    last.set(key, e.data);
    out.push(prev === undefined ? { ...e, first: true } : { ...e, prev });
  }
  return out;
}

/* Set once the debug tab is wired; called again whenever the ECU list is
   refilled, which happens after this runs. */
let showTarget = () => {};

function renderTrace() {
  const onlyWrites = $("tr-writes").checked;
  const changesOnly = $("tr-changes").checked;
  const all = decodeExchanges(traceFrames);
  let rows = all.filter((e) => !onlyWrites || e.write);
  if (changesOnly) rows = onlyChanges(rows);

  const tbody = document.querySelector("#tr-table tbody");
  tbody.innerHTML = "";
  for (const e of rows.slice(-300)) {
    tbody.append(el("tr", {},
      el("td", { className: "num mono small", textContent: (e.us / 1e6).toFixed(3) }),
      el("td", { className: "mono", textContent: `0x${e.ecu.toString(16).toUpperCase()}` }),
      el("td", { className: "small muted", textContent: e.tx ? "von uns" : "vom Bus" }),
      el("td", { textContent: e.svc,
                 style: e.write ? "color:var(--err);font-weight:600" : "" }),
      el("td", { className: "num mono", textContent: e.did ?? "–" }),
      el("td", {},
         el("span", { textContent: e.raw ? "(Format 0x77, roh)"
                                         : (e.name || (e.did !== null ? "(nicht in DB)" : "")) }),
         (e.did !== null && didDe[e.did])
           ? el("div", { className: "small muted", textContent: didDe[e.did] }) : ""),
      e.prev !== undefined
        ? el("td", { className: "mono small" },
             el("span", { className: "muted", textContent: e.prev || "–" }),
             " → ",
             el("b", { textContent: e.data || "–" }))
        : el("td", { className: "mono small" },
             el("span", { textContent: e.data || "–" }),
             e.first ? el("span", { className: "small muted", textContent: "  (erster Wert)" }) : "")));
  }

  const writes = rows.filter((e) => e.write).length;
  const co = decodeExchanges.collectIds || [];
  $("tr-summary").textContent =
    `${traceFrames.length} Frames → ${all.length} UDS-Zugriffe`
    + (changesOnly ? `, davon ${rows.length} mit geändertem Wert` : "")
    + (onlyWrites ? "" : `, ${writes} Steuerbefehle`)
    + (co.length ? ` · Broadcast (Collect) auf ${co.map((i) => "0x" + i.toString(16).toUpperCase()).join(", ")}`
                   + " — dekodiert im Reiter Passive Daten" : "")
    + (rows.length > 300 ? " · nur die letzten 300 angezeigt" : "");
}

async function traceLoad() {
  const btn = $("tr-load");
  btn.disabled = true;
  try {
    traceFrames = [];
    /* Paged: a full ring is tens of thousands of frames and one response would
       be several megabytes. */
    for (let from = 0; ; from += 500) {
      const page = await api(`/api/trace/frames?from=${from}&count=500`);
      if (!page.length) break;
      traceFrames.push(...page);
      if (page.length < 500 || traceFrames.length >= 20000) break;
    }
    renderTrace();
  } catch (e) {
    toast(e.message, "err");
  } finally {
    btn.disabled = false;
  }
}

/* ------------------------------------------------------------------ */
/* Broadcast channel (collect)                                          */

async function loadCollect() {
  let c;
  try {
    c = await api("/api/broadcast");
  } catch (e) {
    /* Not swallowed: a dash with no reason reads the same as a quiet bus, and
       the two want completely different things done about them. */
    $("co-msgs").textContent = "Fehler";
    $("co-bad").textContent = e.message;
    $("co-msgs").style.color = "var(--err)";
    return;
  }
  $("co-msgs").style.color = "";

  /* "aus" rather than 0: the receiver being switched off and the bus being
     silent look identical in a counter, and only one of them is a setting. */
  $("co-msgs").textContent = c.enabled ? (c.messages ?? 0) : "Empfang aus";
  $("co-drop").textContent = c.dropped ?? "–";
  $("co-bad").textContent = c.enabled ? (c.incomplete ?? 0) : "–";

  const tbody = document.querySelector("#co-table tbody");
  tbody.innerHTML = "";
  for (const d of c.dids || []) {
    const key = `co:${d.did}`;
    const cfg = selection.get(key);

    const on = el("input", { type: "checkbox", checked: !!cfg });
    const iv = el("input", { type: "number", value: cfg?.interval ?? 10, min: 1,
                             style: "width:72px", disabled: !cfg });
    const mode = el("select", { disabled: !cfg, style: "width:104px" });
    mode.append(el("option", { value: "json", textContent: "JSON" }),
                el("option", { value: "flat", textContent: "geflacht" }));
    mode.value = cfg?.mode ?? "json";
    const topic = el("input", { type: "text", value: cfg?.topic ?? "",
                                placeholder: `collect/${d.name}`, disabled: !cfg });

    const sync = () => {
      for (const x of [iv, mode, topic]) x.disabled = !on.checked;
      if (on.checked) {
        selection.set(key, {
          type: "collect", did: d.did, len: d.len, enabled: true,
          interval: Number(iv.value) || 10,
          mode: mode.value, topic: topic.value,
        });
      } else {
        selection.delete(key);
      }
    };
    for (const x of [on, iv, mode, topic]) x.onchange = sync;

    tbody.append(el("tr", {},
      el("td", {}, on),
      el("td", { className: "num mono", textContent: d.did }),
      el("td", {}, el("span", { textContent: d.name || "(nicht in DB)" }),
         d.name ? "" : el("span", { className: "pill warn small",
                                    style: "margin-left:6px",
                                    textContent: `${d.len} B` }),
         didDe[d.did] ? el("div", { className: "small muted",
                                    textContent: didDe[d.did] }) : ""),
      el("td", { className: "num", textContent: d.count }),
      el("td", { className: "num" }, iv, el("span", { className: "muted small",
                                                      textContent: " s" })),
      el("td", {}, mode),
      el("td", {}, topic),
      el("td", { className: "mono small",
                 textContent: d.value === undefined ? "–" : JSON.stringify(d.value) })));
  }
  if (!(c.dids || []).length) {
    tbody.append(el("tr", {}, el("td", {
      colSpan: 8, className: "small muted",
      textContent: c.enabled
        ? "Noch nichts empfangen — sendet auf dieser CAN-ID gerade jemand?"
        : "Nicht aktiv. Oben einschalten.",
    })));
  }
}

/* ------------------------------------------------------------------ */
/* evcc                                                                */

/* Which datapoint delivers which value evcc wants.
 *
 * A curated table, not a derivation: the open3e database carries neither the
 * field names nor the sign conventions, so both were read off a running
 * Vitocharge VX3 and are pinned here.
 *
 * `field` is null for a datapoint that is a bare scalar -- those publish at
 * their own topic in flat mode and as a plain JSON value in json mode, so
 * there is nothing to pick out of them. */
const EVCC_ROLES = [
  { key: "grid", did: 1603, field: "ActivePower", usage: "grid",
    label: "Netzleistung", name: "PointOfCommonCouplingPower" },
  /* The AC side, and already summed over the strings -- measured as
     3742 + 1776 = 5518 W. DID 1831 is the DC input per string (three of them
     on the installation this was checked against, not the two the database
     comment suggests), which would need adding up and would read high by the
     inverter's own losses. */
  { key: "pv", did: 1690, field: "ActivePower cumulated", usage: "pv",
    label: "PV-Leistung", name: "ElectricalEnergySystemPhotovoltaicStatus" },
  { key: "battery", did: 1836, field: null, usage: "battery",
    label: "Speicherleistung", name: "ElectricalEnergyStorageCurrentPower" },
  { key: "soc", did: 1664, field: null, usage: "battery",
    label: "Ladestand", name: "ElectricalEnergyStorageStateOfCharge" },
];

/* @topic-expansion-start
   Mirror of mqtt_pub_topic() in main/mqtt_pub.c. That function is the
   authority; this exists because the generated configuration has to name the
   exact topics the firmware publishes to, and the browser already holds
   everything needed to work them out. test/test_topic.js keeps the two from
   drifting apart -- a mismatch here produces a configuration evcc accepts and
   then silently never fills. */
function expandTopic(base, format, ecu, did, didName, device, override) {
  /* A per-datapoint override replaces the formatted part, not the base. */
  if (override) return `${base}/${override}`;

  let out = `${base}/`;
  const f = format || "{didName}";
  for (let i = 0; i < f.length; ) {
    if (f[i] !== "{") { out += f[i++]; continue; }
    const close = f.indexOf("}", i);
    const body = close < 0 ? "" : f.slice(i + 1, close);
    const colon = body.indexOf(":");
    const name = colon < 0 ? body : body.slice(0, colon);
    const spec = colon < 0 ? "" : body.slice(colon + 1);
    let sub = null;
    if (close < 0) { out += f[i++]; continue; }
    if (name === "didName") {
      sub = didName || "";
    } else if (name === "didNumber") {
      /* The documented spec is {didNumber:04d}; any 0-prefixed spec is a
         zero-padded width. */
      const width = spec.startsWith("0") ? parseInt(spec, 10) || 0 : 0;
      sub = String(did).padStart(width, "0");
    } else if (name === "ecuAddr") {
      if (spec.includes("X")) sub = ecu.toString(16).toUpperCase().padStart(3, "0");
      else if (spec.includes("x")) sub = ecu.toString(16).padStart(3, "0");
      else sub = String(ecu);            /* open3e's plain int */
    } else if (name === "device") {
      sub = device || "";
    }
    /* An unknown placeholder is copied through, so a typo shows up in the
       topic instead of vanishing -- same as the firmware does. */
    if (sub === null) { out += f[i++]; continue; }
    out += sub;
    i = close + 1;
  }
  return out;
}
/* @topic-expansion-end */

/* Where one role's value actually lands, and how to pick it out. */
/* Aus den Eingabefeldern, nicht aus dem letzten Speichern: so zeigt der
   Erzeuger, was mit den gerade eingetragenen Einstellungen herauskäme. */
function mqttField(id, dflt) {
  const el = $(id);
  return (el && el.value) ? el.value : dflt;
}

function evccSource(role, cfg, didName, device) {
  const base = mqttField("mq-base", "open3e");
  const topic = expandTopic(base, mqttField("mq-format", "{didName}"),
                            cfg.ecu, role.did, didName, device, cfg.topic);
  if (cfg.mode === "flat") {
    /* Every scalar leaf is its own topic with a bare value; a datapoint that
       is itself a scalar publishes at the topic directly. */
    return { topic: role.field ? `${topic}/${role.field}` : topic, jq: null };
  }
  /* json: one topic per datapoint. A complex type is an object, a scalar is a
     plain JSON value with nothing to select. */
  const jq = role.field
    ? (/[^A-Za-z0-9_]/.test(role.field) ? `."${role.field}"` : `.${role.field}`)
    : null;
  return { topic, jq };
}

function yamlStr(s) { return `"${String(s).replace(/"/g, '\\"')}"`; }

function evccConfig() {
  const found = [], missing = [];
  for (const role of EVCC_ROLES) {
    let hit = null;
    for (const cfg of selection.values()) {
      if (cfg.did === role.did && cfg.enabled && !cfg.type) { hit = cfg; break; }
    }
    if (!hit) { missing.push(role); continue; }
    const dev = (system.devices || []).find((d) => d.addr === hit.ecu);
    const src = evccSource(role, hit, didNames[role.did] || role.name,
                           dev ? dev.name : "");
    /* Three poll intervals, so a single missed reading does not make evcc
       drop the value -- floored at a minute because evcc re-reads far more
       often than this gateway polls. */
    const timeout = Math.max(60, (Number(hit.interval) || 60) * 3);
    found.push({ role, cfg: hit, ...src, timeout });
  }
  return { found, missing };
}

function evccYaml() {
  const { found, missing } = evccConfig();
  const get = (k) => found.find((f) => f.role.key === k);
  const grid = get("grid"), pv = get("pv"), bat = get("battery"), soc = get("soc");
  const cmnd = mqttField("mq-cmnd", "open3e/cmnd");
  const L = [];

  const reading = (f, indent) => {
    const p = " ".repeat(indent);
    L.push(`${p}source: mqtt`);
    L.push(`${p}topic: ${yamlStr(f.topic)}`);
    if (f.jq) L.push(`${p}jq: ${yamlStr(f.jq)}`);
    L.push(`${p}timeout: ${f.timeout}s`);
  };

  L.push("# Von diesem Gateway erzeugt. Topics und Feldnamen stammen aus der");
  L.push("# tatsächlichen Datenpunkt-Auswahl dieses Geräts, nicht aus einer Vorlage.");
  L.push("#");
  L.push("# Vorzeichen: evcc erwartet positiv für einströmend -- Netzbezug,");
  L.push("# PV-Erzeugung, Speicherentladung. Die Datenpunkte der VX3 halten sich");
  L.push("# daran, deshalb steht nirgends ein scale: -1.");
  if (missing.length) {
    L.push("#");
    L.push("# FEHLT: " + missing.map((r) => `${r.label} (DID ${r.did})`).join(", "));
    L.push("# Diese Datenpunkte sind nicht aktiv -- unter Datenpunkte einschalten,");
    L.push("# dann erscheinen sie hier von selbst.");
  }
  L.push("");
  L.push("meters:");

  if (grid) {
    L.push("  - name: vx3grid");
    L.push("    type: custom");
    L.push("    power:");
    reading(grid, 6);
  }
  if (pv) {
    L.push("  - name: vx3pv");
    L.push("    type: custom");
    L.push("    power:");
    reading(pv, 6);
  }
  if (bat || soc) {
    L.push("  - name: vx3battery");
    L.push("    type: custom");
    if (bat) { L.push("    power:"); reading(bat, 6); }
    if (soc) { L.push("    soc:"); reading(soc, 6); }
    if (bat && soc) {
      /* Ausdrücklich, statt sich auf den Speicher-ECU aus den
         Systemeinstellungen zu verlassen: ist der nicht gesetzt, landet der
         Befehl auf der Vorgabe 0x680, und die trägt auf einer Anlage mit
         Wärmepumpe keinen der beiden Datenpunkte. Der Hold meldet sich dann
         als aktiv, schreibt nie, und verschwindet nach fünf Fehlversuchen.
         An einer laufenden Anlage nachgemessen. */
      const addr = "0x" + bat.cfg.ecu.toString(16).toUpperCase().padStart(3, "0");
      L.push("");
      L.push("    # Steuerung. evcc ruft batteryMode mit einer Zahl auf:");
      L.push("    # 1 = normal, 2 = hold (nicht entladen), 3 = charge (aus dem Netz).");
      L.push("    #");
      L.push(`    # Die Adresse ${addr} steht ausdrücklich in jeder Nutzlast: ohne sie`);
      L.push("    # geht der Befehl an die Vorgabe 0x680, die diese Datenpunkte nicht");
      L.push("    # hat -- der Hold meldet sich dann aktiv, schreibt nie und ist nach");
      L.push("    # fünf Fehlversuchen weg.");
      L.push("    #");
      L.push("    # Der watchdog ist nicht optional: evcc schickt batteryMode nur bei");
      L.push("    # Änderung, und ein Hold in diesem Gateway endet nach höchstens einer");
      L.push("    # Stunde von selbst -- absichtlich, damit nichts unbeaufsichtigt");
      L.push("    # weiterläuft. Alle 10 Minuten nachschreiben hält den Hold am Leben");
      L.push("    # und lässt die Frist trotzdem greifen, wenn evcc verschwindet.");
      L.push("    batterymodes: [normal, hold, charge]");
      L.push("    batterymode:");
      L.push("      source: watchdog");
      L.push("      timeout: 20m");
      L.push("      reset: 1");
      L.push("      set:");
      L.push("        source: switch");
      L.push("        switch:");
      const cases = [
        [1, "normal -- Hold beenden, die Anlage regelt wieder selbst",
         `{"mode":"grid","addr":"${addr}","stop":true}`],
        [2, "hold -- nicht entladen, PV-Laden bleibt erlaubt",
         `{"mode":"storage","addr":"${addr}","storage":"nur laden","seconds":3600}`],
        [3, "charge -- aus dem Netz laden; Leistung nach Bedarf ändern",
         `{"mode":"grid","addr":"${addr}","watts":-2000,"seconds":3600}`],
      ];
      for (const [n, why, payload] of cases) {
        L.push(`          # ${why}`);
        L.push(`          - case: ${n}`);
        L.push("            set:");
        L.push("              source: mqtt");
        L.push(`              topic: ${yamlStr(cmnd)}`);
        L.push(`              payload: '${payload}'`);
      }
    }
  }

  L.push("");
  L.push("site:");
  L.push("  meters:");
  if (grid) L.push("    grid: vx3grid");
  if (pv) L.push("    pv:\n      - vx3pv");
  if (bat || soc) L.push("    battery:\n      - vx3battery");
  return L.join("\n") + "\n";
}

function renderEvcc() {
  const out = $("evcc-yaml");
  if (!out) return;
  const { found, missing } = evccConfig();
  out.textContent = found.length ? evccYaml()
    : "Kein passender Datenpunkt aktiv – siehe unten.";

  const st = $("evcc-status");
  st.innerHTML = "";
  for (const role of EVCC_ROLES) {
    const f = found.find((x) => x.role.key === role.key);
    const li = document.createElement("div");
    li.className = "kv";
    li.innerHTML = `<div class="k">${role.label}</div><div class="v ${f ? "" : "muted"}">`
      + (f ? `DID ${role.did} ✓` : `DID ${role.did} fehlt`) + "</div>";
    st.appendChild(li);
  }
  const hint = $("evcc-missing");
  hint.hidden = missing.length === 0;
  if (missing.length) {
    hint.innerHTML = "Nicht aktiv: "
      + missing.map((r) => `<b>${r.label}</b> – DID ${r.did} <span class="mono">${r.name}</span>`).join(", ")
      + ". Unter <i>Datenpunkte</i> einschalten und speichern, dann erscheint der Block hier von selbst.";
  }
  const ctl = $("evcc-control");
  const hasBoth = found.some((f) => f.role.key === "battery")
               && found.some((f) => f.role.key === "soc");
  ctl.hidden = hasBoth;
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */

/* Oberfläche und Firmware werden getrennt aktualisiert -- der Dateiweg für die
   Weboberfläche ist laut README der Alltagsfall. Damit kann die Seite
   Bedienelemente für Funktionen zeigen, die die laufende Firmware nicht hat.
   Das ist genau einmal passiert und sah aus wie ein Fehler der Oberfläche:
   Kontaktname eintragen, "Einstellungen gespeichert", neu laden, Feld leer.
   Also wird jeder Abschnitt gegen das abgeglichen, was /api/settings
   tatsächlich liefert -- fehlt er dort, kann die Firmware ihn nicht. */
const CAPS = {};

function markUnsupported(cardId, present, what) {
  CAPS[cardId] = present;
  const card = $(cardId);
  if (!card) return;
  let note = card.querySelector(".cap-note");
  if (present) {
    if (note) note.remove();
    for (const el of card.querySelectorAll("input,select,button")) el.disabled = false;
    return;
  }
  if (!note) {
    note = document.createElement("div");
    note.className = "warnbox cap-note";
    note.innerHTML = `<b>Diese Firmware kennt ${what} nicht.</b> Die Felder unten
      sind deshalb gesperrt — eingetragene Werte würden beim Speichern
      stillschweigend verworfen. Unter <i>Debug → Firmware-Update</i>
      aktualisieren.`;
    card.insertBefore(note, card.querySelector("h2").nextSibling);
  }
  for (const el of card.querySelectorAll("input,select,button")) el.disabled = true;
}

function markFieldsUnsupported(ids, present, noteId, text) {
  CAPS[noteId] = present;
  let note = $(noteId);
  for (const id of ids) {
    const el = $(id);
    if (el) el.disabled = !present;
  }
  if (present) {
    if (note) note.remove();
    return;
  }
  if (!note) {
    const anchor = $(ids[0]);
    if (!anchor) return;
    note = document.createElement("p");
    note.id = noteId;
    note.className = "small";
    note.style.color = "var(--warn)";
    note.textContent = text + " Die Felder sind gesperrt; Firmware aktualisieren.";
    (anchor.closest("label") || anchor).insertAdjacentElement("afterend", note);
  }
}

async function loadSettings() {
  const s = await api("/api/settings");
  $("mq-enabled").checked = s.mqtt.enabled;
  $("mq-host").value = s.mqtt.host;
  $("mq-port").value = s.mqtt.port;
  $("mq-user").value = s.mqtt.user;
  $("mq-pass").placeholder = s.mqtt.hasPass ? "unverändert" : "";
  $("mq-base").value = s.mqtt.baseTopic;
  $("mq-format").value = s.mqtt.format;
  $("mq-cmnd").value = s.mqtt.cmndTopic;
  $("ha-on").checked = s.mqtt.haDiscovery;
  $("ha-prefix").value = s.mqtt.haPrefix;
  $("sys-write").checked = s.system.writeEnabled;
  $("sys-rawwrite").checked = s.system.rawWriteEnabled;
  $("sys-rawids").value = s.system.rawCanIds || "";
  $("sys-em380").checked = s.system.em380Enabled;
  $("co-on").checked = s.system.collectEnabled;
  $("co-id").value = s.system.collectCanIds || "0x451,0x441";
  $("sys-tz").value = s.system.tz;
  /* Die rohe API sitzt mitten in der System-Karte zwischen Feldern, die jede
     Firmware kennt -- also einzeln sperren statt die ganze Karte. */
  markFieldsUnsupported(["sys-rawwrite", "sys-rawids"],
                        "rawWriteEnabled" in s.system, "raw-note",
                        "Rohes Schreiben und das Roh-Relay kennt diese Firmware nicht.");
  markUnsupported("card-contacts", "contacts" in s.system, "die Kontakteingänge");
  markUnsupported("card-knx", "knx" in s.system, "KNX");
  renderEvcc();
  (s.system.contacts || []).forEach((c, i) => {
    if (!$(`ct${i}-on`)) return;
    $(`ct${i}-on`).checked = c.enabled;
    $(`ct${i}-mode`).value = c.mode || "input";
    $(`ct${i}-name`).value = c.name || "";
    $(`ct${i}-class`).value = c.deviceClass || "";
    $(`ct${i}-wire`).value = c.wire || "gnd";
    $(`ct${i}-rel`).value = c.releaseMs || 150;
    $(`ct${i}-alow`).value = c.activeLow ? "low" : "high";
    $(`ct${i}-ga`).value = c.knxGa || "";
    contactModeChanged(i);
  });
  const k = s.system.knx || {};
  $("knx-on").checked = !!k.enabled;
  $("knx-mode").value = k.mode || "tunnelling";
  $("knx-gw").value = k.gateway || "";
  $("knx-port").value = k.port || 3671;
  $("knx-src").value = k.source || "";
  $("dbg-wstate").textContent =
    (s.system.writeEnabled
      ? "Schreiben ist freigegeben."
      : "Schreiben ist gesperrt – in den Einstellungen freigeben.") +
    (s.system.rawWriteEnabled ? " Rohes Schreiben ist ebenfalls freigegeben." : "");
}

/* Show the fields that belong to the chosen direction. The row keeps its
   shape otherwise, so switching back and forth does not reflow the card. */
function contactModeChanged(i) {
  const out = $(`ct${i}-mode`).value === "output";
  const row = $(`ct${i}-mode`).closest(".row");
  row.querySelectorAll(".ct-in").forEach((el) => { el.hidden = out; });
  row.querySelectorAll(".ct-out").forEach((el) => { el.hidden = !out; });
  $(`ct${i}-name`).placeholder = out ? `Ausgang ${i + 1}` : `Eingang ${i + 1}`;
}

[0, 1].forEach((i) => {
  const sel = $(`ct${i}-mode`);
  if (sel) sel.addEventListener("change", () => contactModeChanged(i));
  const btn = $(`ct${i}-toggle`);
  if (btn) btn.addEventListener("click", async () => {
    btn.disabled = true;
    try {
      await api("/api/output", { method: "POST", body: JSON.stringify({
        gpio: Number(btn.dataset.gpio), on: btn.dataset.on === "1" }) });
      /* The pin task applies it within a few milliseconds; refreshing right
         away shows the new state instead of waiting a whole poll. */
      setTimeout(poll, 200);
    } catch (e) {
      toast(`Schalten fehlgeschlagen: ${e.message || e}`, "err");
    } finally {
      btn.disabled = false;
    }
  });
});

async function saveSettings() {
  const body = {
    mqtt: {
      enabled: $("mq-enabled").checked,
      host: $("mq-host").value,
      port: Number($("mq-port").value) || 1883,
      user: $("mq-user").value,
      baseTopic: $("mq-base").value,
      format: $("mq-format").value,
      cmndTopic: $("mq-cmnd").value,
      haDiscovery: $("ha-on").checked,
      haPrefix: $("ha-prefix").value,
    },
    system: {
      writeEnabled: $("sys-write").checked,
      ...(CAPS["raw-note"] === false ? {} : {
        rawWriteEnabled: $("sys-rawwrite").checked,
        rawCanIds: $("sys-rawids").value,
      }),
      em380Enabled: $("sys-em380").checked,
      tz: $("sys-tz").value,
      ...(CAPS["card-contacts"] === false ? {} : { contacts: [0, 1].map((i) => ({
        enabled: $(`ct${i}-on`).checked,
        mode: $(`ct${i}-mode`).value,
        activeLow: $(`ct${i}-alow`).value === "low",
        name: $(`ct${i}-name`).value,
        deviceClass: $(`ct${i}-class`).value,
        wire: $(`ct${i}-wire`).value,
        releaseMs: Number($(`ct${i}-rel`).value) || 150,
        knxGa: $(`ct${i}-ga`).value,
      })) }),
      ...(CAPS["card-knx"] === false ? {} : { knx: {
        enabled: $("knx-on").checked,
        mode: $("knx-mode").value,
        gateway: $("knx-gw").value,
        port: Number($("knx-port").value) || 3671,
        source: $("knx-src").value,
      } }),
    },
  };
  /* Only send the password when one was actually typed, so the stored value
     survives an unrelated settings change. */
  if ($("mq-pass").value) body.mqtt.pass = $("mq-pass").value;

  try {
    await api("/api/settings", { method: "PUT", body: JSON.stringify(body) });
    $("mq-pass").value = "";
    toast("Einstellungen gespeichert.", "ok");
    loadSettings();
  } catch (e) {
    toast(e.message, "err");
  }
}

/* ------------------------------------------------------------------ */

function initApp() {
  $("app").hidden = false;

  /* Ausdrücklich "header nav": der Selektor "nav button" fängt sonst auch die
     Unter-Navigation der Einstellungen mit, und die hat kein data-tab -- der
     erste Klick dort liefe auf tab-undefined. */
  for (const b of document.querySelectorAll("header nav button")) {
    b.onclick = () => {
      document.querySelectorAll("header nav button").forEach((x) => x.classList.remove("active"));
      document.querySelectorAll("main > section").forEach((x) => x.classList.remove("active"));
      b.classList.add("active");
      $(`tab-${b.dataset.tab}`).classList.add("active");
    };
  }

  /* Unterteilung der Einstellungen. Der zuletzt geöffnete Bereich wird
     gemerkt: wer an MQTT arbeitet, landet nach jedem Neuladen sonst wieder
     bei Anbindung und sucht sich zurück. */
  const subButtons = document.querySelectorAll(".subnav button");
  const showSub = (name) => {
    let hit = false;
    for (const b of subButtons) {
      const on = b.dataset.sub === name;
      b.classList.toggle("active", on);
      const panel = $(`sub-${b.dataset.sub}`);
      if (panel) panel.classList.toggle("active", on);
      hit = hit || on;
    }
    return hit;
  };
  for (const b of subButtons) {
    b.onclick = () => {
      showSub(b.dataset.sub);
      try { localStorage.setItem("settingsSub", b.dataset.sub); } catch (e) { /* egal */ }
    };
  }
  try {
    const last = localStorage.getItem("settingsSub");
    if (last) showSub(last);
  } catch (e) { /* privater Modus: dann eben immer Anbindung */ }

  $("diag-run").onclick = async () => {
    const out = $("diag-out"), btn = $("diag-run");
    out.hidden = false;
    out.innerHTML = "<p class='muted small'>Läuft… Loopback-Test, dann "
                  + "Mithören auf vier Bitraten.</p>";
    btn.disabled = true;
    try {
      const d = await api("/api/candiag", { method: "POST" });
      const best = (d.rates || []).reduce((a, b) => (b.frames > (a?.frames ?? -1) ? b : a), null);

      /* The two tests answer different questions, so the verdict combines
         them rather than reporting each in isolation. */
      let verdict, kind;
      if (!d.loopback) {
        kind = "err";
        verdict = "Der Controller empfängt nicht einmal sein eigenes Frame. "
                + "Das ist ein Firmware- oder Pin-Problem, nicht die Verkabelung.";
      } else if (!best || best.frames === 0) {
        kind = "err";
        verdict = "Controller in Ordnung, aber auf keiner Bitrate kommt auch nur "
                + "ein Frame an. Der Bus ist also nicht erreichbar: CAN-H/CAN-L "
                + "vertauscht, falscher CAN-Strang, fehlender Gleichtaktbezug, "
                + "oder am Bus sendet gerade niemand.";
      } else if (best.bitrate !== 250000) {
        kind = "warn";
        verdict = `Frames kommen bei ${best.bitrate / 1000} kBit/s an, nicht bei `
                + "250 kBit/s. Der Bus läuft mit einer anderen Bitrate als "
                + "E3-Geräte üblicherweise verwenden.";
      } else {
        kind = "ok";
        verdict = "Verkabelung und Bitrate stimmen — es kommen Frames an. "
                + "Bleibt der TX-Fehlerzähler trotzdem stehen, quittiert niemand "
                + "das Senden; das deutet auf Abschlusswiderstand oder einen Bus, "
                + "auf dem sonst kein aktiver Knoten hängt.";
      }

      const rows = (d.rates || []).map((r) =>
        `<tr><td class="mono">${r.bitrate / 1000} kBit/s</td>`
        + `<td class="num">${r.frames}</td>`
        + `<td class="mono small">${r.ids.length ? r.ids.join(", ") : "–"}</td></tr>`).join("");

      out.innerHTML =
        `<p class="pill ${kind}" style="display:inline-block">Loopback: `
        + `${d.loopback ? "bestanden" : "fehlgeschlagen"}</p>`
        + `<div class="warnbox" style="margin-top:10px">${verdict}</div>`
        + '<div class="tablewrap"><table><thead><tr><th>Bitrate</th>'
        + '<th class="num">Frames in 2 s</th><th>Gesehene CAN-IDs</th></tr></thead>'
        + `<tbody>${rows}</tbody></table></div>`;
    } catch (e) {
      out.innerHTML = `<div class="warnbox">${e.message}</div>`;
    }
    btn.disabled = false;
  };

  $("scan-start").onclick = async () => {
    const mode = $("scan-mode").value;
    const lo = parseId($("scan-lo").value), hi = parseId($("scan-hi").value);
    const addrs = Math.max(1, hi - lo + 1);
    if (addrs > 256 && !confirm(
        `${addrs} Adressen werden abgefragt — die Gerätesuche allein dauert `
      + `etwa ${Math.ceil(addrs * 0.2 / 60)} Minuten, danach kommt noch die `
      + "Datenpunkt-Phase je gefundenem Gerät. Fortfahren?")) return;
    if (mode === "full" && !confirm(
        "Ein Vollscan dauert 10 bis 20 Minuten pro Gerät und belegt so lange den "
      + "CAN-Bus. Trotzdem starten?")) return;
    try {
      await api("/api/scan", {
        method: "POST",
        body: JSON.stringify({
          mode,
          cobFirst: parseId($("scan-lo").value),
          cobLast: parseId($("scan-hi").value),
        }),
      });
      scanning = true;
      toast("Scan gestartet.", "ok");
    } catch (e) { toast(e.message, "err"); }
  };
  $("scan-abort").onclick = () => api("/api/scan/abort", { method: "POST" });
  $("scan-wide").onclick = () => {
    $("scan-lo").value = "0x000";
    $("scan-hi").value = "0x7FF";
  };

  /* Any change of scope starts the paging over, otherwise switching devices
     would keep whatever page depth the previous one was scrolled to. */
  const rerender = () => { pointLimit = POINT_PAGE; renderPoints(); };
  $("pt-ecu").onchange = rerender;
  $("pt-filter").oninput = rerender;
  $("pt-only-active").onchange = rerender;
  $("pt-only-known").onchange = rerender;
  $("pt-more").onclick = () => { pointLimit += POINT_PAGE; renderPoints(); };
  $("pt-save").onclick = () => saveSelection($("pt-save"));
  $("em-save").onclick = () => saveSelection($("em-save"));
  $("co-save-sel").onclick = () => saveSelection($("co-save-sel"));
  $("co-save").onclick = async () => {
    try {
      await api("/api/settings", {
        method: "PUT",
        body: JSON.stringify({ system: {
          collectEnabled: $("co-on").checked,
          collectCanIds: $("co-id").value,
        } }),
      });
      toast("Übernommen.", "ok");
      setTimeout(loadCollect, 500);
    } catch (e) { toast(e.message, "err"); }
  };
  $("em-only-seen").onchange = () => loadMeter().catch(() => {});

  $("set-save").onclick = saveSettings;
  $("evcc-copy").onclick = async () => {
    try {
      await navigator.clipboard.writeText($("evcc-yaml").textContent);
      toast("Kopiert.", "ok");
    } catch (e) {
      /* Ohne HTTPS gibt es keine Ablage-API; dann markieren statt scheitern. */
      const r = document.createRange();
      r.selectNodeContents($("evcc-yaml"));
      window.getSelection().removeAllRanges();
      window.getSelection().addRange(r);
      toast("Markiert – mit Strg+C kopieren.", "warn");
    }
  };
  $("crash-clear").onclick = async () => {
    try {
      await api("/api/crash", { method: "DELETE" });
      $("crash-box").hidden = true;
      toast("Verworfen.", "ok");
    } catch (e) { toast(e.message, "err"); }
  };

  $("dev-restart").onclick = async () => {
    if (!confirm("Gerät neu starten?")) return;
    await api("/api/restart", { method: "POST" }).catch(() => {});
    toast("Neustart läuft…");
  };
  $("dev-forget").onclick = async () => {
    if (!confirm(
        "WLAN-Zugangsdaten löschen? Das Gerät startet neu und spannt wieder "
      + "den Einrichtungs-Hotspot auf.")) return;
    await api("/api/wifi/forget", { method: "POST" }).catch(() => {});
  };

  /* fetch() reports no upload progress, so the firmware upload uses XHR: a
     megabyte over a basement Wi-Fi link takes long enough that a silent wait
     looks like a hang. */
  const otaFile = $("ota-file");
  otaFile.onchange = () => { $("ota-start").disabled = !otaFile.files.length; };

  $("ota-start").onclick = () => {
    const file = otaFile.files[0];
    if (!file) return;
    if (!confirm(
        `${file.name} (${Math.round(file.size / 1024)} KiB) hochladen?\n\n`
      + "Das Gerät startet danach neu. Die laufende Firmware bleibt bis zum "
      + "Abschluss unangetastet.")) return;

    const bar = $("ota-bar"), msg = $("ota-msg");
    $("ota-progress").hidden = false;
    $("ota-start").disabled = true;
    otaFile.disabled = true;

    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/ota");
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = (e) => {
      if (!e.lengthComputable) return;
      const pct = (100 * e.loaded) / e.total;
      bar.style.width = `${pct}%`;
      msg.textContent = `${Math.round(e.loaded / 1024)} von `
                      + `${Math.round(e.total / 1024)} KiB (${Math.round(pct)} %)`;
    };
    xhr.onload = () => {
      let body = {};
      try { body = JSON.parse(xhr.responseText); } catch { /* ignore */ }
      if (xhr.status >= 200 && xhr.status < 300) {
        bar.style.width = "100%";
        msg.textContent = "Übertragen. Das Gerät startet neu – die Seite lädt "
                        + "in etwa 20 Sekunden von selbst neu.";
        toast("Update eingespielt, Neustart läuft.", "ok");
        setTimeout(() => location.reload(), 20000);
      } else {
        msg.textContent = body.error || `Fehlgeschlagen (HTTP ${xhr.status})`;
        toast(msg.textContent, "err");
        $("ota-start").disabled = false;
        otaFile.disabled = false;
      }
    };
    xhr.onerror = () => {
      msg.textContent = "Verbindung während des Uploads abgebrochen.";
      toast(msg.textContent, "err");
      $("ota-start").disabled = false;
      otaFile.disabled = false;
    };
    xhr.send(file);
  };

  /* Upload with progress; fetch() cannot report it. */
  const upload = (url, blob, bar, msg, onDone) => new Promise((resolve) => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", url);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    /* Without this a request the device never answers just spins forever and
       the page looks stuck with no explanation. */
    xhr.timeout = 120000;
    xhr.ontimeout = () => {
      if (onDone) onDone(false, { error: "Zeitüberschreitung – das Gerät hat nicht geantwortet." }, 0);
      resolve(false);
    };
    xhr.upload.onprogress = (e) => {
      if (!e.lengthComputable || !bar) return;
      const pct = (100 * e.loaded) / e.total;
      bar.style.width = `${pct}%`;
      if (msg) {
        msg.textContent = `${Math.round(e.loaded / 1024)} von `
                        + `${Math.round(e.total / 1024)} KiB (${Math.round(pct)} %)`;
      }
    };
    xhr.onload = () => {
      let body = {};
      try { body = JSON.parse(xhr.responseText); } catch { /* ignore */ }
      const ok = xhr.status >= 200 && xhr.status < 300;
      if (onDone) onDone(ok, body, xhr.status);
      resolve(ok);
    };
    xhr.onerror = () => { if (onDone) onDone(false, {}, 0); resolve(false); };
    xhr.send(blob);
  });

  const fsFiles = $("fs-files");
  fsFiles.onchange = () => { $("fs-upload").disabled = !fsFiles.files.length; };
  $("fs-upload").onclick = async () => {
    const files = [...fsFiles.files];
    $("fs-upload").disabled = true;
    let done = 0;
    for (const f of files) {
      $("fs-msg").textContent = `Lade ${f.name} …`;
      const path = `/data/www/${encodeURIComponent(f.name)}`;
      const ok = await upload(`/api/fs?path=${path}`, f, null, null,
        (ok, body) => { if (!ok) toast(`${f.name}: ${body.error || "fehlgeschlagen"}`, "err"); });
      if (ok) done++;
    }
    $("fs-msg").textContent = `${done} von ${files.length} Dateien ersetzt.`
                            + (done ? " Seite neu laden, um sie zu sehen." : "");
    if (done) toast("Hochgeladen. Mit Strg+F5 neu laden.", "ok");
    $("fs-upload").disabled = false;
  };

  const stFile = $("st-file");
  stFile.onchange = () => { $("st-upload").disabled = !stFile.files.length; };
  $("st-upload").onclick = async () => {
    const f = stFile.files[0];
    if (!f) return;
    if (!confirm(
        `${f.name} (${Math.round(f.size / 1024)} KiB) auf die Storage-Partition schreiben?\n\n`
      + "Datenpunkt-Auswahl und Scan-Ergebnis gehen dabei verloren. "
      + "Das Gerät startet danach neu.")) return;
    $("st-progress").hidden = false;
    $("st-upload").disabled = true;
    await upload("/api/storage", f, $("st-bar"), $("st-msg"), (ok, body, status) => {
      if (ok) {
        $("st-msg").textContent = "Geschrieben. Neustart läuft – die Seite lädt "
                                + "in etwa 20 Sekunden neu.";
        setTimeout(() => location.reload(), 20000);
      } else {
        $("st-msg").textContent = body.error || `Fehlgeschlagen (HTTP ${status})`;
        $("st-upload").disabled = false;
      }
    });
  };

  $("tr-start").onclick = async () => {
    try {
      await api("/api/trace", {
        method: "POST",
        body: JSON.stringify({
          action: "start",
          frames: Number($("tr-frames").value) || 65536,
          idLow: parseId($("tr-lo").value),
          idHigh: parseId($("tr-hi").value),
          trigger: $("tr-trigger").value,
          postFrames: Number($("tr-post").value) || 0,
          learnS: Number($("tr-learn").value) || 600,
          excludeOwn: $("tr-own").checked,
        }),
      });
      toast("Aufzeichnung läuft.", "ok");
      traceStatus();
    } catch (e) { toast(e.message, "err"); }
  };
  $("tr-stop").onclick = async () => {
    try {
      await api("/api/trace", { method: "POST", body: JSON.stringify({ action: "stop" }) });
      toast("Gestoppt.", "ok");
      await traceStatus();
      await traceLoad();
    } catch (e) { toast(e.message, "err"); }
  };
  $("tr-load").onclick = traceLoad;
  $("tr-ids").onclick = async () => {
    try {
      const ids = await api("/api/trace/ids");
      const tb = document.querySelector("#tr-idtable tbody");
      tb.innerHTML = "";
      for (const d of ids) {
        const span = Math.max(1, (d.lastUs - d.firstUs) / 1e6);
        /* A byte that never varied is where a change would mean something. */
        const bytes = [...Array(8).keys()]
          .map((k) => ((d.varying >> k) & 1) ? "·" : "K").join(" ");
        tb.append(el("tr", {},
          el("td", { className: "mono", textContent: `0x${d.id.toString(16).toUpperCase()}` }),
          el("td", { className: "num", textContent: d.count }),
          el("td", { className: "num", textContent: (d.count / span).toFixed(1) }),
          el("td", { className: "mono small", textContent: bytes }),
          el("td", { className: "mono small", textContent: d.last })));
      }
      if (!ids.length) toast("Noch keine Frames aufgezeichnet.", "err");
    } catch (e) { toast(e.message, "err"); }
  };
  $("tr-dl").onclick = () => { window.location = "/api/trace/dump"; };
  $("tr-writes").onchange = renderTrace;
  $("tr-changes").onchange = renderTrace;

  $("exp-run").onclick = () => {
    /* A plain navigation, so the browser's own download handling applies and
       the Content-Disposition filename is honoured. */
    const scan = $("exp-scan").checked ? 1 : 0;
    window.location = `/api/export?scan=${scan}`;
  };

  const impFile = $("imp-file");
  impFile.onchange = () => { $("imp-run").disabled = !impFile.files.length; };
  $("imp-run").onclick = async () => {
    const f = impFile.files[0];
    if (!f) return;
    if (!confirm(
        `${f.name} einspielen?\n\n`
      + "Überschreibt WLAN, MQTT, Systemeinstellungen und die Datenpunkt-Auswahl. "
      + "Das Gerät startet danach neu — falls die Sicherung ein anderes WLAN "
      + "enthält, ist es anschließend dort erreichbar.")) return;

    $("imp-run").disabled = true;
    $("imp-msg").textContent = "Wird eingespielt…";
    await upload("/api/import", f, null, null, (ok, body, status) => {
      if (ok) {
        $("imp-msg").textContent = "Eingespielt. Neustart läuft – die Seite lädt "
                                 + "in etwa 20 Sekunden neu.";
        toast("Wiederhergestellt, Neustart läuft.", "ok");
        setTimeout(() => location.reload(), 20000);
      } else {
        $("imp-msg").textContent = body.error || `Fehlgeschlagen (HTTP ${status})`;
        toast($("imp-msg").textContent, "err");
        $("imp-run").disabled = false;
      }
    });
  };

  for (const b of document.querySelectorAll("[data-reset]")) {
    const what = b.dataset.reset;
    const labels = {
      points: "die Datenpunkt-Auswahl",
      scan: "das Scan-Ergebnis und die Auswahl",
      mqtt: "die MQTT-Einstellungen",
      all: "alles außer den WLAN-Zugangsdaten",
    };
    b.onclick = async () => {
      if (!confirm(`Wirklich ${labels[what]} zurücksetzen?`)) return;
      try {
        await api("/api/reset", { method: "POST", body: JSON.stringify({ what }) });
        toast("Zurückgesetzt.", "ok");
        if (what === "all") {
          setTimeout(() => location.reload(), 5000);
        } else {
          await loadPoints();
          await loadSystem();
          await loadMeter();
          await loadSettings();
        }
      } catch (e) { toast(e.message, "err"); }
    };
  }

  $("dbg-read").onclick = async () => {
    $("dbg-out").textContent = "…";
    try {
      const r = await api(`/api/read?ecu=${$("dbg-ecu").value}&did=${$("dbg-did").value}`);
      $("dbg-out").textContent = JSON.stringify(r.value, null, 2);
      $("dbg-value").value = JSON.stringify(r.value);
    } catch (e) {
      $("dbg-out").textContent = e.message;
    }
  };
  /* The write card has no fields of its own -- it acts on the ECU and DID
     above it. Showing the target removes the one guess that matters here:
     writing the right value to the wrong datapoint is not a typo you notice. */
  showTarget = () => {
    const did = Number($("dbg-did").value);
    const name = didNames[did];
    $("dbg-target").textContent = $("dbg-ecu").value
      ? `ECU 0x${Number($("dbg-ecu").value).toString(16).toUpperCase()}, DID ${did}`
        + (name ? ` (${name})` : "")
      : "–";
  };
  $("dbg-did").oninput = showTarget;
  $("dbg-ecu").onchange = showTarget;
  showTarget();

  $("gh-start").onclick = async () => {
    const watts = Number($("gh-watts").value);
    const minutes = Number($("gh-min").value);
    const what = watts < 0 ? `${-watts} W aus dem Netz ziehen`
                           : `${watts} W ins Netz einspeisen`;
    if (!confirm(`${what}, ${minutes} Minuten lang?\n\n`
               + `Die Eigenverbrauchsregelung wird so lange überschrieben. `
               + `Nach Ablauf übernimmt sie von selbst wieder.`)) return;
    try {
      await api("/api/grid", { method: "POST", body: JSON.stringify({
        ecu: Number($("gh-ecu").value), watts, seconds: minutes * 60 }) });
      toast("Halten gestartet.", "ok");
    } catch (e) { toast(e.message, "err"); }
  };
  $("sh-set").onclick = async () => {
    const mode = $("sh-mode").value;
    const minutes = Number($("sh-min").value);
    if (mode !== "normal"
        && !confirm(`Speicher auf "${mode}" setzen, ${minutes} Minuten lang?\n\n`
                  + `Die Regelung der Anlage wird so lange überschrieben. `
                  + `Nach Ablauf übernimmt sie von selbst wieder.`)) return;
    try {
      await api("/api/grid", { method: "POST", body: JSON.stringify({
        ecu: Number($("gh-ecu").value), storage: mode, seconds: minutes * 60 }) });
      toast("Gesetzt.", "ok");
    } catch (e) { toast(e.message, "err"); }
  };

  $("gh-stop").onclick = async () => {
    try {
      await api("/api/grid", { method: "POST", body: JSON.stringify({ stop: true }) });
      toast("Beendet.", "ok");
    } catch (e) { toast(e.message, "err"); }
  };

  $("dbg-write").onclick = async () => {
    let value;
    try { value = JSON.parse($("dbg-value").value); }
    catch { return toast("Der Wert ist kein gültiges JSON.", "err"); }
    const force = $("dbg-force").checked;
    if (!confirm(`DID ${$("dbg-did").value} auf ${$("dbg-value").value} schreiben?`
                 + (force ? "\n\nDer Schreibschutz der Datenbank wird dabei übergangen."
                          : ""))) return;
    try {
      await api("/api/write", {
        method: "POST",
        body: JSON.stringify({ ecu: Number($("dbg-ecu").value),
                               did: Number($("dbg-did").value), value, force }),
      });
      toast("Geschrieben.", "ok");
    } catch (e) { toast(e.message, "err"); }
  };

  loadNames().then(loadPoints).then(loadSystem).then(loadMeter).then(loadCollect);
  loadCrash();
  loadSysinfo();
  loadSettings().catch(() => {});
}

/* ------------------------------------------------------------------ */

/* Read once per page load, not per poll: it only changes across a reboot,
   and a reboot reloads the page anyway. */
async function loadCrash() {
  let c;
  try { c = await api("/api/crash"); } catch { return; }
  $("s-reset").textContent = c.reason || "–";
  const box = $("crash-box");
  if (!c.dump) { box.hidden = true; return; }
  box.hidden = false;
  const d = c.dump;
  $("crash-info").innerHTML = "";
  const line = (k, v) => el("div", {}, el("span", { className: "muted",
                             textContent: k + ": " }), el("span", { textContent: v }));
  $("crash-info").append(
    line("Task", d.task),
    line("Programmzähler", d.pc),
    line("Ursache", `${d.cause}, Adresse ${d.vaddr}`),
    line("Build", d.elfSha),
    line("Rückverfolgung", (d.bt || []).join(" ") + (d.corrupted ? "  (unvollständig)" : "")));
}

async function poll() {
  try {
    const s = await api("/api/status");
    renderStatus(s);
    renderScan(s.scan);
    if ($("tab-meter").classList.contains("active")) {
      /* One after the other, not both at once: this tab produces the two
         largest answers the device has, and asking for them in parallel is
         what pushed the connection count over the edge. */
      loadMeter().then(loadCollect).catch(() => {});
    }
    /* Only while the status page is open: the snapshot walks every task. */
    if ($("tab-status").classList.contains("active")) {
      loadSysinfo();
    }
    if ($("tab-trace").classList.contains("active")) {
      traceStatus().then((st) => {
        /* Fetch the frames the moment capture ends, so an event that fired
           while nobody was looking is on screen when they come back. */
        if (st && st.triggered && !st.running && !traceFrames.length) {
          traceLoad();
        }
      });
    }
  } catch {
    pill($("hdr-can"), false, "offline");
  }
}

(async function main() {
  let status;
  try {
    status = await api("/api/status");
  } catch (e) {
    document.body.innerHTML =
      `<main><div class="card"><h2>Keine Verbindung zum Gerät</h2>
       <p class="muted">${e.message}</p></div></main>`;
    return;
  }

  /* Setup mode with no station connection means a first boot: show only the
     setup page. If the setup AP is up *alongside* a working connection the
     device merely failed to join once, so the full interface is more useful. */
  if (status.net.setupMode && status.net.state === "setup") {
    initSetup();
    return;
  }

  initApp();
  renderStatus(status);
  renderScan(status.scan);
  /* One second while a scan is running so the progress bar moves, five
     otherwise: each poll is an HTTP round trip to a device that is also
     driving a CAN bus. */
  let tick = 0;
  setInterval(() => {
    if (scanning || ++tick >= 5) {
      tick = 0;
      poll();
    }
  }, 1000);
})();
