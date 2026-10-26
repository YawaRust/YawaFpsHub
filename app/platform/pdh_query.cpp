#include "platform/pdh_query.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

namespace yawa {

PdhCounter::~PdhCounter()
{
    close();
}

bool PdhCounter::open(const wchar_t* path)
{
    close();

    if (PdhOpenQueryW(nullptr, 0, reinterpret_cast<PDH_HQUERY*>(&m_query)) != ERROR_SUCCESS) {
        m_query = nullptr;
        return false;
    }

    if (PdhAddEnglishCounterW(static_cast<PDH_HQUERY>(m_query), path, 0,
            reinterpret_cast<PDH_HCOUNTER*>(&m_counter))
        != ERROR_SUCCESS) {
        close();
        return false;
    }

    // Первый сбор нужен, чтобы счётчик с дельтой имел базу для расчёта.
    PdhCollectQueryData(static_cast<PDH_HQUERY>(m_query));
    return true;
}

void PdhCounter::close()
{
    if (m_query != nullptr) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(m_query));
    }
    m_query = nullptr;
    m_counter = nullptr;
    m_value = 0.0;
}

bool PdhCounter::collect()
{
    if (m_query == nullptr || m_counter == nullptr) {
        return false;
    }

    if (PdhCollectQueryData(static_cast<PDH_HQUERY>(m_query)) != ERROR_SUCCESS) {
        return false;
    }

    PDH_FMT_COUNTERVALUE formatted{};
    const PDH_STATUS status = PdhGetFormattedCounterValue(static_cast<PDH_HCOUNTER>(m_counter),
        PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &formatted);
    if (status != ERROR_SUCCESS || formatted.CStatus != PDH_CSTATUS_VALID_DATA) {
        return false;
    }

    m_value = formatted.doubleValue;
    return true;
}

PdhWildcardCounter::~PdhWildcardCounter()
{
    close();
}

bool PdhWildcardCounter::open(const wchar_t* path)
{
    close();

    if (PdhOpenQueryW(nullptr, 0, reinterpret_cast<PDH_HQUERY*>(&m_query)) != ERROR_SUCCESS) {
        m_query = nullptr;
        return false;
    }

    if (PdhAddEnglishCounterW(static_cast<PDH_HQUERY>(m_query), path, 0,
            reinterpret_cast<PDH_HCOUNTER*>(&m_counter))
        != ERROR_SUCCESS) {
        close();
        return false;
    }

    PdhCollectQueryData(static_cast<PDH_HQUERY>(m_query));
    return true;
}

void PdhWildcardCounter::close()
{
    m_values.clear();
    if (m_query != nullptr) {
        PdhCloseQuery(static_cast<PDH_HQUERY>(m_query));
    }
    m_query = nullptr;
    m_counter = nullptr;
}

bool PdhWildcardCounter::collect()
{
    if (m_query == nullptr || m_counter == nullptr) {
        return false;
    }

    if (PdhCollectQueryData(static_cast<PDH_HQUERY>(m_query)) != ERROR_SUCCESS) {
        return false;
    }

    DWORD bufferSize = 0;
    DWORD itemCount = 0;
    PDH_STATUS status = PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(m_counter),
        PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufferSize, &itemCount, nullptr);
    if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA) || bufferSize == 0) {
        m_values.clear();
        return status == ERROR_SUCCESS;
    }

    std::vector<unsigned char> buffer(bufferSize);
    status = PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(m_counter),
        PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bufferSize, &itemCount,
        reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()));
    if (status != ERROR_SUCCESS) {
        m_values.clear();
        return false;
    }

    const auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    m_values.clear();
    m_values.reserve(itemCount);

    for (DWORD index = 0; index < itemCount; ++index) {
        if (items[index].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) {
            continue;
        }
        m_values.emplace_back(items[index].szName != nullptr ? items[index].szName : L"",
            items[index].FmtValue.doubleValue);
    }
    return true;
}

} // namespace yawa
