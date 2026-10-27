#include "platform/spd_probe_win.h"
#include "resources/pawnio_ids.h"

#include <windows.h>
#include <winioctl.h>

#include <array>
#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

namespace yawa {
namespace {

    constexpr DWORD kLoadModule = CTL_CODE(41394u, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kExecute = CTL_CODE(41394u, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr DWORD kVersion = CTL_CODE(41394u, 0x861, METHOD_BUFFERED, FILE_ANY_ACCESS);
    constexpr ULONGLONG kProbeBudgetMs = 5000;

    struct Device
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        ~Device()
        {
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
        }
    };

    struct BusLock
    {
        HANDLE handle = nullptr;
        bool owned = false;
        ~BusLock()
        {
            if (owned)
                ReleaseMutex(handle);
            if (handle != nullptr)
                CloseHandle(handle);
        }
    };

    bool isCezanne()
    {
        int vendor[4] { };
#ifdef _MSC_VER
        __cpuidex(vendor, 0, 0);
#else
        __cpuid_count(0, 0, vendor[0], vendor[1], vendor[2], vendor[3]);
#endif
        char name[13] { };
        std::memcpy(name, &vendor[1], 4);
        std::memcpy(name + 4, &vendor[3], 4);
        std::memcpy(name + 8, &vendor[2], 4);
        if (std::strcmp(name, "AuthenticAMD") != 0)
            return false;
        int cpu[4] { };
#ifdef _MSC_VER
        __cpuidex(cpu, 1, 0);
#else
        __cpuid_count(1, 0, cpu[0], cpu[1], cpu[2], cpu[3]);
#endif
        const unsigned signature = static_cast<unsigned>(cpu[0]);
        const unsigned family = ((signature >> 8) & 0xfu) + ((signature >> 20) & 0xffu);
        const unsigned model = ((signature >> 4) & 0xfu) | ((signature >> 12) & 0xf0u);
        return family == 0x19 && model == 0x50;
    }

    bool loadSmbusModule(HANDLE device)
    {
        const HMODULE instance = GetModuleHandleW(nullptr);
        const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(IDR_PAWNIO_SMBUS), RT_RCDATA);
        if (resource == nullptr)
            return false;
        const DWORD size = SizeofResource(instance, resource);
        const HGLOBAL data = LoadResource(instance, resource);
        const void* bytes = data != nullptr ? LockResource(data) : nullptr;
        if (bytes == nullptr || size != 42404)
            return false;
        DWORD written = 0;
        return DeviceIoControl(
                   device, kLoadModule, const_cast<void*>(bytes), size, nullptr, 0, &written, nullptr)
            != FALSE;
    }

    struct ExecuteRequest
    {
        char name[32] { };
        std::uint64_t input[5] { };
    };

    bool execute(HANDLE device, const char* name, const std::uint64_t* input, unsigned count,
        std::uint64_t* output, DWORD outputSize)
    {
        ExecuteRequest request { };
        std::memcpy(request.name, name, std::strlen(name));
        if (count != 0)
            std::memcpy(request.input, input, count * sizeof(std::uint64_t));
        DWORD written = 0;
        const DWORD inputSize = static_cast<DWORD>(sizeof(request.name) + count * sizeof(std::uint64_t));
        return DeviceIoControl(device, kExecute, &request, inputSize, output, outputSize, &written, nullptr)
            && written == outputSize;
    }

    bool transfer(HANDLE device, unsigned address, unsigned direction, unsigned command, unsigned protocol,
        std::uint8_t* byte = nullptr)
    {
        if (!permittedDdr4SpdTransfer(address, direction, command, protocol))
            return false;
        const std::uint64_t input[5] { address, direction, command, protocol, 0 };
        std::uint64_t output = 0;
        const DWORD outputSize = direction == 1 ? static_cast<DWORD>(sizeof(output)) : 0;
        if (!execute(device, "ioctl_smbus_xfer", input, direction == 1 ? 4u : 5u,
                direction == 1 ? &output : nullptr, outputSize)
            || (byte != nullptr && output > 0xffu)) {
            return false;
        }
        if (byte != nullptr)
            *byte = static_cast<std::uint8_t>(output);
        return true;
    }

