export const API_ROOT = "/bmx/dev/v1";
export const MAX_FILE_SIZE = 0xffffffff;

export function isTransportFailure(error) {
  return error?.name === "AbortError" || error?.transport === true;
}

const USB_DIAGNOSTIC_STATES = new Set([
  "idle", "starting", "waiting", "capturing", "stopping",
]);
const USB_DIAGNOSTIC_MODES = new Set(["none", "new", "connected"]);
const USB_DIAGNOSTIC_BEGIN_PREFIX = Uint8Array.from(
  "usbdiag: BEGIN ", (character) => character.charCodeAt(0),
);
const USB_DIAGNOSTIC_END_PREFIX = Uint8Array.from(
  "usbdiag: END ", (character) => character.charCodeAt(0),
);

function startsWithBytes(value, prefix) {
  if (value.byteLength < prefix.byteLength) return false;
  for (let index = 0; index < prefix.byteLength; index += 1) {
    if (value[index] !== prefix[index]) return false;
  }
  return true;
}

// Collect raw log bytes between exact usbdiag marker lines. Marker matching is
// deliberately byte based: USB product text and lossy UTF-8 decoding must not
// be able to manufacture or hide capture boundaries.
export class UsbDiagnosticLogCollector {
  constructor(maxBytes, pendingLimit = 64 * 1024) {
    if (!Number.isInteger(maxBytes) || maxBytes <= 0) {
      fail("USB-Diagnosepuffergröße muss positiv sein.");
    }
    if (!Number.isInteger(pendingLimit) || pendingLimit < 64) {
      fail("USB-Diagnose-Zeilenpuffer ist zu klein.");
    }
    this.maxBytes = maxBytes;
    this.pendingLimit = pendingLimit;
    this.storage = null;
    this.clear();
  }

  clear() {
    this.pending = new Uint8Array();
    this.pendingAtLineStart = true;
    this.size = 0;
    this.received = 0;
    this.dropped = 0;
    this.seenBegin = false;
    this.active = false;
    this.complete = false;
    this.truncated = false;
  }

  append(value) {
    const input = asBytes(value);
    const event = {reset: false, chunks: [], completed: false};
    if (input.byteLength === 0) return event;

    const combined = new Uint8Array(this.pending.byteLength + input.byteLength);
    combined.set(this.pending);
    combined.set(input, this.pending.byteLength);
    let start = 0;
    let atLineStart = this.pendingAtLineStart;
    for (let index = 0; index < combined.byteLength; index += 1) {
      if (combined[index] !== 0x0a) continue;
      this._consumeLine(combined.slice(start, index + 1), event, atLineStart);
      start = index + 1;
      atLineStart = true;
    }
    this.pending = combined.slice(start);
    this.pendingAtLineStart = atLineStart;

    if (this.pending.byteLength > this.pendingLimit) {
      // Keep enough tail to recognize either marker when it is split across
      // network reads, while bounding a hostile unterminated log line.
      const keep = 64;
      const split = this.pending.byteLength - keep;
      const fragment = this.pending.slice(0, split);
      this.pending = this.pending.slice(split);
      this._consumeLine(fragment, event, this.pendingAtLineStart);
      // The retained bytes continue the same physical line. They must never
      // turn a marker embedded later in that line into a synthetic line start.
      this.pendingAtLineStart = false;
    }
    return event;
  }

  flush() {
    const event = {reset: false, chunks: [], completed: false};
    if (this.pending.byteLength) {
      const pending = this.pending;
      this.pending = new Uint8Array();
      this._consumeLine(pending, event, this.pendingAtLineStart);
      this.pendingAtLineStart = true;
    }
    return event;
  }

  _beginSection(event) {
    if (!this.storage) this.storage = new Uint8Array(this.maxBytes);
    this.size = 0;
    this.received = 0;
    this.dropped = 0;
    this.seenBegin = true;
    this.active = true;
    this.complete = false;
    this.truncated = false;
    event.reset = true;
    event.chunks = [];
    event.completed = false;
  }

  _store(value) {
    this.received += value.byteLength;
    const available = this.maxBytes - this.size;
    const stored = Math.min(available, value.byteLength);
    if (stored > 0) {
      this.storage.set(value.subarray(0, stored), this.size);
      this.size += stored;
    }
    if (stored !== value.byteLength) {
      this.dropped += value.byteLength - stored;
      this.truncated = true;
    }
  }

