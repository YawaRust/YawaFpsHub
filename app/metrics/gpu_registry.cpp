#include "metrics/gpu_registry.h"

#include <algorithm>
#include <cwctype>
#include <utility>

namespace yawa {

void GpuRegistry::add(GpuAdapter adapter)
{
    m_adapters.push_back(std::move(adapter));
}

const GpuAdapter* GpuRegistry::byIndex(std::size_t index) const
{
    return index < m_adapters.size() ? &m_adapters[index] : nullptr;
}

const GpuAdapter* GpuRegistry::byLuid(std::uint32_t luidLow, std::int32_t luidHigh) const
{
    for (const GpuAdapter& adapter : m_adapters) {
        if (adapter.luidLow == luidLow && adapter.luidHigh == luidHigh) {
            return &adapter;
        }
    }
    return nullptr;
}

std::size_t GpuRegistry::preferredIndex() const
{
    std::size_t best = 0;
    bool hasBest = false;

    for (std::size_t index = 0; index < m_adapters.size(); ++index) {
        const GpuAdapter& adapter = m_adapters[index];
        if (adapter.isSoftware) {
            continue;
        }
        if (!hasBest || adapter.dedicatedVideoMemory > m_adapters[best].dedicatedVideoMemory) {
            best = index;
            hasBest = true;
        }
    }
    return best;
}

std::size_t GpuRegistry::resolve(std::uint32_t preferredLuidLow, std::int32_t preferredLuidHigh) const
{
    if (m_adapters.empty()) {
        return 0;
    }
    if (byLuid(preferredLuidLow, preferredLuidHigh) != nullptr) {
        for (std::size_t index = 0; index < m_adapters.size(); ++index) {
            if (m_adapters[index].luidLow == preferredLuidLow && m_adapters[index].luidHigh == preferredLuidHigh) {
                return index;
            }
        }
    }
    return preferredIndex();
}

GpuVendor vendorFromId(std::uint32_t vendorId)
{
    switch (vendorId) {
    case 0x10DE:
        return GpuVendor::Nvidia;
    case 0x1002:
    case 0x1022:
        return GpuVendor::Amd;
    case 0x8086:
        return GpuVendor::Intel;
    case 0x1414:
        return GpuVendor::Microsoft;
    default:
        return GpuVendor::Unknown;
    }
}

std::string toUtf8(const std::wstring& text)
{
    std::string result;
    result.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t code = text[index];

        if (code >= 0xD800 && code <= 0xDBFF && index + 1 < text.size()) {
            const std::uint32_t low = text[index + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
        }

        if (code < 0x80) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return result;
}

std::optional<std::size_t> uniqueGpuNameMatch(std::wstring_view hint, const std::vector<std::wstring>& names)
{
    const auto lower = [](std::wstring_view name) {
        std::wstring result(name);
        std::transform(result.begin(), result.end(), result.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return result;
    };
    const std::wstring target = lower(hint);
    if (target.empty()) {
        return std::nullopt;
    }
    std::optional<std::size_t> selected;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::wstring name = lower(names[i]);
        if (!name.empty()
            && (target.find(name) != std::wstring::npos || name.find(target) != std::wstring::npos)) {
            if (selected) {
                return std::nullopt;
            }
            selected = i;
        }
    }
    return selected;
}

} // namespace yawa
