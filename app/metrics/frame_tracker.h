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
    // Флаги вызова Present: DXGI_PRESENT_TEST — это проверка, а не кадр.
    std::uint32_t flags = 0;
};

// Кадры приходят из разных процессов и цепочек подмены сразу: рабочий стол, оверлеи
// и сама игра. Как и RTSS, игрой считаем поток кадров процесса в активном окне, а не
// самый быстрый: композитор рабочего стола (dwm.exe) на мониторе 144 Гц подаёт кадры
// чаще игры со 120 кадрами, и «самый быстрый поток» давал завышенный FPS. Если активное
// окно к кадрам не относится, берём самый быстрый поток, но служебные процессы
// (dwm, explorer, хосты оболочки) пропускаем всегда. Поток, который перестал отдавать
// кадры, через секунду перестаёт считаться активным, иначе оверлей залипал бы на
// закрытой игре.
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

    // Процесс активного окна: его поток кадров считается игрой.
    void setForegroundProcess(std::uint32_t processId);
    // Служебные процессы: их кадры никогда не становятся «игрой».
    void setSystemProcesses(const std::uint32_t* processes, std::size_t count);
    // Процессы уже известных потоков: приложение по ним обновляет список служебных.
    std::size_t streamProcesses(std::uint32_t* out, std::size_t capacity) const;
    bool isSystemProcess(std::uint32_t processId) const;

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
    std::uint32_t m_foregroundPid = 0;
    std::array<std::uint32_t, kMaxStreams> m_systemPids{};
    std::size_t m_systemCount = 0;
    std::uint64_t m_newestQpc = 0;
    std::array<Stream, kMaxStreams> m_streams{};
    std::size_t m_streamCount = 0;
    std::size_t m_lastTouched = 0;
    std::uint64_t m_droppedStreams = 0;
};

} // namespace yawa
