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