  _consumeLine(line, event, atLineStart = true) {
    if (atLineStart && startsWithBytes(line, USB_DIAGNOSTIC_BEGIN_PREFIX)) {
      this._beginSection(event);
    }
    if (!this.active) return;

    this._store(line);
    event.chunks.push(line);
    if (atLineStart && startsWithBytes(line, USB_DIAGNOSTIC_END_PREFIX)) {
      this.complete = true;
      this.active = false;
      event.completed = true;
    }
  }

  parts() {
    if (!this.storage || this.size === 0) return [];
    return [this.storage.subarray(0, this.size)];
  }

  bytes() {
    if (!this.storage || this.size === 0) return new Uint8Array();
    return this.storage.slice(0, this.size);
  }

  get pendingSize() {
    return this.pending.byteLength;
  }
}

function objectPayload(payload, context) {
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    fail(`${context} enthält kein JSON-Objekt.`);
  }
  return payload;
}

function unsignedInteger(value, name) {
  if (!Number.isSafeInteger(value) || value < 0) {
    fail(`${name} ist keine gültige nichtnegative Ganzzahl.`);
  }
  return value;
}

export function parseUsbDevices(payload) {
  const object = objectPayload(payload, "USB-Geräteantwort");
  if (!Array.isArray(object.devices)) {
    fail("USB-Geräteantwort enthält keine Geräteliste.");
  }
  return object.devices.map((entry, index) => {
    const device = objectPayload(entry, `USB-Gerät ${index + 1}`);
    const prefix = `USB-Gerät ${index + 1}`;
    const host = unsignedInteger(device.host, `${prefix}: host`);
    const port = unsignedInteger(device.port, `${prefix}: port`);
    const route = unsignedInteger(device.route, `${prefix}: route`);
    if (typeof device.connected !== "boolean") {
      fail(`${prefix}: connected ist kein Wahrheitswert.`);
    }
    if (typeof device.state !== "string") {
      fail(`${prefix}: state ist kein Text.`);
    }
    if (typeof device.vid !== "string" || !/^[0-9a-fA-F]{4}$/.test(device.vid) ||
        typeof device.pid !== "string" || !/^[0-9a-fA-F]{4}$/.test(device.pid)) {
      fail(`${prefix}: VID oder PID ist keine vierstellige Hexadezimalzahl.`);
    }
    if (typeof device.product !== "string") {
      fail(`${prefix}: product ist kein Text.`);
    }
    return {
      host,
      port,
      route,
      connected: device.connected,
      state: device.state,
      vid: device.vid.toLowerCase(),
      pid: device.pid.toLowerCase(),
      product: device.product,
    };
  });
}

export function parseUsbDiagnosticStatus(payload) {
  const status = objectPayload(payload, "USB-Diagnosestatus");
  if (!USB_DIAGNOSTIC_STATES.has(status.state)) {
    fail("USB-Diagnosestatus enthält einen unbekannten Zustand.");
  }
  if (!USB_DIAGNOSTIC_MODES.has(status.mode)) {
    fail("USB-Diagnosestatus enthält einen unbekannten Modus.");
  }
  if (typeof status.waiting_for_device !== "boolean" ||
      typeof status.truncated !== "boolean") {
    fail("USB-Diagnosestatus enthält ungültige Wahrheitswerte.");
  }
  // During a browser-based update the SD-card UI and the running kernel can
  // briefly be from adjacent BMX versions. Accept the former HID-specific
  // names while exposing only the generic input-report vocabulary internally.
  const inputReports = status.input_reports ?? status.hid_reports;
  const inputReportsDropped =
    status.input_reports_dropped ?? status.hid_reports_dropped;
  const result = {
    state: status.state,
    mode: status.mode,
    waiting_for_device: status.waiting_for_device,
    target_host: unsignedInteger(status.target_host, "target_host"),
    target_port: unsignedInteger(status.target_port, "target_port"),
    target_route: unsignedInteger(status.target_route, "target_route"),
    remaining_ms: unsignedInteger(status.remaining_ms, "remaining_ms"),
    devices_seen: unsignedInteger(status.devices_seen, "devices_seen"),
    descriptor_bytes: unsignedInteger(status.descriptor_bytes, "descriptor_bytes"),
    input_reports: unsignedInteger(inputReports, "input_reports"),
    input_reports_dropped: unsignedInteger(
      inputReportsDropped, "input_reports_dropped",
    ),
    input_reports_duplicates: status.input_reports_duplicates === undefined
      ? 0
      : unsignedInteger(status.input_reports_duplicates, "input_reports_duplicates"),
    input_reports_coalesced: status.input_reports_coalesced === undefined
      ? 0
      : unsignedInteger(status.input_reports_coalesced, "input_reports_coalesced"),
    truncated: status.truncated,
  };
  return result;
}

