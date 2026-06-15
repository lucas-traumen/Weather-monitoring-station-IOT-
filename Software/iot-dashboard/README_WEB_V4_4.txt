# WEB_VERSION 4.4 — Full UI cleanup + visible OTA

## Main changes
- OTA panel is visible again on the Dashboard page.
- Config modal still keeps an OTA section for quick access.
- Dashboard font sizes were increased for readability.
- Sensor values and units are baseline-aligned.
- Metric cards are larger and more evenly spaced.
- Dashboard still avoids duplicate sensor values: each metric appears once.
- OTA uses CONFIG.STORAGE.FIRMWARE_BUCKET and writes ota_url, ota_seq, ota_sha256 to device_configs.

## Replace these files
- index.html
- app.js
- style.css
- config.js
- supabase.js
- chart.html
- chart-render.js
- manifest_v4_4.json

## Notes
Browser reads plaintext rows from public.weather_logs through Supabase Realtime.
Do not put ASCON keys or service_role key in browser JavaScript.
