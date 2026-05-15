async function pushSensorData(data) {

  const payload = {
    temperature: data.temperature,
    humidity: data.humidity,
    pressure: data.pressure,
    battery: data.battery,

    pressure_trend: data.pressure_trend || 'stable',
    storm_warning: data.storm_warning || false,
    device_id: data.device_id || 'SIMULATOR'
  };

  const { error } = await supabaseClient
    .from(CONFIG.API.TABLE)
    .insert([payload]);

  if (error) {
    console.error('Push failed:', error);
    return false;
  }

  console.log('Data pushed:', payload);
  return true;
}