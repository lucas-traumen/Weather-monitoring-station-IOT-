# WEB_VERSION 4.1 — Plaintext DB synchronized with Edge Function

## Flow
ESP32 GW v3.7.x -> Supabase Edge Function v1.3 -> public.weather_logs plaintext -> Web Dashboard v4.1.

## Files
Replace your current web files with these:
- index.html
- chart.html
- config.js
- supabase.js
- app.js
- chart-render.js
- style.css

## Important changes
- Browser no longer decrypts ASCON.
- Web reads plaintext columns from public.weather_logs.
- Realtime listens to INSERT and UPDATE using Supabase postgres_changes.
- Dashboard loads latest row on startup.
- Chart page loads history from DB and updates from Realtime.
- OTA function now uses bucket configured in CONFIG.STORAGE.FIRMWARE_BUCKET, currently "firmware".

## Expected DB columns
public.weather_logs should include:
id, created_at, frame_counter, device_id, temperature, humidity, pressure,
board_temp, battery, predicted_temp_2h, health_flag, encrypted_payload.
