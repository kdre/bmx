import {
  API_ROOT,
  RawLogBuffer,
  UsbDiagnosticLogCollector,
  diagnosticReportHeader,
  formatUsbDeviceLabel,
  hashFile,
  isTransportFailure,
  parseDestination,
  parseLogHandshake,
  parseLogSnapshotHeaders,
  parseRemoteFileHeaders,
  parseUsbDevices,
  parseUsbDiagnosticStatus,
  rebootObservation,
  sameRemoteFile,
  usbDeviceIdentity,
  usbDiagnosticInstruction,
  usbDiagnosticIsActive,
  usbDiagnosticReportHeader,
  usbDiagnosticStartPath,
  validatePutResult,
} from "./core.mjs";

const LOG_BUFFER_BYTES = 8 * 1024 * 1024;
const LOG_DISPLAY_CHARACTERS = 512 * 1024;
const LOG_RECONNECT_DELAY_MS = 500;
const USB_DIAGNOSTIC_BUFFER_BYTES = 2 * 1024 * 1024;
const USB_DIAGNOSTIC_DISPLAY_CHARACTERS = 256 * 1024;
const USB_DIAGNOSTIC_POLL_MS = 1000;
const USB_DIAGNOSTIC_WATCHDOG_MS = 75 * 1000;
const REBOOT_TIMEOUT_MS = 180 * 1000;
const REBOOT_POLL_MS = 1000;
const STATUS_REQUEST_TIMEOUT_MS = 5000;

const element = (id) => document.getElementById(id);
const ui = {
  connectionState: element("connection-state"),
  deviceAddress: element("device-address"),
  password: element("password"),
  connect: element("connect"),
  forgetPassword: element("forget-password"),
  connectionMessage: element("connection-message"),
  refreshStatus: element("refresh-status"),
  downloadDiagnosticReport: element("download-diagnostic-report"),
  diagnosticReportMessage: element("diagnostic-report-message"),
  statusBoard: element("status-board"),
  statusMachine: element("status-machine"),
  statusUptime: element("status-uptime"),
  statusNetwork: element("status-network"),
  statusHeap: element("status-heap"),
  statusRam: element("status-ram"),
  statusHeapLow: element("status-heap-low"),
  statusHeapHigh: element("status-heap-high"),
  statusArmClock: element("status-arm-clock"),
  statusEmuCycles: element("status-emu-cycles"),
  statusTemperature: element("status-temperature"),
  statusThrottleClock: element("status-throttle-clock"),
  statusLogBuffer: element("status-log-buffer"),
  statusJson: element("status-json"),
  logState: element("log-state"),
  startLogs: element("start-logs"),
  stopLogs: element("stop-logs"),
  downloadLogs: element("download-logs"),
  clearLogs: element("clear-logs"),
  logMessage: element("log-message"),
  logOutput: element("log-output"),
  usbState: element("usb-state"),
  usbStartNew: element("usb-start-new"),
  usbDevice: element("usb-device"),
  usbRefreshDevices: element("usb-refresh-devices"),
  usbStartConnected: element("usb-start-connected"),
  usbStop: element("usb-stop"),
  usbDownload: element("usb-download"),
  usbInstruction: element("usb-instruction"),
  usbRemaining: element("usb-remaining"),
  usbDevicesSeen: element("usb-devices-seen"),
  usbDescriptorBytes: element("usb-descriptor-bytes"),
  usbInputReports: element("usb-input-reports"),
  usbInputDropped: element("usb-input-dropped"),
  usbInputDuplicates: element("usb-input-duplicates"),
  usbInputCoalesced: element("usb-input-coalesced"),
  usbTruncated: element("usb-truncated"),
  usbMessage: element("usb-message"),
  usbOutput: element("usb-output"),
  deployFile: element("deploy-file"),
  deployDestination: element("deploy-destination"),
  deployReboot: element("deploy-reboot"),
  deploy: element("deploy"),
  deployProgress: element("deploy-progress"),
  deployMessage: element("deploy-message"),
  reboot: element("reboot"),
  rebootMessage: element("reboot-message"),
};

class ApiError extends Error {
  constructor(message, status = null, transport = false) {
    super(message);
    this.name = "ApiError";
    this.status = status;
    this.transport = transport;
  }
}

let password = "";
let logController = null;
let logTask = null;
let logCursor = null;
let logEpoch = null;
let logDisplay = "";
let logNotices = [];
let logCaptureStarted = null;
let logStreamConnected = false;
let logReadyWaiters = [];
let actionBusy = false;
let lastSystemStatus = null;
let usbStatus = null;
let usbDevices = [];
let usbOperationBusy = false;
let usbMonitorGeneration = 0;
let usbCaptureActive = false;
let usbCaptureCompleted = false;
let usbCaptureMode = "none";
let usbCaptureTarget = null;
let usbCaptureStarted = null;
let usbCaptureEnded = null;
let usbCaptureSystemStatus = null;
let usbCaptureFinalStatus = null;
let usbCaptureCompletion = "not started";
let usbCaptureNotices = [];
let usbCaptureReconnects = 0;
let usbCaptureLogGap = false;
let usbDisplay = "";
let usbDecoder = null;
const rawLogs = new RawLogBuffer(LOG_BUFFER_BYTES);
const usbRawLogs = new UsbDiagnosticLogCollector(USB_DIAGNOSTIC_BUFFER_BYTES);

function setMessage(target, message, kind = "") {
  target.textContent = message;
  target.className = `message${kind ? ` ${kind}` : ""}`;
}

function setState(target, message, kind) {
  target.textContent = message;
  target.className = `state state-${kind}`;
}

function singleLine(value) {
  return String(value).replace(/[\r\n\t]+/g, " ").trim();
}

function errorText(error) {
  if (error?.name === "AbortError") return "Vorgang abgebrochen.";
  return singleLine(error instanceof Error ? error.message : error);
}

function setConnection(online, message = "") {
  setState(ui.connectionState, online ? "Verbunden" : "Nicht verbunden",
    online ? "online" : "offline");
  if (message) setMessage(ui.connectionMessage, message, online ? "success" : "error");
}

function updateLogControls() {
  ui.startLogs.disabled = actionBusy || usbOperationBusy || logTask !== null;
  ui.stopLogs.disabled = actionBusy || usbOperationBusy || logTask === null ||
    usbCaptureActive;
}

