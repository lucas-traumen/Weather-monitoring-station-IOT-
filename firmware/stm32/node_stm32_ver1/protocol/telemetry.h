#ifndef _TELEMETRY_H_
#define _TELEMETRY_H_

#include "min.h"
#include "sensors.h"

// Định nghĩa ID cho gói tin
#define MIN_ID_TELEMETRY 0x01

void Telemetry_Send_MIN(struct min_context *min_ctx, const SensorData_t *data, uint8_t battery_pct);

#endif

