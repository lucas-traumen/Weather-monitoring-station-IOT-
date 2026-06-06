/**
 * @file      app.js
 * @version   WEB_VERSION 4.1
 * @brief     Dashboard controller — plaintext DB synchronized with Edge Function pipeline
 *
 * New architecture:
 *   STM32 -> LoRa ASCON key1 -> ESP32 -> ASCON key2 HTTPS -> Edge Function
 *   Edge Function decrypts key2 and inserts plaintext into public.weather_logs.
 *   Web reads plaintext rows and subscribes to Supabase Realtime.
 */

let prevData = null;
let realtimeChannel = null;

const fmt = {
  temp: (v) => v == null ? "--" : Number(v).toFixed(1),
  hum:  (v) => v == null ? "--" : Number(v).toFixed(1),
  pres: (v) => v == null ? "--" : Number(v).toFixed(0),
  volt: (v) => v == null ? "--" : Number(v).toFixed(2),
  id:   (v) => v ?? "--"
};

function setText(id, value) {
  const el = document.getElementById(id);
  if (el) el.textContent = value;
}

function setHTML(id, value) {
  const el = document.getElementById(id);
  if (el) el.innerHTML = value;
}

function setConnStatus(label, isLive) {
  setText("connLabel", label);
  const dot = document.getElementById("connDot");
  if (dot) dot.classList.toggle("live", !!isLive);
}

function calculateDelta(cur, prev, unit = "") {
  if (cur == null || prev == null) return "— Chưa đủ dữ liệu";
  const diff = cur - prev;
  if (Math.abs(diff) < 0.05) return "→ Không biến động";
  return diff > 0
    ? `▲ Tăng +${Math.abs(diff).toFixed(1)}${unit}`
    : `▼ Giảm −${Math.abs(diff).toFixed(1)}${unit}`;
}

function updateGatewayStatusUI(latestData) {
  const statusText = document.getElementById("sensor-status-text");
  const heroInlineStatus = document.getElementById("hero-status-inline");
  if (!statusText || !heroInlineStatus) return;

  if (!latestData || !latestData.created_at) {
    statusText.innerHTML = "⚫ CHƯA CÓ DỮ LIỆU";
    statusText.style.color = "var(--text-2)";
    heroInlineStatus.innerHTML = "OFFLINE";
    heroInlineStatus.style.background = "rgba(248,113,113,0.1)";
    heroInlineStatus.style.color = "var(--text-2)";
    setConnStatus("NO DATA", false);
    return;
  }

  const diffMinutes = (Date.now() - new Date(latestData.created_at).getTime()) / 60000;

  if (diffMinutes > 5) {
    statusText.innerHTML = `🔴 MẤT KẾT NỐI TÍN HIỆU (${Math.round(diffMinutes)} phút trước)`;
    statusText.style.color = "var(--red)";
    heroInlineStatus.innerHTML = "DISCONNECTED";
    heroInlineStatus.style.background = "rgba(239,68,68,0.1)";
    heroInlineStatus.style.color = "var(--red)";
    setConnStatus("STALE", false);
  } else {
    statusText.innerHTML = "🟢 HỆ THỐNG HOẠT ĐỘNG ỔN ĐỊNH";
    statusText.style.color = "var(--green)";
    heroInlineStatus.innerHTML = "ONLINE";
    heroInlineStatus.style.background = "rgba(16,185,129,0.1)";
    heroInlineStatus.style.color = "var(--green)";
    setConnStatus("LIVE", true);
  }
}

function updateUI(data) {
  if (!data) return;

  // Main metrics
  setText("temperature", fmt.temp(data.temperature));
  setText("humidity", fmt.hum(data.humidity));
  setText("pressure", fmt.pres(data.pressure));
  setText("board_temp", fmt.temp(data.board_temp));
  setText("battery", fmt.volt(data.battery));
  setText("predicted_temp_2h", fmt.temp(data.predicted_temp_2h));

  // Hero mini metrics
  setText("humidity-mini", data.humidity == null ? "-- %" : `${fmt.hum(data.humidity)} %`);
  setText("pressure-mini", data.pressure == null ? "-- hPa" : `${fmt.pres(data.pressure)} hPa`);
  setText("pred-mini", data.predicted_temp_2h == null ? "-- °C" : `${fmt.temp(data.predicted_temp_2h)} °C`);
  setText("hero-boardtemp", data.board_temp == null ? "-- °C" : `${fmt.temp(data.board_temp)} °C`);
  setText("hero-battery", data.battery == null ? "-- V" : `${fmt.volt(data.battery)} V`);

  // Device and counters
  const device = data.device_id ?? CONFIG.DEVICE_ID;
  setText("deviceChip", device);
  setText("hero-device", device);
  setText("deviceId", device);
  setText("packetCounter", data.frame_counter != null ? `#${data.frame_counter}` : "--");

  // Time
  if (data.created_at) {
    const t = new Date(data.created_at).toLocaleTimeString("vi-VN");
    setText("updated", t);
    setText("updated-footer", t);
    setText("lastUpdateChip", t);
  }

  // Battery state
  const batCard = document.getElementById("batCard");
  if (batCard && data.battery != null) {
    batCard.classList.toggle("low", data.battery < 3.6);
    setText("batSub",
      data.battery >= 4.0 ? "🟢 Đầy nguồn" :
      data.battery >= 3.6 ? "🟡 Tốt" :
      "🔴 Cạn nguồn - cần sạc"
    );
  } else if (data.battery == null) {
    setText("batSub", "— Không có dữ liệu pin");
  }

  // Trends
  if (prevData) {
    setText("tempSub", calculateDelta(data.temperature, prevData.temperature, "°C"));
    setText("humSub",  calculateDelta(data.humidity, prevData.humidity, "%"));
    setText("presSub", calculateDelta(data.pressure, prevData.pressure, " hPa"));
  } else {
    setText("tempSub", "— Đang lấy mốc so sánh");
    setText("humSub", "— Đang lấy mốc so sánh");
    setText("presSub", "— Đang lấy mốc so sánh");
  }

  if (data.predicted_temp_2h != null && data.temperature != null) {
    const diff = data.predicted_temp_2h - data.temperature;
    setText("predictedSub",
      `MLR tại gateway · dự báo lệch ${diff >= 0 ? "+" : ""}${diff.toFixed(1)}°C · Edge Function đã lưu DB plaintext`
    );
  } else {
    setText("predictedSub", "MLR tại gateway · chờ dữ liệu dự báo");
  }

  updateGatewayStatusUI(data);
  prevData = data;
}

