/**
 * @file      config.js
 * @version   WEB_VERSION 4.4
 * @brief     IoTSenseHub Web config — Edge Function plaintext DB pipeline
 *
 * Pipeline:
 *   ESP32 GW_VERSION 3.8.1-baseline-mlr-tzfix -> Supabase Edge Function -> public.weather_logs plaintext
 *   Web Dashboard reads plaintext rows from DB and subscribes to Supabase Realtime.
 *
 * IMPORTANT:
 *   Do NOT put ASCON key1/key2 or service_role key in browser JavaScript.
 */

const WEB_VERSION = "4.9";

const CONFIG = {
  SUPABASE_URL: "https://hbuluhjjfivezrrxesaz.supabase.co",

  // Public anon key only. This is allowed in browser when RLS SELECT policy is configured.
  SUPABASE_KEY:
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImhidWx1aGpqZml2ZXpycnhlc2F6Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3Nzg1MDE4MDksImV4cCI6MjA5NDA3NzgwOX0.KhjB0T-8Yy34P3p37XipEutwVfraabsG274NL_88J4Q",

  MODE: "REAL", // DEMO | REAL
  DEVICE_ID: "ESP32_LORA_GW",
  FETCH_LIMIT: 50,

  PIPELINE: {
    GATEWAY_VERSION: "3.8.1-baseline-mlr-tzfix",
    EDGE_FUNCTION_VERSION: "1.3",
    WEB_VERSION: WEB_VERSION,
    INGEST: "ESP32 -> Edge Function -> plaintext DB",
    REALTIME: "Supabase Realtime postgres_changes",
    FORECAST_DISPLAY: "predicted_temp_2h is rendered at created_at + 2h"
  },

  API: {
    TABLE: "weather_logs",
    CONFIG_TABLE: "device_configs",
    GATEWAY_STATUS_TABLE: "gateway_status",
    GATEWAY_STATUS_SELECT_COLUMNS:
        "device_id,gateway_last_seen_at,last_lora_packet_at,last_frame_counter,mqtt_enabled,mqtt_connected,mqtt_last_change_at,mqtt_broker,wifi_rssi,firmware_version,firmware_sha256,last_applied_ota_sha256,ip_address,uptime_sec,free_heap,status_note",
    SELECT_COLUMNS:
        "id,created_at,created_at_vn,forecast_for_at,forecast_for_at_vn,frame_counter,device_id,temperature,humidity,pressure,board_temp,battery,predicted_temp_2h,health_flag,encrypted_payload"
  },

  STORAGE: {
    // Match the Storage RLS policies you created earlier.
    FIRMWARE_BUCKET: "firmware"
  }
};
