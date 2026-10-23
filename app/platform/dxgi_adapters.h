#pragma once

#include "metrics/gpu_registry.h"

#include <cstdint>
#include <string>
#include <vector>

struct IDXGIAdapter1;
struct IDXGIFactory1;

namespace yawa {

struct AdapterMemoryInfo {
    std::uint64_t budgetBytes = 0;
    std::uint64_t usageBytes = 0;
    std::uint64_t availableForReservation = 0;
    std::uint64_t currentReservation = 0;
};

// DXGI даёт имя, LUID, объём памяти и бюджет видеопамяти без прав администратора
// и без сторонних драйверов — это основной источник для блока «Видеокарта».
class DxgiAdapterList {
public:
    DxgiAdapterList() = default;
    ~DxgiAdapterList();

    DxgiAdapterList(const DxgiAdapterList&) = delete;
    DxgiAdapterList& operator=(const DxgiAdapterList&) = delete;

    bool refresh(std::string& error);
    const GpuRegistry& registry() const { return m_registry; }
    GpuRegistry& registry() { return m_registry; }

    bool queryMemory(std::size_t index, AdapterMemoryInfo& info) const;

private:
    void release();

    GpuRegistry m_registry;
    std::vector<IDXGIAdapter1*> m_adapters;
    IDXGIFactory1* m_factory = nullptr;
};

// Метка вида luid_0x00000000_0x0000C4B3: по ней PDH-инстансы GPU Engine привязываются к адаптеру.
std::wstring luidTag(std::uint32_t luidLow, std::int32_t luidHigh);

} // namespace yawa