function updateUsbControls() {
  const available = usbStatus !== null;
  const active = available && usbDiagnosticIsActive(usbStatus);
  const busy = actionBusy || usbOperationBusy;
  const capturePending = usbCaptureActive;
  const selected = ui.usbDevice.selectedIndex >= 0 && ui.usbDevice.value !== "";
  ui.usbStartNew.disabled = busy || !available || active || capturePending;
  ui.usbRefreshDevices.disabled = busy || !available || active || capturePending;
  ui.usbDevice.disabled = busy || !available || active || capturePending ||
    usbDevices.length === 0;
  ui.usbStartConnected.disabled = busy || !available || active || capturePending ||
    !selected;
  ui.usbStop.disabled = busy || !active;
  ui.usbDownload.disabled = usbCaptureActive || !usbCaptureCompleted;
  const disruptiveDisabled = actionBusy || usbOperationBusy || active || capturePending;
  ui.deploy.disabled = disruptiveDisabled;
  ui.deployFile.disabled = disruptiveDisabled;
  ui.deployDestination.disabled = disruptiveDisabled;
  ui.deployReboot.disabled = disruptiveDisabled;
  ui.reboot.disabled = disruptiveDisabled;
  updateLogControls();
}

function setActionBusy(busy) {
  actionBusy = busy;
  ui.connect.disabled = busy;
  ui.password.disabled = busy;
  ui.forgetPassword.disabled = busy;
  ui.refreshStatus.disabled = busy;
  ui.downloadDiagnosticReport.disabled = busy;
  ui.deploy.disabled = busy;
  ui.deployFile.disabled = busy;
  ui.deployDestination.disabled = busy;
  ui.deployReboot.disabled = busy;
  ui.reboot.disabled = busy;
  updateUsbControls();
}

function authHeaders(initial = undefined) {
  const headers = new Headers(initial);
  if (password) headers.set("X-Password", password);
  return headers;
}

async function responseError(response) {
  let detail = "";
  try {
    const text = (await response.text()).slice(0, 1024);
    try {
      const payload = JSON.parse(text);
      if (payload && typeof payload.error === "string") detail = payload.error;
    } catch {
      detail = text;
    }
  } catch {
    // The status code remains useful when the response body cannot be read.
  }
  const suffix = detail ? `: ${singleLine(detail)}` : "";
  return new ApiError(`BMX antwortet mit HTTP ${response.status}${suffix}`,
    response.status);
}

async function apiFetch(path, options = {}) {
  let response;
  try {
    response = await fetch(path, {
      ...options,
      cache: "no-store",
      headers: authHeaders(options.headers),
    });
  } catch (error) {
    if (error?.name === "AbortError") throw error;
    throw new ApiError("BMX ist nicht erreichbar.", null, true);
  }
  if (response.status === 403) {
    throw new ApiError("Developer-Passwort fehlt oder ist falsch.", 403);
  }
  return response;
}

async function requireSuccess(response) {
  if (!response.ok) throw await responseError(response);
  return response;
}

async function jsonResponse(response, context) {
  await requireSuccess(response);
  let payload;
  try {
    payload = await response.json();
  } catch {
    throw new ApiError(`${context} enthält kein gültiges JSON.`, response.status);
  }
  if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
    throw new ApiError(`${context} enthält kein JSON-Objekt.`, response.status);
  }
  return payload;
}

function formatUptime(milliseconds) {
  if (!Number.isInteger(milliseconds) || milliseconds < 0) return "–";
  const seconds = Math.floor(milliseconds / 1000);
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const rest = seconds % 60;
  return `${days ? `${days} d ` : ""}${hours.toString().padStart(2, "0")}:` +
    `${minutes.toString().padStart(2, "0")}:${rest.toString().padStart(2, "0")}`;
}

function displayStatus(status) {
  lastSystemStatus = status;
  ui.statusBoard.textContent = typeof status.board === "string" ? status.board : "–";
  ui.statusMachine.textContent = typeof status.machine === "string" ? status.machine : "–";
  ui.statusUptime.textContent = formatUptime(status.uptime_ms);
  ui.statusNetwork.textContent = status.network_ready === true ? "bereit" :
    status.network_ready === false ? "nicht bereit" : "–";
  ui.statusHeap.textContent = Number.isInteger(status.heap_free_kb)
    ? `${status.heap_free_kb} KiB` : "–";
  ui.statusRam.textContent = Number.isInteger(status.ram_total_kb)
    ? `${status.ram_total_kb} KiB` : "–";
  ui.statusHeapLow.textContent = Number.isInteger(status.heap_low_free_kb)
    ? `${status.heap_low_free_kb} KiB` : "–";
  ui.statusHeapHigh.textContent = Number.isInteger(status.heap_high_free_kb)
    ? `${status.heap_high_free_kb} KiB` : "–";
  ui.statusArmClock.textContent = Number.isInteger(status.arm_clock_hz)
    ? `${(status.arm_clock_hz / 1_000_000).toFixed(0)} MHz` : "–";
  ui.statusEmuCycles.textContent = Number.isInteger(status.emu_cycles_per_sec)
    ? `${status.emu_cycles_per_sec} Hz` : "–";
  ui.statusTemperature.textContent = Number.isFinite(status.temperature_c)
    ? `${status.temperature_c} °C` : "–";
  ui.statusThrottleClock.textContent = Number.isInteger(status.throttle_clock_hz)
    ? `${(status.throttle_clock_hz / 1_000_000).toFixed(0)} MHz` : "–";
  ui.statusLogBuffer.textContent = Number.isInteger(status.log_buffer_kb)
    ? `${status.log_buffer_kb} KiB` : "–";
  ui.statusJson.textContent = JSON.stringify(status, null, 2);
}

async function readStatus({timeout = 0} = {}) {
  const controller = timeout ? new AbortController() : null;
  const timer = controller
    ? window.setTimeout(() => controller.abort(), timeout)
    : null;
  try {
    const response = await apiFetch(`${API_ROOT}/status`, {
      method: "GET",
      signal: controller?.signal,
    });
    return await jsonResponse(response, "Statusantwort");
  } finally {
    if (timer !== null) window.clearTimeout(timer);
  }
}

