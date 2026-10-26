#pragma once

#include <cstdint>

namespace yawa {

// SMBIOS Type 17, offset 0x10: 0x7fff means use Extended Size (0x20, MB).
// Bit 15 on other values switches the lower 15 bits from MB to KB.
inline double smbiosModuleSizeGb(std::uint16_t size, std::uint32_t extendedMb)
{
    if (size == 0 || size == 0xffff) {
        return 0.0;
    }
    const double mb = size == 0x7fff ? static_cast<double>(extendedMb)
        : (size & 0x8000)            ? static_cast<double>(size & 0x7fff) / 1024.0
                                     : static_cast<double>(size);
    const double gb = mb / 1024.0;
    return gb >= 0.25 && gb <= 8192.0 ? gb : 0.0;
}

// Физически доступная память может быть немного меньше суммы установленных модулей
// (резерв iGPU и прошивки). Большое расхождение — ненадёжный Type 17.
inline bool smbiosSizesPlausible(double reportedGb, double availableGb)
{
    return availableGb <= 0.0 || (reportedGb >= availableGb * 0.85 && reportedGb <= availableGb * 1.35);
}

} // namespace yawa
