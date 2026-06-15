/**
 * @file      app.js
 * @version   WEB_VERSION 4.9
 * @brief     Dashboard renderer, realtime sync, operation alerts and OTA deployment pipeline.
 */

let prevData = null;
let latestWeatherData = null;
let latestGatewayStatus = null;

// Sensor Node sends LoRa data every ~10 minutes.
// Do NOT mark warning/offline too early.
const LORA_EXPECTED_INTERVAL_MIN = 10;
const LORA_WATCH_MIN = 15;
const LORA_OFFLINE_MIN = 20;

// Gateway heartbeat is independent from LoRa sensor packet.
const GATEWAY_HEARTBEAT_WATCH_MIN = 5;
const GATEWAY_HEARTBEAT_OFFLINE_MIN = 10;

function byId(id) {
  return document.getElementById(id);
}

function setText(id, text) {
  const el = byId(id);
  if (el) el.textContent = text;
}

function setFixed(id, value, decimals = 1) {
  const el = byId(id);
  if (!el) return;
  el.textContent = value == null || !Number.isFinite(Number(value))
    ? "--"
    : Number(value).toFixed(decimals);
}

function formatFrameCounter(v) {
  return v == null ? "--" : `#${v}`;
}

function formatLocalDateTime(isoString) {
  if (!isoString) return "--";
  const d = new Date(isoString);
  if (Number.isNaN(d.getTime())) return "--";
  return d.toLocaleString("vi-VN", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    day: "2-digit",
    month: "2-digit"
  });
}

function minutesSince(isoString) {
  if (!isoString) return null;
  const t = new Date(isoString).getTime();
  if (Number.isNaN(t)) return null;
  return (Date.now() - t) / 60000;
}

function ageText(mins) {
  if (mins == null) return "--";
  if (mins < 1) return "vừa nhận";
  if (mins < 60) return `${Math.round(mins)} phút`;
  const h = Math.floor(mins / 60);
  const m = Math.round(mins % 60);
  return `${h} giờ ${m} phút`;
}

function formatDuration(sec) {
  if (sec == null || !Number.isFinite(Number(sec))) return "--";
  const s = Math.max(0, Math.floor(Number(sec)));
  const hh = String(Math.floor(s / 3600)).padStart(2, "0");
  const mm = String(Math.floor((s % 3600) / 60)).padStart(2, "0");
  const ss = String(s % 60).padStart(2, "0");
  return `${hh}:${mm}:${ss}`;
}

function shortText(v, n = 24) {
  if (!v) return "--";
  const s = String(v);
  return s.length > n ? `${s.slice(0, n)}…` : s;
}

function calculateDelta(cur, prev, unit = "") {
  if (cur == null || prev == null) return "Chưa đủ dữ liệu so sánh";
  const diff = Number(cur) - Number(prev);
  if (!Number.isFinite(diff)) return "Chưa đủ dữ liệu so sánh";
  if (Math.abs(diff) < 0.05) return "→ Không biến động";
  return diff > 0
    ? `▲ Tăng +${Math.abs(diff).toFixed(1)}${unit}`
    : `▼ Giảm −${Math.abs(diff).toFixed(1)}${unit}`;
}

function healthFlagText(flag) {
  if (flag == null) return "--";
  const n = Number(flag);
  if (!Number.isFinite(n)) return "--";
  if (n === 0) return "OK";
  return `Lỗi 0x${n.toString(16).toUpperCase().padStart(2, "0")}`;
}

function setConnectionState(label, mode = "neutral") {
  const dot = byId("connDot");
  const connLabel = byId("connLabel");
  if (connLabel) connLabel.textContent = label;
  if (dot) {
    dot.classList.remove("live", "offline", "warn");
    if (mode === "live") dot.classList.add("live");
    else if (mode === "offline") dot.classList.add("offline");
    else if (mode === "warn") dot.classList.add("warn");
  }
}