async function refreshStatus(announce = true, options = {}) {
  try {
    const status = await readStatus(options);
    displayStatus(status);
    setConnection(true, announce ? "Verbindung zur lokalen REST-API hergestellt." : "");
    return status;
  } catch (error) {
    setConnection(false, errorText(error));
    throw error;
  }
}

async function readLogSnapshot() {
  const response = await apiFetch(`${API_ROOT}/logs?follow=0&since=0`, {
    method: "GET",
  });
  await requireSuccess(response);
  let metadata;
  try {
    metadata = parseLogSnapshotHeaders(response.headers);
  } catch (error) {
    throw new ApiError(errorText(error), response.status);
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (BigInt(bytes.byteLength) !== metadata.end - metadata.start) {
    throw new ApiError("Log-Snapshot ist unvollständig.", response.status);
  }
  return {metadata, bytes};
}

async function downloadDiagnosticReport() {
  if (actionBusy) return;
  setActionBusy(true);
  setMessage(ui.diagnosticReportMessage, "Sammle lokalen Diagnosebericht …");
  try {
    const generatedAt = new Date();
    const systemStatus = await readStatus();
    displayStatus(systemStatus);
    const notices = [];
    let reportUsbDevices = null;
    let reportUsbStatus = null;
    try {
      reportUsbDevices = await readUsbDevices();
    } catch (error) {
      notices.push(`USB-Geräteliste nicht verfügbar: ${errorText(error)}`);
    }
    try {
      reportUsbStatus = await readUsbStatus();
    } catch (error) {
      notices.push(`USB-Diagnosestatus nicht verfügbar: ${errorText(error)}`);
    }
    const snapshot = await readLogSnapshot();
    if (snapshot.metadata.gap) {
      notices.push("Der Anfang des Bootlogs wurde bereits überschrieben.");
    }
    const header = diagnosticReportHeader({
      generatedAt,
      systemStatus,
      usbDevices: reportUsbDevices,
      usbDiagnosticStatus: reportUsbStatus,
      log: snapshot.metadata,
      logBytes: snapshot.bytes.byteLength,
      notices,
    });
    const blob = new Blob([
      header,
      snapshot.bytes,
      "\n--- END RETAINED LOG BYTES ---\n",
    ], {type: "text/plain;charset=utf-8"});
    const link = document.createElement("a");
    const stamp = generatedAt.toISOString().replace(/[:.]/g, "-");
    link.href = URL.createObjectURL(blob);
    link.download = `bmx-diagnostic-${stamp}.txt`;
    document.body.append(link);
    link.click();
    link.remove();
    window.setTimeout(() => URL.revokeObjectURL(link.href), 0);
    setConnection(true);
    setMessage(ui.diagnosticReportMessage,
      `Diagnosebericht mit ${snapshot.bytes.byteLength} Logbytes lokal erstellt.` +
        (notices.length ? ` Hinweise: ${notices.join(" ")}` : ""),
      notices.length ? "error" : "success");
  } catch (error) {
    setMessage(ui.diagnosticReportMessage, errorText(error), "error");
  } finally {
    setActionBusy(false);
  }
}

function appendLogText(text) {
  if (!text) return;
  logDisplay += text;
  if (logDisplay.length > LOG_DISPLAY_CHARACTERS) {
    logDisplay = logDisplay.slice(-LOG_DISPLAY_CHARACTERS);
  }
  ui.logOutput.textContent = logDisplay;
  ui.logOutput.scrollTop = ui.logOutput.scrollHeight;
}

function addLogNotice(message) {
  if (!message) return;
  logNotices.push(message);
  logNotices = logNotices.slice(-6);
  updateLogMessage();
}

function updateLogMessage() {
  const details = [...logNotices];
  if (rawLogs.dropped) {
    details.push(`${rawLogs.dropped} ältere Rohbytes wurden aus dem lokalen ` +
      `${LOG_BUFFER_BYTES / 1024 / 1024}-MiB-Puffer entfernt.`);
  }
  setMessage(ui.logMessage, details.join("\n"), rawLogs.dropped ? "error" : "");
  ui.downloadLogs.disabled = rawLogs.size === 0;
}

function setLogStreamConnection(connected) {
  logStreamConnected = connected;
  if (!connected) return;
  const waiters = logReadyWaiters;
  logReadyWaiters = [];
  for (const waiter of waiters) {
    window.clearTimeout(waiter.timer);
    waiter.resolve();
  }
}

function waitForLogStream(timeout) {
  if (logStreamConnected) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const waiter = {resolve, timer: null};
    waiter.timer = window.setTimeout(() => {
      logReadyWaiters = logReadyWaiters.filter((entry) => entry !== waiter);
      reject(new ApiError("Live-Log konnte nicht rechtzeitig gestartet werden."));
    }, timeout);
    logReadyWaiters.push(waiter);
  });
}

function delay(milliseconds, signal) {
  return new Promise((resolve) => {
    if (signal.aborted) {
      resolve();
      return;
    }
    const timer = window.setTimeout(resolve, milliseconds);
    signal.addEventListener("abort", () => {
      window.clearTimeout(timer);
      resolve();
    }, {once: true});
  });
}

