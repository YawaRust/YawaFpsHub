#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace yawa {

enum class SpdType { Ddr4, Ddr5 };

struct SpdJedecTimings
{
    unsigned tAaPs = 0;
    unsigned tRcdPs = 0;
    unsigned tRpPs = 0;
    unsigned tRasPs = 0;
};

struct SpdIdentity
{
    SpdType type;
    unsigned minimumCyclePs = 0;
    std::optional<unsigned> moduleCapacityMiB;
    std::optional<SpdJedecTimings> jedecTimings;
};

// Pure decoder: JEDEC passport, not current controller timings or an XMP/EXPO state.
// Only the CRC-covered base 128 bytes are consumed. DDR5 module organization and
// vendor profile pages are intentionally not inferred from an incomplete dump.
inline std::optional<SpdIdentity> decodeSpdBase(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 128 || (bytes[2] != 0x0c && bytes[2] != 0x12))
        return std::nullopt;
    const bool isDdr4 = bytes[2] == 0x0c;
    const std::size_t covered = isDdr4 && (bytes[0] & 0x80u) ? 116 : 126;
    std::uint16_t crc = 0;
    for (std::size_t index = 0; index < covered; ++index) {
        crc ^= static_cast<std::uint16_t>(bytes[index]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000u) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<std::uint16_t>(crc << 1);
        }
    }
    if (crc != static_cast<unsigned>(bytes[126] | (bytes[127] << 8)))
        return std::nullopt;

    SpdIdentity result { isDdr4 ? SpdType::Ddr4 : SpdType::Ddr5, 0, std::nullopt, std::nullopt };
    if (!isDdr4) {
        result.minimumCyclePs = static_cast<unsigned>(bytes[20] | (bytes[21] << 8));
        if (result.minimumCyclePs < 250 || result.minimumCyclePs > 2500)
            return std::nullopt;
        return result;
    }

    const int tCk = bytes[18] * 125 + static_cast<std::int8_t>(bytes[125]);
    if (tCk < 500 || tCk > 2500)
        return std::nullopt;
    result.minimumCyclePs = static_cast<unsigned>(tCk);
    const unsigned densityCode = bytes[4] & 0x0fu;
    const unsigned deviceCode = bytes[12] & 7u;
    const unsigned busCode = bytes[13] & 7u;
    if (densityCode <= 7 && deviceCode <= 3 && busCode <= 3 && (bytes[6] & 0x80u) == 0) {
        const unsigned densityMbit = 256u << densityCode;
        const unsigned width = 4u << deviceCode;
        const unsigned busWidth = 8u << busCode;
        const unsigned ranks = 1u + ((bytes[12] >> 3) & 7u);
        if (busWidth >= width)
            result.moduleCapacityMiB = densityMbit / 8 * (busWidth / width) * ranks;
    }
    const int tAa = bytes[24] * 125 + static_cast<std::int8_t>(bytes[123]);
    const int tRcd = bytes[25] * 125 + static_cast<std::int8_t>(bytes[122]);
    const int tRp = bytes[26] * 125 + static_cast<std::int8_t>(bytes[121]);
    const int tRas = (((bytes[27] & 15u) << 8) | bytes[28]) * 125;
    if (tAa > 0 && tRcd > 0 && tRp > 0 && tRas > 0) {
        result.jedecTimings = SpdJedecTimings { static_cast<unsigned>(tAa), static_cast<unsigned>(tRcd),
            static_cast<unsigned>(tRp), static_cast<unsigned>(tRas) };
    }
    return result;
}

} // namespace yawa
