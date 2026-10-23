#pragma once

#include "metrics/metric_id.h"
#include "metrics/statistics.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace yawa {

constexpr std::size_t kHistoryCapacity = 300;

struct MetricSlot {
    MetricStats stats;
    std::array<float, kHistoryCapacity> history{};
    std::size_t historyHead = 0;
    std::size_t historyCount = 0;

    void push(double value, double smoothed);
    std::size_t orderedIndex(std::size_t index) const;
};

// Хранилище последнего кадра показателей: снимок обновляется одним потоком опроса,
// читается отрисовкой. Без блокировок на уровне класса — синхронизация на вызывающей стороне.
class MetricStore {
public:
    void set(MetricId id, double value, std::uint8_t smoothingPercent = 0);

    const MetricSlot& slot(MetricId id) const;
    MetricSlot& slot(MetricId id);

    double value(MetricId id) const { return slot(id).stats.current; }
    bool hasValue(MetricId id) const { return slot(id).stats.initialized; }

    // Копирует историю в порядке от старой к новой: графику нужен непрерывный массив,
    // а кольцевой буфер отдаёт данные через orderedIndex.
    std::size_t copyHistory(MetricId id, float* destination, std::size_t capacity) const;

    void reset();

private:
    std::array<MetricSlot, kMetricCount> m_slots{};
};

} // namespace yawa
