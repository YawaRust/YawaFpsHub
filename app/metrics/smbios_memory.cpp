#include "metrics/smbios_memory.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace yawa {
namespace {

    std::uint16_t wordAt(std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        if (offset + 2 > bytes.size()) {
            return 0;
        }
        return static_cast<std::uint16_t>(
            bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
    }

    std::uint32_t dwordAt(std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        if (offset + 4 > bytes.size()) {
            return 0;
        }
        return static_cast<std::uint32_t>(bytes[offset])
            | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    }

    std::string smbiosString(std::span<const std::uint8_t> strings, std::uint8_t index)
    {
        if (index == 0) {
            return { };
        }
        std::size_t start = 0;
        for (unsigned current = 1; current < index; ++current) {
            while (start < strings.size() && strings[start] != 0) {
                ++start;
            }
            if (start == strings.size()) {
                return { };
            }
            ++start;
        }
        if (start >= strings.size() || strings[start] == 0) {
            return { };
        }
        std::size_t end = start;
        while (end < strings.size() && strings[end] != 0) {
            ++end;
        }
        return std::string(reinterpret_cast<const char*>(strings.data() + start), end - start);
    }

    struct MemoryArray
    {
        std::uint16_t handle = 0;
        std::uint16_t slots = 0;
    };

} // namespace

std::string smbiosMemoryTypeName(std::uint8_t type)
{
    switch (type) {
    case 0x12:
        return "DDR";
    case 0x13:
        return "DDR2";
    case 0x18:
        return "DDR3";
    case 0x1A:
        return "DDR4";
    case 0x22:
        return "DDR5";
    case 0x1E:
        return "LPDDR4";
    case 0x21:
        return "LPDDR5";
    default:
        return { };
    }
}

double validatedMemorySpeed(double speed)
{
    return std::isfinite(speed) && speed >= 100.0 && speed <= 50000.0 ? speed : 0.0;
}

MemoryIdentity parseSmbiosMemory(std::span<const std::uint8_t> raw, double availableGb)
{
    MemoryIdentity memory;
    memory.totalGb = availableGb;
    // RAW_SMBIOS_DATA: метод/версия (4 байта), длина DWORD, затем структуры.
    if (raw.size() < 8 || dwordAt(raw, 4) > raw.size() - 8) {
        return memory;
    }
    const std::size_t end = 8 + dwordAt(raw, 4);
    std::vector<MemoryArray> arrays;
    std::vector<std::pair<std::uint16_t, MemoryModule>> modules;
    for (std::size_t offset = 8; offset + 4 <= end;) {
        const std::uint8_t type = raw[offset];
        const std::size_t length = raw[offset + 1];
        if (length < 4 || length > end - offset) {
            break;
        }
        const auto record = raw.subspan(offset, length);
        const std::size_t stringsStart = offset + length;
        std::size_t next = stringsStart;
        while (next + 1 < end && (raw[next] != 0 || raw[next + 1] != 0)) {
            ++next;
        }
        if (next + 1 >= end) {
            break;
        }
        const auto strings = raw.subspan(stringsStart, next + 1 - stringsStart);
        offset = next + 2;

        if (type == 16 && length >= 0x0F && record[5] == 3) {
            const std::uint16_t devices = wordAt(record, 0x0D);
            if (devices > 0 && devices <= 64) {
                arrays.push_back({ wordAt(record, 2), devices });
            }
        } else if (type == 17 && length >= 0x15) {
            const std::uint16_t rawSize = wordAt(record, 0x0C);
            if (rawSize == 0 || rawSize == 0xFFFF) {
                continue;
            }
            MemoryModule module;
            module.populated = true;
            module.sizeGb = smbiosModuleSizeGb(rawSize, dwordAt(record, 0x1C));
            module.slot = smbiosString(strings, record[0x10]);
            module.type = smbiosMemoryTypeName(record[0x12]);
            if (length >= 0x17) {
                const std::uint16_t speed = wordAt(record, 0x15);
                module.ratedMt
                    = validatedMemorySpeed(speed == 0xFFFF && length >= 0x5C ? dwordAt(record, 0x54)
                            : speed == 0xFFFF                                ? 0.0
                                                                             : speed);
            }
            if (length >= 0x1B) {
                module.vendor = smbiosString(strings, record[0x17]);
                module.part = smbiosString(strings, record[0x1A]);
            }
            if (length >= 0x22) {
                const std::uint16_t speed = wordAt(record, 0x20);
                module.configuredMt
                    = validatedMemorySpeed(speed == 0xFFFF && length >= 0x5C ? dwordAt(record, 0x58)
                            : speed == 0xFFFF                                ? 0.0
                                                                             : speed);
            }
            if (length >= 0x28) {
                const std::uint16_t voltage = wordAt(record, 0x26);
                module.voltage = voltage != 0 && voltage != 0xFFFF ? voltage / 1000.0 : 0.0;
            }
            modules.emplace_back(wordAt(record, 4), std::move(module));
        }
    }

    std::uint32_t slotCount = 0;
    for (const MemoryArray& array : arrays) {
        slotCount += array.slots;
    }
    for (auto& [handle, module] : modules) {
        if (!arrays.empty() && std::none_of(arrays.begin(), arrays.end(), [handle](const MemoryArray& array) {
                return array.handle == handle;
            })) {
            continue;
        }
        memory.modules.push_back(std::move(module));
    }
    if (slotCount >= memory.modules.size() && slotCount <= 64) {
        memory.slots = slotCount;
    }
    double reportedGb = 0.0;
    for (const MemoryModule& module : memory.modules) {
        reportedGb += module.sizeGb;
    }
    if (!memory.modules.empty() && !smbiosSizesPlausible(reportedGb, availableGb)) {
        for (MemoryModule& module : memory.modules) {
            module.sizeGb = 0.0;
        }
    }
    return memory;
}

double configuredMemorySpeed(const std::vector<MemoryModule>& modules)
{
    double speed = 0.0;
    std::size_t populated = 0;
    for (const MemoryModule& module : modules) {
        if (!module.populated) {
            continue;
        }
        ++populated;
        if (validatedMemorySpeed(module.configuredMt) == 0.0) {
            return 0.0;
        }
        if (speed > 0.0 && std::abs(speed - module.configuredMt) > 1.0) {
            return 0.0;
        }
        speed = module.configuredMt;
    }
    return populated > 0 ? speed : 0.0;
}

} // namespace yawa
