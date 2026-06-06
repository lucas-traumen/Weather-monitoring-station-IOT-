/**
 * @file      supabase.js
 * @version   WEB_VERSION 4.1
 * @brief     Supabase client helpers for plaintext weather_logs + Realtime
 */

const supabaseClient = supabase.createClient(
  CONFIG.SUPABASE_URL,
  CONFIG.SUPABASE_KEY,
  {
    realtime: {
      params: {
        eventsPerSecond: 10
      }
    }
  }
);

function normalizeWeatherRow(row) {
  if (!row) return null;

  const numOrNull = (v) => {
    if (v === null || v === undefined || v === "") return null;
    const n = Number(v);
    return Number.isFinite(n) ? n : null;
  };

  return {
    id: row.id ?? null,
    created_at: row.created_at ?? null,
    frame_counter: row.frame_counter != null ? Number(row.frame_counter) : null,
    device_id: row.device_id ?? CONFIG.DEVICE_ID,

    temperature: numOrNull(row.temperature),
    humidity: numOrNull(row.humidity),
    pressure: numOrNull(row.pressure),
    board_temp: numOrNull(row.board_temp),
    battery: numOrNull(row.battery),
    predicted_temp_2h: numOrNull(row.predicted_temp_2h),

    health_flag: row.health_flag != null ? Number(row.health_flag) : null,
    encrypted_payload: row.encrypted_payload ?? null
  };
}

async function fetchLatestWeather() {
  const { data, error } = await supabaseClient
    .from(CONFIG.API.TABLE)
    .select(CONFIG.API.SELECT_COLUMNS)
    .order("created_at", { ascending: false })
    .limit(1);

  if (error) throw error;
  return normalizeWeatherRow(data?.[0] ?? null);
}

async function fetchWeatherHistory(limit = CONFIG.FETCH_LIMIT) {
  const { data, error } = await supabaseClient
    .from(CONFIG.API.TABLE)
    .select(CONFIG.API.SELECT_COLUMNS)
    .order("created_at", { ascending: false })
    .limit(limit);

  if (error) throw error;
  return (data ?? []).map(normalizeWeatherRow).filter(Boolean).reverse();
}

async function countWeatherLogs() {
  const { count, error } = await supabaseClient
    .from(CONFIG.API.TABLE)
    .select("*", { count: "exact", head: true });

  if (error) throw error;
  return count ?? 0;
}

function subscribeRealtime(callback, statusCallback = null) {
  const channel = supabaseClient
    .channel("weather-realtime-v4-1")
    .on(
      "postgres_changes",
      {
        event: "*",
        schema: "public",
        table: CONFIG.API.TABLE
      },
      (payload) => {
        if (payload.eventType === "DELETE") return;
        const row = normalizeWeatherRow(payload.new);
        if (row) callback(row, payload);
      }
    )
    .subscribe((status) => {
      console.log("[Realtime status]", status);
      if (typeof statusCallback === "function") statusCallback(status);
    });

  return channel;
}
