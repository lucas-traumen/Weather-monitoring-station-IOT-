// app.js — IoTSenseHub Dashboard
// Chỉ đọc từ Supabase. Simulator chạy độc lập ở run_sim.html.

let prevData = null;

function delta(cur, prev, unit = '') {
  if (prev == null) return '—';
  const d = cur - prev;
  if (Math.abs(d) < 0.05) return '→ stable';
  return d > 0
    ? `▲ +${Math.abs(d).toFixed(1)}${unit} from last`
    : `▼ −${Math.abs(d).toFixed(1)}${unit} from last`;
}

function setSub(id, txt) {
  const el = document.getElementById(id);
  if (el) el.textContent = txt;
}

function updateUI(data) {
  if (!data) return;

  const temp = parseFloat(data.temperature) || 0;
  const hum  = parseFloat(data.humidity)    || 0;
  const pres = parseFloat(data.pressure)    || 0;
  const bat  = parseFloat(data.battery)     || 0;

  // Cập nhật giá trị — giữ lại <span class="unit"> bên trong
  function setVal(id, num, dec) {
    const el = document.getElementById(id);
    if (!el) return;
    const unitEl = el.querySelector('.unit');
    const unitHTML = unitEl ? unitEl.outerHTML : '';
    el.innerHTML = num.toFixed(dec) + unitHTML;
  }

  setVal('temperature', temp, 1);
  setVal('humidity',    hum,  1);
  setVal('pressure',    pres, 1);
  setVal('battery',     bat,  2);

  const trendEl = document.getElementById('trend');
  if (trendEl) trendEl.textContent = data.pressure_trend || '--';

  const warnEl = document.getElementById('warning');
  if (warnEl) warnEl.textContent = data.storm_warning ? '⚠ Storm Warning' : '✓ Normal';

  const updEl = document.getElementById('updated');
  if (updEl) updEl.textContent = new Date(data.created_at).toLocaleString('vi-VN');

  // Sub hints
  setSub('tempSub',   delta(temp, prevData?.temperature, '°C'));
  setSub('humSub',    delta(hum,  prevData?.humidity,    '%'));
  setSub('presSub',   `${pres < 1000 ? 'Low pressure' : pres > 1020 ? 'High pressure' : 'Normal'} · ${delta(pres, prevData?.pressure, ' hPa')}`);
  setSub('trendSub',  pres < 1000
    ? 'Possible rain incoming'
    : pres > 1020 ? 'Clear skies likely' : 'Steady conditions');
  setSub('statusSub', `Device: ${data.device_id || 'unknown'}`);

  // Chips
  const devEl = document.getElementById('deviceChip');
  if (devEl) devEl.textContent = data.device_id || 'unknown';

  const luEl = document.getElementById('lastUpdateChip');
  if (luEl) luEl.textContent = new Date(data.created_at)
    .toLocaleTimeString('vi-VN', { hour12: false });

  prevData = data;
}

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
  // Initial fetch
  const latest = await fetchLatest();
  if (latest) updateUI(latest);

  // Live updates
  subscribeRealtime((row) => {
    console.log('[App] Realtime:', row);
    updateUI(row);
  });
}

window.addEventListener('DOMContentLoaded', () => {
  console.log('[App] mode =', CONFIG.MODE);
  initDashboard();
});
