async function getJSON(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return await res.json();
}

async function postJSON(url, body) {
  const res = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return await res.json();
}

const listEl = document.getElementById("device-list");
const form = document.getElementById("cfg-form");
const statusEl = document.getElementById("status");
const previewContainer = document.getElementById("preview-container");
const previewGrid = document.getElementById("preview-grid");
const previewCells = previewGrid ? Array.from(previewGrid.querySelectorAll("img")) : [];
const previewPlaceholder = document.getElementById("preview-placeholder");
const previewMainImg = document.getElementById("preview-main-img");
const previewMainPlaceholder = document.getElementById("preview-main-placeholder");
const previewPrevBtn = document.getElementById("preview-prev");
const previewNextBtn = document.getElementById("preview-next");
const previewPageEl = document.getElementById("preview-page");
const lastUploadEl = document.getElementById("last-upload");
const refreshBtn = document.getElementById("refresh-btn");
const deviceCountEl = document.getElementById("device-count");
const selectedDeviceSubtitle = document.getElementById("selected-device-subtitle");
const aiIndicatorEl = document.getElementById("ai-indicator");
const openPreviewBtn = document.getElementById("open-preview");
const analysisTextEl = document.getElementById("analysis-text");
const analysisHostEl = document.getElementById("analysis-host");
const analysisModelEl = document.getElementById("analysis-model");
const analysisPromptEl = document.getElementById("analysis-prompt");
const analysisNumCtxEl = document.getElementById("analysis-num-ctx");
const analysisNumPredictEl = document.getElementById("analysis-num-predict");
const analysisTimeEl = document.getElementById("analysis-time");

const $ = (id) => document.getElementById(id);

const PREVIEW_PAGE_SIZE = 4;
let currentPreviewUrls = [];
let currentPreviewPage = 0;
let currentMainPreviewUrl = null;
let currentDeviceId = null;
let hasInitialSelection = false;
const deviceCards = new Map();
let listLoadedOnce = false;

if (previewCells.length) {
  previewCells.forEach((img) => {
    img.addEventListener("click", () => {
      const url = img.dataset.url;
      if (url) {
        currentMainPreviewUrl = url;
        setMainPreview(url);
      }
    });
  });
}

if (previewPrevBtn) {
  previewPrevBtn.addEventListener("click", () => {
    if (currentPreviewPage > 0) {
      currentPreviewPage -= 1;
      renderPreviewGrid();
    }
  });
}

if (previewNextBtn) {
  previewNextBtn.addEventListener("click", () => {
    const totalPages = Math.max(1, Math.ceil(currentPreviewUrls.length / PREVIEW_PAGE_SIZE));
    if (currentPreviewPage < totalPages - 1) {
      currentPreviewPage += 1;
      renderPreviewGrid();
    }
  });
}

if (refreshBtn) {
  refreshBtn.addEventListener("click", () => {
    refreshDevices();
  });
}

if (openPreviewBtn) {
  openPreviewBtn.addEventListener("click", () => {
    const url = openPreviewBtn.dataset.url;
    if (url) window.open(url, "_blank", "noopener");
  });
}

function numberOrNull(value) {
  const num = Number(value);
  return Number.isFinite(num) ? num : null;
}

function formatDateTime(ts) {
  const num = numberOrNull(ts);
  if (!num) return null;
  try {
    return new Date(num * 1000).toLocaleString("tr-TR", { hour12: false });
  } catch (e) {
    return new Date(num * 1000).toUTCString();
  }
}

function setDeviceCount(count) {
  if (!deviceCountEl) return;
  deviceCountEl.textContent = `${count} cihaz`;
}

function updateSelectedSubtitle(meta) {
  if (!selectedDeviceSubtitle) return;
  if (!meta) {
    selectedDeviceSubtitle.textContent = "Henuz secilmedi";
    updateAiIndicator(null);
    return;
  }
  const seen = formatDateTime(meta.lastSeen) || "-";
  selectedDeviceSubtitle.textContent = "ID " + meta.deviceId + " | Son gorulme " + seen;
  updateAiIndicator(meta.aiReachable);
}

function updateAiIndicator(status) {
  if (!aiIndicatorEl) return;
  if (status === null || status === undefined) {
    aiIndicatorEl.classList.add("hidden");
    aiIndicatorEl.classList.add("offline");
    aiIndicatorEl.setAttribute("title", "A.I. katmani baglanti durumu bilinmiyor");
    return;
  }
  aiIndicatorEl.classList.remove("hidden");
  const online = Boolean(status);
  aiIndicatorEl.classList.toggle("offline", !online);
  aiIndicatorEl.setAttribute("title", online ? "A.I. katmani bagli" : "A.I. katmani erisilemiyor");
}

