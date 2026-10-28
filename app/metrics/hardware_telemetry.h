#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace yawa {

struct NvmeSmartHealth
{
    std::optional<double> compositeCelsius;
    std::uint8_t criticalWarning = 0;
    std::optional<unsigned> availableSparePercent;
    std::optional<unsigned> spareThresholdPercent;
    unsigned percentUsed = 0;
    std::optional<std::uint64_t> dataUnitsRead;
    std::optional<std::uint64_t> dataUnitsWritten;
    std::optional<std::uint64_t> powerOnHours;
    std::optional<std::uint64_t> unsafeShutdowns;
    std::optional<std::uint64_t> mediaErrors;
    std::optional<std::uint64_t> errorLogEntries;
};

inline std::optional<double> nvmeKelvin(unsigned kelvin)
{
    if (kelvin == 0 || kelvin == 0xffff) {
        return std::nullopt;
    }
    const double celsius = kelvin - 273.15;
    return celsius >= -40.0 && celsius <= 150.0 ? std::optional<double>(celsius) : std::nullopt;
}

inline std::optional<std::uint64_t> nvmeCounter(std::span<const std::byte> log, std::size_t offset)
{
    // NVMe health counters occupy 128 bits. Do not silently truncate a large counter to 64 bits.
    for (std::size_t i = 8; i < 16; ++i) {
        if (log[offset + i] != std::byte { 0 }) {
            return std::nullopt;
        }
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= std::uint64_t(std::to_integer<unsigned>(log[offset + i])) << (8 * i);
    }
    return value;
}

inline std::optional<NvmeSmartHealth> decodeNvmeSmartHealth(std::span<const std::byte> log)
{
    if (log.size() < 512) {
        return std::nullopt;
    }
    NvmeSmartHealth health;
    const unsigned kelvin = std::to_integer<unsigned>(log[1]) | (std::to_integer<unsigned>(log[2]) << 8);
    health.compositeCelsius = nvmeKelvin(kelvin);
    health.criticalWarning = std::to_integer<std::uint8_t>(log[0]);
    const unsigned spare = std::to_integer<unsigned>(log[3]);
    const unsigned threshold = std::to_integer<unsigned>(log[4]);
    if (spare <= 100)
        health.availableSparePercent = spare;
    if (threshold <= 100)
        health.spareThresholdPercent = threshold;
    health.percentUsed = std::to_integer<unsigned>(log[5]); // 255 means at least 255% in NVMe.
    health.dataUnitsRead = nvmeCounter(log, 32);
    health.dataUnitsWritten = nvmeCounter(log, 48);
    health.powerOnHours = nvmeCounter(log, 128);
    health.unsafeShutdowns = nvmeCounter(log, 144);
    health.mediaErrors = nvmeCounter(log, 160);
    health.errorLogEntries = nvmeCounter(log, 176);
    return health;
}

inline std::optional<double> nvmeCompositeTemperature(std::span<const std::byte> log)
{
    const auto health = decodeNvmeSmartHealth(log);
    return health ? health->compositeCelsius : std::nullopt;
}

class ContinuousEnergyCounter
{
public:
    void reset() { m_ready = false; }
    std::optional<double> sample(double joules, double seconds)
    {
        if (!std::isfinite(joules) || !std::isfinite(seconds) || joules < 0 || seconds < 0) {
            reset();
            return std::nullopt;
        }
        const double elapsed = seconds - m_seconds;
        const double delta = joules - m_joules;
        const bool valid = m_ready && elapsed >= 0.001 && elapsed <= 10.0 && delta >= 0;
        m_joules = joules;
        m_seconds = seconds;
        m_ready = true;
        if (!valid) {
            return std::nullopt;
        }
        const double watts = delta / elapsed;
        return watts <= 2000.0 ? std::optional<double>(watts) : std::nullopt;
    }

private:
    double m_joules = 0;
    double m_seconds = 0;
    bool m_ready = false;
};

} // namespace yawa
