/**
 * @file      chart-render.js
 * @version   WEB_VERSION 4.3
 * @brief     Analytics charts for plaintext weather_logs.
 *
 * Important display rule:
 *   - temperature is plotted at created_at.
 *   - predicted_temp_2h is plotted at created_at + 2 hours.
 *
 * This prevents users from reading a +2h forecast as if it were the
 * measured temperature at the same timestamp.
 */

const CCOL = {
  temp:     { line: "#ff6b35", fill: "rgba(255,107,53,0.04)"  },
  predict:  { line: "#818cf8", fill: "transparent"            },
  humidity: { line: "#38bdf8", fill: "rgba(56,189,248,0.04)"  },
  pressure: { line: "#c084fc", fill: "rgba(192,132,252,0.04)" },
  battery:  { line: "#34d399", fill: "rgba(52,211,153,0.04)"  },
};

const WEB_CHART_RENDER_VERSION = "4.3";
const FORECAST_HOURS_AHEAD = 2;

const charts = { temp: null, humidity: null, pressure: null, battery: null };
let rowsCache = [];

function applyTheme() {
  if (typeof Chart === "undefined") return;
  Chart.defaults.color = "#526685";
  Chart.defaults.borderColor = "#162235";
  Chart.defaults.font.family = "'Figtree', sans-serif";
  Chart.defaults.font.size = 11;
  Chart.defaults.animation.duration = 200;
}

function dsStyle(key, labelName, dataKey) {
  const c = CCOL[key];
  return {
    label: labelName,
    dataKey,
    borderColor: c.line,
    backgroundColor: c.fill,
    borderWidth: 1.8,
    pointRadius: 1.5,
    pointHoverRadius: 4,
    tension: 0.35,
    fill: key !== "predict",
    spanGaps: true
  };
}

function dateFromCreatedAt(createdAt) {
  return createdAt ? new Date(createdAt) : null;
}

function addHours(dateObj, hours) {
  return new Date(dateObj.getTime() + hours * 60 * 60 * 1000);
}

function timeLabelFromDate(dateObj) {
  if (!dateObj) return "--:--";
  return dateObj.toLocaleTimeString("vi-VN", {
    hour: "2-digit",
    minute: "2-digit"
  });
}

function timeLabel(row) {
  return row.created_at
    ? timeLabelFromDate(new Date(row.created_at))
    : "--:--";
}

/**
 * Build a single timeline for the temperature chart.
 *
 * Example:
 *   Row at 00:10:
 *     actual temperature is placed at 00:10.
 *     predicted_temp_2h is placed at 02:10.
 */
function buildTemperatureForecastTimeline(rows) {
  const pointMap = new Map();

  for (const row of rows) {
    if (!row || !row.created_at) continue;

    const actualTime = dateFromCreatedAt(row.created_at);
    const actualKey = actualTime.toISOString();

    if (!pointMap.has(actualKey)) {
      pointMap.set(actualKey, {
        timeKey: actualKey,
        label: timeLabelFromDate(actualTime),
        actual: null,
        predicted: null
      });
    }

    pointMap.get(actualKey).actual = row.temperature ?? null;

    if (row.predicted_temp_2h !== null && row.predicted_temp_2h !== undefined) {
      const forecastTime = addHours(actualTime, FORECAST_HOURS_AHEAD);
      const forecastKey = forecastTime.toISOString();

      if (!pointMap.has(forecastKey)) {
        pointMap.set(forecastKey, {
          timeKey: forecastKey,
          label: timeLabelFromDate(forecastTime),
          actual: null,
          predicted: null
        });
      }

      pointMap.get(forecastKey).predicted = row.predicted_temp_2h;
    }
  }

  return Array.from(pointMap.values())
    .sort((a, b) => new Date(a.timeKey) - new Date(b.timeKey));
}