export function usbDiagnosticIsActive(status) {
  return status?.state !== undefined && status.state !== "idle";
}

export function usbDeviceIdentity(device) {
  const host = unsignedInteger(device?.host, "host");
  const port = unsignedInteger(device?.port, "port");
  const route = unsignedInteger(device?.route, "route");
  return `${host}:${port}:${route}`;
}

export function usbDiagnosticStartPath(mode, device = null) {
  if (mode === "new") {
    return `${API_ROOT}/diagnostics/usb/start?mode=new`;
  }
  if (mode !== "connected" || !device) {
    fail("USB-Diagnosemodus ist ungültig.");
  }
  const host = unsignedInteger(device.host, "host");
  const port = unsignedInteger(device.port, "port");
  const route = unsignedInteger(device.route, "route");
  return `${API_ROOT}/diagnostics/usb/start?mode=connected&host=${host}` +
    `&port=${port}&route=${route}`;
}

export function formatUsbDeviceLabel(device) {
  const host = unsignedInteger(device?.host, "host");
  const port = unsignedInteger(device?.port, "port");
  const route = unsignedInteger(device?.route, "route");
  const vid = typeof device?.vid === "string" ? device.vid.toUpperCase() : "????";
  const pid = typeof device?.pid === "string" ? device.pid.toUpperCase() : "????";
  const product = typeof device?.product === "string"
    ? device.product.replace(/[\r\n\t]+/g, " ").trim()
    : "";
  const state = typeof device?.state === "string"
    ? device.state.replace(/[\r\n\t]+/g, " ").trim()
    : "unbekannt";
  const presence = device?.connected === true ? "verbunden" : "nicht verbunden";
  return `Host ${host} / Port ${port} / Route ${route} — ${vid}:${pid}` +
    `${product ? ` — ${product}` : ""} — ${presence}, ${state}`;
}

export function usbDiagnosticInstruction(status) {
  if (!status || status.state === "idle") {
    return "Wähle einen der beiden Diagnoseabläufe.";
  }
  if (status.state === "starting") return "USB-Diagnose wird vorbereitet …";
  if (status.state === "stopping") return "USB-Diagnose wird beendet …";
  if (status.waiting_for_device && status.mode === "new") {
    return "Stecke das zu untersuchende USB-Gerät jetzt ein.";
  }
  if (status.waiting_for_device && status.mode === "connected") {
    return `Ziehe das Gerät an Host ${status.target_host} / Port ` +
      `${status.target_port} / Route ${status.target_route} ab und stecke es ` +
      "dort erneut ein.";
  }
  if (status.state === "capturing") {
    return "Betätige alle Bedienelemente einzeln und halte jede Eingabe kurz. " +
      "Identische USB-Eingabereports werden gefiltert, schnelle Änderungen verdichtet.";
  }
  return "USB-Diagnose läuft.";
}

function isoTimestamp(value, name) {
  const date = value instanceof Date ? value : new Date(value);
  if (!Number.isFinite(date.getTime())) fail(`${name} ist kein gültiger Zeitpunkt.`);
  return date.toISOString();
}

