#include "platform/dxgi_adapters.h"
#include "platform/win_util.h"

#include <windows.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <utility>

namespace yawa {

std::wstring luidTag(std::uint32_t luidLow, std::int32_t luidHigh)
{
    const auto high = static_cast<std::uint32_t>(luidHigh);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"luid_0x%08X_0x%08X", high, luidLow);
    return buffer;
}

DxgiAdapterList::~DxgiAdapterList()
{
    release();
}

void DxgiAdapterList::release()
{
    for (IDXGIAdapter1* adapter : m_adapters) {
        if (adapter != nullptr) {
            adapter->Release();
        }
    }
    m_adapters.clear();

    if (m_factory != nullptr) {
        m_factory->Release();
        m_factory = nullptr;
    }
}

bool DxgiAdapterList::refresh(std::string& error)
{
    release();

    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&m_factory));
    if (FAILED(result)) {
        error = "CreateDXGIFactory1: " + hresultText(result);
        return false;
    }

    GpuRegistry registry;
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        if (m_factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (adapter == nullptr) {
            continue;
        }

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) {
            adapter->Release();
            continue;
        }

        GpuAdapter entry;
        entry.luidLow = description.AdapterLuid.LowPart;
        entry.luidHigh = description.AdapterLuid.HighPart;
        entry.name = description.Description;
        entry.vendorId = description.VendorId;
        entry.deviceId = description.DeviceId;
        entry.vendor = vendorFromId(description.VendorId);
        entry.dedicatedVideoMemory = description.DedicatedVideoMemory;
        entry.isSoftware = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        registry.add(entry);
        m_adapters.push_back(adapter);
    }

    m_registry = std::move(registry);
    if (m_adapters.empty()) {
        error = "DXGI не вернул ни одного адаптера";
        return false;
    }
    return true;
}

bool DxgiAdapterList::queryMemory(std::size_t index, AdapterMemoryInfo& info) const
{
    if (index >= m_adapters.size()) {
        return false;
    }

    IDXGIAdapter3* adapter3 = nullptr;
    if (FAILED(m_adapters[index]->QueryInterface(IID_PPV_ARGS(&adapter3))) || adapter3 == nullptr) {
        return false;
    }

    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    const HRESULT result = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory);
    adapter3->Release();
    if (FAILED(result)) {
        return false;
    }

    info.budgetBytes = memory.Budget;
    info.usageBytes = memory.CurrentUsage;
    info.availableForReservation = memory.AvailableForReservation;
    info.currentReservation = memory.CurrentReservation;
    return true;
}

} // namespace yawa