function updateAnalysis(meta) {
  const analysis = meta?.lastAnalysis ? String(meta.lastAnalysis).trim() : "";
  const analysisTime = meta ? formatDateTime(meta.lastAnalysisTime) : null;

  analysisTextEl.textContent = analysis || "Analiz verisi yok.";
  analysisTextEl.classList.toggle("muted", !analysis);
  analysisTextEl.title = analysis || "";

  const host = meta?.aiHost?.trim() || "-";
  const model = meta?.aiModel?.trim() || "-";
  const prompt = meta?.aiPrompt ? String(meta.aiPrompt) : "-";
  const numCtx = meta?.aiNumCtx ?? "-";
  const numPredict = meta?.aiNumPredict ?? "-";

  analysisHostEl.textContent = host;
  analysisHostEl.title = host != "-" ? host : "";

  analysisModelEl.textContent = model;
  analysisModelEl.title = model != "-" ? model : "";

  analysisPromptEl.textContent = prompt;
  analysisPromptEl.title = prompt != "-" ? prompt : "";

  analysisNumCtxEl.textContent = numCtx;
  analysisNumCtxEl.title = numCtx != "-" ? String(numCtx) : "";

  analysisNumPredictEl.textContent = numPredict;
  analysisNumPredictEl.title = numPredict != "-" ? String(numPredict) : "";

  analysisTimeEl.textContent = analysisTime || "-";
  analysisTimeEl.title = analysisTime || "";
}

function setMainPreview(url, skipHighlight = false) {
  currentMainPreviewUrl = url || null;
  if (!previewMainImg || !previewMainPlaceholder) return;
  if (url) {
    const bust = url.includes("?") ? "&" : "?";
    previewMainImg.src = `${url}${bust}v=${Date.now()}`;
    previewMainImg.style.display = "block";
    previewMainPlaceholder.style.display = "none";
  } else {
    previewMainImg.removeAttribute("src");
    previewMainImg.style.display = "none";
    previewMainPlaceholder.style.display = "block";
  }
  if (openPreviewBtn) {
    if (url) {
      openPreviewBtn.disabled = false;
      openPreviewBtn.dataset.url = url;
    } else {
      openPreviewBtn.disabled = true;
      delete openPreviewBtn.dataset.url;
    }
  }
  if (!skipHighlight) {
    highlightThumbnails();
  }
}

function highlightThumbnails() {
  if (!previewCells.length) return;
  previewCells.forEach((img) => {
    const cell = img.parentElement;
    const url = img.dataset.url;
    const active = currentMainPreviewUrl && url === currentMainPreviewUrl;
    if (cell) cell.classList.toggle("active", !!active);
  });
}

function renderPreviewGrid() {
  if (!previewGrid || !previewPlaceholder) return;
  const total = currentPreviewUrls.length;
  const pageCount = total ? Math.ceil(total / PREVIEW_PAGE_SIZE) : 1;
  if (currentPreviewPage >= pageCount) currentPreviewPage = Math.max(0, pageCount - 1);
  const sliceStart = currentPreviewPage * PREVIEW_PAGE_SIZE;
  const slice = currentPreviewUrls.slice(sliceStart, sliceStart + PREVIEW_PAGE_SIZE);

  previewCells.forEach((img, idx) => {
    const cell = img.parentElement;
    const url = slice[idx];
    if (url) {
      const bust = url.includes("?") ? "&" : "?";
      img.src = `${url}${bust}v=${Date.now()}`;
      img.dataset.url = url;
      img.style.display = "block";
      if (cell) cell.style.display = "block";
    } else {
      img.removeAttribute("src");
      img.removeAttribute("data-url");
      img.style.display = "none";
      if (cell) cell.style.display = "none";
    }
  });

  const hasImages = total > 0;
  previewGrid.style.display = hasImages ? "grid" : "none";
  previewPlaceholder.style.display = hasImages ? "none" : "block";
  previewPlaceholder.textContent = hasImages ? "" : "Hen�z JPEG alinmadi.";

  if (previewPrevBtn) previewPrevBtn.disabled = !hasImages || currentPreviewPage === 0;
  if (previewNextBtn) previewNextBtn.disabled = !hasImages || currentPreviewPage >= Math.max(0, pageCount - 1);
  if (previewPageEl) previewPageEl.textContent = hasImages ? `${currentPreviewPage + 1} / ${pageCount}` : "0 / 0";

  if (hasImages) {
    if (!currentMainPreviewUrl || !currentPreviewUrls.includes(currentMainPreviewUrl)) {
      currentMainPreviewUrl = currentPreviewUrls[0];
    }
    setMainPreview(currentMainPreviewUrl, true);
  } else {
    setMainPreview(null, true);
  }
  highlightThumbnails();
}