export function usbDiagnosticReportHeader({
  startedAt,
  endedAt,
  mode,
  target = null,
  systemStatus = null,
  diagnosticStatus = null,
  receivedBytes,
  storedBytes,
  droppedBytes,
  reconnects = 0,
  beginSeen = true,
  endSeen = true,
  logGap = false,
  notices = [],
  completion = "completed",
}) {
  if (!USB_DIAGNOSTIC_MODES.has(mode) || mode === "none") {
    fail("Bericht enthält keinen gültigen USB-Diagnosemodus.");
  }
  for (const [name, value] of Object.entries({
    receivedBytes, storedBytes, droppedBytes, reconnects,
  })) {
    unsignedInteger(value, name);
  }
  if (!Array.isArray(notices) || notices.some((notice) => typeof notice !== "string")) {
    fail("Berichtshinweise sind ungültig.");
  }
  const targetText = target
    ? `Host ${unsignedInteger(target.host, "target.host")} / ` +
      `Port ${unsignedInteger(target.port, "target.port")} / ` +
      `Route ${unsignedInteger(target.route, "target.route")}`
    : "neues Gerät";
  const lines = [
    "BMX USB diagnostic report",
    "=========================",
    `Started: ${isoTimestamp(startedAt, "startedAt")}`,
    `Ended: ${isoTimestamp(endedAt, "endedAt")}`,
    `Completion: ${String(completion).replace(/[\r\n\t]+/g, " ")}`,
    `Mode: ${mode}`,
    `Target: ${targetText}`,
    ...(mode === "connected" ? [
      "Connected mode: Supported HID is captured immediately; an unsupported " +
        "HID rejected before capture must be physically unplugged and replugged " +
        "after start. A retained disconnected path must likewise be replugged " +
        "at the selected host, port and route.",
    ] : []),
    `Browser log bytes: received=${receivedBytes} stored=${storedBytes} dropped=${droppedBytes}`,
    `Log reconnects: ${reconnects}`,
    `Capture markers: BEGIN=${beginSeen ? "yes" : "no"} END=${endSeen ? "yes" : "no"}`,
    `Log gap or reset observed: ${logGap ? "yes" : "no"}`,
    "USB input privacy: Reports may contain keystrokes and other user input.",
    "",
    "BMX system status:",
    JSON.stringify(systemStatus, null, 2),
    "",
    "Final USB diagnostic status:",
    JSON.stringify(diagnosticStatus, null, 2),
    "",
    "Capture notices:",
    ...(notices.length ? notices.map((notice) => `- ${notice}`) : ["- none"]),
    "",
    "--- BEGIN CAPTURED LOG BYTES ---",
    "",
  ];
  return `${lines.join("\n")}\n`;
}

const CP850_HIGH = [
  0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
  0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
  0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
  0x00ff, 0x00d6, 0x00dc, 0x00f8, 0x00a3, 0x00d8, 0x00d7, 0x0192,
  0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
  0x00bf, 0x00ae, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00c1, 0x00c2, 0x00c0,
  0x00a9, 0x2563, 0x2551, 0x2557, 0x255d, 0x00a2, 0x00a5, 0x2510,
  0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x00e3, 0x00c3,
  0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x00a4,
  0x00f0, 0x00d0, 0x00ca, 0x00cb, 0x00c8, 0x0131, 0x00cd, 0x00ce,
  0x00cf, 0x2518, 0x250c, 0x2588, 0x2584, 0x00a6, 0x00cc, 0x2580,
  0x00d3, 0x00df, 0x00d4, 0x00d2, 0x00f5, 0x00d5, 0x00b5, 0x00fe,
  0x00de, 0x00da, 0x00db, 0x00d9, 0x00fd, 0x00dd, 0x00af, 0x00b4,
  0x00ad, 0x00b1, 0x2017, 0x00be, 0x00b6, 0x00a7, 0x00f7, 0x00b8,
  0x00b0, 0x00a8, 0x00b7, 0x00b9, 0x00b3, 0x00b2, 0x25a0, 0x00a0,
];

const CP850_REVERSE = new Map(
  CP850_HIGH.map((codePoint, index) => [codePoint, index + 0x80]),
);
const FORBIDDEN_FAT_BYTES = new Set(
  [...'"*:<>?\\|'].map((character) => character.charCodeAt(0)),
);

function fail(message) {
  throw new Error(message);
}

function cp850Bytes(value) {
  const bytes = [];
  for (const character of value) {
    const codePoint = character.codePointAt(0);
    if (codePoint <= 0x7f) {
      bytes.push(codePoint);
      continue;
    }
    const encoded = CP850_REVERSE.get(codePoint);
    if (encoded === undefined) {
      fail("Der Zielpfad enthält Zeichen, die BMX/FatFs nicht als CP850 speichern kann.");
    }
    bytes.push(encoded);
  }
  return Uint8Array.from(bytes);
}

function isUnreserved(byte) {
  return (
    (byte >= 0x41 && byte <= 0x5a) ||
    (byte >= 0x61 && byte <= 0x7a) ||
    (byte >= 0x30 && byte <= 0x39) ||
    byte === 0x2d ||
    byte === 0x2e ||
    byte === 0x5f ||
    byte === 0x7e
  );
}