function latestLoraTimestamp() {
  return latestGatewayStatus?.last_lora_packet_at || latestWeatherData?.created_at || null;
}

function updateSensorStatusText() {
  const statusEl = byId("sensor-status-text");
  const loraAge = minutesSince(latestLoraTimestamp());

  if (!statusEl) return;

  if (loraAge == null) {
    statusEl.textContent = "CHƯA CÓ DỮ LIỆU";
    statusEl.style.color = "var(--text-2)";
    return;
  }

  if (loraAge > LORA_OFFLINE_MIN) {
    statusEl.textContent = `MẤT GÓI LORA · ${ageText(loraAge)}`;
    statusEl.style.color = "var(--red)";
  } else if (loraAge > LORA_WATCH_MIN) {
    statusEl.textContent = `THEO DÕI LORA · ${ageText(loraAge)}`;
    statusEl.style.color = "var(--amber)";
  } else {
    statusEl.textContent = "HỆ THỐNG ỔN ĐỊNH";
    statusEl.style.color = "var(--green)";
  }
}

function buildOperationAlerts() {
  const alerts = [];
  const loraAge = minutesSince(latestLoraTimestamp());
  const gwAge = minutesSince(latestGatewayStatus?.gateway_last_seen_at);
  const flag = latestWeatherData?.health_flag;
  const batt = latestWeatherData?.battery;
  const rssi = latestGatewayStatus?.wifi_rssi;
  const heap = latestGatewayStatus?.free_heap;

  if (loraAge == null) {
    alerts.push({ level: "warn", text: "Chưa có thời điểm gói LoRa gần nhất để đánh giá." });
  } else if (loraAge > LORA_OFFLINE_MIN) {
    alerts.push({ level: "danger", text: `Không nhận gói LoRa ${ageText(loraAge)}. Ngưỡng mất dữ liệu là ${LORA_OFFLINE_MIN} phút.` });
  } else if (loraAge > LORA_WATCH_MIN) {
    alerts.push({ level: "warn", text: `Gói LoRa đang chậm ${ageText(loraAge)}. Chu kỳ dự kiến ${LORA_EXPECTED_INTERVAL_MIN} phút, bắt đầu theo dõi sau ${LORA_WATCH_MIN} phút.` });
  }

  if (gwAge != null && gwAge > GATEWAY_HEARTBEAT_OFFLINE_MIN) {
    alerts.push({ level: "danger", text: `Gateway heartbeat ngừng cập nhật ${ageText(gwAge)}.` });
  } else if (gwAge != null && gwAge > GATEWAY_HEARTBEAT_WATCH_MIN) {
    alerts.push({ level: "warn", text: `Gateway heartbeat chậm ${ageText(gwAge)}.` });
  }

  if (flag != null && Number(flag) !== 0) {
    alerts.push({ level: "warn", text: `Sensor health flag khác OK: ${healthFlagText(flag)}.` });
  }

  if (batt != null && Number(batt) < 3.6) {
    alerts.push({ level: "danger", text: `Nguồn pin thấp: ${Number(batt).toFixed(2)} V.` });
  } else if (batt != null && Number(batt) < 3.8) {
    alerts.push({ level: "warn", text: `Nguồn pin cần theo dõi: ${Number(batt).toFixed(2)} V.` });
  }

  if (rssi != null && Number(rssi) < -85) {
    alerts.push({ level: "danger", text: `WiFi rất yếu: ${Number(rssi).toFixed(0)} dBm.` });
  } else if (rssi != null && Number(rssi) < -75) {
    alerts.push({ level: "warn", text: `WiFi yếu: ${Number(rssi).toFixed(0)} dBm.` });
  }

  if (heap != null && Number(heap) < 30000) {
    alerts.push({ level: "danger", text: `Free heap thấp: ${(Number(heap) / 1024).toFixed(1)} KB.` });
  } else if (heap != null && Number(heap) < 60000) {
    alerts.push({ level: "warn", text: `Free heap cần theo dõi: ${(Number(heap) / 1024).toFixed(1)} KB.` });
  }

  return alerts;
}

