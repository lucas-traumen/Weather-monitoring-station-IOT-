-- ============================================================
-- SCHEMA VERSION 3.7 - Plaintext DB, secure ingest by Edge Function
-- Architecture:
--   ESP32 -> Supabase Edge Function -> public.weather_logs -> Web Dashboard
-- Notes:
--   - weather_logs stores readable/plaintext values for dashboard/query.
--   - encrypted_payload stores the ASCON key2 ciphertext for audit/debug only.
--   - anon can SELECT for dashboard, but cannot INSERT directly.
--   - Edge Function should insert using SUPABASE_SERVICE_ROLE_KEY.
-- ============================================================

BEGIN;

DROP TABLE IF EXISTS public.weather_logs CASCADE;

CREATE TABLE public.weather_logs (
    id BIGSERIAL PRIMARY KEY,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),

    frame_counter BIGINT UNIQUE NOT NULL,
    device_id TEXT NOT NULL DEFAULT 'ESP32_LORA_GW',

    temperature FLOAT8,
    humidity FLOAT8,
    pressure FLOAT8,
    board_temp FLOAT8,
    battery FLOAT8,
    predicted_temp_2h FLOAT8,

    health_flag INTEGER,
    encrypted_payload TEXT,

    CONSTRAINT encrypted_payload_hex_or_null CHECK (
      encrypted_payload IS NULL OR encrypted_payload ~ '^[0-9a-fA-F]+$'
    )
);

CREATE INDEX idx_weather_logs_created_at
ON public.weather_logs(created_at DESC);

CREATE INDEX idx_weather_logs_frame_counter
ON public.weather_logs(frame_counter);

CREATE INDEX idx_weather_logs_device_id
ON public.weather_logs(device_id);

ALTER TABLE public.weather_logs ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "dashboard_select" ON public.weather_logs;
DROP POLICY IF EXISTS "anon read" ON public.weather_logs;
DROP POLICY IF EXISTS "anon insert" ON public.weather_logs;
DROP POLICY IF EXISTS "allow_insert" ON public.weather_logs;
DROP POLICY IF EXISTS "allow_select" ON public.weather_logs;
DROP POLICY IF EXISTS "esp32_insert" ON public.weather_logs;

CREATE POLICY "dashboard_select"
ON public.weather_logs
FOR SELECT
TO anon
USING (true);

-- IMPORTANT: no anon INSERT policy here.
-- Edge Function inserts with service_role and bypasses RLS.

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM pg_publication_tables
        WHERE pubname = 'supabase_realtime'
          AND schemaname = 'public'
          AND tablename = 'weather_logs'
    ) THEN
        ALTER PUBLICATION supabase_realtime ADD TABLE public.weather_logs;
    END IF;
END $$;

CREATE TABLE IF NOT EXISTS public.device_configs (
    device_id TEXT PRIMARY KEY,
    ota_url TEXT,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

ALTER TABLE public.device_configs ENABLE ROW LEVEL SECURITY;

DROP POLICY IF EXISTS "device_configs_select" ON public.device_configs;
DROP POLICY IF EXISTS "dashboard_select_configs" ON public.device_configs;

CREATE POLICY "device_configs_select"
ON public.device_configs
FOR SELECT
TO anon
USING (true);

NOTIFY pgrst, 'reload schema';

COMMIT;

SELECT
    column_name,
    data_type,
    is_nullable,
    column_default
FROM information_schema.columns
WHERE table_schema = 'public'
  AND table_name = 'weather_logs'
ORDER BY ordinal_position;