async function updateCount() {
  try {
    const count = await countWeatherLogs();
    setText("recordsChip", count);
  } catch (err) {
    console.warn("[Dashboard] Không đếm được records:", err);
    setText("recordsChip", "--");
  }
}

async function fetchAndRenderLatest() {
  try {
    const latest = await fetchLatestWeather();
    if (latest) updateUI(latest);
    else updateGatewayStatusUI(null);
  } catch (err) {
    console.error("[Dashboard] Lỗi nạp bản ghi mới nhất:", err);
    setHTML("sensor-status-text", "🔴 LỖI ĐỌC DATABASE");
    setConnStatus("DB ERROR", false);
  }
}

async function uploadAndDeployFirmware() {
  const fileInput = document.getElementById("otaFileInput");
  const logEl = document.getElementById("otaStatusLog");
  if (!fileInput || !logEl) return;

  const file = fileInput.files[0];
  if (!file) {
    logEl.textContent = "⚠ Vui lòng chọn file firmware dạng .bin trước!";
    logEl.style.color = "var(--amber)";
    return;
  }

  try {
    logEl.innerHTML = "⏳ Đang tải firmware lên Supabase Storage...";
    logEl.style.color = "var(--text-2)";

    const safeName = file.name.replace(/[^a-zA-Z0-9._-]/g, "_");
    const fileName = `firmware_${Date.now()}_${safeName}`;

    const { error: uploadError } = await supabaseClient
      .storage
      .from(CONFIG.STORAGE.FIRMWARE_BUCKET)
      .upload(fileName, file, { upsert: true });

    if (uploadError) throw uploadError;

    const { data: { publicUrl } } = supabaseClient
      .storage
      .from(CONFIG.STORAGE.FIRMWARE_BUCKET)
      .getPublicUrl(fileName);

    logEl.innerHTML = "⏳ Đang ghi lệnh OTA vào device_configs...";

    const { error: dbError } = await supabaseClient
      .from(CONFIG.API.CONFIG_TABLE)
      .upsert([{
        device_id: CONFIG.DEVICE_ID,
        ota_url: publicUrl,
        updated_at: new Date().toISOString()
      }]);

    if (dbError) throw dbError;

    logEl.innerHTML =
      `✅ <b>PHÁT LỆNH OTA THÀNH CÔNG!</b><br>` +
      `• Bucket: ${CONFIG.STORAGE.FIRMWARE_BUCKET}<br>` +
      `• Device: ${CONFIG.DEVICE_ID}<br>` +
      `<span style="font-size:0.62rem;color:var(--text-2);">URL: ${publicUrl}</span>`;

    logEl.style.color = "var(--green)";
    fileInput.value = "";
  } catch (err) {
    console.error("[OTA Pipeline Error]:", err);
    logEl.textContent = `❌ LỖI OTA PIPELINE: ${err.message}`;
    logEl.style.color = "var(--red)";
  }
}

async function initDashboard() {
  // Config modal values
  setText("cfgMode", CONFIG.MODE);
  setText("cfgTable", CONFIG.API.TABLE);
  setText("cfgUrl", CONFIG.SUPABASE_URL);
  setText("cfgLimit", CONFIG.FETCH_LIMIT);

  const cfgMode = document.getElementById("cfgMode");
  if (cfgMode) cfgMode.className = "cr-val " + (CONFIG.MODE === "DEMO" ? "mode-sim" : "mode-real");

  const modeChip = document.getElementById("modeChip");
  if (modeChip) {
    modeChip.textContent = CONFIG.MODE;
    modeChip.className = "ic-val " + (CONFIG.MODE === "DEMO" ? "mode-sim" : "mode-real");
  }

  setConnStatus("CONNECTING", false);

  await fetchAndRenderLatest();
  await updateCount();

  realtimeChannel = subscribeRealtime(
    async (newRow) => {
      console.log("[Realtime RX]", newRow);
      updateUI(newRow);
      await updateCount();
    },
    (status) => {
      if (status === "SUBSCRIBED") {
        setConnStatus(prevData ? "LIVE" : "WAITING", !!prevData);
      } else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT") {
        setConnStatus("WS ERROR", false);
      } else if (status === "CLOSED") {
        setConnStatus("CLOSED", false);
      }
    }
  );

  setInterval(() => {
    if (prevData) updateGatewayStatusUI(prevData);
  }, 15000);

  setInterval(updateCount, 10000);
}

window.uploadAndDeployFirmware = uploadAndDeployFirmware;
document.addEventListener("DOMContentLoaded", initDashboard);