async function followLogs(signal) {
  while (!signal.aborted) {
    const requestedCursor = logCursor;
    const previousEpoch = logEpoch;
    const query = new URLSearchParams({follow: "1"});
    if (logCursor !== null) query.set("since", logCursor.toString());
    if (logEpoch !== null) query.set("epoch", logEpoch.toString());

    let response;
    try {
      response = await apiFetch(`${API_ROOT}/logs?${query}`, {
        method: "GET",
        signal,
      });
      await requireSuccess(response);
      if (!response.body) throw new ApiError("Browser stellt keinen Logstream bereit.");
      const handshake = parseLogHandshake(response.headers, requestedCursor,
        previousEpoch);
      logCursor = handshake.start;
      logEpoch = handshake.epoch;
      if (usbCaptureActive && handshake.notices.length) {
        usbCaptureLogGap = true;
      }
      for (const notice of handshake.notices) {
        addLogNotice(notice);
        addUsbCaptureNotice(notice);
      }
      setLogStreamConnection(true);
      setState(ui.logState, "Live", "online");

      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let streamFailure = null;
      try {
        while (!signal.aborted) {
          let chunk;
          try {
            chunk = await reader.read();
          } catch (error) {
            streamFailure = error;
            break;
          }
          const {done, value} = chunk;
          if (done) break;
          rawLogs.append(value);
          appendUsbCaptureBytes(value);
          logCursor += BigInt(value.byteLength);
          appendLogText(decoder.decode(value, {stream: true}));
          updateLogMessage();
        }
        appendLogText(decoder.decode());
      } finally {
        try {
          await reader.cancel();
        } catch {
          // The connection may already be closed.
        }
      }
      if (streamFailure && !signal.aborted) {
        addLogNotice(`${errorText(streamFailure)} Erneuter Versuch …`);
      }
    } catch (error) {
      if (signal.aborted || error?.name === "AbortError") break;
      if (!isTransportFailure(error)) throw error;
      addLogNotice(`${errorText(error)} Erneuter Versuch …`);
    }

    if (!signal.aborted) {
      setLogStreamConnection(false);
      setState(ui.logState, "Verbinde neu", "busy");
      addLogNotice(`Logstream getrennt; Fortsetzung ab Sequenz ${logCursor ?? "?"}.`);
      noteUsbCaptureReconnect();
      await delay(LOG_RECONNECT_DELAY_MS, signal);
    }
  }
  setLogStreamConnection(false);
}

function startLogs() {
  if (logTask) return;
  logController = new AbortController();
  if (!logCaptureStarted) logCaptureStarted = new Date();
  updateLogControls();
  setState(ui.logState, "Verbinde", "busy");
  logTask = followLogs(logController.signal)
    .catch((error) => {
      setMessage(ui.logMessage, errorText(error), "error");
      addUsbCaptureNotice(`Live-Log beendet: ${errorText(error)}`);
      if (usbCaptureActive) {
        setMessage(ui.usbMessage,
          `Live-Log beendet: ${errorText(error)} Der Diagnosebericht kann unvollständig sein.`,
          "error");
      }
      setConnection(false);
    })
    .finally(() => {
      logTask = null;
      logController = null;
      setLogStreamConnection(false);
      updateLogControls();
      setState(ui.logState, "Gestoppt", "offline");
    });
  updateLogControls();
}

async function stopLogs() {
  if (!logTask) return;
  logController?.abort();
  await logTask;
}

function clearLogs() {
  rawLogs.clear();
  logDisplay = "";
  logNotices = [];
  logCaptureStarted = new Date();
  ui.logOutput.textContent = "";
  updateLogMessage();
}

function downloadLogs() {
  if (rawLogs.size === 0) return;
  const blob = new Blob(rawLogs.parts(), {type: "application/octet-stream"});
  const link = document.createElement("a");
  const stamp = new Date().toISOString().replace(/[:.]/g, "-");
  link.href = URL.createObjectURL(blob);
  link.download = `bmx-log-${stamp}.txt`;
  document.body.append(link);
  link.click();
  link.remove();
  window.setTimeout(() => URL.revokeObjectURL(link.href), 0);
  const since = logCaptureStarted?.toLocaleString() ?? "unbekannt";
  setMessage(ui.logMessage,
    `${rawLogs.size} unveränderte Rohbytes heruntergeladen (Erfassung seit ${since}).` +
    (rawLogs.dropped ? ` ${rawLogs.dropped} ältere Bytes fehlten im lokalen Puffer.` : ""),
    "success");
}

function usbStatePresentation(status) {
  if (!status) return {text: "Nicht geladen", kind: "offline"};
  const presentations = {
    idle: {text: "Bereit", kind: "offline"},
    starting: {text: "Startet", kind: "busy"},
    waiting: {text: "Wartet auf Gerät", kind: "busy"},
    capturing: {text: "Erfasst live", kind: "online"},
    stopping: {text: "Stoppt", kind: "busy"},
  };
  return presentations[status.state] ?? {text: status.state, kind: "busy"};
}

function displayUsbStatus(status) {
  usbStatus = status;
  const presentation = usbStatePresentation(status);
  setState(ui.usbState, presentation.text, presentation.kind);
  ui.usbInstruction.textContent = usbDiagnosticInstruction(status);
  ui.usbRemaining.textContent = `${Math.ceil(status.remaining_ms / 1000)} s`;
  ui.usbDevicesSeen.textContent = status.devices_seen.toString();
  ui.usbDescriptorBytes.textContent = status.descriptor_bytes.toString();
  ui.usbInputReports.textContent = status.input_reports.toString();
  ui.usbInputDropped.textContent = status.input_reports_dropped.toString();
  ui.usbInputDuplicates.textContent = status.input_reports_duplicates.toString();
  ui.usbInputCoalesced.textContent = status.input_reports_coalesced.toString();
  ui.usbTruncated.textContent = status.truncated ? "ja" : "nein";
  if (usbCaptureActive) usbCaptureFinalStatus = status;
  updateUsbControls();
}

function clearUsbStatus(message) {
  usbStatus = null;
  setState(ui.usbState, "Nicht verfügbar", "offline");
  ui.usbInstruction.textContent = message;
  for (const output of [
    ui.usbRemaining,
    ui.usbDevicesSeen,
    ui.usbDescriptorBytes,
    ui.usbInputReports,
    ui.usbInputDropped,
    ui.usbInputDuplicates,
    ui.usbInputCoalesced,
    ui.usbTruncated,
  ]) output.textContent = "–";
  updateUsbControls();
}

function selectedUsbDevice() {
  const key = ui.usbDevice.value;
  return usbDevices.find((device) => usbDeviceIdentity(device) === key) ?? null;
}

function displayUsbDevices(devices) {
  const previous = ui.usbDevice.value;
  usbDevices = devices;
  ui.usbDevice.replaceChildren();
  if (devices.length === 0) {
    const option = document.createElement("option");
    option.value = "";
    option.textContent = "Keine bekannten USB-Geräte";
    ui.usbDevice.append(option);
  } else {
    for (const device of devices) {
      const option = document.createElement("option");
      option.value = usbDeviceIdentity(device);
      option.textContent = formatUsbDeviceLabel(device);
      ui.usbDevice.append(option);
    }
    if (devices.some((device) => usbDeviceIdentity(device) === previous)) {
      ui.usbDevice.value = previous;
    }
  }
  updateUsbControls();
}