function percentEncode(bytes) {
  let encoded = "";
  for (const byte of bytes) {
    encoded += isUnreserved(byte)
      ? String.fromCharCode(byte)
      : `%${byte.toString(16).toUpperCase().padStart(2, "0")}`;
  }
  return encoded;
}

export function parseDestination(value) {
  if (typeof value !== "string") fail("Der Zielpfad fehlt.");
  const separator = value.indexOf(":/");
  if (separator <= 0 || separator + 2 >= value.length) {
    fail("Der Zielpfad muss die Form VOLUME:/relativer/pfad haben.");
  }
  if ([...value].some((character) => {
    const codePoint = character.codePointAt(0);
    return codePoint < 0x20 || codePoint === 0x7f;
  })) {
    fail("Der Zielpfad darf keine Steuerzeichen enthalten.");
  }

  const volume = value.slice(0, separator);
  const relative = value.slice(separator + 2);
  if (volume.length > 15 || !/^[A-Za-z0-9_]+$/.test(volume)) {
    fail("Das Ziel-Volume ist ungültig.");
  }

  const components = relative.split("/");
  if (components.some((component) =>
    component === "" || component === "." || component === "..")) {
    fail("Der Zielpfad darf keine leeren, '.'- oder '..'-Komponenten enthalten.");
  }

  let relativeBytes = components.length - 1;
  const encodedComponents = components.map((component) => {
    const bytes = cp850Bytes(component);
    const last = bytes.at(-1);
    if (
      bytes.length > 255 ||
      last === 0x20 ||
      last === 0x2e ||
      [...bytes].some((byte) =>
        byte < 0x20 || byte === 0x7f || FORBIDDEN_FAT_BYTES.has(byte))
    ) {
      fail("Der Zielpfad enthält einen Namen, den FatFs nicht speichern kann.");
    }
    relativeBytes += bytes.length;
    return percentEncode(bytes);
  });

  if (relativeBytes >= 512) fail("Der Zielpfad ist zu lang.");
  return {
    displayPath: `${volume}:/${relative}`,
    requestPath: `${API_ROOT}/fs/${volume}/${encodedComponents.join("/")}`,
  };
}

const SHA256_K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

function rotateRight(value, bits) {
  return (value >>> bits) | (value << (32 - bits));
}

function asBytes(value) {
  if (value instanceof Uint8Array) return value;
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  fail("SHA-256 erwartet Bytes.");
}

export class Sha256 {
  constructor() {
    this.state = new Uint32Array([
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    ]);
    this.words = new Uint32Array(64);
    this.buffer = new Uint8Array(64);
    this.bufferLength = 0;
    this.totalBytes = 0;
    this.finalized = false;
  }

  update(value) {
    if (this.finalized) fail("SHA-256 wurde bereits abgeschlossen.");
    const bytes = asBytes(value);
    this.totalBytes += bytes.byteLength;
    if (!Number.isSafeInteger(this.totalBytes)) fail("Datei ist zu groß.");

    let offset = 0;
    while (offset < bytes.byteLength) {
      if (this.bufferLength === 0 && bytes.byteLength - offset >= 64) {
        this.#compress(bytes, offset);
        offset += 64;
        continue;
      }
      const count = Math.min(64 - this.bufferLength, bytes.byteLength - offset);
      this.buffer.set(bytes.subarray(offset, offset + count), this.bufferLength);
      this.bufferLength += count;
      offset += count;
      if (this.bufferLength === 64) {
        this.#compress(this.buffer, 0);
        this.bufferLength = 0;
      }
    }
    return this;
  }

  #compress(bytes, offset) {
    const words = this.words;
    for (let index = 0; index < 16; index += 1) {
      const start = offset + index * 4;
      words[index] = (
        (bytes[start] << 24) |
        (bytes[start + 1] << 16) |
        (bytes[start + 2] << 8) |
        bytes[start + 3]
      ) >>> 0;
    }
    for (let index = 16; index < 64; index += 1) {
      const before15 = words[index - 15];
      const before2 = words[index - 2];
      const sigma0 = rotateRight(before15, 7) ^
        rotateRight(before15, 18) ^ (before15 >>> 3);
      const sigma1 = rotateRight(before2, 17) ^
        rotateRight(before2, 19) ^ (before2 >>> 10);
      words[index] = (
        words[index - 16] + sigma0 + words[index - 7] + sigma1
      ) >>> 0;
    }

