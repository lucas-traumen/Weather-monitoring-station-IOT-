// app.js — IoTSenseHub Dashboard & MLOps Control Pipeline
let prevData = null;

function delta(cur, prev, unit = '') {
  if (prev == null) return '—';
  const d = cur - prev;
  if (Math.abs(d) < 0.05) return '→ stable';
  return d > 0
    ? `▲ +${Math.abs(d).toFixed(1)}${unit} từ gói cũ`
    : `▼ −${Math.abs(d).toFixed(1)}${unit} từ gói cũ`;
}

function setSub(id, txt) {
  const el = document.getElementById(id);
  if (el) el.textContent = txt;
}

// -------------------------------------------------------------------
// 1. HÀM THUẬT TOÁN KIỂM TRA NHỊP TIM (HEARTBEAT MONITORING)
// -------------------------------------------------------------------
function updateSensorStatusUI(latestData) {
    const statusText = document.getElementById('sensor-status-text');
    if (!statusText) return;

    if (!latestData || !latestData.created_at) {
        statusText.innerHTML = "⚫ CHƯA CÓ DỮ LIỆU";
        statusText.style.color = "var(--text-2)";
        return;
    }

    const lastSeenTime = new Date(latestData.created_at).getTime();
    const currentTime = new Date().getTime();
    const diffMinutes = (currentTime - lastSeenTime) / (1000 * 60);

    if (diffMinutes > 5) {
        statusText.innerHTML = `🔴 MẤT KẾT NỐI TÍN HIỆU (${Math.round(diffMinutes)} phút trước)`;
        statusText.style.color = "#FF4C4C"; 
    } 
    else if (latestData.temperature === 0 && latestData.humidity === 0) {
        statusText.innerHTML = "🟡 LỖI ĐỌC CẢM BIẾN PHẦN CỨNG";
        statusText.style.color = "#FFD700"; 
    } 
    else {
        statusText.innerHTML = "🟢 TRUYỀN DỮ LIỆU ỔN ĐỊNH";
        statusText.style.color = "#00FF7F"; 
    }
}