async function readUsbStatus() {
  const response = await apiFetch(`${API_ROOT}/diagnostics/usb/status`, {
    method: "GET",
  });
  const payload = await jsonResponse(response, "USB-Diagnosestatus");
  try {
    return parseUsbDiagnosticStatus(payload);
  } catch (error) {
    throw new ApiError(errorText(error), response.status);
  }
}

async function readUsbDevices() {
  const response = await apiFetch(`${API_ROOT}/diagnostics/usb/devices`, {
    method: "GET",
  });
  const payload = await jsonResponse(response, "USB-Geräteantwort");
  try {
    return parseUsbDevices(payload);
  } catch (error) {
    throw new ApiError(errorText(error), response.status);
  }
}

async function refreshUsbDevices(announce = true) {
  const devices = await readUsbDevices();
  displayUsbDevices(devices);
  if (announce) {
    setMessage(ui.usbMessage,
      devices.length === 1 ? "Ein USB-Gerät geladen." :
        `${devices.length} USB-Geräte geladen.`, "success");
  }
  return devices;
}

function addUsbCaptureNotice(message) {
  if (!usbCaptureActive || !message) return;
  usbCaptureNotices.push(singleLine(message));
  usbCaptureNotices = usbCaptureNotices.slice(-32);
}

function noteUsbCaptureReconnect() {
  if (!usbCaptureActive) return;
  usbCaptureReconnects += 1;
  setMessage(ui.usbMessage,
    "Live-Log wurde getrennt und wird ab der letzten Sequenz fortgesetzt.");
}

function applyUsbCollectorEvent(event) {
  if (event.reset) {
    usbDisplay = "";
    usbDecoder = new TextDecoder();
  }
  if (!event.chunks.length) {
    if (event.reset) ui.usbOutput.textContent = "";
    return;
  }
  if (!usbDecoder) usbDecoder = new TextDecoder();
  for (const chunk of event.chunks) {
    usbDisplay += usbDecoder.decode(chunk, {stream: true});
  }
  if (usbDisplay.length > USB_DIAGNOSTIC_DISPLAY_CHARACTERS) {
    usbDisplay = usbDisplay.slice(-USB_DIAGNOSTIC_DISPLAY_CHARACTERS);
  }
  ui.usbOutput.textContent = usbDisplay;
  ui.usbOutput.scrollTop = ui.usbOutput.scrollHeight;
}

function appendUsbCaptureBytes(value) {
  if (!usbCaptureActive || !value?.byteLength) return;
  applyUsbCollectorEvent(usbRawLogs.append(value));
}

function beginUsbCapture(mode, target) {
  usbRawLogs.clear();
  usbDisplay = "";
  usbDecoder = new TextDecoder();
  usbCaptureActive = true;
  usbCaptureCompleted = false;
  usbCaptureMode = mode;
  usbCaptureTarget = target
    ? {host: target.host, port: target.port, route: target.route}
    : null;
  usbCaptureStarted = new Date();
  usbCaptureEnded = null;
  usbCaptureSystemStatus = lastSystemStatus;
  usbCaptureFinalStatus = null;
  usbCaptureCompletion = "running";
  usbCaptureNotices = [];
  usbCaptureReconnects = 0;
  usbCaptureLogGap = false;
  ui.usbOutput.textContent = "";
  updateUsbControls();
}

function finishUsbCapture(status, completion, notice = "") {
  if (!usbCaptureActive) return;
  applyUsbCollectorEvent(usbRawLogs.flush());
  usbCaptureActive = false;
  if (usbDecoder) usbDisplay += usbDecoder.decode();
  usbDecoder = null;
  usbCaptureEnded = new Date();
  usbCaptureFinalStatus = status ?? usbCaptureFinalStatus;
  usbCaptureCompletion = completion === "completed" && !usbRawLogs.complete
    ? "END marker missing"
    : completion;
  if (notice) usbCaptureNotices.push(singleLine(notice));
  usbCaptureCompleted = true;
  ui.usbOutput.textContent = usbDisplay;

  const warnings = [];
  if (usbRawLogs.dropped) {
    warnings.push(`${usbRawLogs.dropped} Bytes des Diagnoseabschnitts wurden ` +
      "wegen des Browserlimits nicht gespeichert.");
  }
  if (!usbRawLogs.seenBegin) {
    warnings.push("Die usbdiag-BEGIN-Markierung wurde nicht empfangen.");
  } else if (!usbRawLogs.complete) {
    warnings.push("Die usbdiag-END-Markierung wurde nicht empfangen.");
  }
  if (usbCaptureLogGap) {
    warnings.push("Der Logstream meldete eine Lücke oder einen Reset.");
  }
  if (usbCaptureFinalStatus?.input_reports_dropped) {
    warnings.push(`${usbCaptureFinalStatus.input_reports_dropped} USB-Eingabereports ` +
      "wurden in BMX verworfen.");
  }
  if (usbCaptureFinalStatus?.truncated) {
    warnings.push("BMX meldet abgeschnittene Diagnosedaten.");
  }
  const filtering = [];
  if (usbCaptureFinalStatus?.input_reports_duplicates) {
    filtering.push(`${usbCaptureFinalStatus.input_reports_duplicates} identische Reports gefiltert`);
  }
  if (usbCaptureFinalStatus?.input_reports_coalesced) {
    filtering.push(`${usbCaptureFinalStatus.input_reports_coalesced} schnelle Änderungen verdichtet`);
  }
  setMessage(ui.usbMessage,
    `USB-Diagnose beendet; ${usbRawLogs.received} Bytes im neuesten ` +
      "BEGIN/END-Abschnitt empfangen." +
      (filtering.length ? ` BMX hat ${filtering.join(" und ")}.` : "") +
      (warnings.length ? ` ${warnings.join(" ")}` : " Bericht ist bereit."),
    warnings.length ? "error" : "success");
  updateUsbControls();
}

function syntheticStartingStatus(mode, target) {
  return {
    state: "starting",
    mode,
    waiting_for_device: false,
    target_host: target?.host ?? 0,
    target_port: target?.port ?? 0,
    target_route: target?.route ?? 0,
    remaining_ms: 60000,
    devices_seen: 0,
    descriptor_bytes: 0,
    input_reports: 0,
    input_reports_dropped: 0,
    input_reports_duplicates: 0,
    input_reports_coalesced: 0,
    truncated: false,
  };
}

