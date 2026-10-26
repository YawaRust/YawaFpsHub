#include "metrics/hardware_info.h"

#include "metrics/units.h"

#include <algorithm>
#include <cmath>

namespace yawa {

double MemoryIdentity::cycleTimeNs() const
{
    // tCK = 2000 / MT/с: DDR делает два переноса за такт, поэтому частота в МТ/с
    // вдвое выше тактовой. Величина честная — её отдаёт BIOS через SMBIOS.
    if (configuredMt <= 0.0) {
        return 0.0;
    }
    return 2000.0 / configuredMt;
}

const GpuIdentity* HardwareIdentity::primaryGpu() const
{
    const auto discrete = std::find_if(gpus.begin(), gpus.end(), [](const GpuIdentity& gpu) {
        return gpu.discrete;
    });
    return discrete == gpus.end() ? (gpus.empty() ? nullptr : &gpus.front()) : &*discrete;
}

const DisplayIdentity* HardwareIdentity::primaryDisplay() const
{
    const auto primary = std::find_if(displays.begin(), displays.end(),
        [](const DisplayIdentity& display) { return display.primary; });
    return primary == displays.end() ? (displays.empty() ? nullptr : &displays.front()) : &*primary;
}

std::string describeMemory(const MemoryIdentity& memory)
{
    std::vector<double> sizes;
    double sum = 0.0;
    for (const MemoryModule& module : memory.modules) {
        if (module.populated && module.sizeGb > 0.0) {
            sizes.push_back(module.sizeGb);
            sum += module.sizeGb;
        }
    }
    std::sort(sizes.begin(), sizes.end());

    std::string text;
    // «2×16 ГБ» вместо перечисления планок: так видно, что память стоит двумя модулями.
    // Раскладку показываем только когда она сходится с полным объёмом: на виртуалках
    // SMBIOS рапортует по рангам, и «2×1 ГБ = 16 ГБ» выглядело бы поломкой.
    const bool consistent = !sizes.empty() && (memory.totalGb <= 0.0 || sum > memory.totalGb * 0.75);
    if (consistent) {
        const double smallest = sizes.front();
        const bool uniform = std::all_of(sizes.begin(), sizes.end(),
            [smallest](double size) { return std::abs(size - smallest) < 0.5; });
        if (uniform) {
            text = std::to_string(sizes.size()) + "×" + formatValue(smallest, smallest < 1.0 ? 1 : 0) + " ГБ";
        } else {
            for (std::size_t index = 0; index < sizes.size(); ++index) {
                if (index > 0) {
                    text += " + ";
                }
                text += formatValue(sizes[index], sizes[index] < 1.0 ? 1 : 0) + " ГБ";
            }
        }
        if (memory.totalGb > 0.0) {
            text += " = " + formatValue(memory.totalGb, 0) + " ГБ";
        }
    } else if (memory.totalGb > 0.0) {
        text = formatValue(memory.totalGb, 0) + " ГБ";
        if (!sizes.empty()) {
            text += ", планок " + std::to_string(sizes.size());
        }
    } else {
        return "нет данных";
    }

    if (!memory.type.empty()) {
        text += " " + memory.type;
        if (memory.configuredMt > 0.0) {
            text.push_back('-');
            text += std::to_string(static_cast<int>(std::lround(memory.configuredMt)));
        }
    }
    return text;
}

std::string describeDisplay(const DisplayIdentity& display)
{
    std::string text;
    if (!display.name.empty()) {
        text = display.name + " · ";
    }
    if (display.width > 0 && display.height > 0) {
        text += std::to_string(display.width) + "×" + std::to_string(display.height);
    } else {
        text += "нет данных";
    }
    if (display.refreshHz > 0) {
        text += " · " + std::to_string(display.refreshHz) + " Гц";
    }
    return text;
}

} // namespace yawa
