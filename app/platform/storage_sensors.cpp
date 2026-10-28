#include "platform/storage_sensors.h"
#include "metrics/hardware_telemetry.h"
#include "platform/log.h"

#include <setupapi.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace yawa {
namespace {
    constexpr GUID kDiskInterface
        = { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
}

StorageSensors::~StorageSensors()
{
    shutdown();
}

bool StorageSensors::initialize(Log& log)
{
    shutdown();
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stop != nullptr) {
        m_thread = CreateThread(nullptr, 0, worker, this, 0, nullptr);
    }
    if (m_thread == nullptr) {
        shutdown();
        log.warning("Диски: поток опроса температуры не запущен");
        return false;
    }
    log.info("Диски: фоновый опрос Storage Temperature / NVMe health log");
    return true;
}

void StorageSensors::shutdown()
{
    if (m_stop != nullptr) {
        SetEvent(m_stop);
    }
    if (m_thread != nullptr) {
        WaitForSingleObject(m_thread, INFINITE);
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    if (m_stop != nullptr) {
        CloseHandle(m_stop);
        m_stop = nullptr;
    }
    std::lock_guard lock(m_mutex);
    m_temperature.reset();
    m_health.clear();
    m_sampledAt = 0;
}

std::optional<double> StorageSensors::temperature() const
{
    std::lock_guard lock(m_mutex);
    return GetTickCount64() - m_sampledAt <= 15000 ? m_temperature : std::nullopt;
}

std::vector<DiskSmartReading> StorageSensors::health() const
{
    std::lock_guard lock(m_mutex);
    return GetTickCount64() - m_sampledAt <= 15000 ? m_health : std::vector<DiskSmartReading> { };
}

DWORD WINAPI StorageSensors::worker(void* context)
{
    auto& sensors = *static_cast<StorageSensors*>(context);
    do {
        sensors.poll();
    } while (WaitForSingleObject(sensors.m_stop, 5000) == WAIT_TIMEOUT);
    return 0;
}

bool StorageSensors::query(HANDLE disk, DWORD controlCode, void* buffer, DWORD size, DWORD& written)
{
    OVERLAPPED request { };
    request.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (request.hEvent == nullptr) {
        return false;
    }
    written = 0;
    bool success = DeviceIoControl(disk, controlCode, buffer, size, buffer, size, nullptr, &request) != FALSE;
    if (success) {
        success = GetOverlappedResult(disk, &request, &written, FALSE) != FALSE;
    }
    if (!success && GetLastError() == ERROR_IO_PENDING) {
        const HANDLE events[] = { m_stop, request.hEvent };
        if (WaitForMultipleObjects(2, events, FALSE, 1000) == WAIT_OBJECT_0 + 1) {
            success = GetOverlappedResult(disk, &request, &written, FALSE) != FALSE;
        } else {
            CancelIoEx(disk, &request);
            // OVERLAPPED и буфер нельзя освобождать до подтверждения отмены ядром.
            GetOverlappedResult(disk, &request, &written, TRUE);
        }
    }
    CloseHandle(request.hEvent);
    return success && written <= size;
}

std::optional<double> StorageSensors::readDisk(HANDLE disk)
{
    struct TemperatureQuery
    {
        STORAGE_PROPERTY_QUERY query { };
        std::array<std::byte, 1024> padding { };
    } request;
    request.query.PropertyId = StorageDeviceTemperatureProperty;
    request.query.QueryType = PropertyStandardQuery;
    DWORD written = 0;
    if (query(disk, IOCTL_STORAGE_QUERY_PROPERTY, &request, sizeof(request), written)
        && written >= offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo)) {
        const auto* descriptor = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(&request);
        const std::size_t count = (written - offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo))
            / sizeof(STORAGE_TEMPERATURE_INFO);
        if (descriptor->Version >= sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) && descriptor->Size <= written
            && descriptor->InfoCount <= count) {
            std::optional<double> hottest;
            for (unsigned i = 0; i < descriptor->InfoCount; ++i) {
                const int value = descriptor->TemperatureInfo[i].Temperature;
                if (value >= -40 && value <= 150) {
                    hottest = hottest ? std::max(*hottest, static_cast<double>(value)) : value;
                }
            }
            if (hottest) {
                return hottest;
            }
        }
    }
    if (WaitForSingleObject(m_stop, 0) != WAIT_TIMEOUT) {
        return std::nullopt;
    }
    const auto health = readNvme(disk, StorageAdapterProtocolSpecificProperty);
    return health ? health->compositeCelsius : std::nullopt;
}

