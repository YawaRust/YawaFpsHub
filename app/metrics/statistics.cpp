#include "metrics/statistics.h"

#include <algorithm>
#include <cmath>

namespace yawa {

void MetricStats::add(double value)
{
    if (!initialized) {
        minimum = value;
        maximum = value;
        smoothed = value;
        initialized = true;
    } else {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }

    current = value;
    average = (average * static_cast<double>(samples) + value) / static_cast<double>(samples + 1);
    ++samples;
}

double smoothStep(double previous, double value, std::uint8_t smoothingPercent)
{
    const double weight = std::clamp(static_cast<double>(smoothingPercent) / 100.0, 0.0, 0.95);
    return previous * weight + value * (1.0 - weight);
}

double percentile(std::span<const double> values, double fraction)
{
    if (values.empty()) {
        return 0.0;
    }

    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const std::size_t rank = static_cast<std::size_t>(std::ceil(clamped * static_cast<double>(values.size())));
    const std::size_t index = rank == 0 ? 0 : std::min(rank - 1, values.size() - 1);
    return values[index];
}

void FrameTimeWindow::push(double frameTimeMs)
{
    if (!(frameTimeMs > 0.0) || !std::isfinite(frameTimeMs)) {
        return;
    }

    m_samples[m_head] = frameTimeMs;
    m_head = (m_head + 1) % kCapacity;
    if (m_count < kCapacity) {
        ++m_count;
    }
}

double FrameTimeWindow::lowFraction(double fraction) const
{
    if (m_count == 0) {
        return 0.0;
    }

    for (std::size_t index = 0; index < m_count; ++index) {
        m_scratch[index] = m_samples[index];
    }

    const std::span<double> sorted(m_scratch.data(), m_count);
    std::sort(sorted.begin(), sorted.end());

    // 1% low — это frame time, который хуже 99% кадров, то есть правый хвост распределения.
    // Брать левый хвост нельзя: тогда «низкий» перцентиль показывал бы самые быстрые кадры.
    const double slowest = percentile(sorted, 1.0 - fraction);
    return slowest > 0.0 ? 1000.0 / slowest : 0.0;
}

double FrameTimeWindow::averageFps() const
{
    if (m_count == 0) {
        return 0.0;
    }

    double total = 0.0;
    for (std::size_t index = 0; index < m_count; ++index) {
        total += m_samples[index];
    }
    return static_cast<double>(m_count) * 1000.0 / total;
}

double FrameTimeWindow::fpsFromMs(double frameTimeMs)
{
    return frameTimeMs > 0.0 ? 1000.0 / frameTimeMs : 0.0;
}

double FrameTimeWindow::currentFps() const
{
    if (m_count == 0) {
        return 0.0;
    }
    return fpsFromMs(m_samples[(m_head + kCapacity - 1) % kCapacity]);
}

double FrameTimeWindow::recentFps(double windowMs) const
{
    if (m_count == 0 || windowMs <= 0.0) {
        return 0.0;
    }

    // Идём от последнего кадра назад, пока сумма интервалов не наберёт окно.
    double total = 0.0;
    std::size_t frames = 0;
    for (std::size_t step = 0; step < m_count; ++step) {
        const std::size_t index = (m_head + kCapacity - 1 - step) % kCapacity;
        total += m_samples[index];
        ++frames;
        if (total >= windowMs) {
            break;
        }
    }

    return total > 0.0 ? static_cast<double>(frames) * 1000.0 / total : 0.0;
}

double FrameTimeWindow::droppedShare(double targetMs) const
{
    if (m_count == 0 || targetMs <= 0.0) {
        return 0.0;
    }

    std::size_t late = 0;
    for (std::size_t index = 0; index < m_count; ++index) {
        if (m_samples[index] > targetMs) {
            ++late;
        }
    }
    return static_cast<double>(late) / static_cast<double>(m_count);
}

FpsSummary FpsSummary::fromWindow(const FrameTimeWindow& window, double targetFps)
{
    FpsSummary summary;
    if (window.size() == 0) {
        return summary;
    }

    // Как в RTSS: показываем среднее за последнюю секунду, иначе цифра скачет
    // на каждом кадре и расходится с тем, что видит игрок.
    summary.currentFps = window.recentFps(1000.0);
    summary.frameTimeMs = summary.currentFps > 0.0 ? 1000.0 / summary.currentFps : 0.0;
    summary.low1Fps = window.lowFraction(0.01);
    summary.low01Fps = window.lowFraction(0.001);
    summary.averageFps = window.averageFps();
    summary.droppedShare = window.droppedShare(targetFps > 0.0 ? 1000.0 / targetFps : 0.0);
    return summary;
}

} // namespace yawa
