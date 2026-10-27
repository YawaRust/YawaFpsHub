#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace yawa {

// Family 17h model 71h (Matisse), UMC 0x50100/0x50DF0/0x50000/0x50008.
// Нулевой код типа означает DDR4; all-ones не считаем установленным модулем.
inline bool isMatisseDdr4Channel(unsigned family, unsigned model, std::uint32_t config, std::uint32_t maxRate,
    std::uint32_t dimm0, std::uint32_t dimm1)
{
    if (family != 0x17 || model != 0x71 || config == 0xffffffffu || maxRate == 0xffffffffu
        || dimm0 == 0xffffffffu || dimm1 == 0xffffffffu) {
        return false;
    }
    return (config & 3u) == 0 && (maxRate & (1u << 19)) == 0 && ((dimm0 | dimm1) & 1u) != 0;
}

struct AmdDdr4Timings
{
    unsigned cl = 0;
    unsigned rcdRead = 0;
    unsigned rcdWrite = 0;
    unsigned rp = 0;
    unsigned ras = 0;
    unsigned rc = 0;
    bool operator==(const AmdDdr4Timings&) const = default;
};

// UMC DDR4 registers 0x50204 and 0x50208, for an already identified channel.
// Layout cross-checked with ZenStates-Core DDR4Dictionary at 869b7e8; this function
// does not identify a CPU, read registers or establish that DDR4 is installed.
inline std::optional<AmdDdr4Timings> decodeAmdDdr4Timings(std::uint32_t timing0, std::uint32_t timing1)
{
    if (timing0 == 0 || timing1 == 0 || timing0 == 0xffffffffu || timing1 == 0xffffffffu) {
        return std::nullopt;
    }
    const AmdDdr4Timings timings { timing0 & 0x3fu, (timing0 >> 16) & 0x3fu, (timing0 >> 24) & 0x3fu,
        (timing1 >> 16) & 0x3fu, (timing0 >> 8) & 0x7fu, timing1 & 0xffu };
    if (timings.cl == 0 || timings.rcdRead == 0 || timings.rcdWrite == 0 || timings.rp == 0
        || timings.ras == 0 || timings.rc == 0 || timings.rc < timings.ras) {
        return std::nullopt;
    }
    return timings;
}

struct AmdDdr4ChannelRegisters
{
    std::uint32_t config = 0;
    std::uint32_t maxRate = 0;
    std::uint32_t dimm0 = 0;
    std::uint32_t dimm1 = 0;
    std::uint32_t timing0 = 0;
    std::uint32_t timing1 = 0;
    bool hasTimings = false;
};

using AmdDdr4MemoryDump = std::array<AmdDdr4ChannelRegisters, 2>;

inline std::optional<AmdDdr4Timings> decodeAmdDdr4MemoryDump(const AmdDdr4MemoryDump& dump)
{
    std::optional<AmdDdr4Timings> result;
    for (const auto& channel : dump) {
        if (!isMatisseDdr4Channel(
                0x17, 0x71, channel.config, channel.maxRate, channel.dimm0, channel.dimm1)) {
            if (((channel.dimm0 | channel.dimm1) & 1u) != 0) {
                return std::nullopt;
            }
            continue;
        }
        if (!channel.hasTimings) {
            return std::nullopt;
        }
        const auto decoded = decodeAmdDdr4Timings(channel.timing0, channel.timing1);
        if (!decoded || (result && *result != *decoded)) {
            return std::nullopt;
        }
        result = decoded;
    }
    return result;
}

inline std::string formatAmdDdr4MemoryDump(const AmdDdr4MemoryDump& dump)
{
    std::string text
        = "YawaFpsHub UMC dump v1; AMD Family 17h Model 71h; read-only.\n"
          "Review before sharing: memory configuration may be sensitive. No SPD, serials or paths.\n";
    for (std::size_t channel = 0; channel < dump.size(); ++channel) {
        const auto& data = dump[channel];
        const unsigned base = static_cast<unsigned>(channel) << 20;
        char line[256] {};
        std::snprintf(line, sizeof(line),
            "channel %u: 0x%08X=0x%08X 0x%08X=0x%08X "
            "0x%08X=0x%08X 0x%08X=0x%08X\n",
            static_cast<unsigned>(channel), base | 0x50100u, data.config, base | 0x50df0u, data.maxRate,
            base | 0x50000u, data.dimm0, base | 0x50008u, data.dimm1);
        text += line;
        if (!data.hasTimings) {
            text += "timing registers: not_read\n";
            continue;
        }
        std::snprintf(line, sizeof(line), "0x%08X=0x%08X 0x%08X=0x%08X\n", base | 0x50204u, data.timing0,
            base | 0x50208u, data.timing1);
        text += line;
    }
    return text;
}

} // namespace yawa
