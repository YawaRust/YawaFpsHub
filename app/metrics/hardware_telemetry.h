#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace yawa {

inline std::optional<double> nvmeCompositeTemperature(std::span<const std::byte> log)
{
    if (log.size() < 512) {
        return std::nullopt;
    }
    const unsigned kelvin = std::to_integer<unsigned>(log[1]) | (std::to_integer<unsigned>(log[2]) << 8);
    if (kelvin == 0 || kelvin == 0xffff) {
        return std::nullopt;
    }
    const double celsius = kelvin - 273.15;
    return celsius >= -40.0 && celsius <= 150.0 ? std::optional<double>(celsius) : std::nullopt;
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
