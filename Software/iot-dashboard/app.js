/**
 * @file      app.js
 * @brief     Luồng điều khiển chính và đồng bộ giao diện Web Dashboard (Realtime)
 * @details   Kết nối cơ sở dữ liệu để cập nhật UI tức thời, quản lý watchdog tín hiệu và 
 * triển khai hệ thống nạp firmware từ xa (OTA Update Pipeline).
 */

let prevData = null;

/**
 * @brief Tính toán xu hướng thay đổi giữa gói tin hiện tại và gói tin trước đó
 * @param cur Giá trị hiện tại
 * @param prev Giá trị trước đó
 * @param unit Đơn vị đo đi kèm
 */
function calculateDelta(cur, prev, unit = '') {
  if (prev == null) return '— Ổn định';
  const diff = cur - prev;
  if (Math.abs(diff) < 0.05) return '→ Không biến động';
  return diff > 0
    ? `▲ Tăng +${Math.abs(diff).toFixed(1)}${unit}`
    : `▼ Giảm −${Math.abs(diff).toFixed(1)}${unit}`;
}

/**
 * @brief Cập nhật nhanh nội dung text của một thẻ HTML
 */
function setSubText(id, txt) {
  const el = document.getElementById(id);
  if (el) el.textContent = txt;
}

/* ═══════════════════════════════════════════════════════════════════
 * 1. GIÁM SÁT TÍN HIỆU HEARTBEAT (WATCHDOG MONITOR)
 * ═══════════════════════════════════════════════════════════════════ */
function updateGatewayStatusUI(latestData) {
  const statusText = document.getElementById('sensor-status-text');
  const heroInlineStatus = document.getElementById('hero-status-inline');
  if (!statusText || !heroInlineStatus) return;

  if (!latestData || !latestData.created_at) {
    statusText.innerHTML = "⚫ CHƯA CÓ DỮ LIỆU";
    statusText.style.color = "var(--text-2)";
    heroInlineStatus.innerHTML = "OFFLINE";
    return;
  }

  // Tính khoảng cách thời gian (phút) kể từ lần cuối nhận dữ liệu
  const diffMinutes = (Date.now() - new Date(latestData.created_at).getTime()) / 60000;

  if (diffMinutes > 5) {
    statusText.innerHTML = `🔴 MẤT KẾT NỐI TÍN HIỆU (${Math.round(diffMinutes)} phút trước)`;
    statusText.style.color = "var(--red)";
    heroInlineStatus.innerHTML = "DISCONNECTED";
    heroInlineStatus.style.background = "rgba(239,68,68,0.1)";
    heroInlineStatus.style.color = "var(--red)";
  } else {
    statusText.innerHTML = "🟢 HỆ THỐNG HOẠT ĐỘNG ỔN ĐỊNH";
    statusText.style.color = "var(--green)";
    heroInlineStatus.innerHTML = "ONLINE";
    heroInlineStatus.style.background = "rgba(16,185,129,0.1)";
    heroInlineStatus.style.color = "var(--green)";
  }
}

/* ═══════════════════════════════════════════════════════════════════
 * 2. CẬP NHẬT GIAO DIỆN (UI DATA RENDERER)
 * ═══════════════════════════════════════════════════════════════════ */
function updateUI(data) {
  if (!data) return;

  // Cập nhật các thông số cảm biến cơ bản
  const fields = ['temperature', 'humidity', 'pressure', 'predicted_temp_2h', 'device_id'];
  fields.forEach(f => {
    const el = document.getElementById(f);
    if (el) {
      if (typeof data[f] === 'number') {
        el.textContent = f === 'pressure' ? data[f].toFixed(0) : data[f].toFixed(1);
      } else {
        el.textContent = data[f] ?? '--';
      }
    }
  });

  // Cập nhật thông số PIN từ INA219
  const batEl = document.getElementById('battery');
  if (batEl) {
    batEl.textContent = data.battery != null ? parseFloat(data.battery).toFixed(2) : '--';
  }

  const pktEl = document.getElementById('packetCounter');
  if (pktEl) {
    pktEl.textContent = data.frame_counter != null ? `#${data.frame_counter}` : '--';
  }

  const devEl = document.getElementById('deviceId');
  if (devEl) {
    devEl.textContent = data.device_id ?? 'N/A';
  }

  // Quản lý trạng thái Pin yếu trực quan
  const batCard = document.getElementById('batCard');
  if (batCard && data.battery != null) {
    batCard.classList.toggle('low', data.battery < 3.6);
    setSubText('batSub', data.battery >= 4.0 ? '🟢 Đầy Nguồn' : data.battery >= 3.6 ? '🟡 Tốt' : '🔴 Cạn Nguồn - Cần Sạc');
  } else if (data.battery == null) {
    setSubText('batSub', '❌ Lỗi cảm biến INA219');
  }

  // Tính toán delta và kết xuất ra màn hình xu hướng thay đổi
  if (prevData) {
    setSubText('tempSub', calculateDelta(data.temperature, prevData.temperature, '°C'));
    setSubText('humSub',  calculateDelta(data.humidity,    prevData.humidity, '%'));
    setSubText('presSub', calculateDelta(data.pressure,    prevData.pressure, ' hPa'));
    
    // Xu hướng của mô hình AI dự báo
    const aiDiff = data.predicted_temp_2h - data.temperature;
    setSubText('predTempSub', `Dự báo nhiệt độ lệch đối lưu: ${aiDiff > 0 ? '+' : ''}${aiDiff.toFixed(1)}°C`);
  }

  updateGatewayStatusUI(data);
  prevData = data;
}

