#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yawa {

enum class GpuVendor : std::uint8_t { Unknown, Nvidia, Amd, Intel, Microsoft };

struct GpuAdapter {
    std::uint32_t luidLow = 0;
    std::int32_t luidHigh = 0;
    std::wstring name;
    GpuVendor vendor = GpuVendor::Unknown;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedVideoMemory = 0;
    bool isSoftware = false;
};

// Порядок адаптеров должен быть стабильным между запусками: на него опираются профили
// игр и сохранённый выбор GPU в ini, а DXGI перечисляет адаптеры в разном порядке,
// когда меняется набор мониторов.
class GpuRegistry {
public:
    void add(GpuAdapter adapter);

    std::size_t count() const { return m_adapters.size(); }
    bool empty() const { return m_adapters.empty(); }

    const GpuAdapter* byIndex(std::size_t index) const;
    const GpuAdapter* byLuid(std::uint32_t luidLow, std::int32_t luidHigh) const;

    // Выбор по умолчанию — дискретная карта с наибольшим объёмом памяти: на ноутбуках
    // с гибридной графикой первый адаптер DXGI часто оказывается встроенным.
    std::size_t preferredIndex() const;

    std::size_t resolve(std::uint32_t preferredLuidLow, std::int32_t preferredLuidHigh) const;

    const std::vector<GpuAdapter>& adapters() const { return m_adapters; }

private:
    std::vector<GpuAdapter> m_adapters;
};

std::optional<std::size_t> uniqueGpuNameMatch(std::wstring_view hint, const std::vector<std::wstring>& names);

GpuVendor vendorFromId(std::uint32_t vendorId);
std::string toUtf8(const std::wstring& text);

} // namespace yawa
