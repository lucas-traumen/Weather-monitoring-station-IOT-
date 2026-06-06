/**
 * @file      config.js
 * @version   WEB_VERSION 4.1
 * @brief     IoTSenseHub Web config — Edge Function plaintext DB pipeline
 *
 * Pipeline:
 *   ESP32 GW_VERSION 3.7.x -> Supabase Edge Function -> public.weather_logs plaintext
 *   Web Dashboard reads plaintext rows from DB and subscribes to Supabase Realtime.
 *
 * IMPORTANT:
 *   Do NOT put ASCON key1/key2 or service_role key in browser JavaScript.
 */

const WEB_VERSION = "4.1";

const CONFIG = {
  SUPABASE_URL: "https://hbuluhjjfivezrrxesaz.supabase.co",

  // Public anon key only. This is allowed in browser when RLS SELECT policy is configured.
  SUPABASE_KEY:
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImhidWx1aGpqZml2ZXpycnhlc2F6Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3Nzg1MDE4MDksImV4cCI6MjA5NDA3NzgwOX0.KhjB0T-8Yy34P3p37XipEutwVfraabsG274NL_88J4Q",

  MODE: "REAL", // DEMO | REAL
  DEVICE_ID: "ESP32_LORA_GW",
  FETCH_LIMIT: 50,

  PIPELINE: {
    GATEWAY_VERSION: "3.7.x",
    EDGE_FUNCTION_VERSION: "1.3",
    WEB_VERSION: WEB_VERSION,
    INGEST: "ESP32 -> Edge Function -> plaintext DB",
    REALTIME: "Supabase Realtime postgres_changes"
  },

  API: {
    TABLE: "weather_logs",
    CONFIG_TABLE: "device_configs",
    SELECT_COLUMNS:
      "id,created_at,frame_counter,device_id,temperature,humidity,pressure,board_temp,battery,predicted_temp_2h,health_flag,encrypted_payload"
  },

  STORAGE: {
    // Match the Storage RLS policies you created earlier.
    FIRMWARE_BUCKET: "firmware"
  }
};