// -------------------------------------------------------------------
// 2. HÀM XỬ LÝ VÀ ĐỔ DỮ LIỆU LÊN GIAO DIỆN (MAIN UI RENDERER)
// -------------------------------------------------------------------
function updateUI(data) {
  if (!data) return;

  // Trích xuất các chỉ số môi trường & kỹ thuật
  const temp = parseFloat(data.temperature) || 0;
  const hum  = parseFloat(data.humidity)    || 0;
  const pres = parseFloat(data.pressure)    || 0;
  const bat  = parseFloat(data.battery)     || 0;
  const boardTemp = parseFloat(data.board_temp) || 0; // Đã bổ sung Board Temp

  // Trích xuất các thông số Telemetry từ thuật toán AI SLR
  const bPress = parseFloat(data.beta_pressure) != null ? parseFloat(data.beta_pressure) : 0;
  const bHum   = parseFloat(data.beta_humidity) != null ? parseFloat(data.beta_humidity) : 0;
  const pTemp  = parseFloat(data.predicted_temp) != null ? parseFloat(data.predicted_temp) : 0;

  // Hàm helper đổ số vào thẻ HTML
  function setVal(id, num, dec) {
    const el = document.getElementById(id);
    if (!el) return;
    const unitEl = el.querySelector('.unit');
    const unitHTML = unitEl ? unitEl.outerHTML : '';
    el.innerHTML = num.toFixed(dec) + unitHTML;
  }

  // Đẩy số liệu môi trường ra UI
  setVal('temperature', temp, 1);
  setVal('humidity',    hum,  1);
  setVal('pressure',    pres, 1);
  setVal('battery',     bat,  2);
  setVal('board_temp',  boardTemp, 1); // Render trực tiếp nhiệt độ bo mạch lên thẻ

  // Đẩy số liệu đánh giá mô hình toán học (Edge AI)
  const bPresEl = document.getElementById('beta_pressure');
  if (bPresEl) bPresEl.textContent = bPress >= 0 ? `+${bPress.toFixed(4)}` : bPress.toFixed(4);
  
  const bHumEl = document.getElementById('beta_humidity');
  if (bHumEl) bHumEl.textContent = bHum >= 0 ? `+${bHum.toFixed(4)}` : bHum.toFixed(4);
  
  setVal('predicted_temp', pTemp, 1);

  const trendEl = document.getElementById('trend');
  if (trendEl) trendEl.textContent = data.pressure_trend || '--';

  // Xử lý logic Thẻ Cảnh báo (Alert System)
  const warnEl = document.getElementById('warning');
  const statCard = document.getElementById('statusCard');
  const isStorm = data.storm_warning;
  const isFrost = data.frost_warning;

  if (warnEl) {
    if (isStorm && isFrost) {
      warnEl.innerHTML = '<span style="color:var(--red)">⚠ DÔNG LỐC & SƯƠNG MÙ</span>';
      if (statCard) statCard.className = 'card storm';
    } else if (isStorm) {
      warnEl.innerHTML = '<span style="color:var(--red)">⚠ CẢNH BÁO DÔNG LỐC</span>';
      if (statCard) statCard.className = 'card storm';
    } else if (isFrost) {
      warnEl.innerHTML = '<span style="color:var(--cyan)">❄ CẢNH BÁO SƯƠNG GIÁ</span>';
      if (statCard) statCard.className = 'card frost';
    } else {
      warnEl.innerHTML = '<span style="color:var(--green)">✓ HỆ THỐNG AN TOÀN</span>';
      if (statCard) statCard.className = 'card';
    }
  }

  // Cập nhật các mẹo phân tích xu hướng phụ (Sub-text)
  setSub('tempSub',      delta(temp, prevData?.temperature, '°C'));
  setSub('humSub',       delta(hum,  prevData?.humidity,    '%'));
  setSub('boardTempSub', `Biến thiên mạch: ${delta(boardTemp, prevData?.board_temp, '°C')}`);
  setSub('presSub',      `${pres < 1000 ? 'Áp suất thấp' : pres > 1020 ? 'Áp suất cao' : 'Ổn định'} · ${delta(pres, prevData?.pressure, ' hPa')}`);
  setSub('predictedSub', `Dự liệu xu hướng: ${temp_slope_text(temp, pTemp)}`);
  setSub('statusSub',    `Mã thiết bị biên: ${data.device_id || 'unknown'}`);

  // Cập nhật các thẻ Header tĩnh
  const devEl = document.getElementById('deviceChip');
  if (devEl) devEl.textContent = data.device_id || 'unknown';

  const updEl = document.getElementById('updated');
  if (updEl) updEl.textContent = new Date(data.created_at).toLocaleString('vi-VN');

  const luEl = document.getElementById('lastUpdateChip');
  if (luEl) luEl.textContent = new Date(data.created_at).toLocaleTimeString('vi-VN', { hour12: false });

  // Gọi ngay hàm đánh giá nhịp tim khi có dữ liệu mới
  updateSensorStatusUI(data);

  // Lưu lại lịch sử để lần sau so sánh Delta
  prevData = data;
}

// Hàm hỗ trợ dịch xu hướng nhiệt độ
function temp_slope_text(cur, pred) {
  const diff = pred - cur;
  if(Math.abs(diff) < 0.2) return "Nhiệt độ đứng yên";
  return diff > 0 ? `Nhiệt độ dự kiến tăng lên ${pred.toFixed(1)}°C` : `Nhiệt độ dự kiến giảm sâu xuống ${pred.toFixed(1)}°C`;
}

