function random(min, max) {
  return +(Math.random() * (max - min) + min).toFixed(1);
}

function generateFakeData() {

  const pressure = random(995, 1025);

  return {
    temperature: random(25, 38),
    humidity: random(40, 90),
    pressure,
    battery: random(3.6, 4.2),

    pressure_trend:
      pressure < 1000
        ? 'falling'
        : pressure > 1020
        ? 'rising'
        : 'stable',

    storm_warning: pressure < 990,

    device_id: 'SIMULATOR'
  };
}

async function startSimulator() {

  console.log('Simulator started');

  setInterval(async () => {

    const data = generateFakeData();

    await pushSensorData(data);

  }, 3000);
}