function renderOperationStatus() {
  updateSensorStatusText();

  const panel = byId("opsPanel");
  const badge = byId("opsStateBadge");
  const text = byId("opsStateText");
  const sub = byId("opsStateSub");
  const list = byId("opsAlertList");

  const alerts = buildOperationAlerts();
  const hasDanger = alerts.some(a => a.level === "danger");
  const hasWarn = alerts.some(a => a.level === "warn");

  if (panel) {
    panel.classList.remove("ok", "warn", "danger");
    panel.classList.add(hasDanger ? "danger" : hasWarn ? "warn" : "ok");
  }

  if (badge) {
    badge.classList.remove("ok", "warn", "danger");
    badge.classList.add(hasDanger ? "danger" : hasWarn ? "warn" : "ok");
    badge.textContent = hasDanger ? "CẦN KIỂM TRA" : hasWarn ? "THEO DÕI" : "ỔN ĐỊNH";
  }

  if (text) {
    text.textContent = hasDanger
      ? "Có cảnh báo vận hành cần xử lý."
      : hasWarn
        ? "Hệ thống vẫn chạy, nhưng có thông số cần theo dõi."
        : "Hệ thống hoạt động ổn định.";
  }

  const loraAge = minutesSince(latestLoraTimestamp());
  if (sub) {
    sub.textContent = `Chu kỳ LoRa dự kiến ${LORA_EXPECTED_INTERVAL_MIN} phút. Gói gần nhất: ${ageText(loraAge)}. Chỉ cảnh báo sau ${LORA_WATCH_MIN} phút và xem là mất dữ liệu sau ${LORA_OFFLINE_MIN} phút.`;
  }

  if (list) {
    if (alerts.length === 0) {
      list.innerHTML = `<div class="ops-alert ok">🟢 Không có cảnh báo. LoRa, nguồn, WiFi và gateway trong ngưỡng bình thường.</div>`;
    } else {
      list.innerHTML = alerts.map(a => {
        const icon = a.level === "danger" ? "🔴" : "🟡";
        return `<div class="ops-alert ${a.level}">${icon} ${a.text}</div>`;
      }).join("");
    }
  }

  setText("gwFirmwareShort", shortText(latestGatewayStatus?.firmware_version, 20));
  setText("gwWifiShort", latestGatewayStatus?.wifi_rssi == null ? "--" : `${Number(latestGatewayStatus.wifi_rssi).toFixed(0)} dBm`);
  setText("gwLoraAge", ageText(loraAge));
  setText("gwHeapShort", latestGatewayStatus?.free_heap == null ? "--" : `${(Number(latestGatewayStatus.free_heap) / 1024).toFixed(1)} KB`);

  if (hasDanger) setConnectionState("CẦN KIỂM TRA", "offline");
  else if (hasWarn) setConnectionState("THEO DÕI", "warn");
  else setConnectionState("ỔN ĐỊNH", "live");
}

