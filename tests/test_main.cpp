#include "test_support.h"

#include <cstdio>

namespace yawa::test {

void runStatisticsTests();
void runUnitsTests();
void runMetricStoreTests();
void runSystemSamplerTests();
void runGpuRegistryTests();
void runFrameTrackerTests();

} // namespace yawa::test

int main()
{
    yawa::test::runStatisticsTests();
    yawa::test::runUnitsTests();
    yawa::test::runMetricStoreTests();
    yawa::test::runSystemSamplerTests();
    yawa::test::runGpuRegistryTests();
    yawa::test::runFrameTrackerTests();

    if (yawa::test::failureCount() == 0) {
        std::printf("все тесты пройдены\n");
        return 0;
    }

    std::printf("провалено проверок: %d\n", yawa::test::failureCount());
    return 1;
}