function makeTemperatureForecastChart(el, rows) {
  const timeline = buildTemperatureForecastTimeline(rows);

  return new Chart(el, {
    type: "line",
    data: {
      labels: timeline.map(p => p.label),
      datasets: [
        {
          label: "Nhiệt độ thực tế tại thời điểm đo (°C)",
          data: timeline.map(p => p.actual),
          borderColor: CCOL.temp.line,
          backgroundColor: CCOL.temp.fill,
          borderWidth: 1.8,
          pointRadius: 1.5,
          pointHoverRadius: 4,
          tension: 0.35,
          fill: true,
          spanGaps: true
        },
        {
          label: "Dự báo MLR cho thời điểm +2h (°C)",
          data: timeline.map(p => p.predicted),
          borderColor: CCOL.predict.line,
          backgroundColor: CCOL.predict.fill,
          borderWidth: 1.8,
          pointRadius: 1.5,
          pointHoverRadius: 4,
          tension: 0.35,
          fill: false,
          spanGaps: true
        }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: {
        intersect: false,
        mode: "index"
      },
      plugins: {
        legend: { display: true, position: "top", labels: { boxWidth: 12 } },
        tooltip: {
          callbacks: {
            afterLabel: function(context) {
              if (context.datasetIndex === 1) {
                return "Forecast point = thời điểm đo + 2 giờ";
              }
              return "Actual point = thời điểm đo thật";
            }
          }
        }
      },
      scales: {
        x: { grid: { display: false } },
        y: { grid: { color: "#131e30" }, ticks: { precision: 1 } }
      }
    }
  });
}

function makeChart(canvasId, typeKey, rows) {
  const el = document.getElementById(canvasId);
  if (!el) return null;

  if (typeKey === "temp") {
    return makeTemperatureForecastChart(el, rows);
  }

  const labels = rows.map(timeLabel);
  let datasets = [];

  if (typeKey === "humidity") {
    datasets = [dsStyle("humidity", "Độ ẩm thực tế (%)", "humidity")];
  } else if (typeKey === "pressure") {
    datasets = [dsStyle("pressure", "Áp suất khí quyển (hPa)", "pressure")];
  } else if (typeKey === "battery") {
    datasets = [dsStyle("battery", "Điện áp PIN nguồn (V)", "battery")];
  }

  datasets.forEach(ds => {
    ds.data = rows.map(r => r[ds.dataKey] ?? null);
  });

  return new Chart(el, {
    type: "line",
    data: { labels, datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: {
        intersect: false,
        mode: "index"
      },
      plugins: {
        legend: { display: true, position: "top", labels: { boxWidth: 12 } }
      },
      scales: {
        x: { grid: { display: false } },
        y: { grid: { color: "#131e30" }, ticks: { precision: 1 } }
      }
    }
  });
}

function average(rows, key) {
  const vals = rows.map(r => r[key]).filter(v => v != null && Number.isFinite(Number(v)));
  if (!vals.length) return null;
  return vals.reduce((a, b) => a + Number(b), 0) / vals.length;
}

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text;
}

function setFixed(id, val, dec, suffix = "") {
  const el = document.getElementById(id);
  if (!el) return;
  el.textContent = val == null ? "--" : `${Number(val).toFixed(dec)}${suffix}`;
}

function updateStats(rows) {
  if (!rows.length) {
    setText("pointCount", "0");
    setText("shownCount", "0");
    return;
  }

  const last = rows[rows.length - 1];

  setFixed("liveTemp", last.temperature, 1);
  setFixed("liveHum",  last.humidity, 1);
  setFixed("livePres", last.pressure, 0);
  setFixed("liveBat",  last.battery, 2);

  setFixed("avgTemp", average(rows, "temperature"), 1, " °C");
  setFixed("avgHum",  average(rows, "humidity"), 1, " %");
  setFixed("avgPres", average(rows, "pressure"), 0, " hPa");

  setText("pointCount", rows.length);
  setText("shownCount", rows.length);
}

async function updateTotal() {
  try {
    const count = await countWeatherLogs();
    setText("totalCount", count);
  } catch (err) {
    console.warn("[Charts] Không thể đếm tổng records:", err);
    setText("totalCount", "--");
  }
}

function redrawAllCharts() {
  Object.values(charts).forEach(chart => chart?.destroy());

  charts.temp     = makeChart("tempChart",     "temp",     rowsCache);
  charts.humidity = makeChart("humidityChart", "humidity", rowsCache);
  charts.pressure = makeChart("pressureChart", "pressure", rowsCache);
  charts.battery  = makeChart("batteryChart",  "battery",  rowsCache);

  updateStats(rowsCache);
}

function insertOrReplaceRow(row) {
  const key = row.frame_counter;
  const idx = key != null ? rowsCache.findIndex(r => r.frame_counter === key) : -1;

  if (idx >= 0) rowsCache[idx] = row;
  else rowsCache.push(row);

  rowsCache.sort((a, b) => new Date(a.created_at) - new Date(b.created_at));

  while (rowsCache.length > CONFIG.FETCH_LIMIT) rowsCache.shift();

  redrawAllCharts();
}

async function initCharts() {
  applyTheme();

  try {
    rowsCache = await fetchWeatherHistory(CONFIG.FETCH_LIMIT);
  } catch (err) {
    console.error("[Charts] Không thể tải lịch sử:", err);
    rowsCache = [];
  }

  redrawAllCharts();
  await updateTotal();

  subscribeRealtime(async (row) => {
    console.log("[Charts Realtime RX]", row);
    insertOrReplaceRow(row);
    await updateTotal();
  });
}

document.addEventListener("DOMContentLoaded", initCharts);
