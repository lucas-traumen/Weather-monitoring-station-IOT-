// chart-render.js — IoTSenseHub Analytics
// 4 biểu đồ: Temperature, Humidity, Pressure, Battery
// Realtime update qua subscribeRealtime — không dùng setInterval

const CCOL = {
  temp:     { line: '#ff6b35', fill: 'rgba(255,107,53,0.07)'  },
  humidity: { line: '#38bdf8', fill: 'rgba(56,189,248,0.07)'  },
  pressure: { line: '#c084fc', fill: 'rgba(192,132,252,0.07)' },
  battery:  { line: '#34d399', fill: 'rgba(52,211,153,0.07)'  },
};

function applyTheme() {
  if (typeof Chart === 'undefined') return;
  Chart.defaults.color              = '#3b5070';
  Chart.defaults.borderColor        = '#1e2d47';
  Chart.defaults.font.family        = "'JetBrains Mono', monospace";
  Chart.defaults.font.size          = 11;
  Chart.defaults.animation.duration = 250;
}

function dsStyle(key) {
  const c = CCOL[key];
  return {
    borderColor:          c.line,
    backgroundColor:      c.fill,
    borderWidth:          1.8,
    fill:                 true,
    tension:              0.35,
    pointRadius:          2,
    pointHoverRadius:     5,
    pointBackgroundColor: c.line,
    pointBorderColor:     'transparent',
  };
}

function scaleOpts() {
  return {
    grid:   { color: 'rgba(30,45,71,0.7)', drawBorder: false },
    ticks:  { color: '#3b5070', maxTicksLimit: 6, maxRotation: 0 },
    border: { display: false },
  };
}

let charts = {};

async function loadHistory() {
  const { data, error } = await supabaseClient
    .from(CONFIG.API.TABLE)
    .select('*')
    .order('created_at', { ascending: true })
    .limit(CONFIG.FETCH_LIMIT || 50);
  if (error) { console.error('[Chart]', error); return []; }
  return data;
}

function makeChart(canvasId, label, rows, field, colorKey) {
  const ctx = document.getElementById(canvasId);
  if (!ctx) return null;
  return new Chart(ctx, {
    type: 'line',
    data: {
      labels:   rows.map(r => new Date(r.created_at).toLocaleTimeString('vi-VN', { hour12: false })),
      datasets: [{ label, data: rows.map(r => r[field]), ...dsStyle(colorKey) }],
    },
    options: {
      responsive: true, maintainAspectRatio: false,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: { display: false },
        tooltip: {
          backgroundColor: '#111827',
          borderColor:      CCOL[colorKey].line,
          borderWidth:      1,
          titleColor:       '#f8faff',
          bodyColor:        CCOL[colorKey].line,
          padding:          10,
          displayColors:    false,
        },
      },
      scales: { x: scaleOpts(), y: scaleOpts() },
    },
  });
}

function pushPoint(chart, liveId, value) {
  if (!chart) return;
  chart.data.labels.push(new Date().toLocaleTimeString('vi-VN', { hour12: false }));
  chart.data.datasets[0].data.push(value);
  const MAX = CONFIG.FETCH_LIMIT || 50;
  if (chart.data.labels.length > MAX) {
    chart.data.labels.shift();
    chart.data.datasets[0].data.shift();
  }
  chart.update('active');
  const el = document.getElementById(liveId);
  if (el) el.textContent = typeof value === 'number' ? value.toFixed(2) : '--';
}

function avg(rows, field) {
  const vals = rows.map(r => parseFloat(r[field])).filter(v => !isNaN(v));
  return vals.length ? (vals.reduce((a, b) => a + b, 0) / vals.length).toFixed(1) : '--';
}

function updateStats(rows) {
  const set = (id, v) => { const el = document.getElementById(id); if (el) el.textContent = v; };
  set('avgTemp',   avg(rows, 'temperature') + ' °C');
  set('avgHum',    avg(rows, 'humidity')    + ' %');
  set('avgPres',   avg(rows, 'pressure')    + ' hPa');
  set('pointCount', rows.length);
  set('shownCount', rows.length);
}

async function updateTotal() {
  try {
    const { count } = await supabaseClient
      .from(CONFIG.API.TABLE)
      .select('*', { count: 'exact', head: true });
    const el = document.getElementById('totalCount');
    if (el) el.textContent = count ?? '--';
  } catch {}
}

async function initCharts() {
  applyTheme();
  const rows = await loadHistory();

  charts.temp     = makeChart('tempChart',     'Temperature', rows, 'temperature', 'temp');
  charts.humidity = makeChart('humidityChart', 'Humidity',    rows, 'humidity',    'humidity');
  charts.pressure = makeChart('pressureChart', 'Pressure',    rows, 'pressure',    'pressure');
  charts.battery  = makeChart('batteryChart',  'Battery',     rows, 'battery',     'battery');

  updateStats(rows);
  updateTotal();

  // Set initial live values
  if (rows.length) {
    const last = rows[rows.length - 1];
    const set = (id, v, d) => { const el = document.getElementById(id); if (el) el.textContent = parseFloat(v)?.toFixed(d) ?? '--'; };
    set('liveTemp', last.temperature, 1);
    set('liveHum',  last.humidity,    1);
    set('livePres', last.pressure,    1);
    set('liveBat',  last.battery,     2);
  }

  // Realtime updates
  subscribeRealtime((row) => {
    pushPoint(charts.temp,     'liveTemp', parseFloat(row.temperature));
    pushPoint(charts.humidity, 'liveHum',  parseFloat(row.humidity));
    pushPoint(charts.pressure, 'livePres', parseFloat(row.pressure));
    pushPoint(charts.battery,  'liveBat',  parseFloat(row.battery));
    updateTotal();
  });
}

window.addEventListener('DOMContentLoaded', initCharts);