/* ═══════════════════════════════════════════════════════════════════
 * 3. HỆ THỐNG ĐẨY FIRMWARE QUA MẠNG (OTA PIPELINE)
 * ═══════════════════════════════════════════════════════════════════ */
async function handleOTAPipeline() {
  const fileInput = document.getElementById('firmwareFile');
  const logEl     = document.getElementById('otaLog');
  if (!fileInput || !logEl) return;

  const file = fileInput.files[0];
  if (!file) {
    logEl.textContent = "⚠ Vui lòng chọn file firmware dạng .bin trước!";
    logEl.style.color = "var(--orange)";
    return;
  }

  try {
    logEl.innerHTML = "⏳ Đang tạo gói tin bảo mật và tải lên Cloud Storage...";
    logEl.style.color = "var(--text-2)";

    const fileName = `firmware_${Date.now()}.bin`;
    const { data: uploadData, error: uploadError } = await supabaseClient
      .storage
      .from('ota-binaries')
      .upload(fileName, file);

    if (uploadError) throw uploadError;

    // Lấy Public URL của tệp nhị phân vừa tải lên
    const { data: { publicUrl } } = supabaseClient
      .storage
      .from('ota-binaries')
      .getPublicUrl(fileName);

    logEl.innerHTML = "⏳ Thực hiện cấu hình thiết bị ngầm (Upsert Config)...";

    // Ghi đè cấu hình lệnh OTA vào bảng device_configs để ESP32 kéo về
    const { error: dbError } = await supabaseClient
      .from('device_configs')
      .upsert([{ device_id: "ESP32_LORA_GW", ota_url: publicUrl, updated_at: new Date().toISOString() }]);

    if (dbError) throw dbError;

    logEl.innerHTML = `✅ <b>PHÁT LỆNH OTA THÀNH CÔNG!</b><br>` +
      `• File nhị phân đã nằm an toàn trên Đám mây.<br>` +
      `• Hệ thống đã phát tín hiệu hiệu năng cập nhật ngầm.<br>` +
      `<span style="font-size:0.62rem;color:var(--text-2);">Đường dẫn: ${publicUrl}</span>`;
    logEl.style.color = "var(--green)";
    fileInput.value   = "";

  } catch (err) {
    console.error('[OTA Pipeline Error]:', err);
    logEl.textContent = `❌ LỖI HỆ THỐNG PIPELINE: ${err.message}`;
    logEl.style.color = "var(--red)";
  }
}

/* ═══════════════════════════════════════════════════════════════════
 * 4. KHỞI CHẠY HỆ THỐNG VÀ ĐĂNG KÝ REALTIME
 * ═══════════════════════════════════════════════════════════════════ */
async function fetchLatestRecord() {
  const { data, error } = await supabaseClient
    .from(CONFIG.API.TABLE)
    .select('*')
    .order('created_at', { ascending: false })
    .limit(1);
  if (error) { console.error('[App] Lỗi nạp bản ghi gốc:', error); return null; }
  return data?.[0] ?? null;
}

async function initDashboard() {
  // Lấy cấu hình đưa lên Modal hiển thị
  document.getElementById('cfgMode').textContent  = CONFIG.MODE;
  document.getElementById('cfgTable').textContent = CONFIG.API.TABLE;
  document.getElementById('cfgUrl').textContent   = CONFIG.SUPABASE_URL;
  document.getElementById('cfgLimit').textContent = CONFIG.FETCH_LIMIT;
  document.getElementById('cfgMode').className   = "badge " + CONFIG.MODE.toLowerCase();

  // Đọc dữ liệu mới nhất để render giao diện ngay lập tức khi load trang
  const latest = await fetchLatestRecord();
  if (latest) updateUI(latest);

  // Đăng ký nhận sự kiện WebSocket từ Supabase Realtime Engine
  subscribeRealtime((newRow) => {
    console.log('[Realtime RX]:', newRow);
    updateUI(newRow);
  });

  // Watchdog ngầm kiểm tra mất kết nối định kỳ mỗi 15 giây
  setInterval(() => {
    if (prevData) updateGatewayStatusUI(prevData);
  }, 15000);
}

document.addEventListener('DOMContentLoaded', initDashboard);