// -------------------------------------------------------------------
// 3. ĐƯỜNG ỐNG UPLOAD OTA FIRMWARE (MLOPS DEPLOYMENT)
// -------------------------------------------------------------------
async function uploadAndDeployFirmware() {
  const fileInput = document.getElementById('otaFileInput');
  const logEl = document.getElementById('otaStatusLog');
  
  if (!fileInput || fileInput.files.length === 0) {
    alert("Vui lòng chọn một file firmware .bin từ máy tính trước!");
    return;
  }
  
  const file = fileInput.files[0];
  const uniqueFileName = `firmware_${Date.now()}.bin`; // Ép cache-busting
  
  logEl.textContent = "⏳ Bước 1/3: Đang đẩy file cấu trúc .bin lên đám mây Supabase Storage...";
  logEl.style.color = "var(--amber)";

  try {
    const { data: storageData, error: storageError } = await supabaseClient
      .storage
      .from('firmware')
      .upload(uniqueFileName, file, { cacheControl: '0', upsert: true });

    if (storageError) throw storageError;

    logEl.textContent = "⏳ Bước 2/3: Đã lưu file an toàn. Đang mở rộng liên kết công khai...";

    const { data: urlData } = supabaseClient
      .storage
      .from('firmware')
      .getPublicUrl(uniqueFileName);

    const generatedPublicUrl = urlData.publicUrl;
    console.log("[MLOps Pipeline] Public URL cấp phát:", generatedPublicUrl);

    logEl.textContent = "⏳ Bước 3/3: Đang khóa tọa độ URL vào CSDL để kích hoạt vi điều khiển...";

    const { error: dbError } = await supabaseClient
      .from('device_configs') 
      .upsert([{ 
         device_id: "ESP32_LORA_GW", 
         ota_url: generatedPublicUrl,
         updated_at: new Date().toISOString()
      }]);

    if (dbError) throw dbError;

    logEl.innerHTML = `✅ <b>PHÁT LỆNH THÀNH CÔNG!</b><br>` +
                      `• File đã nằm an toàn trên Đám mây.<br>` +
                      `• Hệ thống đã phát sóng tín hiệu lệnh OTA ngầm.<br>` +
                      `<span style="font-size:0.65rem; color:var(--text-2);">Link truyền tải: ${generatedPublicUrl}</span>`;
    logEl.style.color = "var(--green)";
    
    fileInput.value = "";

  } catch (err) {
    console.error('[OTA Upload Pipeline Error]:', err);
    logEl.textContent = `❌ LỖI HỆ THỐNG: ${err.message}.`;
    logEl.style.color = "var(--red)";
  }
}

// -------------------------------------------------------------------
// 4. KHỞI TẠO HỆ THỐNG (BOOTSTRAPPING & REALTIME)
// -------------------------------------------------------------------
async function fetchLatest() {
  const { data, error } = await supabaseClient
    .from(CONFIG.API.TABLE)
    .select('*')
    .order('created_at', { ascending: false })
    .limit(1);
  if (error) { console.error('[App] fetch error:', error); return null; }
  return data?.[0] ?? null;
}

async function initDashboard() {
  // Lấy dữ liệu lần đầu khi vừa mở trang
  const latest = await fetchLatest();
  if (latest) updateUI(latest);

  // Đăng ký nghe WebSocket từ Supabase
  subscribeRealtime((row) => {
    console.log('[App] Realtime Triggered:', row);
    updateUI(row);
  });

  // VÒNG LẶP TIM MẠCH (WATCHDOG TIMER)
  // Quét ngầm mỗi 30 giây: Nếu không có ai gọi `updateUI` nữa (chết Node),
  // hàm này sẽ tự động phát hiện thời gian trôi qua và đổi cờ sang ĐỎ.
  setInterval(() => {
    if (prevData) {
        updateSensorStatusUI(prevData);
    }
  }, 30000);
}

// Bắt đầu chạy hệ thống khi cây DOM HTML tải xong
window.addEventListener('DOMContentLoaded', () => {
  initDashboard();
});