function startUsbMonitor() {
  const generation = ++usbMonitorGeneration;
  void monitorUsbDiagnostic(generation);
}

async function monitorUsbDiagnostic(generation) {
  const deadline = Date.now() + USB_DIAGNOSTIC_WATCHDOG_MS;
  let lastError = "";
  while (generation === usbMonitorGeneration && Date.now() < deadline) {
    try {
      const status = await readUsbStatus();
      displayUsbStatus(status);
      setConnection(true);
      lastError = "";
      if (!usbDiagnosticIsActive(status)) {
        if (usbCaptureActive && !usbRawLogs.complete) {
          setState(ui.usbState, "Wartet auf Log-Ende", "busy");
          ui.usbInstruction.textContent =
            "BMX ist fertig; der Browser wartet noch auf die usbdiag-END-Zeile im Logstream.";
          setMessage(ui.usbMessage,
            "Diagnosestatus ist beendet; warte auf die vollständige BEGIN/END-Erfassung …");
        } else if (usbCaptureActive) {
          finishUsbCapture(status, "completed");
        }
        if (usbCaptureActive) {
          // END may already be in BMX's ring but still be delayed by network
          // delivery or a reconnect. Keep polling until the collector sees it.
          await new Promise((resolve) =>
            window.setTimeout(resolve, USB_DIAGNOSTIC_POLL_MS));
          continue;
        }
        try {
          await refreshUsbDevices(false);
        } catch {
          // The finished report remains usable if refreshing the list fails.
        }
        return;
      }
    } catch (error) {
      if (generation !== usbMonitorGeneration) return;
      if (error instanceof ApiError && error.status === 403) {
        setConnection(false, errorText(error));
        if (usbCaptureActive) finishUsbCapture(usbCaptureFinalStatus,
          "authorization failed", errorText(error));
        return;
      }
      const message = errorText(error);
      if (message !== lastError) {
        addUsbCaptureNotice(`Statusabfrage: ${message}`);
        setMessage(ui.usbMessage,
          `${message} Die Statusabfrage wird fortgesetzt …`);
        lastError = message;
      }
    }
    await new Promise((resolve) => window.setTimeout(resolve, USB_DIAGNOSTIC_POLL_MS));
  }
  if (generation !== usbMonitorGeneration) return;
  const message = "Browser-Zeitlimit erreicht; BMX-Status konnte nicht als beendet bestätigt werden.";
  if (usbCaptureActive) {
    finishUsbCapture(usbCaptureFinalStatus, "browser watchdog expired", message);
  } else {
    setMessage(ui.usbMessage, message, "error");
  }
}

async function syncUsbDiagnostic() {
  try {
    const status = await readUsbStatus();
    displayUsbStatus(status);
    if (usbDiagnosticIsActive(status)) {
      setMessage(ui.usbMessage,
        "Auf BMX läuft bereits eine USB-Diagnose. Startaktionen bleiben gesperrt.");
      startUsbMonitor();
    } else {
      await refreshUsbDevices(false);
    }
  } catch (error) {
    clearUsbStatus("USB-Diagnose ist derzeit nicht verfügbar.");
    setMessage(ui.usbMessage, errorText(error), "error");
  }
}

const USB_INPUT_CONFIRMATION =
  "USB-Eingabereports können Tastendrücke, Mausbewegungen und Controller-Eingaben " +
  "enthalten. Während der Diagnose keine Passwörter oder vertraulichen Texte " +
  "eingeben. USB-Diagnose jetzt starten?";

async function startUsbDiagnostic(mode) {
  if (usbOperationBusy || usbDiagnosticIsActive(usbStatus)) return;
  const target = mode === "connected" ? selectedUsbDevice() : null;
  if (mode === "connected" && !target) {
    setMessage(ui.usbMessage, "Bitte zuerst ein USB-Gerät auswählen.", "error");
    return;
  }
  if (!window.confirm(USB_INPUT_CONFIRMATION)) return;

  usbOperationBusy = true;
  updateUsbControls();
  try {
    const current = await readUsbStatus();
    displayUsbStatus(current);
    if (usbDiagnosticIsActive(current)) {
      throw new ApiError("Auf BMX läuft bereits eine USB-Diagnose.", 409);
    }

    startLogs();
    await waitForLogStream(STATUS_REQUEST_TIMEOUT_MS);
    beginUsbCapture(mode, target);
    setMessage(ui.usbMessage, "Starte USB-Diagnose …");
    const response = await apiFetch(usbDiagnosticStartPath(mode, target), {
      method: "POST",
      body: new Uint8Array(0),
    });
    await requireSuccess(response);
    displayUsbStatus(syntheticStartingStatus(mode, target));
    startUsbMonitor();
  } catch (error) {
    if (usbCaptureActive) {
      finishUsbCapture(usbStatus, "start failed", errorText(error));
    }
    setMessage(ui.usbMessage, errorText(error), "error");
  } finally {
    usbOperationBusy = false;
    updateUsbControls();
  }
}

async function stopUsbDiagnostic() {
  if (usbOperationBusy || !usbDiagnosticIsActive(usbStatus)) return;
  usbOperationBusy = true;
  updateUsbControls();
  try {
    const response = await apiFetch(`${API_ROOT}/diagnostics/usb/stop`, {
      method: "POST",
      body: new Uint8Array(0),
    });
    await requireSuccess(response);
    displayUsbStatus({...usbStatus, state: "stopping", waiting_for_device: false});
    setMessage(ui.usbMessage, "Stop angefordert; warte auf Abschluss …");
    startUsbMonitor();
  } catch (error) {
    setMessage(ui.usbMessage, errorText(error), "error");
  } finally {
    usbOperationBusy = false;
    updateUsbControls();
  }
}

async function refreshUsbDeviceList() {
  if (usbOperationBusy || usbDiagnosticIsActive(usbStatus)) return;
  usbOperationBusy = true;
  updateUsbControls();
  try {
    await refreshUsbDevices();
  } catch (error) {
    setMessage(ui.usbMessage, errorText(error), "error");
  } finally {
    usbOperationBusy = false;
    updateUsbControls();
  }
}