function updateUI(data) {
  if (!data) return;
  latestWeatherData = data;

  setFixed("temperature", data.temperature, 1);
  setFixed("humidity", data.humidity, 1);
  setFixed("pressure", data.pressure, 0);
  setFixed("board_temp", data.board_temp, 1);
  setFixed("battery", data.battery, 2);
  setFixed("predicted_temp_2h", data.predicted_temp_2h, 1);

  setText("deviceChip", data.device_id ?? CONFIG.DEVICE_ID ?? "--");
  setText("frameChip", formatFrameCounter(data.frame_counter));
  setText("lastUpdateChip", formatLocalDateTime(data.created_at));
  setText("healthFlag", healthFlagText(data.health_flag));

  if (prevData) {
    setText("tempSub", calculateDelta(data.temperature, prevData.temperature, "°C"));
    setText("humSub", calculateDelta(data.humidity, prevData.humidity, "%"));
    setText("presSub", calculateDelta(data.pressure, prevData.pressure, " hPa"));
  } else {
    setText("tempSub", "Dữ liệu đo mới nhất từ Sensor Node");
    setText("humSub", "Độ ẩm tương đối");
    setText("presSub", "Áp suất khí quyển");
  }

  setText("boardTempSub", data.board_temp == null ? "Chưa có dữ liệu board temp" : "Nhiệt độ nội bộ/bo mạch");

  const batCard = byId("batCard");
  if (batCard) batCard.classList.toggle("low", data.battery != null && Number(data.battery) < 3.6);

  if (data.battery == null) setText("batSub", "Chưa có dữ liệu nguồn");
  else if (Number(data.battery) >= 4.0) setText("batSub", "🟢 Nguồn tốt");
  else if (Number(data.battery) >= 3.6) setText("batSub", "🟡 Theo dõi nguồn");
  else setText("batSub", "🔴 Nguồn thấp");

  if (data.predicted_temp_2h != null && data.temperature != null) {
    const diff = Number(data.predicted_temp_2h) - Number(data.temperature);
    setText("predictedSub", `MLR dự báo cho thời điểm đo +2 giờ (${diff >= 0 ? "+" : ""}${diff.toFixed(1)}°C so với hiện tại).`);
  } else {
    setText("predictedSub", "Chưa có giá trị dự báo MLR.");
  }

  prevData = data;
  renderOperationStatus();
}

function updateGatewayStatus(data) {
  latestGatewayStatus = data;
  renderOperationStatus();
}

async function updateRecordCount() {
  try {
    const count = await countWeatherLogs();
    setText("recordsChip", count ?? "--");
  } catch (err) {
    console.warn("[Dashboard] Không thể đếm records:", err);
    setText("recordsChip", "--");
  }
}

async function sha256Hex(file) {
  const buf = await file.arrayBuffer();
  const digest = await crypto.subtle.digest("SHA-256", buf);
  return [...new Uint8Array(digest)].map(b => b.toString(16).padStart(2, "0")).join("");
}

function getOtaDom() {
  return {
    fileInput: byId("otaFileInput") || byId("otaFileInputModal") || byId("otaFileInputDash") || byId("firmwareFile"),
    logEl: byId("otaStatusLog") || byId("otaStatusLogModal") || byId("otaStatusLogDash") || byId("otaLog")
  };
}

async function uploadAndDeployFirmware() {
  const { fileInput, logEl } = getOtaDom();
  if (!fileInput || !logEl) return;

  const file = fileInput.files[0];
  if (!file) {
    logEl.textContent = "⚠ Vui lòng chọn file firmware dạng .bin trước.";
    logEl.style.color = "var(--amber)";
    return;
  }
  if (!file.name.toLowerCase().endsWith(".bin")) {
    logEl.textContent = "⚠ File OTA phải là firmware .bin.";
    logEl.style.color = "var(--amber)";
    return;
  }

  try {
    const bucket = CONFIG.STORAGE?.FIRMWARE_BUCKET ?? "firmware";
    const deviceId = CONFIG.DEVICE_ID ?? "ESP32_LORA_GW";
    const ota_seq = Date.now();

    logEl.innerHTML = "⏳ Đang tính SHA-256 firmware...";
    logEl.style.color = "var(--text-2)";

    const ota_sha256 = await sha256Hex(file);
    const fileName = `firmware_${ota_seq}_esp32_gateway.bin`;

    logEl.innerHTML = "⏳ Đang tải firmware lên Supabase Storage...";
    const { error: uploadError } = await supabaseClient.storage.from(bucket).upload(fileName, file, {
      cacheControl: "0",
      upsert: false,
      contentType: "application/octet-stream"
    });
    if (uploadError) throw uploadError;

    const { data: { publicUrl } } = supabaseClient.storage.from(bucket).getPublicUrl(fileName);

    logEl.innerHTML = "⏳ Đang ghi lệnh OTA vào device_configs...";
    const { error: dbError } = await supabaseClient.from(CONFIG.API.CONFIG_TABLE).upsert({
      device_id: deviceId,
      ota_url: publicUrl,
      ota_seq,
      ota_sha256,
      updated_at: new Date().toISOString()
    }, { onConflict: "device_id" });
    if (dbError) throw dbError;

    logEl.innerHTML = `✅ <b>PHÁT LỆNH OTA THÀNH CÔNG</b><br>
      • Bucket: ${bucket}<br>
      • Device: ${deviceId}<br>
      • ota_seq: ${ota_seq}<br>
      • SHA-256: <span class="ota-long-text">${ota_sha256}</span><br>
      • URL: <span class="ota-long-text">${publicUrl}</span>`;
    logEl.style.color = "var(--green)";
    fileInput.value = "";
  } catch (err) {
    console.error("[OTA Pipeline Error]:", err);
    logEl.textContent = `❌ LỖI OTA PIPELINE: ${err.message}`;
    logEl.style.color = "var(--red)";
  }
}

