#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace yawa {

struct MetricStats {
    double current = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    double average = 0.0;
    double smoothed = 0.0;
    std::uint64_t samples = 0;
    bool initialized = false;

    void add(double value);
};

// Экспоненциальное сглаживание: smoothing 0 — сырые значения, 100 — график почти заморожен.
double smoothStep(double previous, double value, std::uint8_t smoothingPercent);

// Перцентиль по методу ближайшего ранга: индекс ceil(p * n) - 1 в отсортированном массиве.
// Для 1% и 0.1% low это даёт ровно тот кадр, который считается «худшим», без интерполяции.
double percentile(std::span<const double> values, double fraction);

struct FrameTimeWindow {
    static constexpr std::size_t kCapacity = 1800;

    void push(double frameTimeMs);

    std::size_t size() const { return m_count; }
    std::size_t capacity() const { return kCapacity; }
    double lowFraction(double fraction) const;

    double averageFps() const;
    double currentFps() const;
    static double fpsFromMs(double frameTimeMs);
    double droppedShare(double targetMs) const;

private:
    std::array<double, kCapacity> m_samples{};
    std::size_t m_head = 0;
    std::size_t m_count = 0;
    // Буфер для сортировки: выделен заранее, чтобы перцентили не аллоцировали память на каждом опросе.
    mutable std::array<double, kCapacity> m_scratch{};
};

struct FpsSummary {
    double currentFps = 0.0;
    double frameTimeMs = 0.0;
    double low1Fps = 0.0;
    double low01Fps = 0.0;
    double averageFps = 0.0;
    double droppedShare = 0.0;

    static FpsSummary fromWindow(const FrameTimeWindow& window, double targetFps);
};

} // namespace yawa
