#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace yawa {

// Intel SDM: IA32_PACKAGE_THERM_STATUS и IA32_TEMPERATURE_TARGET.
inline std::optional<double> intelPackageTemperature(std::uint64_t status, std::uint64_t target)
{
    if ((status & (1ull << 31)) == 0) {
        return std::nullopt;
    }
    const unsigned limit = static_cast<unsigned>((target >> 16) & 0xff);
    const unsigned distance = static_cast<unsigned>((status >> 16) & 0x7f);
    if (limit < 40 || limit > 125 || distance > limit + 20) {
        return std::nullopt;
    }
    return static_cast<double>(limit) - distance;
}

// AMD Zen PPR (17h/19h/1Ah), THM_TCON_CUR_TMP: это Tctl, не универсальная Tdie.
inline std::optional<double> amdControlTemperature(std::uint32_t raw)
{
    if (raw == 0 || raw == 0xffffffffu) {
        return std::nullopt;
    }
    const double celsius = static_cast<double>(raw >> 21) / 8.0
        - ((raw & (1u << 19)) || ((raw >> 16) & 3u) == 3u ? 49.0 : 0.0);
    if (celsius < -20.0 || celsius > 150.0) {
        return std::nullopt;
    }
    return celsius;
}

class CpuEnergyCounter
{
public:
    void reset() { m_hasBaseline = false; }

    std::optional<double> sample(std::uint32_t energy, unsigned exponent, double seconds)
    {
        if (exponent > 31 || !std::isfinite(seconds) || seconds < 0.0) {
            reset();
            return std::nullopt;
        }
        const double elapsed = seconds - m_seconds;
        const std::uint32_t delta = energy - m_energy;
        const bool hasInterval = m_hasBaseline && exponent == m_exponent && elapsed >= 0.001
            && elapsed <= 10.0
            && elapsed * kMaxPackageWatts < std::ldexp(1.0, 32 - static_cast<int>(exponent));
        m_energy = energy;
        m_seconds = seconds;
        m_exponent = exponent;
        m_hasBaseline = true;
        if (!hasInterval) {
            return std::nullopt;
        }
        // Счётчик энергии 32-битный; вычитание unsigned учитывает один оборот.
        const double watts = std::ldexp(static_cast<double>(delta), -static_cast<int>(exponent)) / elapsed;
        return watts <= kMaxPackageWatts ? std::optional<double>(watts) : std::nullopt;
    }

private:
    // Фильтр сбросов счётчика для одного пакета, не паспортный предел мощности CPU.
    static constexpr double kMaxPackageWatts = 2000.0;
    std::uint32_t m_energy = 0;
    unsigned m_exponent = 0;
    double m_seconds = 0.0;
    bool m_hasBaseline = false;
};

} // namespace yawa
