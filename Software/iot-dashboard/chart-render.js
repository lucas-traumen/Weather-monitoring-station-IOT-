// chart-render.js — IoTSenseHub Analytics Core Engine
// Tích hợp 4 biểu đồ phần cứng chuẩn kèm vẽ đè đường dự đoán MLR thực tế

const CCOL = {
  temp:     { line: '#ff6b35', fill: 'rgba(255,107,53,0.04)'  },
  predict:  { line: '#818cf8', fill: 'transparent'            }, // Màu sắc của đường AI MLR
  humidity: { line: '#38bdf8', fill: 'rgba(56,189,248,0.04)'  },
  pressure: { line: '#c084fc', fill: 'rgba(192,132,252,0.04)' },
  battery:  { line: '#34d399', fill: 'rgba(52,211,153,0.04)'  },
};

function applyTheme() {
  if (typeof Chart === 'undefined') return;
  Chart.defaults.color              = '#526685';
  Chart.defaults.borderColor        = '#162235';
  Chart.defaults.font.family        = "'Figtree', sans-serif";
  Chart.defaults.font.size          = 11;
  Chart.defaults.animation.duration = 200;
}

function dsStyle(key, labelName, dataKey) {
  const c = CCOL[key];
  return {
    label:                labelName,
    dataKey:              dataKey, 
    borderColor:          c.line,
    backgroundColor:      c.fill,
    borderWidth:          1.8,
    pointRadius:          1.5,
    pointHoverRadius:     4,
    tension:              0.35,
    fill:                 key !== 'predict' 
  };
}

function makeChart(canvasId, typeKey, historicalRows) {
  const el = document.getElementById(canvasId);
  if (!el) return null;

  const labels = historicalRows.map(r => new Date(r.created_at).toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit' }));
  let datasets = [];

  // Logic đặc biệt cho đồ thị Nhiệt độ: Đổ 2 đường song song để so sánh RMSE
  if (typeKey === 'temp') {
    datasets = [
      dsStyle('temp', 'Nhiệt độ đo thực tế (°C)', 'temperature'),
      dsStyle('predict', 'Dự báo trước 2 giờ (MLR)', 'predicted_temp_2h')
    ];
  } else if (typeKey === 'humidity') {
    datasets = [dsStyle('humidity', 'Độ ẩm thực tế (%)', 'humidity')];
  } else if (typeKey === 'pressure') {
    datasets = [dsStyle('pressure', 'Áp suất khí quyển (hPa)', 'pressure')];
  } else if (typeKey === 'battery') {
    datasets = [dsStyle('battery', 'Điện áp PIN nguồn (V)', 'battery')];
  }

  // Áp dụng mảng giá trị vào data của Chart.js
  datasets.forEach(ds => {
    ds.data = historicalRows.map(r => r[ds.dataKey]);
  });

  return new Chart(el, {
    type: 'line',
    data: { labels, datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      plugins: { legend: { display: true, position: 'top', labels: { boxWidth: 12 } } },
      scales: {
        x: { grid: { display: false } },
        y: { grid: { color: '#131e30' }, ticks: { precision: 1 } }
      }
    }
  });
}

async function loadHistory() {
  try {
    const { data, error } = await supabaseClient
      .from(CONFIG.API.TABLE)
      .select('*')
      .order('created_at', { ascending: false })
      .limit(CONFIG.FETCH_LIMIT);

    if (error) throw error;
    return data ? data.reverse() : []; // Đảo mảng để hiển thị từ cũ đến mới trên đồ thị
  } catch (err) {
    console.error('[Charts] Không thể tải dữ liệu lịch sử:', err);
    return [];
  }
}

function updateStats(rows) {
  if (!rows.length) return;
  const last = rows[rows.length - 1];
  
  const set = (id, val, dec) => {
    const el = document.getElementById(id);
    if (el && val != null) el.textContent = parseFloat(val).toFixed(dec);
  };

  set('liveTemp', last.temperature, 1);
  set('liveHum',  last.humidity,    1);
  set('livePres', last.pressure,    0);
  set('liveBat',  last.battery,     2);
  
  const countEl = document.getElementById('shownCount');
  if (countEl) countEl.textContent = rows.length;
}

async function updateTotal() {
  try {
    const { count, error } = await supabaseClient
      .from(CONFIG.API.TABLE)
      .select('*', { count: 'exact', head: true });
    const el = document.getElementById('totalCount');
    if (el) el.textContent = count ?? '--';
  } catch {}
}

async function initCharts() {
  applyTheme();
  const rows = await loadHistory();

  // Khởi tạo đồ thị
  charts.temp     = makeChart('tempChart',     'temp',     rows);
  charts.humidity = makeChart('humidityChart', 'humidity', rows);
  charts.pressure = makeChart('pressureChart', 'pressure', rows);
  charts.battery  = makeChart('batteryChart',  'battery',  rows);

  updateStats(rows);
  updateTotal();

  // Lắng nghe sự kiện Realtime cập nhật gia tốc điểm đồ thị mới
  subscribeRealtime((row) => {
    const timeStr = new Date(row.created_at).toLocaleTimeString('vi-VN', { hour: '2-digit', minute: '2-digit' });
    
    Object.keys(charts).forEach(key => {
      const chart = charts[key];
      if (!chart) return;

      // Đẩy mốc thời gian mới
      chart.data.labels.push(timeStr);
      if (chart.data.labels.length > CONFIG.FETCH_LIMIT) chart.data.labels.shift();

      // Đẩy giá trị mới vào từng trục dữ liệu tương ứng
      chart.data.datasets.forEach(ds => {
        const val = row[ds.dataKey];
        ds.data.push(val);
        if (ds.data.length > CONFIG.FETCH_LIMIT) ds.data.shift();
      });

      chart.update('none'); // Cập nhật mượt, tắt hiệu ứng animation nặng khi dồn điểm
    });

    // Cập nhật lại chỉ số tĩnh bên trên
    const currentRows = charts.temp ? charts.temp.data.labels.length : CONFIG.FETCH_LIMIT;
    const countEl = document.getElementById('shownCount');
    if (countEl) countEl.textContent = currentRows;

    // Cập nhật giá trị hiển thị thời gian thực nhanh
    const quickSet = (id, v, d) => { const el = document.getElementById(id); if (el && v != null) el.textContent = parseFloat(v).toFixed(d); };
    quickSet('liveTemp', row.temperature, 1);
    quickSet('liveHum',  row.humidity,    1);
    quickSet('livePres', row.pressure,    0);
    quickSet('liveBat',  row.battery,     2);
  });
}

const charts = { temp: null, humidity: null, pressure: null, battery: null };
document.addEventListener('DOMContentLoaded', initCharts);
