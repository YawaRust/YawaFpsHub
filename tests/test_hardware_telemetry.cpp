#include "metrics/hardware_telemetry.h"
#include "test_support.h"

#include <array>
#include <limits>

namespace yawa::test {
void runHardwareTelemetryTests()
{
    std::array<std::byte, 512> log { };
    CHECK(!nvmeCompositeTemperature(log));
    CHECK(!nvmeCompositeTemperature(std::span(log).first(2)));
    log[1] = std::byte(0x2c);
    log[2] = std::byte(0x01);
    CHECK_NEAR(nvmeCompositeTemperature(log).value_or(-999), 26.85, 1e-9);
    log[1] = log[2] = std::byte(0xff);
    CHECK(!nvmeCompositeTemperature(log));
    log[1] = std::byte(1);
    log[2] = std::byte(0);
    CHECK(!nvmeCompositeTemperature(log));

    CHECK(!decodeNvmeSmartHealth(std::span(log).first(511)));
    log[0] = std::byte(0x05);
    log[3] = std::byte(98);
    log[4] = std::byte(10);
    log[5] = std::byte(255);
    log[32] = std::byte(11); // Data units read, not written.
    log[48] = std::byte(42); // Each data unit is 512,000 bytes, rounded up by the drive.
    log[128] = std::byte(7);
    log[144] = std::byte(3);
    log[160] = std::byte(2);
    log[176] = std::byte(9);
    auto health = decodeNvmeSmartHealth(log);
    CHECK(health.has_value());
    CHECK(health->criticalWarning == 0x05);
    CHECK(health->availableSparePercent == 98);
    CHECK(health->spareThresholdPercent == 10);
    CHECK(health->percentUsed == 255);
    CHECK(health->dataUnitsRead == 11);
    CHECK(health->dataUnitsWritten == 42);
    CHECK(health->powerOnHours == 7);
    CHECK(health->unsafeShutdowns == 3);
    CHECK(health->mediaErrors == 2);
    CHECK(health->errorLogEntries == 9);
    CHECK(!health->compositeCelsius);
    log[56] = std::byte(1); // High half of the written 128-bit counter is nonzero.
    health = decodeNvmeSmartHealth(log);
    CHECK(!health->dataUnitsWritten);
    CHECK(health->dataUnitsRead == 11);
    log[3] = std::byte(101);
    log[4] = std::byte(255);
    health = decodeNvmeSmartHealth(log);
    CHECK(!health->availableSparePercent && !health->spareThresholdPercent);
    CHECK(health->mediaErrors == 2);

    ContinuousEnergyCounter energy;
    CHECK(!energy.sample(10, 1));
    const auto power = energy.sample(60, 1.5);
    CHECK_NEAR(power.value_or(-999), 100.0, 1e-9);
    CHECK(!energy.sample(65, 1.5));
    CHECK(!energy.sample(1, 2));
    CHECK(!energy.sample(10, 20));
    CHECK(!energy.sample(std::numeric_limits<double>::infinity(), 21));
    CHECK(!energy.sample(20, 22));
    CHECK(!energy.sample(999999, 23));
}
} // namespace yawa::test