function downloadUsbDiagnostic() {
  if (!usbCaptureCompleted || !usbCaptureStarted || !usbCaptureEnded) return;
  const notices = usbCaptureNotices.slice();
  if (usbRawLogs.dropped) {
    notices.push(`${usbRawLogs.dropped} Bytes nach Erreichen des lokalen ` +
      "Diagnosepufferlimits wurden nicht gespeichert.");
  }
  const header = usbDiagnosticReportHeader({
    startedAt: usbCaptureStarted,
    endedAt: usbCaptureEnded,
    mode: usbCaptureMode,
    target: usbCaptureTarget,
    systemStatus: usbCaptureSystemStatus,
    diagnosticStatus: usbCaptureFinalStatus,
    receivedBytes: usbRawLogs.received,
    storedBytes: usbRawLogs.size,
    droppedBytes: usbRawLogs.dropped,
    reconnects: usbCaptureReconnects,
    beginSeen: usbRawLogs.seenBegin,
    endSeen: usbRawLogs.complete,
    logGap: usbCaptureLogGap,
    notices,
    completion: usbCaptureCompletion,
  });
  const blob = new Blob([
    header,
    ...usbRawLogs.parts(),
    "\n--- END CAPTURED LOG BYTES ---\n",
  ], {type: "text/plain;charset=utf-8"});
  const link = document.createElement("a");
  const stamp = usbCaptureStarted.toISOString().replace(/[:.]/g, "-");
  link.href = URL.createObjectURL(blob);
  link.download = `bmx-usb-diagnostic-${stamp}.txt`;
  document.body.append(link);
  link.click();
  link.remove();
  window.setTimeout(() => URL.revokeObjectURL(link.href), 0);
  setMessage(ui.usbMessage,
    `Diagnosebericht mit ${usbRawLogs.size} rohen Bytes aus dem neuesten ` +
      "BEGIN/END-Abschnitt heruntergeladen.",
    "success");
}

async function headRemote(destination) {
  const response = await apiFetch(destination.requestPath, {method: "HEAD"});
  if (response.status === 404) return null;
  await requireSuccess(response);
  return parseRemoteFileHeaders(response.headers);
}

function xhrUpload(path, file, sha256, onProgress) {
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open("PUT", path);
    request.timeout = 300000;
    request.setRequestHeader("Content-Type", "application/octet-stream");
    request.setRequestHeader("X-BMX-SHA256", sha256);
    if (password) request.setRequestHeader("X-Password", password);
    request.upload.addEventListener("progress", (event) => {
      if (event.lengthComputable) onProgress(event.loaded / event.total);
    });
    request.addEventListener("load", () => resolve(request));
    request.addEventListener("error", () => reject(
      new ApiError("Uploadverbindung zu BMX abgebrochen.", null, true)));
    request.addEventListener("timeout", () => reject(
      new ApiError("Upload zu BMX hat das Zeitlimit überschritten.", null, true)));
    request.addEventListener("abort", () => reject(new ApiError("Upload abgebrochen.")));
    request.send(file);
  });
}

async function parseXhrJson(request, context) {
  if (request.status === 403) {
    throw new ApiError("Developer-Passwort fehlt oder ist falsch.", 403);
  }
  if (request.status === 411) {
    throw new ApiError(
      "Der Browser hat keine feste Content-Length gesendet; dieser Browser kann " +
      "Dateien deshalb nicht direkt zu BMX hochladen.", 411);
  }
  if (request.status < 200 || request.status >= 300) {
    let detail = "";
    try {
      const payload = JSON.parse(request.responseText);
      if (typeof payload?.error === "string") detail = `: ${singleLine(payload.error)}`;
    } catch {
      detail = request.responseText ? `: ${singleLine(request.responseText).slice(0, 512)}` : "";
    }
    throw new ApiError(`BMX antwortet mit HTTP ${request.status}${detail}`, request.status);
  }
  try {
    const payload = JSON.parse(request.responseText);
    if (!payload || typeof payload !== "object" || Array.isArray(payload)) throw new Error();
    return payload;
  } catch {
    throw new ApiError(`${context} enthält kein gültiges JSON-Objekt.`, request.status);
  }
}

async function postReboot() {
  const response = await apiFetch(`${API_ROOT}/reboot`, {
    method: "POST",
    body: new Uint8Array(0),
  });
  const payload = await jsonResponse(response, "Reboot-Antwort");
  if (payload.reboot_scheduled !== undefined && payload.reboot_scheduled !== true) {
    throw new ApiError("BMX hat den Reboot nicht bestätigt.");
  }
  return payload;
}

async function waitForReboot(beforeStatus, report) {
  const deadline = Date.now() + REBOOT_TIMEOUT_MS;
  let disconnected = false;
  let lastError = "";
  while (Date.now() < deadline) {
    try {
      const current = await readStatus({timeout: STATUS_REQUEST_TIMEOUT_MS});
      const observation = rebootObservation(beforeStatus, current, disconnected);
      if (observation.observed) {
        displayStatus(current);
        setConnection(true);
        return observation;
      }
    } catch (error) {
      if (error instanceof ApiError && error.status === 403) throw error;
      if (isTransportFailure(error)) disconnected = true;
      lastError = errorText(error);
    }
    const seconds = Math.max(0, Math.ceil((deadline - Date.now()) / 1000));
    report(`Warte auf Reboot … noch ${seconds} s${lastError ? ` (${lastError})` : ""}`);
    await new Promise((resolve) => window.setTimeout(resolve, REBOOT_POLL_MS));
  }
  throw new ApiError("BMX-Reboot wurde innerhalb von 180 Sekunden nicht beobachtet.");
}

