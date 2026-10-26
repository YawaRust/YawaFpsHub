#include "metrics/gpu_registry.h"
#include "test_support.h"

#include <string>

namespace yawa::test {

namespace {

GpuAdapter makeAdapter(std::uint32_t luid, const wchar_t* name, std::uint32_t vendorId, std::uint64_t memory)
{
    GpuAdapter adapter;
    adapter.luidLow = luid;
    adapter.luidHigh = 0;
    adapter.name = name;
    adapter.vendorId = vendorId;
    adapter.vendor = vendorFromId(vendorId);
    adapter.dedicatedVideoMemory = memory;
    return adapter;
}

void testVendorMapping()
{
    CHECK(vendorFromId(0x10DE) == GpuVendor::Nvidia);
    CHECK(vendorFromId(0x1002) == GpuVendor::Amd);
    CHECK(vendorFromId(0x8086) == GpuVendor::Intel);
    CHECK(vendorFromId(0x1234) == GpuVendor::Unknown);
}

void testPreferredIsDiscrete()
{
    GpuRegistry registry;
    registry.add(makeAdapter(1, L"AMD Radeon 780M", 0x1002, 512ull * 1024 * 1024));
    registry.add(makeAdapter(2, L"NVIDIA GeForce RTX 4080", 0x10DE, 16ull * 1024 * 1024 * 1024));

    CHECK(registry.count() == 2);
    CHECK(registry.preferredIndex() == 1);
    CHECK(registry.byIndex(1) != nullptr && registry.byIndex(1)->vendor == GpuVendor::Nvidia);
}

void testSoftwareAdapterIsSkipped()
{
    GpuRegistry registry;
    registry.add(makeAdapter(1, L"AMD Radeon 780M", 0x1002, 1024ull * 1024 * 1024));

    GpuAdapter software = makeAdapter(2, L"Microsoft Basic Render Driver", 0x1414, 8ull * 1024 * 1024 * 1024);
    software.isSoftware = true;
    registry.add(software);

    CHECK(registry.preferredIndex() == 0);
}

void testResolveByLuid()
{
    GpuRegistry registry;
    registry.add(makeAdapter(11, L"AMD Radeon 780M", 0x1002, 512ull * 1024 * 1024));
    registry.add(makeAdapter(22, L"NVIDIA GeForce RTX 4080", 0x10DE, 16ull * 1024 * 1024 * 1024));

    CHECK(registry.resolve(11, 0) == 0);
    CHECK(registry.resolve(22, 0) == 1);
    CHECK(registry.resolve(99, 0) == 1);
}

void testEmptyRegistry()
{
    GpuRegistry registry;
    CHECK(registry.empty());
    CHECK(registry.byIndex(0) == nullptr);
    CHECK(registry.resolve(1, 0) == 0);
    CHECK(registry.preferredIndex() == 0);
}

void testUtf8Conversion()
{
    CHECK(toUtf8(L"GPU") == "GPU");
    CHECK(toUtf8(L"Видеокарта") == "\xD0\x92\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBE\xD0\xBA\xD0\xB0\xD1\x80\xD1\x82\xD0\xB0");
    CHECK(toUtf8(L"").empty());

    const std::string emoji = toUtf8(L"\xD83D\xDE00");
    CHECK(emoji == "\xF0\x9F\x98\x80");
}

} // namespace

void runGpuRegistryTests()
{
    CHECK(uniqueGpuNameMatch(L"NVIDIA RTX 4080", { L"AMD Radeon", L"nvidia rtx 4080" }) == 1);
    CHECK(!uniqueGpuNameMatch(L"AMD Radeon", { L"NVIDIA RTX 4080" }));
    CHECK(!uniqueGpuNameMatch(L"", { L"NVIDIA RTX 4080" }));
    CHECK(!uniqueGpuNameMatch(L"NVIDIA RTX 4080", { L"NVIDIA RTX 4080", L"NVIDIA RTX 4080" }));
    CHECK(!uniqueGpuNameMatch(L"GTX 1060", { L"GTX 1060", L"GTX 1060 Ti" }));
    testVendorMapping();
    testPreferredIsDiscrete();
    testSoftwareAdapterIsSkipped();
    testResolveByLuid();
    testEmptyRegistry();
    testUtf8Conversion();
}

} // namespace yawa::test