function initStaticConfigUI() {
  setText("cfgMode", CONFIG.MODE);
  setText("cfgTable", CONFIG.API.TABLE);
  setText("cfgUrl", CONFIG.SUPABASE_URL);
  setText("cfgLimit", CONFIG.FETCH_LIMIT);
  setText("webVersionLabel", CONFIG.PIPELINE?.WEB_VERSION ?? WEB_VERSION ?? "--");

  const cfgMode = byId("cfgMode");
  if (cfgMode) cfgMode.className = "cr-val " + (CONFIG.MODE === "DEMO" ? "mode-sim" : "mode-real");

  const modeBadge = byId("modeBadge");
  if (modeBadge) {
    modeBadge.textContent = CONFIG.MODE === "REAL" ? "DB REAL" : "DEMO";
    modeBadge.className = "mode-badge " + (CONFIG.MODE === "DEMO" ? "demo" : "real");
  }
}

function startUptimeCounter() {
  const t0 = Date.now();
  setInterval(() => {
    const s = Math.floor((Date.now() - t0) / 1000);
    const hh = String(Math.floor(s / 3600)).padStart(2, "0");
    const mm = String(Math.floor((s % 3600) / 60)).padStart(2, "0");
    const ss = String(s % 60).padStart(2, "0");
    setText("uptimeChip", `${hh}:${mm}:${ss}`);
  }, 1000);
}

async function initDashboard() {
  initStaticConfigUI();
  startUptimeCounter();
  setConnectionState("CONNECTING", "neutral");

  try {
    const latest = await fetchLatestWeather();
    if (latest) updateUI(latest);
  } catch (err) {
    console.error("[Dashboard] Không thể tải latest weather:", err);
    setConnectionState("DB ERROR", "offline");
  }

  try {
    if (typeof fetchLatestGatewayStatus === "function") {
      const gw = await fetchLatestGatewayStatus();
      if (gw) updateGatewayStatus(gw);
    }
  } catch (err) {
    console.warn("[Dashboard] Không thể tải gateway_status:", err);
  }

  await updateRecordCount();

  subscribeRealtime(async (newRow) => {
    console.log("[Weather Realtime RX]", newRow);
    updateUI(newRow);
    await updateRecordCount();
  });

  if (typeof subscribeGatewayStatus === "function") {
    subscribeGatewayStatus((row) => {
      console.log("[Gateway Status RX]", row);
      updateGatewayStatus(row);
    });
  }

  setInterval(renderOperationStatus, 15000);
  setInterval(updateRecordCount, 10000);

  const otaBtn = document.querySelector(".config-ota-block .ota-btn");
  if (otaBtn) otaBtn.addEventListener("click", (e) => {
    e.preventDefault();
    uploadAndDeployFirmware();
  });
}

document.addEventListener("DOMContentLoaded", initDashboard);
