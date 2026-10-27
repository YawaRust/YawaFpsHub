#include "metrics/memory_timings.h"
#include "test_support.h"

namespace yawa::test {
void runMemoryTimingsTests()
{
    // Искусственные регистры DDR4 UMC 0x50204/0x50208: 30-18-18-18-42-60.
    const std::uint32_t timing0 = (18u << 24) | (18u << 16) | (42u << 8) | 30u;
    const std::uint32_t timing1 = (18u << 16) | 60u;
    const auto timings = decodeAmdDdr4Timings(timing0, timing1);
    CHECK(timings.has_value());
    if (timings) {
        CHECK(timings->cl == 30);
        CHECK(timings->rcdRead == 18);
        CHECK(timings->rcdWrite == 18);
        CHECK(timings->rp == 18);
        CHECK(timings->ras == 42);
        CHECK(timings->rc == 60);
    }
    CHECK(isMatisseDdr4Channel(0x17, 0x71, 0, 0, 1, 0));
    CHECK(!isMatisseDdr4Channel(0x17, 0x70, 0, 0, 1, 0));
    CHECK(!isMatisseDdr4Channel(0x19, 0x71, 0, 0, 1, 0));
    CHECK(!isMatisseDdr4Channel(0x17, 0x71, 1, 0, 1, 0)); // DDR5
    CHECK(!isMatisseDdr4Channel(0x17, 0x71, 0xffffffffu, 0, 1, 0));
    CHECK(!isMatisseDdr4Channel(0x17, 0x71, 0, 1u << 19, 1, 0));
    CHECK(!isMatisseDdr4Channel(0x17, 0x71, 0, 0, 0, 0));
    CHECK(!isMatisseDdr4Channel(0x17, 0x71, 0, 0, 0xffffffffu, 0));
    CHECK(!decodeAmdDdr4Timings(0, timing1));
    CHECK(!decodeAmdDdr4Timings(timing0, 0));
    CHECK(!decodeAmdDdr4Timings(0xffffffffu, timing1));
    CHECK(!decodeAmdDdr4Timings(timing0, 0xffffffffu));
    CHECK(!decodeAmdDdr4Timings(timing0 & ~0x3fu, timing1));
    CHECK(!decodeAmdDdr4Timings(timing0, timing1 & ~0xffu));
    CHECK(!decodeAmdDdr4Timings(timing0, (18u << 16) | 40u)); // tRC < tRAS
    const auto reservedBits = decodeAmdDdr4Timings(timing0 | (3u << 6) | (1u << 31), timing1 | (0x7fu << 24));
    CHECK(reservedBits.has_value());
    if (reservedBits) {
        CHECK(reservedBits->cl == 30);
        CHECK(reservedBits->rc == 60);
    }

    AmdDdr4MemoryDump dump {};
    dump[0] = { 0, 0, 1, 0, timing0, timing1, true };
    dump[1] = { 0, 0, 0, 0, 0, 0, false };
    CHECK(decodeAmdDdr4MemoryDump(dump) == timings);
    const auto text = formatAmdDdr4MemoryDump(dump);
    CHECK(text.find("0x00050204=0x12122A1E") != std::string::npos);
    CHECK(text.find("0x00050208=0x0012003C") != std::string::npos);
    CHECK(text.find("0x00150100=0x00000000") != std::string::npos);
    CHECK(text.find("not_read") != std::string::npos);
    CHECK(text.find("0x00150204=0x") == std::string::npos);
    dump[1] = { 0, 0, 1, 0, timing0, timing1, true };
    CHECK(decodeAmdDdr4MemoryDump(dump) == timings);
    dump[1].timing1 = (18u << 16) | 64u;
    CHECK(!decodeAmdDdr4MemoryDump(dump));
    dump[1].config = 1;
    CHECK(!decodeAmdDdr4MemoryDump(dump));
    dump[0].hasTimings = false;
    CHECK(!decodeAmdDdr4MemoryDump(dump));
}
} // namespace yawa::test
