#pragma once

#include "metrics/statistics.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace yawa {

// Один вызов Present из ETW: кто, через какую цепочку и когда по QPC.
struct PresentSample {
    std::uint64_t qpc = 0;
    std::uint32_t processId = 0;
    std::uint32_t threadId = 0;
    std::uint64_t swapChain = 0;
};

// Кадры приходят из разных процессов и цепочек подмены сразу: рабочий стол, оверлеи
// и сама игра. Игрой считается поток кадров с наибольшей частотой за последнюю секунду —
// остальные (медиаплеер, браузер с видео) отсекаются. Поток, который перестал отдавать
// кадры, через пару секунд перестаёт считаться активным, иначе оверлей залипал бы
// на закрытой игре.
class FrameTracker {
public:
    static constexpr std::size_t kMaxStreams = 8;

    explicit FrameTracker(std::uint64_t qpcFrequency);

    bool onPresent(const PresentSample& sample);
    void reset();

    std::uint32_t activeProcessId() const;
    std::uint64_t activeSwapChain() const;
    bool hasActiveStream() const;

    FpsSummary summary(double targetFps) const;
    const FrameTimeWindow* activeWindow() const;

    std::size_t streamCount() const { return m_streamCount; }
    std::uint64_t droppedStreams() const { return m_droppedStreams; }
    // Сколько кадров отрисовала активная игра: нужно для строки «Отрисовано кадров».
    std::uint64_t activePresents() const;

private:
    struct Stream {
        std::uint32_t processId = 0;
        std::uint64_t swapChain = 0;
        std::uint64_t lastQpc = 0;
        std::uint64_t bucketStartQpc = 0;
        std::uint64_t totalPresents = 0;
        std::uint64_t recentPresents = 0;
        std::uint64_t ratePerSecond = 0;
        bool hasLast = false;
        FrameTimeWindow frames;
    };

    Stream* findStream(std::uint32_t processId, std::uint64_t swapChain);
    Stream* claimStream(std::uint32_t processId, std::uint64_t swapChain);
    std::size_t activeIndex() const;
    bool isFresh(const Stream& stream, std::uint64_t newestQpc) const;

    static constexpr std::uint64_t kStaleTimeoutSeconds = 1;
    // Пауза дольше двух секунд — это смена окна или загрузка уровня, а не кадр на 500 мс.
    static constexpr double kMaxPlausibleFrameMs = 2000.0;

    std::uint64_t m_qpcFrequency = 0;
    std::uint64_t m_newestQpc = 0;
    std::array<Stream, kMaxStreams> m_streams{};
    std::size_t m_streamCount = 0;
    std::size_t m_lastTouched = 0;
    std::uint64_t m_droppedStreams = 0;
};

} // namespace yawa
