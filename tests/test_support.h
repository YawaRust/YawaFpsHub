#pragma once

#include <cstdio>
#include <string>

namespace yawa::test {

inline int& failureCount()
{
    static int count = 0;
    return count;
}

inline void report(bool passed, const char* expression, const char* file, int line)
{
    if (passed) {
        return;
    }
    ++failureCount();
    std::printf("FAIL %s:%d  %s\n", file, line, expression);
}

inline void reportNear(bool passed, const char* expression, double actual, double expected, const char* file, int line)
{
    if (passed) {
        return;
    }
    ++failureCount();
    std::printf("FAIL %s:%d  %s (получено %.6f, ожидалось %.6f)\n", file, line, expression, actual, expected);
}

} // namespace yawa::test

#define CHECK(expression) ::yawa::test::report(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define CHECK_NEAR(actual, expected, tolerance)                                                          \
    ::yawa::test::reportNear((actual) >= (expected) - (tolerance) && (actual) <= (expected) + (tolerance), \
        #actual " ≈ " #expected, (actual), (expected), __FILE__, __LINE__)