function updatePreview(meta) {
  if (!previewContainer) return;
  const urls = Array.isArray(meta?.lastImgUrls) && meta.lastImgUrls.length
    ? meta.lastImgUrls
    : (meta && meta.lastImgUrl ? [meta.lastImgUrl] : []);

  currentPreviewUrls = urls;
  currentPreviewPage = 0;

  updateAnalysis(meta);

  renderPreviewGrid();

  if (lastUploadEl) {
    const ts = meta ? numberOrNull(meta.lastImgTime) : null;
    if (ts) {
      const text = formatDateTime(ts);
      lastUploadEl.textContent = `Son upload: ${text}`;
    } else {
      lastUploadEl.textContent = "Son upload: bilgi yok";
    }
  }
}

function highlightSelectedCard() {
  if (!listEl) return;
  listEl.querySelectorAll(".device-card").forEach((card) => {
    card.classList.toggle("selected", card.dataset.deviceId === currentDeviceId);
  });
}

function createDeviceCard(device) {
  const card = document.createElement("div");
  card.className = "device-card";
  card.dataset.deviceId = device.deviceId;

  card.innerHTML = `
    <div class="device-card-header">
      <h3 data-field="deviceId"></h3>
      <div class="device-card-badges">
        <span class="chip chip-status" data-field="status"></span>
        <span class="ai-chip" data-field="ai"></span>
      </div>
    </div>
    <div class="device-meta">
      <span data-field="ip"></span>
      <span data-field="rssi"></span>
      <span data-field="auto"></span>
    </div>
    <div class="device-meta">
      <span data-field="framesize"></span>
      <span data-field="quality"></span>
      <span data-field="interval"></span>
    </div>
    <div class="device-meta">
      <span data-field="lastUpload"></span>
      <span data-field="lastSeen"></span>
    </div>
    <div class="device-actions">
      <button class="btn tiny subtle" type="button">Sec</button>
      <span class="device-url" data-field="uploadUrl"></span>
    </div>
  `;

  const refs = {};
  card.querySelectorAll("[data-field]").forEach((el) => {
    refs[el.dataset.field] = el;
  });
  card._refs = refs;

  const button = card.querySelector("button");
  card.addEventListener("click", () => {
    selectDevice(card.dataset.deviceId);
  });
  if (button) {
    button.addEventListener("click", (ev) => {
      ev.stopPropagation();
      selectDevice(card.dataset.deviceId);
    });
  }

  updateDeviceCard(card, device);
  return card;
}

function updateDeviceCard(card, device) {
  const refs = card._refs || {};
  card.dataset.deviceId = device.deviceId;

  const lastSeenSec = numberOrNull(device.lastSeen);
  const nowSec = Date.now() / 1000;
  const offline = !lastSeenSec || (nowSec - lastSeenSec > 60);

  card.classList.toggle("offline", offline);

  if (refs.deviceId) refs.deviceId.textContent = device.deviceId;
  if (refs.status) {
    refs.status.textContent = offline ? "Offline" : "Online";
    refs.status.classList.toggle("offline", offline);
    refs.status.classList.toggle("online", !offline);
  }
  if (refs.ai) {
    const reachable = device.aiReachable;
    if (reachable === undefined || reachable === null) {
      refs.ai.classList.add("hidden");
      refs.ai.classList.add("offline");
      refs.ai.style.display = "none";
      refs.ai.title = "";
    } else {
      refs.ai.classList.remove("hidden");
      refs.ai.style.display = "inline-flex";
      refs.ai.textContent = "AI";
      const isOnline = Boolean(reachable);
      refs.ai.classList.toggle("offline", !isOnline);
      refs.ai.title = isOnline ? "A.I. katmani bagli" : "A.I. katmani erisilemiyor";
    }
  }
  }
  if (refs.ip) refs.ip.textContent = `IP: ${device.ip || "-"}`;
  if (refs.rssi) refs.rssi.textContent = `RSSI: ${device.rssi ?? "-"}`;
  if (refs.auto) refs.auto.textContent = `Auto: ${device.autoUpload ? "Acik" : "Kapali"}`;
  if (refs.framesize) refs.framesize.textContent = `FS: ${device.framesize || "-"}`;
  if (refs.quality) refs.quality.textContent = `Q: ${device.jpegQuality ?? "-"}`;
  const intervalValue = device.uploadIntervalSec != null ? `${device.uploadIntervalSec}s` : "-";
  if (refs.interval) refs.interval.textContent = `Interval: ${intervalValue}`;

  const lastUploadText = formatDateTime(device.lastImgTime) || "-";
  const lastSeenText = formatDateTime(device.lastSeen) || "-";
  if (refs.lastUpload) refs.lastUpload.textContent = `Last JPEG: ${lastUploadText}`;
  if (refs.lastSeen) refs.lastSeen.textContent = `Last seen: ${lastSeenText}`;

  if (refs.uploadUrl) {
    if (device.uploadUrl) {
      refs.uploadUrl.textContent = device.uploadUrl;
      refs.uploadUrl.title = device.uploadUrl;
      refs.uploadUrl.style.display = "inline-block";
    } else {
      refs.uploadUrl.textContent = "";
      refs.uploadUrl.title = "";
      refs.uploadUrl.style.display = "none";
    }
  }
}