async function deploy() {
  const file = ui.deployFile.files?.[0];
  if (!file) {
    setMessage(ui.deployMessage, "Bitte zuerst eine lokale Datei auswählen.", "error");
    return;
  }

  setActionBusy(true);
  ui.deployProgress.value = 0;
  try {
    const destination = parseDestination(ui.deployDestination.value);
    setMessage(ui.deployMessage, "Berechne lokalen SHA-256 …");
    const local = await hashFile(file, (progress) => {
      ui.deployProgress.value = Math.round(progress * 35);
    });

    setMessage(ui.deployMessage, `Prüfe ${destination.displayPath} …`);
    const remote = await headRemote(destination);
    ui.deployProgress.value = 40;
    const unchanged = sameRemoteFile(remote, local);
    const reboot = ui.deployReboot.checked;

    if (unchanged && !reboot) {
      ui.deployProgress.value = 100;
      setMessage(ui.deployMessage,
        `Identisch (${local.sha256}). Kein Upload erforderlich.`, "success");
      return;
    }

    const beforeStatus = reboot ? await readStatus() : null;
    if (reboot) await stopLogs();

    if (unchanged) {
      setMessage(ui.deployMessage, "Datei ist identisch; plane nur den Reboot …");
      await postReboot();
    } else {
      setMessage(ui.deployMessage, `Lade ${file.name} nach ${destination.displayPath} …`);
      const path = destination.requestPath + (reboot ? "?reboot=1" : "");
      const request = await xhrUpload(path, file, local.sha256, (progress) => {
        ui.deployProgress.value = 40 + Math.round(progress * 50);
      });
      const payload = await parseXhrJson(request, "PUT-Antwort");
      validatePutResult(payload, local, reboot);
    }

    if (reboot) {
      setMessage(ui.deployMessage, "Upload bestätigt; warte auf den Neustart …");
      await waitForReboot(beforeStatus, (message) => setMessage(ui.deployMessage, message));
    }

    const verified = await headRemote(destination);
    if (!sameRemoteFile(verified, local)) {
      throw new ApiError(`Die Datei ${destination.displayPath} stimmt nach dem Deploy nicht überein.`);
    }
    ui.deployProgress.value = 100;
    setMessage(ui.deployMessage,
      `${unchanged ? "Datei war bereits identisch" : "Deploy abgeschlossen"}; ` +
      `SHA-256 ${local.sha256} verifiziert.${reboot ? " Lade die Oberfläche neu …" : ""}`,
      "success");
    if (reboot) window.setTimeout(() => window.location.reload(), 1500);
  } catch (error) {
    setMessage(ui.deployMessage, errorText(error), "error");
  } finally {
    setActionBusy(false);
  }
}

async function reboot() {
  if (!window.confirm("BMX jetzt sauber rebooten und auf den Neustart warten?")) return;
  setActionBusy(true);
  try {
    const beforeStatus = await readStatus();
    await stopLogs();
    setMessage(ui.rebootMessage, "Reboot wird geplant …");
    await postReboot();
    await waitForReboot(beforeStatus,
      (message) => setMessage(ui.rebootMessage, message));
    setMessage(ui.rebootMessage, "BMX ist wieder erreichbar. Lade die Oberfläche neu …", "success");
    window.setTimeout(() => window.location.reload(), 1000);
  } catch (error) {
    setMessage(ui.rebootMessage, errorText(error), "error");
  } finally {
    setActionBusy(false);
  }
}

async function connect() {
  if (actionBusy) return;
  if (/[\r\n]/.test(ui.password.value)) {
    setMessage(ui.connectionMessage, "Das Passwort darf keinen Zeilenumbruch enthalten.", "error");
    return;
  }
  if ([...ui.password.value].length > 63 ||
      [...ui.password.value].some((character) => character.codePointAt(0) > 0xff)) {
    setMessage(ui.connectionMessage,
      "Das Passwort muss in 63 HTTP-Bytes passen.", "error");
    return;
  }
  password = ui.password.value;
  setActionBusy(true);
  try {
    await refreshStatus();
    await syncUsbDiagnostic();
  } catch {
    // refreshStatus already rendered a useful error.
  } finally {
    setActionBusy(false);
  }
}

async function forgetPassword() {
  ++usbMonitorGeneration;
  if (usbCaptureActive) {
    finishUsbCapture(usbStatus, "password cleared",
      "Passwort wurde während der Diagnose aus dem Browser entfernt.");
  }
  await stopLogs();
  password = "";
  ui.password.value = "";
  displayUsbDevices([]);
  clearUsbStatus("Verbinde dich zuerst mit BMX.");
  setMessage(ui.connectionMessage, "Passwort aus diesem Tab gelöscht.");
  setConnection(false);
}

ui.deviceAddress.textContent = `${window.location.protocol}//${window.location.host}`;
ui.connect.addEventListener("click", connect);
ui.password.addEventListener("keydown", (event) => {
  if (event.key === "Enter") connect();
});
ui.forgetPassword.addEventListener("click", forgetPassword);
ui.refreshStatus.addEventListener("click", async () => {
  setActionBusy(true);
  try {
    await refreshStatus();
    await syncUsbDiagnostic();
  } catch {
    // refreshStatus rendered the error.
  } finally {
    setActionBusy(false);
  }
});
ui.downloadDiagnosticReport.addEventListener("click", downloadDiagnosticReport);
ui.startLogs.addEventListener("click", startLogs);
ui.stopLogs.addEventListener("click", stopLogs);
ui.clearLogs.addEventListener("click", clearLogs);
ui.downloadLogs.addEventListener("click", downloadLogs);
ui.usbStartNew.addEventListener("click", () => startUsbDiagnostic("new"));
ui.usbRefreshDevices.addEventListener("click", refreshUsbDeviceList);
ui.usbDevice.addEventListener("change", updateUsbControls);
ui.usbStartConnected.addEventListener("click",
  () => startUsbDiagnostic("connected"));
ui.usbStop.addEventListener("click", stopUsbDiagnostic);
ui.usbDownload.addEventListener("click", downloadUsbDiagnostic);
ui.deployFile.addEventListener("change", () => {
  const file = ui.deployFile.files?.[0];
  if (file && !ui.deployDestination.value) {
    ui.deployDestination.value = `SYS:/${file.name}`;
  }
});
ui.deploy.addEventListener("click", deploy);
ui.reboot.addEventListener("click", reboot);
window.addEventListener("beforeunload", () => {
  ++usbMonitorGeneration;
  logController?.abort();
});

setActionBusy(true);
let initialPasswordRequired = false;
refreshStatus(false, {timeout: STATUS_REQUEST_TIMEOUT_MS})
  .then(() => syncUsbDiagnostic())
  .catch((error) => {
    if (error instanceof ApiError && error.status === 403) {
      initialPasswordRequired = true;
    }
  })
  .finally(() => {
    setActionBusy(false);
    if (initialPasswordRequired) ui.password.focus();
  });
