#pragma once

#include "metrics/gpu_registry.h"
#include "platform/nvml_sensors.h"

namespace yawa {
class Log;

class VendorGpuSensors
{
public:
    VendorGpuSensors() = default;
    ~VendorGpuSensors();
    VendorGpuSensors(const VendorGpuSensors&) = delete;
    VendorGpuSensors& operator=(const VendorGpuSensors&) = delete;
    void initialize(const GpuRegistry& registry, Log& log);
    void selectDevice(const GpuAdapter* adapter, bool singleVendor);
    GpuSensorReading read();
    void shutdown();

private:
    struct Impl;
    Impl* m_impl = nullptr;
};
} // namespace yawa