function upsertDeviceCard(device) {
  let card = deviceCards.get(device.deviceId);
  if (!card) {
    card = createDeviceCard(device);
    deviceCards.set(device.deviceId, card);
  } else {
    updateDeviceCard(card, device);
  }
  return card;
}

function renderDeviceList(items) {
  if (!listEl) return;

  const seen = new Set();
  items.forEach((device, index) => {
    const card = upsertDeviceCard(device);
    seen.add(device.deviceId);
    const currentNode = listEl.children[index];
    if (currentNode !== card) {
      listEl.insertBefore(card, currentNode || null);
    }
  });

  for (let i = listEl.children.length - 1; i >= 0; i -= 1) {
    const child = listEl.children[i];
    const id = child.dataset.deviceId;
    if (!seen.has(id)) {
      listEl.removeChild(child);
      deviceCards.delete(id);
    }
  }

  highlightSelectedCard();
}



async function refreshDevices() {
  if (!listEl) return;
  if (!listLoadedOnce) {
    listEl.innerHTML = "<div class=\"loading\">Yukleniyor...</div>";
  }

  try {
    const items = await getJSON("/admin/api/devices");
    listLoadedOnce = true;
    setDeviceCount(items.length);

    if (!items.length) {
      deviceCards.clear();
      listEl.innerHTML = "<div class=\"empty-state\">Simdilik cihaz yok.</div>";
      hasInitialSelection = false;
      currentDeviceId = null;
      updateSelectedSubtitle(null);
      updatePreview(null);
      return;
    }


    renderDeviceList(items);

    if (currentDeviceId) {
      const current = items.find((it) => it.deviceId === currentDeviceId);
      if (current) {
        updatePreview(current);
        updateSelectedSubtitle(current);
      } else {
        currentDeviceId = null;
        hasInitialSelection = false;
        updateSelectedSubtitle(null);
        updatePreview(null);
      }
    }

    if (!currentDeviceId && !hasInitialSelection) {
      hasInitialSelection = true;
      await selectDevice(items[0].deviceId);
    }
  } catch (e) {
    console.error("Device list refresh error", e);
    if (!listLoadedOnce) {
      listEl.innerHTML = `<div class=\"empty-state error\">Hata: ${e.message}</div>`;
    } else if (statusEl) {
      statusEl.innerHTML = `<span class=\"err\">Liste yenilemede hata: ${e.message}</span>`;
    }
  }
}



















