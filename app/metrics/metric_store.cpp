#include "metrics/metric_store.h"

namespace yawa {

void MetricSlot::push(double value, double smoothed)
{
    history[historyHead] = static_cast<float>(smoothed);
    historyHead = (historyHead + 1) % kHistoryCapacity;
    if (historyCount < kHistoryCapacity) {
        ++historyCount;
    }
    stats.add(value);
}

std::size_t MetricSlot::orderedIndex(std::size_t index) const
{
    const std::size_t oldest = (historyHead + kHistoryCapacity - historyCount) % kHistoryCapacity;
    return (oldest + index) % kHistoryCapacity;
}

void MetricStore::set(MetricId id, double value, std::uint8_t smoothingPercent)
{
    MetricSlot& target = slot(id);
    const double smoothed = target.stats.initialized
        ? smoothStep(target.stats.smoothed, value, smoothingPercent)
        : value;

    target.push(value, smoothed);
    target.stats.smoothed = smoothed;
}

const MetricSlot& MetricStore::slot(MetricId id) const
{
    return m_slots[metricIndex(id)];
}

MetricSlot& MetricStore::slot(MetricId id)
{
    return m_slots[metricIndex(id)];
}

std::size_t MetricStore::copyHistory(MetricId id, float* destination, std::size_t capacity) const
{
    const MetricSlot& source = slot(id);
    const std::size_t count = source.historyCount < capacity ? source.historyCount : capacity;
    const std::size_t offset = source.historyCount - count;

    for (std::size_t index = 0; index < count; ++index) {
        destination[index] = source.history[source.orderedIndex(offset + index)];
    }
    return count;
}

void MetricStore::reset()
{
    for (MetricSlot& item : m_slots) {
        item = MetricSlot{};
    }
}

} // namespace yawa
