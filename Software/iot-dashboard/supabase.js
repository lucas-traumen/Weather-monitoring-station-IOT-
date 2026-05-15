const supabaseClient = supabase.createClient(
  CONFIG.SUPABASE_URL,
  CONFIG.SUPABASE_KEY
);

function subscribeRealtime(callback) {
  supabaseClient
    .channel('weather-realtime')
    .on(
      'postgres_changes',
      {
        event: 'INSERT',
        schema: 'public',
        table: CONFIG.API.TABLE
      },
      (payload) => {
        callback(payload.new);
      }
    )
    .subscribe();
}