std::optional<NvmeSmartHealth> StorageSensors::readNvme(HANDLE disk, DWORD property)
{
    struct ProtocolQuery
    {
        DWORD property;
        DWORD type = PropertyStandardQuery;
        STORAGE_PROTOCOL_SPECIFIC_DATA protocol { };
        std::array<std::byte, 512> log { };
    } nvme { property };
    static_assert(
        offsetof(ProtocolQuery, protocol) == offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters));
    nvme.protocol.ProtocolType = ProtocolTypeNvme;
    nvme.protocol.DataType = NVMeDataTypeLogPage;
    nvme.protocol.ProtocolDataRequestValue = 2; // NVMe SMART / Health Information.
    nvme.protocol.ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    nvme.protocol.ProtocolDataLength = 512;
    DWORD written = 0;
    if (!query(disk, IOCTL_STORAGE_QUERY_PROPERTY, &nvme, sizeof(nvme), written)
        || written < sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) {
        return std::nullopt;
    }
    const auto* descriptor = reinterpret_cast<const STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(&nvme);
    const auto& data = descriptor->ProtocolSpecificData;
    const std::size_t base = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
    if (descriptor->Version != sizeof(*descriptor) || descriptor->Size != sizeof(*descriptor)
        || data.ProtocolType != ProtocolTypeNvme || data.DataType != NVMeDataTypeLogPage
        || data.ProtocolDataOffset < sizeof(data) || data.ProtocolDataOffset > written - base
        || data.ProtocolDataLength < 512
        || data.ProtocolDataLength > written - base - data.ProtocolDataOffset) {
        return std::nullopt;
    }
    const auto* bytes = reinterpret_cast<const std::byte*>(&nvme);
    return decodeNvmeSmartHealth({ bytes + base + data.ProtocolDataOffset, data.ProtocolDataLength });
}

void StorageSensors::poll()
{
    std::optional<double> hottest;
    std::vector<DiskSmartReading> readings;
    const HDEVINFO devices
        = SetupDiGetClassDevsW(&kDiskInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return;
    }
    for (DWORD index = 0; index < 64 && WaitForSingleObject(m_stop, 0) == WAIT_TIMEOUT; ++index) {
        SP_DEVICE_INTERFACE_DATA entry { };
        entry.cbSize = sizeof(entry);
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &kDiskInterface, index, &entry)) {
            break;
        }
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &entry, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || size > 65536) {
            continue;
        }
        std::vector<std::byte> buffer(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &entry, detail, size, nullptr, nullptr)) {
            continue;
        }
        const HANDLE disk = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (disk == INVALID_HANDLE_VALUE) {
            continue;
        }
        STORAGE_DEVICE_NUMBER identity { };
        DWORD written = 0;
        const bool identified
            = query(disk, IOCTL_STORAGE_GET_DEVICE_NUMBER, &identity, sizeof(identity), written)
            && written >= sizeof(identity) && identity.DeviceType == FILE_DEVICE_DISK
            && identity.DeviceNumber < 1024;
        const auto health = identified ? readNvme(disk, StorageDeviceProtocolSpecificProperty) : std::nullopt;
        const auto temperature = readDisk(disk);
        CloseHandle(disk);
        if (identified && health) {
            readings.push_back({ identity.DeviceNumber, *health });
        }
        if (temperature) {
            hottest = hottest ? std::max(*hottest, *temperature) : *temperature;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    std::lock_guard lock(m_mutex);
    m_temperature = hottest;
    m_health = std::move(readings);
    m_sampledAt = GetTickCount64();
}

} // namespace yawa
