#pragma once

#include "metrics/metric_id.h"

namespace yawa {

// Отсутствие значения само по себе не доказывает, что устройство не поддерживается.
enum class SensorFailure { Unknown, NeedsAccess, Unsupported };
enum class CpuDeviceFailure { Missing, AccessDenied, Other };

constexpr SensorFailure cpuSensorFailure(bool supported, CpuDeviceFailure device)
{
    if (!supported) {
        return SensorFailure::Unsupported;
    }
    return device == CpuDeviceFailure::Missing || device == CpuDeviceFailure::AccessDenied
        ? SensorFailure::NeedsAccess
        : SensorFailure::Unknown;
}

constexpr SensorFailure sensorMissingReason(MetricId id, SensorFailure cpuHardware)
{
    return id == MetricId::CpuTemp || id == MetricId::CpuPower ? cpuHardware : SensorFailure::Unknown;
}

} // namespace yawa
