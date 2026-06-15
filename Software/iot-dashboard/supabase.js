/**
 * @file      supabase.js
 * @version   WEB_VERSION 4.7
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


function normalizeGatewayStatusRow(row) {
  if (!row) return null;

  const numOrNull = (v) => {
    if (v === null || v === undefined || v === "") return null;
    const n = Number(v);
    return Number.isFinite(n) ? n : null;
  };

  const boolOrNull = (v) => {
    if (v === null || v === undefined || v === "") return null;
    if (typeof v === "boolean") return v;
    if (v === "true" || v === 1 || v === "1") return true;
    if (v === "false" || v === 0 || v === "0") return false;
    return null;
  };

  return {
    device_id: row.device_id ?? CONFIG.DEVICE_ID,
    gateway_last_seen_at: row.gateway_last_seen_at ?? null,
    last_lora_packet_at: row.last_lora_packet_at ?? null,
    last_frame_counter: row.last_frame_counter != null ? Number(row.last_frame_counter) : null,
    mqtt_enabled: boolOrNull(row.mqtt_enabled),
    mqtt_connected: boolOrNull(row.mqtt_connected),
    mqtt_last_change_at: row.mqtt_last_change_at ?? null,
    mqtt_broker: row.mqtt_broker ?? null,
    wifi_rssi: numOrNull(row.wifi_rssi),
    firmware_version: row.firmware_version ?? null,
    firmware_sha256: row.firmware_sha256 ?? null,
    last_applied_ota_sha256: row.last_applied_ota_sha256 ?? null,
    ip_address: row.ip_address ?? null,
    uptime_sec: numOrNull(row.uptime_sec),
    free_heap: numOrNull(row.free_heap),
    status_note: row.status_note ?? null
  };
}

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
    created_at_vn: row.created_at_vn ?? null,
    forecast_for_at: row.forecast_for_at ?? null,
    forecast_for_at_vn: row.forecast_for_at_vn ?? null,
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


async function fetchLatestGatewayStatus() {
  const table = CONFIG.API.GATEWAY_STATUS_TABLE || "gateway_status";
  const selectCols = CONFIG.API.GATEWAY_STATUS_SELECT_COLUMNS || "*";

  const { data, error } = await supabaseClient
    .from(table)
    .select(selectCols)
    .eq("device_id", CONFIG.DEVICE_ID)
    .limit(1);

  if (error) throw error;
  return normalizeGatewayStatusRow(data?.[0] ?? null);
}

function subscribeGatewayStatus(callback, statusCallback = null) {
  const table = CONFIG.API.GATEWAY_STATUS_TABLE || "gateway_status";

  const channel = supabaseClient
    .channel("gateway-status-realtime-v4-7")
    .on(
      "postgres_changes",
      {
        event: "*",
        schema: "public",
        table,
        filter: `device_id=eq.${CONFIG.DEVICE_ID}`
      },
      (payload) => {
        if (payload.eventType === "DELETE") return;
        const row = normalizeGatewayStatusRow(payload.new);
        if (row) callback(row, payload);
      }
    )
    .subscribe((status) => {
      console.log("[Gateway status realtime]", status);
      if (typeof statusCallback === "function") statusCallback(status);
    });

  return channel;
}

function subscribeRealtime(callback, statusCallback = null) {
  const channel = supabaseClient
    .channel("weather-realtime-v4-3")
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
