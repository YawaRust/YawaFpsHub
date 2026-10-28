#include "metrics/spd.h"
#include "test_support.h"

#include <algorithm>
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
    CHECK(permittedDdr4SpdTransfer(0x50, 1, 0, 2));
    CHECK(permittedDdr4SpdTransfer(0x57, 1, 255, 2));
    CHECK(permittedDdr4SpdTransfer(0x36, 1, 0, 1)); // Page-zero query.
    CHECK(permittedDdr4SpdTransfer(0x36, 0, 0, 1)); // Restore page zero.
    CHECK(permittedDdr4SpdTransfer(0x37, 0, 0, 1)); // Select page one.
    CHECK(!permittedDdr4SpdTransfer(0x50, 0, 1, 2)); // Never write SPD content.
    CHECK(!permittedDdr4SpdTransfer(0x50, 1, 1, 3));
    CHECK(!permittedDdr4SpdTransfer(0x58, 1, 1, 2));
    CHECK(!permittedDdr4SpdTransfer(0x36, 0, 1, 1));
    CHECK(!permittedDdr4SpdTransfer(0x37, 1, 0, 1));
    CHECK(!permittedDdr4SpdTransfer(0x36, 0, 0, 0));
    CHECK(!permittedDdr4SpdTransfer(0x50, 1, 256, 2));
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
        CHECK(ddr4Identity->moduleCapacityMiB == 4096u);
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

    std::array<std::uint8_t, 512> full { };
    std::copy(ddr4.begin(), ddr4.end(), full.begin());
    // Synthetic second CRC-covered block (all zeros); XMP bytes themselves are not CRC-covered.
    const auto noXmp = decodeDdr4Xmp(full);
    CHECK(noXmp.has_value());
    if (noXmp)
        CHECK(noXmp->empty());
    constexpr std::string_view part = "ABC-1234";
    auto noXmpPart = full;
    std::fill(noXmpPart.begin() + 329, noXmpPart.begin() + 349, std::uint8_t { ' ' });
    std::transform(part.begin(), part.end(), noXmpPart.begin() + 329,
        [](char value) { return static_cast<std::uint8_t>(value); });
    CHECK(decodeDdr4PartNumber(noXmpPart) == part);
    std::fill(full.begin() + 256, full.end(), std::uint8_t { 0xff });
    CHECK(!decodeDdr4Xmp(full)); // Unreadable second page must not mean 'no XMP'.
    std::fill(full.begin() + 256, full.end(), std::uint8_t { 0 });
    full[384] = 0x0c;
    full[385] = 0x4a;
    full[386] = 0x03; // Two advertised profiles.
    full[387] = 0x20; // XMP 2.0.
    full[393 + 3] = 10; // 1250 ps, passport ~1600 MT/s, not active frequency.
    full[440 + 3] = 8; // 1000 ps, passport ~2000 MT/s.
    const auto xmp = decodeDdr4Xmp(full);
    CHECK(xmp.has_value());
    if (xmp) {
        CHECK(xmp->size() == 2);
        if (xmp->size() == 2) {
            CHECK((*xmp)[0].number == 1 && (*xmp)[0].minimumCyclePs == 1250);
            CHECK((*xmp)[1].number == 2 && (*xmp)[1].minimumCyclePs == 1000);
        }
    }
    full[128] ^= 1; // Module CRC is mandatory, even though XMP lies outside it.
    CHECK(!decodeDdr4Xmp(full));
    full[128] ^= 1;
    full[393 + 3] = 0; // A signaled but invalid profile is unknown, not 'no XMP'.
    CHECK(!decodeDdr4Xmp(full));
    full[393 + 3] = 10;
    full[387] = 0x30; // XMP 3.0/unknown is not decoded as 2.0.
    CHECK(!decodeDdr4Xmp(full));
    full[387] = 0x20;
    CHECK(!decodeDdr4Xmp(std::span<const std::uint8_t>(full.data(), 256)));

    // The manufacturing label is outside both CRC-covered regions; malformed
    // or truncated page-one bytes must never be printed as a part number.
    CHECK(!decodeDdr4PartNumber(full));
    std::fill(full.begin() + 329, full.begin() + 349, std::uint8_t { ' ' });
    CHECK(!decodeDdr4PartNumber(full));
    std::transform(part.begin(), part.end(), full.begin() + 329,
        [](char value) { return static_cast<std::uint8_t>(value); });
    CHECK(decodeDdr4PartNumber(full) == part);
    CHECK(!decodeDdr4PartNumber(std::span<const std::uint8_t>(full.data(), 256)));
    full[329] = 0xff;
    CHECK(!decodeDdr4PartNumber(full));
    full[329] = static_cast<std::uint8_t>('A');
    full[331] = 0;
    CHECK(!decodeDdr4PartNumber(full));
    full[331] = static_cast<std::uint8_t>('C');
    full[128] ^= 1;
    CHECK(!decodeDdr4PartNumber(full));
    full[128] ^= 1;
    full[4] ^= 1;
    CHECK(!decodeDdr4PartNumber(full));
}

} // namespace yawa::test