    let [a, b, c, d, e, f, g, h] = this.state;
    for (let index = 0; index < 64; index += 1) {
      const sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
      const choose = (e & f) ^ (~e & g);
      const first = (h + sum1 + choose + SHA256_K[index] + words[index]) >>> 0;
      const sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
      const majority = (a & b) ^ (a & c) ^ (b & c);
      const second = (sum0 + majority) >>> 0;
      h = g;
      g = f;
      f = e;
      e = (d + first) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (first + second) >>> 0;
    }
    this.state[0] = (this.state[0] + a) >>> 0;
    this.state[1] = (this.state[1] + b) >>> 0;
    this.state[2] = (this.state[2] + c) >>> 0;
    this.state[3] = (this.state[3] + d) >>> 0;
    this.state[4] = (this.state[4] + e) >>> 0;
    this.state[5] = (this.state[5] + f) >>> 0;
    this.state[6] = (this.state[6] + g) >>> 0;
    this.state[7] = (this.state[7] + h) >>> 0;
  }

  digestBytes() {
    if (this.finalized) fail("SHA-256 wurde bereits abgeschlossen.");
    const originalBytes = this.totalBytes;
    this.buffer[this.bufferLength] = 0x80;
    this.bufferLength += 1;
    if (this.bufferLength > 56) {
      this.buffer.fill(0, this.bufferLength);
      this.#compress(this.buffer, 0);
      this.bufferLength = 0;
    }
    this.buffer.fill(0, this.bufferLength, 56);

    const highBits = Math.floor(originalBytes / 0x20000000) >>> 0;
    const lowBits = (originalBytes * 8) >>> 0;
    this.buffer[56] = highBits >>> 24;
    this.buffer[57] = highBits >>> 16;
    this.buffer[58] = highBits >>> 8;
    this.buffer[59] = highBits;
    this.buffer[60] = lowBits >>> 24;
    this.buffer[61] = lowBits >>> 16;
    this.buffer[62] = lowBits >>> 8;
    this.buffer[63] = lowBits;
    this.#compress(this.buffer, 0);
    this.finalized = true;

    const digest = new Uint8Array(32);
    for (let index = 0; index < this.state.length; index += 1) {
      const value = this.state[index];
      digest[index * 4] = value >>> 24;
      digest[index * 4 + 1] = value >>> 16;
      digest[index * 4 + 2] = value >>> 8;
      digest[index * 4 + 3] = value;
    }
    return digest;
  }

  digestHex() {
    return [...this.digestBytes()]
      .map((byte) => byte.toString(16).padStart(2, "0"))
      .join("");
  }
}

export function sha256Hex(value, chunkSize = 0) {
  const bytes = asBytes(value);
  const hash = new Sha256();
  if (!chunkSize) {
    hash.update(bytes);
  } else {
    for (let offset = 0; offset < bytes.length; offset += chunkSize) {
      hash.update(bytes.subarray(offset, offset + chunkSize));
    }
  }
  return hash.digestHex();
}

function abortError() {
  const error = new Error("Vorgang abgebrochen.");
  error.name = "AbortError";
  return error;
}

export async function hashFile(file, onProgress = () => {}, signal = null) {
  if (!file || !Number.isInteger(file.size) || file.size < 0) {
    fail("Keine gültige lokale Datei ausgewählt.");
  }
  if (file.size > MAX_FILE_SIZE) {
    fail("Die Datei ist größer als das BMX-Uploadlimit von 4 GiB minus 1 Byte.");
  }
  const hash = new Sha256();
  const chunkSize = 1024 * 1024;
  if (file.size === 0) onProgress(1);
  for (let offset = 0; offset < file.size; offset += chunkSize) {
    if (signal?.aborted) throw abortError();
    const end = Math.min(file.size, offset + chunkSize);
    const bytes = new Uint8Array(await file.slice(offset, end).arrayBuffer());
    if (signal?.aborted) throw abortError();
    hash.update(bytes);
    onProgress(end / file.size);
  }
  return {size: file.size, sha256: hash.digestHex()};
}