async function selectDevice(id) {
  try {
    const d = await getJSON(`/admin/api/device/${id}`);
    currentDeviceId = d.deviceId;
    highlightSelectedCard();

    $("deviceId").value = d.deviceId;
    $("framesize").value = d.framesize;
    $("jpegQuality").value = d.jpegQuality;
    $("uploadIntervalSec").value = d.uploadIntervalSec;
    $("autoUpload").checked = !!d.autoUpload;
    $("uploadUrl").value = d.uploadUrl || "";
    $("uploadToken").value = "";

    $("whitebal").checked = !!d.whitebal;
    $("wbMode").value = String(d.wbMode ?? 0);
    $("hmirror").checked = !!d.hmirror;
    $("vflip").checked = !!d.vflip;
    $("brightness").value = d.brightness ?? 0;
    $("contrast").value = d.contrast ?? 0;
    $("saturation").value = d.saturation ?? 0;
    $("sharpness").value = d.sharpness ?? 0;
    $("gainceiling").value = String(d.gainceiling ?? 4);
    $("aeLevel").value = d.aeLevel ?? 0;
    $("specialEffect").value = String(d.specialEffect ?? 0);
    $("awbGain").checked = !!d.awbGain;
    $("gainCtrl").checked = !!d.gainCtrl;
    $("exposureCtrl").checked = !!d.exposureCtrl;
    $("lensCorr").checked = !!d.lensCorr;
    $("rawGma").checked = !!d.rawGma;
    $("bpc").checked = !!d.bpc;
    $("wpc").checked = !!d.wpc;
    $("dcw").checked = !!d.dcw;
    $("colorbar").checked = !!d.colorbar;
    $("lowLightBoost").checked = !!d.lowLightBoost;

    $("aiHost").value = d.aiHost || "";
    $("aiModel").value = d.aiModel || "";
    $("aiPrompt").value = d.aiPrompt || "";
    $("aiNumCtx").value = d.aiNumCtx ?? "";
    $("aiNumPredict").value = d.aiNumPredict ?? "";

    updatePreview(d);
    updateSelectedSubtitle(d);
    statusEl.textContent = "";
  } catch (e) {
    statusEl.innerHTML = `<span class=\"err\">Secim hatasi: ${e.message}</span>`;
  }
}

function parseIntSafe(value) {
  const v = Number.parseInt(String(value), 10);
  return Number.isNaN(v) ? null : v;
}

form.addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const id = $("deviceId").value.trim();
  if (!id) {
    statusEl.innerHTML = "<span class=\"err\">Once bir cihaz sec.</span>";
    return;
  }

  const intField = (value) => {
    const trimmed = String(value ?? "").trim();
    if (!trimmed) return null;
    const parsed = parseIntSafe(trimmed);
    return parsed;
  };

  const body = {
    framesize: $("framesize").value,
    jpegQuality: parseIntSafe($("jpegQuality").value),
    uploadIntervalSec: parseIntSafe($("uploadIntervalSec").value),
    autoUpload: $("autoUpload").checked,
    uploadUrl: $("uploadUrl").value.trim(),
    whitebal: $("whitebal").checked,
    wbMode: parseIntSafe($("wbMode").value),
    hmirror: $("hmirror").checked,
    vflip: $("vflip").checked,
    brightness: parseIntSafe($("brightness").value),
    contrast: parseIntSafe($("contrast").value),
    saturation: parseIntSafe($("saturation").value),
    sharpness: parseIntSafe($("sharpness").value),
    awbGain: $("awbGain").checked,
    gainCtrl: $("gainCtrl").checked,
    exposureCtrl: $("exposureCtrl").checked,
    gainceiling: parseIntSafe($("gainceiling").value),
    aeLevel: parseIntSafe($("aeLevel").value),
    lensCorr: $("lensCorr").checked,
    rawGma: $("rawGma").checked,
    bpc: $("bpc").checked,
    wpc: $("wpc").checked,
    dcw: $("dcw").checked,
    colorbar: $("colorbar").checked,
    specialEffect: parseIntSafe($("specialEffect").value),
    lowLightBoost: $("lowLightBoost").checked,
    aiHost: $("aiHost").value.trim(),
    aiModel: $("aiModel").value.trim(),
    aiPrompt: $("aiPrompt").value,
    aiNumCtx: intField($("aiNumCtx").value),
    aiNumPredict: intField($("aiNumPredict").value),
  };

  // allow clearing string fields by sending empty string
  if (!body.aiHost) body.aiHost = "";
  if (!body.aiModel) body.aiModel = "";
  if (!body.aiPrompt) body.aiPrompt = "";

  if (body.aiNumCtx === null) body.aiNumCtx = null;
  if (body.aiNumPredict === null) body.aiNumPredict = null;

  const token = $("uploadToken").value.trim();
  if (token) {
    body.uploadToken = token;
  }

  try {
    const resp = await postJSON(`/admin/api/device/${id}/config`, body);
    statusEl.innerHTML = `<span class=\"ok\">Kaydedildi.</span>`;
    await refreshDevices();
  } catch (e) {
    statusEl.innerHTML = `<span class=\"err\">Kaydetme hatasi: ${e.message}</span>`;
  }
});

refreshDevices();













