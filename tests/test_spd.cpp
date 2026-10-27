#include "metrics/spd.h"
#include "test_support.h"

#include <array>
#include <string_view>

namespace yawa::test {

std::array<std::uint8_t, 128> fromHex(std::string_view hex)
{
    std::array<std::uint8_t, 128> bytes { };
    const auto nibble = [](char digit) { return digit <= '9' ? digit - '0' : digit - 'a' + 10; };
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(nibble(hex[i * 2]) * 16 + nibble(hex[i * 2 + 1]));
    }
    return bytes;
}

void runSpdTests()
{
    // coreboot/coreboot 1879b6a34a6e93a93d691a0d9f2457d6251a17c1,
    // src/mainboard/google/hatch/spd/8G_2666.spd.hex, blob 7042c766f84a1a611dfbb5763dc7b7400f501a0e, bytes
    // 0..127.
    const auto ddr4 = fromHex("23110c034521000800600003020300000000060df83f00006e6e6e11006ef00a"
                              "2008000500f02b3428007800143c000000000000000000000000000016360b35"
                              "16360b35000016360b3516360b35000000000000000000000000000000000000"
                              "000000000000000000000000000000000000000000009cb500000000e700872e");

    // coreboot/coreboot 1879b6a34a6e93a93d691a0d9f2457d6251a17c1,
    // src/mainboard/intel/adlrvp/spd/adlrvp_ddr5_mr.spd.hex, blob 80ef521cfae4b57f36c09b71ffb2797d239c187a,
    // bytes 0..127.
    const auto ddr5 = fromHex("3008120304002062000000006000000000000000a101e8037215000000001e41"
                              "1e411e41007d1ebe30752701a000820000000000000000000000000000000000"
                              "0000000000000000000000000000000000000000000000000000000000000000"
                              "00000000000000000000000000000000000000000000000000000000000047ae");

    const auto ddr4Identity = decodeSpdBase(ddr4);
    CHECK(ddr4Identity.has_value());
    if (ddr4Identity) {
        CHECK(ddr4Identity->type == SpdType::Ddr4);
        CHECK(ddr4Identity->minimumCyclePs == 750);
        CHECK(ddr4Identity->moduleCapacityMiB == 4096);
        CHECK(ddr4Identity->jedecTimings.has_value());
        if (ddr4Identity->jedecTimings) {
            CHECK(ddr4Identity->jedecTimings->tAaPs == 13750);
            CHECK(ddr4Identity->jedecTimings->tRcdPs == 13750);
            CHECK(ddr4Identity->jedecTimings->tRpPs == 13750);
            CHECK(ddr4Identity->jedecTimings->tRasPs == 32000);
        }
    }
    const auto ddr5Identity = decodeSpdBase(ddr5);
    CHECK(ddr5Identity.has_value());
    if (ddr5Identity) {
        CHECK(ddr5Identity->type == SpdType::Ddr5);
        CHECK(ddr5Identity->minimumCyclePs == 417);
        CHECK(!ddr5Identity->moduleCapacityMiB);
        CHECK(!ddr5Identity->jedecTimings);
    }
    CHECK(!decodeSpdBase(std::span<const std::uint8_t>(ddr4.data(), 127)));
    auto broken = ddr4;
    broken[4] ^= 1;
    CHECK(!decodeSpdBase(broken));
    broken = ddr5;
    broken[20] ^= 1;
    CHECK(!decodeSpdBase(broken));
    broken = ddr4;
    broken[2] = 0x0b; // DDR3 is not supported by this decoder.
    CHECK(!decodeSpdBase(broken));
}

} // namespace yawa::test