export function parseRemoteFileHeaders(headers) {
  const length = headers?.get?.("Content-Length");
  const etag = headers?.get?.("ETag");
  if (typeof length !== "string" || !/^[0-9]+$/.test(length)) {
    fail("HEAD enthält kein gültiges Content-Length.");
  }
  const size = Number(length);
  if (!Number.isSafeInteger(size)) fail("HEAD meldet eine ungültige Dateigröße.");
  const match = typeof etag === "string"
    ? /^"sha256:([0-9a-fA-F]{64})"$/.exec(etag)
    : null;
  if (!match) fail('HEAD enthält kein gültiges ETag "sha256:…".');
  return {size, sha256: match[1].toLowerCase()};
}

export function sameRemoteFile(remote, local) {
  return remote !== null && remote.size === local.size &&
    remote.sha256 === local.sha256;
}

export function validatePutResult(payload, expected, reboot) {
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    fail("PUT hat kein JSON-Objekt zurückgegeben.");
  }
  if (typeof payload.path !== "string") fail("PUT-Antwort enthält keinen Pfad.");
  if (!Number.isInteger(payload.size) || payload.size !== expected.size) {
    fail("PUT-Antwort meldet eine andere Dateigröße.");
  }
  if (typeof payload.sha256 !== "string" ||
      payload.sha256.toLowerCase() !== expected.sha256) {
    fail("PUT-Antwort meldet einen anderen SHA-256.");
  }
  if (typeof payload.changed !== "boolean") {
    fail("PUT-Antwort enthält keinen gültigen Änderungsstatus.");
  }
  if (payload.reboot_scheduled !== reboot) {
    fail("PUT-Antwort enthält einen unerwarteten Reboot-Status.");
  }
  return payload;
}

export class RawLogBuffer {
  constructor(maxBytes) {
    if (!Number.isInteger(maxBytes) || maxBytes <= 0) {
      fail("Logpuffergröße muss positiv sein.");
    }
    this.maxBytes = maxBytes;
    this.clear();
  }

  clear() {
    this.chunks = [];
    this.size = 0;
    this.dropped = 0;
    this.received = 0;
  }

  append(value) {
    const input = asBytes(value);
    if (input.byteLength === 0) return;
    this.received += input.byteLength;
    let bytes = input.slice();
    if (bytes.byteLength >= this.maxBytes) {
      this.dropped += this.size + bytes.byteLength - this.maxBytes;
      bytes = bytes.slice(bytes.byteLength - this.maxBytes);
      this.chunks = [bytes];
      this.size = bytes.byteLength;
      return;
    }
    this.chunks.push(bytes);
    this.size += bytes.byteLength;
    while (this.size > this.maxBytes) {
      const excess = this.size - this.maxBytes;
      const first = this.chunks[0];
      if (first.byteLength <= excess) {
        this.chunks.shift();
        this.size -= first.byteLength;
        this.dropped += first.byteLength;
      } else {
        this.chunks[0] = first.slice(excess);
        this.size -= excess;
        this.dropped += excess;
      }
    }
  }

  parts() {
    return this.chunks.slice();
  }

  bytes() {
    const output = new Uint8Array(this.size);
    let offset = 0;
    for (const chunk of this.chunks) {
      output.set(chunk, offset);
      offset += chunk.byteLength;
    }
    return output;
  }
}

export function parseUnsignedDecimal(value, name) {
  if (typeof value !== "string" || !/^[0-9]+$/.test(value)) {
    fail(`Logantwort enthält einen ungültigen ${name}-Header.`);
  }
  return BigInt(value);
}

export function parseLogHandshake(headers, requestedCursor, previousEpoch) {
  const start = parseUnsignedDecimal(headers?.get?.("X-BMX-Log-Start"),
    "X-BMX-Log-Start");
  const oldest = parseUnsignedDecimal(headers?.get?.("X-BMX-Log-Oldest"),
    "X-BMX-Log-Oldest");
  const epoch = parseUnsignedDecimal(headers?.get?.("X-BMX-Log-Epoch"),
    "X-BMX-Log-Epoch");
  if (start < oldest) fail("Logantwort beginnt vor ihrer ältesten Sequenz.");

  const notices = [];
  if (previousEpoch !== null && epoch !== previousEpoch) {
    notices.push(`BMX-Reboot erkannt; Log-Epoche ${previousEpoch} → ${epoch}.`);
  } else if (requestedCursor !== null) {
    if (oldest > requestedCursor) {
      notices.push(`${oldest - requestedCursor} Logbytes wurden überschrieben.`);
    } else if (start > requestedCursor) {
      notices.push(`${start - requestedCursor} angeforderte Logbytes fehlen.`);
    } else if (start < requestedCursor) {
      notices.push(`Logsequenz wurde von ${requestedCursor} auf ${start} zurückgesetzt.`);
    }
  }
  if ((headers?.get?.("X-BMX-Log-Gap") === "1" ||
       headers?.get?.("X-BMX-Log-Reset") === "1") && notices.length === 0) {
    notices.push("BMX meldet eine Lücke oder zurückgesetzte Logsequenz.");
  }
  return {start, oldest, epoch, notices};
}