    bool readRange(HANDLE device, unsigned address, unsigned first, unsigned last, ULONGLONG deadline,
        std::array<std::uint8_t, 512>& bytes)
    {
        for (unsigned offset = first; offset < last; ++offset) {
            if (GetTickCount64() >= deadline
                || !transfer(device, address, 1, offset % 256, 2, &bytes[offset]))
                return false;
        }
        return true;
    }

} // namespace

bool readCezanneDdr4Spd(unsigned address, Ddr4SpdProbeResult& result, std::string& error)
{
    if (address < 0x50 || address > 0x57 || !isCezanne()) {
        error = "SPD probe requires an explicit address 0x50-0x57 and an AMD 19h/50h CPU.";
        return false;
    }
    Device device { CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr) };
    if (device.handle == INVALID_HANDLE_VALUE) {
        error = "PawnIO device unavailable; enable sensors in the application first.";
        return false;
    }
    DWORD version = 0;
    DWORD written = 0;
    if (!DeviceIoControl(device.handle, kVersion, nullptr, 0, &version, sizeof(version), &written, nullptr)
        || written != sizeof(version) || version < 0x00020200 || (version >> 16) != 2) {
        error = "PawnIO 2.2 or newer (ABI 2) is required.";
        return false;
    }
    BusLock bus { CreateMutexW(nullptr, FALSE, L"Global\\Access_SMBUS.HTP.Method") };
    if (bus.handle == nullptr) {
        error = "Cannot open the SMBus coordination mutex.";
        return false;
    }
    const DWORD wait = WaitForSingleObject(bus.handle, 20);
    bus.owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (wait != WAIT_OBJECT_0) {
        error = "SMBus is busy or its previous owner exited unexpectedly.";
        return false;
    }
    if (!loadSmbusModule(device.handle)) {
        error = "Confirmed AMD 00:14.0 SMBus controller or bundled module unavailable.";
        return false;
    }
    std::uint64_t identity[3] { };
    if (!execute(device.handle, "ioctl_identity", nullptr, 0, identity, sizeof(identity))
        || identity[1] != 0xb00 || (identity[2] & 0xffffffffu) != 0x790b1022u) {
        error = "AMD SMBus controller identity does not match the guarded 00:14.0 port.";
        return false;
    }
    // If page 1 is already selected (or the selector is absent), do not alter it.
    if (!transfer(device.handle, 0x36, 1, 0, 1)) {
        error = "Cannot confirm DDR4 SPD page zero without changing the bus page.";
        return false;
    }

    std::array<std::uint8_t, 512> bytes { };
    const ULONGLONG deadline = GetTickCount64() + kProbeBudgetMs;
    if (!readRange(device.handle, address, 0, 128, deadline, bytes)) {
        error = "SPD base page read failed or exceeded the five-second budget.";
        return false;
    }
    const auto passport = decodeSpdBase(bytes);
    if (!passport || passport->type != SpdType::Ddr4 || (bytes[0] & 0x70u) != 0x20u) {
        error = "SPD base CRC, DDR4 type or 512-byte EE1004 capacity is invalid.";
        return false;
    }
    if (!readRange(device.handle, address, 128, 256, deadline, bytes)
        || spdCrc16(std::span(bytes).subspan(128, 126))
            != static_cast<unsigned>(bytes[254] | (bytes[255] << 8))) {
        error = "SPD module CRC or remaining base page read is invalid.";
        return false;
    }

    const bool selected = transfer(device.handle, 0x37, 0, 0, 1);
    const bool read = selected && readRange(device.handle, address, 256, 512, deadline, bytes);
    // Even a NACK can change the EE1004 page. Always attempt to restore it.
    transfer(device.handle, 0x36, 0, 0, 1);
    const bool restored = transfer(device.handle, 0x36, 1, 0, 1);
    if (!restored) {
        error = "SPD page zero could not be restored; stop probing this controller.";
        return false;
    }
    if (!read) {
        error = "SPD second page read failed or exceeded the five-second budget.";
        return false;
    }
    const auto profiles = decodeDdr4Xmp(bytes);
    if (!profiles) {
        error = "SPD second page or XMP metadata is incomplete or invalid.";
        return false;
    }
    result = { *passport, *profiles };
    return true;
}

} // namespace yawa