export function parseLogSnapshotHeaders(headers) {
  const handshake = parseLogHandshake(headers, 0n, null);
  const end = parseUnsignedDecimal(headers?.get?.("X-BMX-Log-End"),
    "X-BMX-Log-End");
  if (end < handshake.start) {
    fail("Log-Snapshot endet vor seiner Startsequenz.");
  }
  return {
    ...handshake,
    end,
    gap: headers?.get?.("X-BMX-Log-Gap") === "1" || handshake.oldest > 0n,
  };
}

export function diagnosticReportHeader({
  generatedAt,
  systemStatus,
  usbDevices = null,
  usbDiagnosticStatus = null,
  log,
  logBytes,
  notices = [],
}) {
  objectPayload(systemStatus, "Systemstatus");
  if (usbDevices !== null && !Array.isArray(usbDevices)) {
    fail("USB-Geräteliste des Berichts ist ungültig.");
  }
  if (usbDiagnosticStatus !== null &&
      (typeof usbDiagnosticStatus !== "object" ||
       Array.isArray(usbDiagnosticStatus))) {
    fail("USB-Diagnosestatus des Berichts ist ungültig.");
  }
  if (!log || typeof log !== "object") {
    fail("Log-Metadaten des Berichts fehlen.");
  }
  for (const name of ["start", "oldest", "end", "epoch"]) {
    if (typeof log[name] !== "bigint" || log[name] < 0n) {
      fail(`Log-Metadatum ${name} ist ungültig.`);
    }
  }
  unsignedInteger(logBytes, "logBytes");
  if (log.end < log.start || BigInt(logBytes) !== log.end - log.start) {
    fail("Log-Snapshotgröße stimmt nicht mit seinen Sequenzen überein.");
  }
  if (!Array.isArray(notices) ||
      notices.some((notice) => typeof notice !== "string")) {
    fail("Berichtshinweise sind ungültig.");
  }

  const lines = [
    "BMX diagnostic report",
    "=====================",
    `Generated: ${isoTimestamp(generatedAt, "generatedAt")}`,
    "Local generation: This browser created the file; BMX did not upload it.",
    "Privacy: Retained logs and USB product names may contain personal data.",
    "Review this file before sending it to another person.",
    "",
    "BMX system status:",
    JSON.stringify(systemStatus, null, 2),
    "",
    "Known USB devices:",
    JSON.stringify(usbDevices, null, 2),
    "",
    "USB diagnostic status:",
    JSON.stringify(usbDiagnosticStatus, null, 2),
    "",
    "Retained log snapshot:",
    `start=${log.start} oldest=${log.oldest} end=${log.end} epoch=${log.epoch}`,
    `bytes=${logBytes} gap=${log.gap ? "yes" : "no"}`,
    "",
    "Report notices:",
    ...(notices.length ? notices.map((notice) =>
      `- ${notice.replace(/[\r\n\t]+/g, " ").trim()}`)
      : ["- none"]),
    "",
    "--- BEGIN RETAINED LOG BYTES ---",
    "",
  ];
  return `${lines.join("\n")}\n`;
}

function uptime(status) {
  const value = status?.uptime_ms;
  return Number.isInteger(value) && value >= 0 ? value : null;
}

export function rebootObservation(beforeStatus, currentStatus, disconnected) {
  const before = uptime(beforeStatus);
  const current = uptime(currentStatus);
  const comparable = before !== null && current !== null;
  const uptimeReset = comparable && current < before;
  return {
    observed: uptimeReset || (Boolean(disconnected) && !comparable),
    disconnectObserved: Boolean(disconnected),
    uptimeReset,
    uptimeMs: current,
  };
}
